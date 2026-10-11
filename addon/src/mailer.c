/* A small, private HTTPS-to-SMTP bridge. The ledger stores hashes, never mail bodies. */
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <sqlite3.h>
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

#define HEADER_LIMIT 8192
#define BODY_LIMIT 32768
#define TOKEN_LIMIT 4096
#define MAIL_BODY_LIMIT 8192

typedef struct {
    char token[TOKEN_LIMIT + 1];
    const char *bind_address, *allowed_client, *cert, *key, *ledger;
    const char *host, *smtp_mode, *from, *user, *ca, *proxy;
    char password[TOKEN_LIMIT + 1];
    int port, smtp_port, timeout;
    bool configured;
} Config;

typedef struct {
    char method[12], path[128], authorization[TOKEN_LIMIT + 16], content_type[128];
    char body[BODY_LIMIT + 1];
    size_t length;
} Request;

typedef struct {
    char key[129], to[255], subject[251], body[MAIL_BODY_LIMIT + 1];
    char message_id[256], in_reply_to[256], references[1025];
} Email;

static Config config;
static sqlite3 *ledger;
static volatile sig_atomic_t stopping;

static const char *env_value(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static bool number(const char *text, unsigned long min, unsigned long max, int *result) {
    if (!text || !*text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!isdigit(*p)) return false;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value < min || value > max) return false;
    *result = (int)value;
    return true;
}

static bool read_secret(const char *path, char *buffer, size_t capacity) {
    if (!path || !*path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = fread(buffer, 1, capacity - 1, file);
    bool valid = !ferror(file) && feof(file);
    fclose(file);
    if (!valid || memchr(buffer, 0, count)) return false;
    while (count && (buffer[count - 1] == '\r' || buffer[count - 1] == '\n')) count--;
    buffer[count] = 0;
    return count > 0;
}

static bool valid_email(const char *address) {
    size_t length = strlen(address);
    if (!length || length > 254) return false;
    const char *at = strchr(address, '@');
    if (!at || at == address || at - address > 64 || !at[1] || strchr(at + 1, '@')) return false;
    if (address[0] == '.' || at[-1] == '.' || strstr(address, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)address; p < (const unsigned char *)at; p++)
        if (!(isalnum(*p) || strchr(".!#$%&'*+-/=?^_`{|}~", *p))) return false;
    const char *label = at + 1;
    for (const unsigned char *p = (const unsigned char *)label; ; p++) {
        if (*p == '.' || !*p) {
            size_t label_length = (const char *)p - label;
            if (!label_length || label_length > 63 || label[0] == '-' || p[-1] == '-') return false;
            if (!*p) break;
            label = (const char *)p + 1;
        } else if (!(isalnum(*p) || *p == '-')) return false;
    }
    return true;
}

static bool valid_host(const char *host) {
    if (!*host || strlen(host) > 253) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++)
        if (!(isalnum(*p) || *p == '.' || *p == '-')) return false;
    return true;
}

static bool load_config(void) {
    memset(&config, 0, sizeof(config));
    config.bind_address = env_value("MAIL_BIND", "0.0.0.0");
    config.allowed_client = env_value("MAIL_ALLOWED_CLIENT", "172.30.60.10");
    config.cert = env_value("MAIL_TLS_CERT_FILE", "/etc/punto-mailer/tls/server.crt");
    config.key = env_value("MAIL_TLS_KEY_FILE", "/etc/punto-mailer/tls/server.key");
    config.ledger = env_value("MAIL_LEDGER_PATH", "/var/lib/punto-mailer/delivery.sqlite3");
    config.host = env_value("MAIL_SMTP_HOST", "");
    config.smtp_mode = env_value("MAIL_SMTP_MODE", "starttls");
    config.from = env_value("MAIL_SMTP_FROM", "");
    config.user = env_value("MAIL_SMTP_USER", "");
    config.ca = env_value("MAIL_SMTP_CA_FILE", "");
    config.proxy = env_value("MAIL_PROXY_URL", "http://squid:3128");
    if (!number(env_value("MAIL_PORT", "8090"), 1, 65535, &config.port) ||
        !number(env_value("MAIL_SMTP_PORT", "587"), 1, 65535, &config.smtp_port) ||
        !number(env_value("MAIL_SMTP_TIMEOUT", "20"), 1, 60, &config.timeout) ||
        !read_secret(env_value("MAIL_API_TOKEN_FILE", ""), config.token, sizeof(config.token)) ||
        strlen(config.token) < 32) return false;
    for (const unsigned char *p = (const unsigned char *)config.token; *p; p++)
        if (*p <= 32 || *p > 126) return false;
    if (strcmp(config.smtp_mode, "starttls") && strcmp(config.smtp_mode, "smtps")) return false;
    config.configured = *config.host && *config.from;
    if ((*config.host || *config.from || *config.user) && !config.configured)
        return false; /* Partial configuration fails closed. */
    if (config.configured && (!valid_host(config.host) || !valid_email(config.from) ||
                             strncmp(config.proxy, "http://", 7) || !config.proxy[7])) return false;
    if (*config.user && !read_secret(env_value("MAIL_SMTP_PASSWORD_FILE", ""), config.password, sizeof(config.password)))
        return false;
    if (strchr(config.user, '\r') || strchr(config.user, '\n')) return false;
    return true;
}

static void stop_server(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static bool source_allowed(const char *address) {
    const char *start = config.allowed_client;
    size_t address_length = strlen(address);
    while (*start) {
        const char *end = strchr(start, ',');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length == address_length && !strncmp(start, address, length)) return true;
        if (!end) break;
        start = end + 1;
    }
    return false;
}

static bool write_all(SSL *tls, const char *text, size_t length) {
    while (length) {
        int count = SSL_write(tls, text, (int)length);
        if (count <= 0) return false;
        text += count;
        length -= (size_t)count;
    }
    return true;
}

static void respond(SSL *tls, int status, const char *json) {
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" :
        status == 415 ? "Unsupported Media Type" : status == 502 ? "Bad Gateway" :
        status == 503 ? "Service Unavailable" : "Internal Server Error";
    char header[512];
    int count = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, strlen(json),
        status == 401 ? "WWW-Authenticate: Bearer\r\n" : "");
    if (count > 0 && (size_t)count < sizeof(header) && write_all(tls, header, (size_t)count))
        (void)write_all(tls, json, strlen(json));
}

static int bounded_read(SSL *tls, void *buffer, int capacity, struct timespec deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    long remaining = (deadline.tv_sec - now.tv_sec) * 1000000L + (deadline.tv_nsec - now.tv_nsec) / 1000L;
    if (remaining <= 0) return -1;
    struct timeval timeout = {remaining / 1000000L, remaining % 1000000L};
    if (setsockopt(SSL_get_fd(tls), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) return -1;
    return SSL_read(tls, buffer, capacity);
}

/* Only one HTTP/1 request per TLS connection, no chunking or ambiguous framing. */
static int receive_request(SSL *tls, Request *request) {
    char buffer[HEADER_LIMIT + BODY_LIMIT + 1];
    size_t count = 0, header_size = 0;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) return 400;
    deadline.tv_sec += 10;
    memset(request, 0, sizeof(*request));
    while (!header_size) {
        if (count >= HEADER_LIMIT) return 413;
        int got = bounded_read(tls, buffer + count, (int)(HEADER_LIMIT - count), deadline);
        if (got <= 0) return 400;
        count += (size_t)got;
        buffer[count] = 0;
        if (memchr(buffer, 0, count)) return 400;
        char *end = strstr(buffer, "\r\n\r\n");
        if (end) header_size = (size_t)(end + 4 - buffer);
    }
    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) return 400;
    *line_end = 0;
    char version[16], extra;
    if (sscanf(buffer, "%11s %127s %15s %c", request->method, request->path, version, &extra) != 3 ||
        strcmp(version, "HTTP/1.1")) return 400;
    bool have_length = false, have_authorization = false, have_type = false, have_host = false;
    int content_length = 0;
    char *line = line_end + 2;
    while (line < buffer + header_size - 2) {
        char *end = strstr(line, "\r\n");
        if (!end) return 400;
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line || isspace((unsigned char)line[0])) return 400;
        *colon = 0;
        for (const unsigned char *p = (const unsigned char *)line; *p; p++)
            if (!(isalnum(*p) || *p == '-')) return 400;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *last = value + strlen(value);
        while (last > value && (last[-1] == ' ' || last[-1] == '\t')) *--last = 0;
        for (const unsigned char *p = (const unsigned char *)value; *p; p++)
            if (*p < 32 && *p != '\t') return 400;
        if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect")) return 400;
        if (!strcasecmp(line, "Host")) {
            if (have_host || !*value) return 400;
            have_host = true;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (have_length || !number(value, 0, BODY_LIMIT, &content_length)) return 400;
            have_length = true;
        } else if (!strcasecmp(line, "Authorization")) {
            if (have_authorization || strlen(value) >= sizeof(request->authorization)) return 400;
            strcpy(request->authorization, value);
            have_authorization = true;
        } else if (!strcasecmp(line, "Content-Type")) {
            if (have_type || strlen(value) >= sizeof(request->content_type)) return 400;
            strcpy(request->content_type, value);
            have_type = true;
        }
        line = end + 2;
    }
    if (!have_host || (!strcmp(request->method, "POST") && !have_length)) return 400;
    size_t body_bytes = count - header_size;
    if (body_bytes > (size_t)content_length) return 400;
    memcpy(request->body, buffer + header_size, body_bytes);
    while (body_bytes < (size_t)content_length) {
        int got = bounded_read(tls, request->body + body_bytes, content_length - (int)body_bytes, deadline);
        if (got <= 0) return 400;
        body_bytes += (size_t)got;
    }
    if (memchr(request->body, 0, body_bytes)) return 400;
    request->body[body_bytes] = 0;
    request->length = body_bytes;
    return 0;
}

static int hex_value(unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static bool decode(const char *source, size_t length, char *target, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            if (i + 2 >= length) return false;
            int a = hex_value((unsigned char)source[i + 1]), b = hex_value((unsigned char)source[i + 2]);
            if (a < 0 || b < 0) return false;
            c = (unsigned char)(a * 16 + b);
            i += 2;
        } else if (c == '+') c = ' ';
        if (!c || used + 1 >= capacity) return false;
        target[used++] = (char)c;
    }
    target[used] = 0;
    return true;
}

static bool valid_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 128) { p++; continue; }
        unsigned int value, need;
        if (*p >= 0xc2 && *p <= 0xdf) { value = *p & 31; need = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { value = *p & 15; need = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { value = *p & 7; need = 3; }
        else return false;
        p++;
        for (unsigned int i = 0; i < need; i++, p++) {
            if ((*p & 0xc0) != 0x80) return false;
            value = (value << 6) | (*p & 63);
        }
        if ((need == 2 && value < 0x800) || (need == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool thread_ids(const char *value, bool multiple) {
    if (!*value) return true;
    size_t count = 0;
    const char *p = value;
    while (*p) {
        if (*p++ != '<') return false;
        const char *start = p;
        bool at = false;
        while (*p && *p != '>') {
            if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || strchr("<()\\\"", *p)) return false;
            if (*p == '@') { if (at || p == start || p[1] == '>') return false; at = true; }
            p++;
        }
        if (*p != '>' || !at || (size_t)(p - start) > 253 || ++count > 32) return false;
        p++;
        if (!*p) return true;
        if (!multiple || *p++ != ' ' || !*p) return false;
    }
    return false;
}

static bool parse_email(const Request *request, Email *email) {
    memset(email, 0, sizeof(*email));
    unsigned int present = 0;
    const char *part = request->body;
    while (*part) {
        const char *end = strchr(part, '&');
        if (!end) end = part + strlen(part);
        const char *equals = memchr(part, '=', (size_t)(end - part));
        char name[40];
        if (!equals || !decode(part, (size_t)(equals - part), name, sizeof(name))) return false;
        char *target = NULL;
        size_t capacity = 0;
        unsigned int bit;
        if (!strcmp(name, "idempotency_key")) { target = email->key; capacity = sizeof(email->key); bit = 1; }
        else if (!strcmp(name, "to")) { target = email->to; capacity = sizeof(email->to); bit = 2; }
        else if (!strcmp(name, "subject")) { target = email->subject; capacity = sizeof(email->subject); bit = 4; }
        else if (!strcmp(name, "body")) { target = email->body; capacity = sizeof(email->body); bit = 8; }
        else if (!strcmp(name, "message_id")) { target = email->message_id; capacity = sizeof(email->message_id); bit = 16; }
        else if (!strcmp(name, "in_reply_to")) { target = email->in_reply_to; capacity = sizeof(email->in_reply_to); bit = 32; }
        else if (!strcmp(name, "references")) { target = email->references; capacity = sizeof(email->references); bit = 64; }
        else return false;
        if ((present & bit) || !decode(equals + 1, (size_t)(end - equals - 1), target, capacity)) return false;
        present |= bit;
        part = *end ? end + 1 : end;
    }
    if ((present & 15) != 15 || !*email->key || !*email->subject || !*email->body || !valid_email(email->to) ||
        !thread_ids(email->message_id, false) || !thread_ids(email->in_reply_to, false) || !thread_ids(email->references, true)) return false;
    for (const unsigned char *p = (const unsigned char *)email->key; *p; p++)
        if (!(isalnum(*p) || strchr("_.:-", *p))) return false;
    for (const unsigned char *p = (const unsigned char *)email->subject; *p; p++)
        if (*p < 32 || *p == 127) return false;
    for (const unsigned char *p = (const unsigned char *)email->body; *p; p++)
        if ((*p < 32 && *p != '\r' && *p != '\n' && *p != '\t') || *p == 127) return false;
    return valid_utf8(email->subject) && valid_utf8(email->body);
}

static bool payload_hash(const Email *email, char output[65]) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) return false;
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    const char *parts[] = {email->to, email->subject, email->body};
    for (size_t i = 0; ok && i < sizeof(parts) / sizeof(parts[0]); i++) {
        char length[32];
        int written = snprintf(length, sizeof(length), "%zu:", strlen(parts[i]));
        ok = written > 0 && EVP_DigestUpdate(context, length, (size_t)written) == 1 &&
             EVP_DigestUpdate(context, parts[i], strlen(parts[i])) == 1;
    }
    /* Keep existing, unthreaded ledger hashes stable across upgrades. */
    if (*email->message_id || *email->in_reply_to || *email->references) {
        const char *headers[] = {email->message_id, email->in_reply_to, email->references};
        ok = ok && EVP_DigestUpdate(context, "thread-v1:", 10) == 1;
        for (size_t i = 0; ok && i < 3; i++) {
            char length[32];
            int written = snprintf(length, sizeof(length), "%zu:", strlen(headers[i]));
            ok = written > 0 && EVP_DigestUpdate(context, length, (size_t)written) == 1 &&
                 EVP_DigestUpdate(context, headers[i], strlen(headers[i])) == 1;
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    ok = ok && EVP_DigestFinal_ex(context, digest, &length) == 1 && length == 32;
    EVP_MD_CTX_free(context);
    if (!ok) return false;
    for (unsigned int i = 0; i < length; i++) snprintf(output + i * 2, 3, "%02x", digest[i]);
    return true;
}

static bool initialize_ledger(void) {
    if (sqlite3_open(config.ledger, &ledger) != SQLITE_OK) return false;
    sqlite3_busy_timeout(ledger, 5000);
    const char *sql = "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS deliveries(idempotency_key TEXT PRIMARY KEY,"
        "payload_hash TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN('sending','sent','unknown','failed')),"
        "created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
        "UPDATE deliveries SET status='unknown',updated_at=unixepoch() WHERE status='sending';";
    return sqlite3_exec(ledger, sql, NULL, NULL, NULL) == SQLITE_OK;
}

/* Reserve and fsync before contacting SMTP. A crash cannot cause silent resending. */
static int reserve_delivery(const Email *email, char status[16]) {
    char hash[65];
    if (!payload_hash(email, hash) || sqlite3_exec(ledger, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    sqlite3_stmt *query = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(ledger, "SELECT payload_hash,status FROM deliveries WHERE idempotency_key=?", -1, &query, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(query, 1, email->key, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(query);
    if (step == SQLITE_ROW) {
        const char *stored_hash = (const char *)sqlite3_column_text(query, 0);
        const char *stored_status = (const char *)sqlite3_column_text(query, 1);
        if (strcmp(stored_hash, hash)) result = 2;
        else {
            snprintf(status, 16, "%s", !strcmp(stored_status, "sending") ? "unknown" : stored_status);
            result = 1;
        }
        goto done;
    }
    if (step != SQLITE_DONE) goto done;
    sqlite3_finalize(query);
    query = NULL;
    if (sqlite3_prepare_v2(ledger, "INSERT INTO deliveries VALUES(?,?,'sending',unixepoch(),unixepoch())", -1, &query, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(query, 1, email->key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 2, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(query) == SQLITE_DONE) result = 0;
done:
    sqlite3_finalize(query);
    if (sqlite3_exec(ledger, result < 0 ? "ROLLBACK" : "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(ledger, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return result;
}

static bool complete_delivery(const char *key, const char *status) {
    sqlite3_stmt *query = NULL;
    if (sqlite3_prepare_v2(ledger, "UPDATE deliveries SET status=?,updated_at=unixepoch() WHERE idempotency_key=?", -1, &query, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(query, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 2, key, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(query) == SQLITE_DONE && sqlite3_changes(ledger) == 1;
    sqlite3_finalize(query);
    return ok;
}

typedef struct { const char *data; size_t length, offset; } Upload;

static size_t read_upload(char *buffer, size_t size, size_t count, void *data) {
    Upload *upload = data;
    size_t available = size * count;
    size_t remaining = upload->length - upload->offset;
    if (available > remaining) available = remaining;
    memcpy(buffer, upload->data + upload->offset, available);
    upload->offset += available;
    return available;
}

static char *mime_message(const Email *email) {
    unsigned char encoded_body[4 * ((sizeof(email->body) + 2) / 3) + 1];
    int body_length = EVP_EncodeBlock(encoded_body, (const unsigned char *)email->body, (int)strlen(email->body));
    size_t capacity = sizeof(email->subject) * 2 + (size_t)body_length + 6144;
    char *message = malloc(capacity);
    if (!message) return NULL;
    time_t now = time(NULL);
    struct tm utc;
    char date[64];
    gmtime_r(&now, &utc);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &utc);
    int header = snprintf(message, capacity, "Date: %s\r\nFrom: <%s>\r\nTo: <%s>\r\nSubject: ", date, config.from, email->to);
    size_t used = (size_t)header;
    /* Short encoded words keep RFC 5322 header lines within the folding limit. */
    size_t subject_length = strlen(email->subject);
    for (size_t offset = 0; offset < subject_length;) {
        size_t length = subject_length - offset;
        if (length > 42) length = 42;
        while (offset + length < subject_length && ((unsigned char)email->subject[offset + length] & 0xc0) == 0x80) length--;
        unsigned char encoded[61];
        int encoded_length = EVP_EncodeBlock(encoded, (const unsigned char *)email->subject + offset, (int)length);
        int wrote = snprintf(message + used, capacity - used, "%s=?UTF-8?B?%.*s?=", offset ? "\r\n " : "", encoded_length, encoded);
        used += (size_t)wrote;
        offset += length;
    }
    if (*email->message_id) used += (size_t)snprintf(message + used, capacity - used, "\r\nMessage-ID: %s", email->message_id);
    if (*email->in_reply_to) used += (size_t)snprintf(message + used, capacity - used, "\r\nIn-Reply-To: %s", email->in_reply_to);
    if (*email->references) {
        used += (size_t)snprintf(message + used, capacity - used, "\r\nReferences:");
        const char *p = email->references;
        while (*p) {
            const char *end = strchr(p, ' ');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            used += (size_t)snprintf(message + used, capacity - used, "\r\n %.*s", (int)n, p);
            p += n; if (*p) p++;
        }
    }
    int wrote = snprintf(message + used, capacity - used, "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: base64\r\n\r\n");
    used += (size_t)wrote;
    for (int offset = 0; offset < body_length; offset += 76) {
        int length = body_length - offset;
        if (length > 76) length = 76;
        memcpy(message + used, encoded_body + offset, (size_t)length);
        used += (size_t)length;
        memcpy(message + used, "\r\n", 2);
        used += 2;
    }
    message[used] = 0;
    return message;
}

static const char *send_email(const Email *email) {
    CURL *curl = curl_easy_init();
    char *message = mime_message(email);
    if (!curl || !message) { if (curl) curl_easy_cleanup(curl); free(message); return "failed"; }
    char url[512];
    snprintf(url, sizeof(url), "%s://%s:%d", !strcmp(config.smtp_mode, "smtps") ? "smtps" : "smtp", config.host, config.smtp_port);
    struct curl_slist *recipients = curl_slist_append(NULL, email->to);
    Upload upload = {message, strlen(message), 0};
    CURLcode options = CURLE_OK;
#define SETOPT(option, value) do { if (options == CURLE_OK) options = curl_easy_setopt(curl, option, value); } while (0)
    SETOPT(CURLOPT_URL, url);
    SETOPT(CURLOPT_PROTOCOLS_STR, "smtp,smtps");
    SETOPT(CURLOPT_PROXY, config.proxy);
    SETOPT(CURLOPT_PROXYTYPE, (long)CURLPROXY_HTTP);
    SETOPT(CURLOPT_HTTPPROXYTUNNEL, 1L);
    SETOPT(CURLOPT_NOPROXY, "");
    SETOPT(CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    SETOPT(CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_CONNECTTIMEOUT, (long)config.timeout);
    SETOPT(CURLOPT_TIMEOUT, (long)config.timeout);
    SETOPT(CURLOPT_MAIL_FROM, config.from);
    SETOPT(CURLOPT_MAIL_RCPT, recipients);
    SETOPT(CURLOPT_READFUNCTION, read_upload);
    SETOPT(CURLOPT_READDATA, &upload);
    SETOPT(CURLOPT_UPLOAD, 1L);
    SETOPT(CURLOPT_INFILESIZE_LARGE, (curl_off_t)upload.length);
    if (*config.ca) SETOPT(CURLOPT_CAINFO, config.ca);
    if (*config.user) {
        SETOPT(CURLOPT_USERNAME, config.user);
        SETOPT(CURLOPT_PASSWORD, config.password);
    }
    CURLcode result = recipients && options == CURLE_OK ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    long response = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    const char *status = result == CURLE_OK ? "sent" :
        upload.offset && !(response >= 400 && response < 600) ? "unknown" : "failed";
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    free(message);
#undef SETOPT
    return status;
}

static void delivery_response(SSL *tls, const Email *email, const char *status, bool replayed) {
    char json[384];
    const char *error = !strcmp(status, "sent") ? "" : !strcmp(status, "unknown") ?
        "\"error\":\"delivery_unknown\"," : "\"error\":\"smtp_delivery_failed\",";
    snprintf(json, sizeof(json), "{%s\"status\":\"%s\",\"idempotency_key\":\"%s\",\"replayed\":%s}",
             error, status, email->key, replayed ? "true" : "false");
    respond(tls, !strcmp(status, "sent") ? 200 : !strcmp(status, "unknown") ? 409 : 502, json);
}

static void handle_request(SSL *tls, const char *source) {
    Request request;
    int error = receive_request(tls, &request);
    if (error) { respond(tls, error, "{\"error\":\"invalid_request\"}"); return; }
    if (!strcmp(request.path, "/healthz") && !strcmp(request.method, "GET")) {
        respond(tls, 200, "{\"status\":\"ok\"}");
        return;
    }
    if (!source_allowed(source)) {
        respond(tls, 403, "{\"error\":\"forbidden\"}"); return;
    }
    const char *prefix = "Bearer ";
    size_t token_length = strlen(config.token);
    if (strncmp(request.authorization, prefix, 7) || strlen(request.authorization + 7) != token_length ||
        CRYPTO_memcmp(request.authorization + 7, config.token, token_length)) {
        respond(tls, 401, "{\"error\":\"unauthorized\"}"); return;
    }
    if (!strcmp(request.path, "/v1/status")) {
        if (strcmp(request.method, "GET")) { respond(tls, 405, "{\"error\":\"method_not_allowed\"}"); return; }
        respond(tls, 200, config.configured ?
            "{\"smtp_configured\":true,\"transport\":\"smtp_tls\",\"idempotency\":true}" :
            "{\"smtp_configured\":false,\"transport\":\"smtp_tls\",\"idempotency\":true}");
        return;
    }
    if (strcmp(request.path, "/v1/emails")) { respond(tls, 404, "{\"error\":\"not_found\"}"); return; }
    if (strcmp(request.method, "POST")) { respond(tls, 405, "{\"error\":\"method_not_allowed\"}"); return; }
    if (strcmp(request.content_type, "application/x-www-form-urlencoded") &&
        strcmp(request.content_type, "application/x-www-form-urlencoded; charset=UTF-8")) {
        respond(tls, 415, "{\"error\":\"unsupported_media_type\"}"); return;
    }
    Email email;
    if (!parse_email(&request, &email)) { respond(tls, 400, "{\"error\":\"invalid_email\"}"); return; }
    if (!config.configured) {
        respond(tls, 503, "{\"error\":\"smtp_not_configured\",\"status\":\"pending_configuration\"}"); return;
    }
    char status[16];
    int reservation = reserve_delivery(&email, status);
    if (reservation < 0) { respond(tls, 503, "{\"error\":\"ledger_unavailable\"}"); return; }
    if (reservation == 2) { respond(tls, 409, "{\"error\":\"idempotency_conflict\"}"); return; }
    if (reservation == 1) { delivery_response(tls, &email, status, true); return; }
    const char *delivery = send_email(&email);
    if (!complete_delivery(email.key, delivery)) delivery = "unknown";
    delivery_response(tls, &email, delivery, false);
}

int main(void) {
    (void)umask(0077);
    signal(SIGTERM, stop_server);
    signal(SIGINT, stop_server);
    signal(SIGPIPE, SIG_IGN);
    if (!load_config()) { fputs("mail-addon: invalid or incomplete configuration\n", stderr); return 1; }
    if (!initialize_ledger()) { fputs("mail-addon: ledger unavailable\n", stderr); return 1; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(context, config.cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, config.key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1) {
        fputs("mail-addon: TLS configuration unavailable\n", stderr); return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config.port);
    if (listener < 0 || inet_pton(AF_INET, config.bind_address, &address.sin_addr) != 1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 32)) {
        fputs("mail-addon: listener unavailable\n", stderr); return 1;
    }
    fprintf(stderr, "mail-addon: HTTPS ready; SMTP %s\n", config.configured ? "configured" : "pending configuration");
    while (!stopping) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(listener, &readers);
        struct timeval interval = {1, 0};
        int available = select(listener + 1, &readers, NULL, NULL, &interval);
        if (available <= 0) continue;
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int client = accept(listener, (struct sockaddr *)&peer, &peer_size);
        if (client < 0) continue;
        char source[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &peer.sin_addr, source, sizeof(source)) ||
            (!source_allowed(source) && strcmp(source, "127.0.0.1") && strcmp(source, config.bind_address))) {
            close(client); continue;
        }
        struct timeval timeout = {10, 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        SSL *tls = SSL_new(context);
        if (tls) {
            SSL_set_fd(tls, client);
            if (SSL_accept(tls) == 1) handle_request(tls, source);
            (void)SSL_shutdown(tls);
            SSL_free(tls);
        }
        close(client);
    }
    close(listener);
    SSL_CTX_free(context);
    sqlite3_close(ledger);
    curl_global_cleanup();
    OPENSSL_cleanse(config.token, sizeof(config.token));
    OPENSSL_cleanse(config.password, sizeof(config.password));
    return 0;
}
