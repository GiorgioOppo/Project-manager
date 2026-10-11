/* Private asset inventory: local SQLite store, TLS API, immutable revision snapshots. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
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
#include "inventory_backup.h"

#define HEADER_LIMIT 8192
#define BODY_LIMIT (800 * 1024)
#define RESPONSE_LIMIT (128 * 1024)
#define FIELD_COUNT 24
#define ASSET_COLUMNS "id,asset_tag,name,category,serial_number,manufacturer,model,status,location,assignee,purchase_date,warranty_until,notes,actor_id,version,created_at,updated_at,asset_type,details"
typedef struct {
    char token[129];
    const char *bind_address, *allowed_client, *cert, *key, *database;
    int port;
} Config;
typedef struct {
    char method[12], path[4096], authorization[256], content_type[128];
    char body[BODY_LIMIT + 1];
    size_t length;
} Request;
typedef struct { char *key, *value; } Field;
typedef struct { Field fields[FIELD_COUNT]; int count; } Form;
static Config config;
static sqlite3 *database;
static volatile sig_atomic_t stopping;
static const char *const names[] = {"asset_tag", "name", "category", "serial_number", "manufacturer", "model", "status", "location", "assignee", "purchase_date", "warranty_until", "notes"};
static const size_t maxima[] = {64,160,80,120,120,120,16,160,160,10,10,4000};
static const char *const statuses[] = {"available", "assigned", "maintenance", "retired", "lost"};
static const char *const asset_types[] = {"it_device", "network_device", "physical", "software", "license"};
static bool valid_date(const char *date);
static const char *jstr(json_object *o, const char *key) {
    json_object *v = NULL;
    return json_object_object_get_ex(o, key, &v) && v ? json_object_get_string(v) : "";
}
static sqlite3_int64 jint(json_object *o, const char *key) {
    json_object *v = NULL;
    return json_object_object_get_ex(o, key, &v) && v ? json_object_get_int64(v) : 0;
}
static bool timestamp_now(char timestamp[32]) {
    time_t now = time(NULL);
    struct tm utc;
    return gmtime_r(&now, &utc) && strftime(timestamp, 32, "%Y-%m-%dT%H:%M:%SZ", &utc);
}
static bool business_date(char today[11]) {
    time_t now = time(NULL);
    struct tm local;
    return localtime_r(&now, &local) && strftime(today, 11, "%Y-%m-%d", &local);
}
static bool license_expired(json_object *details) {
    const char *expires = jstr(details, "expires_on");
    char today[11];
    return *expires && business_date(today) && strcmp(expires, today) < 0;
}

static const char *env_value(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}
static bool integer(const char *text, sqlite3_int64 min, sqlite3_int64 max, sqlite3_int64 *out) {
    if (!text || !*text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (errno || !end || *end || value < min || value > max) return false;
    *out = (sqlite3_int64)value;
    return true;
}
static bool number(const char *text, unsigned long min, unsigned long max, int *out) {
    sqlite3_int64 value;
    if (!integer(text, (sqlite3_int64)min, (sqlite3_int64)max, &value)) return false;
    *out = (int)value;
    return true;
}
static bool secret(const char *path, char *out, size_t cap) {
    if (!path || !*path) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && !(st.st_mode & 0022) && st.st_size < (off_t)cap;
    size_t used = 0;
    while (ok && used < cap - 1) {
        ssize_t count = read(fd, out + used, cap - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (!count) break;
        used += (size_t)count;
    }
    close(fd);
    if (!ok || memchr(out, 0, used)) return false;
    while (used && (out[used-1] == '\n' || out[used-1] == '\r')) used--;
    out[used] = 0;
    return used >= 32;
}
static bool load_config(void) {
    if (setenv("TZ", env_value("INVENTORY_BUSINESS_TIMEZONE", "Europe/Rome"), 1)) return false;
    tzset();
    config.bind_address = env_value("INVENTORY_BIND", "172.30.80.2");
    config.allowed_client = env_value("INVENTORY_ALLOWED_CLIENT", "172.30.80.10");
    config.cert = env_value("INVENTORY_TLS_CERT_FILE", "/etc/punto/tls/inventory-addon.crt");
    config.key = env_value("INVENTORY_TLS_KEY_FILE", "/etc/punto/tls/inventory-addon.key");
    config.database = env_value("INVENTORY_DB_PATH", "/var/lib/punto-inventory/inventory.sqlite3");
    /* Read optional LF/CRLF separately from the normalized 128-byte token limit. */
    char token[131] = {0};
    bool valid_token = secret(env_value("INVENTORY_API_TOKEN_FILE", ""), token, sizeof(token)) &&
                       strlen(token) < sizeof(config.token);
    if (valid_token) memcpy(config.token, token, strlen(token) + 1);
    OPENSSL_cleanse(token, sizeof(token));
    if (!valid_token || !number(env_value("INVENTORY_PORT", "8092"), 1, 65535, &config.port)) return false;
    for (const unsigned char *p = (const unsigned char *)config.token; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_') return false;
    struct in_addr address;
    if (inet_pton(AF_INET, config.bind_address, &address) != 1) return false;
    char *copy = strdup(config.allowed_client);
    if (!copy) return false;
    bool valid = *copy && copy[strlen(copy)-1] != ',';
    char *save = NULL;
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save))
        if (inet_pton(AF_INET, p, &address) != 1) valid = false;
    if (strstr(config.allowed_client, ",,")) valid = false;
    free(copy);
    return valid;
}
static void addstr(json_object *o, const char *key, const char *value) {
    json_object_object_add(o, key, json_object_new_string(value ? value : ""));
}
static void addint(json_object *o, const char *key, sqlite3_int64 value) {
    json_object_object_add(o, key, json_object_new_int64(value));
}
static json_object *ok_json(void) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(true));
    return o;
}
static json_object *error_json(const char *message) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(false));
    addstr(o, "error", message);
    return o;
}
static json_object *failure(int *status, int code, const char *message) {
    *status = code;
    return error_json(message);
}
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Reject invalid Unicode, overlong encodings, surrogate values, and controls. */
static bool utf8_text(const char *text, bool multiline) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t value;
        size_t count;
        if (*p < 0x80) { value = *p; count = 1; }
        else if (*p >= 0xc2 && *p <= 0xdf) { value = *p & 0x1f; count = 2; }
        else if (*p >= 0xe0 && *p <= 0xef) { value = *p & 0x0f; count = 3; }
        else if (*p >= 0xf0 && *p <= 0xf4) { value = *p & 0x07; count = 4; }
        else return false;
        for (size_t i = 1; i < count; i++) {
            if (!p[i] || (p[i] & 0xc0) != 0x80) return false;
            value = (value << 6) | (p[i] & 0x3f);
        }
        if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
            (count == 4 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
        if ((value < 32 && !(multiline && (value == 9 || value == 10 || value == 13))) ||
            (value >= 0x7f && value <= 0x9f)) return false;
        p += count;
    }
    return true;
}
static bool decode(const char *source, size_t length, char **out) {
    char *value = malloc(length + 1);
    if (!value) return false;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            if (i + 2 >= length) { free(value); return false; }
            int a = hex_value((unsigned char)source[i+1]), b = hex_value((unsigned char)source[i+2]);
            if (a < 0 || b < 0) { free(value); return false; }
            c = (unsigned char)(a * 16 + b);
            i += 2;
        } else if (c == '+') c = ' ';
        if (!c) { free(value); return false; }
        value[used++] = (char)c;
    }
    value[used] = 0;
    if (!utf8_text(value, true)) { free(value); return false; }
    *out = value;
    return true;
}
static void form_free(Form *form) {
    for (int i = 0; i < form->count; i++) {
        free(form->fields[i].key);
        if (form->fields[i].value) {
            OPENSSL_cleanse(form->fields[i].value, strlen(form->fields[i].value));
            free(form->fields[i].value);
        }
    }
    memset(form, 0, sizeof(*form));
}
static bool form_parse(const char *text, Form *form) {
    memset(form, 0, sizeof(*form));
    for (const char *p = text; *p;) {
        if (form->count >= FIELD_COUNT) goto fail;
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq || eq == p) goto fail;
        Field *f = &form->fields[form->count++];
        if (!decode(p, (size_t)(eq-p), &f->key) || !decode(eq+1, (size_t)(end-eq-1), &f->value)) goto fail;
        for (int i = 0; i < form->count-1; i++)
            if (!strcmp(form->fields[i].key, f->key)) goto fail;
        p = *end ? end + 1 : end;
        if (*end && !*p) goto fail;
    }
    return true;
fail:
    form_free(form);
    return false;
}
static const char *field(const Form *form, const char *name) {
    for (int i = 0; i < form->count; i++)
        if (!strcmp(form->fields[i].key, name)) return form->fields[i].value;
    return "";
}
static bool has_field(const Form *form, const char *name) {
    for (int i = 0; i < form->count; i++)
        if (!strcmp(form->fields[i].key, name)) return true;
    return false;
}
static bool only_fields(const Form *form, const char *const *allowed, size_t count) {
    for (int i = 0; i < form->count; i++) {
        bool found = false;
        for (size_t j = 0; j < count; j++)
            if (!strcmp(form->fields[i].key, allowed[j])) found = true;
        if (!found) return false;
    }
    return true;
}
static bool execute(const char *sql) { return sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK; }
static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) return NULL;
    return statement;
}
static bool init_database(void) {
    if (sqlite3_open_v2(config.database, &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) return false;
    sqlite3_busy_timeout(database, 3000);
    sqlite3_stmt *version = prepare("PRAGMA user_version");
    if (!version) return false;
    int current = sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : -1;
    sqlite3_finalize(version);
    if (current < 0 || current > 3) return false;
    if (!execute("PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;BEGIN IMMEDIATE;")) return false;
    bool ok = true;
    if (current == 0) ok = execute(
        "CREATE TABLE assets("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE,"
        "name TEXT NOT NULL,category TEXT NOT NULL,serial_number TEXT NOT NULL,manufacturer TEXT NOT NULL,"
        "model TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('available','assigned','maintenance','retired','lost')),"
        "location TEXT NOT NULL,assignee TEXT NOT NULL,purchase_date TEXT NOT NULL,warranty_until TEXT NOT NULL,"
        "notes TEXT NOT NULL,actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL CHECK(version>0),"
        "created_at TEXT NOT NULL,updated_at TEXT NOT NULL);"
        "CREATE TABLE asset_history(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "asset_id INTEGER NOT NULL REFERENCES assets(id),action TEXT NOT NULL CHECK(action IN ('created','updated')),"
        "actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL,created_at TEXT NOT NULL,"
        "snapshot TEXT NOT NULL,UNIQUE(asset_id,version));");
    if (ok && current < 2) ok = execute(
        "ALTER TABLE assets ADD COLUMN asset_type TEXT NOT NULL DEFAULT 'it_device' CHECK(asset_type IN ('it_device','network_device','physical','software','license'));"
        "ALTER TABLE assets ADD COLUMN details TEXT NOT NULL DEFAULT '{}';"
        "ALTER TABLE asset_history ADD COLUMN change TEXT;"
        "CREATE TABLE asset_relations(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "source_id INTEGER NOT NULL REFERENCES assets(id),target_id INTEGER REFERENCES assets(id),"
        "relation_type TEXT NOT NULL CHECK(relation_type IN ('installed_on','connected_to','licensed_to')),"
        "assignee TEXT NOT NULL DEFAULT '' COLLATE NOCASE,quantity INTEGER NOT NULL CHECK(quantity BETWEEN 1 AND 1000000),"
        "installed_version TEXT NOT NULL DEFAULT '',source_port TEXT NOT NULL DEFAULT '',target_port TEXT NOT NULL DEFAULT '',"
        "notes TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,"
        "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),removed_at TEXT,removed_actor_id INTEGER);"
        "CREATE UNIQUE INDEX active_relation_target ON asset_relations(source_id,relation_type,target_id) WHERE active=1 AND target_id IS NOT NULL;"
        "CREATE UNIQUE INDEX active_relation_assignee ON asset_relations(source_id,relation_type,assignee COLLATE NOCASE) WHERE active=1 AND target_id IS NULL;"
        "CREATE INDEX relation_target_id ON asset_relations(target_id,id);"
        "CREATE INDEX relation_source_id ON asset_relations(source_id,id);");
    if (ok && current < 3) ok = execute(
        "CREATE TABLE inventory_import_previews(preview_id TEXT PRIMARY KEY,actor_id INTEGER NOT NULL CHECK(actor_id>0),"
        "created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,used INTEGER NOT NULL DEFAULT 0 CHECK(used IN (0,1)),plan TEXT NOT NULL);"
        "CREATE INDEX inventory_import_previews_expiry ON inventory_import_previews(expires_at);");
    if (ok) ok = execute(
        "CREATE INDEX IF NOT EXISTS assets_status_id ON assets(status,id);"
        "CREATE INDEX IF NOT EXISTS assets_type_id ON assets(asset_type,id);"
        "CREATE INDEX IF NOT EXISTS asset_history_asset_id ON asset_history(asset_id,id);"
        "PRAGMA user_version=3;");
    if (ok) ok = execute("COMMIT");
    if (!ok) (void)execute("ROLLBACK");
    return ok;
}
static sqlite3_int64 allocated_seats(sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s = prepare("SELECT COALESCE(SUM(quantity),0) FROM asset_relations WHERE source_id=? AND relation_type='licensed_to' AND active=1");
    if (!s) { *error = true; return 0; }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_int64 count = 0;
    if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int64(s, 0); else *error = true;
    sqlite3_finalize(s);
    return count;
}
static json_object *asset_row(sqlite3_stmt *s) {
    json_object *asset = json_object_new_object();
    addint(asset, "id", sqlite3_column_int64(s, 0));
    for (int i = 0; i < 12; i++) addstr(asset, names[i], (const char *)sqlite3_column_text(s, i+1));
    addint(asset, "actor_id", sqlite3_column_int64(s, 13));
    addint(asset, "version", sqlite3_column_int64(s, 14));
    addstr(asset, "created_at", (const char *)sqlite3_column_text(s, 15));
    addstr(asset, "updated_at", (const char *)sqlite3_column_text(s, 16));
    const char *type = (const char *)sqlite3_column_text(s, 17);
    addstr(asset, "asset_type", type);
    json_object *details = json_tokener_parse((const char *)sqlite3_column_text(s, 18));
    if (!details || !json_object_is_type(details, json_type_object)) {
        if (details) json_object_put(details);
        json_object_put(asset);
        return NULL;
    }
    json_object_object_add(asset, "details", details);
    if (!strcmp(type, "license")) {
        bool error = false;
        sqlite3_int64 allocated = allocated_seats(sqlite3_column_int64(s, 0), &error);
        if (error) { json_object_put(asset); return NULL; }
        json_object *usage = json_object_new_object();
        addint(usage, "allocated", allocated);
        sqlite3_int64 available = jint(details, "seats_total") - allocated;
        addint(usage, "available", available < 0 ? 0 : available);
        json_object_object_add(usage, "expired", json_object_new_boolean(license_expired(details)));
        json_object_object_add(asset, "license_usage", usage);
    }
    return asset;
}
static json_object *load_asset(sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s = prepare("SELECT " ASSET_COLUMNS " FROM assets WHERE id=?");
    if (!s) { *error = true; return NULL; }
    sqlite3_bind_int64(s, 1, id);
    int result = sqlite3_step(s);
    json_object *asset = result == SQLITE_ROW ? asset_row(s) : NULL;
    if ((result == SQLITE_ROW && !asset) || (result != SQLITE_ROW && result != SQLITE_DONE)) *error = true;
    sqlite3_finalize(s);
    return asset;
}
static json_object *summary(void) {
    sqlite3_stmt *s = prepare("SELECT status,COUNT(*) FROM assets GROUP BY status");
    if (!s) return NULL;
    json_object *o = json_object_new_object();
    sqlite3_int64 total = 0;
    for (size_t i = 0; i < 5; i++) addint(o, statuses[i], 0);
    int result;
    while ((result = sqlite3_step(s)) == SQLITE_ROW) {
        sqlite3_int64 count = sqlite3_column_int64(s, 1);
        addint(o, (const char *)sqlite3_column_text(s, 0), count);
        total += count;
    }
    sqlite3_finalize(s);
    if (result != SQLITE_DONE) { json_object_put(o); return NULL; }
    addint(o, "total", total);
    return o;
}
static bool valid_date(const char *date) {
    if (!*date) return true;
    if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    int year = (date[0]-'0')*1000 + (date[1]-'0')*100 + (date[2]-'0')*10 + date[3]-'0';
    int month = (date[5]-'0')*10 + date[6]-'0', day = (date[8]-'0')*10 + date[9]-'0';
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!year || month < 1 || month > 12) return false;
    int maximum = days[month-1] + (month == 2 && (!(year%400) || (!(year%4) && year%100)));
    return day >= 1 && day <= maximum;
}
static bool nonblank(const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) if (!isspace(*p)) return true;
    return false;
}
/* json-c accepts repeated keys; reject them before canonicalizing a flat details object. */
static json_object *strict_details(const char *text) {
    json_tokener *tokener = json_tokener_new();
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *o = json_tokener_parse_ex(tokener, text, (int)strlen(text)+1);
    bool ok = json_tokener_get_error(tokener) == json_tokener_success && o && json_object_is_type(o, json_type_object);
    json_tokener_free(tokener);
    if (!ok) { if (o) json_object_put(o); return NULL; }
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '{') goto invalid;
    char *keys[24] = {0};
    size_t count = 0;
    while (true) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '}') break;
        if (*p != '"' || count >= 24) goto duplicate_invalid;
        const char *start = p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p != '"') goto duplicate_invalid;
        p++;
        char *raw = strndup(start, (size_t)(p-start));
        json_object *key = raw ? json_tokener_parse(raw) : NULL;
        free(raw);
        if (!key || !json_object_is_type(key, json_type_string) ||
            (size_t)json_object_get_string_len(key) != strlen(json_object_get_string(key))) {
            if (key) json_object_put(key);
            goto duplicate_invalid;
        }
        const char *value = json_object_get_string(key);
        for (size_t i = 0; i < count; i++) if (!strcmp(keys[i], value)) {
            json_object_put(key); goto duplicate_invalid;
        }
        keys[count] = strdup(value);
        json_object_put(key);
        if (!keys[count++]) goto duplicate_invalid;
        while (isspace((unsigned char)*p)) p++;
        if (*p++ != ':') goto duplicate_invalid;
        bool quoted = false;
        while (*p) {
            if (*p == '"') quoted = !quoted;
            else if (quoted && *p == '\\' && p[1]) { p += 2; continue; }
            else if (!quoted && (*p == '[' || *p == '{')) goto duplicate_invalid;
            else if (!quoted && (*p == ',' || *p == '}')) break;
            p++;
        }
        if (*p == '}') break;
        if (*p++ != ',') goto duplicate_invalid;
    }
    for (size_t i = 0; i < count; i++) free(keys[i]);
    return o;
duplicate_invalid:
    for (size_t i = 0; i < count; i++) free(keys[i]);
invalid:
    json_object_put(o);
    return NULL;
}
typedef struct { const char *name; size_t maximum; int kind; } DetailSpec;
/* kind 0 string, 1 date, 2 optional integer, 3 required positive integer. */
static json_object *canonical_details(const char *type, json_object *input) {
    static const DetailSpec devices[] = {{"hostname",253,0},{"ip_address",45,0},{"mac_address",17,0},{"operating_system",120,0}};
    static const DetailSpec network[] = {{"hostname",253,0},{"ip_address",45,0},{"mac_address",17,0},{"operating_system",120,0},{"network_role",80,0},{"vlan",4094,2},{"port_count",65535,2}};
    static const DetailSpec physical[] = {{"asset_kind",80,0},{"department",120,0},{"maintenance_due",10,1}};
    static const DetailSpec software[] = {{"publisher",120,0},{"software_version",80,0},{"edition",80,0},{"software_kind",80,0}};
    static const DetailSpec license[] = {{"software_id",INT64_MAX,3},{"license_model",20,0},{"seats_total",1000000,3},{"expires_on",10,1},{"renewal_on",10,1},{"renewal_mode",16,0},{"license_reference",120,0}};
    const DetailSpec *spec = devices;
    size_t count = sizeof(devices)/sizeof(*devices);
    if (!strcmp(type, "network_device")) { spec = network; count = sizeof(network)/sizeof(*network); }
    else if (!strcmp(type, "physical")) { spec = physical; count = sizeof(physical)/sizeof(*physical); }
    else if (!strcmp(type, "software")) { spec = software; count = sizeof(software)/sizeof(*software); }
    else if (!strcmp(type, "license")) { spec = license; count = sizeof(license)/sizeof(*license); }
    json_object_object_foreach(input, key, unused) {
        (void)unused;
        bool known = false;
        for (size_t i = 0; i < count; i++) if (!strcmp(key, spec[i].name)) known = true;
        if (!known) return NULL;
    }
    json_object *out = json_object_new_object();
    for (size_t i = 0; i < count; i++) {
        json_object *v = NULL;
        bool present = json_object_object_get_ex(input, spec[i].name, &v);
        if (spec[i].kind >= 2) {
            if (!present || !v) { if (spec[i].kind == 3) goto invalid; else continue; }
            if (!json_object_is_type(v, json_type_int)) goto invalid;
            sqlite3_int64 value = json_object_get_int64(v);
            sqlite3_int64 minimum = !strcmp(spec[i].name, "port_count") ? 0 : 1;
            if (value < minimum || (uint64_t)value > (uint64_t)spec[i].maximum) goto invalid;
            addint(out, spec[i].name, value);
        } else {
            if (present && (!v || !json_object_is_type(v, json_type_string))) goto invalid;
            const char *value = present ? json_object_get_string(v) : "";
            if ((present && (size_t)json_object_get_string_len(v) != strlen(value)) ||
                strlen(value) > spec[i].maximum || !utf8_text(value, false) ||
                (spec[i].kind == 1 && !valid_date(value))) goto invalid;
            if (!strcmp(spec[i].name, "ip_address") && *value) {
                struct in6_addr address;
                if (inet_pton(AF_INET, value, &address) != 1 && inet_pton(AF_INET6, value, &address) != 1) goto invalid;
            }
            if (!strcmp(spec[i].name, "mac_address") && *value) {
                if (strlen(value) != 17) goto invalid;
                char mac[18];
                for (size_t j = 0; j < 17; j++) {
                    if ((j%3 == 2 && value[j] != ':') || (j%3 != 2 && hex_value((unsigned char)value[j]) < 0)) goto invalid;
                    mac[j] = (char)tolower((unsigned char)value[j]);
                }
                mac[17] = 0;
                addstr(out, spec[i].name, mac);
                continue;
            }
            if (!strcmp(spec[i].name, "license_model")) {
                if (!present) value = "per_device";
                if (strcmp(value,"per_device") && strcmp(value,"per_user") && strcmp(value,"concurrent") && strcmp(value,"site")) goto invalid;
            }
            if (!strcmp(spec[i].name, "renewal_mode")) {
                if (!present) value = "manual";
                if (strcmp(value,"manual") && strcmp(value,"automatic") && strcmp(value,"none")) goto invalid;
            }
            addstr(out, spec[i].name, value);
        }
    }
    return out;
invalid:
    json_object_put(out);
    return NULL;
}
static bool active_relations(sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s = prepare("SELECT 1 FROM asset_relations WHERE active=1 AND (source_id=?1 OR target_id=?1) LIMIT 1");
    if (!s) { *error = true; return false; }
    sqlite3_bind_int64(s, 1, id);
    int result = sqlite3_step(s);
    if (result != SQLITE_ROW && result != SQLITE_DONE) *error = true;
    sqlite3_finalize(s);
    return result == SQLITE_ROW;
}
static bool referenced_software(sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s = prepare("SELECT 1 FROM assets WHERE asset_type='license' AND json_extract(details,'$.software_id')=? LIMIT 1");
    if (!s) { *error = true; return false; }
    sqlite3_bind_int64(s, 1, id);
    int result = sqlite3_step(s);
    if (result != SQLITE_ROW && result != SQLITE_DONE) *error = true;
    sqlite3_finalize(s);
    return result == SQLITE_ROW;
}
static bool history_write(sqlite3_int64 id, const char *action, sqlite3_int64 actor, sqlite3_int64 version,
                          const char *timestamp, json_object *asset, json_object *change) {
    sqlite3_stmt *s = prepare("INSERT INTO asset_history(asset_id,action,actor_id,version,created_at,snapshot,change) VALUES(?,?,?,?,?,?,?)");
    bool ok = s && sqlite3_bind_int64(s,1,id) == SQLITE_OK &&
        sqlite3_bind_text(s,2,action,-1,SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(s,3,actor) == SQLITE_OK && sqlite3_bind_int64(s,4,version) == SQLITE_OK &&
        sqlite3_bind_text(s,5,timestamp,-1,SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(s,6,json_object_to_json_string_ext(asset,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok) ok = change ? sqlite3_bind_text(s,7,json_object_to_json_string_ext(change,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT) == SQLITE_OK : sqlite3_bind_null(s,7) == SQLITE_OK;
    if (ok) ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

static json_object *save_asset(sqlite3_int64 id, Form *form, int *status) {
    const char *allowed[16];
    for (int i = 0; i < 12; i++) allowed[i] = names[i];
    allowed[12] = "actor_id"; allowed[13] = "asset_type"; allowed[14] = "details"; allowed[15] = "version";
    if (!only_fields(form, allowed, id ? 16 : 15)) return failure(status, 400, "Campo inventario sconosciuto.");
    const char *values[12];
    for (int i = 0; i < 12; i++) {
        values[i] = field(form, names[i]);
        if (strlen(values[i]) > maxima[i] || !utf8_text(values[i], i == 11))
            return failure(status, 400, "Campo troppo lungo o con caratteri non validi.");
    }
    if (!nonblank(values[0]) || !nonblank(values[1])) return failure(status, 400, "Etichetta e nome del bene sono obbligatori.");
    if (!*values[2]) values[2] = "hardware";
    if (!*values[6]) values[6] = "available";
    bool valid_status = false;
    for (int i = 0; i < 5; i++) if (!strcmp(values[6], statuses[i])) valid_status = true;
    if (!valid_status) return failure(status, 400, "Stato del bene non valido.");
    if (!strcmp(values[6], "assigned") && !nonblank(values[8])) return failure(status, 400, "Indicare l'assegnatario del bene assegnato.");
    if (!valid_date(values[9]) || !valid_date(values[10]) ||
        (*values[9] && *values[10] && strcmp(values[10], values[9]) < 0))
        return failure(status, 400, "Date non valide: la garanzia non può precedere l'acquisto.");
    sqlite3_int64 actor, version = 0;
    if (!integer(field(form, "actor_id"), 1, INT64_MAX, &actor)) return failure(status, 400, "Utente autore non valido.");
    if (id && !integer(field(form, "version"), 1, INT64_MAX-1, &version)) return failure(status, 400, "Versione corrente obbligatoria e valida.");
    if (strlen(field(form,"details")) > 4096) return failure(status,400,"Dettagli inventario troppo lunghi.");
    bool outer_transaction = !sqlite3_get_autocommit(database);
    if (!execute(outer_transaction ? "SAVEPOINT inventory_asset_save" : "BEGIN IMMEDIATE"))
        return failure(status, 503, "Archivio inventario temporaneamente occupato.");
    bool error = false;
    int code = 500;
    const char *message = "Salvataggio inventario non riuscito.";
    json_object *old = id ? load_asset(id, &error) : NULL, *details = NULL, *asset = NULL;
    if (id && (!old || error)) { code = error ? 500 : 404; message = error ? "Impossibile leggere il bene." : "Bene non trovato."; goto fail; }
    if (old && jint(old,"version") != version) { code = 409; message = "Il bene è stato modificato: aggiorna i dati prima di salvare."; goto fail; }
    const char *type = has_field(form,"asset_type") ? field(form,"asset_type") : old ? jstr(old,"asset_type") : "it_device";
    bool known_type = false;
    for (size_t i = 0; i < 5; i++) if (!strcmp(type,asset_types[i])) known_type = true;
    if (!known_type) { code = 400; message = "Tipologia del bene non valida."; goto fail; }
    bool type_changed = old && strcmp(type,jstr(old,"asset_type"));
    if (old && !has_field(form,"details") && !type_changed) {
        json_object *v = NULL; json_object_object_get_ex(old,"details",&v); details = json_object_get(v);
    } else if (!old && !has_field(form,"asset_type") && !has_field(form,"details")) details = json_object_new_object();
    else {
        json_object *input = has_field(form,"details") ? strict_details(field(form,"details")) : json_object_new_object();
        if (input) { details = canonical_details(type,input); json_object_put(input); }
        if (!details) { code = 400; message = "Dettagli non validi per la tipologia selezionata."; goto fail; }
    }
    if (type_changed && (active_relations(id,&error) || referenced_software(id,&error))) {
        code = 409; message = "Rimuovere le relazioni attive e i riferimenti delle licenze prima di cambiare tipologia."; goto fail;
    }
    if (error) goto fail;
    if (!strcmp(type,"license")) {
        sqlite3_int64 software_id = jint(details,"software_id");
        json_object *software = software_id ? load_asset(software_id,&error) : NULL;
        bool valid = software && !strcmp(jstr(software,"asset_type"),"software");
        if (software) json_object_put(software);
        if (!valid) { code = error ? 500 : 400; message = "La licenza deve fare riferimento a un software esistente."; goto fail; }
        sqlite3_int64 allocated = old ? allocated_seats(id,&error) : 0;
        if (error) goto fail;
        if (jint(details,"seats_total") < allocated) { code = 409; message = "Il numero di posti non può essere inferiore alle assegnazioni attive."; goto fail; }
        if (old && allocated) {
            json_object *previous = NULL; json_object_object_get_ex(old,"details",&previous);
            if (jint(details,"software_id") != jint(previous,"software_id") || strcmp(jstr(details,"license_model"),jstr(previous,"license_model"))) {
                code = 409; message = "Rimuovere le assegnazioni prima di cambiare software o modello di licenza."; goto fail;
            }
        }
    }
    char timestamp[32];
    if (!timestamp_now(timestamp)) { message = "Impossibile determinare l'ora della modifica."; goto fail; }
    sqlite3_stmt *s = prepare(id ?
        "UPDATE assets SET asset_tag=?1,name=?2,category=?3,serial_number=?4,manufacturer=?5,model=?6,status=?7,location=?8,assignee=?9,purchase_date=?10,warranty_until=?11,notes=?12,actor_id=?13,version=version+1,updated_at=?14,asset_type=?17,details=?18 WHERE id=?15 AND version=?16" :
        "INSERT INTO assets(asset_tag,name,category,serial_number,manufacturer,model,status,location,assignee,purchase_date,warranty_until,notes,actor_id,created_at,updated_at,version,asset_type,details) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?14,1,?17,?18)");
    bool ok = s != NULL;
    for (int i = 0; ok && i < 12; i++) ok = sqlite3_bind_text(s,i+1,values[i],-1,SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok) ok = sqlite3_bind_int64(s,13,actor) == SQLITE_OK && sqlite3_bind_text(s,14,timestamp,-1,SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(s,17,type,-1,SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(s,18,json_object_to_json_string_ext(details,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT) == SQLITE_OK;
    if (ok && id) ok = sqlite3_bind_int64(s,15,id) == SQLITE_OK && sqlite3_bind_int64(s,16,version) == SQLITE_OK;
    int result = ok ? sqlite3_step(s) : SQLITE_ERROR;
    int extended = sqlite3_extended_errcode(database);
    sqlite3_finalize(s);
    if (result != SQLITE_DONE || sqlite3_changes(database) != 1) {
        if (extended == SQLITE_CONSTRAINT_UNIQUE) { code = 409; message = "Etichetta inventario già presente."; }
        goto fail;
    }
    bool created = !id;
    if (created) id = sqlite3_last_insert_rowid(database);
    asset = load_asset(id,&error);
    ok = asset && !error && history_write(id,created ? "created" : "updated",actor,created ? 1 : version+1,timestamp,asset,NULL);
    if (ok) ok = execute(outer_transaction ? "RELEASE inventory_asset_save" : "COMMIT");
    if (!ok) { message = "Storico inventario non salvato; modifica annullata."; goto fail; }
    if (old) json_object_put(old);
    json_object_put(details);
    json_object *o = ok_json();
    json_object_object_add(o,"asset",asset);
    *status = created ? 201 : 200;
    return o;
fail:
    (void)execute(outer_transaction ? "ROLLBACK TO inventory_asset_save;RELEASE inventory_asset_save" : "ROLLBACK");
    if (old) json_object_put(old);
    if (details) json_object_put(details);
    if (asset) json_object_put(asset);
    return failure(status,code,message);
}
static bool page_query(const char *query, bool history, Form *form, int *limit, sqlite3_int64 *after) {
    static const char *const allowed[] = {"limit", "after_id", "q", "status", "asset_type"};
    if (!form_parse(query, form)) return false;
    bool ok = only_fields(form, allowed, history ? 2 : 5);
    *limit = 30;
    *after = 0;
    if (has_field(form, "limit") && !number(field(form, "limit"), 1, 100, limit)) ok = false;
    if (has_field(form, "after_id") && !integer(field(form, "after_id"), 1, INT64_MAX, after)) ok = false;
    if (strlen(field(form, "q")) > 200 || !utf8_text(field(form, "q"), false)) ok = false;
    if (has_field(form, "status")) {
        bool valid = false;
        for (int i = 0; i < 5; i++) if (!strcmp(field(form, "status"), statuses[i])) valid = true;
        if (!valid) ok = false;
    }
    if (has_field(form, "asset_type")) {
        bool valid = false;
        for (int i = 0; i < 5; i++) if (!strcmp(field(form,"asset_type"),asset_types[i])) valid = true;
        if (!valid) ok = false;
    }
    if (!ok) form_free(form);
    return ok;
}
static json_object *list_assets(const char *query, int *status) {
    Form form;
    int limit;
    sqlite3_int64 after;
    if (!page_query(query, false, &form, &limit, &after)) return failure(status, 400, "Filtri inventario non validi.");
    sqlite3_stmt *s = prepare("SELECT " ASSET_COLUMNS " FROM assets WHERE (?1=0 OR id<?1) AND (?2='' OR status=?2) AND (?5='' OR asset_type=?5) AND (?3='' OR instr(lower(asset_tag),lower(?3))>0 OR instr(lower(name),lower(?3))>0 OR instr(lower(category),lower(?3))>0 OR instr(lower(serial_number),lower(?3))>0 OR instr(lower(manufacturer),lower(?3))>0 OR instr(lower(model),lower(?3))>0 OR instr(lower(location),lower(?3))>0 OR instr(lower(assignee),lower(?3))>0 OR instr(lower(notes),lower(?3))>0 OR EXISTS(SELECT 1 FROM json_each(assets.details) typed WHERE typed.type='text' AND instr(lower(typed.value),lower(?3))>0)) ORDER BY id DESC LIMIT ?4");
    json_object *totals = summary();
    bool ok = s && totals && sqlite3_bind_int64(s, 1, after) == SQLITE_OK &&
        sqlite3_bind_text(s, 2, field(&form, "status"), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(s, 3, field(&form, "q"), -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_bind_int(s, 4, limit+1) == SQLITE_OK &&
        sqlite3_bind_text(s, 5, field(&form, "asset_type"), -1, SQLITE_TRANSIENT) == SQLITE_OK;
    form_free(&form);
    if (!ok) {
        sqlite3_finalize(s);
        if (totals) json_object_put(totals);
        return failure(status, 500, "Impossibile leggere l'inventario.");
    }
    json_object *o = ok_json(), *items = json_object_new_array();
    json_object_object_add(o, "items", items);
    json_object_object_add(o, "summary", totals);
    json_object_object_add(o, "next_after_id", NULL);
    size_t bytes = strlen(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    int count = 0, result = SQLITE_DONE;
    sqlite3_int64 last = 0;
    bool more = false;
    while ((result = sqlite3_step(s)) == SQLITE_ROW) {
        if (count >= limit) { more = true; break; }
        json_object *asset = asset_row(s);
        if (!asset) { result = SQLITE_CORRUPT; break; }
        size_t length = strlen(json_object_to_json_string_ext(asset, JSON_C_TO_STRING_PLAIN));
        if (bytes + length + (count ? 1 : 0) > RESPONSE_LIMIT-64) {
            json_object_put(asset);
            more = true;
            break;
        }
        bytes += length + (count ? 1 : 0);
        last = sqlite3_column_int64(s, 0);
        json_object_array_add(items, asset);
        count++;
    }
    sqlite3_finalize(s);
    if (result != SQLITE_DONE && result != SQLITE_ROW) {
        json_object_put(o);
        return failure(status, 500, "Lettura inventario interrotta.");
    }
    if (more && last) addint(o, "next_after_id", last);
    return o;
}
static json_object *asset_history(sqlite3_int64 id, const char *query, int *status) {
    Form form;
    int limit;
    sqlite3_int64 after;
    if (!page_query(query, true, &form, &limit, &after)) return failure(status, 400, "Filtri storico non validi.");
    form_free(&form);
    bool error = false;
    json_object *asset = load_asset(id, &error);
    if (!asset) return failure(status, error ? 500 : 404, error ? "Impossibile leggere il bene." : "Bene non trovato.");
    json_object_put(asset);
    sqlite3_stmt *s = prepare("SELECT id,action,actor_id,version,created_at,snapshot,change FROM asset_history WHERE asset_id=?1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3");
    if (!s || sqlite3_bind_int64(s, 1, id) != SQLITE_OK || sqlite3_bind_int64(s, 2, after) != SQLITE_OK || sqlite3_bind_int(s, 3, limit+1) != SQLITE_OK) {
        sqlite3_finalize(s);
        return failure(status, 500, "Impossibile leggere lo storico.");
    }
    json_object *o = ok_json(), *items = json_object_new_array();
    json_object_object_add(o, "items", items);
    json_object_object_add(o, "next_after_id", NULL);
    size_t bytes = strlen(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    int count = 0, result = SQLITE_DONE;
    sqlite3_int64 last = 0;
    bool more = false;
    while ((result = sqlite3_step(s)) == SQLITE_ROW) {
        if (count >= limit) { more = true; break; }
        json_object *snapshot = json_tokener_parse((const char *)sqlite3_column_text(s, 5));
        if (!snapshot || !json_object_is_type(snapshot, json_type_object)) {
            if (snapshot) json_object_put(snapshot);
            result = SQLITE_CORRUPT;
            break;
        }
        json_object *item = json_object_new_object();
        addint(item, "id", sqlite3_column_int64(s, 0));
        addstr(item, "action", (const char *)sqlite3_column_text(s, 1));
        addint(item, "actor_id", sqlite3_column_int64(s, 2));
        addint(item, "version", sqlite3_column_int64(s, 3));
        addstr(item, "created_at", (const char *)sqlite3_column_text(s, 4));
        json_object_object_add(item, "snapshot", snapshot);
        if (sqlite3_column_type(s,6) != SQLITE_NULL) {
            json_object *change = json_tokener_parse((const char *)sqlite3_column_text(s,6));
            if (!change || !json_object_is_type(change,json_type_object)) {
                if (change) json_object_put(change);
                json_object_put(item); result = SQLITE_CORRUPT; break;
            }
            json_object_object_add(item,"change",change);
        }
        size_t length = strlen(json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN));
        if (bytes + length + (count ? 1 : 0) > RESPONSE_LIMIT-64) {
            json_object_put(item);
            more = true;
            break;
        }
        bytes += length + (count ? 1 : 0);
        last = sqlite3_column_int64(s, 0);
        json_object_array_add(items, item);
        count++;
    }
    sqlite3_finalize(s);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        json_object_put(o);
        return failure(status, 500, "Lettura storico interrotta.");
    }
    if (more && last) addint(o, "next_after_id", last);
    return o;
}
#define RELATION_COLUMNS "r.id,r.source_id,r.target_id,r.relation_type,r.assignee,r.quantity,r.installed_version,r.source_port,r.target_port,r.notes,r.created_at,r.actor_id,r.active,s.asset_tag,s.name,s.asset_type,t.id,t.asset_tag,t.name,t.asset_type"
#define RELATION_FROM " FROM asset_relations r JOIN assets s ON s.id=r.source_id LEFT JOIN assets t ON t.id=r.target_id "
static json_object *relation_row(sqlite3_stmt *s, sqlite3_int64 perspective) {
    json_object *row = json_object_new_object();
    addint(row,"id",sqlite3_column_int64(s,0));
    addint(row,"source_id",sqlite3_column_int64(s,1));
    if (sqlite3_column_type(s,2) == SQLITE_NULL) json_object_object_add(row,"target_id",NULL);
    else addint(row,"target_id",sqlite3_column_int64(s,2));
    addstr(row,"relation_type",(const char *)sqlite3_column_text(s,3));
    addstr(row,"assignee",(const char *)sqlite3_column_text(s,4));
    addint(row,"quantity",sqlite3_column_int64(s,5));
    addstr(row,"installed_version",(const char *)sqlite3_column_text(s,6));
    addstr(row,"source_port",(const char *)sqlite3_column_text(s,7));
    addstr(row,"target_port",(const char *)sqlite3_column_text(s,8));
    addstr(row,"notes",(const char *)sqlite3_column_text(s,9));
    addstr(row,"created_at",(const char *)sqlite3_column_text(s,10));
    addint(row,"actor_id",sqlite3_column_int64(s,11));
    json_object_object_add(row,"active",json_object_new_boolean(sqlite3_column_int(s,12)));
    addstr(row,"direction",sqlite3_column_int64(s,1) == perspective ? "outgoing" : "incoming");
    json_object *source = json_object_new_object();
    addint(source,"id",sqlite3_column_int64(s,1));
    addstr(source,"asset_tag",(const char *)sqlite3_column_text(s,13));
    addstr(source,"name",(const char *)sqlite3_column_text(s,14));
    addstr(source,"asset_type",(const char *)sqlite3_column_text(s,15));
    json_object_object_add(row,"source",source);
    if (sqlite3_column_type(s,16) == SQLITE_NULL) json_object_object_add(row,"target",NULL);
    else {
        json_object *target = json_object_new_object();
        addint(target,"id",sqlite3_column_int64(s,16));
        addstr(target,"asset_tag",(const char *)sqlite3_column_text(s,17));
        addstr(target,"name",(const char *)sqlite3_column_text(s,18));
        addstr(target,"asset_type",(const char *)sqlite3_column_text(s,19));
        json_object_object_add(row,"target",target);
    }
    return row;
}
static json_object *load_relation(sqlite3_int64 source, sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s = prepare("SELECT " RELATION_COLUMNS RELATION_FROM "WHERE r.id=? AND r.source_id=?");
    if (!s) { *error = true; return NULL; }
    sqlite3_bind_int64(s,1,id); sqlite3_bind_int64(s,2,source);
    int result = sqlite3_step(s);
    json_object *relation = result == SQLITE_ROW ? relation_row(s,source) : NULL;
    if (result != SQLITE_ROW && result != SQLITE_DONE) *error = true;
    sqlite3_finalize(s);
    return relation;
}
static json_object *list_relations(sqlite3_int64 id, const char *query, int *status) {
    Form form;
    int limit;
    sqlite3_int64 after;
    if (!page_query(query,true,&form,&limit,&after)) return failure(status,400,"Filtri relazioni non validi.");
    form_free(&form);
    bool error = false;
    json_object *asset = load_asset(id,&error);
    if (!asset) return failure(status,error ? 500 : 404,error ? "Impossibile leggere il bene." : "Bene non trovato.");
    json_object_put(asset);
    sqlite3_stmt *s = prepare("SELECT " RELATION_COLUMNS RELATION_FROM "WHERE r.active=1 AND (r.source_id=?1 OR r.target_id=?1) AND (?2=0 OR r.id<?2) ORDER BY r.id DESC LIMIT ?3");
    if (!s || sqlite3_bind_int64(s,1,id) != SQLITE_OK || sqlite3_bind_int64(s,2,after) != SQLITE_OK || sqlite3_bind_int(s,3,limit+1) != SQLITE_OK) {
        sqlite3_finalize(s); return failure(status,500,"Impossibile leggere le relazioni.");
    }
    json_object *o = ok_json(), *items = json_object_new_array();
    json_object_object_add(o,"items",items);
    json_object_object_add(o,"next_after_id",NULL);
    size_t bytes = strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    int count = 0, result = SQLITE_DONE;
    sqlite3_int64 last = 0;
    bool more = false;
    while ((result = sqlite3_step(s)) == SQLITE_ROW) {
        if (count >= limit) { more = true; break; }
        json_object *item = relation_row(s,id);
        size_t length = strlen(json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN));
        if (bytes + length + (count ? 1 : 0) > RESPONSE_LIMIT-64) { json_object_put(item); more = true; break; }
        bytes += length + (count ? 1 : 0);
        last = sqlite3_column_int64(s,0);
        json_object_array_add(items,item); count++;
    }
    sqlite3_finalize(s);
    if (result != SQLITE_ROW && result != SQLITE_DONE) { json_object_put(o); return failure(status,500,"Lettura relazioni interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
static bool is_device(json_object *asset) {
    const char *type = jstr(asset,"asset_type");
    return !strcmp(type,"it_device") || !strcmp(type,"network_device");
}
static bool unavailable(json_object *asset) {
    const char *status = jstr(asset,"status");
    return !strcmp(status,"retired") || !strcmp(status,"lost");
}
static json_object *save_relation(sqlite3_int64 source_id, sqlite3_int64 remove_id, Form *form, int *status) {
    static const char *const allowed[] = {"actor_id","version","relation_type","target_id","assignee","quantity","installed_version","source_port","target_port","notes"};
    if (!only_fields(form,allowed,remove_id ? 2 : 10)) return failure(status,400,"Campo relazione sconosciuto.");
    sqlite3_int64 actor, version, target_id = 0, quantity = 1;
    if (!integer(field(form,"actor_id"),1,INT64_MAX,&actor) || !integer(field(form,"version"),1,INT64_MAX-1,&version))
        return failure(status,400,"Autore e versione corrente sono obbligatori.");
    if (!remove_id && ((*field(form,"target_id") && !integer(field(form,"target_id"),1,INT64_MAX,&target_id)) ||
        (has_field(form,"quantity") && !integer(field(form,"quantity"),1,1000000,&quantity))))
        return failure(status,400,"Destinazione o quantità non valida.");
    const char *relation_type = field(form,"relation_type"), *assignee = field(form,"assignee"),
        *installed_version = field(form,"installed_version"), *source_port = field(form,"source_port"),
        *target_port = field(form,"target_port"), *notes = field(form,"notes");
    if (strlen(assignee)>160 || !utf8_text(assignee,false) || strlen(installed_version)>80 || !utf8_text(installed_version,false) ||
        strlen(source_port)>80 || !utf8_text(source_port,false) || strlen(target_port)>80 || !utf8_text(target_port,false) ||
        strlen(notes)>2000 || !utf8_text(notes,true)) return failure(status,400,"Campo relazione troppo lungo o non valido.");
    char normalized_assignee[161];
    while (*assignee && isspace((unsigned char)*assignee)) assignee++;
    size_t assignee_length = strlen(assignee);
    while (assignee_length && isspace((unsigned char)assignee[assignee_length-1])) assignee_length--;
    memcpy(normalized_assignee,assignee,assignee_length); normalized_assignee[assignee_length] = 0;
    assignee = normalized_assignee;
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    int code = 500;
    const char *message = "Modifica relazione non riuscita.";
    bool error = false;
    json_object *source = load_asset(source_id,&error), *target = NULL, *relation = NULL, *asset = NULL;
    if (!source) { code = error ? 500 : 404; message = error ? "Impossibile leggere il bene." : "Bene non trovato."; goto fail; }
    if (jint(source,"version") != version) { code = 409; message = "Il bene è stato modificato: aggiorna i dati prima di salvare."; goto fail; }
    if (remove_id) {
        relation = load_relation(source_id,remove_id,&error);
        json_object *active = NULL;
        if (!relation) { code = error ? 500 : 404; message = "Relazione non trovata."; goto fail; }
        if (!json_object_object_get_ex(relation,"active",&active) || !json_object_get_boolean(active)) {
            code = 409; message = "La relazione è già stata rimossa."; goto fail;
        }
    } else {
        if (unavailable(source)) { code = 409; message = "Il bene di origine è dismesso o smarrito."; goto fail; }
        if (strcmp(relation_type,"installed_on") && strcmp(relation_type,"connected_to") && strcmp(relation_type,"licensed_to")) {
            code = 400; message = "Tipo di relazione non valido."; goto fail;
        }
        if (target_id) {
            target = load_asset(target_id,&error);
            if (!target) { code = error ? 500 : 404; message = "Bene di destinazione non trovato."; goto fail; }
            if (unavailable(target)) { code = 409; message = "Il bene di destinazione è dismesso o smarrito."; goto fail; }
        }
        bool valid = true;
        if (!strcmp(relation_type,"installed_on")) valid = !strcmp(jstr(source,"asset_type"),"software") && target && is_device(target) && quantity == 1 && !*assignee && !*source_port && !*target_port;
        else if (!strcmp(relation_type,"connected_to")) valid = is_device(source) && target && !strcmp(jstr(target,"asset_type"),"network_device") && source_id != target_id && quantity == 1 && !*assignee && !*installed_version;
        else {
            valid = !strcmp(jstr(source,"asset_type"),"license") && !*installed_version && !*source_port && !*target_port;
            if (!valid) { code = 400; message = "Relazione non compatibile con le tipologie dei beni."; goto fail; }
            json_object *details = NULL;
            json_object_object_get_ex(source,"details",&details);
            const char *model = jstr(details,"license_model");
            if (!strcmp(model,"per_device")) valid = target && is_device(target) && !*assignee;
            else if (!strcmp(model,"per_user")) valid = !target && *assignee;
            else if (!strcmp(model,"concurrent") || !strcmp(model,"site")) valid = (target && is_device(target) && !*assignee) || (!target && *assignee);
            else valid = false;
            if (unavailable(source) || license_expired(details)) { code = 409; message = "La licenza è dismessa, smarrita o scaduta."; goto fail; }
            sqlite3_int64 allocated = allocated_seats(source_id,&error);
            if (error) goto fail;
            if (quantity > jint(details,"seats_total") - allocated) { code = 409; message = "Posti disponibili insufficienti per l'assegnazione."; goto fail; }
        }
        if (!valid) { code = 400; message = "Relazione non compatibile con le tipologie dei beni."; goto fail; }
    }
    char timestamp[32];
    if (!timestamp_now(timestamp)) goto fail;
    sqlite3_stmt *s;
    if (remove_id) {
        s = prepare("UPDATE asset_relations SET active=0,removed_at=?,removed_actor_id=? WHERE id=? AND source_id=? AND active=1");
        if (s) { sqlite3_bind_text(s,1,timestamp,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,actor); sqlite3_bind_int64(s,3,remove_id); sqlite3_bind_int64(s,4,source_id); }
    } else {
        s = prepare("INSERT INTO asset_relations(source_id,target_id,relation_type,assignee,quantity,installed_version,source_port,target_port,notes,actor_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        if (s) {
            sqlite3_bind_int64(s,1,source_id);
            if (target_id) sqlite3_bind_int64(s,2,target_id); else sqlite3_bind_null(s,2);
            sqlite3_bind_text(s,3,relation_type,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,4,assignee,-1,SQLITE_TRANSIENT);
            sqlite3_bind_int64(s,5,quantity); sqlite3_bind_text(s,6,installed_version,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(s,7,source_port,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,8,target_port,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(s,9,notes,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,10,actor); sqlite3_bind_text(s,11,timestamp,-1,SQLITE_TRANSIENT);
        }
    }
    int result = s ? sqlite3_step(s) : SQLITE_ERROR;
    int extended = sqlite3_extended_errcode(database);
    sqlite3_finalize(s);
    if (result != SQLITE_DONE || sqlite3_changes(database) != 1) {
        if (extended == SQLITE_CONSTRAINT_UNIQUE) { code = 409; message = "Relazione attiva già presente."; }
        goto fail;
    }
    sqlite3_int64 relation_id = remove_id ? remove_id : sqlite3_last_insert_rowid(database);
    s = prepare("UPDATE assets SET version=version+1,actor_id=?,updated_at=? WHERE id=? AND version=?");
    bool ok = s && sqlite3_bind_int64(s,1,actor) == SQLITE_OK && sqlite3_bind_text(s,2,timestamp,-1,SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(s,3,source_id) == SQLITE_OK && sqlite3_bind_int64(s,4,version) == SQLITE_OK && sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database) == 1;
    sqlite3_finalize(s);
    if (!ok) goto fail;
    if (relation) json_object_put(relation);
    relation = load_relation(source_id,relation_id,&error);
    asset = load_asset(source_id,&error);
    if (!relation || !asset || error) goto fail;
    json_object *change = json_object_new_object();
    addstr(change,"operation",remove_id ? "relation_removed" : "relation_added");
    json_object_object_add(change,"relation",json_object_get(relation));
    ok = history_write(source_id,"updated",actor,version+1,timestamp,asset,change);
    json_object_put(change);
    if (ok) ok = execute("COMMIT");
    if (!ok) { message = "Storico relazione non salvato; modifica annullata."; goto fail; }
    json_object_put(source);
    if (target) json_object_put(target);
    json_object *o = ok_json();
    json_object_object_add(o,"asset",asset); json_object_object_add(o,"relation",relation);
    *status = remove_id ? 200 : 201;
    return o;
fail:
    (void)execute("ROLLBACK");
    if (source) json_object_put(source);
    if (target) json_object_put(target);
    if (relation) json_object_put(relation);
    if (asset) json_object_put(asset);
    return failure(status,code,message);
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
    if (strlen(json) > RESPONSE_LIMIT) {
        status = 500;
        json = "{\"ok\":false,\"error\":\"Risposta inventario troppo grande.\"}";
    }
    const char *reason = status == 200   ? "OK"
                         : status == 201 ? "Created"
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
    char buffer[HEADER_LIMIT + 1];
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
            int body_limit=!strcmp(request->path,"/v1/import/preview") ? BODY_LIMIT : 32768;
            if (have_length || !number(value, 0, (unsigned long)body_limit, &content_length))
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

#include "inventory_csv.inc"
#include "inventory_due.inc"

static json_object *route(Request *request, const char *address, int *status) {
    bool health = !strcmp(request->path, "/healthz") && !strcmp(request->method, "GET") && !request->length;
    if (health && (source_allowed(address) || !strcmp(address, config.bind_address) || !strcmp(address, "127.0.0.1"))) {
        json_object *o = ok_json();
        addstr(o, "status", "ok");
        return o;
    }
    if (!source_allowed(address)) return failure(status, 403, "Sorgente non autorizzata.");
    const char *auth = request->authorization;
    size_t length = strlen(config.token);
    if (strlen(auth) != length+7 || strncmp(auth, "Bearer ", 7) || CRYPTO_memcmp(auth+7, config.token, length))
        return failure(status, 401, "Autenticazione inventario richiesta.");
    char *query = strchr(request->path, '?');
    if (query) *query++ = 0;
    else query = request->path + strlen(request->path);
    if (strchr(request->path, '#') || strchr(query, '#')) return failure(status, 400, "Percorso non valido.");
    bool get = !strcmp(request->method, "GET"), post = !strcmp(request->method, "POST");
    if (!get && !post) return failure(status, 405, "Metodo non consentito; dismettere i beni tramite lo stato.");
    if (get && request->length) return failure(status, 400, "Corpo richiesta inatteso.");
    if (request->length>32768 && strcmp(request->path,"/v1/import/preview"))
        return failure(status,413,"Richiesta inventario troppo grande.");
    if (!strcmp(request->path, "/v1/status") && get && !*query) {
        json_object *totals = summary();
        if (!totals) return failure(status, 500, "Impossibile leggere il riepilogo inventario.");
        json_object *o = ok_json();
        json_object_object_add(o, "configured", json_object_new_boolean(true));
        json_object_object_add(o, "summary", totals);
        json_object_object_add(o, "backup", inventory_backup_status());
        return o;
    }
    if (!strcmp(request->path,"/v1/assets/export")) {
        if (!get) return failure(status,405,"L'esportazione richiede GET.");
        return csv_export(query,status);
    }
    if (!strcmp(request->path,"/v1/due")) {
        if (!get) return failure(status,405,"Le scadenze sono consultabili in sola lettura.");
        return list_due(query,status);
    }
    if (!strcmp(request->path,"/v1/import/preview") || !strcmp(request->path,"/v1/import/commit")) {
        if (!post) return failure(status,405,"L'importazione richiede POST.");
        if (*query) return failure(status,400,"Parametri inattesi per l'importazione.");
        if (strcasecmp(request->content_type,"application/x-www-form-urlencoded")) return failure(status,415,"Formato richiesto: application/x-www-form-urlencoded.");
        Form form;
        if (!form_parse(request->body,&form)) return failure(status,400,"Modulo non valido o campi duplicati.");
        json_object *o=!strcmp(request->path,"/v1/import/preview") ? csv_preview(&form,status) : csv_commit(&form,status);
        form_free(&form);
        return o;
    }
    sqlite3_int64 id = 0;
    bool history = false, relations = false;
    sqlite3_int64 remove_id = 0;
    if (!strcmp(request->path, "/v1/assets")) {
        if (get) return list_assets(query, status);
    } else if (!strncmp(request->path, "/v1/assets/", 11)) {
        char *identifier = request->path+11, *suffix = strchr(identifier, '/');
        if (suffix) {
            *suffix++ = 0;
            if (!strcmp(suffix,"history")) history = true;
            else if (!strcmp(suffix,"relations")) relations = true;
            else if (!strncmp(suffix,"relations/",10)) {
                char *relation_identifier = suffix+10, *remove = strchr(relation_identifier,'/');
                if (!remove || strcmp(remove,"/remove")) return failure(status,404,"Percorso inventario non trovato.");
                *remove = 0;
                if (!integer(relation_identifier,1,INT64_MAX,&remove_id)) return failure(status,400,"Identificativo relazione non valido.");
                relations = true;
            } else return failure(status,404,"Percorso inventario non trovato.");
        }
        if (!integer(identifier, 1, INT64_MAX, &id)) return failure(status, 400, "Identificativo del bene non valido.");
        if (relations && get) {
            if (remove_id) return failure(status,405,"La rimozione richiede POST.");
            return list_relations(id,query,status);
        }
        if (history && get) return asset_history(id, query, status);
        if (history) return failure(status, 405, "Lo storico è consultabile in sola lettura.");
        if (get) {
            if (*query) return failure(status, 400, "Parametri inattesi per il dettaglio del bene.");
            bool error = false;
            json_object *asset = load_asset(id, &error);
            if (!asset) return failure(status, error ? 500 : 404, error ? "Impossibile leggere il bene." : "Bene non trovato.");
            json_object *o = ok_json();
            json_object_object_add(o, "asset", asset);
            return o;
        }
    } else return failure(status, 404, "Percorso inventario non trovato.");
    if (*query) return failure(status, 400, "Parametri inattesi per il salvataggio.");
    if (strcasecmp(request->content_type, "application/x-www-form-urlencoded")) return failure(status, 415, "Formato richiesto: application/x-www-form-urlencoded.");
    Form form;
    if (!form_parse(request->body, &form)) return failure(status, 400, "Modulo non valido o campi duplicati.");
    json_object *o = relations ? save_relation(id,remove_id,&form,status) : save_asset(id, &form, status);
    form_free(&form);
    return o;
}
int main(void) {
    umask(0077);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    if (!load_config()) { fprintf(stderr, "Invalid inventory configuration\n"); return 1; }
    if (!init_database()) { fprintf(stderr, "Inventory store unavailable\n"); return 1; }
    inventory_backup_open(database);
    inventory_backup_tick(time(NULL));
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) ||
        SSL_CTX_use_certificate_chain_file(context, config.cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, config.key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1) {
        fprintf(stderr, "Inventory TLS unavailable\n");
        return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0), reuse = 1;
    struct sockaddr_in bind_address = {0};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)config.port);
    if (listener < 0 || inet_pton(AF_INET, config.bind_address, &bind_address.sin_addr) != 1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
        bind(listener, (struct sockaddr *)&bind_address, sizeof(bind_address)) || listen(listener, 32)) {
        fprintf(stderr, "Inventory listener unavailable\n");
        return 1;
    }
    fprintf(stderr, "Inventory API ready\n");
    while (!stopping) {
        inventory_backup_tick(time(NULL));
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        struct timeval wait = {1, 0};
        int ready = select(listener+1, &set, NULL, NULL, &wait);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!ready) continue;
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_size);
        if (fd < 0) continue;
        struct timeval timeout = {5, 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        SSL *tls = SSL_new(context);
        if (tls) {
            SSL_set_fd(tls, fd);
            if (!stopping && SSL_accept(tls) == 1) {
                char address[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
                Request *request = calloc(1, sizeof(*request));
                int status = request ? receive_request(tls, request) : 500;
                json_object *o = status ? error_json("Richiesta HTTP non valida o troppo grande.") : NULL;
                if (!status) { status = 200; o = route(request, address, &status); }
                respond(tls, status, o);
                json_object_put(o);
                if (request) { OPENSSL_cleanse(request, sizeof(*request)); free(request); }
            }
            SSL_free(tls);
        }
        close(fd);
    }
    close(listener);
    SSL_CTX_free(context);
    inventory_backup_close();
    sqlite3_close(database);
    OPENSSL_cleanse(&config, sizeof(config));
    return 0;
}
