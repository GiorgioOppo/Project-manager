/* A small, independent ticket service. One request per connection, no proxy trust. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
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
#include <sqlite3.h>
#include <json-c/json.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define HEADER_MAX 16384
#define BODY_MAX 65536
#define PASSWORD_ROUNDS 600000
#define SESSION_SECONDS 28800
#define TICKET_COLUMNS "t.id,t.title,t.description,t.requester,t.status,t.priority,t.assignee_id,a.name,t.created_by,c.name,t.due_date,t.created_at,t.updated_at,t.revision"
#define TICKET_FROM " FROM tickets t LEFT JOIN users a ON a.id=t.assignee_id JOIN users c ON c.id=t.created_by "

static sqlite3 *db;
static volatile sig_atomic_t stopping;
static int listener = -1;
static int configured_port = 8083;
static const char *public_dir = "public";
static char public_origin[512];
static char public_authority[256];
static bool secure_cookie;

typedef struct {
    int fd;
    char method[12], target[2048], path[2048];
    char *query, *body;
    size_t body_len;
    char host[256], origin[512], cookie[2048], csrf[128], content_type[128];
    bool has_origin;
} Request;
typedef struct {
    sqlite3_int64 id;
    char name[401], email[255], role[16], csrf[65], token_hash[65];
} Auth;

static void stop_server(int sig) { (void)sig;
stopping = 1;
if (listener >= 0) { close(listener);
listener = -1;
} }
/* Validate UTF-8 and count scalar values, matching HTML character limits. */
static size_t utf8_length(const char *s) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t codepoint;
        unsigned continuation;
        if (*p < 0x80) { codepoint = *p;
        continuation = 0;
        }
        else if (*p >= 0xc2 && *p <= 0xdf) { codepoint = *p & 0x1f;
        continuation = 1;
        }
        else if (*p >= 0xe0 && *p <= 0xef) { codepoint = *p & 0x0f;
        continuation = 2;
        }
        else if (*p >= 0xf0 && *p <= 0xf4) { codepoint = *p & 0x07;
        continuation = 3;
        }
        else return SIZE_MAX;
        p++;
        for (unsigned i = 0; i < continuation; i++) {
            if (*p < 0x80 || *p > 0xbf) return SIZE_MAX;
            codepoint = (codepoint << 6) | (*p++ & 0x3f);
        }
        if ((continuation == 1 && codepoint < 0x80) || (continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000) || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return SIZE_MAX;
        count++;
    }
    return count;
}
static bool whitespace_only(const char *s) { for (; *s; s++) if (!isspace((unsigned char)*s)) return false;
return true;
}
static const char *env_or(const char *name, const char *fallback) { const char *v = getenv(name);
return v && *v ? v : fallback;
}
static void bounded_copy(char *dst, size_t n, const char *src) { if (n) { size_t len = strlen(src);
if (len >= n) len = n - 1;
memcpy(dst, src, len);
dst[len] = 0;
} }
static bool constant_equal(const char *a, const char *b) { size_t n = strlen(a);
return n == strlen(b) && CRYPTO_memcmp(a, b, n) == 0;
}
static bool random_hex(char *out, size_t bytes) {
    unsigned char raw[64];
    static const char digits[] = "0123456789abcdef";
    if (bytes > sizeof(raw) || RAND_bytes(raw, (int)bytes) != 1) return false;
    for (size_t i = 0; i < bytes; i++) { out[i * 2] = digits[raw[i] >> 4];
    out[i * 2 + 1] = digits[raw[i] & 15];
    }
    out[bytes * 2] = 0;
    OPENSSL_cleanse(raw, sizeof(raw));
    return true;
}
static bool digest_hex(const char *value, char out[65]) {
    unsigned char raw[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    static const char digits[] = "0123456789abcdef";
    if (EVP_Digest(value, strlen(value), raw, &len, EVP_sha256(), NULL) != 1 || len != 32) return false;
    for (unsigned i = 0; i < len; i++) { out[i * 2] = digits[raw[i] >> 4];
    out[i * 2 + 1] = digits[raw[i] & 15];
    }
    out[64] = 0;
    return true;
}
static bool password_hash(const char *password, const char *salt, char out[65]) {
    unsigned char raw[32];
    static const char digits[] = "0123456789abcdef";
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), (const unsigned char *)salt, (int)strlen(salt), PASSWORD_ROUNDS, EVP_sha256(), 32, raw) != 1) return false;
    for (unsigned i = 0; i < 32; i++) { out[i * 2] = digits[raw[i] >> 4];
    out[i * 2 + 1] = digits[raw[i] & 15];
    }
    out[64] = 0;
    OPENSSL_cleanse(raw, sizeof(raw));
    return true;
}
static bool exec_sql(const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) { fprintf(stderr, "SQLite: %s\n", error ? error : "errore");
    sqlite3_free(error);
    return false;
    }
    return true;
}
static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { fprintf(stderr, "SQLite prepare: %s\n", sqlite3_errmsg(db));
    return NULL;
    }
    return st;
}
static void bind_text(sqlite3_stmt *st, int index, const char *s) { if (s) sqlite3_bind_text(st, index, s, -1, SQLITE_TRANSIENT);
else sqlite3_bind_null(st, index);
}
static const char *column_text(sqlite3_stmt *st, int col) { const unsigned char *v = sqlite3_column_text(st, col);
return v ? (const char *)v : "";
}
static void add_string(json_object *o, const char *key, const char *s) { json_object_object_add(o, key, s ? json_object_new_string(s) : NULL);
}
static void add_int(json_object *o, const char *key, sqlite3_int64 value) { json_object_object_add(o, key, json_object_new_int64(value));
}
static bool send_bytes(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) { ssize_t n = send(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    len -= (size_t)n;
    }
    return true;
}
static const char *status_text(int status) {
    switch (status) { case 200:return "OK";
    case 201:return "Created";
    case 400:return "Bad Request";
    case 401:return "Unauthorized";
    case 403:return "Forbidden";
    case 404:return "Not Found";
    case 405:return "Method Not Allowed";
    case 409:return "Conflict";
    case 413:return "Payload Too Large";
    case 415:return "Unsupported Media Type";
    case 429:return "Too Many Requests";
    default:return "Internal Server Error";
    }
}
static void response(Request *r, int status, const char *mime, const void *body, size_t len, const char *extra) {
    char header[2048];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n"
        "Cache-Control: no-store\r\n%s\r\n", status, status_text(status), mime, len, extra ? extra : "");
    if (n > 0 && (size_t)n < sizeof(header) && send_bytes(r->fd, header, (size_t)n)) (void)send_bytes(r->fd, body, len);
}
static void send_json_extra(Request *r, int status, json_object *o, const char *extra) {
    const char *body = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    response(r, status, "application/json; charset=utf-8", body, strlen(body), extra);
    json_object_put(o);
}
static void send_json(Request *r, int status, json_object *o) { send_json_extra(r, status, o, NULL);
}
static void error_response(Request *r, int status, const char *message) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(false));
    add_string(o, "error", message);
    send_json(r, status, o);
}
static void db_error(Request *r) { error_response(r, 500, "Errore di archivio. Riprova.");
}
static bool parse_uint(const char *s, long long max, long long *out) {
    if (!*s) return false;
    long long n = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9' || n > (max - (*s - '0')) / 10) return false;
    n = n * 10 + (*s - '0');
    } *out = n;
    return true;
}
static bool local_authority(const char *host) {
    char expected[80];
    const char *names[] = {"localhost", "127.0.0.1", "[::1]"};
    for (size_t i = 0; i < 3; i++) { snprintf(expected, sizeof(expected), "%s:%d", names[i], configured_port);
    if (!strcasecmp(host, expected)) return true;
    if (configured_port == 80 && !strcasecmp(host, names[i])) return true;
    }
    return false;
}
static bool host_allowed(Request *r) {
    if (*public_authority) { if (!strcasecmp(r->host, public_authority)) return true;
    return !strcmp(r->method, "GET") && !strcmp(r->path, "/healthz") && local_authority(r->host);
    }
    return local_authority(r->host);
}
static bool origin_allowed(Request *r) {
    if (!r->has_origin) return true;
    if (*public_origin) return !strcmp(r->origin, public_origin);
    if (strncmp(r->origin, "http://", 7)) return false;
    return local_authority(r->origin + 7);
}
static bool header_value(char *dst, size_t cap, const char *value) { if (strlen(value) >= cap || *dst) return false;
bounded_copy(dst, cap, value);
return true;
}
static bool valid_header_name(const char *name) {
    for (; *name; name++) if (!(isalnum((unsigned char)*name) || strchr("!#$%&'*+-.^_`|~", *name))) return false;
    return true;
}
static int read_request(Request *r) {
    char *raw = malloc(HEADER_MAX + BODY_MAX + 1);
    if (!raw) return 500;
    size_t used = 0, header_len = 0;
    time_t deadline = time(NULL) + 10;
    while (!header_len) {
        if (used >= HEADER_MAX || time(NULL) >= deadline) { free(raw);
        return 413;
        }
        ssize_t n = recv(r->fd, raw + used, HEADER_MAX - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(raw);
        return 400;
        }
        used += (size_t)n;
        raw[used] = 0;
        if (memchr(raw, 0, used)) { free(raw);
        return 400;
        }
        char *end = strstr(raw, "\r\n\r\n");
        if (end) header_len = (size_t)(end - raw) + 4;
    }
    raw[header_len - 2] = 0;
    char *line_end = strstr(raw, "\r\n");
    if (!line_end) { free(raw);
    return 400;
    } *line_end = 0;
    char version[16], trailing;
    int fields = sscanf(raw, "%11s %2047s %15s%c", r->method, r->target, version, &trailing);
    if (fields != 3 || (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) || r->target[0] != '/' || strchr(r->target, '#')) { free(raw);
    return 400;
    }
    for (const char *p = r->target; *p; p++) if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '\\') { free(raw);
    return 400;
    }
    bounded_copy(r->path, sizeof(r->path), r->target);
    char *query = strchr(r->path, '?');
    if (query) { *query++ = 0;
    r->query = query;
    }
    long long content_len = 0;
    bool has_length = false, has_host = false, has_cookie = false, has_csrf = false, has_type = false;
    for (char *line = line_end + 2; *line; ) {
        char *next = strstr(line, "\r\n");
        if (!next) { free(raw);
        return 400;
        } *next = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line || *line == ' ' || *line == '\t') { free(raw);
        return 400;
        }
        *colon = 0;
        if (!valid_header_name(line)) { free(raw);
        return 400;
        }
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        for (char *p = value; *p; p++) if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127) { free(raw);
        return 400;
        }
        bool ok = true;
        if (!strcasecmp(line, "Host")) { ok = !has_host && *value && header_value(r->host, sizeof(r->host), value);
        has_host = true;
        }
        else if (!strcasecmp(line, "Origin")) { ok = !r->has_origin && *value && header_value(r->origin, sizeof(r->origin), value);
        r->has_origin = true;
        }
        else if (!strcasecmp(line, "Cookie")) { ok = !has_cookie && header_value(r->cookie, sizeof(r->cookie), value);
        has_cookie = true;
        }
        else if (!strcasecmp(line, "X-CSRF-Token")) { ok = !has_csrf && header_value(r->csrf, sizeof(r->csrf), value);
        has_csrf = true;
        }
        else if (!strcasecmp(line, "Content-Type")) { ok = !has_type && header_value(r->content_type, sizeof(r->content_type), value);
        has_type = true;
        }
        else if (!strcasecmp(line, "Content-Length")) { ok = !has_length && parse_uint(value, BODY_MAX, &content_len);
        has_length = true;
        }
        else if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect")) ok = false;
        if (!ok) { free(raw);
        return 400;
        } line = next + 2;
    }
    if (!has_host || !host_allowed(r)) { free(raw);
    return 400;
    }
    if (!strcmp(r->method, "GET") && content_len) { free(raw);
    return 400;
    }
    size_t body_read = used - header_len;
    if (body_read > (size_t)content_len) { free(raw);
    return 400;
    }
    while (body_read < (size_t)content_len) {
        if (time(NULL) >= deadline) { free(raw);
        return 400;
        }
        ssize_t n = recv(r->fd, raw + header_len + body_read, (size_t)content_len - body_read, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(raw);
        return 400;
        } body_read += (size_t)n;
    }
    r->body_len = (size_t)content_len;
    r->body = malloc(r->body_len + 1);
    if (!r->body) { free(raw);
    return 500;
    }
    memcpy(r->body, raw + header_len, r->body_len);
    r->body[r->body_len] = 0;
    free(raw);
    return 0;
}
static json_object *parse_json(Request *r) {
    if (strncasecmp(r->content_type, "application/json", 16) || (r->content_type[16] && r->content_type[16] != ';')) { error_response(r, 415, "Usa Content-Type application/json.");
    return NULL;
    }
    json_tokener *tok = json_tokener_new();
    if (!tok) { db_error(r);
    return NULL;
    }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *o = json_tokener_parse_ex(tok, r->body, (int)r->body_len);
    enum json_tokener_error error = json_tokener_get_error(tok);
    size_t end = json_tokener_get_parse_end(tok);
    while (end < r->body_len && isspace((unsigned char)r->body[end])) end++;
    bool valid = error == json_tokener_success && o && json_object_is_type(o, json_type_object) && end == r->body_len;
    json_tokener_free(tok);
    if (!valid) { if (o) json_object_put(o);
    error_response(r, 400, "Corpo JSON non valido.");
    return NULL;
    }
    return o;
}
static bool get_string(json_object *o, const char *key, const char **out, size_t max, bool required, bool nonblank) {
    json_object *v = NULL;
    if (!json_object_object_get_ex(o, key, &v)) return !required;
    if (!v || !json_object_is_type(v, json_type_string)) return false;
    const char *s = json_object_get_string(v);
    size_t len = (size_t)json_object_get_string_len(v);
    if (len != strlen(s) || utf8_length(s) > max || (nonblank && (!len || whitespace_only(s)))) return false;
    *out = s;
    return true;
}
static bool allowed_keys(json_object *o, const char *const keys[]) {
    json_object_object_foreach(o, key, value) { (void)value;
    bool found = false;
    for (size_t i = 0; keys[i]; i++) if (!strcmp(key, keys[i])) { found = true;
    break;
    }
    if (!found) return false;
    }
    return true;
}
static bool email_normalize(const char *raw, char out[255]) {
    size_t len = strlen(raw);
    if (!len || len > 254) return false;
    const char *at = strchr(raw, '@');
    if (!at || at == raw || !at[1] || strchr(at + 1, '@')) return false;
    for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)raw[i];
    if (c <= 32 || c >= 127 || strchr("<>(),;:\\[]\"", c)) return false;
    out[i] = (char)tolower(c);
    } out[len] = 0;
    return true;
}
static bool valid_password(const char *s) { size_t n = utf8_length(s);
return n >= 12 && n <= 1024;
}
static bool valid_role(const char *s) { return !strcmp(s, "admin") || !strcmp(s, "agent");
}
static json_object *user_json(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_string(o, "name", column_text(st, 1));
    add_string(o, "email", column_text(st, 2));
    add_string(o, "role", column_text(st, 3));
    json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 4)));
    return o;
}
static json_object *fetch_user(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare("SELECT id,name,email,role,active FROM users WHERE id=?1");
    if (!st) return NULL;
    sqlite3_bind_int64(st, 1, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? user_json(st) : NULL;
    sqlite3_finalize(st);
    return o;
}
static bool session_cookie(const char *cookies, char token[65]) {
    bool found = false;
    const char *p = cookies;
    while (*p) { while (*p == ' ' || *p == ';') p++;
    const char *end = strchr(p, ';');
    if (!end) end = p + strlen(p);
        if ((size_t)(end - p) >= 16 && !strncmp(p, "tickets_session=", 16)) { if (found || end - p != 80) return false;
        for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)p[16 + i])) return false;
        memcpy(token, p + 16, 64);
        token[64] = 0;
        found = true;
        }
        p = end;
    }
    return found;
}
static bool authenticate(Request *r, Auth *a) {
    char token[65];
    if (!session_cookie(r->cookie, token) || !digest_hex(token, a->token_hash)) return false;
    sqlite3_stmt *st = prepare("SELECT u.id,u.name,u.email,u.role,s.csrf FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=?1 AND s.expires_at>unixepoch() AND u.active=1");
    if (!st) return false;
    bind_text(st, 1, a->token_hash);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) { a->id = sqlite3_column_int64(st, 0);
    bounded_copy(a->name, sizeof(a->name), column_text(st, 1));
    bounded_copy(a->email, sizeof(a->email), column_text(st, 2));
    bounded_copy(a->role, sizeof(a->role), column_text(st, 3));
    bounded_copy(a->csrf, sizeof(a->csrf), column_text(st, 4));
    }
    sqlite3_finalize(st);
    OPENSSL_cleanse(token, sizeof(token));
    return ok;
}
static void session_response(Request *r, Auth *a, const char *cookie) {
    json_object *o = json_object_new_object(), *user = fetch_user(a->id);
    if (!user) { json_object_put(o);
    db_error(r);
    return;
    }
    json_object_object_add(o, "ok", json_object_new_boolean(true));
    json_object_object_add(o, "user", user);
    add_string(o, "csrf_token", a->csrf);
    send_json_extra(r, 200, o, cookie);
}
static void login(Request *r) {
    static time_t failures_since;
    static unsigned failures;
    time_t now = time(NULL);
    if (now - failures_since >= 60) { failures = 0;
    failures_since = now;
    }
    if (failures >= 20) { error_response(r, 429, "Troppi tentativi. Riprova tra un minuto.");
    return;
    }
    json_object *input = parse_json(r);
    if (!input) return;
    const char *email = NULL, *password = NULL;
    char normalized[255];
    const char *const keys[] = {"email", "password", NULL};
    if (!allowed_keys(input, keys) || !get_string(input, "email", &email, 254, true, true) || !get_string(input, "password", &password, 1024, true, false) || !email_normalize(email, normalized)) { json_object_put(input);
    error_response(r, 400, "Email o password non validi.");
    return;
    }
    sqlite3_stmt *st = prepare("SELECT id,password_salt,password_hash FROM users WHERE email=?1 COLLATE NOCASE AND active=1");
    if (!st) { json_object_put(input);
    db_error(r);
    return;
    }
    bind_text(st, 1, normalized);
    char candidate[65], salt[33], expected[65];
    sqlite3_int64 user_id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) { user_id = sqlite3_column_int64(st, 0);
    bounded_copy(salt, sizeof(salt), column_text(st, 1));
    bounded_copy(expected, sizeof(expected), column_text(st, 2));
    }
    else { bounded_copy(salt, sizeof(salt), "00000000000000000000000000000000");
    bounded_copy(expected, sizeof(expected), "0000000000000000000000000000000000000000000000000000000000000000");
    }
    sqlite3_finalize(st);
    bool valid = password_hash(password, salt, candidate) && constant_equal(candidate, expected) && user_id;
    OPENSSL_cleanse(candidate, sizeof(candidate));
    json_object_put(input);
    if (!valid) { failures++;
    error_response(r, 401, "Email o password errati.");
    return;
    }
    Auth a = {0};
    a.id = user_id;
    char token[65];
    if (!random_hex(token, 32) || !random_hex(a.csrf, 32) || !digest_hex(token, a.token_hash)) { db_error(r);
    return;
    }
    if (!exec_sql("DELETE FROM sessions WHERE expires_at<=unixepoch()")) { db_error(r);
    return;
    }
    st = prepare("INSERT INTO sessions(token_hash,user_id,csrf,expires_at) VALUES(?1,?2,?3,unixepoch()+?4)");
    if (!st) { db_error(r);
    return;
    }
    bind_text(st, 1, a.token_hash);
    sqlite3_bind_int64(st, 2, user_id);
    bind_text(st, 3, a.csrf);
    sqlite3_bind_int(st, 4, SESSION_SECONDS);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { db_error(r);
    return;
    }
    char cookie[256];
    snprintf(cookie, sizeof(cookie), "Set-Cookie: tickets_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d%s\r\n", token, SESSION_SECONDS, secure_cookie ? "; Secure" : "");
    OPENSSL_cleanse(token, sizeof(token));
    failures = 0;
    session_response(r, &a, cookie);
}
static void logout(Request *r, Auth *a) {
    sqlite3_stmt *st = prepare("DELETE FROM sessions WHERE token_hash=?1");
    if (!st) { db_error(r);
    return;
    }
    bind_text(st, 1, a->token_hash);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { db_error(r);
    return;
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(true));
    char cookie[180];
    snprintf(cookie, sizeof(cookie), "Set-Cookie: tickets_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%s\r\n", secure_cookie ? "; Secure" : "");
    send_json_extra(r, 200, o, cookie);
}
static void list_users(Request *r, Auth *a) {
    sqlite3_stmt *st = prepare(!strcmp(a->role, "admin") ? "SELECT id,name,email,role,active FROM users ORDER BY active DESC,name COLLATE NOCASE,id" : "SELECT id,name,email,role,active FROM users WHERE active=1 ORDER BY name COLLATE NOCASE,id");
    if (!st) { db_error(r);
    return;
    }
    json_object *o = json_object_new_object(), *items = json_object_new_array();
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) json_object_array_add(items, user_json(st));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { json_object_put(o);
    json_object_put(items);
    db_error(r);
    return;
    }
    json_object_object_add(o, "items", items);
    send_json(r, 200, o);
}
static void create_user(Request *r) {
    json_object *input = parse_json(r);
    if (!input) return;
    const char *name = NULL, *email = NULL, *password = NULL, *role = "agent";
    char normalized[255], salt[33], hash[65];
    const char *const keys[] = {"name", "email", "password", "role", NULL};
    if (!allowed_keys(input, keys) || !get_string(input, "name", &name, 100, true, true) || !get_string(input, "email", &email, 254, true, true) || !get_string(input, "password", &password, 1024, true, false) || !get_string(input, "role", &role, 16, false, true) || !valid_role(role) || !valid_password(password) || !email_normalize(email, normalized)) { json_object_put(input);
    error_response(r, 400, "Utente non valido: nome, email, ruolo e password di almeno 12 caratteri sono richiesti.");
    return;
    }
    if (!random_hex(salt, 16) || !password_hash(password, salt, hash)) { json_object_put(input);
    db_error(r);
    return;
    }
    sqlite3_stmt *st = prepare("INSERT INTO users(name,email,role,password_salt,password_hash) VALUES(?1,?2,?3,?4,?5)");
    if (!st) { json_object_put(input);
    db_error(r);
    return;
    }
    bind_text(st, 1, name);
    bind_text(st, 2, normalized);
    bind_text(st, 3, role);
    bind_text(st, 4, salt);
    bind_text(st, 5, hash);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    OPENSSL_cleanse(hash, sizeof(hash));
    json_object_put(input);
    if (rc != SQLITE_DONE) { if (rc == SQLITE_CONSTRAINT) error_response(r, 409, "Questa email è già registrata.");
    else db_error(r);
    return;
    }
    json_object *o = json_object_new_object(), *user = fetch_user(sqlite3_last_insert_rowid(db));
    if (!user) { json_object_put(o);
    db_error(r);
    return;
    }
    json_object_object_add(o, "user", user);
    send_json(r, 201, o);
}
static void update_user(Request *r, sqlite3_int64 id) {
    json_object *input = parse_json(r);
    if (!input) return;
    const char *name = NULL, *role = NULL, *password = NULL;
    const char *const keys[] = {"name", "role", "active", "password", NULL};
    json_object *active_value = NULL;
    bool has_active = json_object_object_get_ex(input, "active", &active_value);
    if (!allowed_keys(input, keys) || !get_string(input, "name", &name, 100, false, true) || !get_string(input, "role", &role, 16, false, true) || (role && !valid_role(role)) || !get_string(input, "password", &password, 1024, false, false) || (password && !valid_password(password)) || (has_active && (!active_value || !json_object_is_type(active_value, json_type_boolean)))) { json_object_put(input);
    error_response(r, 400, "Dati utente non validi. La password deve avere almeno 12 caratteri.");
    return;
    }
    char salt[33] = "", hash[65] = "";
    if (password && (!random_hex(salt, 16) || !password_hash(password, salt, hash))) { json_object_put(input);
    db_error(r);
    return;
    }
    if (!exec_sql("BEGIN IMMEDIATE")) { json_object_put(input);
    db_error(r);
    return;
    }
    sqlite3_stmt *st = prepare("SELECT name,role,active FROM users WHERE id=?1");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st);
    exec_sql("ROLLBACK");
    json_object_put(input);
    if (rc == SQLITE_DONE) error_response(r, 404, "Utente non trovato.");
    else db_error(r);
    return;
    }
    char old_name[401], old_role[16];
    bounded_copy(old_name, sizeof(old_name), column_text(st, 0));
    bounded_copy(old_role, sizeof(old_role), column_text(st, 1));
    bool old_active = sqlite3_column_int(st, 2) != 0;
    sqlite3_finalize(st);
    const char *next_role = role ? role : old_role;
    bool next_active = has_active ? json_object_get_boolean(active_value) != 0 : old_active;
    if (old_active && !strcmp(old_role, "admin") && (!next_active || strcmp(next_role, "admin"))) {
        st = prepare("SELECT count(*) FROM users WHERE active=1 AND role='admin'");
        if (!st) goto fail;
        rc = sqlite3_step(st);
        int count = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
        sqlite3_finalize(st);
        if (rc != SQLITE_ROW) goto fail;
        if (count <= 1) { exec_sql("ROLLBACK");
        json_object_put(input);
        error_response(r, 409, "Deve rimanere almeno un amministratore attivo.");
        return;
        }
    }
    st = prepare("UPDATE users SET name=?1,role=?2,active=?3,password_salt=CASE WHEN ?4 THEN ?5 ELSE password_salt END,password_hash=CASE WHEN ?4 THEN ?6 ELSE password_hash END WHERE id=?7");
    if (!st) goto fail;
    bind_text(st, 1, name ? name : old_name);
    bind_text(st, 2, next_role);
    sqlite3_bind_int(st, 3, next_active);
    sqlite3_bind_int(st, 4, password != NULL);
    bind_text(st, 5, salt);
    bind_text(st, 6, hash);
    sqlite3_bind_int64(st, 7, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    if (password || !next_active || strcmp(next_role, old_role)) { st = prepare("DELETE FROM sessions WHERE user_id=?1");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    }
    if (!exec_sql("COMMIT")) goto fail;
    OPENSSL_cleanse(hash, sizeof(hash));
    json_object_put(input);
    json_object *o = json_object_new_object(), *user = fetch_user(id);
    if (!user) { json_object_put(o);
    db_error(r);
    return;
    }
    json_object_object_add(o, "user", user);
    send_json(r, 200, o);
    return;
fail: exec_sql("ROLLBACK");
OPENSSL_cleanse(hash, sizeof(hash));
json_object_put(input);
db_error(r);
}
static json_object *ticket_json(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_string(o, "title", column_text(st, 1));
    add_string(o, "description", column_text(st, 2));
    add_string(o, "requester", column_text(st, 3));
    add_string(o, "status", column_text(st, 4));
    add_string(o, "priority", column_text(st, 5));
    if (sqlite3_column_type(st, 6) == SQLITE_NULL) json_object_object_add(o, "assignee_id", NULL);
    else add_int(o, "assignee_id", sqlite3_column_int64(st, 6));
    add_string(o, "assignee_name", sqlite3_column_type(st, 7) == SQLITE_NULL ? NULL : column_text(st, 7));
    add_int(o, "created_by", sqlite3_column_int64(st, 8));
    add_string(o, "creator_name", column_text(st, 9));
    add_string(o, "due_date", sqlite3_column_type(st, 10) == SQLITE_NULL ? NULL : column_text(st, 10));
    add_string(o, "created_at", column_text(st, 11));
    add_string(o, "updated_at", column_text(st, 12));
    add_int(o, "revision", sqlite3_column_int64(st, 13));
    return o;
}
static json_object *fetch_ticket(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare("SELECT " TICKET_COLUMNS TICKET_FROM "WHERE t.id=?1");
    if (!st) return NULL;
    sqlite3_bind_int64(st, 1, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? ticket_json(st) : NULL;
    sqlite3_finalize(st);
    return o;
}
static bool valid_status(const char *s) { return !strcmp(s, "open") || !strcmp(s, "in_progress") || !strcmp(s, "waiting") || !strcmp(s, "resolved") || !strcmp(s, "closed");
}
static bool valid_priority(const char *s) { return !strcmp(s, "low") || !strcmp(s, "normal") || !strcmp(s, "high") || !strcmp(s, "urgent");
}
static bool valid_date(const char *s) {
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) return false;
    int y = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0', m = (s[5]-'0')*10+s[6]-'0', d = (s[8]-'0')*10+s[9]-'0';
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || m < 1 || m > 12 || d < 1) return false;
    int last = days[m];
    if (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) last++;
    return d <= last;
}
typedef struct { const char *title, *description, *requester, *status, *priority, *due_date;
sqlite3_int64 assignee_id;
} TicketFields;
static bool ticket_fields(json_object *input, json_object *current, TicketFields *f, bool updating) {
    const char *const create_keys[] = {"title", "description", "requester", "priority", "assignee_id", "due_date", NULL};
    const char *const update_keys[] = {"title", "description", "requester", "priority", "assignee_id", "due_date", "status", "revision", NULL};
    if (!allowed_keys(input, updating ? update_keys : create_keys)) return false;
    f->title = "";
    f->description = "";
    f->requester = "";
    f->status = "open";
    f->priority = "normal";
    f->due_date = NULL;
    f->assignee_id = 0;
    if (current) {
        f->title = json_object_get_string(json_object_object_get(current, "title"));
        f->description = json_object_get_string(json_object_object_get(current, "description"));
        f->requester = json_object_get_string(json_object_object_get(current, "requester"));
        f->status = json_object_get_string(json_object_object_get(current, "status"));
        f->priority = json_object_get_string(json_object_object_get(current, "priority"));
        json_object *due = json_object_object_get(current, "due_date");
        f->due_date = due ? json_object_get_string(due) : NULL;
        f->assignee_id = json_object_get_int64(json_object_object_get(current, "assignee_id"));
    }
    if (!get_string(input, "title", &f->title, 160, !updating, true) || !get_string(input, "description", &f->description, 8000, false, false) || !get_string(input, "requester", &f->requester, 200, false, false) || !get_string(input, "status", &f->status, 32, false, true) || !get_string(input, "priority", &f->priority, 16, false, true) || !valid_status(f->status) || !valid_priority(f->priority)) return false;
    json_object *assignee = NULL;
    if (json_object_object_get_ex(input, "assignee_id", &assignee)) {
        if (!assignee) f->assignee_id = 0;
        else if (!json_object_is_type(assignee, json_type_int) || (f->assignee_id = json_object_get_int64(assignee)) <= 0) return false;
    }
    json_object *due = NULL;
    if (json_object_object_get_ex(input, "due_date", &due)) {
        if (!due) f->due_date = NULL;
        else if (!json_object_is_type(due, json_type_string) || (size_t)json_object_get_string_len(due) != strlen(json_object_get_string(due)) || !valid_date(json_object_get_string(due))) return false;
        else f->due_date = json_object_get_string(due);
    }
    return true;
}
static bool assignee_active(sqlite3_int64 id) {
    if (!id) return true;
    sqlite3_stmt *st = prepare("SELECT 1 FROM users WHERE id=?1 AND active=1");
    if (!st) return false;
    sqlite3_bind_int64(st, 1, id);
    bool active = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return active;
}
static bool add_history(sqlite3_int64 ticket_id, Auth *a, const char *action, const char *detail) {
    sqlite3_stmt *st = prepare("INSERT INTO history(ticket_id,actor_id,actor_name,action,detail) VALUES(?1,?2,?3,?4,?5)");
    if (!st) return false;
    sqlite3_bind_int64(st, 1, ticket_id);
    sqlite3_bind_int64(st, 2, a->id);
    bind_text(st, 3, a->name);
    bind_text(st, 4, action);
    bind_text(st, 5, detail);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
static void bind_ticket(sqlite3_stmt *st, TicketFields *f) {
    bind_text(st, 1, f->title);
    bind_text(st, 2, f->description);
    bind_text(st, 3, f->requester);
    bind_text(st, 4, f->status);
    bind_text(st, 5, f->priority);
    if (f->assignee_id) sqlite3_bind_int64(st, 6, f->assignee_id);
    else sqlite3_bind_null(st, 6);
    bind_text(st, 7, f->due_date);
}
static void ticket_response(Request *r, int status, sqlite3_int64 id) {
    json_object *o = json_object_new_object(), *ticket = fetch_ticket(id);
    if (!ticket) { json_object_put(o);
    db_error(r);
    return;
    }
    json_object_object_add(o, "ticket", ticket);
    send_json(r, status, o);
}
static void create_ticket(Request *r, Auth *a) {
    json_object *input = parse_json(r);
    if (!input) return;
    TicketFields f;
    if (!ticket_fields(input, NULL, &f, false)) { json_object_put(input);
    error_response(r, 400, "Ticket non valido: controlla titolo, priorità, assegnatario e scadenza.");
    return;
    }
    if (!exec_sql("BEGIN IMMEDIATE")) { json_object_put(input);
    db_error(r);
    return;
    }
    if (!assignee_active(f.assignee_id)) { exec_sql("ROLLBACK");
    json_object_put(input);
    error_response(r, 400, "L'assegnatario deve essere un utente attivo.");
    return;
    }
    sqlite3_stmt *st = prepare("INSERT INTO tickets(title,description,requester,status,priority,assignee_id,due_date,created_by) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    if (!st) goto fail;
    bind_ticket(st, &f);
    sqlite3_bind_int64(st, 8, a->id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_int64 id = sqlite3_last_insert_rowid(db);
    if (!add_history(id, a, "created", "Ticket creato.") || !exec_sql("COMMIT")) goto fail;
    json_object_put(input);
    ticket_response(r, 201, id);
    return;
fail: exec_sql("ROLLBACK");
json_object_put(input);
db_error(r);
}
static const char *status_label(const char *s) {
    if (!strcmp(s, "open")) return "Aperto";
    if (!strcmp(s, "in_progress")) return "In lavorazione";
    if (!strcmp(s, "waiting")) return "In attesa";
    if (!strcmp(s, "resolved")) return "Risolto";
    return "Chiuso";
}
static const char *priority_label(const char *s) {
    if (!strcmp(s, "low")) return "Bassa";
    if (!strcmp(s, "normal")) return "Normale";
    if (!strcmp(s, "high")) return "Alta";
    return "Urgente";
}
static void append_change(char *detail, size_t cap, const char *label, const char *before, const char *after) {
    size_t len = strlen(detail);
    if (before && after) snprintf(detail + len, cap - len, "%s%s: %s -> %s", len ? "; " : "", label, before, after);
    else snprintf(detail + len, cap - len, "%s%s", len ? "; " : "", label);
}
static void update_ticket(Request *r, Auth *a, sqlite3_int64 id) {
    json_object *input = parse_json(r);
    if (!input) return;
    json_object *revision = NULL;
    if (!json_object_object_get_ex(input, "revision", &revision) || !revision || !json_object_is_type(revision, json_type_int) || json_object_get_int64(revision) < 1 || json_object_get_int64(revision) >= LLONG_MAX) { json_object_put(input);
    error_response(r, 400, "Indica la revisione attuale del ticket.");
    return;
    }
    if (!exec_sql("BEGIN IMMEDIATE")) { json_object_put(input);
    db_error(r);
    return;
    }
    json_object *current = fetch_ticket(id);
    if (!current) { exec_sql("ROLLBACK");
    json_object_put(input);
    error_response(r, 404, "Ticket non trovato.");
    return;
    }
    if (json_object_get_int64(revision) != json_object_get_int64(json_object_object_get(current, "revision"))) { exec_sql("ROLLBACK");
    json_object_put(current);
    json_object_put(input);
    error_response(r, 409, "Il ticket è stato modificato. Ricaricalo prima di salvare.");
    return;
    }
    TicketFields f;
    if (!ticket_fields(input, current, &f, true)) { exec_sql("ROLLBACK");
    json_object_put(current);
    json_object_put(input);
    error_response(r, 400, "Dati del ticket non validi.");
    return;
    }
    /* An inactive previous assignee may be retained; a new assignment must be active. */
    sqlite3_int64 old_assignee = json_object_get_int64(json_object_object_get(current, "assignee_id"));
    if (f.assignee_id != old_assignee && !assignee_active(f.assignee_id)) { exec_sql("ROLLBACK");
    json_object_put(current);
    json_object_put(input);
    error_response(r, 400, "L'assegnatario deve essere un utente attivo.");
    return;
    }
    sqlite3_stmt *st = prepare("UPDATE tickets SET title=?1,description=?2,requester=?3,status=?4,priority=?5,assignee_id=?6,due_date=?7,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),revision=revision+1 WHERE id=?8 AND revision=?9");
    if (!st) goto fail;
    bind_ticket(st, &f);
    sqlite3_bind_int64(st, 8, id);
    sqlite3_bind_int64(st, 9, json_object_get_int64(revision));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1) goto fail;
    char detail[4096] = "";
    const char *const text_keys[] = {"title", "description", "requester"};
    const char *const labels[] = {"titolo aggiornato", "descrizione aggiornata", "richiedente aggiornato"};
    const char *const next_text[] = {f.title, f.description, f.requester};
    for (size_t i = 0; i < 3; i++) {
        const char *old = json_object_get_string(json_object_object_get(current, text_keys[i]));
        if (strcmp(old, next_text[i])) append_change(detail, sizeof(detail), labels[i], NULL, NULL);
    }
    const char *old_status = json_object_get_string(json_object_object_get(current, "status"));
    const char *old_priority = json_object_get_string(json_object_object_get(current, "priority"));
    if (strcmp(old_status, f.status)) append_change(detail, sizeof(detail), "stato", status_label(old_status), status_label(f.status));
    if (strcmp(old_priority, f.priority)) append_change(detail, sizeof(detail), "priorità", priority_label(old_priority), priority_label(f.priority));
    if (old_assignee != f.assignee_id) {
        json_object *next_user = f.assignee_id ? fetch_user(f.assignee_id) : NULL;
        const char *old_name = json_object_get_string(json_object_object_get(current, "assignee_name"));
        const char *next_name = next_user ? json_object_get_string(json_object_object_get(next_user, "name")) : NULL;
        append_change(detail, sizeof(detail), "assegnatario", old_name ? old_name : "Non assegnato", next_name ? next_name : "Non assegnato");
        if (next_user) json_object_put(next_user);
    }
    const char *old_due = json_object_get_string(json_object_object_get(current, "due_date"));
    if ((old_due == NULL) != (f.due_date == NULL) || (old_due && f.due_date && strcmp(old_due, f.due_date))) append_change(detail, sizeof(detail), "scadenza", old_due ? old_due : "Nessuna", f.due_date ? f.due_date : "Nessuna");
    if (!*detail) bounded_copy(detail, sizeof(detail), "Salvataggio senza variazioni.");
    if (!add_history(id, a, "updated", detail) || !exec_sql("COMMIT")) goto fail;
    json_object_put(current);
    json_object_put(input);
    ticket_response(r, 200, id);
    return;
fail: exec_sql("ROLLBACK");
json_object_put(current);
json_object_put(input);
db_error(r);
}
static json_object *comment_json(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_int(o, "ticket_id", sqlite3_column_int64(st, 1));
    add_int(o, "author_id", sqlite3_column_int64(st, 2));
    add_string(o, "author_name", column_text(st, 3));
    add_string(o, "body", column_text(st, 4));
    add_string(o, "created_at", column_text(st, 5));
    return o;
}
static void get_ticket(Request *r, sqlite3_int64 id) {
    json_object *ticket = fetch_ticket(id);
    if (!ticket) { error_response(r, 404, "Ticket non trovato.");
    return;
    }
    json_object *o = json_object_new_object(), *comments = json_object_new_array(), *history = json_object_new_array();
    json_object_object_add(o, "ticket", ticket);
    json_object_object_add(o, "comments", comments);
    json_object_object_add(o, "history", history);
    sqlite3_stmt *st = prepare("SELECT c.id,c.ticket_id,c.author_id,u.name,c.body,c.created_at FROM comments c JOIN users u ON u.id=c.author_id WHERE c.ticket_id=?1 ORDER BY c.id");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) json_object_array_add(comments, comment_json(st));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    st = prepare("SELECT id,ticket_id,actor_name,action,detail,created_at FROM history WHERE ticket_id=?1 ORDER BY id DESC");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { json_object *h = json_object_new_object();
    add_int(h, "id", sqlite3_column_int64(st, 0));
    add_int(h, "ticket_id", sqlite3_column_int64(st, 1));
    add_string(h, "actor_name", column_text(st, 2));
    add_string(h, "action", column_text(st, 3));
    add_string(h, "detail", column_text(st, 4));
    add_string(h, "created_at", column_text(st, 5));
    json_object_array_add(history, h);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    send_json(r, 200, o);
    return;
fail: json_object_put(o);
db_error(r);
}
static void create_comment(Request *r, Auth *a, sqlite3_int64 id) {
    json_object *input = parse_json(r);
    if (!input) return;
    const char *body = NULL;
    const char *const keys[] = {"body", NULL};
    if (!allowed_keys(input, keys) || !get_string(input, "body", &body, 4000, true, true)) { json_object_put(input);
    error_response(r, 400, "Scrivi un commento di massimo 4000 caratteri.");
    return;
    }
    if (!exec_sql("BEGIN IMMEDIATE")) { json_object_put(input);
    db_error(r);
    return;
    }
    sqlite3_stmt *st = prepare("SELECT 1 FROM tickets WHERE id=?1");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) { exec_sql("ROLLBACK");
    json_object_put(input);
    if (rc == SQLITE_DONE) error_response(r, 404, "Ticket non trovato.");
    else db_error(r);
    return;
    }
    st = prepare("INSERT INTO comments(ticket_id,author_id,body) VALUES(?1,?2,?3)");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, a->id);
    bind_text(st, 3, body);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_int64 comment_id = sqlite3_last_insert_rowid(db);
    /* Comments count as activity without invalidating edits to ticket fields. */
    st = prepare("UPDATE tickets SET updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id=?1");
    if (!st) goto fail;
    sqlite3_bind_int64(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto fail;
    if (!add_history(id, a, "commented", "Aggiunto un commento.") || !exec_sql("COMMIT")) goto fail;
    json_object_put(input);
    st = prepare("SELECT c.id,c.ticket_id,c.author_id,u.name,c.body,c.created_at FROM comments c JOIN users u ON u.id=c.author_id WHERE c.id=?1");
    if (!st) { db_error(r);
    return;
    }
    sqlite3_bind_int64(st, 1, comment_id);
    rc = sqlite3_step(st);
    json_object *comment = rc == SQLITE_ROW ? comment_json(st) : NULL;
    sqlite3_finalize(st);
    if (!comment) { db_error(r);
    return;
    } json_object *o = json_object_new_object();
    json_object_object_add(o, "comment", comment);
    send_json(r, 201, o);
    return;
fail: exec_sql("ROLLBACK");
json_object_put(input);
db_error(r);
}
static int hex_digit(char c) { if (c >= '0' && c <= '9') return c - '0';
if (c >= 'a' && c <= 'f') return c - 'a' + 10;
if (c >= 'A' && c <= 'F') return c - 'A' + 10;
return -1;
}
static bool url_decode(const char *raw, char *out, size_t cap, bool plus) {
    size_t len = 0;
    for (size_t i = 0; raw[i]; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%') { if (!raw[i+1] || !raw[i+2]) return false;
        int a = hex_digit(raw[i+1]), b = hex_digit(raw[i+2]);
        if (a < 0 || b < 0) return false;
        c = (unsigned char)((a << 4) | b);
        i += 2;
        }
        else if (plus && c == '+') c = ' ';
        if (!c || c < 32 || c == 127 || len + 1 >= cap) return false;
        out[len++] = (char)c;
    } out[len] = 0;
    return true;
}
typedef struct {
    char search[801], status[32], priority[16], today[11], due_soon_end[11];
    sqlite3_int64 assignee_id;
    int assignee_mode, queue, sort, limit, offset;
} Filters;

static bool utc_calendar_date(char out[11]) {
    time_t now = time(NULL);
    struct tm utc;
    return gmtime_r(&now, &utc) && strftime(out, 11, "%Y-%m-%d", &utc) == 10;
}

static void date_plus_seven(const char *date, char out[11]) {
    int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
        (date[2] - '0') * 10 + date[3] - '0';
    int month = (date[5] - '0') * 10 + date[6] - '0';
    int day = (date[8] - '0') * 10 + date[9] - '0';
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    for (int i = 0; i < 7; i++) {
        if (year == 9999 && month == 12 && day == 31) break;
        int last = days[month];
        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) last++;
        if (++day > last) {
            day = 1;
            if (++month > 12) {
                month = 1;
                year++;
            }
        }
    }
    /* The validated date and clamp keep every digit inside the ISO range. */
    out[0] = (char)('0' + year / 1000);
    out[1] = (char)('0' + year / 100 % 10);
    out[2] = (char)('0' + year / 10 % 10);
    out[3] = (char)('0' + year % 10);
    out[4] = '-';
    out[5] = (char)('0' + month / 10);
    out[6] = (char)('0' + month % 10);
    out[7] = '-';
    out[8] = (char)('0' + day / 10);
    out[9] = (char)('0' + day % 10);
    out[10] = 0;
}

static int enum_index(const char *value, const char *const choices[]) {
    for (int i = 0; choices[i]; i++) if (!strcmp(value, choices[i])) return i;
    return -1;
}

static bool read_filters(const char *query, Filters *f) {
    const char *const queues[] = {"all", "active", "mine", "overdue", "due_soon", "unassigned", NULL};
    const char *const sorts[] = {"updated", "priority", "due", NULL};
    memset(f, 0, sizeof(*f));
    f->limit = 50;
    if (!utc_calendar_date(f->today)) return false;
    if (query && *query) {
        char copy[2048];
        if (strlen(query) >= sizeof(copy)) return false;
        bounded_copy(copy, sizeof(copy), query);
        char *save = NULL;
        unsigned seen = 0;
        for (char *p = strtok_r(copy, "&", &save); p; p = strtok_r(NULL, "&", &save)) {
            char *equals = strchr(p, '=');
            if (!equals) return false;
            *equals++ = 0;
            char key[40], value[1024];
            if (!url_decode(p, key, sizeof(key), true) || !url_decode(equals, value, sizeof(value), true)) return false;
            unsigned bit = 0;
            long long number;
            if (!strcmp(key, "search")) {
                bit = 1;
                if (utf8_length(value) > 200) return false;
                bounded_copy(f->search, sizeof(f->search), value);
            } else if (!strcmp(key, "status")) {
                bit = 2;
                if (*value && !valid_status(value)) return false;
                bounded_copy(f->status, sizeof(f->status), value);
            } else if (!strcmp(key, "priority")) {
                bit = 4;
                if (*value && !valid_priority(value)) return false;
                bounded_copy(f->priority, sizeof(f->priority), value);
            } else if (!strcmp(key, "assignee_id")) {
                bit = 8;
                if (!strcmp(value, "unassigned")) {
                    f->assignee_mode = 2;
                } else if (*value) {
                    if (!parse_uint(value, LLONG_MAX, &number) || number <= 0) return false;
                    f->assignee_mode = 1;
                    f->assignee_id = number;
                }
            } else if (!strcmp(key, "limit")) {
                bit = 16;
                if (!parse_uint(value, 100, &number) || number < 1) return false;
                f->limit = (int)number;
            } else if (!strcmp(key, "offset")) {
                bit = 32;
                if (!parse_uint(value, 1000000, &number)) return false;
                f->offset = (int)number;
            } else if (!strcmp(key, "queue")) {
                bit = 64;
                f->queue = enum_index(value, queues);
                if (f->queue < 0) return false;
            } else if (!strcmp(key, "sort")) {
                bit = 128;
                f->sort = enum_index(value, sorts);
                if (f->sort < 0) return false;
            } else if (!strcmp(key, "today")) {
                bit = 256;
                if (!valid_date(value)) return false;
                bounded_copy(f->today, sizeof(f->today), value);
            } else {
                return false;
            }
            if (seen & bit) return false;
            seen |= bit;
        }
    }
    date_plus_seven(f->today, f->due_soon_end);
    return true;
}

#define ACTIVE_SQL "t.status IN('open','in_progress','waiting')"
#define FILTER_SQL \
    " WHERE (?1 IS NULL OR t.title LIKE ?1 ESCAPE '\\' OR t.description LIKE ?1 ESCAPE '\\' " \
    "OR t.requester LIKE ?1 ESCAPE '\\' OR CAST(t.id AS TEXT) LIKE ?1 ESCAPE '\\') " \
    "AND (?2 IS NULL OR t.status=?2) AND (?3 IS NULL OR t.priority=?3) " \
    "AND (?4=0 OR (?4=1 AND t.assignee_id=?5) OR (?4=2 AND t.assignee_id IS NULL)) " \
    "AND (?6=0 OR (" ACTIVE_SQL " AND (?6=1 OR (?6=2 AND t.assignee_id=?7) " \
    "OR (?6=3 AND t.due_date<?8) OR (?6=4 AND t.due_date BETWEEN ?8 AND ?9) " \
    "OR (?6=5 AND t.assignee_id IS NULL)))) "
#define PRIORITY_ORDER_SQL "CASE t.priority WHEN 'urgent' THEN 0 WHEN 'high' THEN 1 WHEN 'normal' THEN 2 ELSE 3 END"

static void bind_filters(sqlite3_stmt *st, Filters *f, Auth *a) {
    char pattern[1604];
    size_t n = 0;
    pattern[n++] = '%';
    for (const char *p = f->search; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') pattern[n++] = '\\';
        pattern[n++] = *p;
    }
    pattern[n++] = '%';
    pattern[n] = 0;
    bind_text(st, 1, *f->search ? pattern : NULL);
    bind_text(st, 2, *f->status ? f->status : NULL);
    bind_text(st, 3, *f->priority ? f->priority : NULL);
    sqlite3_bind_int(st, 4, f->assignee_mode);
    sqlite3_bind_int64(st, 5, f->assignee_id);
    sqlite3_bind_int(st, 6, f->queue);
    sqlite3_bind_int64(st, 7, a->id);
    bind_text(st, 8, f->today);
    bind_text(st, 9, f->due_soon_end);
}

static void list_tickets(Request *r, Auth *a) {
    static const char *const order_sql[] = {
        "ORDER BY t.updated_at DESC,t.id DESC ",
        "ORDER BY " PRIORITY_ORDER_SQL ",t.due_date IS NULL,t.due_date ASC,t.updated_at DESC,t.id DESC ",
        "ORDER BY t.due_date IS NULL,t.due_date ASC," PRIORITY_ORDER_SQL ",t.updated_at DESC,t.id DESC "
    };
    Filters f;
    if (!read_filters(r->query, &f)) {
        error_response(r, 400, "Filtri o paginazione non validi.");
        return;
    }
    json_object *o = json_object_new_object();
    json_object *items = json_object_new_array();
    json_object *stats = json_object_new_object();
    json_object *queues = json_object_new_object();
    json_object_object_add(o, "items", items);
    json_object_object_add(o, "stats", stats);
    json_object_object_add(o, "queues", queues);
    add_string(o, "reference_date", f.today);
    const char *const states[] = {"open", "in_progress", "waiting", "resolved", "closed"};
    const char *const queue_keys[] = {"active", "mine", "overdue", "due_soon", "unassigned"};
    for (size_t i = 0; i < 5; i++) add_int(stats, states[i], 0);
    /* Every count and row uses one database snapshot, independent of filtering. */
    if (!exec_sql("BEGIN")) goto fail;
    sqlite3_stmt *st = prepare("SELECT status,count(*) FROM tickets GROUP BY status");
    if (!st) goto rollback;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) add_int(stats, column_text(st, 0), sqlite3_column_int64(st, 1));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto rollback;
    st = prepare("SELECT count(*),coalesce(sum(t.assignee_id=?1),0),"
        "coalesce(sum(t.due_date<?2),0),coalesce(sum(t.due_date BETWEEN ?2 AND ?3),0),"
        "coalesce(sum(t.assignee_id IS NULL),0) FROM tickets t WHERE " ACTIVE_SQL);
    if (!st) goto rollback;
    sqlite3_bind_int64(st, 1, a->id);
    bind_text(st, 2, f.today);
    bind_text(st, 3, f.due_soon_end);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) for (int i = 0; i < 5; i++) add_int(queues, queue_keys[i], sqlite3_column_int64(st, i));
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) goto rollback;
    st = prepare("SELECT count(*) FROM tickets t " FILTER_SQL);
    if (!st) goto rollback;
    bind_filters(st, &f, a);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) add_int(o, "total", sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) goto rollback;
    char sql[4096];
    int length = snprintf(sql, sizeof(sql), "SELECT " TICKET_COLUMNS TICKET_FROM FILTER_SQL "%sLIMIT ?10 OFFSET ?11", order_sql[f.sort]);
    if (length < 0 || (size_t)length >= sizeof(sql)) goto rollback;
    st = prepare(sql);
    if (!st) goto rollback;
    bind_filters(st, &f, a);
    sqlite3_bind_int(st, 10, f.limit);
    sqlite3_bind_int(st, 11, f.offset);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) json_object_array_add(items, ticket_json(st));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) goto rollback;
    if (!exec_sql("COMMIT")) goto rollback;
    send_json(r, 200, o);
    return;
rollback:
    exec_sql("ROLLBACK");
fail:
    json_object_put(o);
    db_error(r);
}
static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".js")) return "text/javascript; charset=utf-8";
    if (!strcasecmp(ext, ".svg")) return "image/svg+xml";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".webp")) return "image/webp";
    if (!strcasecmp(ext, ".ico")) return "image/x-icon";
    if (!strcasecmp(ext, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}
static void static_file(Request *r) {
    char decoded[2048], base[PATH_MAX], candidate[PATH_MAX], resolved[PATH_MAX];
    if (!url_decode(r->path, decoded, sizeof(decoded), false) || strchr(decoded, '\\')) { error_response(r, 400, "Percorso non valido.");
    return;
    }
    for (char *p = decoded; *p; p++) if (*p == '/' && p[1] == '.') { error_response(r, 404, "Pagina non trovata.");
    return;
    }
    if (!realpath(public_dir, base)) { error_response(r, 404, "Interfaccia non disponibile.");
    return;
    }
    const char *relative = !strcmp(decoded, "/") ? "/index.html" : decoded;
    int n = snprintf(candidate, sizeof(candidate), "%s%s", base, relative);
    if (n < 0 || (size_t)n >= sizeof(candidate) || !realpath(candidate, resolved) || strncmp(resolved, base, strlen(base)) || resolved[strlen(base)] != '/') { error_response(r, 404, "Pagina non trovata.");
    return;
    }
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) { error_response(r, 404, "Pagina non trovata.");
    return;
    }
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 5 * 1024 * 1024) { close(fd);
    error_response(r, 404, "Pagina non trovata.");
    return;
    }
    size_t len = (size_t)info.st_size;
    char *data = malloc(len ? len : 1);
    if (!data) { close(fd);
    db_error(r);
    return;
    }
    size_t used = 0;
    while (used < len) { ssize_t count = read(fd, data + used, len - used);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    used += (size_t)count;
    }
    close(fd);
    if (used != len) { free(data);
    error_response(r, 500, "Impossibile leggere la pagina.");
    return;
    }
    response(r, 200, mime_type(resolved), data, len, NULL);
    free(data);
}
static bool path_id(const char *path, const char *prefix, sqlite3_int64 *id, const char **suffix) {
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len)) return false;
    const char *p = path + len, *end = p;
    while (isdigit((unsigned char)*end)) end++;
    if (p == end || (*end && *end != '/')) return false;
    char digits[24];
    size_t n = (size_t)(end - p);
    long long value;
    if (n >= sizeof(digits)) return false;
    memcpy(digits, p, n);
    digits[n] = 0;
    if (!parse_uint(digits, LLONG_MAX, &value) || value <= 0) return false;
    *id = value;
    *suffix = end;
    return true;
}
static void dispatch(Request *r) {
    bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST"), put = !strcmp(r->method, "PUT"), mutation = post || put;
    if (!get && !mutation) { error_response(r, 405, "Metodo non consentito.");
    return;
    }
    if (mutation && !origin_allowed(r)) { error_response(r, 403, "Origine della richiesta non consentita.");
    return;
    }
    if (!strcmp(r->path, "/healthz")) { if (!get) { error_response(r, 405, "Metodo non consentito.");
    return;
    } json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(true));
    send_json(r, 200, o);
    return;
    }
    if (!strcmp(r->path, "/api/login")) { if (post) login(r);
    else error_response(r, 405, "Metodo non consentito.");
    return;
    }
    if (strncmp(r->path, "/api/", 5)) { if (get) static_file(r);
    else error_response(r, 405, "Metodo non consentito.");
    return;
    }
    Auth a = {0};
    if (!authenticate(r, &a)) { error_response(r, 401, "Accedi per continuare.");
    return;
    }
    if (mutation && (!*r->csrf || !constant_equal(r->csrf, a.csrf))) { error_response(r, 403, "Token di sicurezza non valido. Ricarica la pagina.");
    return;
    }
    if (!strcmp(r->path, "/api/session")) { if (get) session_response(r, &a, NULL);
    else error_response(r, 405, "Metodo non consentito.");
    return;
    }
    if (!strcmp(r->path, "/api/logout")) { if (post) logout(r, &a);
    else error_response(r, 405, "Metodo non consentito.");
    return;
    }
    if (!strcmp(r->path, "/api/users")) { if (get) list_users(r, &a);
    else if (post && !strcmp(a.role, "admin")) create_user(r);
    else error_response(r, post ? 403 : 405, post ? "Sono richiesti i permessi di amministratore." : "Metodo non consentito.");
    return;
    }
    if (!strcmp(r->path, "/api/tickets")) { if (get) list_tickets(r, &a);
    else if (post) create_ticket(r, &a);
    else error_response(r, 405, "Metodo non consentito.");
    return;
    }
    sqlite3_int64 id;
    const char *suffix;
    if (path_id(r->path, "/api/users/", &id, &suffix) && !*suffix) { if (put && !strcmp(a.role, "admin")) update_user(r, id);
    else error_response(r, put ? 403 : 405, put ? "Sono richiesti i permessi di amministratore." : "Metodo non consentito.");
    return;
    }
    if (path_id(r->path, "/api/tickets/", &id, &suffix)) {
        if (!*suffix) { if (get) get_ticket(r, id);
        else if (put) update_ticket(r, &a, id);
        else error_response(r, 405, "Metodo non consentito.");
        return;
        }
        if (!strcmp(suffix, "/comments")) { if (post) create_comment(r, &a, id);
        else error_response(r, 405, "Metodo non consentito.");
        return;
        }
    }
    error_response(r, 404, "Risorsa non trovata.");
}
static bool create_parent_dirs(const char *path) {
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) return false;
    bounded_copy(copy, sizeof(copy), path);
    for (char *p = copy + 1; *p; p++) if (*p == '/') { *p = 0;
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) return false;
    *p = '/';
    }
    return true;
}
static bool init_db(const char *path) {
    if (!create_parent_dirs(path) || sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) { fprintf(stderr, "Impossibile aprire l'archivio ticket.\n");
    return false;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *version = prepare("PRAGMA user_version");
    if (!version) return false;
    int version_rc = sqlite3_step(version);
    int schema_version = version_rc == SQLITE_ROW ? sqlite3_column_int(version, 0) : -1;
    sqlite3_finalize(version);
    if (schema_version != 0 && schema_version != 1) {
        fprintf(stderr, "Versione archivio non supportata: %d. Usa un archivio dedicato ai ticket.\n", schema_version);
        return false;
    }
    if (!exec_sql("PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,email TEXT NOT NULL COLLATE NOCASE UNIQUE,role TEXT NOT NULL CHECK(role IN('admin','agent')),active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),password_salt TEXT NOT NULL,password_hash TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
        "DROP TABLE IF EXISTS main.sessions; CREATE TEMP TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY,user_id INTEGER NOT NULL,csrf TEXT NOT NULL,expires_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS sessions_user_idx ON sessions(user_id);"
        "CREATE TABLE IF NOT EXISTS tickets(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,description TEXT NOT NULL DEFAULT '',requester TEXT NOT NULL DEFAULT '',status TEXT NOT NULL DEFAULT 'open' CHECK(status IN('open','in_progress','waiting','resolved','closed')),priority TEXT NOT NULL DEFAULT 'normal' CHECK(priority IN('low','normal','high','urgent')),assignee_id INTEGER REFERENCES users(id),created_by INTEGER NOT NULL REFERENCES users(id),due_date TEXT,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),revision INTEGER NOT NULL DEFAULT 1);"
        "CREATE INDEX IF NOT EXISTS tickets_updated_idx ON tickets(updated_at DESC,id DESC);"
        "CREATE INDEX IF NOT EXISTS tickets_filter_idx ON tickets(status,priority,assignee_id);"
        "CREATE TABLE IF NOT EXISTS comments(id INTEGER PRIMARY KEY AUTOINCREMENT,ticket_id INTEGER NOT NULL REFERENCES tickets(id),author_id INTEGER NOT NULL REFERENCES users(id),body TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
        "CREATE INDEX IF NOT EXISTS comments_ticket_idx ON comments(ticket_id,id);"
        "CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY AUTOINCREMENT,ticket_id INTEGER NOT NULL REFERENCES tickets(id),actor_id INTEGER NOT NULL REFERENCES users(id),actor_name TEXT NOT NULL,action TEXT NOT NULL,detail TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
        "CREATE INDEX IF NOT EXISTS history_ticket_idx ON history(ticket_id,id); PRAGMA user_version=1; COMMIT;")) return false;
    sqlite3_stmt *st = prepare("SELECT count(*) FROM users");
    if (!st) return false;
    int rc = sqlite3_step(st);
    bool empty = rc == SQLITE_ROW && sqlite3_column_int64(st, 0) == 0;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) return false;
    if (!empty) return true;
    const char *email = getenv("TICKETS_EMAIL"), *password = getenv("TICKETS_PASSWORD"), *name = env_or("TICKETS_NAME", "Gestore");
    char normalized[255], salt[33], hash[65];
    if (!email || !password || !email_normalize(email, normalized) || !valid_password(password) || !*name || whitespace_only(name) || utf8_length(name) > 100) { fprintf(stderr, "Primo avvio: imposta TICKETS_EMAIL e TICKETS_PASSWORD (almeno 12 caratteri); TICKETS_NAME è facoltativo.\n");
    return false;
    }
    if (!random_hex(salt, 16) || !password_hash(password, salt, hash)) return false;
    /* Recheck under a write lock in case another process initialized this DB. */
    if (!exec_sql("BEGIN IMMEDIATE")) return false;
    st = prepare("INSERT INTO users(name,email,role,password_salt,password_hash) SELECT ?1,?2,'admin',?3,?4 WHERE NOT EXISTS(SELECT 1 FROM users)");
    if (!st) { exec_sql("ROLLBACK");
    return false;
    }
    bind_text(st, 1, name);
    bind_text(st, 2, normalized);
    bind_text(st, 3, salt);
    bind_text(st, 4, hash);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    OPENSSL_cleanse(hash, sizeof(hash));
    if (rc != SQLITE_DONE || !exec_sql("COMMIT")) { exec_sql("ROLLBACK");
    return false;
    }
    return true;
}
static bool valid_authority(const char *authority) {
    char host[256];
    bounded_copy(host, sizeof(host), authority);
    char *port = NULL;
    if (*host == '[') {
        char *end = strchr(host, ']');
        if (!end || (end[1] && end[1] != ':')) return false;
        *end = 0;
        struct in6_addr addr;
        if (inet_pton(AF_INET6, host + 1, &addr) != 1) return false;
        if (end[1] == ':') port = end + 2;
    } else {
        char *colon = strchr(host, ':');
        if (colon) { *colon = 0;
        port = colon + 1;
        }
        size_t len = strlen(host);
        if (!len || len > 253 || host[0] == '.' || host[len - 1] == '.' || host[0] == '-' || host[len - 1] == '-') return false;
        size_t label_len = 0;
        for (const char *p = host; *p; p++) {
            if (*p == '.') {
                if (!label_len || p[-1] == '-' || p[1] == '-') return false;
                label_len = 0;
            } else {
                if (!(isalnum((unsigned char)*p) || *p == '-') || ++label_len > 63) return false;
            }
        }
    }
    long long number;
    return !port || (parse_uint(port, 65535, &number) && number > 0);
}
static bool configure_origin(void) {
    const char *origin = getenv("TICKETS_PUBLIC_ORIGIN");
    if (!origin || !*origin) return true;
    size_t prefix = !strncmp(origin, "https://", 8) ? 8 : !strncmp(origin, "http://", 7) ? 7 : 0;
    if (!prefix || strlen(origin) >= sizeof(public_origin) || !origin[prefix] || strlen(origin + prefix) >= sizeof(public_authority) || strpbrk(origin + prefix, "/?#@\\ \t\r\n")) return false;
    for (const char *p = origin + prefix; *p; p++) if ((unsigned char)*p < 33 || (unsigned char)*p >= 127) return false;
    if (!valid_authority(origin + prefix)) return false;
    bounded_copy(public_origin, sizeof(public_origin), origin);
    bounded_copy(public_authority, sizeof(public_authority), origin + prefix);
    secure_cookie = prefix == 8;
    return true;
}
static int open_listener(const char *bind_address) {
    char port[16];
    snprintf(port, sizeof(port), "%d", configured_port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo(bind_address, port, &hints, &addresses);
    if (rc) { fprintf(stderr, "TICKETS_BIND deve essere un indirizzo IP numerico: %s\n", gai_strerror(rc));
    return -1;
    }
    int fd = -1;
    for (struct addrinfo *p = addresses; p; p = p->ai_next) { fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (p->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        if (!bind(fd, p->ai_addr, p->ai_addrlen) && !listen(fd, 64)) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}
int main(void) {
    umask(0077);
    long long port;
    if (!parse_uint(env_or("TICKETS_PORT", "8083"), 65535, &port) || port < 1) { fprintf(stderr, "TICKETS_PORT non valida.\n");
    return 1;
    } configured_port = (int)port;
    if (!configure_origin()) { fprintf(stderr, "TICKETS_PUBLIC_ORIGIN deve essere una singola origine http(s), senza percorso o credenziali.\n");
    return 1;
    }
    public_dir = env_or("TICKETS_PUBLIC_DIR", "public");
    if (!init_db(env_or("TICKETS_DB_PATH", "data/tickets.db"))) { if (db) sqlite3_close(db);
    return 1;
    }
    /* Bootstrap material is no longer needed after initialization. */
    unsetenv("TICKETS_PASSWORD");
    const char *bind_address = env_or("TICKETS_BIND", "127.0.0.1");
    listener = open_listener(bind_address);
    if (listener < 0) { perror("Ascolto ticket");
    sqlite3_close(db);
    return 1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stdout, "Punto Ticket in ascolto su %s:%d\n", bind_address, configured_port);
    fflush(stdout);
    while (!stopping) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (stopping) break;
        if (errno == EINTR) continue;
        perror("accept");
        break;
        }
        struct timeval timeout = {8,0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        Request request;
        memset(&request, 0, sizeof(request));
        request.fd = fd;
        int status = read_request(&request);
        if (status) error_response(&request, status, status == 413 ? "Richiesta troppo grande." : "Richiesta HTTP non valida.");
        else dispatch(&request);
        if (request.body) { OPENSSL_cleanse(request.body, request.body_len);
        free(request.body);
        }
        close(fd);
    }
    if (listener >= 0) close(listener);
    sqlite3_close(db);
    return 0;
}
