#include "mysql_schema.h"

#ifdef PUNTO_MYSQL

#include "db.h"

#include <stddef.h>

#define MYSQL_SCHEMA_VERSION 3

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
        "UNIQUE KEY jobs_code(code),KEY jobs_contract(contract_id,id),"
        "CONSTRAINT jobs_contract_fk FOREIGN KEY(contract_id) REFERENCES contracts(id),"
        "CHECK(status IN ('planned','active','paused','completed','cancelled'))"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",

        "CREATE TABLE IF NOT EXISTS time_entries ("
        "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "job_id BIGINT NOT NULL,"
        "owner VARCHAR(254) COLLATE utf8mb4_general_ci NOT NULL,"
        "date CHAR(10) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "minutes SMALLINT NOT NULL,note VARCHAR(1000) NOT NULL DEFAULT '',"
        "user_id BIGINT NULL,"
        "KEY time_entries_owner_date(owner,date),"
        "KEY time_entries_job_owner(job_id,owner),"
        "KEY time_entries_user_date(user_id,date),"
        "KEY time_entries_job_user(job_id,user_id),"
        "CONSTRAINT time_entries_job_fk FOREIGN KEY(job_id) REFERENCES jobs(id),"
        "CONSTRAINT time_entries_user_fk FOREIGN KEY(user_id) REFERENCES users(id),"
        "CHECK(minutes BETWEEN 1 AND 1440),CHECK(CHAR_LENGTH(date)=10)"
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
