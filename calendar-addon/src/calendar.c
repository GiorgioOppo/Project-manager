/* Private Google Calendar synchronizer. OAuth and event payloads are authenticated ciphertext. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HEADER_LIMIT 8192
#define BODY_LIMIT 65536
#define RESPONSE_LIMIT (4 * 1024 * 1024)
#define TOKEN_LIMIT 4096
#define PAGE_LIMIT 100
#define EVENT_LIMIT 20000
#define SCOPE                                                                                      \
    "https://www.googleapis.com/auth/calendar.events "                                             \
    "https://www.googleapis.com/auth/calendar.calendarlist.readonly"
typedef struct {
    char token[129], client_secret[TOKEN_LIMIT + 1];
    unsigned char encryption_key[32];
    const char *bind_address, *allowed_client, *cert, *key, *database;
    const char *client_id, *redirect_uri, *proxy, *google_ca;
    char api_origin[256], token_url[300], revoke_url[300], auth_url[300];
    int port, interval, timeout;
    bool configured, test_mode;
} Config;
typedef struct {
    char method[12], path[4096], authorization[256], content_type[128];
    char body[BODY_LIMIT + 1];
    size_t length;
} Request;
typedef struct {
    char *data;
    size_t length;
} Buffer;
typedef struct {
    long status;
    CURLcode transport;
    json_object *json;
} Http;
typedef struct {
    char *key, *value;
} Field;
typedef struct {
    Field fields[12];
    int count;
} Form;
static Config config;
static _Thread_local sqlite3 *database;
static _Thread_local bool worker_connection;
static _Atomic sig_atomic_t stopping;
static _Thread_local const char *expected_provider;
static _Thread_local const char *expected_generation;
static sqlite3_int64 worker_cursor;

static const char *env_value(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}
static bool number(const char *text, unsigned long min, unsigned long max, int *result) {
    if (!text || !*text)
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!isdigit(*p))
            return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 10);
    if (errno || !end || *end || v < min || v > max)
        return false;
    *result = (int)v;
    return true;
}
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool hex_string(const char *s, size_t n) {
    if (strlen(s) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (hex_value((unsigned char)s[i]) < 0)
            return false;
    return true;
}
static bool secret(const char *path, char *out, size_t cap, bool empty_allowed) {
    if (!path || !*path)
        return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok =
        !fstat(fd, &st) && S_ISREG(st.st_mode) && !(st.st_mode & 0022) && st.st_size < (off_t)cap;
    size_t used = 0;
    while (ok && used < cap - 1) {
        ssize_t n = read(fd, out + used, cap - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        used += (size_t)n;
    }
    close(fd);
    if (!ok || memchr(out, 0, used))
        return false;
    while (used && (out[used - 1] == '\n' || out[used - 1] == '\r'))
        used--;
    out[used] = 0;
    return used || empty_allowed;
}
static bool loopback_origin(const char *url) {
    const char *p = NULL;
    if (!strncmp(url, "https://127.0.0.1:", 18))
        p = url + 18;
    else if (!strncmp(url, "https://localhost:", 18))
        p = url + 18;
    int port;
    return p && number(p, 1, 65535, &port);
}
static bool valid_redirect(const char *url) {
    if (strlen(url) > 512 || strchr(url, '#') || strchr(url, '?') || strchr(url, '@') ||
        strchr(url, '\\'))
        return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p > 126)
            return false;
    CURLU *parsed = curl_url();
    if (!parsed)
        return false;
    char *scheme = NULL, *host = NULL, *path = NULL;
    bool valid = curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_PATH, &path, 0) == CURLUE_OK;
    valid =
        valid && *host && !strcmp(path, "/api/calendar/oauth/callback") &&
        (!strcmp(scheme, "https") ||
         (!strcmp(scheme, "http") && (!strcmp(host, "localhost") || !strcmp(host, "127.0.0.1"))));
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    curl_url_cleanup(parsed);
    return valid;
}
static bool load_config(void) {
    memset(&config, 0, sizeof(config));
    config.bind_address = env_value("CALENDAR_BIND", "172.30.70.2");
    config.allowed_client = env_value("CALENDAR_ALLOWED_CLIENT", "172.30.70.10");
    config.cert = env_value("CALENDAR_TLS_CERT_FILE", "/etc/punto/tls/calendar-addon.crt");
    config.key = env_value("CALENDAR_TLS_KEY_FILE", "/etc/punto/tls/calendar-addon.key");
    config.database = env_value("CALENDAR_DB_PATH", "/var/lib/punto-calendar/calendar.sqlite3");
    config.client_id = env_value("GOOGLE_CLIENT_ID", "");
    config.redirect_uri = env_value("GOOGLE_REDIRECT_URI", "");
    config.proxy = env_value("CALENDAR_PROXY_URL", "http://squid:3128");
    config.google_ca = env_value("CALENDAR_GOOGLE_CA_FILE", "");
    config.test_mode = !strcmp(env_value("CALENDAR_TEST_MODE", "0"), "1");
    char key_hex[66];
    if (!number(env_value("CALENDAR_PORT", "8091"), 1, 65535, &config.port) ||
        !number(env_value("CALENDAR_SYNC_INTERVAL", "300"), config.test_mode ? 1 : 30, 86400,
                &config.interval) ||
        !number(env_value("CALENDAR_HTTP_TIMEOUT", "10"), 1, 60, &config.timeout) ||
        !secret(env_value("CALENDAR_API_TOKEN_FILE", ""), config.token, sizeof(config.token),
                false) ||
        strlen(config.token) < 32 ||
        !secret(env_value("CALENDAR_TOKEN_KEY_FILE", ""), key_hex, sizeof(key_hex), false) ||
        !hex_string(key_hex, 64))
        return false;
    for (const unsigned char *p = (const unsigned char *)config.token; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return false;
    for (int i = 0; i < 32; i++)
        config.encryption_key[i] = (unsigned char)(hex_value((unsigned char)key_hex[i * 2]) * 16 +
                                                   hex_value((unsigned char)key_hex[i * 2 + 1]));
    OPENSSL_cleanse(key_hex, sizeof(key_hex));
    const char *secret_file = env_value("GOOGLE_CLIENT_SECRET_FILE", "");
    if (*secret_file &&
        !secret(secret_file, config.client_secret, sizeof(config.client_secret), true))
        return false;
    config.configured = *config.client_id && *config.client_secret && *config.redirect_uri;
    if ((*config.client_id || *config.client_secret || *config.redirect_uri) && !config.configured)
        return false;
    if (config.configured &&
        (!valid_redirect(config.redirect_uri) || strlen(config.client_id) > 250))
        return false;
    for (const unsigned char *p = (const unsigned char *)config.client_id; *p; p++)
        if (*p <= 32 || *p > 126)
            return false;
    const char *origin = env_value("CALENDAR_GOOGLE_ORIGIN", "");
    if (*origin) {
        if (!config.test_mode || !loopback_origin(origin) ||
            strcmp(config.bind_address, "127.0.0.1") ||
            strncmp(config.proxy, "http://127.0.0.1:", 17))
            return false;
        int port;
        if (!number(config.proxy + 17, 1, 65535, &port))
            return false;
        snprintf(config.api_origin, sizeof(config.api_origin), "%s", origin);
        snprintf(config.token_url, sizeof(config.token_url), "%s/token", origin);
        snprintf(config.revoke_url, sizeof(config.revoke_url), "%s/revoke", origin);
        snprintf(config.auth_url, sizeof(config.auth_url), "%s/auth", origin);
    } else {
        if (strcmp(config.proxy, "http://squid:3128"))
            return false;
        strcpy(config.api_origin, "https://www.googleapis.com");
        strcpy(config.token_url, "https://oauth2.googleapis.com/token");
        strcpy(config.revoke_url, "https://oauth2.googleapis.com/revoke");
        strcpy(config.auth_url, "https://accounts.google.com/o/oauth2/v2/auth");
    }
    return true;
}
/* json-c accepts duplicate object members by default; reject ambiguity before use. */
static bool unique_json_members(const char *text, size_t length) {
    typedef struct {
        char **keys;
        size_t count;
    } Members;
    Members stack[25] = {0};
    int depth = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < length; i++) {
        if (text[i] == '{' || text[i] == '[') {
            if (++depth >= 25) {
                ok = false;
                break;
            }
        } else if (text[i] == '}' || text[i] == ']') {
            if (depth <= 0) {
                ok = false;
                break;
            }
            for (size_t k = 0; k < stack[depth].count; k++)
                free(stack[depth].keys[k]);
            free(stack[depth].keys);
            memset(&stack[depth], 0, sizeof(stack[depth]));
            depth--;
        } else if (text[i] == '"') {
            size_t start = i++;
            while (i < length) {
                if (text[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (text[i] == '"')
                    break;
                i++;
            }
            if (i >= length) {
                ok = false;
                break;
            }
            size_t n = i - start + 1;
            char *literal = malloc(n + 1);
            if (!literal) {
                ok = false;
                break;
            }
            memcpy(literal, text + start, n);
            literal[n] = 0;
            json_object *value = json_tokener_parse(literal);
            free(literal);
            if (!value || !json_object_is_type(value, json_type_string) ||
                strlen(json_object_get_string(value)) !=
                    (size_t)json_object_get_string_len(value)) {
                if (value)
                    json_object_put(value);
                ok = false;
                break;
            }
            size_t next = i + 1;
            while (next < length && isspace((unsigned char)text[next]))
                next++;
            if (next < length && text[next] == ':') {
                if (depth <= 0 || stack[depth].count >= 128) {
                    json_object_put(value);
                    ok = false;
                    break;
                }
                const char *key = json_object_get_string(value);
                for (size_t k = 0; k < stack[depth].count; k++)
                    if (!strcmp(stack[depth].keys[k], key)) {
                        ok = false;
                        break;
                    }
                if (ok) {
                    char *copy = strdup(key);
                    char **keys =
                        copy ? realloc(stack[depth].keys, (stack[depth].count + 1) * sizeof(char *))
                             : NULL;
                    if (!keys) {
                        free(copy);
                        ok = false;
                    } else {
                        stack[depth].keys = keys;
                        stack[depth].keys[stack[depth].count++] = copy;
                    }
                }
            }
            json_object_put(value);
        }
    }
    for (int d = 0; d < 25; d++) {
        for (size_t k = 0; k < stack[d].count; k++)
            free(stack[d].keys[k]);
        free(stack[d].keys);
    }
    return ok && depth == 0;
}
static json_object *parse_json(const char *text, size_t length) {
    if (!length || length > RESPONSE_LIMIT || memchr(text, 0, length))
        return NULL;
    json_tokener *tok = json_tokener_new_ex(24);
    if (!tok)
        return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *obj = json_tokener_parse_ex(tok, text, (int)length);
    size_t end = json_tokener_get_parse_end(tok);
    while (end < length && isspace((unsigned char)text[end]))
        end++;
    bool ok = json_tokener_get_error(tok) == json_tokener_success && end == length;
    if (ok)
        ok = unique_json_members(text, length);
    json_tokener_free(tok);
    if (!ok) {
        if (obj)
            json_object_put(obj);
        return NULL;
    }
    return obj;
}
static const char *jstr(json_object *obj, const char *name) {
    json_object *v = NULL;
    return obj && json_object_object_get_ex(obj, name, &v) &&
                   json_object_is_type(v, json_type_string)
               ? json_object_get_string(v)
               : "";
}
static sqlite3_int64 jint(json_object *obj, const char *name) {
    json_object *v = NULL;
    return obj && json_object_object_get_ex(obj, name, &v) && json_object_is_type(v, json_type_int)
               ? json_object_get_int64(v)
               : 0;
}
static void addstr(json_object *o, const char *name, const char *v) {
    json_object_object_add(o, name, json_object_new_string(v ? v : ""));
}
static void addint(json_object *o, const char *name, sqlite3_int64 v) {
    json_object_object_add(o, name, json_object_new_int64(v));
}
static void addbool(json_object *o, const char *name, bool v) {
    json_object_object_add(o, name, json_object_new_boolean(v));
}
static json_object *error_json(const char *error) {
    json_object *o = json_object_new_object();
    addstr(o, "error", error);
    return o;
}
static void digest(const char *text, char out[65]) {
    unsigned char bytes[32];
    unsigned int n = 0;
    EVP_Digest(text, strlen(text), bytes, &n, EVP_sha256(), NULL);
    for (size_t i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
}
static char *seal(sqlite3_int64 user, const char *purpose, const char *plain, int *size) {
    size_t n = strlen(plain);
    if (n > RESPONSE_LIMIT)
        return NULL;
    unsigned char *out = malloc(n + 29);
    if (!out)
        return NULL;
    out[0] = 1;
    char aad[4096];
    int aad_n = snprintf(aad, sizeof(aad), "punto-calendar:v1:%lld:%s", (long long)user, purpose);
    if (aad_n < 0 || (size_t)aad_n >= sizeof(aad)) {
        free(out);
        return NULL;
    }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int used = 0, total = 0;
    bool ok = ctx && RAND_bytes(out + 1, 12) == 1 &&
              EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
              EVP_EncryptInit_ex(ctx, NULL, NULL, config.encryption_key, out + 1) == 1 &&
              EVP_EncryptUpdate(ctx, NULL, &used, (const unsigned char *)aad, aad_n) == 1 &&
              EVP_EncryptUpdate(ctx, out + 29, &used, (const unsigned char *)plain, (int)n) == 1;
    total = used;
    if (ok)
        ok = EVP_EncryptFinal_ex(ctx, out + 29 + total, &used) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + 13) == 1;
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        free(out);
        return NULL;
    }
    *size = 29 + total + used;
    return (char *)out;
}
static char *unseal(sqlite3_int64 user, const char *purpose, const void *blob, int size) {
    if (!blob || size < 29 || ((const unsigned char *)blob)[0] != 1)
        return NULL;
    const unsigned char *in = blob;
    char *out = malloc((size_t)size + 1);
    if (!out)
        return NULL;
    char aad[4096];
    int aad_n = snprintf(aad, sizeof(aad), "punto-calendar:v1:%lld:%s", (long long)user, purpose);
    if (aad_n < 0 || (size_t)aad_n >= sizeof(aad)) {
        free(out);
        return NULL;
    }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int used = 0, total = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
              EVP_DecryptInit_ex(ctx, NULL, NULL, config.encryption_key, in + 1) == 1 &&
              EVP_DecryptUpdate(ctx, NULL, &used, (const unsigned char *)aad, aad_n) == 1 &&
              EVP_DecryptUpdate(ctx, (unsigned char *)out, &used, in + 29, size - 29) == 1;
    total = used;
    if (ok)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)(in + 13)) == 1 &&
             EVP_DecryptFinal_ex(ctx, (unsigned char *)out + total, &used) == 1;
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        OPENSSL_cleanse(out, (size_t)size);
        free(out);
        return NULL;
    }
    out[total + used] = 0;
    return out;
}
static bool execute(const char *sql) {
    return sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK;
}
static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(database, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static bool init_database(void) {
    if (sqlite3_open_v2(config.database, &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK)
        return false;
    sqlite3_stmt *version = NULL;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, NULL) != SQLITE_OK)
        return false;
    int current = sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : -1;
    sqlite3_finalize(version);
    if (current < 0 || current > 1)
        return false;
    sqlite3_busy_timeout(database, 3000);
    return execute(
        "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS accounts(user_id INTEGER PRIMARY KEY,token_blob BLOB NOT "
        "NULL,calendar_id TEXT NOT NULL DEFAULT '',sync_token TEXT NOT NULL DEFAULT '',last_sync "
        "INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT '',reauth_required INTEGER "
        "NOT NULL DEFAULT 0,next_poll INTEGER NOT NULL DEFAULT 0,sync_running INTEGER NOT NULL "
        "DEFAULT 0,provider_id TEXT NOT NULL DEFAULT '',generation TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS oauth_states(state_hash TEXT PRIMARY KEY,user_id INTEGER NOT "
        "NULL,session_hash TEXT NOT NULL,verifier_blob BLOB NOT NULL,expires INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS events(user_id INTEGER NOT NULL REFERENCES accounts(user_id) "
        "ON DELETE CASCADE,event_id TEXT NOT NULL,etag TEXT NOT NULL,status TEXT NOT NULL,payload "
        "BLOB NOT NULL,PRIMARY KEY(user_id,event_id));"
        "CREATE TABLE IF NOT EXISTS changes(id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER "
        "NOT NULL REFERENCES accounts(user_id) ON DELETE CASCADE,idempotency_key TEXT NOT "
        "NULL,payload_hash TEXT NOT NULL,operation TEXT NOT NULL,event_id TEXT NOT NULL,etag TEXT "
        "NOT NULL,payload BLOB NOT NULL,state TEXT NOT NULL CHECK(state IN "
        "('queued','applied','conflict','failed','unknown')),response BLOB,error TEXT NOT NULL "
        "DEFAULT '',attempts INTEGER NOT NULL DEFAULT 0,next_attempt INTEGER NOT NULL DEFAULT "
        "0,created INTEGER NOT NULL,UNIQUE(user_id,idempotency_key));"
        "CREATE INDEX IF NOT EXISTS changes_ready ON changes(user_id,state,next_attempt,id);PRAGMA "
        "user_version=1;");
}
static json_object *load_tokens(sqlite3_int64 user) {
    sqlite3_stmt *s =
        prepare("SELECT token_blob,provider_id,generation FROM accounts WHERE user_id=?");
    if (!s)
        return NULL;
    sqlite3_bind_int64(s, 1, user);
    char *plain = NULL;
    if (sqlite3_step(s) == SQLITE_ROW &&
        (!expected_provider ||
         !strcmp(expected_provider, (const char *)sqlite3_column_text(s, 1))) &&
        (!expected_generation ||
         !strcmp(expected_generation, (const char *)sqlite3_column_text(s, 2))))
        plain = unseal(user, "tokens", sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
    sqlite3_finalize(s);
    if (!plain)
        return NULL;
    json_object *o = parse_json(plain, strlen(plain));
    OPENSSL_cleanse(plain, strlen(plain));
    free(plain);
    return o;
}
static bool save_tokens(sqlite3_int64 user, json_object *tokens) {
    int n = 0;
    char *blob =
        seal(user, "tokens", json_object_to_json_string_ext(tokens, JSON_C_TO_STRING_PLAIN), &n);
    if (!blob)
        return false;
    sqlite3_stmt *s = prepare(
        worker_connection
            ? "UPDATE accounts SET token_blob=?2,reauth_required=0,last_error='' WHERE user_id=?1 "
              "AND provider_id=?3 AND generation=?4"
            : "INSERT INTO accounts(user_id,token_blob) VALUES(?,?) ON CONFLICT(user_id) DO UPDATE "
              "SET token_blob=excluded.token_blob,reauth_required=0,last_error=''");
    bool ok = s && sqlite3_bind_int64(s, 1, user) == SQLITE_OK &&
              sqlite3_bind_blob(s, 2, blob, n, SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok && worker_connection)
        ok = expected_provider &&
             sqlite3_bind_text(s, 3, expected_provider, -1, SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok && worker_connection)
        ok = expected_generation &&
             sqlite3_bind_text(s, 4, expected_generation, -1, SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok)
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
    sqlite3_finalize(s);
    OPENSSL_cleanse(blob, (size_t)n);
    free(blob);
    return ok;
}
static void account_error(sqlite3_int64 user, const char *error, bool reauth) {
    sqlite3_stmt *s =
        prepare("UPDATE accounts SET last_error=?,reauth_required=MAX(reauth_required,?) WHERE "
                "user_id=? AND (?='' OR generation=?)");
    if (!s)
        return;
    sqlite3_bind_text(s, 1, error, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, reauth);
    sqlite3_bind_int64(s, 3, user);
    sqlite3_bind_text(s, 4, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(s);
    sqlite3_finalize(s);
}
static size_t receive_body(char *data, size_t size, size_t count, void *opaque) {
    Buffer *b = opaque;
    size_t n = size * count;
    if (n > RESPONSE_LIMIT - b->length)
        return 0;
    char *p = realloc(b->data, b->length + n + 1);
    if (!p)
        return 0;
    b->data = p;
    memcpy(p + b->length, data, n);
    b->length += n;
    p[b->length] = 0;
    return n;
}
static Http http(const char *method, const char *url, const char *token, const char *body,
                 const char *etag, bool form) {
    Http result = {0, CURLE_FAILED_INIT, NULL};
    CURL *curl = curl_easy_init();
    if (!curl)
        return result;
    struct curl_slist *headers = NULL;
    char *authorization = NULL, *match = NULL;
    Buffer response = {NULL, 0};
    if (token) {
        authorization = malloc(strlen(token) + 24);
        if (!authorization)
            goto done;
        snprintf(authorization, strlen(token) + 24, "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
    }
    if (etag) {
        match = malloc(strlen(etag) + 16);
        if (!match)
            goto done;
        snprintf(match, strlen(etag) + 16, "If-Match: %s", etag);
        headers = curl_slist_append(headers, match);
    }
    headers = curl_slist_append(headers, form ? "Content-Type: application/x-www-form-urlencoded"
                                              : "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROXY, config.proxy);
    curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)config.timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "punto-calendar/1");
    if (*config.google_ca)
        curl_easy_setopt(curl, CURLOPT_CAINFO, config.google_ca);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    result.transport = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    if (response.data)
        result.json = parse_json(response.data, response.length);
done:
    if (authorization) {
        OPENSSL_cleanse(authorization, strlen(authorization));
        free(authorization);
    }
    free(match);
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}
static void http_free(Http *h) {
    if (h->json)
        json_object_put(h->json);
    h->json = NULL;
}
static char *escape(const char *value) {
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    char *e = curl_easy_escape(c, value, 0);
    char *copy = e ? strdup(e) : NULL;
    curl_free(e);
    curl_easy_cleanup(c);
    return copy;
}
static char *form_encode(const char **names, const char **values, size_t count) {
    size_t cap = 1;
    char *out = calloc(1, 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        char *v = escape(values[i]);
        if (!v) {
            free(out);
            return NULL;
        }
        size_t n = strlen(names[i]) + strlen(v) + 2;
        char *p = realloc(out, cap + n);
        if (!p) {
            free(v);
            free(out);
            return NULL;
        }
        out = p;
        snprintf(out + cap - 1, n + 1, "%s%s=%s", i ? "&" : "", names[i], v);
        cap += n - (i ? 0 : 1);
        free(v);
    }
    return out;
}
static bool token_string(const char *s) {
    if (!*s || strlen(s) > TOKEN_LIMIT)
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 32 || *p > 126)
            return false;
    return true;
}
static bool scope_complete(const char *scope) {
    if (!*scope)
        return true;
    bool events = false, calendars = false;
    char *copy = strdup(scope);
    if (!copy)
        return false;
    char *save = NULL;
    for (char *p = strtok_r(copy, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
        if (!strcmp(p, "https://www.googleapis.com/auth/calendar.events"))
            events = true;
        if (!strcmp(p, "https://www.googleapis.com/auth/calendar.calendarlist.readonly"))
            calendars = true;
    }
    free(copy);
    return events && calendars;
}
static bool absorb_tokens(sqlite3_int64 user, json_object *response, json_object *old) {
    const char *access = jstr(response, "access_token"), *refresh = jstr(response, "refresh_token");
    sqlite3_int64 expiry = jint(response, "expires_in");
    if (!token_string(access) || expiry < 1 || expiry > 86400 ||
        strcmp(jstr(response, "token_type"), "Bearer") || !scope_complete(jstr(response, "scope")))
        return false;
    if (!*refresh)
        refresh = jstr(old, "refresh_token");
    if (!token_string(refresh))
        return false;
    json_object *tokens = json_object_new_object();
    addstr(tokens, "access_token", access);
    addstr(tokens, "refresh_token", refresh);
    addint(tokens, "expires_at", (sqlite3_int64)time(NULL) + expiry);
    bool ok = save_tokens(user, tokens);
    json_object_put(tokens);
    return ok;
}
static bool refresh_tokens(sqlite3_int64 user) {
    json_object *old = load_tokens(user);
    if (!old)
        return false;
    const char *names[] = {"client_id", "client_secret", "refresh_token", "grant_type"};
    const char *values[] = {config.client_id, config.client_secret, jstr(old, "refresh_token"),
                            "refresh_token"};
    char *form = form_encode(names, values, 4);
    if (!form) {
        json_object_put(old);
        return false;
    }
    Http h = http("POST", config.token_url, NULL, form, NULL, true);
    OPENSSL_cleanse(form, strlen(form));
    free(form);
    bool ok = h.transport == CURLE_OK && h.status == 200 && absorb_tokens(user, h.json, old);
    if (!ok)
        account_error(user, h.status == 400 ? "reauth_required" : "token_refresh_failed",
                      h.status == 400);
    http_free(&h);
    json_object_put(old);
    return ok;
}
static Http google(sqlite3_int64 user, const char *method, const char *url, const char *body,
                   const char *etag) {
    Http empty = {0, CURLE_LOGIN_DENIED, NULL};
    json_object *tokens = load_tokens(user);
    if (!tokens)
        return empty;
    if (jint(tokens, "expires_at") <= (sqlite3_int64)time(NULL) + 60) {
        json_object_put(tokens);
        if (!refresh_tokens(user))
            return empty;
        tokens = load_tokens(user);
        if (!tokens)
            return empty;
    }
    Http h = http(method, url, jstr(tokens, "access_token"), body, etag, false);
    json_object_put(tokens);
    if (h.transport == CURLE_OK && h.status == 401) {
        http_free(&h);
        if (!refresh_tokens(user))
            return empty;
        tokens = load_tokens(user);
        if (!tokens)
            return empty;
        h = http(method, url, jstr(tokens, "access_token"), body, etag, false);
        json_object_put(tokens);
    }
    return h;
}
static char *base64url(const unsigned char *bytes, size_t n) {
    char *out = malloc(4 * ((n + 2) / 3) + 1);
    if (!out)
        return NULL;
    int used = EVP_EncodeBlock((unsigned char *)out, bytes, (int)n);
    for (int i = 0; i < used; i++) {
        if (out[i] == '+')
            out[i] = '-';
        else if (out[i] == '/')
            out[i] = '_';
    }
    while (used && out[used - 1] == '=')
        used--;
    out[used] = 0;
    return out;
}
static char *random_text(void) {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return NULL;
    return base64url(bytes, sizeof(bytes));
}
static bool decode(const char *source, size_t n, char **out) {
    char *p = malloc(n + 1);
    if (!p)
        return false;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            if (i + 2 >= n) {
                free(p);
                return false;
            }
            int a = hex_value((unsigned char)source[i + 1]),
                b = hex_value((unsigned char)source[i + 2]);
            if (a < 0 || b < 0) {
                free(p);
                return false;
            }
            c = (unsigned char)(a * 16 + b);
            i += 2;
        } else if (c == '+')
            c = ' ';
        if (!c) {
            free(p);
            return false;
        }
        p[used++] = (char)c;
    }
    p[used] = 0;
    *out = p;
    return true;
}
static void form_free(Form *f) {
    for (int i = 0; i < f->count; i++) {
        free(f->fields[i].key);
        if (f->fields[i].value) {
            OPENSSL_cleanse(f->fields[i].value, strlen(f->fields[i].value));
            free(f->fields[i].value);
        }
    }
    memset(f, 0, sizeof(*f));
}
static bool form_parse(const char *body, Form *form) {
    memset(form, 0, sizeof(*form));
    const char *p = body;
    while (*p) {
        if (form->count >= 12)
            goto fail;
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq || eq == p)
            goto fail;
        Field *field = &form->fields[form->count++];
        if (!decode(p, (size_t)(eq - p), &field->key) ||
            !decode(eq + 1, (size_t)(end - eq - 1), &field->value))
            goto fail;
        for (int i = 0; i < form->count - 1; i++)
            if (!strcmp(form->fields[i].key, field->key))
                goto fail;
        p = *end ? end + 1 : end;
        if (*end && !*p)
            goto fail;
    }
    return true;
fail:
    form_free(form);
    return false;
}
static const char *field(Form *f, const char *name) {
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->fields[i].key, name))
            return f->fields[i].value;
    return "";
}
static bool only_fields(Form *f, const char *const *names, size_t count) {
    for (int i = 0; i < f->count; i++) {
        bool found = false;
        for (size_t j = 0; j < count; j++)
            if (!strcmp(f->fields[i].key, names[j]))
                found = true;
        if (!found)
            return false;
    }
    return true;
}
static bool safe_text(const char *text, size_t limit, bool empty);
static json_object *oauth_start(sqlite3_int64 user, Form *form, int *status) {
    const char *names[] = {"session_binding"};
    const char *binding = field(form, "session_binding");
    if (!only_fields(form, names, 1) || !hex_string(binding, 64)) {
        *status = 400;
        return error_json("invalid_session_binding");
    }
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    char *state = random_text(), *verifier = random_text();
    if (!state || !verifier) {
        free(state);
        free(verifier);
        *status = 500;
        return error_json("random_failed");
    }
    unsigned char hash[32];
    unsigned int used = 0;
    EVP_Digest(verifier, strlen(verifier), hash, &used, EVP_sha256(), NULL);
    char *challenge = base64url(hash, sizeof(hash));
    char state_hash[65];
    digest(state, state_hash);
    int blob_n = 0;
    char verifier_purpose[100];
    snprintf(verifier_purpose, sizeof(verifier_purpose), "verifier:%s", state_hash);
    char *blob = seal(user, verifier_purpose, verifier, &blob_n);
    sqlite3_stmt *s =
        prepare("INSERT INTO oauth_states(state_hash,user_id,session_hash,verifier_blob,expires) "
                "VALUES(?,?,?,?,?)");
    bool ok = challenge && blob && s;
    if (ok) {
        sqlite3_bind_text(s, 1, state_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, user);
        sqlite3_bind_text(s, 3, binding, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 4, blob, blob_n, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)time(NULL) + 600);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    free(blob);
    OPENSSL_cleanse(verifier, strlen(verifier));
    free(verifier);
    if (!ok) {
        free(state);
        free(challenge);
        *status = 500;
        return error_json("state_storage_failed");
    }
    s = prepare("DELETE FROM oauth_states WHERE expires<?");
    if (s) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL));
        (void)sqlite3_step(s);
        sqlite3_finalize(s);
    }
    const char *keys[] = {"client_id",
                          "redirect_uri",
                          "response_type",
                          "scope",
                          "access_type",
                          "prompt",
                          "include_granted_scopes",
                          "state",
                          "code_challenge",
                          "code_challenge_method"};
    const char *values[] = {config.client_id, config.redirect_uri,
                            "code",           SCOPE,
                            "offline",        "consent",
                            "true",           state,
                            challenge,        "S256"};
    char *query = form_encode(keys, values, 10);
    json_object *result = json_object_new_object();
    if (!query) {
        json_object_put(result);
        result = error_json("allocation_failed");
        *status = 500;
    } else {
        size_t n = strlen(config.auth_url) + strlen(query) + 2;
        char *url = malloc(n);
        if (!url) {
            json_object_put(result);
            result = error_json("allocation_failed");
            *status = 500;
        } else {
            snprintf(url, n, "%s?%s", config.auth_url, query);
            addstr(result, "authorization_url", url);
            addint(result, "expires_in", 600);
            free(url);
        }
        free(query);
    }
    free(state);
    free(challenge);
    return result;
}
static json_object *oauth_callback(sqlite3_int64 user, Form *form, int *status) {
    const char *names[] = {"state", "code", "session_binding", "error"};
    const char *state = field(form, "state"), *binding = field(form, "session_binding"),
               *code = field(form, "code"), *cancel = field(form, "error");
    if (!only_fields(form, names, 4) || !hex_string(binding, 64) || strlen(state) < 32 ||
        strlen(state) > 128 || strlen(code) > 2048 || (!*code && !*cancel) || (*code && *cancel)) {
        *status = 400;
        return error_json("invalid_oauth_callback");
    }
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    char hash[65];
    digest(state, hash);
    sqlite3_stmt *s = prepare("SELECT verifier_blob FROM oauth_states WHERE state_hash=? AND "
                              "user_id=? AND session_hash=? AND expires>=?");
    char *verifier = NULL;
    if (s) {
        sqlite3_bind_text(s, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, user);
        sqlite3_bind_text(s, 3, binding, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
        if (sqlite3_step(s) == SQLITE_ROW) {
            char purpose[100];
            snprintf(purpose, sizeof(purpose), "verifier:%s", hash);
            verifier = unseal(user, purpose, sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
        }
        sqlite3_finalize(s);
    }
    if (!verifier) {
        *status = 400;
        return error_json("invalid_oauth_state");
    }
    s = prepare("DELETE FROM oauth_states WHERE state_hash=?");
    bool deleted = false;
    if (s) {
        sqlite3_bind_text(s, 1, hash, -1, SQLITE_TRANSIENT);
        deleted = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
        sqlite3_finalize(s);
    }
    if (!deleted) {
        free(verifier);
        *status = 500;
        return error_json("state_storage_failed");
    }
    if (*cancel) {
        OPENSSL_cleanse(verifier, strlen(verifier));
        free(verifier);
        *status = 400;
        return error_json("oauth_cancelled");
    }
    const char *keys[] = {"client_id", "client_secret", "redirect_uri",
                          "code",      "code_verifier", "grant_type"};
    const char *values[] = {config.client_id, config.client_secret, config.redirect_uri, code,
                            verifier,         "authorization_code"};
    char *encoded = form_encode(keys, values, 6);
    OPENSSL_cleanse(verifier, strlen(verifier));
    free(verifier);
    if (!encoded) {
        *status = 500;
        return error_json("allocation_failed");
    }
    Http h = http("POST", config.token_url, NULL, encoded, NULL, true);
    OPENSSL_cleanse(encoded, strlen(encoded));
    free(encoded);
    bool ok =
        h.transport == CURLE_OK && h.status == 200 && token_string(jstr(h.json, "access_token"));
    char *provider_id = NULL;
    if (ok) {
        char url[512];
        snprintf(url, sizeof(url), "%s/calendar/v3/users/me/calendarList/primary",
                 config.api_origin);
        Http identity = http("GET", url, jstr(h.json, "access_token"), NULL, NULL, false);
        ok = identity.transport == CURLE_OK && identity.status == 200 &&
             safe_text(jstr(identity.json, "id"), 1024, false);
        if (ok)
            provider_id = strdup(jstr(identity.json, "id"));
        http_free(&identity);
        ok = ok && provider_id;
    }
    bool mismatch = false;
    if (ok) {
        s = prepare("SELECT provider_id FROM accounts WHERE user_id=?");
        ok = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            if (sqlite3_step(s) == SQLITE_ROW)
                mismatch = strcmp((const char *)sqlite3_column_text(s, 0), provider_id) != 0;
            sqlite3_finalize(s);
        }
    }
    if (mismatch) {
        http_free(&h);
        free(provider_id);
        *status = 409;
        return error_json("google_account_mismatch");
    }
    char *generation = ok ? random_text() : NULL;
    ok = ok && generation;
    if (ok)
        ok = execute("BEGIN IMMEDIATE") && absorb_tokens(user, h.json, NULL);
    if (ok) {
        s = prepare(
            "UPDATE accounts SET provider_id=?,generation=?,sync_running=0 WHERE user_id=?");
        ok = s != NULL;
        if (s) {
            sqlite3_bind_text(s, 1, provider_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, generation, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, user);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    if (ok)
        ok = execute("COMMIT");
    else
        (void)execute("ROLLBACK");
    http_free(&h);
    free(provider_id);
    free(generation);
    if (!ok) {
        *status = 502;
        return error_json("oauth_exchange_failed");
    }
    json_object *o = json_object_new_object();
    addbool(o, "ok", true);
    addbool(o, "connected", true);
    bool selected = false;
    s = prepare("SELECT calendar_id!='' FROM accounts WHERE user_id=?");
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        if (sqlite3_step(s) == SQLITE_ROW)
            selected = sqlite3_column_int(s, 0) != 0;
        sqlite3_finalize(s);
    }
    addbool(o, "calendar_selected", selected);
    return o;
}
static json_object *status_json(sqlite3_int64 user) {
    json_object *o = json_object_new_object();
    addstr(o, "provider", "google");
    addbool(o, "oauth_configured", config.configured);
    addbool(o, "bidirectional", true);
    addint(o, "sync_interval", config.interval);
    if (!user)
        return o;
    sqlite3_stmt *s =
        prepare("SELECT calendar_id,last_sync,last_error,reauth_required,next_poll,sync_running "
                "FROM accounts WHERE user_id=?");
    bool connected = false;
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        if (sqlite3_step(s) == SQLITE_ROW) {
            connected = true;
            addstr(o, "calendar_id", (const char *)sqlite3_column_text(s, 0));
            addint(o, "last_sync", sqlite3_column_int64(s, 1));
            addstr(o, "last_error", (const char *)sqlite3_column_text(s, 2));
            addbool(o, "reauth_required", sqlite3_column_int(s, 3) != 0);
            addbool(o, "sync_requested",
                    sqlite3_column_int64(s, 4) <= (sqlite3_int64)time(NULL) &&
                        *sqlite3_column_text(s, 0));
            addbool(o, "sync_running", sqlite3_column_int(s, 5) != 0);
        }
        sqlite3_finalize(s);
    }
    addbool(o, "connected", connected);
    if (!connected) {
        addstr(o, "calendar_id", "");
        addint(o, "last_sync", 0);
        addstr(o, "last_error", "");
        addbool(o, "reauth_required", false);
        addbool(o, "sync_requested", false);
        addbool(o, "sync_running", false);
    }
    s = prepare("SELECT SUM(state='queued'),SUM(state='conflict'),SUM(state='unknown') FROM "
                "changes WHERE user_id=?");
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        if (sqlite3_step(s) == SQLITE_ROW) {
            addint(o, "queued_changes", sqlite3_column_int64(s, 0));
            addint(o, "conflicts", sqlite3_column_int64(s, 1));
            addint(o, "unknown_changes", sqlite3_column_int64(s, 2));
        }
        sqlite3_finalize(s);
    }
    return o;
}
static bool connected(sqlite3_int64 user) {
    sqlite3_stmt *s = prepare("SELECT 1 FROM accounts WHERE user_id=? AND reauth_required=0");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, user);
    bool yes = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return yes;
}
static json_object *calendars(sqlite3_int64 user, const char *query, int *status) {
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    if (!connected(user)) {
        *status = 409;
        return error_json("calendar_not_connected");
    }
    Form form;
    if (!form_parse(query, &form)) {
        *status = 400;
        return error_json("invalid_query");
    }
    const char *names[] = {"page_token", "limit"};
    const char *page = field(&form, "page_token"), *limit_text = field(&form, "limit");
    int limit = 50;
    bool valid = only_fields(&form, names, 2) && strlen(page) <= 2048 &&
                 (!*limit_text || number(limit_text, 1, 50, &limit));
    for (const unsigned char *p = (const unsigned char *)page; *p; p++)
        if (*p <= 32 || *p > 126)
            valid = false;
    if (!valid) {
        form_free(&form);
        *status = 400;
        return error_json("invalid_query");
    }
    char *encoded = escape(page);
    size_t n = encoded ? strlen(config.api_origin) + strlen(encoded) + 250 : 0;
    char *url = encoded ? malloc(n) : NULL;
    if (!url) {
        free(encoded);
        form_free(&form);
        *status = 500;
        return error_json("allocation_failed");
    }
    snprintf(url, n,
             "%s/calendar/v3/users/me/"
             "calendarList?maxResults=%d&fields=nextPageToken,items(id,summary,timeZone,accessRole,"
             "primary)%s%s",
             config.api_origin, limit, *page ? "&pageToken=" : "", encoded);
    free(encoded);
    form_free(&form);
    Http h = google(user, "GET", url, NULL, NULL);
    free(url);
    json_object *array = NULL;
    bool ok = h.transport == CURLE_OK && h.status == 200 && h.json &&
              json_object_object_get_ex(h.json, "items", &array) &&
              json_object_is_type(array, json_type_array) &&
              json_object_array_length(array) <= (size_t)limit;
    json_object *items = json_object_new_array();
    const char *allowed[] = {"id", "summary", "timeZone", "accessRole", "primary"};
    for (size_t i = 0; ok && i < json_object_array_length(array); i++) {
        json_object *entry = json_object_array_get_idx(array, i);
        ok = json_object_is_type(entry, json_type_object) &&
             safe_text(jstr(entry, "id"), 1024, false);
        if (!ok)
            break;
        json_object *view = json_object_new_object();
        for (size_t j = 0; j < 5; j++) {
            json_object *v = NULL;
            if (json_object_object_get_ex(entry, allowed[j], &v))
                json_object_object_add(view, allowed[j], json_object_get(v));
        }
        json_object_array_add(items, view);
    }
    const char *next = jstr(h.json, "nextPageToken");
    ok = ok && strlen(next) <= 2048;
    json_object *o = json_object_new_object();
    json_object_object_add(o, "items", items);
    json_object_object_add(o, "next_page_token", *next ? json_object_new_string(next) : NULL);
    http_free(&h);
    if (!ok || strlen(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN)) > 100 * 1024) {
        json_object_put(o);
        *status = 502;
        return error_json(ok ? "calendar_page_too_large" : "calendar_list_failed");
    }
    return o;
}
static json_object *selection(sqlite3_int64 user, Form *form, int *status) {
    const char *names[] = {"calendar_id"};
    const char *id = field(form, "calendar_id");
    if (!only_fields(form, names, 1) || !*id || strlen(id) > 1024) {
        *status = 400;
        return error_json("invalid_calendar");
    }
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    if (!connected(user)) {
        *status = 409;
        return error_json("calendar_not_connected");
    }
    char *escaped = escape(id);
    size_t n = escaped ? strlen(config.api_origin) + strlen(escaped) + 100 : 0;
    char *url = escaped ? malloc(n) : NULL;
    if (!url) {
        free(escaped);
        *status = 500;
        return error_json("allocation_failed");
    }
    snprintf(url, n, "%s/calendar/v3/users/me/calendarList/%s", config.api_origin, escaped);
    free(escaped);
    Http h = google(user, "GET", url, NULL, NULL);
    free(url);
    bool allowed = h.transport == CURLE_OK && h.status == 200 && h.json &&
                   (!strcmp(jstr(h.json, "accessRole"), "writer") ||
                    !strcmp(jstr(h.json, "accessRole"), "owner")) &&
                   (!strcmp(jstr(h.json, "id"), id) || !strcmp(id, "primary"));
    char *selected = allowed ? strdup(jstr(h.json, "id")) : NULL;
    long provider_status = h.status;
    http_free(&h);
    if (!selected) {
        *status =
            provider_status == 404 || provider_status == 403 || provider_status == 200 ? 403 : 502;
        return error_json(*status == 403 ? "calendar_not_writable" : "calendar_selection_failed");
    }
    id = selected;
    sqlite3_stmt *s =
        prepare("SELECT 1 FROM changes WHERE user_id=? AND state IN ('queued','unknown') LIMIT 1");
    bool pending = false;
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        pending = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
    }
    if (pending) {
        free(selected);
        *status = 409;
        return error_json("pending_changes");
    }
    char *generation = random_text();
    if (!generation) {
        free(selected);
        *status = 500;
        return error_json("random_failed");
    }
    bool ok = execute("BEGIN IMMEDIATE");
    s = ok ? prepare("UPDATE accounts SET "
                     "calendar_id=?,sync_token='',last_sync=0,last_error='',next_poll=?,sync_"
                     "running=0,generation=? WHERE "
                     "user_id=?")
           : NULL;
    if (s) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL) + config.interval);
        sqlite3_bind_text(s, 3, generation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, user);
        ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
    } else
        ok = false;
    const char *clean[] = {"DELETE FROM events WHERE user_id=?"};
    for (size_t i = 0; ok && i < 1; i++) {
        s = prepare(clean[i]);
        ok = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    if (ok)
        ok = execute("COMMIT");
    else
        (void)execute("ROLLBACK");
    free(generation);
    if (!ok) {
        free(selected);
        *status = 500;
        return error_json("calendar_selection_failed");
    }
    json_object *o = json_object_new_object();
    addstr(o, "calendar_id", id);
    addbool(o, "selected", true);
    free(selected);
    return o;
}
static bool get_calendar(sqlite3_int64 user, char **calendar, char **token) {
    *calendar = NULL;
    if (token)
        *token = NULL;
    sqlite3_stmt *s = prepare("SELECT calendar_id,sync_token FROM accounts WHERE user_id=? AND "
                              "reauth_required=0 AND (?='' OR generation=?)");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, user);
    sqlite3_bind_text(s, 2, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        *calendar = strdup((const char *)sqlite3_column_text(s, 0));
        if (token)
            *token = strdup((const char *)sqlite3_column_text(s, 1));
    }
    sqlite3_finalize(s);
    return *calendar && **calendar && (!token || *token);
}
/* Gregorian civil date conversion, independent of the process timezone. */
static int64_t civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned year = (unsigned)(y - era * 400);
    unsigned day = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    return (int64_t)era * 146097 + (int64_t)(year * 365 + year / 4 - year / 100 + day) - 719468;
}
static bool date_value(const char *s, int64_t *epoch) {
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return false;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)s[i]))
            return false;
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + s[3] - '0';
    int m = (s[5] - '0') * 10 + s[6] - '0';
    int d = (s[8] - '0') * 10 + s[9] - '0';
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1900 || y > 2199 || m < 1 || m > 12 || d < 1 ||
        d > md[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return false;
    *epoch = civil_days(y, (unsigned)m, (unsigned)d) * 86400;
    return true;
}
static bool datetime_value(const char *s, int64_t *epoch) {
    size_t n = strlen(s);
    if (n != 20 && n != 25)
        return false;
    char date[11];
    memcpy(date, s, 10);
    date[10] = 0;
    if (!date_value(date, epoch) || s[10] != 'T' || s[13] != ':' || s[16] != ':')
        return false;
    const int indices[] = {11, 12, 14, 15, 17, 18};
    for (size_t i = 0; i < 6; i++)
        if (!isdigit((unsigned char)s[indices[i]]))
            return false;
    int h = (s[11] - '0') * 10 + s[12] - '0', m = (s[14] - '0') * 10 + s[15] - '0',
        sec = (s[17] - '0') * 10 + s[18] - '0';
    if (h > 23 || m > 59 || sec > 59)
        return false;
    int offset = 0;
    if (n == 20) {
        if (s[19] != 'Z')
            return false;
    } else {
        if ((s[19] != '+' && s[19] != '-') || s[22] != ':' || !isdigit((unsigned char)s[20]) ||
            !isdigit((unsigned char)s[21]) || !isdigit((unsigned char)s[23]) ||
            !isdigit((unsigned char)s[24]))
            return false;
        int oh = (s[20] - '0') * 10 + s[21] - '0', om = (s[23] - '0') * 10 + s[24] - '0';
        if (oh > 14 || om > 59 || (oh == 14 && om))
            return false;
        offset = (oh * 60 + om) * 60 * (s[19] == '+' ? 1 : -1);
    }
    *epoch += h * 3600 + m * 60 + sec - offset;
    return true;
}
static bool safe_text(const char *text, size_t limit, bool empty) {
    size_t n = strlen(text);
    if (n > limit || (!empty && !n))
        return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 32 && *p != '\n' && *p != '\t')
            return false;
        if (*p < 128) {
            p++;
            continue;
        }
        unsigned int value, need;
        if (*p >= 0xc2 && *p <= 0xdf) {
            value = *p & 31;
            need = 1;
        } else if (*p >= 0xe0 && *p <= 0xef) {
            value = *p & 15;
            need = 2;
        } else if (*p >= 0xf0 && *p <= 0xf4) {
            value = *p & 7;
            need = 3;
        } else
            return false;
        p++;
        for (unsigned int i = 0; i < need; i++, p++) {
            if ((*p & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (*p & 63);
        }
        if ((need == 2 && value < 0x800) || (need == 3 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
static bool event_time(json_object *o, int64_t *epoch, bool *all_day) {
    if (!o || !json_object_is_type(o, json_type_object))
        return false;
    json_object_object_foreach(o, key, value) {
        if ((strcmp(key, "date") && strcmp(key, "dateTime") && strcmp(key, "timeZone")) ||
            !json_object_is_type(value, json_type_string))
            return false;
    }
    const char *date = jstr(o, "date"), *dt = jstr(o, "dateTime"), *zone = jstr(o, "timeZone");
    json_object *v = NULL;
    bool has_date = json_object_object_get_ex(o, "date", &v),
         has_dt = json_object_object_get_ex(o, "dateTime", &v);
    if (has_date == has_dt || (!*date && !*dt) || strlen(zone) > 100 ||
        (json_object_object_get_ex(o, "timeZone", &v) && !*zone))
        return false;
    for (const unsigned char *p = (const unsigned char *)zone; *p; p++)
        if (!isalnum(*p) && !strchr("/_+-", *p))
            return false;
    *all_day = *date != 0;
    return *all_day ? date_value(date, epoch) : datetime_value(dt, epoch);
}
static json_object *validated_event(const char *text) {
    json_object *o = parse_json(text, strlen(text));
    if (!o || !json_object_is_type(o, json_type_object))
        goto fail;
    json_object_object_foreach(o, key, value) {
        if (strcmp(key, "summary") && strcmp(key, "description") && strcmp(key, "location") &&
            strcmp(key, "start") && strcmp(key, "end"))
            goto fail;
        if (strcmp(key, "start") && strcmp(key, "end") &&
            !json_object_is_type(value, json_type_string))
            goto fail;
    }
    if (!safe_text(jstr(o, "summary"), 1000, false) ||
        !safe_text(jstr(o, "description"), 8192, true) ||
        !safe_text(jstr(o, "location"), 1024, true))
        goto fail;
    json_object *start = NULL, *end = NULL;
    int64_t a, b;
    bool da, db;
    if (!json_object_object_get_ex(o, "start", &start) ||
        !json_object_object_get_ex(o, "end", &end) || !event_time(start, &a, &da) ||
        !event_time(end, &b, &db) || da != db || b <= a || b - a > 366 * 86400LL)
        goto fail;
    json_object *canonical = json_object_new_object();
    addstr(canonical, "summary", jstr(o, "summary"));
    if (*jstr(o, "description"))
        addstr(canonical, "description", jstr(o, "description"));
    if (*jstr(o, "location"))
        addstr(canonical, "location", jstr(o, "location"));
    const char *boundaries[] = {"start", "end"};
    const char *parts[] = {"date", "dateTime", "timeZone"};
    for (size_t i = 0; i < 2; i++) {
        json_object *input = NULL;
        json_object_object_get_ex(o, boundaries[i], &input);
        json_object *output = json_object_new_object();
        for (size_t j = 0; j < 3; j++)
            if (*jstr(input, parts[j]))
                addstr(output, parts[j], jstr(input, parts[j]));
        json_object_object_add(canonical, boundaries[i], output);
    }
    json_object_put(o);
    return canonical;
fail:
    if (o)
        json_object_put(o);
    return NULL;
}
static bool event_identifier(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 1024)
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return false;
    return true;
}
static bool etag_valid(const char *s) {
    size_t n = strlen(s);
    if (n < 2 || n > 200 || s[0] != '"' || s[n - 1] != '"')
        return false;
    for (size_t i = 1; i < n - 1; i++)
        if ((unsigned char)s[i] <= 32 || (unsigned char)s[i] >= 127 || s[i] == '"' || s[i] == '\\')
            return false;
    return true;
}
static json_object *change_json(sqlite3_stmt *s, bool replay) {
    json_object *o = json_object_new_object();
    addint(o, "id", sqlite3_column_int64(s, 0));
    addstr(o, "idempotency_key", (const char *)sqlite3_column_text(s, 1));
    addstr(o, "event_id", (const char *)sqlite3_column_text(s, 2));
    addstr(o, "state", (const char *)sqlite3_column_text(s, 3));
    addstr(o, "error", (const char *)sqlite3_column_text(s, 4));
    addint(o, "attempts", sqlite3_column_int64(s, 5));
    addbool(o, "replayed", replay);
    return o;
}
static json_object *enqueue(sqlite3_int64 user, const char *operation, Form *form, int *status) {
    const char *names[] = {"idempotency_key", "event", "event_id", "etag"};
    const char *key = field(form, "idempotency_key"), *id = field(form, "event_id"),
               *etag = field(form, "etag"), *text = field(form, "event");
    if (!only_fields(form, names, 4) || strlen(key) < 8 || strlen(key) > 128) {
        *status = 400;
        return error_json("invalid_idempotency_key");
    }
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (!isalnum(*p) && !strchr("_-:.", *p)) {
            *status = 400;
            return error_json("invalid_idempotency_key");
        }
    bool create = !strcmp(operation, "create"), del = !strcmp(operation, "delete");
    if ((create && (*id || *etag)) || (!create && (!event_identifier(id) || !etag_valid(etag))) ||
        (del && *text)) {
        *status = 400;
        return error_json("invalid_event_reference");
    }
    json_object *event = del ? json_object_new_object() : validated_event(text);
    if (!event) {
        *status = 400;
        return error_json("invalid_event");
    }
    char marker_input[200], identity[65], marker[65];
    snprintf(marker_input, sizeof(marker_input), "%lld:%s", (long long)user, key);
    digest(marker_input, identity);
    if (create) {
        id = identity;
        addstr(event, "id", identity);
    }
    json_object *envelope = json_object_new_object();
    addstr(envelope, "operation", operation);
    addstr(envelope, "event_id", id);
    addstr(envelope, "etag", etag);
    json_object_object_add(envelope, "event", event);
    const char *canonical = json_object_to_json_string_ext(envelope, JSON_C_TO_STRING_PLAIN);
    size_t marker_n = strlen(canonical) + 66;
    char *marker_payload = malloc(marker_n);
    if (!marker_payload) {
        json_object_put(envelope);
        *status = 500;
        return error_json("allocation_failed");
    }
    snprintf(marker_payload, marker_n, "%s:%s", identity, canonical);
    digest(marker_payload, marker);
    free(marker_payload);
    if (!del) {
        json_object *props = json_object_new_object(), *priv = json_object_new_object();
        addstr(priv, "puntoMutation", marker);
        json_object_object_add(props, "private", priv);
        json_object_object_add(event, "extendedProperties", props);
    }
    const char *payload = json_object_to_json_string_ext(envelope, JSON_C_TO_STRING_PLAIN);
    char hash[65];
    digest(payload, hash);
    sqlite3_stmt *s =
        prepare("SELECT id,idempotency_key,event_id,state,error,attempts,payload_hash FROM changes "
                "WHERE user_id=? AND idempotency_key=?");
    if (!s) {
        json_object_put(envelope);
        *status = 500;
        return error_json("queue_storage_failed");
    }
    sqlite3_bind_int64(s, 1, user);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        bool match = !strcmp((const char *)sqlite3_column_text(s, 6), hash);
        json_object *o = match ? change_json(s, true) : error_json("idempotency_conflict");
        sqlite3_finalize(s);
        json_object_put(envelope);
        *status = match ? 200 : 409;
        return o;
    }
    sqlite3_finalize(s);
    if (!config.configured || !connected(user)) {
        json_object_put(envelope);
        *status = config.configured ? 409 : 503;
        return error_json(config.configured ? "calendar_not_connected" : "oauth_not_configured");
    }
    char *calendar = NULL;
    if (!get_calendar(user, &calendar, NULL)) {
        free(calendar);
        json_object_put(envelope);
        *status = 409;
        return error_json("calendar_not_selected");
    }
    free(calendar);
    int n = 0;
    char change_purpose[200];
    snprintf(change_purpose, sizeof(change_purpose), "change:%s", key);
    char *blob = seal(user, change_purpose, payload, &n);
    s = blob ? prepare("INSERT INTO "
                       "changes(user_id,idempotency_key,payload_hash,operation,event_id,etag,"
                       "payload,state,created) VALUES(?,?,?,?,?,?,?,'queued',?)")
             : NULL;
    bool ok = s != NULL;
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 5, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, etag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 7, blob, n, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 8, (sqlite3_int64)time(NULL));
        ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
    }
    free(blob);
    json_object_put(envelope);
    if (!ok) {
        *status = 500;
        return error_json("queue_storage_failed");
    }
    json_object *o = json_object_new_object();
    addint(o, "id", sqlite3_last_insert_rowid(database));
    addstr(o, "idempotency_key", key);
    addstr(o, "event_id", id);
    addstr(o, "state", "queued");
    addbool(o, "replayed", false);
    *status = 202;
    return o;
}
static bool row_purpose(char *out, size_t capacity, const char *prefix, const char *identity) {
    int n = snprintf(out, capacity, "%s:%s", prefix, identity);
    return n > 0 && (size_t)n < capacity;
}
static bool save_event(sqlite3_int64 user, const char *calendar, json_object *event) {
    const char *id = jstr(event, "id"), *etag = jstr(event, "etag"), *state = jstr(event, "status");
    if (!event_identifier(id) || !safe_text(etag, 200, false))
        return false;
    const char *plain = json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN);
    if (strlen(plain) > 65536)
        return false;
    char purpose[2300];
    int len = snprintf(purpose, sizeof(purpose), "event:%s:%s", calendar, id);
    if (len < 0 || (size_t)len >= sizeof(purpose))
        return false;
    int n = 0;
    char *blob = seal(user, purpose, plain, &n);
    sqlite3_stmt *s =
        blob ? prepare("INSERT INTO events(user_id,event_id,etag,status,payload) SELECT ?,?,?,?,? "
                       "WHERE EXISTS(SELECT 1 FROM accounts WHERE user_id=? AND calendar_id=? AND "
                       "(?='' OR generation=?)) "
                       "ON CONFLICT(user_id,event_id) DO UPDATE SET "
                       "etag=excluded.etag,status=excluded.status,payload=excluded.payload")
             : NULL;
    bool ok = s != NULL;
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, etag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 5, blob, n, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 6, user);
        sqlite3_bind_text(s, 7, calendar, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 8, expected_generation ? expected_generation : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 9, expected_generation ? expected_generation : "", -1,
                          SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
        sqlite3_finalize(s);
    }
    free(blob);
    return ok;
}
static bool remove_event(sqlite3_int64 user, const char *id) {
    sqlite3_stmt *s = prepare("DELETE FROM events WHERE user_id=? AND event_id=? AND EXISTS(SELECT "
                              "1 FROM accounts WHERE user_id=? AND (?='' OR generation=?))");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, user);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, user);
    sqlite3_bind_text(s, 4, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
static void change_state(sqlite3_int64 id, const char *state, const char *error, int delay) {
    sqlite3_stmt *s =
        prepare("UPDATE changes SET state=?,error=?,next_attempt=? WHERE id=? AND EXISTS(SELECT 1 "
                "FROM accounts WHERE user_id=changes.user_id AND (?='' OR generation=?))");
    if (!s)
        return;
    sqlite3_bind_text(s, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, error, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)time(NULL) + delay);
    sqlite3_bind_int64(s, 4, id);
    sqlite3_bind_text(s, 5, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 6, expected_generation ? expected_generation : "", -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(s);
    sqlite3_finalize(s);
}
static bool marker_matches(json_object *remote, const char *marker) {
    json_object *props = NULL, *priv = NULL;
    return remote && json_object_object_get_ex(remote, "extendedProperties", &props) &&
           json_object_object_get_ex(props, "private", &priv) &&
           !strcmp(jstr(priv, "puntoMutation"), marker);
}
static char *event_url(const char *calendar, const char *id, bool mutate) {
    char *cal = escape(calendar), *eid = escape(id ? id : "");
    if (!cal || !eid) {
        free(cal);
        free(eid);
        return NULL;
    }
    size_t n = strlen(config.api_origin) + strlen(cal) + strlen(eid) + 100;
    char *url = malloc(n);
    if (url)
        snprintf(url, n, "%s/calendar/v3/calendars/%s/events%s%s%s", config.api_origin, cal,
                 id ? "/" : "", eid, mutate ? "?sendUpdates=none" : "");
    free(cal);
    free(eid);
    return url;
}
/* A persisted unknown state is reconciled by GET before every retry, including after a crash. */
static bool reconcile(sqlite3_int64 user, sqlite3_int64 change, const char *calendar,
                      const char *operation, const char *id, const char *etag, const char *marker,
                      int delay) {
    char *url = event_url(calendar, id, false);
    if (!url)
        return false;
    Http h = google(user, "GET", url, NULL, NULL);
    free(url);
    bool retry = false;
    if (h.transport != CURLE_OK || h.status >= 500 || h.status == 429)
        change_state(change, "unknown", "reconciliation_pending", delay);
    else if (h.status == 404 || h.status == 410) {
        if (!strcmp(operation, "delete")) {
            (void)remove_event(user, id);
            change_state(change, "applied", "", 0);
        } else if (!strcmp(operation, "create")) {
            change_state(change, "queued", "", 0);
            retry = true;
        } else
            change_state(change, "conflict", "remote_event_missing", 0);
    } else if (h.status == 200 && h.json) {
        if (marker_matches(h.json, marker) && strcmp(operation, "delete")) {
            if (save_event(user, calendar, h.json))
                change_state(change, "applied", "", 0);
            else
                change_state(change, "unknown", "cache_write_failed", delay);
        } else if (strcmp(operation, "create") && !strcmp(jstr(h.json, "etag"), etag)) {
            change_state(change, "queued", "", 0);
            retry = true;
        } else
            change_state(change, "conflict", "remote_event_changed", 0);
    } else if (h.status == 403 || h.status == 401)
        change_state(change, "failed", "calendar_access_denied", 0);
    else
        change_state(change, "unknown", "reconciliation_pending", delay);
    http_free(&h);
    return retry;
}
static void flush_changes(sqlite3_int64 user, const char *calendar) {
    for (int batch = 0; batch < 20 && !stopping; batch++) {
        sqlite3_stmt *s =
            prepare("SELECT id,idempotency_key,operation,event_id,etag,payload,state,attempts FROM "
                    "changes WHERE user_id=? AND state IN ('queued','unknown') AND next_attempt<=? "
                    "ORDER BY id LIMIT 1");
        if (!s)
            return;
        sqlite3_bind_int64(s, 1, user);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
        if (sqlite3_step(s) != SQLITE_ROW) {
            sqlite3_finalize(s);
            break;
        }
        sqlite3_int64 id = sqlite3_column_int64(s, 0);
        char *key = strdup((const char *)sqlite3_column_text(s, 1)),
             *operation = strdup((const char *)sqlite3_column_text(s, 2)),
             *event_id = strdup((const char *)sqlite3_column_text(s, 3)),
             *etag = strdup((const char *)sqlite3_column_text(s, 4)),
             *state = strdup((const char *)sqlite3_column_text(s, 6));
        int attempts = sqlite3_column_int(s, 7);
        char purpose[200];
        char *plain = NULL;
        if (key && row_purpose(purpose, sizeof(purpose), "change", key))
            plain = unseal(user, purpose, sqlite3_column_blob(s, 5), sqlite3_column_bytes(s, 5));
        sqlite3_finalize(s);
        if (!key || !operation || !event_id || !etag || !state || !plain) {
            change_state(id, "failed", "queue_integrity_failed", 0);
            free(key);
            free(operation);
            free(event_id);
            free(etag);
            free(state);
            free(plain);
            continue;
        }
        json_object *envelope = parse_json(plain, strlen(plain));
        OPENSSL_cleanse(plain, strlen(plain));
        free(plain);
        json_object *event = NULL;
        if (!envelope || !json_object_object_get_ex(envelope, "event", &event)) {
            change_state(id, "failed", "queue_integrity_failed", 0);
            goto next;
        }
        json_object *props = NULL, *priv = NULL;
        const char *marker = "";
        if (json_object_object_get_ex(event, "extendedProperties", &props) &&
            json_object_object_get_ex(props, "private", &priv))
            marker = jstr(priv, "puntoMutation");
        int delay = 30 * (1 << (attempts > 7 ? 7 : attempts));
        if (!strcmp(state, "unknown") &&
            !reconcile(user, id, calendar, operation, event_id, etag, marker, delay))
            goto next;
        char *url = event_url(calendar, !strcmp(operation, "create") ? NULL : event_id, true);
        if (!url) {
            change_state(id, "queued", "allocation_failed", 30);
            goto next;
        }
        s = prepare("UPDATE changes SET "
                    "state='unknown',error='delivery_pending',attempts=attempts+1,next_attempt=? "
                    "WHERE id=?");
        bool claimed = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL) + delay);
            sqlite3_bind_int64(s, 2, id);
            claimed = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
            sqlite3_finalize(s);
        }
        if (!claimed) {
            free(url);
            goto next;
        }
        const char *method = !strcmp(operation, "create")   ? "POST"
                             : !strcmp(operation, "update") ? "PATCH"
                                                            : "DELETE";
        Http h = google(user, method, url,
                        !strcmp(operation, "delete")
                            ? NULL
                            : json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN),
                        !strcmp(operation, "create") ? NULL : etag);
        free(url);
        if (h.transport == CURLE_OK &&
            ((!strcmp(operation, "delete") &&
              (h.status == 204 || h.status == 404 || h.status == 410)) ||
             (strcmp(operation, "delete") && (h.status == 200 || h.status == 201) && h.json &&
              !strcmp(jstr(h.json, "id"), event_id)))) {
            bool saved = !strcmp(operation, "delete") ? remove_event(user, event_id)
                                                      : save_event(user, calendar, h.json);
            change_state(id, saved ? "applied" : "unknown", saved ? "" : "cache_write_failed",
                         saved ? 0 : delay);
        } else if (h.transport == CURLE_OK && h.status == 412)
            change_state(id, "conflict", "etag_conflict", 0);
        else if (h.transport == CURLE_OK && h.status >= 400 && h.status < 500 && h.status != 429 &&
                 h.status != 409)
            change_state(id, "failed",
                         h.status == 403 ? "calendar_access_denied" : "google_rejected_change", 0);
        else {
            change_state(id, "unknown", "delivery_unknown", delay);
            if (h.transport == CURLE_OK && h.status == 409)
                (void)reconcile(user, id, calendar, operation, event_id, etag, marker, delay);
        }
        http_free(&h);
    next:
        if (envelope)
            json_object_put(envelope);
        free(key);
        free(operation);
        free(event_id);
        free(etag);
        free(state);
    }
}
static bool pull_events(sqlite3_int64 user, const char *calendar, const char *sync_token, bool full,
                        int depth) {
    json_object *all = json_object_new_array();
    char *page = strdup("");
    char *next_sync = NULL;
    bool ok = page != NULL, expired = false;
    size_t aggregate_bytes = 0;
    for (int p = 0; ok && p < PAGE_LIMIT && !stopping; p++) {
        char *cal = escape(calendar), *sync = escape(full ? "" : sync_token), *pt = escape(page);
        if (!cal || !sync || !pt) {
            free(cal);
            free(sync);
            free(pt);
            ok = false;
            break;
        }
        size_t n = strlen(config.api_origin) + strlen(cal) + strlen(sync) + strlen(pt) + 200;
        char *url = malloc(n);
        if (!url) {
            free(cal);
            free(sync);
            free(pt);
            ok = false;
            break;
        }
        snprintf(url, n,
                 "%s/calendar/v3/calendars/%s/"
                 "events?maxResults=250&showDeleted=true&singleEvents=false%s%s%s%s",
                 config.api_origin, cal, *sync ? "&syncToken=" : "", sync,
                 *page ? "&pageToken=" : "", pt);
        free(cal);
        free(sync);
        free(pt);
        Http h = google(user, "GET", url, NULL, NULL);
        free(url);
        json_object *items = NULL;
        if (h.transport == CURLE_OK && h.status == 410 && !full && depth == 0) {
            expired = true;
            http_free(&h);
            break;
        }
        ok = h.transport == CURLE_OK && h.status == 200 && h.json &&
             json_object_object_get_ex(h.json, "items", &items) &&
             json_object_is_type(items, json_type_array);
        if (ok) {
            size_t count = json_object_array_length(items);
            if (json_object_array_length(all) + count > EVENT_LIMIT)
                ok = false;
            for (size_t i = 0; ok && i < count; i++) {
                json_object *item = json_object_array_get_idx(items, i);
                size_t item_bytes =
                    strlen(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN));
                if (!json_object_is_type(item, json_type_object) ||
                    !event_identifier(jstr(item, "id")) ||
                    strlen(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN)) > 65536) {
                    ok = false;
                    break;
                }
                if (item_bytes > 16 * 1024 * 1024 - aggregate_bytes) {
                    ok = false;
                    break;
                }
                aggregate_bytes += item_bytes;
                json_object_array_add(all, json_object_get(item));
            }
        }
        const char *next = jstr(h.json, "nextPageToken");
        char *copy = ok ? strdup(next) : NULL;
        if (ok && !*next && *jstr(h.json, "nextSyncToken"))
            next_sync = strdup(jstr(h.json, "nextSyncToken"));
        http_free(&h);
        free(page);
        page = copy;
        if (!ok || !page || !*page)
            break;
        if (p == PAGE_LIMIT - 1)
            ok = false;
    }
    free(page);
    if (expired) {
        json_object_put(all);
        free(next_sync);
        return pull_events(user, calendar, "", true, depth + 1);
    }
    ok = ok && !stopping && next_sync && strlen(next_sync) <= TOKEN_LIMIT;
    if (ok)
        ok = execute("BEGIN IMMEDIATE");
    sqlite3_stmt *s = NULL;
    if (ok) {
        s = prepare("SELECT 1 FROM accounts WHERE user_id=? AND calendar_id=? AND "
                    "reauth_required=0 AND generation=?");
        ok = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            sqlite3_bind_text(s, 2, calendar, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, expected_generation ? expected_generation : "", -1,
                              SQLITE_TRANSIENT);
            ok = sqlite3_step(s) == SQLITE_ROW;
            sqlite3_finalize(s);
        }
    }
    if (ok && full) {
        s = prepare("DELETE FROM events WHERE user_id=?");
        ok = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    for (size_t i = 0; ok && i < json_object_array_length(all); i++) {
        json_object *event = json_object_array_get_idx(all, i);
        const char *state = jstr(event, "status");
        /* Google tombstones may only contain an id; keep the cancellation visible without stale
         * details. */
        if (!strcmp(state, "cancelled") && !*jstr(event, "etag"))
            addstr(event, "etag", "cancelled");
        ok = save_event(user, calendar, event);
    }
    if (ok) {
        s = prepare("UPDATE accounts SET sync_token=?,last_sync=?,last_error='',reauth_required=0 "
                    "WHERE user_id=?");
        ok = s != NULL;
        if (s) {
            sqlite3_bind_text(s, 1, next_sync, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
            sqlite3_bind_int64(s, 3, user);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    if (ok)
        ok = execute("COMMIT");
    else
        (void)execute("ROLLBACK");
    if (!ok)
        account_error(user, "event_sync_failed", false);
    json_object_put(all);
    free(next_sync);
    return ok;
}
static json_object *synchronize(sqlite3_int64 user, int *status) {
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    if (!connected(user)) {
        *status = 409;
        return error_json("calendar_not_connected");
    }
    char *calendar = NULL, *sync = NULL;
    if (!get_calendar(user, &calendar, &sync)) {
        free(calendar);
        free(sync);
        *status = 409;
        return error_json("calendar_not_selected");
    }
    flush_changes(user, calendar);
    bool ok = pull_events(user, calendar, sync, !*sync, 0);
    free(calendar);
    free(sync);
    if (!ok) {
        *status = 502;
        return error_json("event_sync_failed");
    }
    json_object *o = status_json(user);
    addbool(o, "synced", true);
    sqlite3_stmt *s =
        prepare("SELECT COUNT(*) FROM events WHERE user_id=? AND status!='cancelled'");
    if (s) {
        sqlite3_bind_int64(s, 1, user);
        if (sqlite3_step(s) == SQLITE_ROW)
            addint(o, "event_count", sqlite3_column_int64(s, 0));
        sqlite3_finalize(s);
    }
    return o;
}
static void background_tick(void) {
    static time_t next_tick;
    time_t now = time(NULL);
    if (!config.configured || now < next_tick)
        return;
    next_tick = now + 5;
    const char *query =
        "SELECT user_id,provider_id,generation FROM accounts WHERE user_id>? AND calendar_id!='' "
        "AND reauth_required=0 "
        "AND (next_poll<=? OR EXISTS(SELECT 1 FROM changes WHERE changes.user_id=accounts.user_id "
        "AND state IN ('queued','unknown') AND next_attempt<=?)) ORDER BY user_id LIMIT 1";
    sqlite3_stmt *s = NULL;
    sqlite3_int64 user = 0;
    char *provider = NULL, *generation = NULL;
    for (int pass = 0; pass < 2; pass++) {
        s = prepare(query);
        if (!s)
            return;
        sqlite3_bind_int64(s, 1, worker_cursor);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)now);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)now);
        if (sqlite3_step(s) == SQLITE_ROW) {
            user = sqlite3_column_int64(s, 0);
            provider = strdup((const char *)sqlite3_column_text(s, 1));
            generation = strdup((const char *)sqlite3_column_text(s, 2));
        }
        sqlite3_finalize(s);
        if (user)
            break;
        if (!worker_cursor)
            return;
        worker_cursor = 0;
    }
    if (!user || !provider || !generation) {
        free(provider);
        free(generation);
        return;
    }
    worker_cursor = user;
    expected_provider = provider;
    expected_generation = generation;
    s = prepare("UPDATE accounts SET next_poll=?,sync_running=1 WHERE user_id=? AND generation=?");
    bool claimed = false;
    if (s) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)now + config.interval);
        sqlite3_bind_int64(s, 2, user);
        sqlite3_bind_text(s, 3, generation, -1, SQLITE_TRANSIENT);
        claimed = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
        sqlite3_finalize(s);
    }
    if (claimed) {
        int status = 200;
        json_object *o = synchronize(user, &status);
        json_object_put(o);
        s = prepare("UPDATE accounts SET sync_running=0 WHERE user_id=? AND generation=?");
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            sqlite3_bind_text(s, 2, generation, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    expected_provider = NULL;
    expected_generation = NULL;
    free(provider);
    free(generation);
}
static void *worker_loop(void *opaque) {
    database = opaque;
    worker_connection = true;
    while (!stopping) {
        background_tick();
        struct timespec pause = {1, 0};
        (void)nanosleep(&pause, NULL);
    }
    sqlite3_close(database);
    database = NULL;
    return NULL;
}
static json_object *request_sync(sqlite3_int64 user, int *status) {
    if (!config.configured) {
        *status = 503;
        return error_json("oauth_not_configured");
    }
    if (!connected(user)) {
        *status = 409;
        return error_json("calendar_not_connected");
    }
    char *calendar = NULL;
    if (!get_calendar(user, &calendar, NULL)) {
        free(calendar);
        *status = 409;
        return error_json("calendar_not_selected");
    }
    free(calendar);
    sqlite3_stmt *s = prepare("UPDATE accounts SET next_poll=? WHERE user_id=?");
    bool ok = s != NULL;
    if (s) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_int64(s, 2, user);
        ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
    }
    if (!ok) {
        *status = 500;
        return error_json("sync_request_failed");
    }
    *status = 202;
    json_object *o = status_json(user);
    addbool(o, "sync_requested", true);
    return o;
}
/* Expose documented event fields. Never return provider credentials or arbitrary private
 * properties. */
static json_object *public_event(json_object *raw) {
    const char *names[] = {
        "id",      "etag",     "status",     "summary",          "description",       "location",
        "start",   "end",      "recurrence", "recurringEventId", "originalStartTime", "updated",
        "created", "htmlLink", "sequence",   "eventType"};
    json_object *o = json_object_new_object();
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        json_object *v = NULL;
        if (json_object_object_get_ex(raw, names[i], &v))
            json_object_object_add(o, names[i], json_object_get(v));
    }
    return o;
}
static bool in_range(json_object *event, int64_t from, int64_t to) {
    json_object *recurrence = NULL;
    if (json_object_object_get_ex(event, "recurrence", &recurrence))
        return true;
    json_object *a = NULL, *b = NULL;
    int64_t start, end;
    bool da, db;
    if (!json_object_object_get_ex(event, "start", &a) ||
        !json_object_object_get_ex(event, "end", &b) || !event_time(a, &start, &da) ||
        !event_time(b, &end, &db))
        return true;
    return start < to && end > from;
}
static json_object *events_list(sqlite3_int64 user, const char *query, int *status) {
    Form form;
    if (!form_parse(query, &form)) {
        *status = 400;
        return error_json("invalid_query");
    }
    const char *names[] = {"from", "to", "limit", "after_id"};
    const char *from = field(&form, "from"), *to = field(&form, "to"),
               *after = field(&form, "after_id"), *limit_text = field(&form, "limit");
    int limit = 20;
    int64_t first = 0, last = 0;
    bool ranged = *from || *to;
    bool valid = only_fields(&form, names, 4) &&
                 (!*limit_text || number(limit_text, 1, 100, &limit)) &&
                 (!*after || event_identifier(after));
    if (ranged)
        valid = valid && date_value(from, &first) && date_value(to, &last) && last >= first &&
                last - first <= 366 * 86400LL;
    if (!valid) {
        form_free(&form);
        *status = 400;
        return error_json("invalid_query");
    }
    if (ranged)
        last += 86400;
    char *calendar = NULL;
    if (!get_calendar(user, &calendar, NULL)) {
        free(calendar);
        form_free(&form);
        *status = 409;
        return error_json("calendar_not_selected");
    }
    sqlite3_stmt *s = prepare(
        "SELECT event_id,payload FROM events WHERE user_id=? AND event_id>? ORDER BY event_id");
    if (!s) {
        free(calendar);
        form_free(&form);
        *status = 500;
        return error_json("cache_read_failed");
    }
    sqlite3_bind_int64(s, 1, user);
    sqlite3_bind_text(s, 2, after, -1, SQLITE_TRANSIENT);
    json_object *items = json_object_new_array();
    char *cursor = NULL;
    size_t bytes = 0;
    bool more = false, ok = true;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0);
        char purpose[2300];
        snprintf(purpose, sizeof(purpose), "event:%s:%s", calendar, id);
        char *plain = unseal(user, purpose, sqlite3_column_blob(s, 1), sqlite3_column_bytes(s, 1));
        json_object *event = plain ? parse_json(plain, strlen(plain)) : NULL;
        free(plain);
        if (!event) {
            ok = false;
            break;
        }
        if (ranged && !in_range(event, first, last)) {
            json_object_put(event);
            continue;
        }
        json_object *view = public_event(event);
        json_object_put(event);
        size_t n = strlen(json_object_to_json_string_ext(view, JSON_C_TO_STRING_PLAIN));
        if (json_object_array_length(items) >= (size_t)limit ||
            (json_object_array_length(items) > 0 && bytes + n > 100 * 1024)) {
            json_object_put(view);
            more = true;
            break;
        }
        json_object_array_add(items, view);
        bytes += n;
        free(cursor);
        cursor = strdup(id);
        if (!cursor) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(s);
    form_free(&form);
    if (!ok) {
        free(calendar);
        free(cursor);
        json_object_put(items);
        *status = 500;
        return error_json("cache_integrity_failed");
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "items", items);
    addstr(o, "calendar_id", calendar);
    json_object_object_add(o, "next_after_id",
                           more && cursor ? json_object_new_string(cursor) : NULL);
    addstr(o, "recurrence_mode", "masters_and_exceptions");
    sqlite3_stmt *meta = prepare("SELECT last_sync FROM accounts WHERE user_id=?");
    if (meta) {
        sqlite3_bind_int64(meta, 1, user);
        if (sqlite3_step(meta) == SQLITE_ROW)
            addint(o, "last_sync", sqlite3_column_int64(meta, 0));
        sqlite3_finalize(meta);
    }
    free(calendar);
    free(cursor);
    return o;
}
static json_object *changes_list(sqlite3_int64 user, int *status) {
    sqlite3_stmt *s = prepare("SELECT id,idempotency_key,event_id,state,error,attempts FROM "
                              "changes WHERE user_id=? ORDER BY id DESC LIMIT 101");
    if (!s) {
        *status = 500;
        return error_json("queue_read_failed");
    }
    sqlite3_bind_int64(s, 1, user);
    json_object *items = json_object_new_array();
    size_t bytes = 0;
    bool more = false;
    while (sqlite3_step(s) == SQLITE_ROW) {
        json_object *item = change_json(s, false);
        size_t n = strlen(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN));
        if (json_object_array_length(items) >= 100 || bytes + n > 90 * 1024) {
            json_object_put(item);
            more = true;
            break;
        }
        bytes += n;
        json_object_array_add(items, item);
    }
    sqlite3_finalize(s);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "items", items);
    addbool(o, "truncated", more);
    return o;
}
static json_object *disconnect_user(sqlite3_int64 user, int *status) {
    json_object *tokens = load_tokens(user);
    bool revoked = false;
    if (config.configured && tokens) {
        const char *names[] = {"token"};
        const char *values[] = {jstr(tokens, "refresh_token")};
        char *body = form_encode(names, values, 1);
        if (body) {
            Http h = http("POST", config.revoke_url, NULL, body, NULL, true);
            revoked = h.transport == CURLE_OK && h.status == 200;
            http_free(&h);
            OPENSSL_cleanse(body, strlen(body));
            free(body);
        }
    }
    if (tokens)
        json_object_put(tokens);
    bool ok = execute("BEGIN IMMEDIATE");
    const char *queries[] = {"DELETE FROM oauth_states WHERE user_id=?",
                             "DELETE FROM accounts WHERE user_id=?"};
    for (size_t i = 0; ok && i < 2; i++) {
        sqlite3_stmt *s = prepare(queries[i]);
        ok = s != NULL;
        if (s) {
            sqlite3_bind_int64(s, 1, user);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    if (ok)
        ok = execute("COMMIT");
    else
        (void)execute("ROLLBACK");
    if (!ok) {
        *status = 500;
        return error_json("disconnect_failed");
    }
    json_object *o = json_object_new_object();
    addbool(o, "disconnected", true);
    addbool(o, "remote_revoked", revoked);
    return o;
}
static void stop_server(int signal_number) {
    (void)signal_number;
    stopping = 1;
}
static bool source_allowed(const char *address) {
    const char *start = config.allowed_client;
    size_t n = strlen(address);
    while (*start) {
        const char *end = strchr(start, ',');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (n == len && !strncmp(start, address, n))
            return true;
        if (!end)
            break;
        start = end + 1;
    }
    return false;
}
static bool write_all(SSL *tls, const char *text, size_t length) {
    while (length) {
        int n = SSL_write(tls, text, (int)length);
        if (n <= 0)
            return false;
        text += n;
        length -= (size_t)n;
    }
    return true;
}
static void respond(SSL *tls, int status, json_object *object) {
    const char *json = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    const char *reason = status == 200   ? "OK"
                         : status == 202 ? "Accepted"
                         : status == 400 ? "Bad Request"
                         : status == 401 ? "Unauthorized"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 405 ? "Method Not Allowed"
                         : status == 409 ? "Conflict"
                         : status == 413 ? "Payload Too Large"
                         : status == 415 ? "Unsupported Media Type"
                         : status == 502 ? "Bad Gateway"
                         : status == 503 ? "Service Unavailable"
                                         : "Internal Server Error";
    char header[512];
    int n = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: "
        "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n",
        status, reason, strlen(json), status == 401 ? "WWW-Authenticate: Bearer\r\n" : "");
    if (n > 0 && (size_t)n < sizeof(header) && write_all(tls, header, (size_t)n))
        (void)write_all(tls, json, strlen(json));
}
static int bounded_read(SSL *tls, void *buffer, int cap, struct timespec deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    long remaining =
        (deadline.tv_sec - now.tv_sec) * 1000000L + (deadline.tv_nsec - now.tv_nsec) / 1000L;
    if (remaining <= 0)
        return -1;
    struct timeval timeout = {remaining / 1000000L, remaining % 1000000L};
    if (setsockopt(SSL_get_fd(tls), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
        return -1;
    return SSL_read(tls, buffer, cap);
}
/* Only one HTTP/1 request per TLS connection, no chunking or ambiguous framing. */
static int receive_request(SSL *tls, Request *request) {
    char buffer[HEADER_LIMIT + BODY_LIMIT + 1];
    size_t count = 0, header_size = 0;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline))
        return 400;
    deadline.tv_sec += 10;
    memset(request, 0, sizeof(*request));
    while (!header_size) {
        if (count >= HEADER_LIMIT)
            return 413;
        int got = bounded_read(tls, buffer + count, (int)(HEADER_LIMIT - count), deadline);
        if (got <= 0)
            return 400;
        count += (size_t)got;
        buffer[count] = 0;
        if (memchr(buffer, 0, count))
            return 400;
        char *end = strstr(buffer, "\r\n\r\n");
        if (end)
            header_size = (size_t)(end + 4 - buffer);
    }
    char *line_end = strstr(buffer, "\r\n");
    if (!line_end)
        return 400;
    *line_end = 0;
    char version[16], extra;
    if (sscanf(buffer, "%11s %4095s %15s %c", request->method, request->path, version, &extra) !=
            3 ||
        strcmp(version, "HTTP/1.1"))
        return 400;
    bool have_length = false, have_authorization = false, have_type = false, have_host = false;
    int content_length = 0;
    char *line = line_end + 2;
    while (line < buffer + header_size - 2) {
        char *end = strstr(line, "\r\n");
        if (!end)
            return 400;
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line || isspace((unsigned char)line[0]))
            return 400;
        *colon = 0;
        for (const unsigned char *p = (const unsigned char *)line; *p; p++)
            if (!(isalnum(*p) || *p == '-'))
                return 400;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *last = value + strlen(value);
        while (last > value && (last[-1] == ' ' || last[-1] == '\t'))
            *--last = 0;
        for (const unsigned char *p = (const unsigned char *)value; *p; p++)
            if (*p < 32 && *p != '\t')
                return 400;
        if (!strcasecmp(line, "Transfer-Encoding") || !strcasecmp(line, "Expect"))
            return 400;
        if (!strcasecmp(line, "Host")) {
            if (have_host || !*value)
                return 400;
            have_host = true;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (have_length || !number(value, 0, BODY_LIMIT, &content_length))
                return 400;
            have_length = true;
        } else if (!strcasecmp(line, "Authorization")) {
            if (have_authorization || strlen(value) >= sizeof(request->authorization))
                return 400;
            strcpy(request->authorization, value);
            have_authorization = true;
        } else if (!strcasecmp(line, "Content-Type")) {
            if (have_type || strlen(value) >= sizeof(request->content_type))
                return 400;
            strcpy(request->content_type, value);
            have_type = true;
        }
        line = end + 2;
    }
    if (!have_host || (!strcmp(request->method, "POST") && !have_length))
        return 400;
    size_t body_bytes = count - header_size;
    if (body_bytes > (size_t)content_length)
        return 400;
    memcpy(request->body, buffer + header_size, body_bytes);
    while (body_bytes < (size_t)content_length) {
        int got = bounded_read(tls, request->body + body_bytes, content_length - (int)body_bytes,
                               deadline);
        if (got <= 0)
            return 400;
        body_bytes += (size_t)got;
    }
    if (memchr(request->body, 0, body_bytes))
        return 400;
    request->body[body_bytes] = 0;
    request->length = body_bytes;
    return 0;
}

static json_object *route(Request *request, const char *address, int *status) {
    bool health = !strcmp(request->path, "/healthz") && !strcmp(request->method, "GET");
    if (health && (source_allowed(address) || !strcmp(address, config.bind_address) ||
                   !strcmp(address, "127.0.0.1"))) {
        json_object *o = json_object_new_object();
        addstr(o, "status", "ok");
        return o;
    }
    if (!source_allowed(address)) {
        *status = 403;
        return error_json("source_not_allowed");
    }
    const char *auth = request->authorization;
    size_t n = strlen(config.token);
    if (strlen(auth) != n + 7 || strncmp(auth, "Bearer ", 7) ||
        CRYPTO_memcmp(auth + 7, config.token, n)) {
        *status = 401;
        return error_json("unauthorized");
    }
    char *query = strchr(request->path, '?');
    if (query)
        *query++ = 0;
    else
        query = request->path + strlen(request->path);
    if (!strcmp(request->path, "/v1/status") && !strcmp(request->method, "GET") && !*query)
        return status_json(0);
    const char *prefix = "/v1/users/";
    if (strncmp(request->path, prefix, strlen(prefix))) {
        *status = 404;
        return error_json("not_found");
    }
    char *user_text = request->path + strlen(prefix), *suffix = strchr(user_text, '/');
    if (!suffix) {
        *status = 404;
        return error_json("not_found");
    }
    *suffix++ = 0;
    int user_number;
    if (!number(user_text, 1, 2147483647, &user_number)) {
        *status = 400;
        return error_json("invalid_user");
    }
    sqlite3_int64 user = user_number;
    if (!strcmp(request->method, "GET")) {
        if (!strcmp(suffix, "status") && !*query)
            return status_json(user);
        if (!strcmp(suffix, "calendars"))
            return calendars(user, query, status);
        if (!strcmp(suffix, "events"))
            return events_list(user, query, status);
        if (!strcmp(suffix, "changes") && !*query)
            return changes_list(user, status);
        *status = 404;
        return error_json("not_found");
    }
    if (strcmp(request->method, "POST")) {
        *status = 405;
        return error_json("method_not_allowed");
    }
    if (*query) {
        *status = 400;
        return error_json("invalid_query");
    }
    if (strcasecmp(request->content_type, "application/x-www-form-urlencoded")) {
        *status = 415;
        return error_json("unsupported_media_type");
    }
    Form form;
    if (!form_parse(request->body, &form)) {
        *status = 400;
        return error_json("invalid_form");
    }
    json_object *o = NULL;
    if (!strcmp(suffix, "oauth/start"))
        o = oauth_start(user, &form, status);
    else if (!strcmp(suffix, "oauth/callback"))
        o = oauth_callback(user, &form, status);
    else if (!strcmp(suffix, "selection"))
        o = selection(user, &form, status);
    else if (!strcmp(suffix, "events/create"))
        o = enqueue(user, "create", &form, status);
    else if (!strcmp(suffix, "events/update"))
        o = enqueue(user, "update", &form, status);
    else if (!strcmp(suffix, "events/delete"))
        o = enqueue(user, "delete", &form, status);
    else if (!strcmp(suffix, "sync") && !form.count)
        o = request_sync(user, status);
    else if (!strcmp(suffix, "disconnect") && !form.count)
        o = disconnect_user(user, status);
    else {
        *status = 404;
        o = error_json("not_found");
    }
    form_free(&form);
    return o;
}
static bool key_check(void) {
    if (!execute("CREATE TABLE IF NOT EXISTS metadata(name TEXT PRIMARY KEY,value BLOB NOT NULL)"))
        return false;
    sqlite3_stmt *s = prepare("SELECT value FROM metadata WHERE name='key_check'");
    if (!s)
        return false;
    int row = sqlite3_step(s);
    bool ok = false;
    if (row == SQLITE_ROW) {
        char *plain = unseal(0, "key_check", sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
        ok = plain && !strcmp(plain, "punto-calendar-key-v1");
        free(plain);
        sqlite3_finalize(s);
        return ok;
    }
    sqlite3_finalize(s);
    if (row != SQLITE_DONE)
        return false;
    /* Existing records cannot be silently reset by introducing a different key. */
    s = prepare("SELECT user_id,token_blob FROM accounts");
    if (!s)
        return false;
    ok = true;
    while (sqlite3_step(s) == SQLITE_ROW) {
        char *plain = unseal(sqlite3_column_int64(s, 0), "tokens", sqlite3_column_blob(s, 1),
                             sqlite3_column_bytes(s, 1));
        if (!plain) {
            ok = false;
            break;
        }
        OPENSSL_cleanse(plain, strlen(plain));
        free(plain);
    }
    sqlite3_finalize(s);
    if (!ok)
        return false;
    int n = 0;
    char *blob = seal(0, "key_check", "punto-calendar-key-v1", &n);
    s = blob ? prepare("INSERT INTO metadata(name,value) VALUES('key_check',?)") : NULL;
    ok = s != NULL;
    if (s) {
        sqlite3_bind_blob(s, 1, blob, n, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
    }
    free(blob);
    return ok;
}
int main(void) {
    umask(0077);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
    if (!load_config()) {
        fprintf(stderr, "Invalid calendar configuration\n");
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || !init_database() || !key_check()) {
        fprintf(stderr, "Calendar encrypted store unavailable\n");
        return 1;
    }
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) ||
        SSL_CTX_use_certificate_chain_file(context, config.cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, config.key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1) {
        fprintf(stderr, "Calendar TLS unavailable\n");
        return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    struct sockaddr_in bind_address = {0};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)config.port);
    if (listener < 0 || inet_pton(AF_INET, config.bind_address, &bind_address.sin_addr) != 1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        bind(listener, (struct sockaddr *)&bind_address, sizeof(bind_address)) ||
        listen(listener, 32)) {
        fprintf(stderr, "Calendar listener unavailable\n");
        return 1;
    }
    sqlite3 *worker_db = NULL;
    if (sqlite3_open_v2(config.database, &worker_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK ||
        sqlite3_exec(worker_db,
                     "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON", NULL,
                     NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Calendar worker store unavailable\n");
        return 1;
    }
    sqlite3_busy_timeout(worker_db, 3000);
    (void)execute("UPDATE accounts SET sync_running=0");
    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_loop, worker_db)) {
        fprintf(stderr, "Calendar worker unavailable\n");
        sqlite3_close(worker_db);
        return 1;
    }
    fprintf(stderr, "Calendar API ready; OAuth %s\n",
            config.configured ? "configured" : "not configured");
    while (!stopping) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        struct timeval wait = {1, 0};
        int ready = select(listener + 1, &set, NULL, NULL, &wait);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready > 0) {
            struct sockaddr_in peer;
            socklen_t peer_size = sizeof(peer);
            int fd = accept(listener, (struct sockaddr *)&peer, &peer_size);
            if (fd < 0)
                continue;
            struct timeval timeout = {10, 0};
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            SSL *tls = SSL_new(context);
            if (tls) {
                SSL_set_fd(tls, fd);
                if (SSL_accept(tls) == 1) {
                    char address[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
                    Request *request = calloc(1, sizeof(*request));
                    int status = request ? receive_request(tls, request) : 500;
                    json_object *o = status ? error_json("invalid_request") : NULL;
                    if (!status) {
                        status = 200;
                        o = route(request, address, &status);
                    }
                    respond(tls, status, o);
                    json_object_put(o);
                    if (request) {
                        OPENSSL_cleanse(request, sizeof(*request));
                        free(request);
                    }
                }
                SSL_free(tls);
            }
            close(fd);
        }
    }
    stopping = 1;
    (void)pthread_join(worker, NULL);
    close(listener);
    SSL_CTX_free(context);
    sqlite3_close(database);
    curl_global_cleanup();
    OPENSSL_cleanse(&config, sizeof(config));
    return 0;
}
