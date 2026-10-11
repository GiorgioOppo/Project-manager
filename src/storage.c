#include "storage.h"
#include "access_control.h"
#include <json-c/json.h>
#ifdef PUNTO_MYSQL
#include "mysql_schema.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCHEMA_VERSION 13
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
static bool ensure_sqlite_column(const char *table, const char *name, const char *ddl) {
    char sql[128];
    int length = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    DbStmt *statement = NULL;
    if (length <= 0 || (size_t)length >= sizeof(sql) || !prepare(database, &statement, sql)) return false;
    int result;
    bool exists = false;
    while ((result = db_step(statement)) == DB_ROW) {
        if (strcmp(column(statement, 1), name) == 0) exists = true;
    }
    db_finalize(statement);
    return result == DB_DONE && (exists || db_exec(database, ddl, NULL, NULL, NULL) == DB_OK);
}

static bool migrate(void) {
    DbStmt *statement = NULL;
    /* Rebuild the referenced alert-rule table while preserving child rows.
       Foreign keys are checked before commit and restored on every exit. */
    if (db_exec(database, "PRAGMA foreign_keys=OFF", NULL, NULL, NULL) != DB_OK ||
        db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != DB_OK) {
        (void)db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
        return false;
    }
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
    if (version <= 3) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS labor_rates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL REFERENCES users(id),"
            "hourly_cents INTEGER NOT NULL CHECK(typeof(hourly_cents)='integer' AND hourly_cents BETWEEN 0 AND 99999999),"
            "currency TEXT NOT NULL DEFAULT 'EUR' CHECK(currency='EUR'),"
            "starts_on TEXT NOT NULL CHECK(length(starts_on)=10),"
            "ends_on TEXT CHECK(ends_on IS NULL OR length(ends_on)=10),"
            "CHECK(ends_on IS NULL OR starts_on<=ends_on),"
            "UNIQUE(user_id,starts_on)"
            ");"
            "CREATE INDEX IF NOT EXISTS labor_rates_user_dates ON labor_rates(user_id,starts_on,ends_on,id);"
            "CREATE TRIGGER IF NOT EXISTS labor_rates_preserve_user_update "
            "BEFORE UPDATE OF user_id ON labor_rates WHEN NEW.user_id<>OLD.user_id "
            "BEGIN SELECT RAISE(ABORT,'rate user_id is immutable'); END;",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",4,NULL,"{\"schema_version\":4}") ||
            db_exec(database,"PRAGMA user_version=4",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 4) {
        bool has_common = false;
        if (!prepare(database,&statement,"PRAGMA table_info(jobs)")) goto failure;
        int result;
        while ((result = db_step(statement)) == DB_ROW) {
            if (strcmp(column(statement,1),"is_common") == 0) has_common = true;
        }
        db_finalize(statement); statement = NULL;
        if (result != DB_DONE || (!has_common && db_exec(database,
            "ALTER TABLE jobs ADD COLUMN is_common INTEGER NOT NULL DEFAULT 0 CHECK(is_common IN (0,1))",
            NULL,NULL,NULL) != DB_OK) ||
            !storage_audit(database,0,"system.migration","system",5,NULL,"{\"schema_version\":5}") ||
            db_exec(database,"PRAGMA user_version=5",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 5) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS alert_rules (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "owner_id INTEGER NOT NULL REFERENCES users(id),name TEXT NOT NULL,"
            "type TEXT NOT NULL CHECK(type IN ('invalid_job','missing_hours')),"
            "run_time TEXT NOT NULL CHECK(length(run_time)=5),enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),"
            "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,last_run_at TEXT,last_run_on TEXT,"
            "last_event_id INTEGER NOT NULL DEFAULT 0 CHECK(last_event_id>=0));"
            "CREATE INDEX IF NOT EXISTS alert_rules_owner ON alert_rules(owner_id,id);"
            "CREATE TABLE IF NOT EXISTS alert_rule_users (rule_id INTEGER NOT NULL REFERENCES alert_rules(id),"
            "user_id INTEGER NOT NULL REFERENCES users(id),PRIMARY KEY(rule_id,user_id));"
            "CREATE TABLE IF NOT EXISTS alert_rule_emails (rule_id INTEGER NOT NULL REFERENCES alert_rules(id),"
            "email TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(rule_id,email));"
            "CREATE TABLE IF NOT EXISTS alert_subjects (rule_id INTEGER NOT NULL REFERENCES alert_rules(id),"
            "user_id INTEGER NOT NULL REFERENCES users(id),starts_on TEXT NOT NULL CHECK(length(starts_on)=10),"
            "last_checked_on TEXT CHECK(last_checked_on IS NULL OR length(last_checked_on)=10),PRIMARY KEY(rule_id,"
            "user_id));"
            "CREATE TABLE IF NOT EXISTS alert_events (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL REFERENCES users(id),job_id INTEGER NOT NULL,"
            "work_date TEXT NOT NULL CHECK(length(work_date)=10),minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),"
            "created_at TEXT NOT NULL);"
            "CREATE INDEX IF NOT EXISTS alert_events_created ON alert_events(created_at,id);"
            "CREATE TABLE IF NOT EXISTS alert_notifications (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "rule_id INTEGER NOT NULL REFERENCES alert_rules(id),recipient_id INTEGER NOT NULL REFERENCES users(id),"
            "subject_user_id INTEGER NOT NULL REFERENCES users(id),event_key TEXT NOT NULL,title TEXT NOT NULL,"
            "message TEXT NOT NULL,work_date TEXT,recorded_minutes INTEGER,created_at TEXT NOT NULL,read_at TEXT,"
            "UNIQUE(rule_id,recipient_id,event_key));"
            "CREATE INDEX IF NOT EXISTS alert_notifications_recipient ON alert_notifications(recipient_id,id);"
            "CREATE INDEX IF NOT EXISTS alert_notifications_unread ON alert_notifications(recipient_id,read_at,id);"
            "CREATE TABLE IF NOT EXISTS alert_email_outbox (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "rule_id INTEGER NOT NULL REFERENCES alert_rules(id),email TEXT NOT NULL COLLATE NOCASE,"
            "subject_user_id INTEGER NOT NULL REFERENCES users(id),event_key TEXT NOT NULL,title TEXT NOT NULL,"
            "message TEXT NOT NULL,created_at TEXT NOT NULL,"
            "status TEXT NOT NULL DEFAULT 'pending_configuration' CHECK(status='pending_configuration'),UNIQUE(rule_id,"
            "email,event_key));"
            ,NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",6,NULL,"{\"schema_version\":6}") ||
            db_exec(database,"PRAGMA user_version=6",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 6) {
        if (db_exec(database,
            "CREATE TABLE alert_email_outbox_v7 (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "rule_id INTEGER NOT NULL REFERENCES alert_rules(id),email TEXT NOT NULL COLLATE NOCASE,"
            "subject_user_id INTEGER NOT NULL REFERENCES users(id),event_key TEXT NOT NULL,title TEXT NOT NULL,"
            "message TEXT NOT NULL,created_at TEXT NOT NULL,"
            "status TEXT NOT NULL DEFAULT 'pending_configuration' CHECK(status IN "
            "('pending_configuration','retrying','sent','delivery_unknown','delivery_failed')),"
            "UNIQUE(rule_id,email,event_key));"
            "INSERT INTO alert_email_outbox_v7(id,rule_id,email,subject_user_id,event_key,title,message,created_at,status) "
            "SELECT id,rule_id,email,subject_user_id,event_key,title,message,created_at,status FROM alert_email_outbox;"
            /* Keep deleted IDs consumed too: delivery idempotency relies on IDs
               never being reused within the same application namespace. */
            "UPDATE sqlite_sequence SET seq=MAX(seq,COALESCE((SELECT seq FROM sqlite_sequence "
            "WHERE name='alert_email_outbox'),0)) WHERE name='alert_email_outbox_v7';"
            "INSERT INTO sqlite_sequence(name,seq) SELECT 'alert_email_outbox_v7',COALESCE((SELECT seq "
            "FROM sqlite_sequence WHERE name='alert_email_outbox'),0) WHERE NOT EXISTS "
            "(SELECT 1 FROM sqlite_sequence WHERE name='alert_email_outbox_v7');"
            "DROP TABLE alert_email_outbox;"
            "ALTER TABLE alert_email_outbox_v7 RENAME TO alert_email_outbox;",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",7,NULL,"{\"schema_version\":7}") ||
            db_exec(database,"PRAGMA user_version=7",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 7) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS job_tasks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,job_id INTEGER NOT NULL REFERENCES jobs(id),"
            "parent_id INTEGER,assigned_user_id INTEGER REFERENCES users(id),"
            "title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 160),"
            "description TEXT NOT NULL DEFAULT '' CHECK(length(description)<=2000),"
            "status TEXT NOT NULL DEFAULT 'todo' CHECK(status IN ('todo','in_progress','blocked','in_review','done')),"
            "priority TEXT NOT NULL DEFAULT 'normal' CHECK(priority IN ('low','normal','high','urgent')),"
            "due_date TEXT CHECK(due_date IS NULL OR length(due_date)=10),"
            "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,"
            "UNIQUE(job_id,id),FOREIGN KEY(job_id,parent_id) REFERENCES job_tasks(job_id,id),"
            "CHECK(parent_id IS NULL OR parent_id<>id));"
            "CREATE INDEX IF NOT EXISTS job_tasks_job_status ON job_tasks(job_id,status,id);"
            "CREATE INDEX IF NOT EXISTS job_tasks_parent ON job_tasks(job_id,parent_id,id);"
            "CREATE INDEX IF NOT EXISTS job_tasks_assignee ON job_tasks(assigned_user_id,job_id,id);"
            "CREATE TABLE IF NOT EXISTS job_blockers ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,job_id INTEGER NOT NULL REFERENCES jobs(id),task_id INTEGER,"
            "title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 160),"
            "description TEXT NOT NULL DEFAULT '' CHECK(length(description)<=2000),"
            "impact TEXT NOT NULL DEFAULT 'medium' CHECK(impact IN ('low','medium','high','critical')),"
            "owner_id INTEGER REFERENCES users(id),status TEXT NOT NULL DEFAULT 'open' "
            "CHECK(status IN ('open','in_progress','resolved')),"
            "next_action TEXT NOT NULL DEFAULT '' CHECK(length(next_action)<=1000),"
            "due_date TEXT CHECK(due_date IS NULL OR length(due_date)=10),"
            "created_by INTEGER NOT NULL REFERENCES users(id),"
            "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,"
            "FOREIGN KEY(job_id,task_id) REFERENCES job_tasks(job_id,id));"
            "CREATE INDEX IF NOT EXISTS job_blockers_job_status ON job_blockers(job_id,status,id);"
            "CREATE INDEX IF NOT EXISTS job_blockers_task ON job_blockers(job_id,task_id,id);"
            "CREATE INDEX IF NOT EXISTS job_blockers_owner ON job_blockers(owner_id,job_id,id);",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",8,NULL,"{\"schema_version\":8}") ||
            db_exec(database,"PRAGMA user_version=8",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 8) {
        if (db_exec(database,
            "CREATE TABLE alert_rules_v9 (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "owner_id INTEGER NOT NULL REFERENCES users(id),name TEXT NOT NULL,"
            "type TEXT NOT NULL CHECK(type IN ('invalid_job','missing_hours','inventory_due')),"
            "run_time TEXT NOT NULL CHECK(length(run_time)=5),enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),"
            "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,last_run_at TEXT,last_run_on TEXT,"
            "last_event_id INTEGER NOT NULL DEFAULT 0 CHECK(last_event_id>=0),"
            "days_before INTEGER NOT NULL DEFAULT 30 CHECK(days_before BETWEEN 1 AND 365));"
            "INSERT INTO alert_rules_v9(id,owner_id,name,type,run_time,enabled,created_at,updated_at,last_run_at,last_run_on,last_event_id) "
            "SELECT id,owner_id,name,type,run_time,enabled,created_at,updated_at,last_run_at,last_run_on,last_event_id FROM alert_rules;"
            "UPDATE sqlite_sequence SET seq=MAX(seq,COALESCE((SELECT seq FROM sqlite_sequence WHERE name='alert_rules'),0)) "
            "WHERE name='alert_rules_v9';"
            "INSERT INTO sqlite_sequence(name,seq) SELECT 'alert_rules_v9',COALESCE((SELECT seq FROM sqlite_sequence "
            "WHERE name='alert_rules'),0) WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='alert_rules_v9');"
            "DROP TABLE alert_rules;ALTER TABLE alert_rules_v9 RENAME TO alert_rules;"
            "CREATE INDEX alert_rules_owner ON alert_rules(owner_id,id);",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",9,NULL,"{\"schema_version\":9}") ||
            db_exec(database,"PRAGMA user_version=9",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 9) {
        if (db_exec(database,
            "CREATE TABLE alert_rules_v10 (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "owner_id INTEGER NOT NULL REFERENCES users(id),name TEXT NOT NULL,"
            "type TEXT NOT NULL CHECK(type IN ('invalid_job','missing_hours','inventory_due','inventory_return','inventory_verification')),"
            "run_time TEXT NOT NULL CHECK(length(run_time)=5),enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),"
            "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,last_run_at TEXT,last_run_on TEXT,"
            "last_event_id INTEGER NOT NULL DEFAULT 0 CHECK(last_event_id>=0),"
            "days_before INTEGER NOT NULL DEFAULT 30 CHECK(days_before BETWEEN 1 AND 365),"
            "verification_days INTEGER NOT NULL DEFAULT 90 CHECK(verification_days BETWEEN 1 AND 365));"
            "INSERT INTO alert_rules_v10(id,owner_id,name,type,run_time,enabled,created_at,updated_at,last_run_at,last_run_on,last_event_id,days_before) "
            "SELECT id,owner_id,name,type,run_time,enabled,created_at,updated_at,last_run_at,last_run_on,last_event_id,days_before FROM alert_rules;"
            "UPDATE sqlite_sequence SET seq=MAX(seq,COALESCE((SELECT seq FROM sqlite_sequence WHERE name='alert_rules'),0)) "
            "WHERE name='alert_rules_v10';"
            "INSERT INTO sqlite_sequence(name,seq) SELECT 'alert_rules_v10',COALESCE((SELECT seq FROM sqlite_sequence "
            "WHERE name='alert_rules'),0) WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='alert_rules_v10');"
            "DROP TABLE alert_rules;ALTER TABLE alert_rules_v10 RENAME TO alert_rules;"
            "CREATE INDEX alert_rules_owner ON alert_rules(owner_id,id);",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",10,NULL,"{\"schema_version\":10}") ||
            db_exec(database,"PRAGMA user_version=10",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 11) {
        if (!ensure_sqlite_column("jobs", "time_closed", "ALTER TABLE jobs ADD COLUMN time_closed INTEGER NOT NULL DEFAULT 0 CHECK(time_closed IN (0,1))") ||
            !ensure_sqlite_column("jobs", "archived", "ALTER TABLE jobs ADD COLUMN archived INTEGER NOT NULL DEFAULT 0 CHECK(archived IN (0,1))") ||
            !ensure_sqlite_column("jobs", "lifecycle_revision", "ALTER TABLE jobs ADD COLUMN lifecycle_revision INTEGER NOT NULL DEFAULT 1 CHECK(lifecycle_revision>=1)") ||
            !ensure_sqlite_column("time_entries", "revision", "ALTER TABLE time_entries ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1)") ||
            !ensure_sqlite_column("time_entries", "task_id", "ALTER TABLE time_entries ADD COLUMN task_id INTEGER REFERENCES job_tasks(id) ON DELETE SET NULL") ||
            db_exec(database,
            "CREATE INDEX IF NOT EXISTS time_entries_task ON time_entries(task_id,date,id);"
            "CREATE TABLE IF NOT EXISTS time_entry_batches (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL REFERENCES users(id),request_key TEXT NOT NULL CHECK(length(request_key) BETWEEN 1 AND 128),"
            "payload_hash TEXT NOT NULL CHECK(length(payload_hash)=64),ids_json TEXT NOT NULL,"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),UNIQUE(user_id,request_key));"
            "CREATE TABLE IF NOT EXISTS time_corrections (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "entry_id INTEGER REFERENCES time_entries(id) ON DELETE SET NULL,original_entry_id INTEGER NOT NULL,"
            "requester_id INTEGER NOT NULL REFERENCES users(id),entry_revision INTEGER NOT NULL CHECK(entry_revision>=1),"
            "original_job_id INTEGER NOT NULL,original_date TEXT NOT NULL CHECK(length(original_date)=10),"
            "original_minutes INTEGER NOT NULL CHECK(original_minutes BETWEEN 1 AND 1440),original_note TEXT NOT NULL,"
            "before_json TEXT NOT NULL,job_id INTEGER NOT NULL,date TEXT NOT NULL CHECK(length(date)=10),"
            "minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),note TEXT NOT NULL,reason TEXT NOT NULL,"
            "delete_requested INTEGER NOT NULL DEFAULT 0 CHECK(delete_requested IN (0,1)),"
            "status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','rejected','cancelled','conflict')),"
            "decision_reason TEXT NOT NULL DEFAULT '',decided_by INTEGER REFERENCES users(id) ON DELETE SET NULL,"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),decided_at TEXT);"
            "CREATE INDEX IF NOT EXISTS time_corrections_requester ON time_corrections(requester_id,status,id);"
            "CREATE INDEX IF NOT EXISTS time_corrections_entry ON time_corrections(entry_id,status,id);"
            "CREATE TABLE IF NOT EXISTS user_work_schedules (id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER NOT NULL REFERENCES users(id),"
            "mon INTEGER NOT NULL DEFAULT 480 CHECK(mon BETWEEN 0 AND 1440),"
            "tue INTEGER NOT NULL DEFAULT 480 CHECK(tue BETWEEN 0 AND 1440),"
            "wed INTEGER NOT NULL DEFAULT 480 CHECK(wed BETWEEN 0 AND 1440),"
            "thu INTEGER NOT NULL DEFAULT 480 CHECK(thu BETWEEN 0 AND 1440),"
            "fri INTEGER NOT NULL DEFAULT 480 CHECK(fri BETWEEN 0 AND 1440),"
            "sat INTEGER NOT NULL DEFAULT 0 CHECK(sat BETWEEN 0 AND 1440),"
            "sun INTEGER NOT NULL DEFAULT 0 CHECK(sun BETWEEN 0 AND 1440),"
            "starts_on TEXT NOT NULL DEFAULT '0001-01-01' CHECK(length(starts_on)=10),UNIQUE(user_id,starts_on));"
            "CREATE TABLE IF NOT EXISTS user_work_exceptions (id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL REFERENCES users(id),date TEXT NOT NULL CHECK(length(date)=10),"
            "expected_minutes INTEGER NOT NULL CHECK(expected_minutes BETWEEN 0 AND 1440),reason TEXT NOT NULL DEFAULT '',"
            "UNIQUE(user_id,date));"
            "CREATE TABLE IF NOT EXISTS business_holidays (date TEXT PRIMARY KEY CHECK(length(date)=10),name TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS job_templates (id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "definition_json TEXT NOT NULL,created_by INTEGER NOT NULL REFERENCES users(id),"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
            "CREATE TABLE IF NOT EXISTS work_notifications (id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER NOT NULL REFERENCES users(id),"
            "job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,"
            "task_id INTEGER REFERENCES job_tasks(id) ON DELETE CASCADE,"
            "blocker_id INTEGER REFERENCES job_blockers(id) ON DELETE CASCADE,"
            "event_key TEXT NOT NULL CHECK(length(event_key) BETWEEN 1 AND 128),type TEXT NOT NULL CHECK(length(type) BETWEEN 1 AND 32),"
            "title TEXT NOT NULL,message TEXT NOT NULL,work_date TEXT,read_at TEXT,"
            "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),UNIQUE(user_id,event_key));"
            "CREATE INDEX IF NOT EXISTS work_notifications_recipient ON work_notifications(user_id,read_at,id);",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",12,NULL,"{\"schema_version\":12}") ||
            db_exec(database,"PRAGMA user_version=12",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (version <= 12) {
        if (db_exec(database,
            "CREATE TABLE IF NOT EXISTS access_groups (code TEXT PRIMARY KEY COLLATE BINARY,"
            "name TEXT NOT NULL,builtin INTEGER NOT NULL DEFAULT 1 CHECK(builtin=1),"
            "CHECK(code IN ('operator','manager','sales','finance')));"
            "CREATE TABLE IF NOT EXISTS access_group_members (group_code TEXT NOT NULL COLLATE BINARY "
            "REFERENCES access_groups(code),user_id INTEGER NOT NULL REFERENCES users(id),"
            "PRIMARY KEY(group_code,user_id),CHECK(group_code IN ('sales','finance')));"
            "CREATE INDEX IF NOT EXISTS access_group_members_user ON access_group_members(user_id,group_code);"
            "INSERT OR IGNORE INTO access_groups(code,name,builtin) VALUES"
            "('operator','Operatori',1),('manager','Gestori',1),('sales','Commerciali',1),('finance','Finance',1);"
            "CREATE TRIGGER IF NOT EXISTS access_groups_no_update BEFORE UPDATE ON access_groups "
            "BEGIN SELECT RAISE(ABORT,'functional groups are immutable'); END;"
            "CREATE TRIGGER IF NOT EXISTS access_groups_no_delete BEFORE DELETE ON access_groups "
            "BEGIN SELECT RAISE(ABORT,'functional groups are immutable'); END;",
            NULL,NULL,NULL) != DB_OK ||
            !storage_audit(database,0,"system.migration","system",13,NULL,"{\"schema_version\":13}") ||
            db_exec(database,"PRAGMA user_version=13",NULL,NULL,NULL) != DB_OK) goto failure;
    }
    if (db_exec(database,
        "CREATE TABLE IF NOT EXISTS oidc_groups ("
        "issuer TEXT NOT NULL COLLATE BINARY,subject TEXT NOT NULL COLLATE BINARY,"
        "group_id INTEGER NOT NULL UNIQUE REFERENCES organization_groups(id),"
        "active INTEGER NOT NULL CHECK(active IN (0,1)),PRIMARY KEY(issuer,subject));",
        NULL,NULL,NULL) != DB_OK) goto failure;
    if (!prepare(database, &statement, "PRAGMA foreign_key_check") || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) {
        return db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) == DB_OK;
    }
failure:
    db_finalize(statement);
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
    (void)db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
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
    if (version <= 3 &&
        (!storage_audit(database,0,"system.migration","system",4,NULL,"{\"schema_version\":4}") ||
         db_exec(database,"UPDATE schema_metadata SET version=4 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 4 &&
        (!storage_audit(database,0,"system.migration","system",5,NULL,"{\"schema_version\":5}") ||
         db_exec(database,"UPDATE schema_metadata SET version=5 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 5 &&
        (!storage_audit(database,0,"system.migration","system",6,NULL,"{\"schema_version\":6}") ||
         db_exec(database,"UPDATE schema_metadata SET version=6 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 6 &&
        (!storage_audit(database,0,"system.migration","system",7,NULL,"{\"schema_version\":7}") ||
         db_exec(database,"UPDATE schema_metadata SET version=7 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 7 &&
        (!storage_audit(database,0,"system.migration","system",8,NULL,"{\"schema_version\":8}") ||
         db_exec(database,"UPDATE schema_metadata SET version=8 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 8 &&
        (!storage_audit(database,0,"system.migration","system",9,NULL,"{\"schema_version\":9}") ||
         db_exec(database,"UPDATE schema_metadata SET version=9 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 9 &&
        (!storage_audit(database,0,"system.migration","system",10,NULL,"{\"schema_version\":10}") ||
         db_exec(database,"UPDATE schema_metadata SET version=10 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 11 &&
        (!storage_audit(database,0,"system.migration","system",12,NULL,"{\"schema_version\":12}") ||
         db_exec(database,"UPDATE schema_metadata SET version=12 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
    if (version <= 12 &&
        (db_exec(database,"INSERT IGNORE INTO access_groups(code,name,builtin) VALUES"
                          "('operator','Operatori',1),('manager','Gestori',1),"
                          "('sales','Commerciali',1),('finance','Finance',1)",NULL,NULL,NULL) != DB_OK ||
         !storage_audit(database,0,"system.migration","system",13,NULL,"{\"schema_version\":13}") ||
         db_exec(database,"UPDATE schema_metadata SET version=13 WHERE id=1",NULL,NULL,NULL) != DB_OK)) goto failure;
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
                strcmp(value, "job_role") != 0 && strcmp(value, "group_grant") != 0 &&
                strcmp(value, "labor_rate") != 0 && strcmp(value, "alert_rule") != 0 &&
                strcmp(value, "task") != 0 && strcmp(value, "blocker") != 0)) return false;
            strcpy(type, value); got_type = true;
        } else return false;
        if (!*end) break;
        p = end + 1;
        if (!*p) return false;
    }
    return true;
}

static DbInt audit_subject(const char *raw) {
    struct json_object *obj = raw && *raw ? json_tokener_parse(raw) : NULL, *value = NULL;
    DbInt subject = 0;
    if (obj && json_object_is_type(obj, json_type_object)) {
        if (json_object_object_get_ex(obj, "requester_id", &value) || json_object_object_get_ex(obj, "user_id", &value))
            subject = json_object_get_int64(value);
    }
    if (obj) json_object_put(obj);
    return subject;
}

/* Job audit snapshots remain complete on disk. The reading policy removes
   monetary/contract data and unscoped historical job totals from the API. */
static void audit_redact(struct json_object *obj, const AccessPolicy *policy, bool time_record, unsigned depth) {
    if (!obj || depth > 32) return;
    if (json_object_is_type(obj, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(obj); i++) audit_redact(json_object_array_get_idx(obj, i), policy, time_record, depth+1);
    } else if (json_object_is_type(obj, json_type_object)) {
        if (!policy->sales && !policy->finance) {
            const char *keys[] = {"contract_id","contract_code","contract_title","supplier_name","value_cents"};
            for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) json_object_object_del(obj,keys[i]);
        }
        if (!policy->finance) {
            const char *keys[] = {"hourly_cents","cost_cents","estimated_cost_cents","known_value_cents"};
            for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) json_object_object_del(obj,keys[i]);
        }
        if (!time_record) {
            const char *keys[] = {"minutes","total_minutes","time_entries","users_with_hours","deleted_minutes","deleted_time_entries"};
            for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) json_object_object_del(obj,keys[i]);
        }
        json_object_object_foreach(obj,key,value) { (void)key; audit_redact(value,policy,time_record,depth+1); }
    }
}

static void audit_snapshot(Json *json, const char *raw, const AccessPolicy *policy, bool time_record) {
    struct json_object *obj = raw && *raw ? json_tokener_parse(raw) : NULL;
    if (!obj) { append(json,"null"); return; }
    audit_redact(obj,policy,time_record,0);
    append(json,json_object_to_json_string_ext(obj,JSON_C_TO_STRING_PLAIN)); json_object_put(obj);
}

static void list_audit(const char *query, DbInt actor, const AccessPolicy *policy, WorkspaceResponse *response) {
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
    DbInt count = 0, scanned = 0, last_id = 0;
    int result;
    bool more = false;
    while ((result = db_step(statement)) == DB_ROW) {
        if (scanned++ == limit) { more = true; break; }
        last_id = db_column_int64(statement, 0);
        const char *entity = column(statement,6);
        bool time_record = !strcmp(entity,"time_entry") || !strcmp(entity,"time_correction");
        if ((!strcmp(entity,"contract") || !strcmp(entity,"supplier") || !strcmp(entity,"contract_document")) && !policy->sales && !policy->finance) continue;
        if (!strcmp(entity,"labor_rate") && !policy->finance) continue;
        if (time_record) {
            DbInt subject=audit_subject(column(statement,9));
            if (!subject) subject=audit_subject(column(statement,8));
            int visible=subject ? access_policy_can_view_time(database,actor,subject,policy->manager) : 0;
            if (visible < 0) { db_finalize(statement);free(json.text);error(response,500,"Impossibile leggere lo storico. Riprova.");return; }
            if (!visible) continue;
        }
        if (count++) append(&json, ",");
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
        audit_snapshot(&json,column(statement,8),policy,time_record);
        append(&json, ",\"after\":");
        audit_snapshot(&json,column(statement,9),policy,time_record);
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
                    DbInt actor_id, const char *role, WorkspaceResponse *response) {
    if (!method || !target || !response || strcmp(target, "/api/audit") != 0) return false;
    response->status = 0; response->body = NULL;
    if (!role || strcmp(role, "manager") != 0)
        error(response, 403, "Solo i gestori possono consultare lo storico delle modifiche.");
    else if (strcmp(method, "GET") != 0) error(response, 405, "Metodo non consentito.");
    else if (!database) error(response, 500, "Impossibile leggere lo storico. Riprova.");
    else {
        AccessPolicy policy;
        if (access_policy_resolve(database,actor_id,role,&policy)!=1 || !policy.manager)
            error(response,403,"Solo i gestori possono consultare lo storico delle modifiche.");
        else list_audit(query,actor_id,&policy,response);
    }
    return true;
}
