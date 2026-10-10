#include "storage.h"
#ifdef PUNTO_MYSQL
#include "mysql_schema.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCHEMA_VERSION 3
#define JSON_LIMIT (4U * 1024U * 1024U)

static Db *database;

/* Refuse newer databases before any schema creation or bootstrap account write. */
bool storage_preflight(const char *db_path) {
#ifdef PUNTO_MYSQL
    (void)db_path;
    return mysql_schema_preflight() && mysql_schema_prepare();
#else
    if (!db_path || !*db_path) return false;
    if (access(db_path, F_OK) != 0) return errno == ENOENT;
    Db *check = NULL;
    DbStmt *statement = NULL;
    bool okay = db_open_v2(db_path, &check, DB_OPEN_READONLY | DB_OPEN_FULLMUTEX,
                               NULL) == DB_OK &&
        db_prepare_v2(check, "PRAGMA user_version", -1, &statement, NULL) == DB_OK &&
        db_step(statement) == DB_ROW;
    if (okay) {
        int version = db_column_int(statement, 0);
        okay = version >= 0 && version <= SCHEMA_VERSION;
    }
    db_finalize(statement);
    if (check) (void)db_close(check);
    return okay;
#endif
}

typedef struct { char *text; size_t used, capacity; bool failed; } Json;

static void append(Json *json, const char *text) {
    if (json->failed) return;
    size_t length = strlen(text);
    if (length > JSON_LIMIT || json->used > JSON_LIMIT - length) {
        json->failed = true; return;
    }
    size_t needed = json->used + length + 1;
    if (needed > json->capacity) {
        size_t capacity = json->capacity ? json->capacity : 256;
        while (capacity < needed) capacity *= 2;
        char *grown = realloc(json->text, capacity);
        if (!grown) { json->failed = true; return; }
        json->text = grown; json->capacity = capacity;
    }
    memcpy(json->text + json->used, text, length + 1);
    json->used += length;
}

static void integer(Json *json, DbInt value) {
    char text[32];
    (void)snprintf(text, sizeof(text), "%lld", (long long)value);
    append(json, text);
}

static void string(Json *json, const char *text) {
    append(json, "\"");
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        unsigned char c = *p++;
        if (c == '"') append(json, "\\\"");
        else if (c == '\\') append(json, "\\\\");
        else if (c < 32 || c == '<' || c == '>' || c == '&') {
            char escaped[7];
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            append(json, escaped);
        } else {
            char one[] = {(char)c, '\0'};
            append(json, one);
        }
    }
    append(json, "\"");
}

static void error(WorkspaceResponse *response, int status, const char *message) {
    Json json = {0};
    append(&json, "{\"ok\":false,\"error\":"); string(&json, message); append(&json, "}");
    response->status = json.failed ? 500 : status;
    if (json.failed) { free(json.text); response->body = NULL; }
    else response->body = json.text;
}

static bool prepare(Db *db, DbStmt **statement, const char *sql) {
    return db_prepare_v2(db, sql, -1, statement, NULL) == DB_OK;
}

static bool bind_text(DbStmt *statement, int index, const char *text) {
    return text ? db_bind_text(statement, index, text, -1, DB_TRANSIENT) == DB_OK :
        db_bind_null(statement, index) == DB_OK;
}

static const char *column(DbStmt *statement, int index) {
    const unsigned char *value = db_column_text(statement, index);
    return value ? (const char *)value : "";
}

bool storage_audit(Db *db, DbInt actor_id, const char *action,
                   const char *entity_type, DbInt entity_id,
                   const char *before_json, const char *after_json) {
    if (!db || !action || !entity_type || actor_id < 0 || entity_id < 0 ||
        db_get_autocommit(db)) return false;
    DbStmt *statement = NULL;
    bool okay = prepare(db, &statement,
#ifdef PUNTO_MYSQL
        "INSERT INTO audit_events(created_at,actor_id,actor_name,actor_email,action,entity_type,entity_id,before_json,after_json) "
        "SELECT DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ'),?,COALESCE"
#else
        "INSERT INTO audit_events(actor_id,actor_name,actor_email,action,entity_type,entity_id,before_json,after_json) "
        "SELECT ?,COALESCE"
#endif
        "((SELECT name FROM users WHERE id=?),'Sistema'),"
        "COALESCE((SELECT email FROM users WHERE id=?),''),?,?,?,?,?;");
    if (okay) okay = (actor_id ? db_bind_int64(statement, 1, actor_id) :
                                db_bind_null(statement, 1)) == DB_OK &&
        db_bind_int64(statement, 2, actor_id) == DB_OK &&
        db_bind_int64(statement, 3, actor_id) == DB_OK &&
        bind_text(statement, 4, action) && bind_text(statement, 5, entity_type) &&
        db_bind_int64(statement, 6, entity_id) == DB_OK &&
        bind_text(statement, 7, before_json) && bind_text(statement, 8, after_json) &&
        db_step(statement) == DB_DONE;
    db_finalize(statement);
    return okay;
}

#ifndef PUNTO_MYSQL
static bool migrate(void) {
    DbStmt *statement = NULL;
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != DB_OK) return false;
    if (!prepare(database, &statement, "PRAGMA user_version") ||
        db_step(statement) != DB_ROW) goto failure;
    int version = db_column_int(statement, 0);
    db_finalize(statement); statement = NULL;
    if (version < 0 || version > SCHEMA_VERSION) goto failure;
    if (version == 0) {
        bool has_user_id = false;
        if (!prepare(database, &statement, "PRAGMA table_info(time_entries)")) goto failure;
        int result;
        while ((result = db_step(statement)) == DB_ROW) {
            if (strcmp(column(statement, 1), "user_id") == 0) has_user_id = true;
        }
        db_finalize(statement); statement = NULL;
        if (result != DB_DONE) goto failure;
        if (!has_user_id && db_exec(database,
            "ALTER TABLE time_entries ADD COLUMN user_id INTEGER REFERENCES users(id);",
            NULL, NULL, NULL) != DB_OK) goto failure;
        if (db_exec(database,
            "UPDATE time_entries SET user_id=(SELECT u.id FROM users u "
            "WHERE u.email=time_entries.owner COLLATE NOCASE) WHERE user_id IS NULL;"
            "CREATE INDEX IF NOT EXISTS time_entries_user_date ON time_entries(user_id,date);"
            "CREATE INDEX IF NOT EXISTS time_entries_job_user ON time_entries(job_id,user_id);"
            "CREATE TRIGGER IF NOT EXISTS time_entries_require_user_insert "
            "BEFORE INSERT ON time_entries WHEN NEW.user_id IS NULL "
            "BEGIN SELECT RAISE(ABORT,'user_id is required'); END;"
            "CREATE TRIGGER IF NOT EXISTS time_entries_preserve_user_update "
            "BEFORE UPDATE OF user_id ON time_entries WHEN OLD.user_id IS NOT NULL AND "
            "(NEW.user_id IS NULL OR NEW.user_id<>OLD.user_id) "
            "BEGIN SELECT RAISE(ABORT,'user_id is immutable'); END;"
            "CREATE TABLE IF NOT EXISTS job_assignments ("
            "job_id INTEGER NOT NULL REFERENCES jobs(id),"
            "user_id INTEGER NOT NULL REFERENCES users(id),"
            "starts_on TEXT,ends_on TEXT,"
            "PRIMARY KEY(job_id,user_id),"
            "CHECK(starts_on IS NULL OR length(starts_on)=10),"
            "CHECK(ends_on IS NULL OR length(ends_on)=10),"
            "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on)"
            ");"
            "CREATE INDEX IF NOT EXISTS job_assignments_user ON job_assignments(user_id,job_id);"
            "CREATE TABLE IF NOT EXISTS audit_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
            "actor_id INTEGER REFERENCES users(id),actor_name TEXT NOT NULL,actor_email TEXT NOT NULL,"
            "action TEXT NOT NULL,entity_type TEXT NOT NULL,entity_id INTEGER NOT NULL,"
            "before_json TEXT,after_json TEXT"
            ");"
            "CREATE INDEX IF NOT EXISTS audit_events_entity ON audit_events(entity_type,id);"
            "CREATE TRIGGER IF NOT EXISTS audit_events_no_update BEFORE UPDATE ON audit_events "
            "BEGIN SELECT RAISE(ABORT,'audit events are immutable'); END;"
            "CREATE TRIGGER IF NOT EXISTS audit_events_no_delete BEFORE DELETE ON audit_events "
            "BEGIN SELECT RAISE(ABORT,'audit events are immutable'); END;",
            NULL, NULL, NULL) != DB_OK) goto failure;
        if (!prepare(database, &statement,
            "SELECT COUNT(user_id),COUNT(*)-COUNT(user_id) FROM time_entries") ||
            db_step(statement) != DB_ROW) goto failure;
        char details[192];
        (void)snprintf(details, sizeof(details),
            "{\"schema_version\":1,\"mapped_entries\":%lld,\"archived_entries\":%lld}",
            (long long)db_column_int64(statement, 0),
            (long long)db_column_int64(statement, 1));
        db_finalize(statement); statement = NULL;
        if (!storage_audit(database, 0, "system.migration", "system", 1, NULL, details) ||
            db_exec(database, "PRAGMA user_version=1", NULL, NULL, NULL) != DB_OK)
            goto failure;
    }
    if (version <= 1) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS suppliers ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "code TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(code) BETWEEN 1 AND 40),"
            "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120),"
            "vat_number TEXT NOT NULL DEFAULT '' CHECK(length(vat_number)<=32),"
            "email TEXT NOT NULL DEFAULT '' CHECK(length(email)<=254),"
            "phone TEXT NOT NULL DEFAULT '' CHECK(length(phone)<=40),"
            "address TEXT NOT NULL DEFAULT '' CHECK(length(address)<=240),"
            "notes TEXT NOT NULL DEFAULT '' CHECK(length(notes)<=1000),"
            "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1))"
            ");"
            "CREATE TABLE IF NOT EXISTS contracts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "code TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(code) BETWEEN 1 AND 40),"
            "title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 120),"
            "supplier_id INTEGER NOT NULL REFERENCES suppliers(id),"
            "status TEXT NOT NULL DEFAULT 'draft' CHECK(status IN ('draft','active','expired','closed','cancelled')),"
            "starts_on TEXT,ends_on TEXT,"
            "value_cents INTEGER CHECK(value_cents IS NULL OR value_cents BETWEEN 0 AND 999999999999),"
            "currency TEXT NOT NULL DEFAULT 'EUR' CHECK(currency='EUR'),"
            "notes TEXT NOT NULL DEFAULT '' CHECK(length(notes)<=1000),"
            "CHECK(starts_on IS NULL OR length(starts_on)=10),"
            "CHECK(ends_on IS NULL OR length(ends_on)=10),"
            "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on)"
            ");"
            "CREATE INDEX IF NOT EXISTS contracts_supplier ON contracts(supplier_id,id);"
            "CREATE TABLE IF NOT EXISTS contract_documents ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,contract_id INTEGER NOT NULL REFERENCES contracts(id),"
            "storage_key TEXT NOT NULL UNIQUE CHECK(length(storage_key)=64),"
            "file_name TEXT NOT NULL CHECK(length(file_name) BETWEEN 1 AND 180),"
            "byte_size INTEGER NOT NULL CHECK(byte_size BETWEEN 5 AND 10485760),"
            "sha256 TEXT NOT NULL CHECK(length(sha256)=64),"
            "uploaded_by INTEGER NOT NULL REFERENCES users(id),"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))"
            ");"
            "CREATE INDEX IF NOT EXISTS contract_documents_contract ON contract_documents(contract_id,id);"
            "CREATE TRIGGER IF NOT EXISTS contract_documents_no_update BEFORE UPDATE ON contract_documents "
            "BEGIN SELECT RAISE(ABORT,'contract documents are immutable'); END;"
            "CREATE TRIGGER IF NOT EXISTS contract_documents_no_delete BEFORE DELETE ON contract_documents "
            "BEGIN SELECT RAISE(ABORT,'contract documents are immutable'); END;",
            NULL,NULL,NULL) != DB_OK) goto failure;
        bool has_contract_id = false;
        if (!prepare(database,&statement,"PRAGMA table_info(jobs)")) goto failure;
        int result;
        while ((result = db_step(statement)) == DB_ROW)
            if (strcmp(column(statement,1),"contract_id") == 0) has_contract_id = true;
        db_finalize(statement); statement = NULL;
        if (result != DB_DONE || (!has_contract_id && db_exec(database,
            "ALTER TABLE jobs ADD COLUMN contract_id INTEGER REFERENCES contracts(id)",NULL,NULL,NULL) != DB_OK) ||
            db_exec(database,"CREATE INDEX IF NOT EXISTS jobs_contract ON jobs(contract_id,id)",NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",2,NULL,"{\"schema_version\":2}") ||
            db_exec(database,"PRAGMA user_version=2",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 2) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS job_roles ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "code TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(code) BETWEEN 1 AND 40),"
            "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120),"
            "can_log_time INTEGER NOT NULL CHECK(can_log_time IN (0,1)),"
            "builtin INTEGER NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))"
            ");"
            "INSERT OR IGNORE INTO job_roles(id,code,name,can_log_time,builtin) VALUES(1,'reader','Lettore',0,1);"
            "INSERT OR IGNORE INTO job_roles(id,code,name,can_log_time,builtin) VALUES(2,'contributor','Collaboratore',1,1);"
            "CREATE TABLE IF NOT EXISTS organization_groups ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 120),"
            "parent_id INTEGER REFERENCES organization_groups(id),CHECK(parent_id IS NULL OR parent_id<>id)"
            ");"
            "CREATE INDEX IF NOT EXISTS organization_groups_parent ON organization_groups(parent_id,id);"
            "CREATE TABLE IF NOT EXISTS group_members ("
            "group_id INTEGER NOT NULL REFERENCES organization_groups(id),"
            "user_id INTEGER NOT NULL REFERENCES users(id),PRIMARY KEY(group_id,user_id)"
            ");"
            "CREATE INDEX IF NOT EXISTS group_members_user ON group_members(user_id,group_id);"
            "CREATE TABLE IF NOT EXISTS job_group_grants ("
            "job_id INTEGER NOT NULL REFERENCES jobs(id),"
            "group_id INTEGER NOT NULL REFERENCES organization_groups(id),"
            "role_id INTEGER NOT NULL REFERENCES job_roles(id),starts_on TEXT,ends_on TEXT,"
            "PRIMARY KEY(job_id,group_id),"
            "CHECK(starts_on IS NULL OR length(starts_on)=10),"
            "CHECK(ends_on IS NULL OR length(ends_on)=10),"
            "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on)"
            ");"
            "CREATE INDEX IF NOT EXISTS job_group_grants_group ON job_group_grants(group_id,job_id);",
            NULL,NULL,NULL) != DB_OK) goto failure;
        bool has_role_id = false;
        if (!prepare(database,&statement,"PRAGMA table_info(job_assignments)")) goto failure;
        int result;
        while ((result = db_step(statement)) == DB_ROW)
            if (strcmp(column(statement,1),"role_id") == 0) has_role_id = true;
        db_finalize(statement); statement = NULL;
        if (result != DB_DONE || (!has_role_id && db_exec(database,
            "ALTER TABLE job_assignments ADD COLUMN role_id INTEGER REFERENCES job_roles(id)",NULL,NULL,NULL) != DB_OK) ||
            db_exec(database,"UPDATE job_assignments SET role_id=2 WHERE role_id IS NULL",NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",3,NULL,"{\"schema_version\":3}") ||
            db_exec(database,"PRAGMA user_version=3",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
failure:
    db_finalize(statement);
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
    return false;
}

#else
static bool migrate(void) {
    DbStmt *statement = NULL;
    if (db_exec(database, "START TRANSACTION", NULL, NULL, NULL) != DB_OK) return false;
    if (!prepare(database, &statement, "SELECT version FROM schema_metadata WHERE id=1 FOR UPDATE") ||
        db_step(statement) != DB_ROW) goto failure;
    int version = db_column_int(statement, 0);
    db_finalize(statement); statement = NULL;
    if (version < 0 || version > SCHEMA_VERSION) goto failure;
    if (version == 0 &&
        (!storage_audit(database, 0, "system.migration", "system", 1, NULL,
                       "{\"schema_version\":1,\"mapped_entries\":0,\"archived_entries\":0}") ||
         db_exec(database, "UPDATE schema_metadata SET version=1 WHERE id=1", NULL, NULL, NULL) != DB_OK))
        goto failure;
    if (version <= 1 &&
        (!storage_audit(database,0,"system.migration","system",2,NULL,"{\"schema_version\":2}") ||
         db_exec(database,"UPDATE schema_metadata SET version=2 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 2 &&
        (db_exec(database,"INSERT IGNORE INTO job_roles(id,code,name,can_log_time,builtin) "
                          "VALUES(1,'reader','Lettore',0,1),(2,'contributor','Collaboratore',1,1)",
                 NULL,NULL,NULL) != DB_OK ||
         db_exec(database,"UPDATE job_assignments SET role_id=2 WHERE role_id IS NULL",NULL,NULL,NULL) != DB_OK ||
         !storage_audit(database,0,"system.migration","system",3,NULL,"{\"schema_version\":3}") ||
         db_exec(database,"UPDATE schema_metadata SET version=3 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
failure:
    db_finalize(statement);
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
    return false;
}
#endif

bool storage_open(const char *db_path) {
    storage_close();
    if (!db_path || !*db_path ||
        db_open_v2(db_path, &database, DB_OPEN_READWRITE | DB_OPEN_FULLMUTEX,
                        NULL) != DB_OK || db_busy_timeout(database, 5000) != DB_OK ||
#ifndef PUNTO_MYSQL
        db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != DB_OK ||
#endif
        !migrate()) {
        storage_close(); return false;
    }
    return true;
}

bool storage_health(void) {
    DbStmt *statement = NULL;
    bool okay = database && prepare(database, &statement, "SELECT 1") &&
        db_step(statement) == DB_ROW && db_column_int(statement, 0) == 1;
    db_finalize(statement);
    return okay;
}

void storage_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
}

static bool positive(const char *text, DbInt *value) {
    if (!text || !*text) return false;
    DbInt number = 0;
    while (*text) {
        if (*text < '0' || *text > '9') return false;
        int digit = *text++ - '0';
        if (number > (INT64_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    if (!number) return false;
    *value = number; return true;
}

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode(char *output, size_t capacity, const char *input, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (length - i < 3) return false;
            int high = hex((unsigned char)input[i + 1]), low = hex((unsigned char)input[i + 2]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)(high * 16 + low); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c >= 127 || used + 1 >= capacity) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0'; return true;
}

static bool query_fields(const char *query, DbInt *limit,
                         DbInt *before_id, char type[32]) {
    bool got_limit = false, got_before = false, got_type = false;
    const char *p = query ? query : "";
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (!equal) return false;
        char name[32], value[64];
        if (!decode(name, sizeof(name), p, (size_t)(equal - p)) ||
            !decode(value, sizeof(value), equal + 1, (size_t)(end - equal - 1))) return false;
        if (strcmp(name, "limit") == 0) {
            if (got_limit || !positive(value, limit) || *limit > 100) return false;
            got_limit = true;
        } else if (strcmp(name, "before_id") == 0) {
            if (got_before || !positive(value, before_id)) return false;
            got_before = true;
        } else if (strcmp(name, "entity_type") == 0) {
            if (got_type || (*value && strcmp(value, "job") != 0 && strcmp(value, "time_entry") != 0 &&
                strcmp(value, "assignment") != 0 && strcmp(value, "user") != 0 &&
                strcmp(value, "system") != 0 && strcmp(value, "supplier") != 0 &&
                strcmp(value, "contract") != 0 && strcmp(value, "contract_document") != 0 &&
                strcmp(value, "group") != 0 && strcmp(value, "group_member") != 0 &&
                strcmp(value, "job_role") != 0 && strcmp(value, "group_grant") != 0)) return false;
            strcpy(type, value); got_type = true;
        } else return false;
        if (!*end) break;
        p = end + 1;
        if (!*p) return false;
    }
    return true;
}

static void list_audit(const char *query, WorkspaceResponse *response) {
    DbInt limit = 30, before_id = 0;
    char type[32] = "";
    if (!query_fields(query, &limit, &before_id, type)) {
        error(response, 400, "Filtri non validi: usa limite da 1 a 100, cursore e tipo di modifica validi."); return;
    }
    DbStmt *statement = NULL;
    if (!prepare(database, &statement,
        "SELECT id,created_at,actor_id,actor_name,actor_email,action,entity_type,entity_id,before_json,after_json "
        "FROM audit_events WHERE (?=0 OR id<?) AND (?='' OR entity_type=?) ORDER BY id DESC LIMIT ?") ||
        db_bind_int64(statement, 1, before_id) != DB_OK ||
        db_bind_int64(statement, 2, before_id) != DB_OK ||
        !bind_text(statement, 3, type) || !bind_text(statement, 4, type) ||
        db_bind_int64(statement, 5, limit + 1) != DB_OK) {
        db_finalize(statement); error(response, 500, "Impossibile leggere lo storico. Riprova."); return;
    }
    Json json = {0};
    append(&json, "{\"events\":[");
    DbInt count = 0, last_id = 0;
    int result;
    bool more = false;
    while ((result = db_step(statement)) == DB_ROW) {
        if (count == limit) { more = true; break; }
        if (count++) append(&json, ",");
        last_id = db_column_int64(statement, 0);
        append(&json, "{\"id\":"); integer(&json, last_id);
        append(&json, ",\"created_at\":"); string(&json, column(statement, 1));
        append(&json, ",\"actor_id\":");
        if (db_column_type(statement, 2) == DB_NULL) append(&json, "null");
        else integer(&json, db_column_int64(statement, 2));
        append(&json, ",\"actor_name\":"); string(&json, column(statement, 3));
        append(&json, ",\"actor_email\":"); string(&json, column(statement, 4));
        append(&json, ",\"action\":"); string(&json, column(statement, 5));
        append(&json, ",\"entity_type\":"); string(&json, column(statement, 6));
        append(&json, ",\"entity_id\":"); integer(&json, db_column_int64(statement, 7));
        append(&json, ",\"before\":");
        append(&json, db_column_type(statement, 8) == DB_NULL ? "null" : column(statement, 8));
        append(&json, ",\"after\":");
        append(&json, db_column_type(statement, 9) == DB_NULL ? "null" : column(statement, 9));
        append(&json, "}");
    }
    db_finalize(statement);
    if ((!more && result != DB_DONE) || json.failed) {
        free(json.text); error(response, 500, "Impossibile leggere lo storico. Riprova."); return;
    }
    append(&json, "],\"next_before_id\":");
    if (more) integer(&json, last_id); else append(&json, "null");
    append(&json, "}");
    if (json.failed) { free(json.text); error(response, 500, "Impossibile leggere lo storico. Riprova."); }
    else { response->status = 200; response->body = json.text; }
}

bool storage_handle(const char *method, const char *target, const char *query,
                    const char *role, WorkspaceResponse *response) {
    if (!method || !target || !response || strcmp(target, "/api/audit") != 0) return false;
    response->status = 0; response->body = NULL;
    if (!role || strcmp(role, "manager") != 0)
        error(response, 403, "Solo i gestori possono consultare lo storico delle modifiche.");
    else if (strcmp(method, "GET") != 0) error(response, 405, "Metodo non consentito.");
    else if (!database) error(response, 500, "Impossibile leggere lo storico. Riprova.");
    else list_audit(query, response);
    return true;
}
