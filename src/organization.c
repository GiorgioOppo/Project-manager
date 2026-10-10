#include "organization.h"
#include "storage.h"

#include <limits.h>
#include "db.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORM_LIMIT 32768U
#define GROUP_LIMIT 200
#define ROLE_LIMIT 100
#define GROUP_DEPTH 16
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

/* The recursive part is bounded even if an administrator corrupts the graph.
   The sentinel rejects the entire result if any membership path exceeds the
   maximum depth, including cycles. Roles never grant global account powers. */
static bool access_query(Db *db, DbInt job_id, DbInt user_id, const char *today,
                         const char *work_date, OrganizationAccess *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!db || user_id <= 0 || job_id < 0 || !valid_date(today) ||
        (work_date && !valid_date(work_date))) return false;
    if (!work_date) work_date = today;
    static const char sql[] =
        "WITH RECURSIVE user_groups(id,depth) AS ("
        "SELECT m.group_id,1 FROM group_members m "
        "JOIN organization_groups g ON g.id=m.group_id WHERE m.user_id=? "
        "UNION ALL SELECT g.parent_id,u.depth+1 FROM organization_groups g "
        "JOIN user_groups u ON g.id=u.id WHERE g.parent_id IS NOT NULL AND u.depth<17),"
        "grants AS ("
        "SELECT a.job_id,r.can_log_time,a.starts_on,a.ends_on FROM job_assignments a "
        "JOIN job_roles r ON r.id=COALESCE(a.role_id,2) "
        "WHERE a.user_id=? AND (?=0 OR a.job_id=?) "
        "UNION ALL SELECT a.job_id,r.can_log_time,a.starts_on,a.ends_on "
        "FROM job_group_grants a JOIN job_roles r ON r.id=a.role_id "
        "WHERE a.group_id IN (SELECT id FROM user_groups) AND (?=0 OR a.job_id=?)),"
        "effective AS (SELECT job_id,MAX(CASE WHEN "
        "(starts_on IS NULL OR starts_on<=?) AND (ends_on IS NULL OR ends_on>=?) "
        "THEN CASE WHEN can_log_time=1 AND "
        "(starts_on IS NULL OR starts_on<=?) AND (ends_on IS NULL OR ends_on>=?) "
        "THEN 3 ELSE 1 END ELSE 0 END) AS permission FROM grants "
        "WHERE EXISTS(SELECT 1 FROM users WHERE id=? AND active=1) GROUP BY job_id) "
        "SELECT job_id,permission FROM effective WHERE permission>0 "
        "UNION ALL SELECT -1,-1 WHERE EXISTS(SELECT 1 FROM user_groups WHERE depth>16) "
        "ORDER BY job_id LIMIT 1002";
    DbStmt *statement = NULL;
    bool okay = db_prepare_v2(db, sql, -1, &statement, NULL) == DB_OK &&
        bind_id(statement, 1, user_id) && bind_id(statement, 2, user_id) &&
        bind_id(statement, 3, job_id) && bind_id(statement, 4, job_id) &&
        bind_id(statement, 5, job_id) && bind_id(statement, 6, job_id) &&
        bind_text(statement, 7, today) && bind_text(statement, 8, today) &&
        bind_text(statement, 9, work_date) && bind_text(statement, 10, work_date) &&
        bind_id(statement, 11, user_id);
    int result = DB_ERROR;
    while (okay && (result = db_step(statement)) == DB_ROW) {
        DbInt id = db_column_int64(statement, 0);
        int permission = db_column_int(statement, 1);
        if (id <= 0 || (permission != 1 && permission != 3) || out->count >= 1000) {
            okay = false; break;
        }
        out->job_ids[out->count] = id;
        out->permissions[out->count++] = (unsigned char)permission;
    }
    db_finalize(statement);
    if (!okay || result != DB_DONE) { memset(out, 0, sizeof(*out)); return false; }
    return true;
}

bool organization_access_list(Db *db, DbInt user_id, const char *today,
                              const char *work_date, OrganizationAccess *out) {
    return access_query(db, 0, user_id, today, work_date, out);
}

int organization_job_access(Db *db, DbInt job_id, DbInt user_id,
                            const char *today, const char *work_date) {
    if (job_id <= 0) return 0;
    OrganizationAccess access;
    if (!access_query(db, job_id, user_id, today, work_date, &access)) return -1;
    return access.count ? access.permissions[0] : 0;
}

bool organization_open(const char *db_path) {
    organization_close();
    if (!db_path || !*db_path || db_open_v2(db_path, &database,
        DB_OPEN_READWRITE | DB_OPEN_FULLMUTEX, NULL) != DB_OK ||
        db_busy_timeout(database, 5000) != DB_OK
#ifndef PUNTO_MYSQL
        || db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != DB_OK
#endif
    ) { organization_close(); return false; }
    return true;
}

void organization_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
}

static bool nullable_id(DbStmt *statement, int index, DbInt id) {
    return id ? bind_id(statement, index, id) : db_bind_null(statement, index) == DB_OK;
}

static bool bind_date(DbStmt *statement, int index, const char *date) {
    return date && *date ? bind_text(statement, index, date) :
        db_bind_null(statement, index) == DB_OK;
}

static void success(WorkspaceResponse *response, int status, DbInt id) {
    Json json = {0};
    append(&json, "{\"ok\":true,\"id\":"); append_integer(&json, id); append(&json, "}");
    finish_json(response, status, &json);
}

static void group_json(Json *json, DbStmt *statement) {
    append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"name\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"parent_id\":");
    if (db_column_type(statement, 2) == DB_NULL) append(json, "null");
    else append_integer(json, db_column_int64(statement, 2));
    append(json, ",\"members_count\":"); append_integer(json, db_column_int64(statement, 3));
    append(json, ",\"children_count\":"); append_integer(json, db_column_int64(statement, 4));
    append(json, "}");
}

static void role_json(Json *json, DbStmt *statement) {
    append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"code\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"name\":"); append_string(json, column_text(statement, 2));
    append(json, ",\"can_log_time\":"); append(json, db_column_int(statement, 3) ? "true" : "false");
    append(json, ",\"builtin\":"); append(json, db_column_int(statement, 4) ? "true" : "false");
    append(json, "}");
}

static void member_json(Json *json, DbStmt *statement) {
    append(json, "{\"user_id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"name\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"email\":"); append_string(json, column_text(statement, 2));
    append(json, ",\"role\":"); append_string(json, column_text(statement, 3));
    append(json, ",\"active\":"); append(json, db_column_int(statement, 4) ? "true" : "false");
    append(json, "}");
}

static void grant_json(Json *json, DbStmt *statement) {
    append(json, "{\"group_id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"group_name\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"role_id\":"); append_integer(json, db_column_int64(statement, 2));
    append(json, ",\"role_code\":"); append_string(json, column_text(statement, 3));
    append(json, ",\"role_name\":"); append_string(json, column_text(statement, 4));
    append(json, ",\"can_log_time\":"); append(json, db_column_int(statement, 5) ? "true" : "false");
    append(json, ",\"starts_on\":"); append_string(json, column_text(statement, 6));
    append(json, ",\"ends_on\":"); append_string(json, column_text(statement, 7));
    append(json, "}");
}

static const char group_select[] =
    "SELECT g.id,g.name,g.parent_id,"
    "(SELECT COUNT(*) FROM group_members m WHERE m.group_id=g.id),"
    "(SELECT COUNT(*) FROM organization_groups c WHERE c.parent_id=g.id) "
    "FROM organization_groups g ";
static const char role_select[] = "SELECT id,code,name,can_log_time,builtin FROM job_roles ";
static const char member_select[] =
    "SELECT u.id,u.name,u.email,u.role,u.active FROM group_members m "
    "JOIN users u ON u.id=m.user_id ";
static const char grant_select[] =
    "SELECT a.group_id,g.name,a.role_id,r.code,r.name,r.can_log_time,a.starts_on,a.ends_on "
    "FROM job_group_grants a JOIN organization_groups g ON g.id=a.group_id "
    "JOIN job_roles r ON r.id=a.role_id ";

typedef void (*JsonRow)(Json *, DbStmt *);

static int snapshot(const char *base, const char *where, DbInt first, DbInt second,
                    JsonRow row, Json *json) {
    char sql[1024];
    int length = snprintf(sql, sizeof(sql), "%s%s", base, where);
    if (length < 0 || (size_t)length >= sizeof(sql)) return -1;
    DbStmt *statement = NULL;
    if (!prepare(&statement, sql) || !bind_id(statement, 1, first) ||
        (second && !bind_id(statement, 2, second))) { db_finalize(statement); return -1; }
    int result = db_step(statement);
    if (result == DB_ROW) row(json, statement);
    db_finalize(statement);
    return json->failed ? -1 : result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static int exists(const char *table, DbInt id) {
    char sql[128];
    (void)snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE id=?", table);
    DbStmt *statement = NULL;
    if (!prepare(&statement, sql) || !bind_id(statement, 1, id)) { db_finalize(statement); return -1; }
    int result = db_step(statement);
    db_finalize(statement);
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static bool require_target(const char *table, DbInt id, const char *message,
                            WorkspaceResponse *response) {
    int found = exists(table, id);
    if (found == 1) return true;
    if (found < 0) server_error(response); else error(response, 404, message);
    return false;
}

static void list_rows(const char *base, const char *suffix, const char *label,
                      DbInt id, int limit, JsonRow row, WorkspaceResponse *response) {
    char sql[1024];
    int length = snprintf(sql, sizeof(sql), "%s%s LIMIT %d", base, suffix, limit + 1);
    DbStmt *statement = NULL;
    if (length < 0 || (size_t)length >= sizeof(sql) || !prepare(&statement, sql) ||
        (id && !bind_id(statement, 1, id))) { db_finalize(statement); server_error(response); return; }
    Json json = {0};
    append(&json, "{");
    if (id) {
        append(&json, strcmp(label, "members") == 0 ? "\"group_id\":" : "\"job_id\":");
        append_integer(&json, id); append(&json, ",");
    }
    append_string(&json, label); append(&json, ":[");
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > limit) break;
        if (count > 1) append(&json, ",");
        row(&json, statement);
    }
    db_finalize(statement);
    if (count > limit || result != DB_DONE) {
        free(json.data);
        if (count > limit) error(response, 409, "Troppi risultati. Riduci gli elementi prima di proseguire.");
        else server_error(response);
        return;
    }
    append(&json, "]}"); finish_json(response, 200, &json);
}

/* Validate every node after a reparent, not just the moved node: moving a
   shallow root can otherwise push an existing descendant past level 16. */
static int valid_group_graph(DbInt edited_id, DbInt parent_id) {
    struct { DbInt id, parent; } groups[GROUP_LIMIT + 1];
    size_t count = 0;
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,parent_id FROM organization_groups ORDER BY id LIMIT 201")) return -1;
    int result;
    while ((result = db_step(statement)) == DB_ROW) {
        if (count >= GROUP_LIMIT) { db_finalize(statement); return -1; }
        groups[count].id = db_column_int64(statement, 0);
        groups[count].parent = db_column_int64(statement, 1);
        if (groups[count].id == edited_id) groups[count].parent = parent_id;
        count++;
    }
    db_finalize(statement);
    if (result != DB_DONE) return -1;
    if (!edited_id) {
        if (count >= GROUP_LIMIT) return 2;
        groups[count].id = -1; groups[count++].parent = parent_id;
    }
    for (size_t i = 0; i < count; i++) {
        DbInt current = groups[i].id;
        size_t depth = 0;
        while (current) {
            if (++depth > GROUP_DEPTH) return 0;
            size_t at = 0;
            while (at < count && groups[at].id != current) at++;
            if (at == count) return 0;
            current = groups[at].parent;
        }
    }
    return 1;
}

static void write_group(const char *body, DbInt actor_id, DbInt id,
                        WorkspaceResponse *response) {
    Field fields[] = {{"name", NULL}, {"parent_id", NULL}};
    if (!parse_fields(body, fields, 2, response)) return;
    trim(fields[0].value);
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    char *old_name = NULL;
    DbInt parent_id = 0;
    if (!begin_transaction(response)) goto cleanup;
    if (id) {
        int found = snapshot(group_select, "WHERE g.id=?", id, 0, group_json, &before);
        if (found < 0) goto database_failure;
        if (!found) { rollback(); error(response, 404, "Gruppo non trovato."); goto cleanup; }
        if (!prepare(&statement, "SELECT name,parent_id FROM organization_groups WHERE id=?") ||
            !bind_id(statement, 1, id) || db_step(statement) != DB_ROW) goto database_failure;
        old_name = duplicate(column_text(statement, 0)); parent_id = db_column_int64(statement, 1);
        db_finalize(statement); statement = NULL;
        if (!old_name) goto database_failure;
    }
    const char *name = fields[0].value ? fields[0].value : old_name ? old_name : "";
    if (!valid_text(name, 120, true, false) ||
        (fields[1].value && *fields[1].value && !parse_positive(fields[1].value, &parent_id))) {
        rollback(); error(response, 400, "Inserisci un nome valido e un gruppo superiore esistente."); goto cleanup;
    }
    if (fields[1].value && !*fields[1].value) parent_id = 0;
    if (parent_id && !require_target("organization_groups", parent_id, "Gruppo superiore non trovato.", response)) {
        rollback(); goto cleanup;
    }
    int graph = valid_group_graph(id, parent_id);
    if (graph < 0) goto database_failure;
    if (graph != 1) {
        rollback(); error(response, graph == 2 ? 409 : 400, graph == 2 ?
            "Hai raggiunto il limite di 200 gruppi." : "Il gruppo creerebbe un ciclo o supererebbe i 16 livelli.");
        goto cleanup;
    }
    if (!prepare(&statement, id ? "UPDATE organization_groups SET name=?,parent_id=? WHERE id=?" :
        "INSERT INTO organization_groups(name,parent_id) VALUES(?,?)") ||
        !bind_text(statement, 1, name) || !nullable_id(statement, 2, parent_id) ||
        (id && !bind_id(statement, 3, id)) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    DbInt saved = id ? id : db_last_insert_rowid(database);
    if (snapshot(group_select, "WHERE g.id=?", saved, 0, group_json, &after) != 1 ||
        !storage_audit(database, actor_id, id ? "groups.update" : "groups.create", "group", saved,
                       before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, id ? 200 : 201, saved);
    goto cleanup;
database_failure:
    db_finalize(statement); statement = NULL; rollback(); server_error(response);
cleanup:
    free(old_name); free(before.data); free(after.data); free_fields(fields, 2);
}

static void delete_group(const char *body, DbInt actor_id, DbInt id,
                         WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La richiesta di eliminazione non deve contenere campi."); return; }
    if (!begin_transaction(response)) return;
    Json before = {0};
    DbStmt *statement = NULL;
    int found = snapshot(group_select, "WHERE g.id=?", id, 0, group_json, &before);
    if (found < 0) goto database_failure;
    if (!found) { rollback(); error(response, 404, "Gruppo non trovato."); goto cleanup; }
    if (!prepare(&statement,
        "SELECT (SELECT COUNT(*) FROM organization_groups WHERE parent_id=?) + "
        "(SELECT COUNT(*) FROM group_members WHERE group_id=?) + "
        "(SELECT COUNT(*) FROM job_group_grants WHERE group_id=?)") ||
        !bind_id(statement, 1, id) || !bind_id(statement, 2, id) || !bind_id(statement, 3, id) ||
        db_step(statement) != DB_ROW) goto database_failure;
    bool busy = db_column_int64(statement, 0) != 0;
    db_finalize(statement); statement = NULL;
    if (busy) { rollback(); error(response, 409, "Rimuovi prima sottogruppi, membri e abilitazioni del gruppo."); goto cleanup; }
    if (!prepare(&statement, "DELETE FROM organization_groups WHERE id=?") ||
        !bind_id(statement, 1, id) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (!storage_audit(database, actor_id, "groups.delete", "group", id, before.data, NULL)) goto database_failure;
    if (commit_transaction(response)) success(response, 200, id);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data);
}

static void write_member(const char *body, DbInt actor_id, DbInt group_id,
                         WorkspaceResponse *response) {
    Field field = {"user_id", NULL};
    if (!parse_fields(body, &field, 1, response)) return;
    DbInt user_id;
    if (!parse_positive(field_value(&field), &user_id)) {
        error(response, 400, "Seleziona un utente valido."); free_fields(&field, 1); return;
    }
    if (!begin_transaction(response)) { free_fields(&field, 1); return; }
    Json after = {0};
    DbStmt *statement = NULL;
    if (!require_target("organization_groups", group_id, "Gruppo non trovato.", response)) {
        rollback(); goto cleanup;
    }
    if (!prepare(&statement, "SELECT active FROM users WHERE id=?") || !bind_id(statement, 1, user_id)) goto database_failure;
    int result = db_step(statement);
    bool active = result == DB_ROW && db_column_int(statement, 0) == 1;
    db_finalize(statement); statement = NULL;
    if (result == DB_DONE || (result == DB_ROW && !active)) {
        rollback(); error(response, result == DB_DONE ? 404 : 400, result == DB_DONE ?
            "Utente non trovato." : "Seleziona un utente attivo."); goto cleanup;
    }
    if (result != DB_ROW) goto database_failure;
    int found = snapshot(member_select, "WHERE m.group_id=? AND m.user_id=?", group_id, user_id, member_json, &after);
    if (found < 0) goto database_failure;
    if (found) { rollback(); success(response, 200, group_id); goto cleanup; }
    if (!prepare(&statement, "SELECT COUNT(*) FROM group_members WHERE group_id=?") ||
        !bind_id(statement, 1, group_id) || db_step(statement) != DB_ROW) goto database_failure;
    DbInt count = db_column_int64(statement, 0);
    db_finalize(statement); statement = NULL;
    if (count >= 200) { rollback(); error(response, 409, "Hai raggiunto il limite di 200 membri del gruppo."); goto cleanup; }
    if (!prepare(&statement, "INSERT INTO group_members(group_id,user_id) VALUES(?,?)") ||
        !bind_id(statement, 1, group_id) || !bind_id(statement, 2, user_id) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    append(&after, "{\"group_id\":"); append_integer(&after, group_id);
    append(&after, ",\"user_id\":"); append_integer(&after, user_id); append(&after, "}");
    if (after.failed || !storage_audit(database, actor_id, "group_members.create", "group_member", group_id,
                                     NULL, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, 201, group_id);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(after.data); free_fields(&field, 1);
}

static void delete_member(const char *body, DbInt actor_id, DbInt group_id,
                          DbInt user_id, WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La richiesta di eliminazione non deve contenere campi."); return; }
    if (!begin_transaction(response)) return;
    Json before = {0};
    DbStmt *statement = NULL;
    if (!require_target("organization_groups", group_id, "Gruppo non trovato.", response)) {
        rollback(); return;
    }
    int found = snapshot(member_select, "WHERE m.group_id=? AND m.user_id=?", group_id, user_id, member_json, &before);
    if (found < 0) goto database_failure;
    if (!found) { rollback(); error(response, 404, "Appartenenza al gruppo non trovata."); goto cleanup; }
    if (!prepare(&statement, "DELETE FROM group_members WHERE group_id=? AND user_id=?") ||
        !bind_id(statement, 1, group_id) || !bind_id(statement, 2, user_id) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    /* Keep both keys in the audit record: entity_id alone identifies the group. */
    free(before.data); before = (Json){0};
    append(&before, "{\"group_id\":"); append_integer(&before, group_id);
    append(&before, ",\"user_id\":"); append_integer(&before, user_id); append(&before, "}");
    if (before.failed || !storage_audit(database, actor_id, "group_members.delete", "group_member", group_id,
                                      before.data, NULL)) goto database_failure;
    if (commit_transaction(response)) success(response, 200, group_id);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data);
}

static bool valid_role_code(const char *code) {
    size_t length = strlen(code);
    if (length < 1 || length > 40 || code[0] < 'a' || code[0] > 'z') return false;
    for (size_t i = 1; i < length; i++)
        if ((code[i] < 'a' || code[i] > 'z') && (code[i] < '0' || code[i] > '9') &&
            code[i] != '_' && code[i] != '-') return false;
    return true;
}

static void write_role(const char *body, DbInt actor_id, DbInt id,
                       WorkspaceResponse *response) {
    Field fields[] = {{"code", NULL}, {"name", NULL}, {"can_log_time", NULL}};
    if (!parse_fields(body, fields, 3, response)) return;
    trim(fields[0].value); trim(fields[1].value);
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    char *old_code = NULL, *old_name = NULL;
    int can_log = 0;
    if (!begin_transaction(response)) goto cleanup;
    if (id) {
        int found = snapshot(role_select, "WHERE id=?", id, 0, role_json, &before);
        if (found < 0) goto database_failure;
        if (!found) { rollback(); error(response, 404, "Ruolo non trovato."); goto cleanup; }
        if (!prepare(&statement, "SELECT code,name,can_log_time,builtin FROM job_roles WHERE id=?") ||
            !bind_id(statement, 1, id) || db_step(statement) != DB_ROW) goto database_failure;
        bool builtin = db_column_int(statement, 3) != 0;
        if (!builtin) {
            old_code = duplicate(column_text(statement, 0)); old_name = duplicate(column_text(statement, 1));
            can_log = db_column_int(statement, 2);
        }
        db_finalize(statement); statement = NULL;
        if (builtin) { rollback(); error(response, 409, "I ruoli predefiniti non possono essere modificati."); goto cleanup; }
        if (!old_code || !old_name) goto database_failure;
    } else {
        if (!prepare(&statement, "SELECT COUNT(*) FROM job_roles") || db_step(statement) != DB_ROW) goto database_failure;
        DbInt count = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
        if (count >= ROLE_LIMIT) { rollback(); error(response, 409, "Hai raggiunto il limite di 100 ruoli."); goto cleanup; }
    }
    const char *code = fields[0].value ? fields[0].value : old_code ? old_code : "";
    const char *name = fields[1].value ? fields[1].value : old_name ? old_name : "";
    if (fields[2].value) {
        if (strcmp(fields[2].value, "0") == 0) can_log = 0;
        else if (strcmp(fields[2].value, "1") == 0) can_log = 1;
        else { rollback(); error(response, 400, "Scegli se il ruolo può registrare ore (0 o 1)."); goto cleanup; }
    }
    if (!valid_role_code(code) || !valid_text(name, 120, true, false) || (!id && !fields[2].value)) {
        rollback(); error(response, 400, "Inserisci codice, nome e permesso di registrazione ore validi."); goto cleanup;
    }
    if (!prepare(&statement, id ? "UPDATE job_roles SET code=?,name=?,can_log_time=? WHERE id=?" :
        "INSERT INTO job_roles(code,name,can_log_time,builtin) VALUES(?,?,?,0)") ||
        !bind_text(statement, 1, code) || !bind_text(statement, 2, name) || !bind_id(statement, 3, can_log) ||
        (id && !bind_id(statement, 4, id))) goto database_failure;
    int result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) {
        bool duplicate_code = db_extended_errcode(database) == DB_CONSTRAINT_UNIQUE;
        rollback();
        if (duplicate_code) error(response, 409, "Esiste già un ruolo con questo codice."); else server_error(response);
        goto cleanup;
    }
    DbInt saved = id ? id : db_last_insert_rowid(database);
    if (snapshot(role_select, "WHERE id=?", saved, 0, role_json, &after) != 1 ||
        !storage_audit(database, actor_id, id ? "job_roles.update" : "job_roles.create", "job_role", saved,
                       before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, id ? 200 : 201, saved);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(old_code); free(old_name); free(before.data); free(after.data); free_fields(fields, 3);
}

static void write_grant(const char *body, DbInt actor_id, DbInt job_id,
                        WorkspaceResponse *response) {
    Field fields[] = {{"group_id", NULL}, {"role_id", NULL}, {"starts_on", NULL}, {"ends_on", NULL}};
    if (!parse_fields(body, fields, 4, response)) return;
    DbInt group_id, role_id;
    if (!parse_positive(field_value(&fields[0]), &group_id) ||
        !parse_positive(field_value(&fields[1]), &role_id)) {
        error(response, 400, "Seleziona un gruppo e un ruolo validi."); free_fields(fields, 4); return;
    }
    if (!begin_transaction(response)) { free_fields(fields, 4); return; }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    char old_start[11] = "", old_end[11] = "";
    if (!require_target("jobs", job_id, "Commessa non trovata.", response) ||
        !require_target("organization_groups", group_id, "Gruppo non trovato.", response) ||
        !require_target("job_roles", role_id, "Ruolo non trovato.", response)) { rollback(); goto cleanup; }
    int existing = snapshot(grant_select, "WHERE a.job_id=? AND a.group_id=?", job_id, group_id, grant_json, &before);
    if (existing < 0) goto database_failure;
    if (existing) {
        if (!prepare(&statement, "SELECT starts_on,ends_on FROM job_group_grants WHERE job_id=? AND group_id=?") ||
            !bind_id(statement, 1, job_id) || !bind_id(statement, 2, group_id) || db_step(statement) != DB_ROW) goto database_failure;
        const char *start = column_text(statement, 0), *end = column_text(statement, 1);
        if (strlen(start) > 10 || strlen(end) > 10) goto database_failure;
        strcpy(old_start, start); strcpy(old_end, end);
        db_finalize(statement); statement = NULL;
    }
    const char *start = fields[2].value ? fields[2].value : old_start;
    const char *end = fields[3].value ? fields[3].value : old_end;
    if ((*start && !valid_date(start)) || (*end && !valid_date(end)) ||
        (*start && *end && strcmp(start, end) > 0)) {
        rollback(); error(response, 400, "Scegli un intervallo di abilitazione valido."); goto cleanup;
    }
    if (!prepare(&statement,
        "INSERT INTO job_group_grants(job_id,group_id,role_id,starts_on,ends_on) VALUES(?,?,?,?,?) "
#ifdef PUNTO_MYSQL
        "ON DUPLICATE KEY UPDATE role_id=VALUES(role_id),starts_on=VALUES(starts_on),ends_on=VALUES(ends_on)"
#else
        "ON CONFLICT(job_id,group_id) DO UPDATE SET role_id=excluded.role_id,starts_on=excluded.starts_on,ends_on=excluded.ends_on"
#endif
    ) || !bind_id(statement, 1, job_id) || !bind_id(statement, 2, group_id) ||
        !bind_id(statement, 3, role_id) || !bind_date(statement, 4, start) || !bind_date(statement, 5, end) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (snapshot(grant_select, "WHERE a.job_id=? AND a.group_id=?", job_id, group_id, grant_json, &after) != 1 ||
        !storage_audit(database, actor_id, existing ? "group_grants.update" : "group_grants.create", "group_grant", job_id,
                       before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) success(response, existing ? 200 : 201, job_id);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data); free(after.data); free_fields(fields, 4);
}

static void delete_grant(const char *body, DbInt actor_id, DbInt job_id,
                         DbInt group_id, WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La richiesta di eliminazione non deve contenere campi."); return; }
    if (!begin_transaction(response)) return;
    Json before = {0};
    DbStmt *statement = NULL;
    if (!require_target("jobs", job_id, "Commessa non trovata.", response)) { rollback(); return; }
    int found = snapshot(grant_select, "WHERE a.job_id=? AND a.group_id=?", job_id, group_id, grant_json, &before);
    if (found < 0) goto database_failure;
    if (!found) { rollback(); error(response, 404, "Abilitazione del gruppo non trovata."); goto cleanup; }
    if (!prepare(&statement, "DELETE FROM job_group_grants WHERE job_id=? AND group_id=?") ||
        !bind_id(statement, 1, job_id) || !bind_id(statement, 2, group_id) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (!storage_audit(database, actor_id, "group_grants.delete", "group_grant", job_id, before.data, NULL)) goto database_failure;
    if (commit_transaction(response)) success(response, 200, job_id);
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data);
}

static bool parse_item(const char *text, DbInt *id, const char **suffix) {
    const char *end = strchr(text, '/');
    size_t length = end ? (size_t)(end - text) : strlen(text);
    if (!length || length >= 32) return false;
    char number[32]; memcpy(number, text, length); number[length] = '\0';
    if (!parse_positive(number, id)) return false;
    *suffix = text + length;
    return true;
}

bool organization_handle(const char *method, const char *target, const char *query,
                         const char *body, DbInt actor_id, const char *role,
                         WorkspaceResponse *response) {
    if (!method || !target || !response) return false;
    bool groups = strcmp(target, "/api/groups") == 0;
    bool group_item = strncmp(target, "/api/groups/", 12) == 0;
    bool roles = strcmp(target, "/api/job-roles") == 0;
    bool role_item = strncmp(target, "/api/job-roles/", 15) == 0;
    bool job_item = strncmp(target, "/api/jobs/", 10) == 0;
    DbInt id = 0, item_id = 0;
    const char *suffix = "";
    if (job_item) {
        if (!parse_item(target + 10, &id, &suffix)) return false;
        if (strcmp(suffix, "/group-grants") != 0 && strncmp(suffix, "/group-grants/", 14) != 0) return false;
    }
    if (!groups && !group_item && !roles && !role_item && !job_item) return false;
    response->status = 0; response->body = NULL;
    if (actor_id <= 0) { error(response, 401, "Accedi per continuare."); return true; }
    if (!role || strcmp(role, "manager") != 0) { error(response, 403, "Solo i gestori possono modificare gruppi, ruoli e abilitazioni."); return true; }
    if (!database) { server_error(response); return true; }
    if (!query) query = "";
    if (!body) body = "";
    if (*query) { error(response, 400, "Parametri della richiesta non validi."); return true; }
    if ((group_item && !parse_item(target + 12, &id, &suffix)) ||
        (role_item && !parse_item(target + 15, &id, &suffix))) {
        error(response, 400, "Identificativo non valido."); return true;
    }
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    bool remove = strcmp(method, "DELETE") == 0;
    if (groups && get) list_rows(group_select, "ORDER BY g.id", "groups", 0, GROUP_LIMIT, group_json, response);
    else if (groups && post) write_group(body, actor_id, 0, response);
    else if (roles && get) list_rows(role_select, "ORDER BY id", "roles", 0, ROLE_LIMIT, role_json, response);
    else if (roles && post) write_role(body, actor_id, 0, response);
    else if (role_item && !*suffix && post) write_role(body, actor_id, id, response);
    else if (group_item && !*suffix && post) write_group(body, actor_id, id, response);
    else if (group_item && !*suffix && remove) delete_group(body, actor_id, id, response);
    else if (group_item && strcmp(suffix, "/members") == 0 && get) {
        if (require_target("organization_groups", id, "Gruppo non trovato.", response))
            list_rows(member_select, "WHERE m.group_id=? ORDER BY u.id", "members", id, 200, member_json, response);
    } else if (group_item && strcmp(suffix, "/members") == 0 && post) write_member(body, actor_id, id, response);
    else if (group_item && strncmp(suffix, "/members/", 9) == 0 && remove) {
        if (!parse_positive(suffix + 9, &item_id)) error(response, 400, "Identificativo utente non valido.");
        else delete_member(body, actor_id, id, item_id, response);
    } else if (job_item && strcmp(suffix, "/group-grants") == 0 && get) {
        if (require_target("jobs", id, "Commessa non trovata.", response))
            list_rows(grant_select, "WHERE a.job_id=? ORDER BY a.group_id", "grants", id, GROUP_LIMIT, grant_json, response);
    } else if (job_item && strcmp(suffix, "/group-grants") == 0 && post) write_grant(body, actor_id, id, response);
    else if (job_item && strncmp(suffix, "/group-grants/", 14) == 0 && remove) {
        if (!parse_positive(suffix + 14, &item_id)) error(response, 400, "Identificativo gruppo non valido.");
        else delete_grant(body, actor_id, id, item_id, response);
    } else error(response, 405, "Metodo non consentito.");
    return true;
}
