#include "accounts.h"
#include "storage.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <Security/SecRandom.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define FORM_LIMIT 32768U
#define USER_LIMIT 200
#define HASH_SIZE 32U
#define SALT_SIZE 16U
#define HASH_ROUNDS 210000
#define PASSWORD_LIMIT 1024U
#define JSON_LIMIT (1024U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} Json;

typedef struct {
    const char *name;
    char *value;
} Field;

static Db *database;
static unsigned char dummy_salt[SALT_SIZE];

static void erase(void *pointer, size_t size) {
    volatile unsigned char *p = pointer;
    while (size--) *p++ = 0;
}

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static bool random_bytes(unsigned char *buffer, size_t size) {
#ifdef __APPLE__
    return SecRandomCopyBytes(kSecRandomDefault, size, buffer) == errSecSuccess;
#else
    return size <= INT_MAX && RAND_bytes(buffer, (int)size) == 1;
#endif
}

static bool hash_password(const char *password, const unsigned char *salt,
                          unsigned char *output) {
    size_t length = strlen(password);
    if (length > PASSWORD_LIMIT) return false;
#ifdef __APPLE__
    return CCKeyDerivationPBKDF(kCCPBKDF2, password, length, salt, SALT_SIZE,
        kCCPRFHmacAlgSHA256, HASH_ROUNDS, output, HASH_SIZE) == kCCSuccess;
#else
    return PKCS5_PBKDF2_HMAC(password, (int)length, salt, SALT_SIZE,
        HASH_ROUNDS, EVP_sha256(), HASH_SIZE, output) == 1;
#endif
}

static bool equal_hash(const unsigned char *a, const unsigned char *b) {
    unsigned char difference = 0;
    for (size_t i = 0; i < HASH_SIZE; i++) difference |= a[i] ^ b[i];
    return difference == 0;
}

static bool append_bytes(Json *json, const char *text, size_t length) {
    if (json->failed) return false;
    if (length > JSON_LIMIT || json->length > JSON_LIMIT - length) {
        json->failed = true;
        return false;
    }
    size_t needed = json->length + length + 1;
    if (needed > json->capacity) {
        size_t capacity = json->capacity ? json->capacity : 256;
        while (capacity < needed) capacity *= 2;
        char *data = realloc(json->data, capacity);
        if (!data) { json->failed = true; return false; }
        json->data = data;
        json->capacity = capacity;
    }
    memcpy(json->data + json->length, text, length);
    json->length += length;
    json->data[json->length] = '\0';
    return true;
}

static void append(Json *json, const char *text) {
    (void)append_bytes(json, text, strlen(text));
}

static void append_integer(Json *json, DbInt value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length < 0 || (size_t)length >= sizeof(text)) json->failed = true;
    else (void)append_bytes(json, text, (size_t)length);
}

static void append_string(Json *json, const char *text) {
    append(json, "\"");
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p; p++) {
        switch (*p) {
            case '"': append(json, "\\\""); break;
            case '\\': append(json, "\\\\"); break;
            case '\b': append(json, "\\b"); break;
            case '\f': append(json, "\\f"); break;
            case '\n': append(json, "\\n"); break;
            case '\r': append(json, "\\r"); break;
            case '\t': append(json, "\\t"); break;
            case '<': append(json, "\\u003c"); break;
            case '>': append(json, "\\u003e"); break;
            case '&': append(json, "\\u0026"); break;
            default:
                if (*p < 32) {
                    char escaped[7];
                    (void)snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    append(json, escaped);
                } else if (*p == 0xe2 && p[1] == 0x80 &&
                           (p[2] == 0xa8 || p[2] == 0xa9)) {
                    append(json, p[2] == 0xa8 ? "\\u2028" : "\\u2029");
                    p += 2;
                } else (void)append_bytes(json, (const char *)p, 1);
                break;
        }
    }
    append(json, "\"");
}

static void error(WorkspaceResponse *response, int status, const char *message) {
    Json json = {0};
    append(&json, "{\"ok\":false,\"error\":");
    append_string(&json, message);
    append(&json, "}");
    response->status = json.failed ? 500 : status;
    if (json.failed) {
        free(json.data);
        response->body = duplicate("{\"ok\":false,\"error\":\"Errore del server. Riprova.\"}");
    } else response->body = json.data;
}

static void server_error(WorkspaceResponse *response) {
    error(response, 500, "Errore del server. Riprova.");
}

static void finish_json(WorkspaceResponse *response, int status, Json *json) {
    if (json->failed) { free(json->data); server_error(response); }
    else { response->status = status; response->body = json->data; }
}

static bool prepare(DbStmt **statement, const char *sql) {
    return db_prepare_v2(database, sql, -1, statement, NULL) == DB_OK;
}

static bool bind_text(DbStmt *statement, int index, const char *text) {
    return db_bind_text(statement, index, text, -1, DB_TRANSIENT) == DB_OK;
}

static const char *column_text(DbStmt *statement, int column) {
    const unsigned char *text = db_column_text(statement, column);
    return text ? (const char *)text : "";
}

static void rollback(void) {
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
}

static bool begin_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == DB_OK)
        return true;
    server_error(response);
    return false;
}

static bool commit_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
    rollback();
    server_error(response);
    return false;
}

/* Validate canonical UTF-8, counting codepoints and rejecting control values. */
static bool utf8_length(const char *text, size_t *length) {
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (*p) {
        uint32_t codepoint;
        size_t continuation;
        unsigned char first = *p++;
        if (first < 0x80) { codepoint = first; continuation = 0; }
        else if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1fU; continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0fU; continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07U; continuation = 3;
        } else return false;
        for (size_t i = 0; i < continuation; i++) {
            if ((*p & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (*p++ & 0x3fU);
        }
        if ((continuation == 1 && codepoint < 0x80) ||
            (continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint < 32 && codepoint != '\r' && codepoint != '\n' && codepoint != '\t') ||
            (codepoint >= 0x7f && codepoint <= 0x9f)) return false;
        count++;
    }
    *length = count;
    return true;
}

static void trim(char *text) {
    if (!text) return;
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    size_t length = strlen(start);
    while (length && (start[length - 1] == ' ' || start[length - 1] == '\t' ||
                     start[length - 1] == '\r' || start[length - 1] == '\n')) length--;
    memmove(text, start, length);
    text[length] = '\0';
}

static bool valid_name(const char *text) {
    size_t characters;
    return text && utf8_length(text, &characters) && characters > 0 &&
        characters <= 120 && !strpbrk(text, "\r\n\t");
}

static bool valid_role(const char *role) {
    return role && (strcmp(role, "manager") == 0 || strcmp(role, "worker") == 0);
}

/* Email identities use ASCII and are stored in lowercase for existing work logs. */
static bool canonical_email(const char *input, char output[255]) {
    if (!input) return false;
    size_t length = strlen(input);
    if (!length || length > 254) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        output[i] = (char)c;
    }
    output[length] = '\0';
    trim(output);
    char *at = strchr(output, '@');
    if (!at || at == output || !at[1] || strchr(at + 1, '@')) return false;
    size_t local_length = (size_t)(at - output);
    if (local_length > 64 || output[0] == '.' || at[-1] == '.') return false;
    for (const char *p = output; p < at; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              strchr(".!#$%&'*+/=?^_`{|}~-", c))) return false;
        if (c == '.' && p[1] == '.') return false;
    }
    const char *label = at + 1;
    for (const char *p = label; ; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || !c) {
            size_t label_length = (size_t)(p - label);
            if (!label_length || label_length > 63 || *label == '-' || p[-1] == '-')
                return false;
            if (!c) break;
            label = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool valid_password(const char *password) {
    if (!password) return false;
    size_t length = strlen(password), characters;
    return length >= 8 && length <= PASSWORD_LIMIT &&
        utf8_length(password, &characters) && !strpbrk(password, "\r\n\t");
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode(const char *input, size_t length, bool *out_of_memory) {
    char *output = malloc(length + 1);
    if (!output) { *out_of_memory = true; return NULL; }
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (length - i < 3) goto invalid;
            int high = hex_digit((unsigned char)input[i + 1]);
            int low = hex_digit((unsigned char)input[i + 2]);
            if (high < 0 || low < 0) goto invalid;
            c = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (c == '+') c = ' ';
        if (!c) goto invalid;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    size_t characters;
    if (!utf8_length(output, &characters)) goto invalid;
    return output;
invalid:
    erase(output, used);
    free(output);
    return NULL;
}

static void free_fields(Field *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (fields[i].value) erase(fields[i].value, strlen(fields[i].value));
        free(fields[i].value);
        fields[i].value = NULL;
    }
}

static bool parse_fields(const char *input, Field *fields, size_t count,
                         WorkspaceResponse *response) {
    bool out_of_memory = false;
    if (!input || strlen(input) > FORM_LIMIT) goto invalid;
    const char *p = input;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (!equal || equal == p) goto invalid;
        char *name = decode(p, (size_t)(equal - p), &out_of_memory);
        if (!name) goto invalid;
        size_t field = 0;
        while (field < count && strcmp(name, fields[field].name) != 0) field++;
        free(name);
        if (field == count || fields[field].value) goto invalid;
        fields[field].value = decode(equal + 1, (size_t)(end - equal - 1), &out_of_memory);
        if (!fields[field].value) goto invalid;
        if (!*end) break;
        p = end + 1;
        if (!*p) goto invalid;
    }
    for (size_t i = 0; i < count; i++) if (!fields[i].value) goto invalid;
    return true;
invalid:
    free_fields(fields, count);
    if (out_of_memory) server_error(response);
    else error(response, 400, "Dati non validi: controlla i campi inviati.");
    return false;
}

static bool parse_id(const char *text, DbInt *value) {
    if (!text || !*text) return false;
    DbInt result = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        int digit = *p - '0';
        if (result > (INT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    if (result <= 0) return false;
    *value = result;
    return true;
}

static bool copy_account(DbStmt *statement, Account *out) {
    const char *email = column_text(statement, 1);
    const char *name = column_text(statement, 2);
    const char *role = column_text(statement, 3);
    char canonical[255];
    if (!out || db_column_int64(statement, 0) <= 0 ||
        strlen(email) >= sizeof(out->email) || strlen(name) >= sizeof(out->name) ||
        strlen(role) >= sizeof(out->role) || !canonical_email(email, canonical) ||
        strcmp(email, canonical) != 0 || !valid_name(name) || !valid_role(role)) return false;
    memset(out, 0, sizeof(*out));
    out->id = db_column_int64(statement, 0);
    memcpy(out->email, email, strlen(email) + 1);
    memcpy(out->name, name, strlen(name) + 1);
    memcpy(out->role, role, strlen(role) + 1);
    return true;
}

bool accounts_open(const char *db_path, const char *bootstrap_email,
                   const char *bootstrap_password) {
    accounts_close();
    if (!db_path || !*db_path ||
        db_open_v2(db_path, &database,
            DB_OPEN_READWRITE | DB_OPEN_CREATE | DB_OPEN_FULLMUTEX,
            NULL) != DB_OK) {
        accounts_close();
        return false;
    }
    DbStmt *statement = NULL;
    unsigned char salt[SALT_SIZE] = {0}, hash[HASH_SIZE] = {0};
    bool success = false;
    if (db_busy_timeout(database, 5000) != DB_OK ||
        !random_bytes(dummy_salt, sizeof(dummy_salt)) ||
        db_exec(database,
#ifdef PUNTO_MYSQL
            "START TRANSACTION"
#else
            "PRAGMA foreign_keys = ON;"
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS users ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "email TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "name TEXT NOT NULL,"
            "role TEXT NOT NULL CHECK(role IN ('manager','worker')),"
            "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),"
            "password_hash BLOB NOT NULL CHECK(length(password_hash)=32),"
            "salt BLOB NOT NULL CHECK(length(salt)=16)"
            ");"
            "CREATE TABLE IF NOT EXISTS oidc_identities ("
            "issuer TEXT NOT NULL COLLATE BINARY,"
            "subject TEXT NOT NULL COLLATE BINARY,"
            "user_id INTEGER NOT NULL UNIQUE REFERENCES users(id),"
            "PRIMARY KEY(issuer,subject)"
            ");"
#endif
            , NULL, NULL, NULL) != DB_OK ||
        !prepare(&statement, "SELECT COUNT(*) FROM users") ||
        db_step(statement) != DB_ROW) goto done;
    DbInt count = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (!count) {
        char email[255];
        if (!canonical_email(bootstrap_email, email) ||
            !valid_password(bootstrap_password) || !random_bytes(salt, sizeof(salt)) ||
            !hash_password(bootstrap_password, salt, hash) ||
            !prepare(&statement,
                "INSERT INTO users(email,name,role,active,password_hash,salt) "
                "VALUES(?,'Gestore','manager',1,?,?)") ||
            !bind_text(statement, 1, email) ||
            db_bind_blob(statement, 2, hash, sizeof(hash), DB_TRANSIENT) != DB_OK ||
            db_bind_blob(statement, 3, salt, sizeof(salt), DB_TRANSIENT) != DB_OK ||
            db_step(statement) != DB_DONE) goto done;
        db_finalize(statement); statement = NULL;
    }
    success = db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK;
done:
    db_finalize(statement);
    erase(salt, sizeof(salt)); erase(hash, sizeof(hash));
    if (!success) { rollback(); accounts_close(); }
    return success;
}

void accounts_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
    erase(dummy_salt, sizeof(dummy_salt));
}

int accounts_authenticate(const char *email, const char *password, Account *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!database || !out || !password) return -1;
    if (strlen(password) > PASSWORD_LIMIT) return 0;
    char canonical[255];
    bool email_valid = canonical_email(email, canonical);
    if (email_valid && !oidc_local_account_allowed(canonical)) return 0;
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT id,email,name,role,active,password_hash,salt FROM users WHERE email=?") ||
        !bind_text(statement, 1, email_valid ? canonical : "")) {
        db_finalize(statement);
        return -1;
    }
    int result = db_step(statement);
    if (result != DB_ROW && result != DB_DONE) {
        db_finalize(statement);
        return -1;
    }
    unsigned char salt[SALT_SIZE], stored[HASH_SIZE] = {0}, candidate[HASH_SIZE] = {0};
    memcpy(salt, dummy_salt, sizeof(salt));
    bool found = result == DB_ROW;
    bool active = found && db_column_int(statement, 4) == 1;
    Account account = {0};
    bool row_valid = false;
    if (found) {
        const void *hash_value = db_column_blob(statement, 5);
        const void *salt_value = db_column_blob(statement, 6);
        row_valid = hash_value && salt_value &&
            db_column_bytes(statement, 5) == (int)sizeof(stored) &&
            db_column_bytes(statement, 6) == (int)sizeof(salt) &&
            copy_account(statement, &account);
        if (row_valid) {
            memcpy(stored, hash_value, sizeof(stored));
            memcpy(salt, salt_value, sizeof(salt));
        }
    }
    db_finalize(statement);
    bool derived = hash_password(password, salt, candidate);
    bool valid = derived && equal_hash(candidate, stored) && found && row_valid && active;
    erase(salt, sizeof(salt)); erase(stored, sizeof(stored));
    erase(candidate, sizeof(candidate));
    if (valid) *out = account;
    erase(&account, sizeof(account));
    if (!derived || (found && !row_valid)) return -1;
    return valid ? 1 : 0;
}

int accounts_lookup(DbInt id, Account *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!database || id <= 0 || !out) return -1;
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT id,email,name,role FROM users WHERE id=? AND active=1") ||
        db_bind_int64(statement, 1, id) != DB_OK) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement);
    int found = result == DB_DONE ? 0 :
        (result == DB_ROW && copy_account(statement, out) ? 1 : -1);
    db_finalize(statement);
    return found;
}

bool accounts_find(DbInt id, Account *out) {
    return accounts_lookup(id, out) == 1;
}

int accounts_lookup_email(const char *email, Account *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!database || !email || !*email || !out) return -1;
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT id,email,name,role FROM users WHERE email=? AND active=1") ||
        db_bind_text(statement, 1, email, -1, DB_TRANSIENT) != DB_OK) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement);
    int found = result == DB_DONE ? 0 :
        (result == DB_ROW && copy_account(statement, out) ? 1 : -1);
    db_finalize(statement);
    return found;
}

static void list_users(WorkspaceResponse *response) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,email,name,role,active FROM users ORDER BY id LIMIT 201")) {
        server_error(response); return;
    }
    Json json = {0};
    append(&json, "{\"users\":[");
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > USER_LIMIT) break;
        if (count > 1) append(&json, ",");
        append(&json, "{\"id\":"); append_integer(&json, db_column_int64(statement, 0));
        append(&json, ",\"email\":"); append_string(&json, column_text(statement, 1));
        append(&json, ",\"name\":"); append_string(&json, column_text(statement, 2));
        append(&json, ",\"role\":"); append_string(&json, column_text(statement, 3));
        append(&json, ",\"active\":");
        append(&json, db_column_int(statement, 4) == 1 ? "true" : "false");
        append(&json, "}");
    }
    db_finalize(statement);
    if (count > USER_LIMIT || result != DB_DONE) {
        free(json.data);
        if (count > USER_LIMIT) error(response, 409, "Il limite è di 200 utenti.");
        else server_error(response);
        return;
    }
    append(&json, "]}");
    finish_json(response, 200, &json);
}

/* Explicit allowlist: credentials never enter an audit snapshot. */
static char *user_snapshot(DbInt id) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,email,name,role,active FROM users WHERE id=?") ||
        db_bind_int64(statement, 1, id) != DB_OK ||
        db_step(statement) != DB_ROW) {
        db_finalize(statement); return NULL;
    }
    Json json = {0};
    append(&json, "{\"id\":"); append_integer(&json, db_column_int64(statement, 0));
    append(&json, ",\"email\":"); append_string(&json, column_text(statement, 1));
    append(&json, ",\"name\":"); append_string(&json, column_text(statement, 2));
    append(&json, ",\"role\":"); append_string(&json, column_text(statement, 3));
    append(&json, ",\"active\":"); append(&json, db_column_int(statement, 4) ? "true" : "false");
    append(&json, "}");
    db_finalize(statement);
    if (json.failed) { free(json.data); return NULL; }
    return json.data;
}

static bool valid_identity_key(const char *text, size_t capacity) {
    size_t length = strnlen(text, capacity);
    if (!length || length == capacity) return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127) return false;
    return true;
}

int accounts_resolve_oidc(const OidcIdentity *identity, Account *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!database || !identity || !out) return -1;
    char email[255], bound_email[255];
    if (!oidc_users_managed() ||
        !valid_identity_key(identity->issuer, sizeof(identity->issuer)) ||
        !valid_identity_key(identity->subject, sizeof(identity->subject)) ||
        strnlen(identity->email, sizeof(identity->email)) == sizeof(identity->email) ||
        strnlen(identity->name, sizeof(identity->name)) == sizeof(identity->name) ||
        strnlen(identity->role, sizeof(identity->role)) == sizeof(identity->role) ||
        strnlen(identity->bound_email, sizeof(identity->bound_email)) == sizeof(identity->bound_email) ||
        !canonical_email(identity->email, email) || !valid_name(identity->name) ||
        !valid_role(identity->role) ||
        (*identity->bound_email && !canonical_email(identity->bound_email, bound_email))) return 0;

    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != DB_OK) return -1;
    DbStmt *statement = NULL;
    DbInt user_id = 0;
    int answer = -1, result;
    bool linked = false, changed = true;
    char *before = NULL, *after = NULL;
    unsigned char salt[SALT_SIZE] = {0}, hash[HASH_SIZE] = {0};
    Account previous = {0};

    if (!prepare(&statement,
        "SELECT user_id FROM oidc_identities WHERE issuer=? AND subject=?") ||
        !bind_text(statement, 1, identity->issuer) ||
        !bind_text(statement, 2, identity->subject)) goto done;
    result = db_step(statement);
    if (result != DB_ROW && result != DB_DONE) goto done;
    linked = result == DB_ROW;
    if (linked) user_id = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;

    /* A persistent provider subject is authoritative. Bindings are migration
       instructions used once; an email claim never claims an existing row. */
    if (!linked && *identity->bound_email) {
        if (!prepare(&statement, "SELECT id FROM users WHERE email=?") ||
            !bind_text(statement, 1, bound_email)) goto done;
        result = db_step(statement);
        if (result != DB_ROW) { if (result == DB_DONE) answer = 0; goto done; }
        user_id = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
        if (!prepare(&statement, "SELECT user_id FROM oidc_identities WHERE user_id=?") ||
            db_bind_int64(statement, 1, user_id) != DB_OK) goto done;
        result = db_step(statement);
        if (result != DB_DONE) { if (result == DB_ROW) answer = 0; goto done; }
        db_finalize(statement); statement = NULL;
    }

    if (!prepare(&statement, "SELECT id FROM users WHERE email=?") ||
        !bind_text(statement, 1, email)) goto done;
    result = db_step(statement);
    if (result != DB_ROW && result != DB_DONE) goto done;
    if (result == DB_ROW && db_column_int64(statement, 0) != user_id) {
        answer = 0; goto done;
    }
    db_finalize(statement); statement = NULL;

    if (user_id) {
        if (!prepare(&statement, "SELECT id,email,name,role,active FROM users WHERE id=?") ||
            db_bind_int64(statement, 1, user_id) != DB_OK ||
            db_step(statement) != DB_ROW || !copy_account(statement, &previous)) goto done;
        changed = strcmp(previous.email, email) != 0 ||
            strcmp(previous.name, identity->name) != 0 ||
            strcmp(previous.role, identity->role) != 0 || db_column_int(statement, 4) != 1;
        db_finalize(statement); statement = NULL;
        before = user_snapshot(user_id);
        if (!before) goto done;
        if (changed) {
            if (!prepare(&statement, "UPDATE users SET email=?,name=?,role=?,active=1 WHERE id=?") ||
                !bind_text(statement, 1, email) || !bind_text(statement, 2, identity->name) ||
                !bind_text(statement, 3, identity->role) ||
                db_bind_int64(statement, 4, user_id) != DB_OK) goto done;
            result = db_step(statement);
            if (result != DB_DONE) {
                if (db_extended_errcode(database) == DB_CONSTRAINT_UNIQUE) answer = 0;
                goto done;
            }
            db_finalize(statement); statement = NULL;
        }
    } else {
        if (!prepare(&statement, "SELECT COUNT(*) FROM users") || db_step(statement) != DB_ROW)
            goto done;
        if (db_column_int64(statement, 0) >= USER_LIMIT) { answer = 0; goto done; }
        db_finalize(statement); statement = NULL;
        /* Random hash material has no known password preimage. Provider-created
           accounts cannot use the local emergency login. */
        if (!random_bytes(salt, sizeof(salt)) || !random_bytes(hash, sizeof(hash)) ||
            !prepare(&statement,
                "INSERT INTO users(email,name,role,active,password_hash,salt) VALUES(?,?,?,1,?,?)") ||
            !bind_text(statement, 1, email) || !bind_text(statement, 2, identity->name) ||
            !bind_text(statement, 3, identity->role) ||
            db_bind_blob(statement, 4, hash, sizeof(hash), DB_TRANSIENT) != DB_OK ||
            db_bind_blob(statement, 5, salt, sizeof(salt), DB_TRANSIENT) != DB_OK) goto done;
        result = db_step(statement);
        if (result != DB_DONE) {
            if (db_extended_errcode(database) == DB_CONSTRAINT_UNIQUE) answer = 0;
            goto done;
        }
        user_id = db_last_insert_rowid(database);
        db_finalize(statement); statement = NULL;
    }

    if (!linked) {
        if (!prepare(&statement, "INSERT INTO oidc_identities(issuer,subject,user_id) VALUES(?,?,?)") ||
            !bind_text(statement, 1, identity->issuer) || !bind_text(statement, 2, identity->subject) ||
            db_bind_int64(statement, 3, user_id) != DB_OK) goto done;
        result = db_step(statement);
        if (result != DB_DONE) {
            if (db_extended_errcode(database) == DB_CONSTRAINT_UNIQUE) answer = 0;
            goto done;
        }
        db_finalize(statement); statement = NULL;
    }
    if (changed || !linked) {
        after = user_snapshot(user_id);
        if (!after || !storage_audit(database, 0, linked ? "users.oidc_sync" : "users.oidc_link",
                                    "user", user_id, before, after)) goto done;
    }
    if (accounts_lookup(user_id, out) != 1) goto done;
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) != DB_OK) goto done;
    answer = 1;
done:
    db_finalize(statement);
    if (answer != 1) { rollback(); memset(out, 0, sizeof(*out)); }
    free(before); free(after);
    erase(&previous, sizeof(previous));
    erase(salt, sizeof(salt)); erase(hash, sizeof(hash));
    return answer;
}

static void create_user(const char *body, DbInt actor_id, WorkspaceResponse *response) {
    Field fields[] = {{"name", NULL}, {"email", NULL}, {"password", NULL}, {"role", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    trim(fields[0].value); trim(fields[1].value);
    char email[255];
    if (!valid_name(fields[0].value) || !canonical_email(fields[1].value, email) ||
        !valid_password(fields[2].value) || !valid_role(fields[3].value)) {
        error(response, 400, "Inserisci nome (max 120 caratteri), email valida, password da 8 a 1024 byte e un ruolo valido.");
        free_fields(fields, count); return;
    }
    unsigned char salt[SALT_SIZE] = {0}, hash[HASH_SIZE] = {0};
    if (!random_bytes(salt, sizeof(salt)) || !hash_password(fields[2].value, salt, hash)) {
        erase(salt, sizeof(salt)); erase(hash, sizeof(hash));
        free_fields(fields, count); server_error(response); return;
    }
    if (!begin_transaction(response)) {
        erase(salt, sizeof(salt)); erase(hash, sizeof(hash));
        free_fields(fields, count); return;
    }
    DbStmt *statement = NULL;
    char *snapshot = NULL;
    if (!prepare(&statement, "SELECT COUNT(*) FROM users") ||
        db_step(statement) != DB_ROW) goto database_failure;
    DbInt existing = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (existing >= USER_LIMIT) {
        rollback();
        error(response, 409, "Hai raggiunto il limite di 200 utenti.");
        goto done;
    }
    if (!prepare(&statement,
        "INSERT INTO users(email,name,role,active,password_hash,salt) VALUES(?,?,?,1,?,?)") ||
        !bind_text(statement, 1, email) || !bind_text(statement, 2, fields[0].value) ||
        !bind_text(statement, 3, fields[3].value) ||
        db_bind_blob(statement, 4, hash, sizeof(hash), DB_TRANSIENT) != DB_OK ||
        db_bind_blob(statement, 5, salt, sizeof(salt), DB_TRANSIENT) != DB_OK)
        goto database_failure;
    int result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) {
        int extended = db_extended_errcode(database);
        rollback();
        if (extended == DB_CONSTRAINT_UNIQUE)
            error(response, 409, "Questa email è già associata a un utente.");
        else server_error(response);
        goto done;
    }
    DbInt id = db_last_insert_rowid(database);
    /* Attach preserved, previously unmatched legacy history to this identity. */
    if (!prepare(&statement,
        "UPDATE time_entries SET user_id=? WHERE user_id IS NULL AND owner=? COLLATE NOCASE") ||
        db_bind_int64(statement, 1, id) != DB_OK || !bind_text(statement, 2, email) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    snapshot = user_snapshot(id);
    if (!snapshot || !storage_audit(database, actor_id, "users.create", "user", id, NULL, snapshot))
        goto database_failure;
    if (commit_transaction(response)) {
        Json json = {0};
        append(&json, "{\"ok\":true,\"id\":"); append_integer(&json, id); append(&json, "}");
        finish_json(response, 201, &json);
    }
    goto done;
database_failure:
    db_finalize(statement); statement = NULL;
    rollback(); server_error(response);
done:
    free(snapshot);
    erase(salt, sizeof(salt)); erase(hash, sizeof(hash));
    free_fields(fields, count);
}

static void update_user(const char *body, DbInt id, DbInt actor_id,
                         WorkspaceResponse *response) {
    Field fields[] = {{"name", NULL}, {"role", NULL}, {"active", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    trim(fields[0].value);
    if (!valid_name(fields[0].value) || !valid_role(fields[1].value) ||
        (strcmp(fields[2].value, "1") != 0 && strcmp(fields[2].value, "0") != 0)) {
        error(response, 400, "Inserisci un nome valido, un ruolo e lo stato dell'utente.");
        free_fields(fields, count); return;
    }
    bool active = strcmp(fields[2].value, "1") == 0;
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    DbStmt *statement = NULL;
    char *before = NULL, *after = NULL;
    if (!prepare(&statement, "SELECT role,active FROM users WHERE id=?") ||
        db_bind_int64(statement, 1, id) != DB_OK) goto database_failure;
    int result = db_step(statement);
    if (result == DB_DONE) {
        db_finalize(statement); statement = NULL;
        rollback(); error(response, 404, "Utente non trovato."); goto done;
    }
    if (result != DB_ROW) goto database_failure;
    bool was_manager = strcmp(column_text(statement, 0), "manager") == 0 &&
        db_column_int(statement, 1) == 1;
    db_finalize(statement); statement = NULL;
    before = user_snapshot(id);
    if (!before) goto database_failure;
    if (was_manager && (!active || strcmp(fields[1].value, "manager") != 0)) {
        if (!prepare(&statement, "SELECT COUNT(*) FROM users WHERE role='manager' AND active=1") ||
            db_step(statement) != DB_ROW) goto database_failure;
        DbInt managers = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
        if (managers <= 1) {
            rollback();
            error(response, 409, "Deve rimanere almeno un gestore attivo."); goto done;
        }
    }
    if (!prepare(&statement, "UPDATE users SET name=?,role=?,active=? WHERE id=?") ||
        !bind_text(statement, 1, fields[0].value) || !bind_text(statement, 2, fields[1].value) ||
        db_bind_int(statement, 3, active ? 1 : 0) != DB_OK ||
        db_bind_int64(statement, 4, id) != DB_OK ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    after = user_snapshot(id);
    if (!after || !storage_audit(database, actor_id, "users.update", "user", id, before, after))
        goto database_failure;
    if (commit_transaction(response)) {
        Json json = {0}; append(&json, "{\"ok\":true}");
        finish_json(response, 200, &json);
    }
    goto done;
database_failure:
    db_finalize(statement); statement = NULL;
    rollback(); server_error(response);
done:
    free(before); free(after);
    free_fields(fields, count);
}

bool accounts_handle(const char *method, const char *target, const char *query,
                     const char *body, const Account *actor,
                     WorkspaceResponse *response) {
    if (!method || !target || !response) return false;
    bool collection = strcmp(target, "/api/users") == 0;
    bool item = strncmp(target, "/api/users/", 11) == 0;
    if (!collection && !item) return false;
    response->body = NULL;
    response->status = 0;
    if (!actor) { error(response, 401, "Accedi per continuare."); return true; }
    if (strcmp(actor->role, "manager") != 0) {
        error(response, 403, "Solo i gestori possono amministrare gli utenti."); return true;
    }
    if (oidc_users_managed() && strcmp(method, "POST") == 0) {
        error(response, 403, "Gli utenti sono gestiti in Keycloak."); return true;
    }
    if (!database) { server_error(response); return true; }
    if (query && *query) {
        error(response, 400, "Parametri della richiesta non validi."); return true;
    }
    if (collection && strcmp(method, "GET") == 0) list_users(response);
    else if (collection && strcmp(method, "POST") == 0) create_user(body, actor->id, response);
    else if (item) {
        DbInt id;
        if (!parse_id(target + 11, &id)) error(response, 404, "Utente non trovato.");
        else if (strcmp(method, "POST") == 0) update_user(body, id, actor->id, response);
        else error(response, 405, "Metodo non consentito.");
    } else error(response, 405, "Metodo non consentito.");
    return true;
}
