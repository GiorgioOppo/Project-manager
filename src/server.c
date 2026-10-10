#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "workspace.h"
#include "accounts.h"
#include "storage.h"

#ifdef __APPLE__
#include <Security/SecRandom.h>
#else
#include <openssl/rand.h>
#endif

#define HEADER_LIMIT 16384
#define BODY_LIMIT 32768
#define LOGIN_BODY_LIMIT 8192
#define REQUEST_LIMIT (HEADER_LIMIT + BODY_LIMIT)
#define SESSION_LIMIT 64
#define TOKEN_BYTES 32
#define TOKEN_LENGTH (TOKEN_BYTES * 2)
#define SHORT_SESSION (8 * 60 * 60)
#define LONG_SESSION (30 * 24 * 60 * 60)
#define PUBLIC_ORIGIN_LIMIT 16

typedef struct {
    char origin[320];
    char authority[256];
    char hostname[254];
    bool secure;
} PublicOrigin;

typedef struct {
    char token[TOKEN_LENGTH + 1];
    time_t expires;
    DbInt user_id;
} Session;

typedef struct {
    char method[12];
    char target[1024];
    char host[256];
    char origin[320];
    char content_type[128];
    char cookie[4096];
    char fetch_site[32];
    size_t content_length;
    bool has_content_length;
    bool transfer_encoding;
    char *body;
} Request;

static Session sessions[SESSION_LIMIT];
static const char *public_dir;
static char authority[64], localhost_authority[64], public_authority[64], public_localhost[64];
static PublicOrigin public_origins[PUBLIC_ORIGIN_LIMIT];
static size_t public_origin_count;
static unsigned failed_attempts;
static time_t attempts_since;
static volatile sig_atomic_t stopping;

static void stop_server(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static void erase(void *pointer, size_t size) {
    volatile unsigned char *p = pointer;
    while (size--) *p++ = 0;
}

static bool random_bytes(unsigned char *buffer, size_t size) {
#ifdef __APPLE__
    return SecRandomCopyBytes(kSecRandomDefault, size, buffer) == errSecSuccess;
#else
    return size <= INT_MAX && RAND_bytes(buffer, (int)size) == 1;
#endif
}

static bool equal_hash(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char difference = 0;
    for (size_t i = 0; i < n; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

static bool write_all(int socket_fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    while (size) {
        ssize_t sent = send(socket_fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes += sent;
        size -= (size_t)sent;
    }
    return true;
}

static const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 303: return "See Other";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

static void respond(int fd, int status, const char *type, const char *body,
                    size_t length, const char *extra, bool head) {
    char headers[2048];
    int n = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'self'; "
        "style-src 'self'; img-src 'self' data:; font-src 'self'; "
        "connect-src 'self'; base-uri 'none'; form-action 'self'; "
        "frame-ancestors 'none'\r\n"
        "Connection: close\r\n%s\r\n",
        status, status_text(status), type, length, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof(headers)) return;
    if (write_all(fd, headers, (size_t)n) && !head && length)
        (void)write_all(fd, body, length);
}

static void json(int fd, int status, const char *body, const char *extra) {
    respond(fd, status, "application/json; charset=utf-8", body, strlen(body),
            extra, false);
}

static void error_response_head(int fd, int status, bool head) {
    const char *message = "Richiesta non valida.";
    switch (status) {
        case 401: message = "Email o password non corrette."; break;
        case 403: message = "Origine della richiesta non consentita."; break;
        case 503: message = "Database temporaneamente non disponibile. Riprova tra poco."; break;
        case 404: message = "Pagina non trovata."; break;
        case 405: message = "Metodo non consentito."; break;
        case 408: message = "Tempo della richiesta scaduto."; break;
        case 413: message = "Richiesta troppo grande."; break;
        case 415: message = "Formato della richiesta non supportato."; break;
        case 429: message = "Troppi tentativi. Riprova tra un minuto."; break;
        case 431: message = "Intestazioni troppo grandi."; break;
        case 500: message = "Errore del server. Riprova."; break;
    }
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    respond(fd, status, "application/json; charset=utf-8", body, strlen(body),
        status == 429 ? "Retry-After: 60\r\n" : NULL, head);
}

static void error_response(int fd, int status) {
    error_response_head(fd, status, false);
}

static bool copy_value(char *output, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (length >= capacity) return false;
    memcpy(output, value, length + 1);
    return true;
}

/* Explicit external origins keep proxy routing independent of untrusted
   Forwarded headers. DNS names are ASCII; international names use punycode. */
static bool normalize_authority(const char *value, bool secure, char output[256],
                                char hostname[254]) {
    const char *colon = strchr(value, ':');
    size_t length = colon ? (size_t)(colon - value) : strlen(value);
    if (!length || length > 253 || (colon && strchr(colon + 1, ':'))) return false;
    size_t label = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '.') {
            if (!label || value[i - 1] == '-') return false;
            label = 0;
        } else {
            if (c >= 128 || !(isalnum(c) || c == '-') ||
                (!label && c == '-') || ++label > 63) return false;
        }
        hostname[i] = (char)tolower(c);
    }
    if (!label || value[length - 1] == '-') return false;
    hostname[length] = '\0';
    unsigned port = secure ? 443 : 80;
    if (colon) {
        if (!colon[1]) return false;
        port = 0;
        for (const char *p = colon + 1; *p; p++) {
            if (*p < '0' || *p > '9' || port > 65535U / 10U) return false;
            port = port * 10U + (unsigned)(*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    int written = port == (secure ? 443U : 80U)
        ? snprintf(output, 256, "%s", hostname)
        : snprintf(output, 256, "%s:%u", hostname, port);
    return written > 0 && written < 256;
}

static bool parse_origin(const char *value, PublicOrigin *origin) {
    const char *host;
    if (strncmp(value, "https://", 8) == 0) {
        origin->secure = true; host = value + 8;
    } else if (strncmp(value, "http://", 7) == 0) {
        origin->secure = false; host = value + 7;
    } else return false;
    if (!normalize_authority(host, origin->secure, origin->authority, origin->hostname))
        return false;
    int written = snprintf(origin->origin, sizeof(origin->origin), "%s://%s",
                           origin->secure ? "https" : "http", origin->authority);
    return written > 0 && (size_t)written < sizeof(origin->origin);
}

static bool configure_public_origins(const char *value) {
    public_origin_count = 0;
    if (!value || !*value) return true;
    const char *start = value;
    for (;;) {
        const char *comma = strchr(start, ',');
        const char *end = comma ? comma : start + strlen(start);
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t length = (size_t)(end - start);
        char one[320];
        if (!length || length >= sizeof(one) || public_origin_count == PUBLIC_ORIGIN_LIMIT)
            return false;
        memcpy(one, start, length); one[length] = '\0';
        PublicOrigin *origin = &public_origins[public_origin_count];
        if (!parse_origin(one, origin)) return false;
        for (size_t i = 0; i < public_origin_count; i++)
            if (strcmp(public_origins[i].origin, origin->origin) == 0) return false;
        public_origin_count++;
        if (!comma) return true;
        start = comma + 1;
    }
}

static bool origin_matches_host(const PublicOrigin *origin, const char *host) {
    char normalized[256], hostname[254];
    if (!normalize_authority(host, origin->secure, normalized, hostname)) return false;
    if (strcmp(normalized, origin->authority) == 0) return true;
    /* Nginx Proxy Manager forwards $host without the published port. The
       browser Origin must still match the complete configured origin. */
    return !strchr(host, ':') && strcmp(hostname, origin->hostname) == 0;
}

static bool local_host(const char *host) {
    return strcmp(host, authority) == 0 || strcmp(host, localhost_authority) == 0 ||
        strcmp(host, public_authority) == 0 || strcmp(host, public_localhost) == 0;
}

static bool allowed_host(const char *host) {
    if (local_host(host)) return true;
    for (size_t i = 0; i < public_origin_count; i++)
        if (origin_matches_host(&public_origins[i], host)) return true;
    return false;
}

static bool secure_request(const Request *request) {
    if (request->origin[0]) {
        PublicOrigin origin;
        return parse_origin(request->origin, &origin) && origin.secure;
    }
    for (size_t i = 0; i < public_origin_count; i++)
        if (public_origins[i].secure && origin_matches_host(&public_origins[i], request->host))
            return true;
    return false;
}

/* Bounded HTTP/1.1 parser. One request per connection; no chunked uploads. */
static int read_request(int fd, char *buffer, Request *request) {
    size_t used = 0, header_size = 0;
    while (!header_size) {
        if (used == REQUEST_LIMIT) return 431;
        ssize_t received = recv(fd, buffer + used, REQUEST_LIMIT - used, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return received < 0 ? 408 : 400;
        if (memchr(buffer + used, '\0', (size_t)received)) return 400;
        used += (size_t)received;
        buffer[used] = '\0';
        char *end = strstr(buffer, "\r\n\r\n");
        if (end) header_size = (size_t)(end - buffer) + 4;
        if (header_size > HEADER_LIMIT || (!header_size && used >= HEADER_LIMIT)) return 431;
    }

    char *first_end = strstr(buffer, "\r\n");
    if (!first_end) return 400;
    *first_end = '\0';
    char version[16], trailing;
    if (sscanf(buffer, "%11s %1023s %15s %c", request->method, request->target,
               version, &trailing) != 3 || strcmp(version, "HTTP/1.1") != 0)
        return 400;
    if (request->target[0] != '/' || strchr(request->target, '#')) return 400;

    bool seen_host = false, seen_origin = false, seen_cookie = false;
    bool seen_type = false, seen_fetch = false;
    char *line = first_end + 2;
    while (line < buffer + header_size - 2) {
        char *end = strstr(line, "\r\n");
        if (!end) return 400;
        *end = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return 400;
        *colon = '\0';
        for (char *p = line; *p; p++) {
            if (!(isalnum((unsigned char)*p) || strchr("!#$%&'*+-.^_`|~", *p)))
                return 400;
        }
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        for (char *p = value; *p; p++)
            if (((unsigned char)*p < 32 && *p != '\t') || *p == 127) return 400;

        if (strcasecmp(line, "Host") == 0) {
            if (seen_host || !copy_value(request->host, sizeof(request->host), value))
                return 400;
            seen_host = true;
        } else if (strcasecmp(line, "Origin") == 0) {
            if (seen_origin || !copy_value(request->origin, sizeof(request->origin), value))
                return 400;
            seen_origin = true;
        } else if (strcasecmp(line, "Cookie") == 0) {
            if (seen_cookie || !copy_value(request->cookie, sizeof(request->cookie), value))
                return 400;
            seen_cookie = true;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (seen_type || !copy_value(request->content_type, sizeof(request->content_type), value))
                return 400;
            seen_type = true;
        } else if (strcasecmp(line, "Sec-Fetch-Site") == 0) {
            if (seen_fetch || !copy_value(request->fetch_site, sizeof(request->fetch_site), value))
                return 400;
            seen_fetch = true;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            if (request->has_content_length || !*value) return 400;
            for (char *p = value; *p; p++) if (!isdigit((unsigned char)*p)) return 400;
            errno = 0;
            char *number_end;
            unsigned long long number = strtoull(value, &number_end, 10);
            size_t body_limit = strncmp(request->target, "/api/login", 10) == 0
                ? LOGIN_BODY_LIMIT : BODY_LIMIT;
            if (errno || *number_end || number > body_limit) return 413;
            request->content_length = (size_t)number;
            request->has_content_length = true;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            request->transfer_encoding = true;
        }
        line = end + 2;
    }
    if (!seen_host || !request->host[0] || request->transfer_encoding) return 400;
    if (!allowed_host(request->host)) return 403;
    if (header_size + request->content_length > REQUEST_LIMIT) return 413;
    size_t total = header_size + request->content_length;
    while (used < total) {
        ssize_t received = recv(fd, buffer + used, total - used, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return received < 0 ? 408 : 400;
        if (memchr(buffer + used, '\0', (size_t)received)) return 400;
        used += (size_t)received;
    }
    if (used != total) return 400;
    buffer[used] = '\0';
    request->body = buffer + header_size;
    return 0;
}

static bool same_origin(const Request *request) {
    if (strcmp(request->fetch_site, "cross-site") == 0) return false;
    if (!request->origin[0]) return true; /* CLI clients do not send Origin. */
    PublicOrigin origin;
    if (!parse_origin(request->origin, &origin) || !origin_matches_host(&origin, request->host))
        return false;
    if (!origin.secure && local_host(request->host)) {
        char expected[320];
        snprintf(expected, sizeof(expected), "http://%s", request->host);
        if (strcmp(request->origin, expected) == 0) return true;
    }
    for (size_t i = 0; i < public_origin_count; i++)
        if (strcmp(origin.origin, public_origins[i].origin) == 0) return true;
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_form(char *output, size_t capacity, const char *input) {
    size_t used = 0;
    while (*input) {
        unsigned char c = (unsigned char)*input++;
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (!input[0] || !input[1]) return false;
            int high = hex_value(input[0]), low = hex_value(input[1]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)(high * 16 + low);
            input += 2;
        }
        if (used + 1 >= capacity || c < 32 || c == 127) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    return true;
}

static bool login_fields(char *body, char *email, char *password, bool *remember) {
    bool got_email = false, got_password = false, got_remember = false;
    char *save;
    for (char *field = strtok_r(body, "&", &save); field; field = strtok_r(NULL, "&", &save)) {
        char *equals = strchr(field, '=');
        if (!equals) return false;
        *equals++ = '\0';
        char key[64];
        if (!decode_form(key, sizeof(key), field)) return false;
        if (strcmp(key, "email") == 0) {
            if (got_email || !decode_form(email, 255, equals)) return false;
            got_email = true;
        } else if (strcmp(key, "password") == 0) {
            if (got_password || !decode_form(password, 1025, equals)) return false;
            got_password = true;
        } else if (strcmp(key, "remember") == 0) {
            char value[16];
            if (got_remember || !decode_form(value, sizeof(value), equals)) return false;
            *remember = strcmp(value, "on") == 0 || strcmp(value, "true") == 0;
            got_remember = true;
        } else return false;
    }
    return got_email && got_password && email[0] && password[0];
}

static Session *find_session(const char *cookie) {
    const char *start = cookie;
    while (*start) {
        while (*start == ' ' || *start == ';') start++;
        const char *end = strchr(start, ';');
        if (!end) end = start + strlen(start);
        const char prefix[] = "punto_session=";
        if ((size_t)(end - start) == sizeof(prefix) - 1 + TOKEN_LENGTH &&
            strncmp(start, prefix, sizeof(prefix) - 1) == 0) {
            const unsigned char *token = (const unsigned char *)(start + sizeof(prefix) - 1);
            for (size_t i = 0; i < SESSION_LIMIT; i++) {
                if (sessions[i].expires > time(NULL) &&
                    equal_hash(token, (const unsigned char *)sessions[i].token, TOKEN_LENGTH)) {
                    Account account;
                    /* A temporary database outage must not revoke a valid token. */
                    if (accounts_lookup(sessions[i].user_id, &account) != 0) return &sessions[i];
                    erase(&sessions[i], sizeof(sessions[i]));
                    return NULL;
                }
            }
            return NULL;
        }
        start = end;
    }
    return NULL;
}

static Session *new_session(int duration, DbInt user_id) {
    size_t slot = 0;
    time_t now = time(NULL);
    for (size_t i = 0; i < SESSION_LIMIT; i++) {
        if (sessions[i].expires <= now) { slot = i; break; }
        if (sessions[i].expires < sessions[slot].expires) slot = i;
    }
    unsigned char random[TOKEN_BYTES];
    if (!random_bytes(random, sizeof(random))) return NULL;
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        sessions[slot].token[2 * i] = digits[random[i] >> 4];
        sessions[slot].token[2 * i + 1] = digits[random[i] & 15];
    }
    sessions[slot].token[TOKEN_LENGTH] = '\0';
    sessions[slot].expires = now + duration;
    sessions[slot].user_id = user_id;
    erase(random, sizeof(random));
    return &sessions[slot];
}

static void login(int fd, Request *request) {
    if (!same_origin(request)) { error_response(fd, 403); return; }
    const char expected[] = "application/x-www-form-urlencoded";
    size_t expected_length = sizeof(expected) - 1;
    if (strncmp(request->content_type, expected, expected_length) != 0 ||
        (request->content_type[expected_length] && request->content_type[expected_length] != ';')) {
        error_response(fd, 415); return;
    }
    char email[255] = {0}, password[1025] = {0};
    bool remember = false;
    if (!login_fields(request->body, email, password, &remember)) {
        erase(password, sizeof(password));
        error_response(fd, 400); return;
    }
    time_t now = time(NULL);
    if (now - attempts_since >= 60) { failed_attempts = 0; attempts_since = now; }
    if (failed_attempts >= 8) {
        erase(password, sizeof(password));
        error_response(fd, 429); return;
    }
    Account account;
    int authenticated = accounts_authenticate(email, password, &account);
    erase(password, sizeof(password));
    if (authenticated < 0) { error_response(fd, 503); return; }
    if (!authenticated) {
        failed_attempts++;
        error_response(fd, 401); return;
    }
    failed_attempts = 0;
    Session *old = find_session(request->cookie);
    if (old) erase(old, sizeof(*old));
    int duration = remember ? LONG_SESSION : SHORT_SESSION;
    Session *session = new_session(duration, account.id);
    if (!session) { error_response(fd, 500); return; }
    char cookie[256];
    const char *secure = secure_request(request) ? "; Secure" : "";
    if (remember) {
        snprintf(cookie, sizeof(cookie),
            "Set-Cookie: punto_session=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s\r\n",
            session->token, duration, secure);
    } else {
        snprintf(cookie, sizeof(cookie),
            "Set-Cookie: punto_session=%s; Path=/; HttpOnly; SameSite=Lax%s\r\n", session->token, secure);
    }
    json(fd, 200, "{\"ok\":true}", cookie);
}

static void serve_file(int fd, const char *name, const char *mime, bool head) {
    char filename[4096];
    int length = snprintf(filename, sizeof(filename), "%s/%s", public_dir, name);
    if (length < 0 || (size_t)length >= sizeof(filename)) { error_response_head(fd, 500, head); return; }
    FILE *file = fopen(filename, "rb");
    if (!file) { error_response_head(fd, 404, head); return; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); error_response_head(fd, 500, head); return; }
    long file_size = ftell(file);
    if (file_size < 0 || file_size > 2 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); error_response_head(fd, 500, head); return;
    }
    char *data = malloc((size_t)file_size + 1);
    if (!data) { fclose(file); error_response_head(fd, 500, head); return; }
    size_t read_size = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size == (size_t)file_size)
        respond(fd, 200, mime, data, read_size, NULL, head);
    else error_response_head(fd, 500, head);
    free(data);
}

/* Output needs at most six bytes per input byte, plus the terminating NUL. */
static void escape_json(char *output, const char *input) {
    const char hex[] = "0123456789abcdef";
    while (*input) {
        unsigned char c = (unsigned char)*input++;
        if (c == '"' || c == '\\') { *output++ = '\\'; *output++ = (char)c; }
        else if (c < 32) {
            memcpy(output, "\\u00", 4); output += 4;
            *output++ = hex[c >> 4]; *output++ = hex[c & 15];
        } else *output++ = (char)c;
    }
    *output = '\0';
}

static void session_response(int fd, const Session *session) {
    Account account;
    int found = session ? accounts_lookup(session->user_id, &account) : 0;
    if (found < 0) { error_response(fd, 503); return; }
    if (!found) {
        json(fd, 200, "{\"authenticated\":false}", NULL); return;
    }
    char email[6 * sizeof(account.email)], name[6 * sizeof(account.name)];
    escape_json(email, account.email);
    escape_json(name, account.name);
    bool manager = strcmp(account.role, "manager") == 0;
    const char *allowed = manager ? "true" : "false";
    char body[sizeof(email) + sizeof(name) + 512];
    snprintf(body, sizeof(body),
        "{\"authenticated\":true,\"email\":\"%s\",\"name\":\"%s\",\"role\":\"%s\","
        "\"permissions\":{\"manage_jobs\":%s,\"manage_users\":%s,\"edit_time_entries\":%s,"
        "\"manage_assignments\":%s,\"view_audit\":%s}}",
        email, name, account.role, allowed, allowed, allowed, allowed, allowed);
    json(fd, 200, body, NULL);
}

static bool workspace_route(int fd, Request *request, const char *query) {
    const char *target = request->target;
    if (strcmp(target, "/api/jobs") != 0 && strncmp(target, "/api/jobs/", 10) != 0 &&
        strcmp(target, "/api/time-entries") != 0 && strncmp(target, "/api/time-entries/", 18) != 0 &&
        strcmp(target, "/api/users") != 0 && strncmp(target, "/api/users/", 11) != 0 &&
        strcmp(target, "/api/audit") != 0)
        return false;
    bool head = strcmp(request->method, "HEAD") == 0;
    Session *session = find_session(request->cookie);
    Account account;
    int found = session ? accounts_lookup(session->user_id, &account) : 0;
    if (found < 0) { error_response_head(fd, 503, head); return true; }
    if (!found) {
        const char *body = "{\"ok\":false,\"error\":\"Accedi per continuare.\"}";
        respond(fd, 401, "application/json; charset=utf-8", body, strlen(body), NULL, head);
        return true;
    }
    bool mutation = strcmp(request->method, "POST") == 0 ||
                    strcmp(request->method, "DELETE") == 0;
    if (mutation && !same_origin(request)) {
        error_response_head(fd, 403, head); return true;
    }
    if (strcmp(request->method, "POST") == 0) {
        const char type[] = "application/x-www-form-urlencoded";
        size_t length = sizeof(type) - 1;
        if (strncmp(request->content_type, type, length) != 0 ||
            (request->content_type[length] && request->content_type[length] != ';')) {
            error_response(fd, 415); return true;
        }
    }
    WorkspaceResponse response = {0};
    bool handled = accounts_handle(request->method, target, query, request->body,
                                   &account, &response);
    if (handled && response.status == 200 && strcmp(request->method, "POST") == 0 &&
        strncmp(target, "/api/users/", 11) == 0) {
        /* Successful item updates already validated the numeric identifier. */
        DbInt updated_id = (DbInt)strtoll(target + 11, NULL, 10);
        Account updated;
        if (!accounts_find(updated_id, &updated)) {
            for (size_t i = 0; i < SESSION_LIMIT; i++) {
                if (sessions[i].user_id == updated_id) erase(&sessions[i], sizeof(sessions[i]));
            }
        }
    }
    if (!handled) handled = storage_handle(request->method, target, query, account.role, &response);
    if (!handled) handled = workspace_handle(request->method, target, query, request->body,
                                             account.id, account.email, account.role, &response);
    if (!handled) {
        error_response_head(fd, 404, head);
    } else if (!response.body) {
        error_response_head(fd, 500, head);
    } else {
        respond(fd, response.status, "application/json; charset=utf-8", response.body,
                strlen(response.body), NULL, head);
    }
    workspace_response_free(&response);
    return true;
}

static void route(int fd, Request *request) {
    if (strcmp(request->target, "/healthz") == 0) {
        bool head = strcmp(request->method, "HEAD") == 0;
        if (!head && strcmp(request->method, "GET") != 0) {
            error_response(fd, 405); return;
        }
#ifdef PUNTO_MYSQL
        const char *healthy = "{\"ok\":true,\"database\":\"mysql\"}";
#else
        const char *healthy = "{\"ok\":true,\"database\":\"sqlite\"}";
#endif
        bool okay = storage_health();
        const char *body = okay ? healthy : "{\"ok\":false}";
        respond(fd, okay ? 200 : 503, "application/json; charset=utf-8", body, strlen(body), NULL, head);
        return;
    }
    char *query = strchr(request->target, '?');
    if (query) *query++ = '\0';
    if (workspace_route(fd, request, query ? query : "")) return;
    const char *target = request->target;
    bool get = strcmp(request->method, "GET") == 0;
    bool head = strcmp(request->method, "HEAD") == 0;
    bool post = strcmp(request->method, "POST") == 0;
    if (strcmp(target, "/api/login") == 0) {
        if (!post) { error_response_head(fd, 405, head); return; }
        login(fd, request); return;
    }
    if (strcmp(target, "/api/session") == 0) {
        if (!get) { error_response_head(fd, 405, head); return; }
        session_response(fd, find_session(request->cookie)); return;
    }
    if (strcmp(target, "/api/logout") == 0) {
        if (!post) { error_response_head(fd, 405, head); return; }
        if (!same_origin(request)) { error_response_head(fd, 403, head); return; }
        Session *session = find_session(request->cookie);
        if (session) erase(session, sizeof(*session));
        json(fd, 200, "{\"ok\":true}", secure_request(request)
            ? "Set-Cookie: punto_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0; Secure\r\n"
            : "Set-Cookie: punto_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n");
        return;
    }
    if (strncmp(target, "/api/", 5) == 0) { error_response_head(fd, 404, head); return; }
    if (!get && !head) { error_response_head(fd, 405, head); return; }
    if (strcmp(target, "/app") == 0 || strcmp(target, "/app.html") == 0) {
        if (!find_session(request->cookie)) {
            respond(fd, 303, "text/plain; charset=utf-8", "", 0, "Location: /\r\n", head);
            return;
        }
        serve_file(fd, "app.html", "text/html; charset=utf-8", head); return;
    }
    const struct { const char *url, *file, *mime; } files[] = {
        {"/", "index.html", "text/html; charset=utf-8"},
        {"/index.html", "index.html", "text/html; charset=utf-8"},
        {"/styles.css", "styles.css", "text/css; charset=utf-8"},
        {"/login.js", "login.js", "text/javascript; charset=utf-8"},
        {"/app.js", "app.js", "text/javascript; charset=utf-8"},
        {"/workspace.css", "workspace.css", "text/css; charset=utf-8"},
        {"/favicon.svg", "favicon.svg", "image/svg+xml"}
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (strcmp(target, files[i].url) == 0) {
            serve_file(fd, files[i].file, files[i].mime, head); return;
        }
    }
    error_response_head(fd, 404, head);
}

int main(void) {
    umask(0077);
    if (!getenv("TZ") && setenv("TZ", "Europe/Rome", 1) != 0) {
        perror("Configurazione fuso orario"); return 1;
    }
    tzset();
    const char *port_env = getenv("PORT");
    char *port_end;
    long port = 8080;
    if (port_env) {
        errno = 0;
        port = strtol(port_env, &port_end, 10);
        if (errno || !*port_env || *port_end || port < 1024 || port > 65535) {
            fprintf(stderr, "PORT deve essere un numero tra 1024 e 65535.\n"); return 1;
        }
    }
    long public_port = port;
    const char *public_port_env = getenv("PUBLIC_PORT");
    if (public_port_env) {
        errno = 0;
        public_port = strtol(public_port_env, &port_end, 10);
        if (errno || !*public_port_env || *port_end || public_port < 1024 || public_port > 65535) {
            fprintf(stderr, "PUBLIC_PORT deve essere un numero tra 1024 e 65535.\n"); return 1;
        }
    }
    const char *bind_host = getenv("BIND_HOST");
    if (!bind_host || !*bind_host) bind_host = "127.0.0.1";
    struct in_addr bind_address;
    if (inet_pton(AF_INET, bind_host, &bind_address) != 1) {
        fprintf(stderr, "BIND_HOST deve essere un indirizzo IPv4 valido.\n"); return 1;
    }
    if (!configure_public_origins(getenv("PUBLIC_ORIGINS"))) {
        fprintf(stderr, "PUBLIC_ORIGINS deve contenere al massimo 16 origini http/https univoche, senza percorso o credenziali.\n");
        return 1;
    }
    const char *email = getenv("PUNTO_EMAIL");
    const char *password = getenv("PUNTO_PASSWORD");
    if (!email) email = "demo@punto.local";
    if (!password) password = "Punto2026!";
    if (!*email || strlen(email) >= 255 || !strchr(email, '@') ||
        strlen(password) < 8 || strlen(password) > 1024) {
        fprintf(stderr, "Configura PUNTO_EMAIL e PUNTO_PASSWORD (8–1024 caratteri).\n"); return 1;
    }
    for (const char *p = email; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
            fprintf(stderr, "PUNTO_EMAIL contiene caratteri non validi.\n"); return 1;
        }
    }
    public_dir = getenv("PUBLIC_DIR");
    if (!public_dir || !*public_dir) public_dir = "public";
#ifdef PUNTO_MYSQL
    const char *database_path = "mysql";
#else
    const char *database_path = getenv("DB_PATH");
    if (!database_path || !*database_path) database_path = "data/punto.db";
    if (strcmp(database_path, "data/punto.db") == 0) {
        if (mkdir("data", 0700) != 0 && errno != EEXIST) {
            perror("Creazione cartella dati"); return 1;
        }
    }
#endif
    if (!storage_preflight(database_path)) {
        fprintf(stderr, "Database non leggibile o creato da una versione più recente. Nessun aggiornamento eseguito.\n");
        return 1;
    }
    if (!workspace_open(database_path)) {
        fprintf(stderr, "Impossibile aprire il database. Controlla DB_PATH e la cartella dei dati.\n");
        return 1;
    }
    if (!accounts_open(database_path, email, password)) {
        fprintf(stderr, "Impossibile inizializzare gli utenti. Controlla database e account iniziale.\n");
        workspace_close(); return 1;
    }
    if (!storage_open(database_path)) {
        fprintf(stderr, "Impossibile aggiornare il database. I dati restano conservati; controlla la versione e i permessi.\n");
        accounts_close(); workspace_close(); return 1;
    }
    snprintf(authority, sizeof(authority), "127.0.0.1:%ld", port);
    snprintf(localhost_authority, sizeof(localhost_authority), "localhost:%ld", port);
    snprintf(public_authority, sizeof(public_authority), "127.0.0.1:%ld", public_port);
    snprintf(public_localhost, sizeof(public_localhost), "localhost:%ld", public_port);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {0};
    stop_action.sa_handler = stop_server;
    sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); storage_close(); accounts_close(); workspace_close(); return 1; }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr = bind_address;
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 16) < 0) {
        perror("Avvio server"); close(server); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    printf("\n  punto · Project management\n  http://127.0.0.1:%ld\n  Utenti e dati: %s\n\n", public_port, database_path);
    fflush(stdout);
    while (!stopping) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        char buffer[REQUEST_LIMIT + 1];
        Request request = {0};
        int status = read_request(client, buffer, &request);
        if (status) error_response(client, status);
        else route(client, &request);
        erase(buffer, sizeof(buffer));
        close(client);
    }
    close(server);
    storage_close();
    accounts_close();
    workspace_close();
    erase(sessions, sizeof(sessions));
    return stopping ? 0 : 1;
}
