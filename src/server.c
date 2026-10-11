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
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>

#include "workspace.h"
#include "accounts.h"
#include "access_control.h"
#include "improvements.h"
#include "storage.h"
#include "procurement.h"
#include "documents.h"
#include "organization.h"
#include "alerts.h"
#include "calendar_client.h"
#include "inventory_client.h"
#include "work_board.h"
#include "ticket_bridge.h"
#include "../auth/oidc.h"
#include "../auth/http_transport.h"

#ifdef __APPLE__
#include <Security/SecRandom.h>
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

#define HEADER_LIMIT 16384
#define BODY_LIMIT 32768
#define WORK_BOARD_BODY_LIMIT 65536
#define LOGIN_BODY_LIMIT 8192
#define SESSION_LIMIT 64
#define SESSIONS_PER_USER 4
#define LOGIN_FAILURE_LIMIT 128
#define TOKEN_BYTES 32
#define TOKEN_LENGTH (TOKEN_BYTES * 2)
#define SHORT_SESSION (8 * 60 * 60)
#define LONG_SESSION (30 * 24 * 60 * 60)
#define PUBLIC_ORIGIN_LIMIT 16
#define TICKET_GRANT_LIMIT 128
#define TICKET_GRANTS_PER_SESSION 2

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
    uint64_t order;
} Session;

typedef struct {
    char token[TOKEN_LENGTH + 1];
    char session_binding[TOKEN_LENGTH + 1];
    char state[TOKEN_LENGTH + 1];
    time_t expires;
    DbInt user_id;
    bool calendar_access;
    uint64_t order;
} TicketGrant;

typedef struct {
    char email[255];
    time_t since;
    unsigned failures;
} LoginFailures;

typedef struct {
    char method[12];
    char target[8192];
    char host[256];
    char origin[320];
    char content_type[128];
    char cookie[4096];
    char fetch_site[32];
    char authorization[256];
    size_t content_length;
    bool has_content_length;
    bool transfer_encoding;
    char *body;
} Request;

static Session sessions[SESSION_LIMIT];
static TicketGrant ticket_grants[TICKET_GRANT_LIMIT];
static PublicOrigin tickets_app_origin;
static bool tickets_configured;
static const char *public_dir;
static char authority[64], localhost_authority[64], public_authority[64], public_localhost[64];
static PublicOrigin public_origins[PUBLIC_ORIGIN_LIMIT];
static size_t public_origin_count;
static PublicOrigin sso_origin;
static LoginFailures login_failures[LOGIN_FAILURE_LIMIT];
static uint64_t session_order, grant_order;
static volatile sig_atomic_t stopping;

static bool calendar_session_binding(const Session *session, char binding[65]);

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

/* Release dependent capabilities immediately, including grants never presented
   again after logout. Only the revoked session's grants are affected. */
static void revoke_session(Session *session) {
    if (!session || !session->user_id) return;
    char binding[TOKEN_LENGTH + 1];
    bool bound = calendar_session_binding(session, binding);
    for (size_t i = 0; i < TICKET_GRANT_LIMIT; i++) {
        TicketGrant *grant = &ticket_grants[i];
        if (grant->user_id == session->user_id && (!bound ||
            equal_hash((const unsigned char *)binding,
                       (const unsigned char *)grant->session_binding, TOKEN_LENGTH)))
            erase(grant, sizeof(*grant));
    }
    erase(binding, sizeof(binding));
    erase(session, sizeof(*session));
}

/* A bounded identity-specific failure window. Failures cannot block another
   identity, and rejected attempts do not extend the fixed sixty-second window. */
static LoginFailures *login_failure_bucket(const char *email, bool create) {
    char canonical[255];
    const char *start = email;
    while (*start == ' ' || *start == '\t') start++;
    size_t length = strlen(start);
    while (length && (start[length - 1] == ' ' || start[length - 1] == '\t')) length--;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)start[i];
        canonical[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : (char)c;
    }
    canonical[length] = '\0';
    time_t now = time(NULL);
    size_t available = LOGIN_FAILURE_LIMIT, oldest = 0;
    for (size_t i = 0; i < LOGIN_FAILURE_LIMIT; i++) {
        LoginFailures *bucket = &login_failures[i];
        if (bucket->email[0] && (now < bucket->since || now - bucket->since >= 60))
            erase(bucket, sizeof(*bucket));
        if (bucket->email[0] && !strcmp(bucket->email, canonical)) return bucket;
        if (!bucket->email[0] && available == LOGIN_FAILURE_LIMIT) available = i;
        if (bucket->since < login_failures[oldest].since) oldest = i;
    }
    if (!create) return NULL;
    LoginFailures *bucket = &login_failures[available < LOGIN_FAILURE_LIMIT ? available : oldest];
    erase(bucket, sizeof(*bucket));
    memcpy(bucket->email, canonical, length + 1);
    bucket->since = now;
    return bucket;
}

static bool write_all(int socket_fd, const void *data, size_t size) {
    return http_transport_write(socket_fd, data, size);
}

static const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
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
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

static void respond(int fd, int status, const char *type, const char *body,
                    size_t length, const char *extra, bool head) {
    char headers[12288];
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

static bool configure_tickets(const char *value) {
    tickets_configured = false;
    if (!value || !*value) return true;
    if (!parse_origin(value, &tickets_app_origin)) return false;
    /* Bearer grants cross services only over TLS; local development stays on
       loopback. Never derive the destination from an incoming HTTP header. */
    if (!tickets_app_origin.secure && strcmp(tickets_app_origin.hostname, "localhost") &&
        strcmp(tickets_app_origin.hostname, "127.0.0.1")) return false;
    tickets_configured = true;
    return true;
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

static bool configure_sso_origin(void) {
    if (!oidc_enabled()) return true;
    const char *redirect = oidc_redirect_uri();
    const char *scheme_end = strstr(redirect, "://");
    const char *path = scheme_end ? strchr(scheme_end + 3, '/') : NULL;
    if (!path || strcmp(path, "/auth/callback")) return false;
    size_t length = (size_t)(path - redirect);
    char origin[320];
    if (length >= sizeof(origin)) return false;
    memcpy(origin, redirect, length);
    origin[length] = '\0';
    if (!parse_origin(origin, &sso_origin)) return false;
    if (!sso_origin.secure && local_host(sso_origin.authority)) return true;
    for (size_t i = 0; i < public_origin_count; i++)
        if (!strcmp(public_origins[i].origin, sso_origin.origin)) return true;
    return false;
}

/* Bounded HTTP/1.1 parser. One request per connection; no chunked uploads. */
static bool pdf_upload_target(const char *target) {
    const char prefix[] = "/api/contracts/";
    if (strncmp(target, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *p = target + sizeof(prefix) - 1;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    const char suffix[] = "/documents";
    return strncmp(p, suffix, sizeof(suffix) - 1) == 0 &&
        (p[sizeof(suffix) - 1] == '\0' || p[sizeof(suffix) - 1] == '?');
}

static size_t request_body_limit(const char *method, const char *target) {
    bool work_board_form = strcmp(method, "POST") == 0 &&
        (strcmp(target, "/api/tasks") == 0 || strncmp(target, "/api/tasks/", 11) == 0 ||
         strcmp(target, "/api/blockers") == 0 || strncmp(target, "/api/blockers/", 14) == 0);
    bool inventory_import = strcmp(method, "POST") == 0 &&
        strcmp(target, "/api/inventory/import/preview") == 0;
    bool workflow_batch = !strcmp(method, "POST") &&
        (!strcmp(target, "/api/time-entries/batch") || !strcmp(target, "/api/jobs/wizard") ||
         !strcmp(target, "/api/job-templates"));
    return strncmp(target, "/api/login", 10) == 0 ? LOGIN_BODY_LIMIT :
        workflow_batch ? 2U * 1024U * 1024U :
        (strcmp(method, "POST") == 0 && pdf_upload_target(target) ? DOCUMENTS_MAX_BYTES :
         work_board_form ? WORK_BOARD_BODY_LIMIT : inventory_import ? 1024U * 1024U : BODY_LIMIT);
}

static int check_http_headers(const char *raw, size_t header_size,
                              size_t content_length, void *context) {
    (void)context;
    const char *end = strstr(raw, "\r\n");
    if (!end || (size_t)(end - raw) >= header_size || header_size > HEADER_LIMIT) return 400;
    char line[HEADER_LIMIT + 1], method[12], target[8192], version[16], trailing;
    size_t length = (size_t)(end - raw);
    memcpy(line, raw, length);
    line[length] = '\0';
    if (sscanf(line, "%11s %8191s %15s %c", method, target, version, &trailing) != 3 ||
        strcmp(version, "HTTP/1.1") || target[0] != '/' || strchr(target, '#')) return 400;
    return content_length > request_body_limit(method, target) ? 413 : 0;
}

static int read_request(char *buffer, size_t used, Request *request) {
    char *header_end = strstr(buffer, "\r\n\r\n");
    if (!header_end) return used >= HEADER_LIMIT ? 431 : 400;
    size_t header_size = (size_t)(header_end - buffer) + 4;
    if (header_size > HEADER_LIMIT) return 431;
    if (memchr(buffer, '\0', header_size)) return 400;

    char *first_end = strstr(buffer, "\r\n");
    if (!first_end) return 400;
    *first_end = '\0';
    char version[16], trailing;
    if (sscanf(buffer, "%11s %8191s %15s %c", request->method, request->target,
               version, &trailing) != 3 || strcmp(version, "HTTP/1.1") != 0)
        return 400;
    if (request->target[0] != '/' || strchr(request->target, '#')) return 400;

    bool seen_host = false, seen_origin = false, seen_cookie = false;
    bool seen_type = false, seen_fetch = false, seen_authorization = false;
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
        } else if (strcasecmp(line, "Authorization") == 0) {
            if (seen_authorization || !copy_value(request->authorization, sizeof(request->authorization), value))
                return 400;
            seen_authorization = true;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            if (request->has_content_length || !*value) return 400;
            for (char *p = value; *p; p++) if (!isdigit((unsigned char)*p)) return 400;
            errno = 0;
            char *number_end;
            unsigned long long number = strtoull(value, &number_end, 10);
            size_t body_limit = request_body_limit(request->method, request->target);
            if (errno || *number_end || number > body_limit) return 413;
            request->content_length = (size_t)number;
            request->has_content_length = true;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            request->transfer_encoding = true;
        }
        line = end + 2;
    }
    if (!seen_host || !request->host[0] || request->transfer_encoding) return 400;
    bool host_allowed = allowed_host(request->host);
    size_t total = header_size + request->content_length;
    if (used != total) return 400;
    buffer[used] = '\0';
    request->body = buffer + header_size;
    /* Consume the bounded declared body before rejecting Host. Closing with
       unread TCP bytes can reset the connection and discard the HTTP error. */
    if (!host_allowed) return 403;
    if (!(strcmp(request->method, "POST") == 0 && pdf_upload_target(request->target)) &&
        memchr(request->body, '\0', request->content_length)) return 400;
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
                    revoke_session(&sessions[i]);
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
    time_t now = time(NULL);
    if (now == (time_t)-1) return NULL;
    size_t slot = SESSION_LIMIT, own_oldest = SESSION_LIMIT, own_count = 0;
    for (size_t i = 0; i < SESSION_LIMIT; i++) {
        if (sessions[i].expires <= now) {
            revoke_session(&sessions[i]);
            if (slot == SESSION_LIMIT) slot = i;
        } else if (sessions[i].user_id == user_id) {
            own_count++;
            if (own_oldest == SESSION_LIMIT || sessions[i].order < sessions[own_oldest].order)
                own_oldest = i;
        }
    }
    /* Repeated logins can replace only this account's sessions. A full pool
       must never silently revoke another user's authenticated session. */
    if (own_count >= SESSIONS_PER_USER || slot == SESSION_LIMIT) slot = own_oldest;
    if (slot == SESSION_LIMIT) return NULL;
    unsigned char random[TOKEN_BYTES];
    if (!random_bytes(random, sizeof(random))) return NULL;
    revoke_session(&sessions[slot]);
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        sessions[slot].token[2 * i] = digits[random[i] >> 4];
        sessions[slot].token[2 * i + 1] = digits[random[i] & 15];
    }
    sessions[slot].token[TOKEN_LENGTH] = '\0';
    sessions[slot].expires = now + duration;
    sessions[slot].user_id = user_id;
    sessions[slot].order = ++session_order;
    erase(random, sizeof(random));
    return &sessions[slot];
}

static void login(int fd, Request *request) {
    if (!oidc_local_enabled()) {
        json(fd, 403, "{\"ok\":false,\"error\":\"Usa l’accesso aziendale.\"}", NULL);
        return;
    }
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
    if (!oidc_local_account_allowed(email)) {
        erase(password, sizeof(password));
        json(fd, 403, "{\"ok\":false,\"error\":\"Usa l’accesso aziendale.\"}", NULL);
        return;
    }
    LoginFailures *bucket = login_failure_bucket(email, false);
    if (bucket && bucket->failures >= 8) {
        erase(password, sizeof(password));
        error_response(fd, 429); return;
    }
    Account account;
    int authenticated = accounts_authenticate(email, password, &account);
    erase(password, sizeof(password));
    if (authenticated < 0) { error_response(fd, 503); return; }
    if (!authenticated) {
        bucket = login_failure_bucket(email, true);
        bucket->failures++;
        error_response(fd, 401); return;
    }
    bucket = login_failure_bucket(email, false);
    if (bucket) erase(bucket, sizeof(*bucket));
    Session *old = find_session(request->cookie);
    if (old) revoke_session(old);
    int duration = remember ? LONG_SESSION : SHORT_SESSION;
    Session *session = new_session(duration, account.id);
    if (!session) { error_response(fd, 503); return; }
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

static void serve_file_response(int fd, const char *name, const char *mime,
                               bool head, int status, const char *extra);

static void sso_login(int fd, const Request *request) {
    char location[8192], cookie[512], headers[9216];
    /* A host-only transaction cookie must be issued on the callback origin.
       Redirect aliases before allocating state; never reflect a requested
       return URL or trust Forwarded headers to choose the destination. HTTPS
       for configured secure origins is enforced by the reverse proxy. */
    if (!origin_matches_host(&sso_origin, request->host) ||
        secure_request(request) != sso_origin.secure) {
        snprintf(headers, sizeof(headers), "Location: %s/auth/login\r\n", sso_origin.origin);
        respond(fd, 303, "text/plain; charset=utf-8", "", 0, headers, false);
        return;
    }
    int status = oidc_start(location, cookie);
    if (status != 200) {
        serve_file_response(fd, "auth-error.html", "text/html; charset=utf-8",
                            false, status, oidc_clear_cookie());
        return;
    }
    int n = snprintf(headers, sizeof(headers), "Location: %s\r\n%s", location, cookie);
    if (n < 0 || (size_t)n >= sizeof(headers)) { error_response(fd, 500); return; }
    respond(fd, 303, "text/plain; charset=utf-8", "", 0, headers, false);
}

static void sso_callback(int fd, Request *request, const char *query) {
    char email[255];
    int lifetime = 0;
    int status;
    Account account;
    if (oidc_users_managed()) {
        OidcIdentity identity;
        status = oidc_callback_identity(query, request->cookie, &identity, &lifetime);
        if (status == 200) {
            int found = accounts_resolve_oidc(&identity, &account);
            if (found != 1) status = found < 0 ? 503 : 403;
            else (void)organization_sync_groups();
        }
        erase(&identity, sizeof(identity));
    } else {
        status = oidc_callback(query, request->cookie, email, &lifetime);
        if (status == 200) {
            int found = accounts_lookup_email(email, &account);
            if (found != 1) status = found < 0 ? 503 : 403;
        }
    }
    if (status != 200) {
        serve_file_response(fd, "auth-error.html", "text/html; charset=utf-8",
                            false, status, oidc_clear_cookie());
        return;
    }
    Session *session = new_session(lifetime, account.id);
    if (!session) {
        serve_file_response(fd, "auth-error.html", "text/html; charset=utf-8",
                            false, 500, oidc_clear_cookie());
        return;
    }
    /* Rotate only after successful authentication, preserving an existing
       session if the identity provider or database is temporarily unavailable. */
    Session *old = find_session(request->cookie);
    if (old && old != session) revoke_session(old);
    char headers[1024];
    snprintf(headers, sizeof(headers),
        "Location: /app\r\n%sSet-Cookie: punto_session=%s; Path=/; HttpOnly; SameSite=Lax%s\r\n",
        oidc_clear_cookie(), session->token, secure_request(request) ? "; Secure" : "");
    respond(fd, 303, "text/plain; charset=utf-8", "", 0, headers, false);
}

static void serve_file_response(int fd, const char *name, const char *mime,
                               bool head, int status, const char *extra) {
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
        respond(fd, status, mime, data, read_size, extra, head);
    else error_response_head(fd, 500, head);
    free(data);
}

static void serve_file(int fd, const char *name, const char *mime, bool head) {
    serve_file_response(fd, name, mime, head, 200, NULL);
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
        char origin[6 * sizeof(tickets_app_origin.origin)], body[sizeof(origin) + 128];
        escape_json(origin, tickets_app_origin.origin);
        snprintf(body, sizeof(body), "{\"authenticated\":false,\"tickets\":{\"configured\":%s,\"origin\":%s%s%s}}",
            tickets_configured ? "true" : "false", tickets_configured ? "\"" : "",
            tickets_configured ? origin : "null", tickets_configured ? "\"" : "");
        json(fd, 200, body, NULL); return;
    }
    char email[6 * sizeof(account.email)], name[6 * sizeof(account.name)];
    escape_json(email, account.email);
    escape_json(name, account.name);
    bool manager = strcmp(account.role, "manager") == 0;
    AccessPolicy policy;
    if (accounts_access_policy(account.id, &policy) != 1) { error_response(fd, 503); return; }
    const char *allowed = manager ? "true" : "false";
    char origin[6 * sizeof(tickets_app_origin.origin)];
    escape_json(origin, tickets_app_origin.origin);
    char body[sizeof(email) + sizeof(name) + sizeof(origin) + 1280];
    snprintf(body, sizeof(body),
        "{\"authenticated\":true,\"user_id\":%lld,\"email\":\"%s\",\"name\":\"%s\",\"role\":\"%s\","
        "\"permissions\":{\"manage_jobs\":%s,\"manage_users\":%s,\"edit_users\":%s,\"edit_time_entries\":%s,"
        "\"manage_assignments\":%s,\"view_audit\":%s,"
        "\"manage_suppliers\":%s,\"manage_contracts\":%s,"
        "\"manage_groups\":%s,\"manage_job_roles\":%s,"
        "\"view_contracts\":%s,\"view_suppliers\":%s,\"view_costs\":%s,"
        "\"view_labor_rates\":%s,\"manage_labor_rates\":%s,"
        "\"view_team_hours\":%s,\"manage_access_groups\":%s,\"manage_space\":%s,\"administrator\":%s%s},"
        "\"tickets\":{\"configured\":%s,\"origin\":%s%s%s}}",
        (long long)account.id, email, name, account.role, allowed,
        policy.admin && !oidc_users_managed() ? "true" : "false",
        manager && !oidc_users_managed() ? "true" : "false",
        allowed, allowed, allowed, policy.sales ? "true" : "false", policy.sales ? "true" : "false",
        allowed, allowed,
        policy.sales || policy.finance ? "true" : "false", policy.sales || policy.finance ? "true" : "false",
        policy.finance ? "true" : "false", policy.finance ? "true" : "false",
        manager && policy.finance ? "true" : "false", allowed, policy.admin ? "true" : "false", policy.admin ? "true" : "false", policy.admin ? "true" : "false",
        organization_groups_managed() ? ",\"edit_groups\":false" : "",
        tickets_configured ? "true" : "false", tickets_configured ? "\"" : "",
        tickets_configured ? origin : "null", tickets_configured ? "\"" : "");
    json(fd, 200, body, NULL);
}

static bool calendar_session_binding(const Session *session, char binding[65]) {
    unsigned char digest[32];
#ifdef __APPLE__
    bool okay = CC_SHA256(session->token, TOKEN_LENGTH, digest) != NULL;
#else
    bool okay = SHA256((const unsigned char *)session->token, TOKEN_LENGTH, digest) != NULL;
#endif
    if (!okay) return false;
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        binding[2 * i] = digits[digest[i] >> 4];
        binding[2 * i + 1] = digits[digest[i] & 15];
    }
    binding[64] = '\0';
    erase(digest, sizeof(digest));
    return true;
}

static bool ticket_token(const char *value) {
    if (strlen(value) != TOKEN_LENGTH) return false;
    for (const char *p = value; *p; p++)
        if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return false;
    return true;
}

static bool ticket_grant_has_session(const TicketGrant *grant, time_t now) {
    for (size_t i = 0; i < SESSION_LIMIT; i++) {
        if (sessions[i].expires <= now || sessions[i].user_id != grant->user_id) continue;
        char binding[TOKEN_LENGTH + 1];
        if (!calendar_session_binding(&sessions[i], binding)) return false;
        bool matches = equal_hash((const unsigned char *)grant->session_binding,
                                  (const unsigned char *)binding, TOKEN_LENGTH);
        erase(binding, sizeof(binding));
        if (matches) return true;
    }
    return false;
}

static TicketGrant *new_ticket_grant(const Session *session, const char *state, bool calendar_access) {
    time_t now = time(NULL);
    if (now == (time_t)-1 || session->expires <= now) return NULL;
    TicketGrant next = {0};
    if (!calendar_session_binding(session, next.session_binding)) return NULL;
    size_t slot = TICKET_GRANT_LIMIT, own_oldest = TICKET_GRANT_LIMIT, own_count = 0;
    for (size_t i = 0; i < TICKET_GRANT_LIMIT; i++) {
        TicketGrant *grant = &ticket_grants[i];
        if (grant->expires <= now || !ticket_grant_has_session(grant, now)) {
            erase(grant, sizeof(*grant));
            if (slot == TICKET_GRANT_LIMIT) slot = i;
        } else if (grant->user_id == session->user_id &&
                   equal_hash((const unsigned char *)grant->session_binding,
                              (const unsigned char *)next.session_binding, TOKEN_LENGTH)) {
            own_count++;
            if (own_oldest == TICKET_GRANT_LIMIT || grant->order < ticket_grants[own_oldest].order)
                own_oldest = i;
        }
    }
    /* Two grants per live session fit the global pool even at its maximum
       session capacity. A flood rotates only the caller's oldest grant. */
    if (own_count >= TICKET_GRANTS_PER_SESSION) slot = own_oldest;
    if (slot == TICKET_GRANT_LIMIT) { erase(&next, sizeof(next)); return NULL; }
    unsigned char random[TOKEN_BYTES];
    if (!random_bytes(random, sizeof(random))) {
        erase(&next, sizeof(next)); return NULL;
    }
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        next.token[2 * i] = digits[random[i] >> 4];
        next.token[2 * i + 1] = digits[random[i] & 15];
    }
    erase(random, sizeof(random));
    memcpy(next.state, state, TOKEN_LENGTH + 1);
    next.user_id = session->user_id;
    next.calendar_access = calendar_access;
    next.order = ++grant_order;
    next.expires = now + SHORT_SESSION;
    if (next.expires > session->expires) next.expires = session->expires;
    erase(&ticket_grants[slot], sizeof(ticket_grants[slot]));
    ticket_grants[slot] = next;
    erase(&next, sizeof(next));
    return &ticket_grants[slot];
}

/* Every use rechecks both the originating Punto session and live account.
   Logout, session replacement, expiry and restart therefore revoke grants. */
static TicketGrant *find_ticket_grant(const char *authorization, Account *account, int *status) {
    *status = 401;
    const char prefix[] = "Bearer ";
    if (strncmp(authorization, prefix, sizeof(prefix) - 1) || !ticket_token(authorization + sizeof(prefix) - 1))
        return NULL;
    time_t now = time(NULL);
    if (now == (time_t)-1) { *status = 503; return NULL; }
    for (size_t i = 0; i < TICKET_GRANT_LIMIT; i++) {
        TicketGrant *grant = &ticket_grants[i];
        if (grant->expires <= now || !equal_hash((const unsigned char *)grant->token,
            (const unsigned char *)(authorization + sizeof(prefix) - 1), TOKEN_LENGTH)) continue;
        bool bound = false;
        for (size_t j = 0; j < SESSION_LIMIT; j++) {
            if (sessions[j].expires <= now || sessions[j].user_id != grant->user_id) continue;
            char binding[TOKEN_LENGTH + 1];
            if (!calendar_session_binding(&sessions[j], binding)) { *status = 503; return NULL; }
            bound = equal_hash((const unsigned char *)grant->session_binding,
                               (const unsigned char *)binding, TOKEN_LENGTH);
            erase(binding, sizeof(binding));
            if (bound) break;
        }
        if (!bound) { erase(grant, sizeof(*grant)); return NULL; }
        int found = accounts_lookup(grant->user_id, account);
        if (found < 0) { *status = 503; return NULL; }
        if (!found) { erase(grant, sizeof(*grant)); return NULL; }
        return grant;
    }
    return NULL;
}

static bool ticket_grant_form(const char *body, char state[TOKEN_LENGTH + 1], bool *calendar_access) {
    char copy[256];
    if (!body || strlen(body) >= sizeof(copy)) return false;
    strcpy(copy, body);
    bool has_state = false, has_calendar = false;
    *calendar_access = false;
    char *part = copy;
    while (*part) {
        char *end = strchr(part, '&');
        if (end) *end = 0;
        char *equal = strchr(part, '=');
        if (!equal || equal == part) return false;
        *equal++ = 0;
        char name[32], value[TOKEN_LENGTH + 1];
        if (!decode_form(name, sizeof(name), part) || !decode_form(value, sizeof(value), equal)) return false;
        if (!strcmp(name, "state") && !has_state && ticket_token(value)) {
            memcpy(state, value, TOKEN_LENGTH + 1); has_state = true;
        } else if (!strcmp(name, "calendar") && !has_calendar && !strcmp(value, "1")) {
            *calendar_access = true; has_calendar = true;
        } else return false;
        if (!end) break;
        part = end + 1;
        if (!*part) return false;
    }
    return has_state;
}

static bool ticket_bridge_route(int fd, Request *request, const char *query) {
    const char prefix[] = "/api/integrations/tickets/";
    if (strncmp(request->target, prefix, sizeof(prefix) - 1)) return false;
    const char *resource = request->target + sizeof(prefix) - 1;
    bool grant_request = !strcmp(resource, "grant");
    bool calendar_request = !strncmp(resource, "calendar/", 9);
    const char *calendar_resource = calendar_request ? resource + 9 : "";
    bool calendar_write = calendar_request && (!strcmp(calendar_resource, "create") || !strcmp(calendar_resource, "update") ||
        !strcmp(calendar_resource, "delete") || !strcmp(calendar_resource, "events/create") ||
        !strcmp(calendar_resource, "events/update") || !strcmp(calendar_resource, "events/delete"));
    bool calendar_read = calendar_request && (!strcmp(calendar_resource, "status") || !strcmp(calendar_resource, "events") ||
        !strcmp(calendar_resource, "changes") || !strcmp(calendar_resource, "event") || !strcmp(calendar_resource, "change"));
    bool head = !strcmp(request->method, "HEAD");
    if (!grant_request && strcmp(resource, "scope") && strcmp(resource, "jobs") &&
        strcmp(resource, "assets") && strcmp(resource, "validate") && !calendar_write && !calendar_read) {
        error_response_head(fd, 404, head); return true;
    }
    if (strcmp(request->method, grant_request || calendar_write ? "POST" : "GET")) {
        error_response_head(fd, 405, head); return true;
    }
    if (!tickets_configured) { error_response(fd, 503); return true; }
    if (grant_request) {
        Session *session = find_session(request->cookie);
        Account account;
        int found = session ? accounts_lookup(session->user_id, &account) : 0;
        if (found <= 0) {
            if (found < 0) error_response(fd, 503);
            else json(fd, 401, "{\"ok\":false,\"error\":\"Accedi a Punto per collegare Ticket.\"}", NULL);
            return true;
        }
        if (!same_origin(request)) { error_response(fd, 403); return true; }
        const char type[] = "application/x-www-form-urlencoded";
        if (strncmp(request->content_type, type, sizeof(type) - 1) ||
            (request->content_type[sizeof(type) - 1] && request->content_type[sizeof(type) - 1] != ';')) {
            error_response(fd, 415); return true;
        }
        char state[TOKEN_LENGTH + 1];
        bool calendar_access;
        if (*query || !ticket_grant_form(request->body, state, &calendar_access)) {
            error_response(fd, 400); return true;
        }
        TicketGrant *grant = new_ticket_grant(session, state, calendar_access);
        if (!grant) { error_response(fd, 503); return true; }
        char destination[sizeof(tickets_app_origin.origin) + 2 * TOKEN_LENGTH + 64];
        snprintf(destination, sizeof(destination), "%s/#punto_grant=%s&state=%s",
            tickets_app_origin.origin, grant->token, grant->state);
        struct json_object *object = json_object_new_object();
        if (!object) { erase(grant, sizeof(*grant)); error_response(fd, 503); return true; }
        json_object_object_add(object, "url", json_object_new_string(destination));
        json(fd, 200, json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN), NULL);
        json_object_put(object);
        erase(destination, sizeof(destination));
        return true;
    }
    if (!calendar_write && request->content_length) { error_response(fd, 400); return true; }
    Account account;
    int status;
    TicketGrant *grant = find_ticket_grant(request->authorization, &account, &status);
    if (!grant) {
        if (status == 401) json(fd, 401, "{\"ok\":false,\"error\":\"Collegamento Punto scaduto o revocato. Ricollega l'account da Punto.\"}", NULL);
        else error_response(fd, status);
        return true;
    }
    WorkspaceResponse response = {0};
    if (calendar_request) {
        if (!grant->calendar_access) {
            json(fd, 403, "{\"ok\":false,\"error\":\"Questo collegamento non autorizza l'accesso al calendario. Ricollega Ticket con il consenso calendario.\"}", NULL);
            return true;
        }
        if (calendar_write) {
            const char type[] = "application/x-www-form-urlencoded";
            if (strncmp(request->content_type, type, sizeof(type) - 1) ||
                (request->content_type[sizeof(type) - 1] && request->content_type[sizeof(type) - 1] != ';')) {
                error_response(fd, 415); return true;
            }
            if (*query) { error_response(fd, 400); return true; }
        }
        ticket_bridge_calendar(calendar_resource, calendar_write ? request->body : query, &account, &response);
    } else ticket_bridge_read(resource, query, &account, grant->state, grant->expires, grant->calendar_access, &response);
    if (response.body) json(fd, response.status, response.body, NULL);
    else error_response(fd, 503);
    workspace_response_free(&response);
    return true;
}

static void calendar_callback_redirect(int fd, bool connected) {
    respond(fd, 303, "text/plain; charset=utf-8", "", 0,
        connected ? "Location: /app?calendar=connected\r\n" : "Location: /app?calendar=error\r\n", false);
}

static bool calendar_route(int fd, Request *request, const char *query) {
    if (strncmp(request->target, "/api/calendar/", 14) != 0) return false;
    static const struct { const char *path; CalendarOperation operation; bool get; } routes[] = {
        {"status", CALENDAR_STATUS, true},
        {"oauth/start", CALENDAR_OAUTH_START, false},
        {"oauth/callback", CALENDAR_OAUTH_CALLBACK, true},
        {"calendars", CALENDAR_CALENDARS, true},
        {"selection", CALENDAR_SELECTION, false},
        {"events", CALENDAR_EVENTS, true},
        {"events/create", CALENDAR_EVENT_CREATE, false},
        {"events/update", CALENDAR_EVENT_UPDATE, false},
        {"events/delete", CALENDAR_EVENT_DELETE, false},
        {"sync", CALENDAR_SYNC, false},
        {"disconnect", CALENDAR_DISCONNECT, false},
        {"changes", CALENDAR_CHANGES, true},
        {"event", CALENDAR_EVENT, true}, {"change", CALENDAR_CHANGE, true}
    };
    size_t index = 0;
    while (index < sizeof(routes) / sizeof(routes[0]) &&
           strcmp(request->target + 14, routes[index].path)) index++;
    bool head = strcmp(request->method, "HEAD") == 0;
    if (index == sizeof(routes) / sizeof(routes[0])) {
        error_response_head(fd, 404, head); return true;
    }
    bool callback = routes[index].operation == CALENDAR_OAUTH_CALLBACK;
    bool get = strcmp(request->method, "GET") == 0;
    bool post = strcmp(request->method, "POST") == 0;
    if ((routes[index].get && !get) || (!routes[index].get && !post)) {
        error_response_head(fd, 405, head); return true;
    }
    Session *session = find_session(request->cookie);
    Account account;
    int found = session ? accounts_lookup(session->user_id, &account) : 0;
    if (found <= 0) {
        if (callback) calendar_callback_redirect(fd, false);
        else if (found < 0) error_response(fd, 503);
        else json(fd, 401, "{\"ok\":false,\"error\":\"Accedi per continuare.\"}", NULL);
        return true;
    }
    if (post && !same_origin(request)) { error_response(fd, 403); return true; }
    if (post) {
        const char type[] = "application/x-www-form-urlencoded";
        if (strncmp(request->content_type, type, sizeof(type) - 1) ||
            (request->content_type[sizeof(type) - 1] && request->content_type[sizeof(type) - 1] != ';')) {
            error_response(fd, 415); return true;
        }
    }
    if ((get && request->content_length) ||
        (*query && !callback && routes[index].operation != CALENDAR_EVENTS &&
         routes[index].operation != CALENDAR_CALENDARS && routes[index].operation != CALENDAR_EVENT &&
         routes[index].operation != CALENDAR_CHANGE)) {
        if (callback) calendar_callback_redirect(fd, false);
        else error_response(fd, 400);
        return true;
    }
    char binding[65] = {0};
    if ((callback || routes[index].operation == CALENDAR_OAUTH_START) &&
        !calendar_session_binding(session, binding)) {
        if (callback) calendar_callback_redirect(fd, false);
        else error_response(fd, 500);
        return true;
    }
    CalendarResponse response = {0};
    calendar_client_call(routes[index].operation, account.id,
        get ? query : request->body, binding, &response);
    erase(binding, sizeof(binding));
    if (callback) calendar_callback_redirect(fd, response.status == 200);
    else if (!response.body) error_response(fd, 500);
    else json(fd, response.status, response.body, NULL);
    calendar_response_free(&response);
    return true;
}

static bool inventory_identifier(const char *number, const char **end, DbInt *value) {
    if (*number < '0' || *number > '9') return false;
    *end = number;
    while (**end >= '0' && **end <= '9') (*end)++;
    char *parsed;
    errno = 0;
    long long identifier = strtoll(number, &parsed, 10);
    if (errno || identifier <= 0 || parsed != *end) return false;
    *value = (DbInt)identifier;
    return true;
}

static int inventory_user_lookup(DbInt user_id, char *name, size_t capacity) {
    Account account;
    int found=accounts_lookup(user_id,&account);
    if (found!=1) return found;
    size_t length=strlen(account.name);
    if (length>=capacity) return -1;
    memcpy(name,account.name,length+1);
    return 1;
}

static void inventory_users(int fd, const Account *account) {
    WorkspaceResponse response={0};
    if (!accounts_handle("GET","/api/users","","",account,&response) || !response.body) {
        workspace_response_free(&response); error_response(fd,503); return;
    }
    if (response.status!=200) {
        json(fd,response.status,response.body,NULL); workspace_response_free(&response); return;
    }
    struct json_object *source=json_tokener_parse(response.body), *users=NULL;
    struct json_object *result=json_object_new_object(), *active=json_object_new_array();
    bool valid=source && result && active && json_object_object_get_ex(source,"users",&users) &&
        json_object_is_type(users,json_type_array);
    if (valid) for (size_t i=0;i<json_object_array_length(users);i++) {
        struct json_object *user=json_object_array_get_idx(users,i), *is_active=NULL;
        if (!json_object_object_get_ex(user,"active",&is_active) || !json_object_get_boolean(is_active)) continue;
        struct json_object *entry=json_object_new_object();
        if (!entry) { valid=false; break; }
        const char *names[]={"id","name","email"};
        for (size_t j=0;j<3;j++) {
            struct json_object *value=NULL;
            if (!json_object_object_get_ex(user,names[j],&value)) { valid=false; break; }
            json_object_object_add(entry,names[j],json_object_get(value));
        }
        json_object_array_add(active,entry);
        if (!valid) break;
    }
    if (valid) {
        json_object_object_add(result,"users",active); active=NULL;
        json(fd,200,json_object_to_json_string_ext(result,JSON_C_TO_STRING_PLAIN),NULL);
    } else error_response(fd,503);
    if (source) json_object_put(source);
    if (result) json_object_put(result);
    if (active) json_object_put(active);
    workspace_response_free(&response);
}

static bool inventory_route(int fd, Request *request, const char *query) {
    if (strncmp(request->target, "/api/inventory/", 15)) return false;
    bool get = !strcmp(request->method, "GET");
    bool post = !strcmp(request->method, "POST");
    bool head = !strcmp(request->method, "HEAD");
    const char *resource = request->target + 15;
    InventoryOperation operation;
    bool user_directory=!strcmp(resource,"users");
    DbInt item_id = 0, relation_id = 0;
    if (user_directory) {
        if (!get) { error_response_head(fd,405,head); return true; }
        operation=INVENTORY_STATUS;
    } else if (!strcmp(resource,"people") || !strcmp(resource,"identities")) {
        if (!get && !post) { error_response_head(fd,405,head); return true; }
        bool people=!strcmp(resource,"people");
        operation=people ? (post ? INVENTORY_PERSON_CREATE : INVENTORY_PEOPLE) :
            (post ? INVENTORY_IDENTITY_CREATE : INVENTORY_IDENTITIES);
    } else if (!strncmp(resource,"people/",7) || !strncmp(resource,"identities/",11)) {
        bool people=!strncmp(resource,"people/",7);
        const char *end;
        if (!inventory_identifier(resource+(people ? 7 : 11),&end,&item_id)) {
            error_response_head(fd,404,head); return true;
        }
        bool read_only=false, write_only=false;
        if (!*end) operation=people ? (post ? INVENTORY_PERSON_UPDATE : INVENTORY_PERSON_DETAIL) :
            (post ? INVENTORY_IDENTITY_UPDATE : INVENTORY_IDENTITY_DETAIL);
        else if (!strcmp(end,"/history")) {
            operation=people ? INVENTORY_PERSON_HISTORY : INVENTORY_IDENTITY_HISTORY; read_only=true;
        } else if (!strcmp(end,"/assets")) {
            operation=people ? INVENTORY_PERSON_ASSETS : INVENTORY_IDENTITY_ASSETS; read_only=true;
        } else if (people && !strcmp(end,"/identities")) {
            operation=INVENTORY_PERSON_IDENTITIES; read_only=true;
        } else if (!people && !strcmp(end,"/people"))
            operation=post ? INVENTORY_IDENTITY_PERSON_CREATE : INVENTORY_IDENTITY_PEOPLE;
        else if (!people && !strncmp(end,"/people/",8)) {
            const char *suffix;
            if (!inventory_identifier(end+8,&suffix,&relation_id) || strcmp(suffix,"/remove")) {
                error_response_head(fd,404,head); return true;
            }
            operation=INVENTORY_IDENTITY_PERSON_REMOVE; write_only=true;
        } else { error_response_head(fd,404,head); return true; }
        if ((!get && !post) || (read_only && !get) || (write_only && !post)) {
            error_response_head(fd,405,head); return true;
        }
    } else if (!strcmp(resource, "places")) {
        if (!get && !post) { error_response_head(fd,405,head); return true; }
        operation=post ? INVENTORY_PLACE_CREATE : INVENTORY_PLACES;
    } else if (!strncmp(resource,"places/",7)) {
        const char *end;
        if (!inventory_identifier(resource+7,&end,&item_id) || *end) {
            error_response_head(fd,404,head); return true;
        }
        if (!post) { error_response_head(fd,405,head); return true; }
        operation=INVENTORY_PLACE_UPDATE;
    } else if (!strcmp(resource, "status")) {
        if (!get) { error_response_head(fd, 405, head); return true; }
        operation = INVENTORY_STATUS;
    } else if (!strcmp(resource, "assets")) {
        if (!get && !post) { error_response_head(fd, 405, head); return true; }
        operation = post ? INVENTORY_CREATE : INVENTORY_ASSETS;
    } else if (!strcmp(resource,"assets/export") || !strcmp(resource,"due") ||
               !strcmp(resource,"dashboard") || !strcmp(resource,"operational-due")) {
        if (!get) { error_response_head(fd,405,head); return true; }
        operation = !strcmp(resource,"due") ? INVENTORY_DUE : !strcmp(resource,"dashboard") ? INVENTORY_DASHBOARD :
            !strcmp(resource,"operational-due") ? INVENTORY_OPERATIONAL_DUE : INVENTORY_EXPORT;
    } else if (!strcmp(resource,"import/preview") || !strcmp(resource,"import/commit")) {
        if (!post) { error_response_head(fd,405,head); return true; }
        operation = !strcmp(resource,"import/preview") ? INVENTORY_IMPORT_PREVIEW : INVENTORY_IMPORT_COMMIT;
    } else if (!strncmp(resource, "assets/", 7)) {
        const char *end;
        if (!inventory_identifier(resource + 7, &end, &item_id)) {
            error_response_head(fd, 404, head); return true;
        }
        bool read_only = false, write_only = false;
        if (!*end) operation = post ? INVENTORY_UPDATE : INVENTORY_DETAIL;
        else if (!strcmp(end, "/history")) { operation = INVENTORY_HISTORY; read_only = true; }
        else if (!strcmp(end,"/context")) { operation=INVENTORY_CONTEXT; write_only=true; }
        else if (!strcmp(end,"/checkout")) { operation=INVENTORY_CHECKOUT; write_only=true; }
        else if (!strcmp(end,"/checkin")) { operation=INVENTORY_CHECKIN; write_only=true; }
        else if (!strcmp(end,"/verify")) { operation=INVENTORY_VERIFY; write_only=true; }
        else if (!strcmp(end,"/custody")) { operation=INVENTORY_CUSTODY; read_only=true; }
        else if (!strcmp(end,"/verifications")) { operation=INVENTORY_VERIFICATIONS; read_only=true; }
        else if (!strcmp(end,"/subjects")) operation=post ? INVENTORY_SUBJECT_CREATE : INVENTORY_SUBJECTS;
        else if (!strncmp(end,"/subjects/",10)) {
            const char *suffix;
            if (!inventory_identifier(end+10,&suffix,&relation_id) || strcmp(suffix,"/remove")) {
                error_response_head(fd,404,head); return true;
            }
            operation=INVENTORY_SUBJECT_REMOVE; write_only=true;
        }
        else if (!strcmp(end, "/relations")) operation = post ? INVENTORY_RELATION_CREATE : INVENTORY_RELATIONS;
        else if (!strncmp(end, "/relations/", 11)) {
            const char *suffix;
            if (!inventory_identifier(end + 11, &suffix, &relation_id) || strcmp(suffix, "/remove")) {
                error_response_head(fd, 404, head); return true;
            }
            operation = INVENTORY_RELATION_REMOVE;
            write_only = true;
        } else { error_response_head(fd, 404, head); return true; }
        if ((!get && !post) || (read_only && !get) || (write_only && !post)) {
            error_response_head(fd, 405, head); return true;
        }
    } else { error_response_head(fd, 404, head); return true; }
    Session *session = find_session(request->cookie);
    Account account;
    int found = session ? accounts_lookup(session->user_id, &account) : 0;
    if (found <= 0) {
        if (found < 0) error_response(fd, 503);
        else json(fd, 401, "{\"ok\":false,\"error\":\"Accedi per continuare.\"}", NULL);
        return true;
    }
    AccessPolicy inventory_policy;
    if (accounts_access_policy(account.id, &inventory_policy) != 1) { error_response(fd,503); return true; }
    if ((user_directory && !inventory_policy.admin) ||
        (post && (!inventory_policy.admin || !same_origin(request)))) {
        error_response(fd, 403); return true;
    }
    if (post) {
        const char type[] = "application/x-www-form-urlencoded";
        if (strncmp(request->content_type, type, sizeof(type) - 1) ||
            (request->content_type[sizeof(type) - 1] && request->content_type[sizeof(type) - 1] != ';')) {
            error_response(fd, 415); return true;
        }
    }
    if ((get && request->content_length) || (post && *query)) {
        error_response(fd, 400); return true;
    }
    if (user_directory) {
        if (*query) error_response(fd,400);
        else inventory_users(fd,&account);
        return true;
    }
    InventoryResponse response = {0};
    inventory_client_call_with_lookup(operation, item_id, relation_id, account.id,
        get ? query : request->body, inventory_user_lookup, &response);
    if (!response.body) error_response(fd, 500);
    else json(fd, response.status, response.body, NULL);
    inventory_response_free(&response);
    return true;
}

static bool workspace_route(int fd, Request *request, const char *query) {
    const char *target = request->target;
    bool workflows = !strncmp(target,"/api/improvements/",18) || !strncmp(target,"/api/time-corrections",21) || !strcmp(target,"/api/my-work") ||
        !strcmp(target,"/api/job-templates") || !strncmp(target,"/api/work-schedules",19) ||
        !strncmp(target,"/api/work-exceptions",20) || !strncmp(target,"/api/business-holidays",22) ||
        !strncmp(target,"/api/work-notifications",23) || !strcmp(target,"/api/access-groups") ||
        !strcmp(target,"/api/time-team");
    if (!workflows && strcmp(target, "/api/jobs") != 0 && strncmp(target, "/api/jobs/", 10) != 0 &&
        strcmp(target, "/api/time-entries") != 0 && strncmp(target, "/api/time-entries/", 18) != 0 &&
        strcmp(target, "/api/users") != 0 && strncmp(target, "/api/users/", 11) != 0 &&
        strcmp(target, "/api/suppliers") != 0 && strncmp(target, "/api/suppliers/", 15) != 0 &&
        strcmp(target, "/api/contracts") != 0 && strncmp(target, "/api/contracts/", 15) != 0 &&
        strcmp(target, "/api/groups") != 0 && strncmp(target, "/api/groups/", 12) != 0 &&
        strcmp(target, "/api/job-roles") != 0 && strncmp(target, "/api/job-roles/", 15) != 0 &&
        strncmp(target, "/api/contract-documents/", 24) != 0 &&
        strcmp(target, "/api/labor-rates") != 0 && strncmp(target, "/api/labor-rates/", 17) != 0 &&
        strcmp(target, "/api/reports") != 0 && strcmp(target, "/api/work-board") != 0 &&
        strcmp(target, "/api/tasks") != 0 && strncmp(target, "/api/tasks/", 11) != 0 &&
        strcmp(target, "/api/blockers") != 0 && strncmp(target, "/api/blockers/", 14) != 0 &&
        strcmp(target, "/api/alert-rules") != 0 && strncmp(target, "/api/alert-rules/", 17) != 0 &&
        strcmp(target, "/api/notifications") != 0 && strncmp(target, "/api/notifications/", 19) != 0 &&
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
    bool upload = strcmp(request->method, "POST") == 0 && pdf_upload_target(target);
    if (strcmp(request->method, "POST") == 0) {
        const char *type = upload ? "application/pdf" : "application/x-www-form-urlencoded";
        size_t length = strlen(type);
        if (strncmp(request->content_type, type, length) != 0 ||
            (request->content_type[length] && request->content_type[length] != ';')) {
            error_response(fd, 415); return true;
        }
    }
    WorkspaceResponse response = {0};
    bool handled = false;
    if (upload) {
        const char *number = target + strlen("/api/contracts/");
        errno = 0;
        char *end;
        long long id = strtoll(number, &end, 10);
        char filename[181] = {0};
        if (errno || id <= 0 || strcmp(end, "/documents") != 0 ||
            strncmp(query, "filename=", 9) != 0 || strchr(query, '&') ||
            !decode_form(filename, sizeof(filename), query + 9)) {
            error_response(fd, 400); return true;
        }
        procurement_upload((DbInt)id, filename, (const unsigned char *)request->body,
                           request->content_length, account.id, account.role, &response);
        handled = true;
    } else if (strncmp(target, "/api/contract-documents/", 24) == 0) {
        const char *number = target + 24;
        errno = 0;
        char *end;
        long long id = strtoll(number, &end, 10);
        if (errno || !isdigit((unsigned char)*number) || id <= 0 || strcmp(end, "/download") != 0 || *query) {
            error_response_head(fd, 404, head); return true;
        }
        if (strcmp(request->method, "GET") != 0 && !head) {
            error_response(fd, 405); return true;
        }
        unsigned char *data = NULL;
        size_t size = 0;
        char filename[181] = {0};
        int status = procurement_download((DbInt)id, account.id, account.role, &data, &size, filename);
        if (status != 200) error_response_head(fd, status, head);
        else {
            char extra[1024];
            size_t used = (size_t)snprintf(extra, sizeof(extra),
                "Content-Disposition: attachment; filename=\"contratto.pdf\"; filename*=UTF-8''");
            const char digits[] = "0123456789ABCDEF";
            for (const unsigned char *p = (const unsigned char *)filename; *p && used + 5 < sizeof(extra); p++) {
                extra[used++] = '%'; extra[used++] = digits[*p >> 4]; extra[used++] = digits[*p & 15];
            }
            memcpy(extra + used, "\r\n", 3);
            respond(fd, 200, "application/pdf", (const char *)data, size, extra, head);
        }
        if (data) { erase(data, size); free(data); }
        return true;
    }
    if (!handled) handled = improvements_handle(request->method, target, query, request->body, account.id, &response);
    if (!handled) handled = organization_handle(request->method, target, query, request->body,
                                                account.id, account.role, &response);
    if (!handled) handled = procurement_handle(request->method, target, query, request->body,
                                              account.id, account.role, &response);
    if (!handled) handled = accounts_handle(request->method, target, query, request->body,
                                   &account, &response);
    if (handled && response.status == 200 && strcmp(request->method, "POST") == 0 &&
        strncmp(target, "/api/users/", 11) == 0) {
        /* Successful item updates already validated the numeric identifier. */
        DbInt updated_id = (DbInt)strtoll(target + 11, NULL, 10);
        Account updated;
        if (!accounts_find(updated_id, &updated)) {
            for (size_t i = 0; i < SESSION_LIMIT; i++) {
                if (sessions[i].user_id == updated_id) revoke_session(&sessions[i]);
            }
        }
    }
    if (!handled) handled = storage_handle(request->method, target, query, account.id, account.role, &response);
    if (!handled) handled = alerts_handle(request->method, target, query, request->body,
                                          account.id, account.role, &response);
    if (!handled) handled = work_board_handle(request->method, target, query, request->body,
                                              account.id, account.role, &response);
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
    (void)organization_sync_groups();
    if (calendar_route(fd, request, query ? query : "")) return;
    if (inventory_route(fd, request, query ? query : "")) return;
    if (ticket_bridge_route(fd, request, query ? query : "")) return;
    if (workspace_route(fd, request, query ? query : "")) return;
    const char *target = request->target;
    bool get = strcmp(request->method, "GET") == 0;
    bool head = strcmp(request->method, "HEAD") == 0;
    bool post = strcmp(request->method, "POST") == 0;
    if (strcmp(target, "/api/auth/config") == 0) {
        if (!get) { error_response_head(fd, 405, head); return; }
        if (oidc_users_managed()) {
            const char *url = oidc_users_admin_url();
            char groups_url[1152] = "";
            if (*url) snprintf(groups_url, sizeof(groups_url), "%.*s#/punto/groups",
                               (int)strcspn(url, "#"), url);
            size_t capacity = 6 * (strlen(url) + strlen(groups_url)) + 320;
            char *escaped = malloc(capacity), *groups_escaped = malloc(capacity), *body = malloc(capacity);
            if (!escaped || !groups_escaped || !body) {
                free(escaped); free(groups_escaped); free(body); error_response(fd, 500); return;
            }
            escape_json(escaped, url);
            escape_json(groups_escaped, groups_url);
            if (organization_groups_managed()) {
                snprintf(body, capacity,
                    "{\"sso\":%s,\"local\":%s,\"managed_users\":true,\"users_admin_url\":\"%s\","
                    "\"managed_groups\":true,\"groups_admin_url\":\"%s\"}",
                    oidc_enabled() ? "true" : "false", oidc_local_enabled() ? "true" : "false", escaped, groups_escaped);
            } else {
                snprintf(body, capacity,
                    "{\"sso\":%s,\"local\":%s,\"managed_users\":true,\"users_admin_url\":\"%s\"}",
                    oidc_enabled() ? "true" : "false", oidc_local_enabled() ? "true" : "false", escaped);
            }
            json(fd, 200, body, NULL);
            free(escaped); free(groups_escaped); free(body); return;
        } else {
            char body[64];
            snprintf(body, sizeof(body), "{\"sso\":%s,\"local\":%s}",
                     oidc_enabled() ? "true" : "false", oidc_local_enabled() ? "true" : "false");
            json(fd, 200, body, NULL); return;
        }
    }
    if (strcmp(target, "/auth/error") == 0) {
        if (!get && !head) { error_response_head(fd, 405, head); return; }
        if (!oidc_enabled()) { error_response_head(fd, 404, head); return; }
        if (query && *query && strcmp(query, "lang=it") && strcmp(query, "lang=en") &&
            strcmp(query, "lang=zh") && strcmp(query, "lang=es") &&
            strcmp(query, "lang=fr") && strcmp(query, "lang=de")) {
            error_response_head(fd, 400, head); return;
        }
        serve_file(fd, "auth-error.html", "text/html; charset=utf-8", head); return;
    }
    if (strcmp(target, "/auth/logout") == 0) {
        if (!post) { error_response_head(fd, 405, head); return; }
        if (!oidc_enabled()) { error_response(fd, 404); return; }
        if (!request->origin[0] || !same_origin(request)) { error_response(fd, 403); return; }
        if ((query && *query) || *request->body) { error_response(fd, 400); return; }
        char location[8192];
        int status = oidc_logout(location);
        if (status != 200) { error_response(fd, status); return; }
        size_t capacity = 6 * strlen(location) + 64;
        char *escaped = malloc(capacity), *body = malloc(capacity);
        if (!escaped || !body) { free(escaped); free(body); error_response(fd, 500); return; }
        escape_json(escaped, location);
        snprintf(body, capacity, "{\"ok\":true,\"logout_url\":\"%s\"}", escaped);
        Session *session = find_session(request->cookie);
        if (session) revoke_session(session);
        char headers[1024];
        snprintf(headers, sizeof(headers), "%sSet-Cookie: punto_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s\r\n",
                 oidc_clear_cookie(), secure_request(request) ? "; Secure" : "");
        json(fd, 200, body, headers);
        free(escaped); free(body); return;
    }
    if (strcmp(target, "/auth/login") == 0 || strcmp(target, "/auth/callback") == 0) {
        if (!get) { error_response_head(fd, 405, head); return; }
        if (!oidc_enabled()) { error_response(fd, 404); return; }
        if (strcmp(target, "/auth/login") == 0) sso_login(fd, request);
        else sso_callback(fd, request, query ? query : "");
        return;
    }
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
        if (session) revoke_session(session);
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
        {"/auth-error.js", "auth-error.js", "text/javascript; charset=utf-8"},
        {"/auth-error.css", "auth-error.css", "text/css; charset=utf-8"},
        {"/i18n.js", "i18n.js", "text/javascript; charset=utf-8"},
        {"/i18n.css", "i18n.css", "text/css; charset=utf-8"},
        {"/locales/en.json", "locales/en.json", "application/json; charset=utf-8"},
        {"/locales/zh.json", "locales/zh.json", "application/json; charset=utf-8"},
        {"/locales/es.json", "locales/es.json", "application/json; charset=utf-8"},
        {"/locales/fr.json", "locales/fr.json", "application/json; charset=utf-8"},
        {"/locales/de.json", "locales/de.json", "application/json; charset=utf-8"},
        {"/app.js", "app.js", "text/javascript; charset=utf-8"},
        {"/improvements-governance.js", "improvements-governance.js", "text/javascript; charset=utf-8"},
        {"/improvements-core.js", "improvements-core.js", "text/javascript; charset=utf-8"},
        {"/improvements-operations.js", "improvements-operations.js", "text/javascript; charset=utf-8"},
        {"/improvements-integrations.js", "improvements-integrations.js", "text/javascript; charset=utf-8"},
        {"/workspace.css", "workspace.css", "text/css; charset=utf-8"},
        {"/reports.css", "reports.css", "text/css; charset=utf-8"},
        {"/work-board.css", "work-board.css", "text/css; charset=utf-8"},
        {"/time-workflows.css", "time-workflows.css", "text/css; charset=utf-8"},
        {"/improvements-governance.css", "improvements-governance.css", "text/css; charset=utf-8"},
        {"/improvements-operations.css", "improvements-operations.css", "text/css; charset=utf-8"},
        {"/improvements-integrations.css", "improvements-integrations.css", "text/css; charset=utf-8"},
        {"/inventory.css", "inventory.css", "text/css; charset=utf-8"},
        {"/qrcodegen.js", "qrcodegen.js", "text/javascript; charset=utf-8"},
        {"/inventory-qr.js", "inventory-qr.js", "text/javascript; charset=utf-8"},
        {"/favicon.svg", "favicon.svg", "image/svg+xml"}
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (strcmp(target, files[i].url) == 0) {
            serve_file(fd, files[i].file, files[i].mime, head); return;
        }
    }
    error_response_head(fd, 404, head);
}

static void handle_http_request(int fd, char *buffer, size_t size, void *context) {
    (void)context;
    Request request = {0};
    int status = read_request(buffer, size, &request);
    if (status) error_response(fd, status);
    else route(fd, &request);
}

static void scheduler_tick(void *context) {
    time_t *last_tick = context;
    (void)organization_sync_groups();
    time_t now = time(NULL);
    if (now != (time_t)-1 && (*last_tick == 0 || difftime(now, *last_tick) >= 30.0 || now < *last_tick)) {
        alerts_tick_at(now);
        *last_tick = now;
    }
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
        /* The proxy may publish standard HTTP/HTTPS ports while this process
           keeps listening on an unprivileged internal PORT. */
        if (errno || !*public_port_env || *port_end || public_port < 1 || public_port > 65535) {
            fprintf(stderr, "PUBLIC_PORT deve essere un numero tra 1 e 65535.\n"); return 1;
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
    if (!configure_tickets(getenv("TICKETS_APP_ORIGIN"))) {
        fprintf(stderr, "TICKETS_APP_ORIGIN deve essere un'origine HTTPS, oppure HTTP su localhost/127.0.0.1, senza percorso o credenziali.\n");
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
    if (!oidc_init()) {
        fprintf(stderr, "Configurazione autenticazione OIDC non valida. Controlla AUTH_MODE, URL, segreti e collegamenti utenti.\n");
        return 1;
    }
    if (atexit(oidc_close) != 0) { oidc_close(); return 1; }
    snprintf(authority, sizeof(authority), "127.0.0.1:%ld", port);
    snprintf(localhost_authority, sizeof(localhost_authority), "localhost:%ld", port);
    snprintf(public_authority, sizeof(public_authority), "127.0.0.1:%ld", public_port);
    snprintf(public_localhost, sizeof(public_localhost), "localhost:%ld", public_port);
    if (!configure_sso_origin()) {
        fprintf(stderr, "OIDC_REDIRECT_URI deve usare un'origine Punto configurata e il percorso /auth/callback.\n");
        return 1;
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
    if (!calendar_client_open()) {
        fprintf(stderr, "Configurazione del servizio calendario non valida. Controlla URL, token e certificato CA.\n");
        return 1;
    }
    if (atexit(calendar_client_close) != 0) {
        calendar_client_close(); return 1;
    }
    if (!inventory_client_open()) {
        fprintf(stderr, "Configurazione Asset inventory non valida. Controlla URL, token e certificato CA.\n");
        return 1;
    }
    if (atexit(inventory_client_close) != 0) {
        inventory_client_close(); return 1;
    }
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
    if (!improvements_open(database_path) || atexit(improvements_close) != 0) {
        fprintf(stderr, "Impossibile inizializzare i flussi operativi.\n");
        improvements_close(); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    if (!organization_open(database_path)) {
        fprintf(stderr, "Impossibile configurare gruppi, ruoli e abilitazioni. Controlla database e permessi.\n");
        organization_close(); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    if (!work_board_open(database_path) || !alerts_open(database_path) || !procurement_open(database_path) || !documents_open(database_path)) {
        fprintf(stderr, "Impossibile configurare attività, alert, contratti e archivio documentale. Controlla database e impostazioni dei servizi.\n");
        work_board_close(); alerts_close(); documents_close(); procurement_close(); organization_close(); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    if (tickets_configured && !ticket_bridge_open(database_path)) {
        fprintf(stderr, "Impossibile inizializzare l'integrazione Ticket. Controlla database e permessi.\n");
        work_board_close(); alerts_close(); documents_close(); procurement_close(); organization_close(); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    if (atexit(ticket_bridge_close) != 0) { ticket_bridge_close(); return 1; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction stop_action = {0};
    stop_action.sa_handler = stop_server;
    sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); work_board_close(); alerts_close(); documents_close(); procurement_close(); organization_close(); storage_close(); accounts_close(); workspace_close(); return 1; }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr = bind_address;
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 16) < 0) {
        perror("Avvio server"); close(server); work_board_close(); alerts_close(); documents_close(); procurement_close(); organization_close(); storage_close(); accounts_close(); workspace_close(); return 1;
    }
    printf("\n  punto · Project management\n  http://127.0.0.1:%ld\n  Utenti e dati: %s\n\n", public_port, database_path);
    fflush(stdout);
    time_t last_alert_tick=0;
    bool transport_ok = http_transport_run(server, &stopping, HEADER_LIMIT, DOCUMENTS_MAX_BYTES,
                                           5, check_http_headers, handle_http_request,
                                           scheduler_tick, &last_alert_tick);
    close(server);
    work_board_close();
    alerts_close();
    calendar_client_close();
    inventory_client_close();
    documents_close();
    procurement_close();
    organization_close();
    storage_close();
    accounts_close();
    workspace_close();
    erase(sessions, sizeof(sessions));
    return stopping && transport_ok ? 0 : 1;
}
