#include "workspace.h"
#include "storage.h"

#include <limits.h>
#include "db.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORM_LIMIT 32768U
#define RESULT_LIMIT 1000
#define JSON_LIMIT (16U * 1024U * 1024U)

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

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
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
        if (!data) {
            json->failed = true;
            return false;
        }
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
                } else {
                    (void)append_bytes(json, (const char *)p, 1);
                }
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
    if (json->failed) {
        free(json->data);
        server_error(response);
    } else {
        response->status = status;
        response->body = json->data;
    }
}

static bool prepare(DbStmt **statement, const char *sql) {
    return db_prepare_v2(database, sql, -1, statement, NULL) == DB_OK;
}

static bool bind_text(DbStmt *statement, int index, const char *text) {
    return db_bind_text(statement, index, text, -1, DB_TRANSIENT) == DB_OK;
}

static bool bind_id(DbStmt *statement, int index, DbInt value) {
    return db_bind_int64(statement, index, value) == DB_OK;
}

static const char *column_text(DbStmt *statement, int column) {
    const unsigned char *text = db_column_text(statement, column);
    return text ? (const char *)text : "";
}

static bool begin_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == DB_OK)
        return true;
    server_error(response);
    return false;
}

static void rollback(void) {
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
}

static bool commit_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
    rollback();
    server_error(response);
    return false;
}

bool workspace_open(const char *db_path) {
    workspace_close();
    if (!db_path || !*db_path ||
        db_open_v2(db_path, &database,
                        DB_OPEN_READWRITE | DB_OPEN_CREATE | DB_OPEN_FULLMUTEX,
                        NULL) != DB_OK) {
        workspace_close();
        return false;
    }
    if (db_busy_timeout(database, 5000) != DB_OK
#ifndef PUNTO_MYSQL
        || db_exec(database,
            "PRAGMA foreign_keys = ON;"
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS jobs ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "code TEXT NOT NULL COLLATE NOCASE UNIQUE,"
            "name TEXT NOT NULL,client TEXT NOT NULL DEFAULT '',"
            "description TEXT NOT NULL DEFAULT '',"
            "status TEXT NOT NULL CHECK(status IN ('planned','active','paused','completed','cancelled'))"
            ");"
            "CREATE TABLE IF NOT EXISTS time_entries ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "job_id INTEGER NOT NULL REFERENCES jobs(id),"
            "owner TEXT NOT NULL,date TEXT NOT NULL,"
            "minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),"
            "note TEXT NOT NULL DEFAULT ''"
            ");"
            "CREATE INDEX IF NOT EXISTS time_entries_owner_date ON time_entries(owner,date);"
            "CREATE INDEX IF NOT EXISTS time_entries_job_owner ON time_entries(job_id,owner);"
            "COMMIT;", NULL, NULL, NULL) != DB_OK
#endif
    ) {
        rollback();
        workspace_close();
        return false;
    }
    return true;
}

void workspace_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
}

void workspace_response_free(WorkspaceResponse *response) {
    if (!response) return;
    free(response->body);
    response->body = NULL;
    response->status = 0;
}

/* Check canonical UTF-8, count characters, and reject forbidden control values. */
static bool utf8_length(const char *text, size_t *length) {
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (*p) {
        uint32_t codepoint;
        size_t continuation;
        unsigned char first = *p++;
        if (first < 0x80) { codepoint = first; continuation = 0; }
        else if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1fU;
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0fU;
            continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07U;
            continuation = 3;
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
            if (length - i < 3) { free(output); return NULL; }
            int high = hex_digit((unsigned char)input[i + 1]);
            int low = hex_digit((unsigned char)input[i + 2]);
            if (high < 0 || low < 0) { free(output); return NULL; }
            c = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (c == '+') c = ' ';
        if (!c) { free(output); return NULL; }
        output[used++] = (char)c;
    }
    output[used] = '\0';
    size_t characters;
    if (!utf8_length(output, &characters)) { free(output); return NULL; }
    return output;
}

static void free_fields(Field *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
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
    return true;
invalid:
    free_fields(fields, count);
    if (out_of_memory) server_error(response);
    else error(response, 400, "Dati non validi: controlla i campi inviati.");
    return false;
}

static char *field_value(Field *field) {
    return field->value ? field->value : "";
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

static bool valid_text(const char *text, size_t maximum, bool required,
                       bool multiline) {
    size_t length;
    if (!utf8_length(text, &length) || length > maximum || (required && !length))
        return false;
    return multiline || !strpbrk(text, "\r\n\t");
}

static bool parse_positive(const char *text, DbInt *value) {
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

static bool valid_date(const char *date) {
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
               (date[2] - '0') * 10 + date[3] - '0';
    int month = (date[5] - '0') * 10 + date[6] - '0';
    int day = (date[8] - '0') * 10 + date[9] - '0';
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    int maximum = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) maximum++;
    return day <= maximum;
}

static bool valid_status(const char *status) {
    return strcmp(status, "planned") == 0 || strcmp(status, "active") == 0 ||
        strcmp(status, "paused") == 0 || strcmp(status, "completed") == 0 ||
        strcmp(status, "cancelled") == 0;
}

static bool today_date(char date[11]) {
    time_t now = time(NULL);
    struct tm local;
    return now != (time_t)-1 && localtime_r(&now, &local) &&
           strftime(date, 11, "%Y-%m-%d", &local) == 10;
}

static void job_json(Json *json, DbStmt *statement) {
    append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"code\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"name\":"); append_string(json, column_text(statement, 2));
    append(json, ",\"client\":"); append_string(json, column_text(statement, 3));
    append(json, ",\"description\":"); append_string(json, column_text(statement, 4));
    append(json, ",\"status\":"); append_string(json, column_text(statement, 5));
}

static void job_contract_json(Json *json, DbStmt *statement, int index) {
    append(json, ",\"contract_id\":");
    if (db_column_type(statement,index) == DB_NULL) append(json,"null");
    else append_integer(json,db_column_int64(statement,index));
    append(json, ",\"contract_code\":"); append_string(json,column_text(statement,index+1));
    append(json, ",\"contract_title\":"); append_string(json,column_text(statement,index+2));
    append(json, ",\"supplier_name\":"); append_string(json,column_text(statement,index+3));
}

/* Snapshot helpers return 1 for an existing row, 0 for a missing row, -1 on error. */
static int job_snapshot(DbInt id, Json *json) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT j.id,j.code,j.name,j.client,j.description,j.status,j.contract_id,c.code,c.title,s.name "
        "FROM jobs j LEFT JOIN contracts c ON c.id=j.contract_id LEFT JOIN suppliers s ON s.id=c.supplier_id WHERE j.id=?") ||
        !bind_id(statement, 1, id)) {
        db_finalize(statement);
        return -1;
    }
    int result = db_step(statement);
    if (result == DB_ROW) {
        job_json(json, statement);
        job_contract_json(json,statement,6);
        append(json, "}");
    }
    db_finalize(statement);
    if (json->failed) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static int entry_snapshot(DbInt id, DbInt actor_id, Json *json) {
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT t.id,t.job_id,t.user_id,t.owner,t.date,t.minutes,t.note,j.code,j.name "
        "FROM time_entries t JOIN jobs j ON j.id=t.job_id WHERE t.id=? AND t.user_id=?") ||
        !bind_id(statement, 1, id) || !bind_id(statement, 2, actor_id)) {
        db_finalize(statement);
        return -1;
    }
    int result = db_step(statement);
    if (result == DB_ROW) {
        append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
        append(json, ",\"job_id\":"); append_integer(json, db_column_int64(statement, 1));
        append(json, ",\"user_id\":"); append_integer(json, db_column_int64(statement, 2));
        append(json, ",\"owner\":"); append_string(json, column_text(statement, 3));
        append(json, ",\"date\":"); append_string(json, column_text(statement, 4));
        append(json, ",\"minutes\":"); append_integer(json, db_column_int64(statement, 5));
        append(json, ",\"note\":"); append_string(json, column_text(statement, 6));
        append(json, ",\"job_code\":"); append_string(json, column_text(statement, 7));
        append(json, ",\"job_name\":"); append_string(json, column_text(statement, 8));
        append(json, "}");
    }
    db_finalize(statement);
    if (json->failed) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static int assignment_snapshot(DbInt job_id, DbInt user_id, Json *json) {
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT a.job_id,a.user_id,a.starts_on,a.ends_on,j.code,j.name,u.name,u.email "
        "FROM job_assignments a JOIN jobs j ON j.id=a.job_id JOIN users u ON u.id=a.user_id "
        "WHERE a.job_id=? AND a.user_id=?") ||
        !bind_id(statement, 1, job_id) || !bind_id(statement, 2, user_id)) {
        db_finalize(statement);
        return -1;
    }
    int result = db_step(statement);
    if (result == DB_ROW) {
        append(json, "{\"job_id\":"); append_integer(json, db_column_int64(statement, 0));
        append(json, ",\"user_id\":"); append_integer(json, db_column_int64(statement, 1));
        append(json, ",\"starts_on\":"); append_string(json, column_text(statement, 2));
        append(json, ",\"ends_on\":"); append_string(json, column_text(statement, 3));
        append(json, ",\"job_code\":"); append_string(json, column_text(statement, 4));
        append(json, ",\"job_name\":"); append_string(json, column_text(statement, 5));
        append(json, ",\"user_name\":"); append_string(json, column_text(statement, 6));
        append(json, ",\"user_email\":"); append_string(json, column_text(statement, 7));
        append(json, "}");
    }
    db_finalize(statement);
    if (json->failed) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static void success(WorkspaceResponse *response, int status, DbInt id) {
    Json json = {0};
    append(&json, "{\"ok\":true");
    if (id) { append(&json, ",\"id\":"); append_integer(&json, id); }
    append(&json, "}");
    finish_json(response, status, &json);
}

static void list_jobs(DbInt actor_id, bool manager, WorkspaceResponse *response) {
    char today[11];
    if (!today_date(today)) { server_error(response); return; }
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT j.id,j.code,j.name,j.client,j.description,j.status,"
        "COALESCE((SELECT SUM(t.minutes) FROM time_entries t WHERE t.job_id=j.id AND t.user_id=?),0),"
        "CASE WHEN ? THEN (SELECT COUNT(*) FROM job_assignments a_count WHERE a_count.job_id=j.id) ELSE 0 END, "
        "j.contract_id,c.code,c.title,s.name "
        "FROM jobs j LEFT JOIN contracts c ON c.id=j.contract_id LEFT JOIN suppliers s ON s.id=c.supplier_id "
        "WHERE ? OR EXISTS(SELECT 1 FROM job_assignments a "
        "WHERE a.job_id=j.id AND a.user_id=? AND (a.starts_on IS NULL OR a.starts_on<=?) "
        "AND (a.ends_on IS NULL OR a.ends_on>=?)) ORDER BY j.id DESC LIMIT 1001") ||
        !bind_id(statement, 1, actor_id) || !bind_id(statement, 2, manager) ||
        !bind_id(statement, 3, manager) || !bind_id(statement, 4, actor_id) ||
        !bind_text(statement, 5, today) || !bind_text(statement, 6, today)) {
        db_finalize(statement);
        server_error(response);
        return;
    }
    Json json = {0};
    append(&json, "{\"jobs\":[");
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > RESULT_LIMIT) break;
        if (count > 1) append(&json, ",");
        job_json(&json, statement);
        job_contract_json(&json,statement,8);
        append(&json, ",\"total_minutes\":"); append_integer(&json, db_column_int64(statement, 6));
        append(&json, ",\"assigned_users\":"); append_integer(&json, db_column_int64(statement, 7));
        append(&json, "}");
    }
    db_finalize(statement);
    if (count > RESULT_LIMIT || result != DB_DONE) {
        free(json.data);
        if (count > RESULT_LIMIT)
            error(response, 409, "Sono presenti troppe commesse: il limite è di 1000.");
        else server_error(response);
        return;
    }
    append(&json, "]}");
    finish_json(response, 200, &json);
}

static void write_job(const char *body, DbInt actor_id, DbInt id,
                      WorkspaceResponse *response) {
    Field fields[] = {{"code", NULL}, {"name", NULL}, {"client", NULL},
                      {"description", NULL}, {"status", NULL}, {"contract_id", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    trim(fields[0].value); trim(fields[1].value); trim(fields[2].value);
    const char *code = field_value(&fields[0]), *name = field_value(&fields[1]);
    const char *client = field_value(&fields[2]), *description = field_value(&fields[3]);
    const char *status = field_value(&fields[4]);
    DbInt contract_id = 0, old_contract_id = 0;
    if ((fields[5].value && *fields[5].value && !parse_positive(fields[5].value,&contract_id)) ||
        !valid_text(code, 40, true, false) || !valid_text(name, 120, true, false) ||
        !valid_text(client, 120, false, false) || !valid_text(description, 1000, false, true) ||
        !valid_status(status)) {
        error(response, 400, "Inserisci codice (max 40 caratteri), nome (max 120), cliente (max 120), descrizione (max 1000) e uno stato valido.");
        free_fields(fields, count);
        return;
    }
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    if (id) {
        int found = job_snapshot(id, &before);
        if (found < 0) goto database_failure;
        if (!found) {
            rollback(); error(response, 404, "Commessa non trovata.");
            free_fields(fields, count); return;
        }
        if (!prepare(&statement,"SELECT contract_id FROM jobs WHERE id=?") ||
            !bind_id(statement,1,id) || db_step(statement) != DB_ROW) goto database_failure;
        old_contract_id = db_column_int64(statement,0);
        db_finalize(statement); statement = NULL;
        if (!fields[5].value) contract_id = old_contract_id;
    } else {
        if (!prepare(&statement, "SELECT COUNT(*) FROM jobs") ||
            db_step(statement) != DB_ROW) goto database_failure;
        DbInt existing = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
        if (existing >= RESULT_LIMIT) {
            rollback(); error(response, 409, "Hai raggiunto il limite di 1000 commesse.");
            free_fields(fields, count); return;
        }
    }
    if (contract_id && contract_id != old_contract_id) {
        if (!prepare(&statement,"SELECT status FROM contracts WHERE id=?") || !bind_id(statement,1,contract_id)) goto database_failure;
        int found = db_step(statement);
        bool attachable = found == DB_ROW && strcmp(column_text(statement,0),"closed") != 0 &&
            strcmp(column_text(statement,0),"cancelled") != 0;
        db_finalize(statement); statement = NULL;
        if (found != DB_ROW && found != DB_DONE) goto database_failure;
        if (!attachable) {
            rollback(); error(response,400,"Seleziona un contratto esistente che non sia chiuso o annullato.");
            free(before.data); free_fields(fields,count); return;
        }
    }
    if (!prepare(&statement, id ?
        "UPDATE jobs SET code=?,name=?,client=?,description=?,status=?,contract_id=? WHERE id=?" :
        "INSERT INTO jobs(code,name,client,description,status,contract_id) VALUES(?,?,?,?,?,?)") ||
        !bind_text(statement, 1, code) || !bind_text(statement, 2, name) ||
        !bind_text(statement, 3, client) || !bind_text(statement, 4, description) ||
        !bind_text(statement, 5, status) ||
        (contract_id ? !bind_id(statement,6,contract_id) : db_bind_null(statement,6) != DB_OK) ||
        (id && !bind_id(statement, 7, id)))
        goto database_failure;
    int result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) {
        int extended = db_extended_errcode(database);
        rollback();
        if (extended == DB_CONSTRAINT_UNIQUE)
            error(response, 409, "Esiste già una commessa con questo codice.");
        else server_error(response);
        free(before.data); free_fields(fields, count); return;
    }
    DbInt saved_id = id ? id : db_last_insert_rowid(database);
    if (job_snapshot(saved_id, &after) != 1 ||
        !storage_audit(database, actor_id, id ? "jobs.update" : "jobs.create", "job",
                       saved_id, before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, id ? 200 : 201, saved_id);
    free(before.data); free(after.data); free_fields(fields, count);
    return;
database_failure:
    db_finalize(statement); rollback();
    free(before.data); free(after.data); free_fields(fields, count);
    server_error(response);
}

static void list_entries(const char *query, DbInt actor_id, WorkspaceResponse *response) {
    Field date = {"date", NULL};
    if (!parse_fields(query, &date, 1, response)) return;
    if (!valid_date(field_value(&date))) {
        free_fields(&date, 1);
        error(response, 400, "Seleziona una data valida nel formato AAAA-MM-GG.");
        return;
    }
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT t.id,t.job_id,j.code,j.name,t.date,t.minutes,t.note,t.owner,t.user_id "
        "FROM time_entries t JOIN jobs j ON j.id=t.job_id "
        "WHERE t.user_id=? AND t.date=? ORDER BY t.id DESC LIMIT 1001") ||
        !bind_id(statement, 1, actor_id) || !bind_text(statement, 2, date.value)) {
        db_finalize(statement); free_fields(&date, 1); server_error(response); return;
    }
    free_fields(&date, 1);
    Json json = {0};
    append(&json, "{\"entries\":[");
    int result, count = 0;
    DbInt total = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > RESULT_LIMIT) break;
        if (count > 1) append(&json, ",");
        append(&json, "{\"id\":"); append_integer(&json, db_column_int64(statement, 0));
        append(&json, ",\"job_id\":"); append_integer(&json, db_column_int64(statement, 1));
        append(&json, ",\"job_code\":"); append_string(&json, column_text(statement, 2));
        append(&json, ",\"job_name\":"); append_string(&json, column_text(statement, 3));
        append(&json, ",\"date\":"); append_string(&json, column_text(statement, 4));
        append(&json, ",\"minutes\":"); append_integer(&json, db_column_int64(statement, 5));
        append(&json, ",\"note\":"); append_string(&json, column_text(statement, 6));
        append(&json, ",\"owner\":"); append_string(&json, column_text(statement, 7));
        append(&json, ",\"user_id\":"); append_integer(&json, db_column_int64(statement, 8));
        append(&json, "}");
        total += db_column_int64(statement, 5);
    }
    db_finalize(statement);
    if (count > RESULT_LIMIT || result != DB_DONE) {
        free(json.data);
        if (count > RESULT_LIMIT)
            error(response, 409, "Sono presenti troppe registrazioni: il limite giornaliero è di 1000.");
        else server_error(response);
        return;
    }
    append(&json, "],\"total_minutes\":"); append_integer(&json, total); append(&json, "}");
    finish_json(response, 200, &json);
}

static void write_entry(const char *body, DbInt actor_id, const char *owner,
                        bool manager, DbInt id, WorkspaceResponse *response) {
    Field fields[] = {{"job_id", NULL}, {"date", NULL}, {"minutes", NULL}, {"note", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    DbInt job_id, minutes;
    const char *date = field_value(&fields[1]), *note = field_value(&fields[3]);
    if (!parse_positive(field_value(&fields[0]), &job_id) || !valid_date(date) ||
        !parse_positive(field_value(&fields[2]), &minutes) || minutes > 1440 ||
        !valid_text(note, 1000, false, true)) {
        error(response, 400, "Seleziona una commessa, una data valida e una durata da 1 a 1440 minuti. Le note possono contenere al massimo 1000 caratteri.");
        free_fields(fields, count); return;
    }
    char today[11];
    if (!today_date(today)) { free_fields(fields, count); server_error(response); return; }
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    int result;
    if (id) {
        int found = entry_snapshot(id, actor_id, &before);
        if (found < 0) goto database_failure;
        if (!found) {
            rollback(); error(response, 404, "Registrazione delle ore non trovata.");
            free_fields(fields, count); return;
        }
    }
    if (!prepare(&statement, manager ? "SELECT 1 FROM jobs WHERE id=?" :
        "SELECT 1 FROM job_assignments WHERE job_id=? AND user_id=? "
        "AND (starts_on IS NULL OR (starts_on<=? AND starts_on<=?)) "
        "AND (ends_on IS NULL OR (ends_on>=? AND ends_on>=?))") ||
        !bind_id(statement, 1, job_id) || (!manager &&
        (!bind_id(statement, 2, actor_id) || !bind_text(statement, 3, today) ||
         !bind_text(statement, 4, date) || !bind_text(statement, 5, today) ||
         !bind_text(statement, 6, date)))) goto database_failure;
    result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result == DB_DONE) {
        rollback();
        error(response, manager ? 404 : 403, manager ? "Commessa non trovata." :
            "La commessa non è abilitata per te oggi e nella data lavorata. Contatta il gestore.");
        free(before.data); free_fields(fields, count); return;
    }
    if (result != DB_ROW) goto database_failure;
    if (!prepare(&statement,
        "SELECT COALESCE(SUM(minutes),0),COUNT(*) FROM time_entries "
        "WHERE user_id=? AND date=? AND id<>?") || !bind_id(statement, 1, actor_id) ||
        !bind_text(statement, 2, date) || !bind_id(statement, 3, id) ||
        db_step(statement) != DB_ROW) goto database_failure;
    DbInt total = db_column_int64(statement, 0);
    DbInt existing = db_column_int64(statement, 1);
    db_finalize(statement); statement = NULL;
    if (total > 1440 - minutes || existing >= RESULT_LIMIT) {
        rollback();
        error(response, 409, total > 1440 - minutes ?
            "Le ore registrate per questa giornata non possono superare le 24 ore." :
            "Hai raggiunto il limite di 1000 registrazioni per questa giornata.");
        free(before.data); free_fields(fields, count); return;
    }
    if (!prepare(&statement, id ?
        "UPDATE time_entries SET job_id=?,date=?,minutes=?,note=? WHERE id=? AND user_id=?" :
        "INSERT INTO time_entries(job_id,date,minutes,note,owner,user_id) VALUES(?,?,?,?,?,?)") ||
        !bind_id(statement, 1, job_id) || !bind_text(statement, 2, date) ||
        !bind_id(statement, 3, minutes) || !bind_text(statement, 4, note) ||
        (id ? (!bind_id(statement, 5, id) || !bind_id(statement, 6, actor_id)) :
              (!bind_text(statement, 5, owner) || !bind_id(statement, 6, actor_id))) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    DbInt saved_id = id ? id : db_last_insert_rowid(database);
    if (entry_snapshot(saved_id, actor_id, &after) != 1 ||
        !storage_audit(database, actor_id, id ? "time_entries.update" : "time_entries.create",
                       "time_entry", saved_id, before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, id ? 200 : 201, saved_id);
    free(before.data); free(after.data); free_fields(fields, count); return;
database_failure:
    db_finalize(statement); rollback();
    free(before.data); free(after.data); free_fields(fields, count); server_error(response);
}

static void delete_entry(const char *body, DbInt actor_id, DbInt id,
                         WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La richiesta di eliminazione non deve contenere campi."); return; }
    if (!begin_transaction(response)) return;
    Json before = {0};
    DbStmt *statement = NULL;
    int found = entry_snapshot(id, actor_id, &before);
    if (found < 0) goto database_failure;
    if (!found) {
        rollback(); error(response, 404, "Registrazione delle ore non trovata."); return;
    }
    if (!prepare(&statement, "DELETE FROM time_entries WHERE id=? AND user_id=?") ||
        !bind_id(statement, 1, id) || !bind_id(statement, 2, actor_id) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (!storage_audit(database, actor_id, "time_entries.delete", "time_entry", id,
                       before.data, NULL)) goto database_failure;
    if (commit_transaction(response)) success(response, 200, 0);
    free(before.data); return;
database_failure:
    db_finalize(statement); rollback(); free(before.data); server_error(response);
}

static int job_exists(DbInt job_id) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT 1 FROM jobs WHERE id=?") || !bind_id(statement, 1, job_id)) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement);
    db_finalize(statement);
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static void list_assignments(DbInt job_id, WorkspaceResponse *response) {
    int found = job_exists(job_id);
    if (found < 0) { server_error(response); return; }
    if (!found) { error(response, 404, "Commessa non trovata."); return; }
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT u.id,u.name,u.email,u.role,u.active,a.starts_on,a.ends_on "
        "FROM job_assignments a JOIN users u ON u.id=a.user_id "
        "WHERE a.job_id=? ORDER BY u.name COLLATE NOCASE,u.id LIMIT 201") ||
        !bind_id(statement, 1, job_id)) {
        db_finalize(statement); server_error(response); return;
    }
    Json json = {0};
    append(&json, "{\"job_id\":"); append_integer(&json, job_id);
    append(&json, ",\"assignments\":[");
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > 200) break;
        if (count > 1) append(&json, ",");
        append(&json, "{\"user_id\":"); append_integer(&json, db_column_int64(statement, 0));
        append(&json, ",\"name\":"); append_string(&json, column_text(statement, 1));
        append(&json, ",\"email\":"); append_string(&json, column_text(statement, 2));
        append(&json, ",\"role\":"); append_string(&json, column_text(statement, 3));
        append(&json, ",\"active\":"); append(&json, db_column_int(statement, 4) ? "true" : "false");
        append(&json, ",\"starts_on\":"); append_string(&json, column_text(statement, 5));
        append(&json, ",\"ends_on\":"); append_string(&json, column_text(statement, 6));
        append(&json, "}");
    }
    db_finalize(statement);
    if (result != DB_DONE || count > 200) { free(json.data); server_error(response); return; }
    append(&json, "]}"); finish_json(response, 200, &json);
}

static bool bind_date(DbStmt *statement, int index, const char *date) {
    return *date ? bind_text(statement, index, date) : db_bind_null(statement, index) == DB_OK;
}

static void write_assignment(const char *body, DbInt actor_id,
                             DbInt job_id, WorkspaceResponse *response) {
    Field fields[] = {{"user_id", NULL}, {"starts_on", NULL}, {"ends_on", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    DbInt user_id;
    const char *starts_on = field_value(&fields[1]), *ends_on = field_value(&fields[2]);
    if (!parse_positive(field_value(&fields[0]), &user_id) ||
        (*starts_on && !valid_date(starts_on)) || (*ends_on && !valid_date(ends_on)) ||
        (*starts_on && *ends_on && strcmp(starts_on, ends_on) > 0)) {
        error(response, 400, "Seleziona un utente e un intervallo di date valido. La fine non può precedere l'inizio.");
        free_fields(fields, count); return;
    }
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    int found = job_exists(job_id);
    if (found < 0) goto database_failure;
    if (!found) {
        rollback(); error(response, 404, "Commessa non trovata."); free_fields(fields, count); return;
    }
    if (!prepare(&statement, "SELECT active FROM users WHERE id=?") ||
        !bind_id(statement, 1, user_id)) goto database_failure;
    int result = db_step(statement);
    bool active = result == DB_ROW && db_column_int(statement, 0) != 0;
    db_finalize(statement); statement = NULL;
    if (result == DB_DONE || (result == DB_ROW && !active)) {
        rollback();
        error(response, result == DB_DONE ? 404 : 400, result == DB_DONE ?
            "Utente non trovato." : "Scegli un utente attivo per abilitare la commessa.");
        free_fields(fields, count); return;
    }
    if (result != DB_ROW) goto database_failure;
    int existing = assignment_snapshot(job_id, user_id, &before);
    if (existing < 0) goto database_failure;
    if (!existing) {
        if (!prepare(&statement, "SELECT COUNT(*) FROM job_assignments WHERE job_id=?") ||
            !bind_id(statement, 1, job_id) || db_step(statement) != DB_ROW)
            goto database_failure;
        DbInt assigned = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
        if (assigned >= 200) {
            rollback(); error(response, 409, "Hai raggiunto il limite di 200 utenti abilitati alla commessa.");
            free_fields(fields, count); return;
        }
    }
    if (!prepare(&statement,
        "INSERT INTO job_assignments(job_id,user_id,starts_on,ends_on) VALUES(?,?,?,?) "
#ifdef PUNTO_MYSQL
        "ON DUPLICATE KEY UPDATE starts_on=VALUES(starts_on),ends_on=VALUES(ends_on)"
#else
        "ON CONFLICT(job_id,user_id) DO UPDATE SET starts_on=excluded.starts_on,ends_on=excluded.ends_on"
#endif
        ) ||
        !bind_id(statement, 1, job_id) || !bind_id(statement, 2, user_id) ||
        !bind_date(statement, 3, starts_on) || !bind_date(statement, 4, ends_on) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (assignment_snapshot(job_id, user_id, &after) != 1 ||
        !storage_audit(database, actor_id, existing ? "assignments.update" : "assignments.create",
                       "assignment", job_id, before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, existing ? 200 : 201, job_id);
    free(before.data); free(after.data); free_fields(fields, count); return;
database_failure:
    db_finalize(statement); rollback();
    free(before.data); free(after.data); free_fields(fields, count); server_error(response);
}

static void delete_assignment(const char *body, DbInt actor_id,
                              DbInt job_id, DbInt user_id,
                              WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La richiesta di revoca non deve contenere campi."); return; }
    if (!begin_transaction(response)) return;
    Json before = {0};
    DbStmt *statement = NULL;
    int found = assignment_snapshot(job_id, user_id, &before);
    if (found < 0) goto database_failure;
    if (!found) {
        rollback(); error(response, 404, "Abilitazione non trovata."); return;
    }
    if (!prepare(&statement, "DELETE FROM job_assignments WHERE job_id=? AND user_id=?") ||
        !bind_id(statement, 1, job_id) || !bind_id(statement, 2, user_id) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (!storage_audit(database, actor_id, "assignments.delete", "assignment", job_id,
                       before.data, NULL)) goto database_failure;
    if (commit_transaction(response)) success(response, 200, 0);
    free(before.data); return;
database_failure:
    db_finalize(statement); rollback(); free(before.data); server_error(response);
}

static bool parse_job_path(const char *path, DbInt *job_id,
                           bool *assignments, DbInt *user_id) {
    const char *suffix = strchr(path, '/');
    if (!suffix) return parse_positive(path, job_id);
    size_t length = (size_t)(suffix - path);
    if (!length || length > 19) return false;
    char identifier[20];
    memcpy(identifier, path, length); identifier[length] = '\0';
    if (!parse_positive(identifier, job_id)) return false;
    if (strcmp(suffix, "/assignments") == 0) { *assignments = true; return true; }
    if (strncmp(suffix, "/assignments/", 13) != 0 ||
        !parse_positive(suffix + 13, user_id)) return false;
    *assignments = true;
    return true;
}

bool workspace_handle(const char *method, const char *target, const char *query,
                      const char *body, DbInt actor_id, const char *owner,
                      const char *role, WorkspaceResponse *response) {
    if (!method || !target || !response) return false;
    bool jobs = strcmp(target, "/api/jobs") == 0;
    bool job_item = strncmp(target, "/api/jobs/", 10) == 0;
    bool entries = strcmp(target, "/api/time-entries") == 0;
    bool entry_item = strncmp(target, "/api/time-entries/", 18) == 0;
    if (!jobs && !job_item && !entries && !entry_item) return false;
    response->status = 0;
    response->body = NULL;
    if (!database) { server_error(response); return true; }
    if (actor_id <= 0 || !owner || !*owner) { error(response, 401, "Accedi per continuare."); return true; }
    bool manager = role && strcmp(role, "manager") == 0;
    bool worker = role && strcmp(role, "worker") == 0;
    if ((!manager && !worker) || (!manager &&
        (((jobs || job_item || entry_item) && strcmp(method, "POST") == 0) ||
         (entry_item && strcmp(method, "DELETE") == 0)))) {
        error(response, 403, "Il tuo ruolo non consente questa operazione."); return true;
    }
    if (!query) query = "";
    if (!body) body = "";
    DbInt id = 0, user_id = 0;
    bool assignments = false;
    if ((job_item && !parse_job_path(target + 10, &id, &assignments, &user_id)) ||
        (entry_item && !parse_positive(target + 18, &id))) {
        error(response, 400, "Identificativo non valido."); return true;
    }
    if (assignments && !manager) {
        error(response, 403, "Il tuo ruolo non consente questa operazione."); return true;
    }
    if (*query && !(entries && strcmp(method, "GET") == 0)) {
        error(response, 400, "Parametri della richiesta non validi."); return true;
    }
    if (assignments) {
        if (!user_id && strcmp(method, "GET") == 0) list_assignments(id, response);
        else if (!user_id && strcmp(method, "POST") == 0) write_assignment(body, actor_id, id, response);
        else if (user_id && strcmp(method, "DELETE") == 0) delete_assignment(body, actor_id, id, user_id, response);
        else error(response, 405, "Metodo non consentito.");
    } else if (jobs && strcmp(method, "GET") == 0) list_jobs(actor_id, manager, response);
    else if ((jobs || job_item) && strcmp(method, "POST") == 0) write_job(body, actor_id, id, response);
    else if (entries && strcmp(method, "GET") == 0) list_entries(query, actor_id, response);
    else if ((entries || entry_item) && strcmp(method, "POST") == 0)
        write_entry(body, actor_id, owner, manager, id, response);
    else if (entry_item && strcmp(method, "DELETE") == 0) delete_entry(body, actor_id, id, response);
    else error(response, 405, "Metodo non consentito.");
    return true;
}
