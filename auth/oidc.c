#include "oidc.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define URL_SIZE 2048U
#define RESPONSE_LIMIT (256U * 1024U)
#define TOKEN_LIMIT 32768U
#define REPLAY_LIMIT_PER_SUBJECT 64U
#define RANDOM_SIZE 32U
#define RANDOM_TEXT 43U
#define TRANSACTION_SECONDS 300
#define COOKIE_IV_SIZE 12U
#define COOKIE_TAG_SIZE 16U
#define TRANSACTION_PAYLOAD_SIZE (1U + 8U + 3U * RANDOM_TEXT)
#define TRANSACTION_ENVELOPE_SIZE (COOKIE_IV_SIZE + TRANSACTION_PAYLOAD_SIZE + COOKIE_TAG_SIZE)
#define TRANSACTION_COOKIE_SIZE (4U * ((TRANSACTION_ENVELOPE_SIZE + 2U) / 3U) + 1U)
#define SESSION_SECONDS (8 * 60 * 60)
#define CLOCK_SKEW 60

typedef struct {
    char state[RANDOM_TEXT + 1], nonce[RANDOM_TEXT + 1];
    char verifier[RANDOM_TEXT + 1];
    time_t created, expires;
} Transaction;

typedef struct Replay Replay;
struct Replay {
    char state[RANDOM_TEXT + 1], subject[256];
    time_t expires;
    Replay *next;
};

static struct {
    bool initialized, enabled, local, managed, curl_ready;
    char issuer[URL_SIZE], backchannel[URL_SIZE], client[129], redirect[URL_SIZE];
    char users_admin_url[URL_SIZE], local_admin_email[255];
    char role_claim[129], admin_role[16], user_role[16];
    char cookie_name[48], clear_cookie[512];
    char *secret, *ca;
    struct json_object *bindings;
    unsigned char cookie_key[32];
    Replay *replays;
} config;

static void erase(void *data, size_t size) {
    OPENSSL_cleanse(data, size);
}

static bool text_value(struct json_object *value, const char **text, size_t max) {
    if (!value || !json_object_is_type(value, json_type_string)) return false;
    const char *s = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!length || length > max || strlen(s) != length) return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127) return false;
    *text = s;
    return true;
}

static bool member_text(struct json_object *object, const char *key,
                        const char **text, size_t maximum) {
    struct json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && text_value(value, text, maximum);
}

static bool member_integer(struct json_object *object, const char *key, int64_t *out) {
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_int)) return false;
    *out = json_object_get_int64(value);
    return true;
}

/* json-c intentionally accepts duplicate object keys. Reject them before
   parsing security claims or administrator bindings, including escaped keys. */
typedef struct { const char *p, *end; unsigned depth; } JsonScan;

static void whitespace(JsonScan *scan) {
    while (scan->p < scan->end && (*scan->p == ' ' || *scan->p == '\t' ||
           *scan->p == '\r' || *scan->p == '\n')) scan->p++;
}

static bool scan_string(JsonScan *scan, const char **start, size_t *length) {
    if (scan->p == scan->end || *scan->p != '"') return false;
    const char *begin = scan->p++;
    while (scan->p < scan->end) {
        unsigned char c = (unsigned char)*scan->p++;
        if (c == '"') {
            if (start) *start = begin;
            if (length) *length = (size_t)(scan->p - begin);
            return true;
        }
        if (c < 32) return false;
        if (c == '\\') {
            if (scan->p == scan->end) return false;
            char escaped = *scan->p++;
            if (escaped == 'u') {
                if (scan->end - scan->p < 4) return false;
                bool zero = true;
                for (unsigned i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)scan->p[i])) return false;
                    if (scan->p[i] != '0') zero = false;
                }
                if (zero) return false;
                scan->p += 4;
            } else if (!strchr("\"\\/bfnrt", escaped)) return false;
        }
    }
    return false;
}

static bool scan_value(JsonScan *scan);

static bool scan_object(JsonScan *scan) {
    char **keys = NULL;
    size_t count = 0;
    bool okay = false;
    scan->p++;
    whitespace(scan);
    if (scan->p < scan->end && *scan->p == '}') { scan->p++; return true; }
    while (scan->p < scan->end) {
        const char *start;
        size_t length;
        if (count >= 1024 || !scan_string(scan, &start, &length) || length > 1538) goto done;
        struct json_tokener *tokener = json_tokener_new_ex(4);
        if (!tokener) goto done;
        json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        struct json_object *key = json_tokener_parse_ex(tokener, start, (int)length);
        bool valid = json_tokener_get_error(tokener) == json_tokener_success && key &&
            json_object_is_type(key, json_type_string) &&
            json_object_get_string_len(key) <= 255 &&
            strlen(json_object_get_string(key)) == (size_t)json_object_get_string_len(key);
        json_tokener_free(tokener);
        if (!valid) { json_object_put(key); goto done; }
        const char *name = json_object_get_string(key);
        for (size_t i = 0; i < count; i++)
            if (!strcmp(keys[i], name)) { json_object_put(key); goto done; }
        char *copy = strdup(name);
        json_object_put(key);
        if (!copy) goto done;
        char **grown = realloc(keys, (count + 1) * sizeof(*keys));
        if (!grown) { free(copy); goto done; }
        keys = grown;
        keys[count++] = copy;
        whitespace(scan);
        if (scan->p == scan->end || *scan->p++ != ':') goto done;
        if (!scan_value(scan)) goto done;
        whitespace(scan);
        if (scan->p == scan->end) goto done;
        char delimiter = *scan->p++;
        if (delimiter == '}') { okay = true; break; }
        if (delimiter != ',') goto done;
        whitespace(scan);
    }
done:
    for (size_t i = 0; i < count; i++) free(keys[i]);
    free(keys);
    return okay;
}

static bool scan_value(JsonScan *scan) {
    whitespace(scan);
    if (scan->p == scan->end || ++scan->depth > 16) return false;
    bool okay = false;
    if (*scan->p == '{') okay = scan_object(scan);
    else if (*scan->p == '"') okay = scan_string(scan, NULL, NULL);
    else if (*scan->p == '[') {
        scan->p++;
        whitespace(scan);
        if (scan->p < scan->end && *scan->p == ']') { scan->p++; okay = true; }
        else for (size_t i = 0; i < 1024; i++) {
            if (!scan_value(scan)) break;
            whitespace(scan);
            if (scan->p == scan->end) break;
            char delimiter = *scan->p++;
            if (delimiter == ']') { okay = true; break; }
            if (delimiter != ',') break;
        }
    } else {
        const char *start = scan->p;
        while (scan->p < scan->end && !strchr(" \t\r\n,]}", *scan->p)) scan->p++;
        okay = scan->p != start;
    }
    scan->depth--;
    return okay;
}

static struct json_object *parse_json(const char *data, size_t length) {
    if (!data || !length || length > RESPONSE_LIMIT || memchr(data, '\0', length)) return NULL;
    JsonScan scan = {data, data + length, 0};
    if (!scan_value(&scan)) return NULL;
    whitespace(&scan);
    if (scan.p != scan.end) return NULL;
    struct json_tokener *tokener = json_tokener_new_ex(16);
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *result = json_tokener_parse_ex(tokener, data, (int)length);
    bool okay = json_tokener_get_error(tokener) == json_tokener_success && result &&
        json_object_is_type(result, json_type_object) &&
        json_tokener_get_parse_end(tokener) == length;
    json_tokener_free(tokener);
    if (!okay) { json_object_put(result); return NULL; }
    return result;
}

static char *read_file(const char *path, size_t limit, size_t *size) {
    if (!path || path[0] != '/') return NULL;
    /* Some platforms hide O_NOFOLLOW with strict POSIX feature flags. Verify
       the directory entry and opened inode as well as using it when exposed. */
    struct stat entry;
    if (lstat(path, &entry) || !S_ISREG(entry.st_mode)) return NULL;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_dev != entry.st_dev || st.st_ino != entry.st_ino ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) || st.st_size <= 0 ||
        (uint64_t)st.st_size > limit) { close(fd); return NULL; }
    size_t length = (size_t)st.st_size;
    char *data = malloc(length + 1);
    if (!data) { close(fd); return NULL; }
    size_t received = 0;
    while (received < length) {
        ssize_t count = read(fd, data + received, length - received);
        if (count <= 0) { erase(data, length); free(data); close(fd); return NULL; }
        received += (size_t)count;
    }
    char extra;
    bool complete = read(fd, &extra, 1) == 0;
    close(fd);
    if (!complete || memchr(data, '\0', length)) { erase(data, length); free(data); return NULL; }
    data[length] = '\0';
    *size = length;
    return data;
}

static bool canonical_email(const char *input, char output[255]) {
    size_t length = input ? strlen(input) : 0;
    if (!length || length > 254) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        output[i] = (char)c;
    }
    output[length] = '\0';
    char *at = strchr(output, '@');
    if (!at || at == output || !at[1] || strchr(at + 1, '@') || at - output > 64 ||
        output[0] == '.' || at[-1] == '.') return false;
    for (char *p = output; p < at; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              strchr(".!#$%&'*+/=?^_`{|}~-", c)) || (c == '.' && p[1] == '.')) return false;
    }
    char *label = at + 1;
    for (char *p = label; ; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || !c) {
            size_t len = (size_t)(p - label);
            if (!len || len > 63 || *label == '-' || p[-1] == '-') return false;
            if (!c) break;
            label = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static bool identifier(const char *text, size_t maximum) {
    size_t length = text ? strlen(text) : 0;
    if (!length || length > maximum) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

static bool loopback_or_keycloak(const char *host) {
    return !strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") ||
        !strcmp(host, "[::1]") || !strcmp(host, "keycloak");
}

/* kind: 0 callback, 1 realm issuer, 2 administrator console. */
static bool validate_url(const char *url, bool allow_http, int kind, char path[512]) {
    if (!url || !*url || strlen(url) >= URL_SIZE || strpbrk(url, "\r\n\t\\?#")) return false;
    /* Keep the exact configured issuer/redirect representation unambiguous;
       cookie security and emitted redirects use this same canonical scheme. */
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p >= 127) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = NULL, *host = NULL, *port = NULL, *part = NULL, *user = NULL, *password = NULL;
    bool okay = curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PATH, &part, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_NO_USER &&
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) == CURLUE_NO_PASSWORD;
    if (okay) okay = !strcmp(scheme, "https") ||
        (!strcmp(scheme, "http") && allow_http && loopback_or_keycloak(host));
    CURLUcode port_status = curl_url_get(parsed, CURLUPART_PORT, &port, 0);
    if (okay && port_status == CURLUE_OK) {
        unsigned number = 0;
        for (const char *p = port; *p; p++) {
            if (*p < '0' || *p > '9' || number > 65535U / 10U) { okay = false; break; }
            number = number * 10U + (unsigned)(*p - '0');
        }
        if (!number || number > 65535) okay = false;
    } else if (port_status != CURLUE_NO_PORT) okay = false;
    if (okay && kind == 1) okay = !strncmp(part, "/realms/", 8) &&
        identifier(part + 8, 255) && strcmp(part + 8, ".") && strcmp(part + 8, "..");
    if (okay && kind == 0) okay = !strcmp(part, "/auth/callback");
    if (okay) {
        size_t length = strlen(part);
        if (length >= 512) okay = false;
        else memcpy(path, part, length + 1);
    }
    curl_free(scheme); curl_free(host); curl_free(port); curl_free(part);
    curl_free(user); curl_free(password); curl_url_cleanup(parsed);
    return okay;
}

static bool base64url(const unsigned char *data, size_t size, char *output, size_t capacity) {
    if (size > INT_MAX || capacity < 4 * ((size + 2) / 3) + 1) return false;
    int length = EVP_EncodeBlock((unsigned char *)output, data, (int)size);
    if (length < 0) return false;
    for (int i = 0; i < length; i++) {
        if (output[i] == '+') output[i] = '-';
        else if (output[i] == '/') output[i] = '_';
    }
    while (length && output[length - 1] == '=') length--;
    output[length] = '\0';
    return true;
}

static unsigned char *unbase64url(const char *text, size_t size, size_t *decoded) {
    if (!size || size > TOKEN_LIMIT || size % 4 == 1) return NULL;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return NULL;
    }
    size_t padded = ((size + 3) / 4) * 4;
    char *input = malloc(padded + 1);
    unsigned char *output = malloc(padded / 4 * 3 + 1);
    if (!input || !output) { free(input); free(output); return NULL; }
    for (size_t i = 0; i < size; i++) input[i] = text[i] == '-' ? '+' : text[i] == '_' ? '/' : text[i];
    for (size_t i = size; i < padded; i++) input[i] = '=';
    input[padded] = '\0';
    int length = EVP_DecodeBlock(output, (const unsigned char *)input, (int)padded);
    free(input);
    if (length < 0) { free(output); return NULL; }
    *decoded = (size_t)length - (padded - size);
    output[*decoded] = '\0';
    /* Reject non-canonical encodings with unused nonzero low bits. */
    char *canonical = malloc(padded + 1);
    bool okay = canonical && base64url(output, *decoded, canonical, padded + 1) &&
        strlen(canonical) == size && !memcmp(canonical, text, size);
    free(canonical);
    if (!okay) { free(output); return NULL; }
    return output;
}

static bool random_text(char output[RANDOM_TEXT + 1]) {
    unsigned char random[RANDOM_SIZE];
    char encoded[45];
    bool okay = RAND_bytes(random, sizeof(random)) == 1 &&
        base64url(random, sizeof(random), encoded, sizeof(encoded));
    if (okay) memcpy(output, encoded, RANDOM_TEXT + 1);
    erase(random, sizeof(random)); erase(encoded, sizeof(encoded));
    return okay;
}

/* Pending logins live only in an authenticated, encrypted browser cookie.
   Anonymous starts allocate no server-side slots. A fresh process key means
   restarts invalidate pending logins, as they did with the old memory pool. */
static bool seal_transaction(const Transaction *transaction, char output[TRANSACTION_COOKIE_SIZE]) {
    unsigned char plain[TRANSACTION_PAYLOAD_SIZE] = {1};
    unsigned char envelope[TRANSACTION_ENVELOPE_SIZE] = {0};
    uint64_t created = (uint64_t)transaction->created;
    for (size_t i = 0; i < 8; i++) plain[1 + i] = (unsigned char)(created >> (56 - 8 * i));
    memcpy(plain + 9, transaction->state, RANDOM_TEXT);
    memcpy(plain + 9 + RANDOM_TEXT, transaction->nonce, RANDOM_TEXT);
    memcpy(plain + 9 + 2 * RANDOM_TEXT, transaction->verifier, RANDOM_TEXT);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int size = 0, final_size = 0;
    bool okay = context && RAND_bytes(envelope, COOKIE_IV_SIZE) == 1 &&
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), NULL, config.cookie_key, envelope) == 1 &&
        EVP_EncryptUpdate(context, NULL, &size, (const unsigned char *)config.cookie_name,
                          (int)strlen(config.cookie_name)) == 1 &&
        EVP_EncryptUpdate(context, envelope + COOKIE_IV_SIZE, &size, plain, sizeof(plain)) == 1 &&
        size == (int)sizeof(plain) &&
        EVP_EncryptFinal_ex(context, envelope + COOKIE_IV_SIZE + size, &final_size) == 1 &&
        final_size == 0 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, COOKIE_TAG_SIZE,
                            envelope + COOKIE_IV_SIZE + TRANSACTION_PAYLOAD_SIZE) == 1 &&
        base64url(envelope, sizeof(envelope), output, TRANSACTION_COOKIE_SIZE);
    EVP_CIPHER_CTX_free(context);
    erase(plain, sizeof(plain)); erase(envelope, sizeof(envelope));
    return okay;
}

static bool open_transaction(const char *cookie, time_t now, Transaction *transaction) {
    size_t size = 0;
    unsigned char *envelope = unbase64url(cookie, strlen(cookie), &size);
    unsigned char plain[TRANSACTION_PAYLOAD_SIZE] = {0};
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int plain_size = 0, final_size = 0;
    bool okay = envelope && size == TRANSACTION_ENVELOPE_SIZE && context &&
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, config.cookie_key, envelope) == 1 &&
        EVP_DecryptUpdate(context, NULL, &plain_size, (const unsigned char *)config.cookie_name,
                          (int)strlen(config.cookie_name)) == 1 &&
        EVP_DecryptUpdate(context, plain, &plain_size, envelope + COOKIE_IV_SIZE,
                          TRANSACTION_PAYLOAD_SIZE) == 1 &&
        plain_size == (int)sizeof(plain) &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, COOKIE_TAG_SIZE,
                            envelope + COOKIE_IV_SIZE + TRANSACTION_PAYLOAD_SIZE) == 1 &&
        EVP_DecryptFinal_ex(context, plain + plain_size, &final_size) == 1 && final_size == 0 &&
        plain[0] == 1;
    uint64_t created = 0;
    if (okay) {
        for (size_t i = 0; i < 8; i++) created = (created << 8) | plain[1 + i];
        okay = now >= 0 && created <= (uint64_t)now &&
            (uint64_t)now - created < TRANSACTION_SECONDS &&
            created <= (uint64_t)INT64_MAX - TRANSACTION_SECONDS;
    }
    if (okay) {
        transaction->created = (time_t)created;
        transaction->expires = (time_t)(created + TRANSACTION_SECONDS);
        memcpy(transaction->state, plain + 9, RANDOM_TEXT);
        memcpy(transaction->nonce, plain + 9 + RANDOM_TEXT, RANDOM_TEXT);
        memcpy(transaction->verifier, plain + 9 + 2 * RANDOM_TEXT, RANDOM_TEXT);
    }
    EVP_CIPHER_CTX_free(context);
    if (envelope) { erase(envelope, size); free(envelope); }
    erase(plain, sizeof(plain));
    return okay;
}

static void prune_replays(time_t now) {
    Replay **link = &config.replays;
    while (*link) {
        Replay *entry = *link;
        if (entry->expires <= now) {
            *link = entry->next;
            erase(entry, sizeof(*entry)); free(entry);
        } else link = &entry->next;
    }
}

/* Only a cryptographically verified IdP identity may allocate a tombstone.
   Invalid codes, provider errors and anonymous starts cannot consume this
   cache. Live records are never evicted; the short-lived quota affects only
   the authenticated subject exceeding it, and cannot evict other logins. */
static int remember_transaction(const Transaction *transaction, const char *subject) {
    time_t now = time(NULL);
    if (now < 0) return 503;
    prune_replays(now);
    if (transaction->expires <= now) return 401;
    size_t count = 0;
    for (Replay *entry = config.replays; entry; entry = entry->next) {
        if (!CRYPTO_memcmp(entry->state, transaction->state, RANDOM_TEXT)) return 401;
        if (!strcmp(entry->subject, subject)) count++;
    }
    if (count >= REPLAY_LIMIT_PER_SUBJECT) return 429;
    Replay *entry = calloc(1, sizeof(*entry));
    if (!entry) return 503;
    memcpy(entry->state, transaction->state, sizeof(entry->state));
    memcpy(entry->subject, subject, strlen(subject) + 1);
    entry->expires = transaction->expires;
    entry->next = config.replays;
    config.replays = entry;
    return 200;
}

void oidc_close(void) {
    if (config.secret) { erase(config.secret, strlen(config.secret)); free(config.secret); }
    free(config.ca);
    json_object_put(config.bindings);
    if (config.curl_ready) curl_global_cleanup();
    while (config.replays) {
        Replay *entry = config.replays;
        config.replays = entry->next;
        erase(entry, sizeof(*entry)); free(entry);
    }
    erase(&config, sizeof(config));
}

bool oidc_init(void) {
    oidc_close();
    const char *management = getenv("AUTH_USER_MANAGEMENT");
    if (management && *management && strcmp(management, "local") && strcmp(management, "keycloak")) return false;
    config.managed = management && !strcmp(management, "keycloak");
    const char *mode = getenv("AUTH_MODE");
    if (!mode || !*mode || !strcmp(mode, "local")) {
        if (config.managed) goto invalid;
        config.initialized = config.local = true;
        return true;
    }
    if (strcmp(mode, "hybrid") && strcmp(mode, "oidc")) return false;
    config.local = !strcmp(mode, "hybrid");
    config.enabled = true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) goto invalid;
    config.curl_ready = true;
    if (config.managed) {
        const char *admin = getenv("OIDC_ADMIN_ROLE"), *user = getenv("OIDC_USER_ROLE");
        const char *claim = getenv("OIDC_ROLE_CLAIM"), *console = getenv("OIDC_USERS_ADMIN_URL");
        if (!claim || !*claim) claim = "app_roles";
        if (!identifier(admin, 15) || !identifier(user, 15) || !strcmp(admin, user) ||
            !identifier(claim, 128)) goto invalid;
        memcpy(config.admin_role, admin, strlen(admin) + 1);
        memcpy(config.user_role, user, strlen(user) + 1);
        memcpy(config.role_claim, claim, strlen(claim) + 1);
        if (config.local && !canonical_email(getenv("AUTH_LOCAL_ADMIN_EMAIL"), config.local_admin_email)) goto invalid;
        if (console && *console) {
            char console_path[512];
            if (!validate_url(console, false, 2, console_path)) goto invalid;
            memcpy(config.users_admin_url, console, strlen(console) + 1);
        }
    }
    const char *issuer = getenv("OIDC_ISSUER_URL"), *backend = getenv("OIDC_BACKCHANNEL_URL");
    const char *client = getenv("OIDC_CLIENT_ID"), *redirect = getenv("OIDC_REDIRECT_URI");
    const char *http = getenv("OIDC_ALLOW_HTTP");
    if (http && *http && strcmp(http, "0") && strcmp(http, "1")) goto invalid;
    bool allow_http = http && !strcmp(http, "1");
    if (!backend || !*backend) backend = issuer;
    char issuer_path[512], backend_path[512], redirect_path[512];
    if (!validate_url(issuer, allow_http, true, issuer_path) ||
        !validate_url(backend, allow_http, true, backend_path) ||
        strcmp(issuer_path, backend_path) ||
        !validate_url(redirect, allow_http, false, redirect_path) || !identifier(client, 128) ||
        (config.managed && strlen(issuer) >= sizeof(((OidcIdentity *)0)->issuer))) goto invalid;
    memcpy(config.issuer, issuer, strlen(issuer) + 1);
    memcpy(config.backchannel, backend, strlen(backend) + 1);
    memcpy(config.client, client, strlen(client) + 1);
    memcpy(config.redirect, redirect, strlen(redirect) + 1);
    size_t length = 0;
    config.secret = read_file(getenv("OIDC_CLIENT_SECRET_FILE"), 4096, &length);
    if (!config.secret) goto invalid;
    if (length && config.secret[length - 1] == '\n') config.secret[--length] = '\0';
    if (!length) goto invalid;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)config.secret[i] < 33 || (unsigned char)config.secret[i] > 126) goto invalid;
    const char *ca = getenv("OIDC_CA_FILE");
    if (ca && *ca) {
        size_t ca_size = 0;
        char *ca_data = read_file(ca, 1024U * 1024U, &ca_size);
        if (!ca_data) goto invalid;
        BIO *bio = BIO_new_mem_buf(ca_data, (int)ca_size);
        X509 *certificate = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
        BIO_free(bio); free(ca_data);
        if (!certificate) goto invalid;
        X509_free(certificate);
        config.ca = strdup(ca);
        if (!config.ca) goto invalid;
    }
    const char *bindings_path = getenv("OIDC_BINDINGS_FILE");
    if (config.managed && (!bindings_path || !*bindings_path)) config.bindings = json_object_new_object();
    else {
        char *bindings = read_file(bindings_path, RESPONSE_LIMIT, &length);
        if (!bindings) goto invalid;
        config.bindings = parse_json(bindings, length);
        free(bindings);
    }
    if (!config.bindings || json_object_object_length(config.bindings) > 1024) goto invalid;
    json_object_object_foreach(config.bindings, subject, email_value) {
        const char *email;
        char canonical[255];
        size_t subject_size = strlen(subject);
        if (!subject_size || subject_size > 255 ||
            !text_value(email_value, &email, 254) || !canonical_email(email, canonical)) goto invalid;
        for (size_t i = 0; i < subject_size; i++)
            if ((unsigned char)subject[i] < 32 || (unsigned char)subject[i] == 127) goto invalid;
    }
    unsigned char digest[32];
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned digest_size = 0;
    bool hashed = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(context, client, strlen(client)) == 1 &&
        EVP_DigestUpdate(context, "\0", 1) == 1 &&
        EVP_DigestUpdate(context, redirect, strlen(redirect)) == 1 &&
        EVP_DigestFinal_ex(context, digest, &digest_size) == 1 && digest_size == sizeof(digest);
    EVP_MD_CTX_free(context);
    if (!hashed) goto invalid;
    char suffix[17];
    for (size_t i = 0; i < 8; i++) (void)snprintf(suffix + 2 * i, 3, "%02x", digest[i]);
    erase(digest, sizeof(digest));
    (void)snprintf(config.cookie_name, sizeof(config.cookie_name), "punto_oidc_%s", suffix);
    (void)snprintf(config.clear_cookie, sizeof(config.clear_cookie),
        "Set-Cookie: %s=; Path=/auth/; HttpOnly; SameSite=Lax; Max-Age=0%s\r\n",
        config.cookie_name, !strncmp(config.redirect, "https://", 8) ? "; Secure" : "");
    if (RAND_bytes(config.cookie_key, sizeof(config.cookie_key)) != 1) goto invalid;
    config.initialized = true;
    return true;
invalid:
    oidc_close();
    return false;
}

bool oidc_enabled(void) { return config.initialized && config.enabled; }
bool oidc_local_enabled(void) { return config.initialized && config.local; }
const char *oidc_redirect_uri(void) { return oidc_enabled() ? config.redirect : ""; }
int oidc_users_managed(void) { return oidc_enabled() && config.managed; }
int oidc_local_account_allowed(const char *email) {
    if (!oidc_local_enabled()) return 0;
    if (!config.managed) return 1;
    char canonical[255];
    return canonical_email(email, canonical) && !strcmp(canonical, config.local_admin_email);
}
const char *oidc_users_admin_url(void) { return config.users_admin_url; }
const char *oidc_clear_cookie(void) { return config.clear_cookie; }

static char *make_form(CURL *curl, const char *const names[], const char *const values[], size_t count) {
    char *result = calloc(1, 1);
    size_t length = 0;
    if (!result) return NULL;
    for (size_t i = 0; i < count; i++) {
        char *encoded = curl_easy_escape(curl, values[i], 0);
        if (!encoded) { free(result); return NULL; }
        size_t addition = strlen(names[i]) + strlen(encoded) + 1 + (i ? 1 : 0);
        if (length + addition > TOKEN_LIMIT) { curl_free(encoded); free(result); return NULL; }
        char *grown = realloc(result, length + addition + 1);
        if (!grown) { curl_free(encoded); free(result); return NULL; }
        result = grown;
        int written = snprintf(result + length, addition + 1, "%s%s=%s", i ? "&" : "", names[i], encoded);
        curl_free(encoded);
        if (written < 0 || (size_t)written != addition) { free(result); return NULL; }
        length += addition;
    }
    return result;
}

int oidc_logout(char location[8192]) {
    if (!location) return 400;
    location[0] = '\0';
    if (!oidc_enabled()) return 404;
    /* validate_url requires this exact callback path. Derive the return URL
       from that configured origin, never a request parameter or Host header. */
    const size_t callback_length = sizeof("/auth/callback") - 1;
    size_t origin_length = strlen(config.redirect) - callback_length;
    char return_uri[URL_SIZE];
    int length = snprintf(return_uri, sizeof(return_uri), "%.*s/",
                          (int)origin_length, config.redirect);
    if (length < 0 || (size_t)length >= sizeof(return_uri)) return 503;
    CURL *curl = curl_easy_init();
    if (!curl) return 503;
    const char *names[] = {"client_id", "post_logout_redirect_uri"};
    const char *values[] = {config.client, return_uri};
    char *form = make_form(curl, names, values, sizeof(names) / sizeof(names[0]));
    curl_easy_cleanup(curl);
    if (!form) return 503;
    length = snprintf(location, 8192, "%s/protocol/openid-connect/logout?%s", config.issuer, form);
    free(form);
    if (length < 0 || length >= 8192) { location[0] = '\0'; return 503; }
    return 200;
}

int oidc_start(char location[8192], char cookie[512]) {
    if (!location || !cookie) return 400;
    location[0] = cookie[0] = '\0';
    if (!oidc_enabled()) return 404;
    time_t now = time(NULL);
    if (now < 0) return 503;
    prune_replays(now);
    Transaction transaction = {0};
    char sealed[TRANSACTION_COOKIE_SIZE] = {0};
    if (!random_text(transaction.state) || !random_text(transaction.nonce) ||
        !random_text(transaction.verifier)) goto failure;
    transaction.created = now;
    transaction.expires = now + TRANSACTION_SECONDS;
    if (!seal_transaction(&transaction, sealed)) goto failure;
    unsigned char digest[32];
    unsigned digest_size = 0;
    char challenge[45];
    if (EVP_Digest(transaction.verifier, RANDOM_TEXT, digest, &digest_size, EVP_sha256(), NULL) != 1 ||
        digest_size != sizeof(digest) || !base64url(digest, sizeof(digest), challenge, sizeof(challenge))) goto failure;
    erase(digest, sizeof(digest));
    CURL *curl = curl_easy_init();
    if (!curl) goto failure;
    const char *names[] = {"client_id", "redirect_uri", "response_type", "scope", "state", "nonce", "code_challenge", "code_challenge_method"};
    const char *values[] = {config.client, config.redirect, "code", config.managed ? "openid profile email" : "openid", transaction.state, transaction.nonce, challenge, "S256"};
    char *form = make_form(curl, names, values, sizeof(names) / sizeof(names[0]));
    curl_easy_cleanup(curl);
    if (!form) goto failure;
    int length = snprintf(location, 8192, "%s/protocol/openid-connect/auth?%s", config.issuer, form);
    free(form);
    if (length < 0 || length >= 8192) goto failure;
    length = snprintf(cookie, 512,
        "Set-Cookie: %s=%s; Path=/auth/; HttpOnly; SameSite=Lax; Max-Age=300%s\r\n",
        config.cookie_name, sealed, !strncmp(config.redirect, "https://", 8) ? "; Secure" : "");
    if (length < 0 || length >= 512) goto failure;
    erase(sealed, sizeof(sealed));
    erase(&transaction, sizeof(transaction));
    return 200;
failure:
    erase(sealed, sizeof(sealed));
    erase(&transaction, sizeof(transaction));
    location[0] = cookie[0] = '\0';
    return 503;
}

typedef struct { char *data; size_t size; } Received;

static size_t receive(void *data, size_t size, size_t count, void *opaque) {
    Received *received = opaque;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    if (length > RESPONSE_LIMIT || received->size > RESPONSE_LIMIT - length) return 0;
    char *grown = realloc(received->data, received->size + length + 1);
    if (!grown) return 0;
    received->data = grown;
    memcpy(grown + received->size, data, length);
    received->size += length;
    grown[received->size] = '\0';
    return length;
}

static struct json_object *http_json(const char *suffix, const char *form, long *status) {
    char url[URL_SIZE + 80];
    int length = snprintf(url, sizeof(url), "%s/protocol/openid-connect/%s", config.backchannel, suffix);
    if (length < 0 || (size_t)length >= sizeof(url)) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
    if (!headers) { curl_easy_cleanup(curl); return NULL; }
    if (form) {
        struct curl_slist *grown = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        if (!grown) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return NULL; }
        headers = grown;
    }
    Received received = {0};
    CURLcode result = CURLE_OK;
#define SET(option, value) do { if (result == CURLE_OK) result = curl_easy_setopt(curl, option, value); } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOPROXY, "*");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_MAXREDIRS, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (config.ca) SET(CURLOPT_CAINFO, config.ca);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https,http");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
    SET(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    SET(CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    SET(CURLOPT_TIMEOUT_MS, 3000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, &received);
    if (form) {
        SET(CURLOPT_POST, 1L);
        SET(CURLOPT_POSTFIELDS, form);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(form));
    }
    if (result == CURLE_OK) result = curl_easy_perform(curl);
    if (result == CURLE_OK) result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
#undef SET
    struct json_object *object = result == CURLE_OK && *status == 200 ? parse_json(received.data, received.size) : NULL;
    if (received.data) { erase(received.data, received.size); free(received.data); }
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return object;
}

static EVP_PKEY *jwks_key(struct json_object *jwks, const char *kid) {
    struct json_object *keys = NULL;
    if (!json_object_object_get_ex(jwks, "keys", &keys) || !json_object_is_type(keys, json_type_array) ||
        !json_object_array_length(keys) || json_object_array_length(keys) > 32) return NULL;
    struct json_object *found = NULL;
    for (size_t i = 0; i < json_object_array_length(keys); i++) {
        struct json_object *key = json_object_array_get_idx(keys, i);
        const char *candidate;
        if (!json_object_is_type(key, json_type_object) || !member_text(key, "kid", &candidate, 255)) continue;
        if (strcmp(candidate, kid)) continue;
        if (found) return NULL;
        found = key;
    }
    const char *kty, *n, *e, *optional;
    if (!found || !member_text(found, "kty", &kty, 16) || strcmp(kty, "RSA") ||
        !member_text(found, "n", &n, 2048) || !member_text(found, "e", &e, 16)) return NULL;
    struct json_object *field = NULL;
    if (json_object_object_get_ex(found, "alg", &field) &&
        (!text_value(field, &optional, 16) || strcmp(optional, "RS256"))) return NULL;
    if (json_object_object_get_ex(found, "use", &field) &&
        (!text_value(field, &optional, 16) || strcmp(optional, "sig"))) return NULL;
    if (json_object_object_get_ex(found, "key_ops", &field)) {
        if (!json_object_is_type(field, json_type_array) || json_object_array_length(field) != 1 ||
            !text_value(json_object_array_get_idx(field, 0), &optional, 16) || strcmp(optional, "verify")) return NULL;
    }
    size_t n_size = 0, e_size = 0;
    unsigned char *n_bytes = unbase64url(n, strlen(n), &n_size);
    unsigned char *e_bytes = unbase64url(e, strlen(e), &e_size);
    if (!n_bytes || !e_bytes || n_size < 256 || n_size > 1024 || !e_size || e_size > 4 ||
        n_bytes[0] == 0 || e_bytes[0] == 0) { free(n_bytes); free(e_bytes); return NULL; }
    BIGNUM *modulus = BN_bin2bn(n_bytes, (int)n_size, NULL);
    BIGNUM *exponent = BN_bin2bn(e_bytes, (int)e_size, NULL);
    free(n_bytes); free(e_bytes);
    EVP_PKEY *result = NULL;
    OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
    OSSL_PARAM *parameters = NULL;
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    bool okay = modulus && exponent && BN_num_bits(modulus) >= 2048 && BN_num_bits(modulus) <= 8192 &&
        BN_is_odd(exponent) && BN_get_word(exponent) >= 3 && builder && context &&
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, modulus) == 1 &&
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, exponent) == 1;
    if (okay) parameters = OSSL_PARAM_BLD_to_param(builder);
    if (!okay || !parameters || EVP_PKEY_fromdata_init(context) != 1 ||
        EVP_PKEY_fromdata(context, &result, EVP_PKEY_PUBLIC_KEY, parameters) != 1) {
        EVP_PKEY_free(result); result = NULL;
    }
    EVP_PKEY_CTX_free(context); OSSL_PARAM_free(parameters); OSSL_PARAM_BLD_free(builder);
    BN_free(modulus); BN_free(exponent);
    return result;
}

static int managed_claims(struct json_object *claims, OidcIdentity *identity) {
    const char *email, *name;
    struct json_object *field = NULL;
    if (!member_text(claims, "email", &email, sizeof(identity->email) - 1) ||
        !canonical_email(email, identity->email)) return 401;
    if (json_object_object_get_ex(claims, "name", &field)) {
        if (!text_value(field, &name, sizeof(identity->name) - 1)) return 401;
    } else if (!member_text(claims, "preferred_username", &name, sizeof(identity->name) - 1)) return 401;
    bool visible = false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p != ' ') visible = true;
    if (!visible) return 401;
    memcpy(identity->name, name, strlen(name) + 1);
    if (!json_object_object_get_ex(claims, config.role_claim, &field) ||
        !json_object_is_type(field, json_type_array)) return 403;
    size_t count = json_object_array_length(field);
    if (!count || count > 128) return 403;
    bool admin = false, user = false;
    for (size_t i = 0; i < count; i++) {
        const char *role;
        if (!text_value(json_object_array_get_idx(field, i), &role, 128)) return 403;
        if (!strcmp(role, config.admin_role)) admin = true;
        if (!strcmp(role, config.user_role)) user = true;
    }
    if (!admin && !user) return 403;
    const char *role = admin ? config.admin_role : config.user_role;
    memcpy(identity->role, role, strlen(role) + 1);
    return 200;
}

static int verify_token(const char *token, const Transaction *transaction,
                        int64_t expires_in, char email[255], OidcIdentity *identity, int *lifetime,
                        char verified_subject[256]) {
    if (!token || strlen(token) > TOKEN_LIMIT) return 401;
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || !second || strchr(second + 1, '.')) return 401;
    size_t header_size = 0, claims_size = 0, signature_size = 0;
    unsigned char *header_data = unbase64url(token, (size_t)(first - token), &header_size);
    unsigned char *claims_data = unbase64url(first + 1, (size_t)(second - first - 1), &claims_size);
    unsigned char *signature = unbase64url(second + 1, strlen(second + 1), &signature_size);
    struct json_object *header = header_data ? parse_json((const char *)header_data, header_size) : NULL;
    struct json_object *claims = claims_data ? parse_json((const char *)claims_data, claims_size) : NULL;
    free(header_data); free(claims_data);
    int status = 401;
    const char *algorithm, *kid, *issuer, *nonce, *subject;
    struct json_object *field = NULL;
    if (!header || !claims || !signature || signature_size < 256 || signature_size > 1024 ||
        !member_text(header, "alg", &algorithm, 16) || strcmp(algorithm, "RS256") ||
        !member_text(header, "kid", &kid, 255) || json_object_object_get_ex(header, "crit", &field) ||
        !member_text(claims, "iss", &issuer, URL_SIZE - 1) || strcmp(issuer, config.issuer) ||
        !member_text(claims, "nonce", &nonce, RANDOM_TEXT) || strlen(nonce) != RANDOM_TEXT ||
        CRYPTO_memcmp(nonce, transaction->nonce, RANDOM_TEXT) ||
        !member_text(claims, "sub", &subject, 255)) goto done;
    struct json_object *audience = NULL;
    if (!json_object_object_get_ex(claims, "aud", &audience)) goto done;
    bool audience_matches = false;
    size_t audiences = 1;
    const char *aud;
    if (json_object_is_type(audience, json_type_string))
        audience_matches = text_value(audience, &aud, 255) && !strcmp(aud, config.client);
    else if (json_object_is_type(audience, json_type_array)) {
        audiences = json_object_array_length(audience);
        if (!audiences || audiences > 16) goto done;
        for (size_t i = 0; i < audiences; i++) {
            if (!text_value(json_object_array_get_idx(audience, i), &aud, 255)) goto done;
            if (!strcmp(aud, config.client)) audience_matches = true;
        }
    } else goto done;
    if (!audience_matches) goto done;
    bool has_azp = json_object_object_get_ex(claims, "azp", &field);
    if ((audiences > 1 && !has_azp) || (has_azp &&
        (!text_value(field, &aud, 255) || strcmp(aud, config.client)))) goto done;
    int64_t expiry, issued, not_before;
    time_t now_value = time(NULL);
    if (now_value < 0) { status = 503; goto done; }
    int64_t now = (int64_t)now_value;
    if (!member_integer(claims, "exp", &expiry) || !member_integer(claims, "iat", &issued) ||
        expiry <= now || issued <= 0 || issued > now + CLOCK_SKEW || issued >= expiry ||
        issued < (int64_t)transaction->created - CLOCK_SKEW || expires_in <= 0) goto done;
    if (json_object_object_get_ex(claims, "nbf", &field) &&
        (!member_integer(claims, "nbf", &not_before) || not_before < 0 ||
         not_before > now + CLOCK_SKEW || not_before >= expiry)) goto done;
    /* Fetch only the configured issuer's fixed JWKS endpoint. JWT header URLs
       and discovery documents cannot choose a network destination. */
    long response_status = 0;
    struct json_object *jwks = http_json("certs", NULL, &response_status);
    if (!jwks) { status = 503; goto done; }
    EVP_PKEY *key = jwks_key(jwks, kid);
    json_object_put(jwks);
    if (!key) goto done;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    EVP_PKEY_CTX *verify_context = NULL;
    bool valid = context && EVP_DigestVerifyInit(context, &verify_context, EVP_sha256(), NULL, key) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(verify_context, RSA_PKCS1_PADDING) == 1 &&
        EVP_DigestVerify(context, signature, signature_size, (const unsigned char *)token,
                         (size_t)(second - token)) == 1;
    EVP_MD_CTX_free(context); EVP_PKEY_free(key);
    if (!valid) goto done;
    /* Network/JWKS verification may take seconds. Never grant a session using
       the earlier clock value after the signed token has already expired. */
    now_value = time(NULL);
    if (now_value < 0) { status = 503; goto done; }
    now = (int64_t)now_value;
    if (expiry <= now) goto done;
    memcpy(verified_subject, subject, strlen(subject) + 1);
    struct json_object *bound = NULL;
    const char *local_email;
    bool has_binding = json_object_object_get_ex(config.bindings, subject, &bound);
    if (identity) {
        status = managed_claims(claims, identity);
        if (status != 200) goto done;
        memcpy(identity->issuer, issuer, strlen(issuer) + 1);
        memcpy(identity->subject, subject, strlen(subject) + 1);
        if (has_binding && (!text_value(bound, &local_email, 254) ||
            !canonical_email(local_email, identity->bound_email))) { status = 403; goto done; }
    } else {
        if (!has_binding || !text_value(bound, &local_email, 254) ||
            !canonical_email(local_email, email)) { status = 403; goto done; }
    }
    int64_t duration = expiry - now;
    if (expires_in < duration) duration = expires_in;
    if (duration > SESSION_SECONDS) duration = SESSION_SECONDS;
    *lifetime = (int)duration;
    status = 200;
done:
    free(signature); json_object_put(header); json_object_put(claims);
    return status;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode(const char *text, size_t size) {
    char *output = malloc(size + 1);
    if (!output) return NULL;
    size_t length = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '%') {
            if (size - i < 3 || hex_digit((unsigned char)text[i + 1]) < 0 ||
                hex_digit((unsigned char)text[i + 2]) < 0) { free(output); return NULL; }
            c = (unsigned char)(16 * hex_digit((unsigned char)text[i + 1]) + hex_digit((unsigned char)text[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) { free(output); return NULL; }
        output[length++] = (char)c;
    }
    output[length] = '\0';
    return output;
}

typedef struct { char *state, *code, *error, *issuer; } Callback;

static void free_callback(Callback *callback) {
    free(callback->state);
    if (callback->code) { erase(callback->code, strlen(callback->code)); free(callback->code); }
    free(callback->error); free(callback->issuer);
    memset(callback, 0, sizeof(*callback));
}

static bool parse_callback(const char *query, Callback *callback) {
    if (!query || !*query || strlen(query) > 8192) return false;
    unsigned seen = 0;
    const char *p = query;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (!equal || equal == p) return false;
        char *name = decode(p, (size_t)(equal - p));
        char *value = decode(equal + 1, (size_t)(end - equal - 1));
        if (!name || !value) { free(name); free(value); return false; }
        const char *names[] = {"state", "code", "error", "iss", "session_state", "error_description", "error_uri"};
        size_t index = 0;
        while (index < sizeof(names) / sizeof(names[0]) && strcmp(name, names[index])) index++;
        free(name);
        if (index == sizeof(names) / sizeof(names[0]) || (seen & (1U << index))) { free(value); return false; }
        seen |= 1U << index;
        size_t length = strlen(value);
        if (!length || (index == 0 && length != RANDOM_TEXT) ||
            (index == 1 && length > 4096) || (index > 1 && length > URL_SIZE - 1)) {
            free(value); return false;
        }
        if (index == 0) callback->state = value;
        else if (index == 1) callback->code = value;
        else if (index == 2) callback->error = value;
        else if (index == 3) callback->issuer = value;
        else free(value);
        if (!*end) break;
        p = end + 1;
        if (!*p) return false;
    }
    return callback->state && ((callback->code != NULL) != (callback->error != NULL));
}

static bool browser_cookie(const char *cookies, char sealed[TRANSACTION_COOKIE_SIZE]) {
    if (!cookies || strlen(cookies) > 4096) return false;
    bool found = false;
    size_t name_size = strlen(config.cookie_name);
    const char *p = cookies;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > name_size && !memcmp(p, config.cookie_name, name_size) && p[name_size] == '=') {
            size_t length = (size_t)(end - p) - name_size - 1;
            if (found || !length || length >= TRANSACTION_COOKIE_SIZE) return false;
            memcpy(sealed, p + name_size + 1, length);
            sealed[length] = '\0';
            found = true;
        }
        p = end;
    }
    return found;
}

static int callback_result(const char *query, const char *cookie, char email[255],
                           OidcIdentity *identity, int *lifetime) {
    if (!oidc_enabled()) return 404;
    Callback callback = {0};
    int status = 400;
    Transaction transaction = {0};
    char sealed[TRANSACTION_COOKIE_SIZE] = {0}, verified_subject[256] = {0};
    if (!parse_callback(query, &callback)) goto done;
    if (!browser_cookie(cookie, sealed)) { status = 401; goto done; }
    time_t now = time(NULL);
    if (now < 0) { status = 503; goto done; }
    prune_replays(now);
    if (!open_transaction(sealed, now, &transaction) ||
        CRYPTO_memcmp(callback.state, transaction.state, RANDOM_TEXT)) { status = 401; goto done; }
    for (Replay *entry = config.replays; entry; entry = entry->next)
        if (!CRYPTO_memcmp(entry->state, transaction.state, RANDOM_TEXT)) { status = 401; goto done; }
    if (callback.issuer && strcmp(callback.issuer, config.issuer)) { status = 401; goto done; }
    if (callback.error) { status = 401; goto done; }
    CURL *curl = curl_easy_init();
    if (!curl) { status = 503; goto done; }
    const char *names[] = {"grant_type", "client_id", "client_secret", "code", "redirect_uri", "code_verifier"};
    const char *values[] = {"authorization_code", config.client, config.secret, callback.code, config.redirect, transaction.verifier};
    char *form = make_form(curl, names, values, sizeof(names) / sizeof(names[0]));
    curl_easy_cleanup(curl);
    if (!form) { status = 503; goto done; }
    long response_status = 0;
    struct json_object *response = http_json("token", form, &response_status);
    erase(form, strlen(form)); free(form);
    if (!response) { status = response_status >= 400 && response_status < 500 ? 401 : 503; goto done; }
    const char *id_token, *token_type;
    int64_t expires_in;
    if (!member_text(response, "id_token", &id_token, TOKEN_LIMIT) ||
        !member_text(response, "token_type", &token_type, 16) ||
        strcmp(token_type, "Bearer") || !member_integer(response, "expires_in", &expires_in) || expires_in <= 0) status = 503;
    else status = verify_token(id_token, &transaction, expires_in, email, identity, lifetime, verified_subject);
    if (verified_subject[0]) {
        int replay_status = remember_transaction(&transaction, verified_subject);
        if (replay_status != 200) status = replay_status;
    }
    json_object_put(response);
done:
    if (status != 200) {
        if (email) email[0] = '\0';
        if (identity) memset(identity, 0, sizeof(*identity));
        *lifetime = 0;
    }
    erase(sealed, sizeof(sealed)); erase(verified_subject, sizeof(verified_subject));
    erase(&transaction, sizeof(transaction));
    free_callback(&callback);
    return status;
}

int oidc_callback(const char *query, const char *cookie, char email[255], int *lifetime) {
    if (!email || !lifetime) return 400;
    email[0] = '\0'; *lifetime = 0;
    return callback_result(query, cookie, email, NULL, lifetime);
}

int oidc_callback_identity(const char *query, const char *cookie, OidcIdentity *out, int *lifetime) {
    if (!out || !lifetime) return 400;
    memset(out, 0, sizeof(*out)); *lifetime = 0;
    if (!oidc_users_managed()) return 404;
    return callback_result(query, cookie, NULL, out, lifetime);
}
