#include "work_board.h"
#include "organization.h"
#include "storage.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORM_LIMIT 65536U
#define ROW_LIMIT 500
#define MAX_DEPTH 8

typedef struct { const char *name; char *value; } Field;
static Db *database;

static void error(WorkspaceResponse *response, int status, const char *message) {
    struct json_object *out = json_object_new_object();
    if (!out) { response->status = 500; response->body = strdup("{\"ok\":false}"); return; }
    json_object_object_add(out, "ok", json_object_new_boolean(false));
    json_object_object_add(out, "error", json_object_new_string(message));
    response->body = strdup(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
    response->status = response->body ? status : 500;
    json_object_put(out);
}

static void server_error(WorkspaceResponse *response) {
    error(response, 500, "Errore del server. Riprova.");
}

static void finish(WorkspaceResponse *response, int status, struct json_object *out) {
    if (!out) { server_error(response); return; }
    response->body = strdup(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
    response->status = response->body ? status : 500;
    json_object_put(out);
}

static bool prepare(DbStmt **statement, const char *sql) {
    return db_prepare_v2(database, sql, -1, statement, NULL) == DB_OK;
}
static bool bind_text(DbStmt *statement, int index, const char *text) {
    return db_bind_text(statement, index, text, -1, DB_TRANSIENT) == DB_OK;
}
static bool bind_id(DbStmt *statement, int index, DbInt id) {
    return db_bind_int64(statement, index, id) == DB_OK;
}
static bool nullable_id(DbStmt *statement, int index, DbInt id) {
    return id ? bind_id(statement, index, id) : db_bind_null(statement, index) == DB_OK;
}
static bool nullable_text(DbStmt *statement, int index, const char *text) {
    return text && *text ? bind_text(statement, index, text) : db_bind_null(statement, index) == DB_OK;
}
static const char *column_text(DbStmt *statement, int index) {
    const unsigned char *text = db_column_text(statement, index);
    return text ? (const char *)text : "";
}
static void rollback(void) { (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL); }
static bool begin(WorkspaceResponse *response) {
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == DB_OK) return true;
    server_error(response); return false;
}
static bool commit(WorkspaceResponse *response) {
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
    rollback(); server_error(response); return false;
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


static DbInt object_id(struct json_object *object, const char *key) {
    struct json_object *value = NULL;
    (void)json_object_object_get_ex(object, key, &value);
    return value ? (DbInt)json_object_get_int64(value) : 0;
}
static const char *object_text(struct json_object *object, const char *key) {
    struct json_object *value = NULL;
    (void)json_object_object_get_ex(object, key, &value);
    return value ? json_object_get_string(value) : "";
}
static bool today_date(char date[11]) {
    time_t now = time(NULL); struct tm local;
    return now != (time_t)-1 && localtime_r(&now, &local) &&
        strftime(date, 11, "%Y-%m-%d", &local) == 10;
}
static bool timestamp(char stamp[21]) {
    time_t now = time(NULL); struct tm utc;
    return now != (time_t)-1 && gmtime_r(&now, &utc) &&
        strftime(stamp, 21, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
}
static bool active_actor(DbInt actor_id, bool *manager, WorkspaceResponse *response) {
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT role,active FROM users WHERE id=?") && bind_id(statement, 1, actor_id);
    int result = okay ? db_step(statement) : DB_ERROR;
    if (result == DB_ROW && db_column_int(statement, 1) == 1)
        *manager = strcmp(column_text(statement, 0), "manager") == 0;
    else {
        if (result == DB_DONE || result == DB_ROW) error(response, 401, "Accedi per continuare.");
        else server_error(response);
        okay = false;
    }
    db_finalize(statement); return okay;
}
static int job_permission(DbInt job_id, DbInt actor_id, bool manager,
                          const char *today, WorkspaceResponse *response) {
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT id FROM jobs WHERE id=?") && bind_id(statement, 1, job_id);
    int result = okay ? db_step(statement) : DB_ERROR;
    db_finalize(statement);
    if (result == DB_DONE) { error(response, 404, "Elemento non trovato."); return 0; }
    if (result != DB_ROW) { server_error(response); return 0; }
    int permission = manager ? 3 : organization_job_access(database, job_id, actor_id, today, NULL);
    if (permission < 0) server_error(response);
    else if (!permission) error(response, 404, "Elemento non trovato.");
    return permission > 0 ? permission : 0;
}
static bool eligible_user(DbInt job_id, DbInt user_id, const char *today,
                          WorkspaceResponse *response) {
    if (!user_id) return true;
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT role,active FROM users WHERE id=?") && bind_id(statement, 1, user_id);
    int result = okay ? db_step(statement) : DB_ERROR;
    bool active = result == DB_ROW && db_column_int(statement, 1) == 1;
    bool manager = active && strcmp(column_text(statement, 0), "manager") == 0;
    db_finalize(statement);
    if (result != DB_ROW && result != DB_DONE) { server_error(response); return false; }
    int access = active ? (manager ? 3 : organization_job_access(database, job_id, user_id, today, NULL)) : 0;
    if (access < 0) { server_error(response); return false; }
    if (access != 3) { error(response, 400, "Seleziona un utente attivo abilitato a lavorare sulla commessa."); return false; }
    return true;
}

static const char task_select[] =
    "SELECT t.id,t.job_id,t.parent_id,t.assigned_user_id,u.name,t.title,t.description,t.status,t.priority,"
    "t.due_date,t.revision,t.created_at,t.updated_at FROM job_tasks t LEFT JOIN users u ON u.id=t.assigned_user_id ";
static const char blocker_select[] =
    "SELECT b.id,b.job_id,b.task_id,b.title,b.description,b.impact,b.owner_id,u.name,b.status,b.next_action,"
    "b.due_date,b.created_by,b.revision,b.created_at,b.updated_at FROM job_blockers b LEFT JOIN users u ON u.id=b.owner_id ";
static struct json_object *row_json(DbStmt *statement, bool task) {
    static const char *task_keys[] = {"id","job_id","parent_id","assigned_user_id","assigned_user_name","title",
        "description","status","priority","due_date","revision","created_at","updated_at"};
    static const char *blocker_keys[] = {"id","job_id","task_id","title","description","impact","owner_id","owner_name",
        "status","next_action","due_date","created_by","revision","created_at","updated_at"};
    struct json_object *out = json_object_new_object();
    if (!out) return NULL;
    int count = task ? 13 : 15;
    for (int i = 0; i < count; i++) {
        bool integer = task ? (i <= 3 || i == 10) : (i <= 2 || i == 6 || i == 11 || i == 12);
        struct json_object *value = NULL;
        if (db_column_type(statement, i) != DB_NULL) {
            value = integer ? json_object_new_int64(db_column_int64(statement, i)) :
                json_object_new_string(column_text(statement, i));
            if (!value) { json_object_put(out); return NULL; }
        }
        if (json_object_object_add(out, task ? task_keys[i] : blocker_keys[i], value) != 0) {
            json_object_put(value); json_object_put(out); return NULL;
        }
    }
    return out;
}
static struct json_object *load_record(bool task, DbInt id, WorkspaceResponse *response) {
    char sql[768]; DbStmt *statement = NULL;
    (void)snprintf(sql, sizeof(sql), "%s WHERE %s.id=?", task ? task_select : blocker_select, task ? "t" : "b");
    bool okay = prepare(&statement, sql) && bind_id(statement, 1, id);
    int result = okay ? db_step(statement) : DB_ERROR;
    struct json_object *out = result == DB_ROW ? row_json(statement, task) : NULL;
    if (result == DB_DONE) error(response, 404, "Elemento non trovato.");
    else if (!out) server_error(response);
    db_finalize(statement); return out;
}
static bool list_records(struct json_object *out, bool task, DbInt job_id,
                         DbInt actor_id, bool manager, int permission,
                         WorkspaceResponse *response) {
    char sql[768]; DbStmt *statement = NULL;
    (void)snprintf(sql, sizeof(sql), "%s WHERE %s.job_id=? ORDER BY %s.id LIMIT 501",
        task ? task_select : blocker_select, task ? "t" : "b", task ? "t" : "b");
    struct json_object *rows = json_object_new_array();
    if (!rows) { server_error(response); return false; }
    bool okay = prepare(&statement, sql) && bind_id(statement, 1, job_id);
    int result = DB_ERROR; size_t count = 0;
    while (okay && (result = db_step(statement)) == DB_ROW) {
        if (++count > ROW_LIMIT) { okay = false; break; }
        struct json_object *row = row_json(statement, task);
        if (!row) { okay = false; break; }
        bool permitted = manager || (permission == 3 && (task ? object_id(row, "assigned_user_id") == actor_id :
            object_id(row, "created_by") == actor_id || object_id(row, "owner_id") == actor_id));
        json_object_object_add(row, "can_edit", json_object_new_boolean(task ? manager : permitted));
        if (task) json_object_object_add(row, "can_update_status", json_object_new_boolean(permitted));
        if (json_object_array_add(rows, row) != 0) { json_object_put(row); okay = false; }
    }
    db_finalize(statement);
    if (!okay || result != DB_DONE) { json_object_put(rows); server_error(response); return false; }
    json_object_object_add(out, task ? "tasks" : "blockers", rows);
    return true;
}
static bool list_assignees(struct json_object *out, DbInt job_id, bool manager,
                           const char *today, WorkspaceResponse *response) {
    struct json_object *rows = json_object_new_array();
    if (!rows) { server_error(response); return false; }
    if (!manager) { json_object_object_add(out, "assignees", rows); return true; }
    /* Materialize candidates before running grant queries on this connection. */
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT id,name,role FROM users WHERE active=1 ORDER BY name,id LIMIT 1001");
    struct json_object *candidates = json_object_new_array();
    int result = DB_ERROR; size_t count = 0;
    while (okay && candidates && (result = db_step(statement)) == DB_ROW) {
        if (++count > 1000) { okay = false; break; }
        struct json_object *item = json_object_new_object();
        if (!item) { okay = false; break; }
        json_object_object_add(item, "id", json_object_new_int64(db_column_int64(statement, 0)));
        json_object_object_add(item, "name", json_object_new_string(column_text(statement, 1)));
        json_object_object_add(item, "manager", json_object_new_boolean(strcmp(column_text(statement, 2), "manager") == 0));
        if (json_object_array_add(candidates, item) != 0) { json_object_put(item); okay = false; }
    }
    db_finalize(statement);
    if (!okay || !candidates || result != DB_DONE) goto failure;
    for (size_t i = 0; i < json_object_array_length(candidates); i++) {
        struct json_object *item = json_object_array_get_idx(candidates, i);
        int access = object_id(item, "manager") ? 3 : organization_job_access(database, job_id, object_id(item, "id"), today, NULL);
        if (access < 0) goto failure;
        if (access == 3) {
            json_object_object_del(item, "manager");
            if (json_object_array_add(rows, json_object_get(item)) != 0) { json_object_put(item); goto failure; }
        }
    }
    json_object_put(candidates);
    json_object_object_add(out, "assignees", rows); return true;
failure:
    json_object_put(candidates); json_object_put(rows); server_error(response); return false;
}
static void get_board(const char *query, DbInt actor_id, WorkspaceResponse *response) {
    Field fields[] = {{"job_id", NULL}};
    if (!parse_fields(query, fields, 1, response)) return;
    DbInt job_id = 0; char today[11]; bool manager = false;
    if (!parse_positive(field_value(&fields[0]), &job_id)) error(response, 400, "Seleziona una commessa.");
    else if (!today_date(today)) server_error(response);
    else if (active_actor(actor_id, &manager, response)) {
        int permission = job_permission(job_id, actor_id, manager, today, response);
        if (permission) {
            struct json_object *out = json_object_new_object(), *permissions = json_object_new_object();
            if (!out || !permissions) { json_object_put(out); json_object_put(permissions); server_error(response); }
            else {
                json_object_object_add(out, "ok", json_object_new_boolean(true));
                json_object_object_add(out, "job_id", json_object_new_int64(job_id));
                json_object_object_add(permissions, "manage_tasks", json_object_new_boolean(manager));
                json_object_object_add(permissions, "report_blockers", json_object_new_boolean(permission == 3));
                json_object_object_add(out, "permissions", permissions);
                if (list_records(out, true, job_id, actor_id, manager, permission, response) &&
                    list_records(out, false, job_id, actor_id, manager, permission, response) &&
                    list_assignees(out, job_id, manager, today, response)) finish(response, 200, out);
                else json_object_put(out);
            }
        }
    }
    free_fields(fields, 1);
}

static bool capacity_available(bool task, DbInt job_id, WorkspaceResponse *response) {
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, task ? "SELECT COUNT(*) FROM job_tasks WHERE job_id=?" :
        "SELECT COUNT(*) FROM job_blockers WHERE job_id=?") && bind_id(statement, 1, job_id);
    int result = okay ? db_step(statement) : DB_ERROR;
    DbInt count = result == DB_ROW ? db_column_int64(statement, 0) : -1;
    db_finalize(statement);
    if (count < 0) server_error(response);
    else if (count >= ROW_LIMIT) error(response, 409, "Raggiunto il limite di 500 elementi per questa commessa.");
    return count >= 0 && count < ROW_LIMIT;
}
static bool task_in_job(DbInt task_id, DbInt job_id, WorkspaceResponse *response) {
    if (!task_id) return true;
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT job_id FROM job_tasks WHERE id=?") && bind_id(statement, 1, task_id);
    int result = okay ? db_step(statement) : DB_ERROR;
    bool valid = result == DB_ROW && db_column_int64(statement, 0) == job_id;
    db_finalize(statement);
    if (!valid) {
        if (result == DB_ROW || result == DB_DONE) error(response, 400, "L’attività deve appartenere alla stessa commessa.");
        else server_error(response);
    }
    return valid;
}
static bool hierarchy_valid(DbInt job_id, DbInt id, DbInt parent_id, WorkspaceResponse *response) {
    DbInt ids[ROW_LIMIT + 1], parents[ROW_LIMIT + 1]; size_t count = 0;
    DbStmt *statement = NULL;
    if (!task_in_job(parent_id, job_id, response)) return false;
    bool okay = prepare(&statement, "SELECT id,parent_id FROM job_tasks WHERE job_id=? ORDER BY id LIMIT 501") &&
        bind_id(statement, 1, job_id);
    int result = DB_ERROR;
    while (okay && (result = db_step(statement)) == DB_ROW) {
        if (count >= ROW_LIMIT) { okay = false; break; }
        ids[count] = db_column_int64(statement, 0);
        parents[count] = ids[count] == id ? parent_id : db_column_int64(statement, 1);
        count++;
    }
    db_finalize(statement);
    if (!okay || result != DB_DONE) { server_error(response); return false; }
    if (!id) { ids[count] = -1; parents[count] = parent_id; count++; }
    /* Check every descendant too: moving a subtree must not bypass the depth cap. */
    for (size_t i = 0; i < count; i++) {
        DbInt parent = parents[i]; int depth = 1;
        while (parent) {
            if (++depth > MAX_DEPTH || parent == ids[i]) {
                error(response, 400, "Gerarchia non valida: niente cicli e al massimo 8 livelli."); return false;
            }
            size_t p = 0;
            while (p < count && ids[p] != parent) p++;
            if (p == count) { error(response, 400, "Attività padre non valida."); return false; }
            parent = parents[p];
        }
    }
    return true;
}
static bool one_of(const char *value, const char *const *choices) {
    for (size_t i = 0; choices[i]; i++) if (strcmp(value, choices[i]) == 0) return true;
    return false;
}
static char *field(Field *fields, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(fields[i].name, name) == 0) return fields[i].value;
    return NULL;
}
static const char *merged_text(Field *fields, size_t count, struct json_object *before,
                               const char *name, const char *fallback) {
    char *value = field(fields, count, name);
    return value ? value : before ? object_text(before, name) : fallback;
}
static bool merged_id(Field *fields, size_t count, struct json_object *before,
                       const char *name, DbInt *id) {
    char *value = field(fields, count, name);
    *id = before ? object_id(before, name) : 0;
    if (!value) return true;
    if (!*value) { *id = 0; return true; }
    return parse_positive(value, id);
}
static bool audit_record(bool task, const char *action, DbInt actor_id, DbInt id,
                          struct json_object *before, struct json_object *after) {
    return storage_audit(database, actor_id, action, task ? "task" : "blocker", id,
        before ? json_object_to_json_string_ext(before, JSON_C_TO_STRING_PLAIN) : NULL,
        after ? json_object_to_json_string_ext(after, JSON_C_TO_STRING_PLAIN) : NULL);
}
static void success(WorkspaceResponse *response, int status, DbInt id, DbInt revision) {
    struct json_object *out = json_object_new_object();
    if (!out) { server_error(response); return; }
    json_object_object_add(out, "ok", json_object_new_boolean(true));
    json_object_object_add(out, "id", json_object_new_int64(id));
    json_object_object_add(out, "revision", json_object_new_int64(revision));
    finish(response, status, out);
}

static void write_record(bool task, const char *body, DbInt id, DbInt actor_id,
                          WorkspaceResponse *response) {
    Field task_fields[] = {{"job_id",NULL},{"parent_id",NULL},{"assigned_user_id",NULL},{"title",NULL},
        {"description",NULL},{"status",NULL},{"priority",NULL},{"due_date",NULL},{"revision",NULL}};
    Field blocker_fields[] = {{"job_id",NULL},{"task_id",NULL},{"title",NULL},{"description",NULL},
        {"impact",NULL},{"owner_id",NULL},{"status",NULL},{"next_action",NULL},{"due_date",NULL},{"revision",NULL}};
    Field *fields = task ? task_fields : blocker_fields;
    size_t count = task ? sizeof(task_fields)/sizeof(*task_fields) : sizeof(blocker_fields)/sizeof(*blocker_fields);
    if (!parse_fields(body, fields, count, response)) return;
    struct json_object *before = NULL, *after = NULL;
    DbStmt *statement = NULL; bool transaction = false, manager = false;
    DbInt job_id = 0, revision = 0, relation = 0, assignee = 0;
    char today[11], stamp[21];
    if ((id && field(fields, count, "job_id")) || (!id && field(fields, count, "revision"))) {
        error(response, 400, "Campi non validi per questa operazione."); goto cleanup;
    }
    if (id && !parse_positive(field(fields, count, "revision"), &revision)) {
        error(response, 400, "La revisione è obbligatoria per modificare un elemento."); goto cleanup;
    }
    if (!id && !parse_positive(field(fields, count, "job_id"), &job_id)) {
        error(response, 400, "Seleziona una commessa."); goto cleanup;
    }
    if (!begin(response)) goto cleanup;
    transaction = true;
    /* The write guard may wait across midnight: evaluate grants after acquiring it. */
    if (!today_date(today) || !timestamp(stamp)) { server_error(response); goto cleanup; }
    if (!active_actor(actor_id, &manager, response)) goto cleanup;
    if (id) {
        before = load_record(task, id, response);
        if (!before) goto cleanup;
        job_id = object_id(before, "job_id");
    }
    int permission = job_permission(job_id, actor_id, manager, today, response);
    if (!permission) goto cleanup;
    bool permitted = manager || (permission == 3 && (task ? id && object_id(before, "assigned_user_id") == actor_id :
        !id || object_id(before, "created_by") == actor_id || object_id(before, "owner_id") == actor_id));
    if (!permitted) { error(response, 403, "Non hai i permessi per questa operazione."); goto cleanup; }
    if (!manager && task) {
        for (size_t i = 0; i < count; i++)
            if (fields[i].value && strcmp(fields[i].name, "revision") != 0 && strcmp(fields[i].name, "status") != 0) {
                error(response, 403, "Puoi aggiornare soltanto lo stato delle attività assegnate a te."); goto cleanup;
            }
        if (!field(fields, count, "status")) { error(response, 400, "Seleziona il nuovo stato."); goto cleanup; }
    }
    if (id && revision != object_id(before, "revision")) {
        error(response, 409, "L’elemento è stato modificato: aggiorna la pagina e riprova."); goto cleanup;
    }
    if (revision == INT64_MAX) { error(response, 409, "Limite revisioni raggiunto."); goto cleanup; }
    if (!id && !capacity_available(task, job_id, response)) goto cleanup;
    for (size_t i = 0; i < count; i++) if (strcmp(fields[i].name, "title") == 0) trim(fields[i].value);
    const char *title = merged_text(fields, count, before, "title", "");
    const char *description = merged_text(fields, count, before, "description", "");
    const char *status = merged_text(fields, count, before, "status", task ? "todo" : "open");
    const char *category = merged_text(fields, count, before, task ? "priority" : "impact", task ? "normal" : "medium");
    const char *due_date = merged_text(fields, count, before, "due_date", "");
    const char *next_action = task ? "" : merged_text(fields, count, before, "next_action", "");
    static const char *const task_statuses[] = {"todo","in_progress","blocked","in_review","done",NULL};
    static const char *const blocker_statuses[] = {"open","in_progress","resolved",NULL};
    static const char *const priorities[] = {"low","normal","high","urgent",NULL};
    static const char *const impacts[] = {"low","medium","high","critical",NULL};
    if (!valid_text(title, 160, true, false) || !valid_text(description, 2000, false, true) ||
        !valid_text(next_action, 1000, false, true) || (*due_date && !valid_date(due_date)) ||
        !one_of(status, task ? task_statuses : blocker_statuses) || !one_of(category, task ? priorities : impacts) ||
        !merged_id(fields, count, before, task ? "parent_id" : "task_id", &relation) ||
        !merged_id(fields, count, before, task ? "assigned_user_id" : "owner_id", &assignee)) {
        error(response, 400, "Dati non validi: controlla testo, stato, scadenza e assegnazioni."); goto cleanup;
    }
    if (!manager && !task) {
        if (!id) {
            if (field(fields, count, "owner_id") && assignee && assignee != actor_id) {
                error(response, 403, "Puoi assegnare a te stesso un nuovo blocco."); goto cleanup;
            }
            assignee = actor_id;
        } else if (field(fields, count, "owner_id") && assignee != object_id(before, "owner_id")) {
            error(response, 403, "Soltanto un gestore può cambiare il responsabile del blocco."); goto cleanup;
        }
    }
    const char *assignee_key = task ? "assigned_user_id" : "owner_id";
    if ((!id || assignee != object_id(before, assignee_key)) && !eligible_user(job_id, assignee, today, response)) goto cleanup;
    if (task ? !hierarchy_valid(job_id, id, relation, response) : !task_in_job(relation, job_id, response)) goto cleanup;
    bool okay;
    if (task) {
        okay = prepare(&statement, id ?
            "UPDATE job_tasks SET parent_id=?,assigned_user_id=?,title=?,description=?,status=?,priority=?,due_date=?,revision=revision+1,updated_at=? WHERE id=? AND revision=?" :
            "INSERT INTO job_tasks(parent_id,assigned_user_id,title,description,status,priority,due_date,updated_at,job_id,created_at,revision) VALUES(?,?,?,?,?,?,?,?,?,?,1)") &&
            nullable_id(statement, 1, relation) && nullable_id(statement, 2, assignee) &&
            bind_text(statement, 3, title) && bind_text(statement, 4, description) && bind_text(statement, 5, status) &&
            bind_text(statement, 6, category) && nullable_text(statement, 7, due_date) && bind_text(statement, 8, stamp) &&
            bind_id(statement, 9, id ? id : job_id) && (id ? bind_id(statement, 10, revision) : bind_text(statement, 10, stamp));
    } else {
        okay = prepare(&statement, id ?
            "UPDATE job_blockers SET task_id=?,title=?,description=?,impact=?,owner_id=?,status=?,next_action=?,due_date=?,updated_at=?,revision=revision+1 WHERE id=? AND revision=?" :
            "INSERT INTO job_blockers(task_id,title,description,impact,owner_id,status,next_action,due_date,updated_at,job_id,created_by,created_at,revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1)") &&
            nullable_id(statement, 1, relation) && bind_text(statement, 2, title) && bind_text(statement, 3, description) &&
            bind_text(statement, 4, category) && nullable_id(statement, 5, assignee) && bind_text(statement, 6, status) &&
            bind_text(statement, 7, next_action) && nullable_text(statement, 8, due_date) && bind_text(statement, 9, stamp) &&
            bind_id(statement, 10, id ? id : job_id) && bind_id(statement, 11, id ? revision : actor_id) &&
            (id || bind_text(statement, 12, stamp));
    }
    if (!okay || db_step(statement) != DB_DONE) { server_error(response); goto cleanup; }
    db_finalize(statement); statement = NULL;
    if (id && db_changes(database) != 1) { error(response, 409, "L’elemento è stato modificato. Aggiorna la pagina."); goto cleanup; }
    DbInt saved = id ? id : db_last_insert_rowid(database);
    after = load_record(task, saved, response);
    if (!after) goto cleanup;
    if (!audit_record(task, task ? (id ? "tasks.update" : "tasks.create") : (id ? "blockers.update" : "blockers.create"),
        actor_id, saved, before, after)) { server_error(response); goto cleanup; }
    if (!commit(response)) { transaction = false; goto cleanup; }
    transaction = false; success(response, id ? 200 : 201, saved, revision + 1);
cleanup:
    db_finalize(statement);
    if (transaction) rollback();
    json_object_put(before); json_object_put(after); free_fields(fields, count);
}

static bool detach_blockers(DbInt task_id, DbInt actor_id, const char *stamp,
                            WorkspaceResponse *response) {
    DbInt ids[ROW_LIMIT]; size_t count = 0;
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, "SELECT id FROM job_blockers WHERE task_id=? ORDER BY id LIMIT 501") && bind_id(statement, 1, task_id);
    int result = DB_ERROR;
    while (okay && (result = db_step(statement)) == DB_ROW) {
        if (count >= ROW_LIMIT) { okay = false; break; }
        ids[count++] = db_column_int64(statement, 0);
    }
    db_finalize(statement);
    if (!okay || result != DB_DONE) { server_error(response); return false; }
    for (size_t i = 0; i < count; i++) {
        struct json_object *before = load_record(false, ids[i], response), *after = NULL;
        if (!before) return false;
        DbInt revision = object_id(before, "revision");
        if (revision == INT64_MAX) {
            json_object_put(before); error(response, 409, "Limite revisioni raggiunto su un blocco collegato."); return false;
        }
        statement = NULL;
        okay = prepare(&statement, "UPDATE job_blockers SET task_id=NULL,revision=revision+1,updated_at=? WHERE id=? AND revision=?") &&
            bind_text(statement, 1, stamp) && bind_id(statement, 2, ids[i]) && bind_id(statement, 3, revision) &&
            db_step(statement) == DB_DONE;
        db_finalize(statement);
        if (okay && db_changes(database) == 1) after = load_record(false, ids[i], response);
        okay = after && audit_record(false, "blockers.task_detached", actor_id, ids[i], before, after);
        json_object_put(before); json_object_put(after);
        if (!okay) { if (!response->body) server_error(response); return false; }
    }
    return true;
}
static void delete_record(bool task, const char *body, DbInt id, DbInt actor_id,
                           WorkspaceResponse *response) {
    Field fields[] = {{"revision", NULL}};
    if (!parse_fields(body, fields, 1, response)) return;
    DbInt revision = 0; bool transaction = false, manager = false;
    struct json_object *before = NULL; DbStmt *statement = NULL;
    char today[11], stamp[21];
    if (!parse_positive(field_value(&fields[0]), &revision)) {
        error(response, 400, "La revisione è obbligatoria per eliminare un elemento."); goto cleanup;
    }
    if (!begin(response)) goto cleanup;
    transaction = true;
    /* The write guard may wait across midnight: evaluate grants after acquiring it. */
    if (!today_date(today) || !timestamp(stamp)) { server_error(response); goto cleanup; }
    if (!active_actor(actor_id, &manager, response)) goto cleanup;
    before = load_record(task, id, response);
    if (!before || !job_permission(object_id(before, "job_id"), actor_id, manager, today, response)) goto cleanup;
    if (!manager) { error(response, 403, "Soltanto i gestori possono eliminare attività e blocchi."); goto cleanup; }
    if (object_id(before, "revision") != revision) {
        error(response, 409, "L’elemento è stato modificato: aggiorna la pagina e riprova."); goto cleanup;
    }
    if (task) {
        bool okay = prepare(&statement, "SELECT id FROM job_tasks WHERE parent_id=? LIMIT 1") && bind_id(statement, 1, id);
        int result = okay ? db_step(statement) : DB_ERROR;
        db_finalize(statement); statement = NULL;
        if (result == DB_ROW) { error(response, 409, "Elimina o sposta prima le sottoattività."); goto cleanup; }
        if (result != DB_DONE) { server_error(response); goto cleanup; }
        if (!detach_blockers(id, actor_id, stamp, response)) goto cleanup;
    }
    bool okay = prepare(&statement, task ? "DELETE FROM job_tasks WHERE id=? AND revision=?" :
        "DELETE FROM job_blockers WHERE id=? AND revision=?") && bind_id(statement, 1, id) &&
        bind_id(statement, 2, revision) && db_step(statement) == DB_DONE;
    db_finalize(statement); statement = NULL;
    if (!okay) { server_error(response); goto cleanup; }
    if (db_changes(database) != 1) { error(response, 409, "L’elemento è stato modificato. Aggiorna la pagina."); goto cleanup; }
    if (!audit_record(task, task ? "tasks.delete" : "blockers.delete", actor_id, id, before, NULL)) {
        server_error(response); goto cleanup;
    }
    if (!commit(response)) { transaction = false; goto cleanup; }
    transaction = false; success(response, 200, id, revision);
cleanup:
    db_finalize(statement); if (transaction) rollback();
    json_object_put(before); free_fields(fields, 1);
}

bool work_board_open(const char *db_path) {
    work_board_close();
    if (!db_path || !*db_path || db_open_v2(db_path, &database,
        DB_OPEN_READWRITE | DB_OPEN_FULLMUTEX, NULL) != DB_OK || db_busy_timeout(database, 5000) != DB_OK
#ifndef PUNTO_MYSQL
        || db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != DB_OK
#endif
    ) { work_board_close(); return false; }
    return true;
}
void work_board_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
}
static bool parse_item(const char *text, DbInt *id, const char **suffix) {
    const char *end = strchr(text, '/');
    size_t length = end ? (size_t)(end - text) : strlen(text);
    if (!length || length >= 32) return false;
    char number[32]; memcpy(number, text, length); number[length] = '\0';
    if (!parse_positive(number, id)) return false;
    *suffix = text + length; return true;
}
bool work_board_handle(const char *method, const char *target, const char *query,
                       const char *body, DbInt actor_id, const char *role,
                       WorkspaceResponse *response) {
    (void)role; /* Re-read live account state in the same transaction as each write. */
    if (!method || !target || !response) return false;
    bool board = strcmp(target, "/api/work-board") == 0;
    bool tasks = strcmp(target, "/api/tasks") == 0;
    bool blockers = strcmp(target, "/api/blockers") == 0;
    bool task_item = strncmp(target, "/api/tasks/", 11) == 0;
    bool blocker_item = strncmp(target, "/api/blockers/", 14) == 0;
    if (!board && !tasks && !blockers && !task_item && !blocker_item) return false;
    response->status = 0; response->body = NULL;
    if (actor_id <= 0) { error(response, 401, "Accedi per continuare."); return true; }
    if (!database) { server_error(response); return true; }
    if (!query) query = "";
    if (!body) body = "";
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if (board) {
        if (!get) error(response, 405, "Metodo non consentito.");
        else if (*body) error(response, 400, "Corpo della richiesta non valido.");
        else get_board(query, actor_id, response);
        return true;
    }
    if (*query) { error(response, 400, "Parametri della richiesta non validi."); return true; }
    DbInt id = 0; const char *suffix = "";
    if ((task_item || blocker_item) && !parse_item(target + (task_item ? 11 : 14), &id, &suffix)) {
        error(response, 400, "Identificativo non valido."); return true;
    }
    if (*suffix && strcmp(suffix, "/delete") != 0) { error(response, 404, "Operazione non trovata."); return true; }
    if (!post) { error(response, 405, "Metodo non consentito."); return true; }
    if (*suffix) delete_record(tasks || task_item, body, id, actor_id, response);
    else write_record(tasks || task_item, body, id, actor_id, response);
    return true;
}
