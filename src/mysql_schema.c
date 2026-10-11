#include "mysql_schema.h"

#ifdef PUNTO_MYSQL

#include "db.h"

#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MYSQL_SCHEMA_VERSION 13

static bool open_database(Db **database, int flags) {
    return db_open_v2("mysql", database, flags | DB_OPEN_FULLMUTEX, NULL) == DB_OK;
}

static bool metadata_supported(Db *database) {
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='schema_metadata'", -1,
        &statement, NULL) == DB_OK && db_step(statement) == DB_ROW;
    bool exists = okay && db_column_int64(statement, 0) == 1;
    if (okay && db_column_int64(statement, 0) > 1) okay = false;
    db_finalize(statement);
    if (!okay || !exists) return okay;

    statement = NULL;
    okay = db_prepare_v2(database, "SELECT id,version FROM schema_metadata", -1,
                         &statement, NULL) == DB_OK;
    if (okay) {
        int result = db_step(statement);
        /* An empty table can result from an interrupted initial installation. */
        if (result == DB_ROW) {
            DbInt version = db_column_int64(statement, 1);
            okay = db_column_int64(statement, 0) == 1 &&
                db_column_type(statement, 1) != DB_NULL &&
                version >= 0 && version <= MYSQL_SCHEMA_VERSION &&
                db_step(statement) == DB_DONE;
        } else okay = result == DB_DONE;
    }
    db_finalize(statement);
    return okay;
}

bool mysql_schema_preflight(void) {
    Db *database = NULL;
    bool okay = open_database(&database, DB_OPEN_READONLY) && metadata_supported(database);
    if (database) (void)db_close(database);
    return okay;
}

static bool ensure_trigger(Db *database, const char *name, const char *ddl) {
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.TRIGGERS "
        "WHERE TRIGGER_SCHEMA=DATABASE() AND TRIGGER_NAME=?", -1, &statement,
        NULL) == DB_OK && db_bind_text(statement, 1, name, -1, DB_STATIC) == DB_OK &&
        db_step(statement) == DB_ROW;
    bool exists = okay && db_column_int64(statement, 0) == 1;
    if (okay && db_column_int64(statement, 0) > 1) okay = false;
    db_finalize(statement);
    return okay && (exists || db_exec(database, ddl, NULL, NULL, NULL) == DB_OK);
}

static bool ensure_job_contract(Db *database) {
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='jobs' AND COLUMN_NAME='contract_id'",-1,&statement,NULL) == DB_OK &&
        db_step(statement) == DB_ROW;
    bool exists = okay && db_column_int64(statement,0) == 1;
    db_finalize(statement); statement = NULL;
    if (!okay) return false;
    if (!exists && db_exec(database,
        "ALTER TABLE jobs ADD COLUMN contract_id BIGINT NULL,"
        "ADD CONSTRAINT jobs_contract_fk FOREIGN KEY(contract_id) REFERENCES contracts(id),"
        "ADD KEY jobs_contract(contract_id,id)",NULL,NULL,NULL) != DB_OK) return false;
    return true;
}

static bool ensure_assignment_role(Db *database) {
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='job_assignments' AND COLUMN_NAME='role_id'", -1, &statement, NULL) == DB_OK &&
        db_step(statement) == DB_ROW;
    bool exists = okay && db_column_int64(statement, 0) == 1;
    db_finalize(statement);
    return okay && (exists || db_exec(database,
        "ALTER TABLE job_assignments ADD COLUMN role_id BIGINT NULL,"
        "ADD CONSTRAINT job_assignments_role_fk FOREIGN KEY(role_id) REFERENCES job_roles(id)",
        NULL, NULL, NULL) == DB_OK);
}

static bool ensure_job_flags(Db *database) {
    static const struct { const char *name; const char *ddl; } columns[] = {
        {"is_common", "ALTER TABLE jobs ADD COLUMN is_common TINYINT NOT NULL DEFAULT 0 CHECK(is_common IN (0,1))"},
        {"time_closed", "ALTER TABLE jobs ADD COLUMN time_closed TINYINT NOT NULL DEFAULT 0 CHECK(time_closed IN (0,1))"},
        {"archived", "ALTER TABLE jobs ADD COLUMN archived TINYINT NOT NULL DEFAULT 0 CHECK(archived IN (0,1))"},
        {"lifecycle_revision", "ALTER TABLE jobs ADD COLUMN lifecycle_revision BIGINT NOT NULL DEFAULT 1 CHECK(lifecycle_revision>=1)"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        DbStmt *statement = NULL;
        bool okay = db_prepare_v2(database,
            "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME='jobs' AND COLUMN_NAME=?", -1, &statement, NULL) == DB_OK &&
            db_bind_text(statement, 1, columns[i].name, -1, DB_STATIC) == DB_OK &&
            db_step(statement) == DB_ROW;
        bool exists = okay && db_column_int64(statement, 0) == 1;
        if (okay && db_column_int64(statement, 0) > 1) okay = false;
        db_finalize(statement);
        if (!okay || (!exists && db_exec(database, columns[i].ddl, NULL, NULL, NULL) != DB_OK))
            return false;
    }
    return true;
}

static bool ensure_time_entry_columns(Db *database) {
    static const struct { const char *name; const char *ddl; } columns[] = {
        {"revision", "ALTER TABLE time_entries ADD COLUMN revision BIGINT NOT NULL DEFAULT 1 CHECK(revision>=1)"},
        {"task_id", "ALTER TABLE time_entries ADD COLUMN task_id BIGINT NULL,"
         "ADD CONSTRAINT time_entries_task_fk FOREIGN KEY(task_id) REFERENCES job_tasks(id) ON DELETE SET NULL,"
         "ADD KEY time_entries_task(task_id,date,id)"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        DbStmt *statement = NULL;
        bool okay = db_prepare_v2(database,
            "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME='time_entries' AND COLUMN_NAME=?", -1, &statement, NULL) == DB_OK &&
            db_bind_text(statement, 1, columns[i].name, -1, DB_STATIC) == DB_OK &&
            db_step(statement) == DB_ROW;
        bool exists = okay && db_column_int64(statement, 0) == 1;
        if (okay && db_column_int64(statement, 0) > 1) okay = false;
        db_finalize(statement);
        if (!okay || (!exists && db_exec(database, columns[i].ddl, NULL, NULL, NULL) != DB_OK)) return false;
    }
    return true;
}

static bool ensure_email_statuses(Db *database) {
    DbStmt *statement = NULL;
    if (db_prepare_v2(database,
        "SELECT t.CONSTRAINT_NAME,c.CHECK_CLAUSE FROM information_schema.TABLE_CONSTRAINTS t "
        "JOIN information_schema.CHECK_CONSTRAINTS c ON c.CONSTRAINT_SCHEMA=t.CONSTRAINT_SCHEMA "
        "AND c.CONSTRAINT_NAME=t.CONSTRAINT_NAME WHERE t.CONSTRAINT_SCHEMA=DATABASE() "
        "AND t.TABLE_NAME='alert_email_outbox' AND t.CONSTRAINT_TYPE='CHECK'", -1,
        &statement,NULL) != DB_OK) return false;
    char name[65] = {0}, normalized[512] = {0};
    bool okay = db_step(statement) == DB_ROW;
    if (okay) {
        const char *identifier = (const char *)db_column_text(statement,0);
        const char *clause = (const char *)db_column_text(statement,1);
        size_t length = identifier ? strlen(identifier) : 0;
        okay = length > 0 && length < sizeof(name) && clause;
        for (size_t i=0; okay && i<length; i++)
            okay = isalnum((unsigned char)identifier[i]) || identifier[i]=='_';
        if (okay) {
            memcpy(name,identifier,length+1);
            size_t used=0;
            for (const char *p=clause; *p && okay; p++) {
                if (strncmp(p,"_utf8mb4",8)==0 || strncmp(p,"_utf8mb3",8)==0) { p+=7; continue; }
                /* MySQL's CHECK_CLAUSE can retain backslash-quoted literals. */
                if (*p=='\\' && p[1]=='\'') p++;
                if (isspace((unsigned char)*p) || *p=='`' || *p=='(' || *p==')') continue;
                if (used+1>=sizeof(normalized)) { okay=false; break; }
                normalized[used++]=(char)tolower((unsigned char)*p);
            }
            normalized[used]='\0';
        }
        if (okay) okay = db_step(statement) == DB_DONE;
    }
    db_finalize(statement);
    if (!okay) return false;
    static const char expanded[] = "statusin'pending_configuration','retrying','sent','delivery_unknown','delivery_failed'";
    if (strcmp(name,"alert_email_outbox_status")==0 && strcmp(normalized,expanded)==0) return true;
    /* Only replace the known legacy restriction, retaining all keys and rows.
       Unknown custom constraints are left intact and fail startup closed. */
    if (strcmp(normalized,"status='pending_configuration'")!=0) return false;
    char ddl[512];
    int count=snprintf(ddl,sizeof(ddl),"ALTER TABLE alert_email_outbox DROP CHECK `%s`,"
        "ADD CONSTRAINT alert_email_outbox_status CHECK(status IN "
        "('pending_configuration','retrying','sent','delivery_unknown','delivery_failed'))",name);
    return count>0 && (size_t)count<sizeof(ddl) && db_exec(database,ddl,NULL,NULL,NULL)==DB_OK;
}

static bool ensure_inventory_alerts(Db *database) {
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='alert_rules' AND COLUMN_NAME='days_before'", -1, &statement, NULL) == DB_OK &&
        db_step(statement) == DB_ROW;
    bool has_days = okay && db_column_int64(statement, 0) == 1;
    db_finalize(statement); statement = NULL;
    if (!okay || (!has_days && db_exec(database,
        "ALTER TABLE alert_rules ADD COLUMN days_before INT NOT NULL DEFAULT 30,"
        "ADD CONSTRAINT alert_rules_days_before CHECK(days_before BETWEEN 1 AND 365)",
        NULL,NULL,NULL) != DB_OK)) return false;
    okay = db_prepare_v2(database,
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='alert_rules' AND COLUMN_NAME='verification_days'", -1, &statement, NULL) == DB_OK &&
        db_step(statement) == DB_ROW;
    bool has_interval = okay && db_column_int64(statement, 0) == 1;
    db_finalize(statement); statement = NULL;
    if (!okay || (!has_interval && db_exec(database,
        "ALTER TABLE alert_rules ADD COLUMN verification_days INT NOT NULL DEFAULT 90,"
        "ADD CONSTRAINT alert_rules_verification_days CHECK(verification_days BETWEEN 1 AND 365)",
        NULL,NULL,NULL) != DB_OK)) return false;
    if (db_prepare_v2(database,
        "SELECT t.CONSTRAINT_NAME,c.CHECK_CLAUSE FROM information_schema.TABLE_CONSTRAINTS t "
        "JOIN information_schema.CHECK_CONSTRAINTS c ON c.CONSTRAINT_SCHEMA=t.CONSTRAINT_SCHEMA "
        "AND c.CONSTRAINT_NAME=t.CONSTRAINT_NAME WHERE t.CONSTRAINT_SCHEMA=DATABASE() "
        "AND t.TABLE_NAME='alert_rules' AND t.CONSTRAINT_TYPE='CHECK'", -1, &statement, NULL) != DB_OK) return false;
    char restriction[65] = {0};
    bool found = false, expanded = false;
    int result;
    while ((result = db_step(statement)) == DB_ROW) {
        const char *name = (const char *)db_column_text(statement,0);
        const char *clause = (const char *)db_column_text(statement,1);
        char normalized[512] = {0}; size_t used = 0;
        if (!name || !clause) { okay = false; break; }
        for (const char *p=clause; *p; p++) {
            if (!strncmp(p,"_utf8mb4",8) || !strncmp(p,"_utf8mb3",8)) { p+=7; continue; }
            if (*p=='\\' && p[1]=='\'') p++;
            if (isspace((unsigned char)*p) || *p=='`' || *p=='(' || *p==')') continue;
            if (used+1 >= sizeof(normalized)) { okay = false; break; }
            normalized[used++]=(char)tolower((unsigned char)*p);
        }
        if (!okay) break;
        if (strncmp(normalized,"typein",6)) continue;
        if (found || strlen(name)>=sizeof(restriction) || !*name) { okay=false; break; }
        for (const char *p=name; *p; p++) if (!isalnum((unsigned char)*p) && *p!='_') okay=false;
        if (!okay) break;
        found=true;
        expanded = !strcmp(normalized,"typein'invalid_job','missing_hours','inventory_due','inventory_return','inventory_verification'");
        if (!expanded && strcmp(normalized,"typein'invalid_job','missing_hours'") &&
            strcmp(normalized,"typein'invalid_job','missing_hours','inventory_due'")) { okay=false; break; }
        strcpy(restriction,name);
    }
    db_finalize(statement);
    if (!okay || result != DB_DONE || !found) return false;
    if (expanded) return true;
    char ddl[512];
    int length=snprintf(ddl,sizeof(ddl),"ALTER TABLE alert_rules DROP CHECK `%s`,"
        "ADD CONSTRAINT alert_rules_type CHECK(type IN ('invalid_job','missing_hours','inventory_due','inventory_return','inventory_verification'))",restriction);
    return length>0 && (size_t)length<sizeof(ddl) && db_exec(database,ddl,NULL,NULL,NULL)==DB_OK;
}

bool mysql_schema_prepare(void) {
    static const char *const tables[] = {
        "CREATE TABLE IF NOT EXISTS schema_metadata ("
        "id TINYINT NOT NULL PRIMARY KEY,version INT NOT NULL,"
        "CHECK(id=1),CHECK(version>=0)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        /* Internal certification guard: all application write transactions
           update this row before checking business rules on any primary. */
        "CREATE TABLE IF NOT EXISTS cluster_write_guard ("
        "id TINYINT NOT NULL PRIMARY KEY,generation BIGINT NOT NULL,"
        "CHECK(id=1),CHECK(generation>=0)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "email VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,"
        "name VARCHAR(120) NOT NULL,role VARCHAR(16) NOT NULL,"
        "active TINYINT NOT NULL DEFAULT 1,"
        "password_hash BINARY(32) NOT NULL,salt BINARY(16) NOT NULL,"
        "UNIQUE KEY users_email(email),"
        "CHECK(role IN ('manager','worker')),CHECK(active IN (0,1))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        /* Exact provider keys; binary columns keep the composite index below
           InnoDB's key-size limit without weakening subject uniqueness. */
        "CREATE TABLE IF NOT EXISTS oidc_identities ("
        "issuer VARBINARY(1023) NOT NULL,subject VARBINARY(255) NOT NULL,"
        "user_id BIGINT NOT NULL,PRIMARY KEY(issuer,subject),"
        "UNIQUE KEY oidc_identities_user(user_id),"
        "CONSTRAINT oidc_identities_user_fk FOREIGN KEY(user_id) REFERENCES users(id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS access_groups ("
        "code VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,"
        "name VARCHAR(120) NOT NULL,builtin TINYINT NOT NULL DEFAULT 1,"
        "CHECK(builtin=1),CHECK(code IN ('operator','manager','sales','finance'))) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS access_group_members ("
        "group_code VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,user_id BIGINT NOT NULL,"
        "PRIMARY KEY(group_code,user_id),KEY access_group_members_user(user_id,group_code),"
        "CONSTRAINT access_group_members_group_fk FOREIGN KEY(group_code) REFERENCES access_groups(code),"
        "CONSTRAINT access_group_members_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(group_code IN ('sales','finance'))) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS suppliers ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "code VARCHAR(40) COLLATE utf8mb4_general_ci NOT NULL,"
        "name VARCHAR(120) NOT NULL,vat_number VARCHAR(32) NOT NULL DEFAULT '',"
        "email VARCHAR(254) NOT NULL DEFAULT '',phone VARCHAR(40) NOT NULL DEFAULT '',"
        "address VARCHAR(240) NOT NULL DEFAULT '',notes VARCHAR(1000) NOT NULL DEFAULT '',"
        "active TINYINT NOT NULL DEFAULT 1,UNIQUE KEY suppliers_code(code),"
        "CHECK(CHAR_LENGTH(code)>=1),CHECK(CHAR_LENGTH(name)>=1),CHECK(active IN (0,1))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS contracts ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "code VARCHAR(40) COLLATE utf8mb4_general_ci NOT NULL,title VARCHAR(120) NOT NULL,"
        "supplier_id BIGINT NOT NULL,status VARCHAR(16) NOT NULL DEFAULT 'draft',"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "ends_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "value_cents BIGINT NULL,currency CHAR(3) NOT NULL DEFAULT 'EUR',notes VARCHAR(1000) NOT NULL DEFAULT '',"
        "UNIQUE KEY contracts_code(code),KEY contracts_supplier(supplier_id,id),"
        "CONSTRAINT contracts_supplier_fk FOREIGN KEY(supplier_id) REFERENCES suppliers(id),"
        "CHECK(CHAR_LENGTH(code)>=1),CHECK(CHAR_LENGTH(title)>=1),"
        "CHECK(status IN ('draft','active','expired','closed','cancelled')),"
        "CHECK(starts_on IS NULL OR CHAR_LENGTH(starts_on)=10),"
        "CHECK(ends_on IS NULL OR CHAR_LENGTH(ends_on)=10),"
        "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on),"
        "CHECK(value_cents IS NULL OR value_cents BETWEEN 0 AND 999999999999),CHECK(currency='EUR')"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS contract_documents ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,contract_id BIGINT NOT NULL,"
        "storage_key CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "file_name VARCHAR(180) NOT NULL,byte_size INT NOT NULL,"
        "sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,uploaded_by BIGINT NOT NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "UNIQUE KEY contract_documents_storage_key(storage_key),KEY contract_documents_contract(contract_id,id),"
        "CONSTRAINT contract_documents_contract_fk FOREIGN KEY(contract_id) REFERENCES contracts(id),"
        "CONSTRAINT contract_documents_user_fk FOREIGN KEY(uploaded_by) REFERENCES users(id),"
        "CHECK(CHAR_LENGTH(file_name)>=1),CHECK(byte_size BETWEEN 5 AND 10485760)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS jobs ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "code VARCHAR(40) COLLATE utf8mb4_general_ci NOT NULL,"
        "name VARCHAR(120) NOT NULL,client VARCHAR(120) NOT NULL DEFAULT '',"
        "description VARCHAR(1000) NOT NULL DEFAULT '',"
        "status VARCHAR(16) NOT NULL,contract_id BIGINT NULL,"
        "is_common TINYINT NOT NULL DEFAULT 0,time_closed TINYINT NOT NULL DEFAULT 0,"
        "archived TINYINT NOT NULL DEFAULT 0,lifecycle_revision BIGINT NOT NULL DEFAULT 1,"
        "UNIQUE KEY jobs_code(code),KEY jobs_contract(contract_id,id),"
        "CONSTRAINT jobs_contract_fk FOREIGN KEY(contract_id) REFERENCES contracts(id),"
        "CHECK(status IN ('planned','active','paused','completed','cancelled')),"
        "CHECK(is_common IN (0,1)),CHECK(time_closed IN (0,1)),CHECK(archived IN (0,1)),CHECK(lifecycle_revision>=1)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_tasks ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,job_id BIGINT NOT NULL,parent_id BIGINT NULL,"
        "assigned_user_id BIGINT NULL,title VARCHAR(160) NOT NULL,description VARCHAR(2000) NOT NULL DEFAULT '',"
        "status VARCHAR(16) NOT NULL DEFAULT 'todo',priority VARCHAR(16) NOT NULL DEFAULT 'normal',"
        "due_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,revision BIGINT NOT NULL DEFAULT 1,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "updated_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "UNIQUE KEY job_tasks_job_id(job_id,id),KEY job_tasks_job_status(job_id,status,id),"
        "KEY job_tasks_parent(job_id,parent_id,id),KEY job_tasks_assignee(assigned_user_id,job_id,id),"
        "CONSTRAINT job_tasks_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT job_tasks_parent_fk FOREIGN KEY(job_id,parent_id) REFERENCES job_tasks(job_id,id),"
        "CONSTRAINT job_tasks_assignee_fk FOREIGN KEY(assigned_user_id) REFERENCES users(id),"
        "CHECK(CHAR_LENGTH(title)>=1),CHECK(status IN ('todo','in_progress','blocked','in_review','done')),"
        "CHECK(priority IN ('low','normal','high','urgent')),CHECK(due_date IS NULL OR CHAR_LENGTH(due_date)=10),"
        "CHECK(revision>=1)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_blockers ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,job_id BIGINT NOT NULL,task_id BIGINT NULL,"
        "title VARCHAR(160) NOT NULL,description VARCHAR(2000) NOT NULL DEFAULT '',"
        "impact VARCHAR(16) NOT NULL DEFAULT 'medium',owner_id BIGINT NULL,status VARCHAR(16) NOT NULL DEFAULT 'open',"
        "next_action VARCHAR(1000) NOT NULL DEFAULT '',due_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "created_by BIGINT NOT NULL,revision BIGINT NOT NULL DEFAULT 1,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "updated_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "KEY job_blockers_job_status(job_id,status,id),KEY job_blockers_task(job_id,task_id,id),"
        "KEY job_blockers_owner(owner_id,job_id,id),"
        "CONSTRAINT job_blockers_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT job_blockers_task_fk FOREIGN KEY(job_id,task_id) REFERENCES job_tasks(job_id,id),"
        "CONSTRAINT job_blockers_owner_fk FOREIGN KEY(owner_id) REFERENCES users(id),"
        "CONSTRAINT job_blockers_creator_fk FOREIGN KEY(created_by) REFERENCES users(id),"
        "CHECK(CHAR_LENGTH(title)>=1),CHECK(impact IN ('low','medium','high','critical')),"
        "CHECK(status IN ('open','in_progress','resolved')),CHECK(due_date IS NULL OR CHAR_LENGTH(due_date)=10),"
        "CHECK(revision>=1)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS time_entries ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "job_id BIGINT NOT NULL,"
        "owner VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,"
        "date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "minutes SMALLINT NOT NULL,note VARCHAR(1000) NOT NULL DEFAULT '',"
        "user_id BIGINT NULL,revision BIGINT NOT NULL DEFAULT 1,task_id BIGINT NULL,"
        "KEY time_entries_owner_date(owner,date),"
        "KEY time_entries_job_owner(job_id,owner),"
        "KEY time_entries_user_date(user_id,date),"
        "KEY time_entries_job_user(job_id,user_id),KEY time_entries_task(task_id,date,id),"
        "CONSTRAINT time_entries_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT time_entries_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CONSTRAINT time_entries_task_fk FOREIGN KEY(task_id) REFERENCES job_tasks(id) ON DELETE SET NULL,"
        "CHECK(minutes BETWEEN 1 AND 1440),CHECK(CHAR_LENGTH(date)=10),CHECK(revision>=1)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS time_entry_batches (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "user_id BIGINT NOT NULL,request_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "payload_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,ids_json MEDIUMTEXT NOT NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL "
        "DEFAULT (DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ')),"
        "UNIQUE KEY time_entry_batches_request(user_id,request_key),"
        "CONSTRAINT time_entry_batches_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(CHAR_LENGTH(request_key)>=1)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS time_corrections (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "entry_id BIGINT NULL,original_entry_id BIGINT NOT NULL,requester_id BIGINT NOT NULL,entry_revision BIGINT NOT NULL,"
        "original_job_id BIGINT NOT NULL,original_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "original_minutes INT NOT NULL,original_note TEXT NOT NULL,before_json MEDIUMTEXT NOT NULL,job_id BIGINT NOT NULL,"
        "date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,minutes INT NOT NULL,note TEXT NOT NULL,reason TEXT NOT NULL,"
        "delete_requested TINYINT NOT NULL DEFAULT 0,status VARCHAR(16) NOT NULL DEFAULT 'pending',"
        "decision_reason TEXT NOT NULL DEFAULT (''),decided_by BIGINT NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL "
        "DEFAULT (DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ')),"
        "decided_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "KEY time_corrections_requester(requester_id,status,id),KEY time_corrections_entry(entry_id,status,id),"
        "CONSTRAINT time_corrections_entry_fk FOREIGN KEY(entry_id) REFERENCES time_entries(id) ON DELETE SET NULL,"
        "CONSTRAINT time_corrections_requester_fk FOREIGN KEY(requester_id) REFERENCES users(id),"
        "CONSTRAINT time_corrections_decider_fk FOREIGN KEY(decided_by) REFERENCES users(id) ON DELETE SET NULL,"
        "CHECK(entry_revision>=1),CHECK(original_minutes BETWEEN 1 AND 1440),CHECK(minutes BETWEEN 1 AND 1440),"
        "CHECK(delete_requested IN (0,1)),CHECK(status IN ('pending','approved','rejected','cancelled','conflict'))) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS user_work_schedules (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,user_id BIGINT NOT NULL,"
        "mon INT NOT NULL DEFAULT 480,tue INT NOT NULL DEFAULT 480,wed INT NOT NULL DEFAULT 480,"
        "thu INT NOT NULL DEFAULT 480,fri INT NOT NULL DEFAULT 480,sat INT NOT NULL DEFAULT 0,sun INT NOT NULL DEFAULT 0,"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '0001-01-01',"
        "UNIQUE KEY user_work_schedules_start(user_id,starts_on),"
        "CONSTRAINT user_work_schedules_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(mon BETWEEN 0 AND 1440),CHECK(tue BETWEEN 0 AND 1440),CHECK(wed BETWEEN 0 AND 1440),"
        "CHECK(thu BETWEEN 0 AND 1440),CHECK(fri BETWEEN 0 AND 1440),CHECK(sat BETWEEN 0 AND 1440),CHECK(sun BETWEEN 0 AND 1440)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS user_work_exceptions (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "user_id BIGINT NOT NULL,date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "expected_minutes INT NOT NULL,reason TEXT NOT NULL DEFAULT (''),UNIQUE KEY user_work_exceptions_date(user_id,date),"
        "CONSTRAINT user_work_exceptions_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(expected_minutes BETWEEN 0 AND 1440)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS business_holidays (date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,"
        "name VARCHAR(160) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_templates (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "name VARCHAR(120) COLLATE utf8mb4_general_ci NOT NULL,definition_json MEDIUMTEXT NOT NULL,created_by BIGINT NOT NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL "
        "DEFAULT (DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ')),UNIQUE KEY job_templates_name(name),"
        "CONSTRAINT job_templates_creator_fk FOREIGN KEY(created_by) REFERENCES users(id)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS work_notifications (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "user_id BIGINT NOT NULL,job_id BIGINT NOT NULL,task_id BIGINT NULL,blocker_id BIGINT NULL,"
        "event_key VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,type VARCHAR(32) NOT NULL,"
        "title TEXT NOT NULL,message TEXT NOT NULL,work_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "read_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL "
        "DEFAULT (DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ')),"
        "UNIQUE KEY work_notifications_event(user_id,event_key),KEY work_notifications_recipient(user_id,read_at,id),"
        "CONSTRAINT work_notifications_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CONSTRAINT work_notifications_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE,"
        "CONSTRAINT work_notifications_task_fk FOREIGN KEY(task_id) REFERENCES job_tasks(id) ON DELETE CASCADE,"
        "CONSTRAINT work_notifications_blocker_fk FOREIGN KEY(blocker_id) REFERENCES job_blockers(id) ON DELETE CASCADE) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS labor_rates ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,user_id BIGINT NOT NULL,"
        "hourly_cents BIGINT NOT NULL,currency CHAR(3) NOT NULL DEFAULT 'EUR',"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "ends_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "UNIQUE KEY labor_rates_user_start(user_id,starts_on),"
        "KEY labor_rates_user_dates(user_id,starts_on,ends_on,id),"
        "CONSTRAINT labor_rates_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(hourly_cents BETWEEN 0 AND 99999999),CHECK(currency='EUR'),"
        "CHECK(CHAR_LENGTH(starts_on)=10),CHECK(ends_on IS NULL OR CHAR_LENGTH(ends_on)=10),"
        "CHECK(ends_on IS NULL OR starts_on<=ends_on)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_roles ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "code VARCHAR(40) COLLATE utf8mb4_general_ci NOT NULL,"
        "name VARCHAR(120) NOT NULL,can_log_time TINYINT NOT NULL,builtin TINYINT NOT NULL DEFAULT 0,"
        "UNIQUE KEY job_roles_code(code),CHECK(CHAR_LENGTH(code)>=1),CHECK(CHAR_LENGTH(name)>=1),"
        "CHECK(can_log_time IN (0,1)),CHECK(builtin IN (0,1))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS organization_groups ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,name VARCHAR(120) NOT NULL,parent_id BIGINT NULL,"
        "KEY organization_groups_parent(parent_id,id),"
        "CONSTRAINT organization_groups_parent_fk FOREIGN KEY(parent_id) REFERENCES organization_groups(id),"
        "CHECK(CHAR_LENGTH(name)>=1)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS group_members ("
        "group_id BIGINT NOT NULL,user_id BIGINT NOT NULL,PRIMARY KEY(group_id,user_id),"
        "KEY group_members_user(user_id,group_id),"
        "CONSTRAINT group_members_group_fk FOREIGN KEY(group_id) REFERENCES organization_groups(id),"
        "CONSTRAINT group_members_user_fk FOREIGN KEY(user_id) REFERENCES users(id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS oidc_groups ("
        "issuer VARCHAR(1023) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "subject VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "group_id BIGINT NOT NULL UNIQUE,active TINYINT NOT NULL,PRIMARY KEY(issuer,subject),"
        "CONSTRAINT oidc_groups_group_fk FOREIGN KEY(group_id) REFERENCES organization_groups(id),"
        "CHECK(active IN (0,1))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_group_grants ("
        "job_id BIGINT NOT NULL,group_id BIGINT NOT NULL,role_id BIGINT NOT NULL,"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "ends_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,PRIMARY KEY(job_id,group_id),"
        "KEY job_group_grants_group(group_id,job_id),"
        "CONSTRAINT job_group_grants_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT job_group_grants_group_fk FOREIGN KEY(group_id) REFERENCES organization_groups(id),"
        "CONSTRAINT job_group_grants_role_fk FOREIGN KEY(role_id) REFERENCES job_roles(id),"
        "CHECK(starts_on IS NULL OR CHAR_LENGTH(starts_on)=10),"
        "CHECK(ends_on IS NULL OR CHAR_LENGTH(ends_on)=10),"
        "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS job_assignments ("
        "job_id BIGINT NOT NULL,user_id BIGINT NOT NULL,role_id BIGINT NULL,"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "ends_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "PRIMARY KEY(job_id,user_id),"
        "KEY job_assignments_user(user_id,job_id),"
        "CONSTRAINT job_assignments_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT job_assignments_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CONSTRAINT job_assignments_role_fk FOREIGN KEY(role_id) REFERENCES job_roles(id),"
        "CHECK(starts_on IS NULL OR CHAR_LENGTH(starts_on)=10),"
        "CHECK(ends_on IS NULL OR CHAR_LENGTH(ends_on)=10),"
        "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_rules (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,owner_id BIGINT NOT NULL,"
        "name VARCHAR(120) NOT NULL,type VARCHAR(24) NOT NULL,"
        "run_time CHAR(5) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,enabled TINYINT NOT NULL DEFAULT 0,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "updated_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "last_run_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "last_run_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,last_event_id BIGINT NOT NULL DEFAULT 0,"
        "KEY alert_rules_owner(owner_id,id),CONSTRAINT alert_rules_owner_fk FOREIGN KEY(owner_id) REFERENCES users(id),"
        "days_before INT NOT NULL DEFAULT 30,verification_days INT NOT NULL DEFAULT 90,"
        "CONSTRAINT alert_rules_type CHECK(type IN ('invalid_job','missing_hours','inventory_due','inventory_return','inventory_verification')),"
        "CONSTRAINT alert_rules_days_before CHECK(days_before BETWEEN 1 AND 365),"
        "CONSTRAINT alert_rules_verification_days CHECK(verification_days BETWEEN 1 AND 365),"
        "CHECK(CHAR_LENGTH(run_time)=5),CHECK(enabled IN (0,1)),"
        "CHECK(last_event_id>=0)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_rule_users (rule_id BIGINT NOT NULL,user_id BIGINT NOT NULL,"
        "PRIMARY KEY(rule_id,user_id),"
        "CONSTRAINT alert_rule_users_rule_fk FOREIGN KEY(rule_id) REFERENCES alert_rules(id),"
        "CONSTRAINT alert_rule_users_user_fk FOREIGN KEY(user_id) REFERENCES users(id)) ENGINE=InnoDB DEFAULT "
        "CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_rule_emails (rule_id BIGINT NOT NULL,"
        "email VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,PRIMARY KEY(rule_id,email),"
        "CONSTRAINT alert_rule_emails_rule_fk FOREIGN KEY(rule_id) REFERENCES alert_rules(id)) ENGINE=InnoDB DEFAULT "
        "CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_subjects (rule_id BIGINT NOT NULL,user_id BIGINT NOT NULL,"
        "starts_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "last_checked_on CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,PRIMARY KEY(rule_id,user_id),"
        "CONSTRAINT alert_subjects_rule_fk FOREIGN KEY(rule_id) REFERENCES alert_rules(id),"
        "CONSTRAINT alert_subjects_user_fk FOREIGN KEY(user_id) REFERENCES users(id),CHECK(CHAR_LENGTH(starts_on)=10),"
        "CHECK(last_checked_on IS NULL OR CHAR_LENGTH(last_checked_on)=10)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
        "COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_events (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,user_id BIGINT NOT NULL,"
        "job_id BIGINT NOT NULL,work_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,minutes INT NOT NULL,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,KEY alert_events_created(created_at,id),"
        "CONSTRAINT alert_events_user_fk FOREIGN KEY(user_id) REFERENCES users(id),CHECK(CHAR_LENGTH(work_date)=10),"
        "CHECK(minutes BETWEEN 1 AND 1440)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_notifications (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "rule_id BIGINT NOT NULL,recipient_id BIGINT NOT NULL,subject_user_id BIGINT NOT NULL,"
        "event_key VARCHAR(120) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,title VARCHAR(160) NOT NULL,"
        "message VARCHAR(1000) NOT NULL,work_date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NULL,"
        "recorded_minutes INT NULL,created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "read_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NULL,UNIQUE KEY alert_notifications_event(rule_id,"
        "recipient_id,event_key),KEY alert_notifications_recipient(recipient_id,id),"
        "KEY alert_notifications_unread(recipient_id,read_at,id),"
        "CONSTRAINT alert_notifications_rule_fk FOREIGN KEY(rule_id) REFERENCES alert_rules(id),"
        "CONSTRAINT alert_notifications_recipient_fk FOREIGN KEY(recipient_id) REFERENCES users(id),"
        "CONSTRAINT alert_notifications_subject_fk FOREIGN KEY(subject_user_id) REFERENCES users(id)) ENGINE=InnoDB "
        "DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS alert_email_outbox (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "rule_id BIGINT NOT NULL,email VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,subject_user_id BIGINT NOT NULL,"
        "event_key VARCHAR(120) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,title VARCHAR(160) NOT NULL,"
        "message VARCHAR(1000) NOT NULL,created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "status VARCHAR(24) NOT NULL DEFAULT 'pending_configuration',UNIQUE KEY alert_email_outbox_event(rule_id,email,"
        "event_key),CONSTRAINT alert_email_outbox_rule_fk FOREIGN KEY(rule_id) REFERENCES alert_rules(id),"
        "CONSTRAINT alert_email_outbox_subject_fk FOREIGN KEY(subject_user_id) REFERENCES users(id),"
        "CONSTRAINT alert_email_outbox_status CHECK(status IN "
        "('pending_configuration','retrying','sent','delivery_unknown','delivery_failed'))) ENGINE=InnoDB "
        "DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS audit_events ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "created_at CHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "actor_id BIGINT NULL,actor_name VARCHAR(120) NOT NULL,"
        "actor_email VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,"
        "action VARCHAR(64) NOT NULL,entity_type VARCHAR(32) NOT NULL,"
        "entity_id BIGINT NOT NULL,before_json MEDIUMTEXT NULL,after_json MEDIUMTEXT NULL,"
        "KEY audit_events_entity(entity_type,id),"
        "CONSTRAINT audit_events_actor_fk FOREIGN KEY(actor_id) REFERENCES users(id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
    };
    static const struct { const char *name; const char *ddl; } triggers[] = {
        {"time_entries_require_user_insert",
         "CREATE TRIGGER time_entries_require_user_insert BEFORE INSERT ON time_entries "
         "FOR EACH ROW BEGIN IF NEW.user_id IS NULL THEN "
         "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='user_id is required'; END IF; END"},
        {"time_entries_preserve_user_update",
         "CREATE TRIGGER time_entries_preserve_user_update BEFORE UPDATE ON time_entries "
         "FOR EACH ROW BEGIN IF OLD.user_id IS NOT NULL AND "
         "(NEW.user_id IS NULL OR NEW.user_id<>OLD.user_id) THEN "
         "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='user_id is immutable'; END IF; END"},
        {"labor_rates_preserve_user_update",
         "CREATE TRIGGER labor_rates_preserve_user_update BEFORE UPDATE ON labor_rates "
         "FOR EACH ROW BEGIN IF NEW.user_id<>OLD.user_id THEN "
         "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='rate user_id is immutable'; END IF; END"},
        {"access_groups_no_update",
         "CREATE TRIGGER access_groups_no_update BEFORE UPDATE ON access_groups "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='functional groups are immutable'"},
        {"access_groups_no_delete",
         "CREATE TRIGGER access_groups_no_delete BEFORE DELETE ON access_groups "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='functional groups are immutable'"},
        {"contract_documents_no_update",
         "CREATE TRIGGER contract_documents_no_update BEFORE UPDATE ON contract_documents "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='contract documents are immutable'"},
        {"contract_documents_no_delete",
         "CREATE TRIGGER contract_documents_no_delete BEFORE DELETE ON contract_documents "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='contract documents are immutable'"},
        {"audit_events_no_update",
         "CREATE TRIGGER audit_events_no_update BEFORE UPDATE ON audit_events "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit events are immutable'"},
        {"audit_events_no_delete",
         "CREATE TRIGGER audit_events_no_delete BEFORE DELETE ON audit_events "
         "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit events are immutable'"}
    };

    Db *database = NULL;
    bool okay = open_database(&database, DB_OPEN_READWRITE | DB_OPEN_CREATE) &&
        metadata_supported(database);
    for (size_t i = 0; okay && i < sizeof(tables) / sizeof(tables[0]); i++)
        okay = db_exec(database, tables[i], NULL, NULL, NULL) == DB_OK;
    if (okay) okay = ensure_job_contract(database);
    if (okay) okay = ensure_assignment_role(database);
    if (okay) okay = ensure_job_flags(database);
    if (okay) okay = ensure_time_entry_columns(database);
    if (okay) okay = ensure_email_statuses(database);
    if (okay) okay = ensure_inventory_alerts(database);
    if (okay) okay = db_exec(database,
        "INSERT IGNORE INTO schema_metadata(id,version) VALUES(1,0)",
        NULL, NULL, NULL) == DB_OK;
    if (okay) okay = db_exec(database,
        "INSERT IGNORE INTO cluster_write_guard(id,generation) VALUES(1,0)",
        NULL, NULL, NULL) == DB_OK;
    for (size_t i = 0; okay && i < sizeof(triggers) / sizeof(triggers[0]); i++)
        okay = ensure_trigger(database, triggers[i].name, triggers[i].ddl);
    if (database) (void)db_close(database);
    return okay;
}

#endif
