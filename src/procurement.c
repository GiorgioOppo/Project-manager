#include "procurement.h"
#include "documents.h"
#include "storage.h"
#include "access_control.h"

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

/* Functional profiles are resolved from live account data on this connection;
   the administrative manager role does not grant commercial access. */
static int procurement_permission(DbInt actor, const char *role, bool write) {
    if (actor <= 0) return 401;
    if (!database) return 500;
    AccessPolicy policy = {0};
    int resolved = access_policy_resolve(database, actor, role, &policy);
    if (resolved < 0) return 500;
    if (!resolved) return 403;
    return (write ? policy.sales : policy.sales || policy.finance) ? 200 : 403;
}

static bool require_procurement(DbInt actor, const char *role, bool write,
                                WorkspaceResponse *response) {
    int status = procurement_permission(actor, role, write);
    if (status == 200) return true;
    if (status >= 500) server_error(response);
    else error(response, status, status == 401 ? "Accedi per continuare." :
               "Il tuo ruolo non consente questa operazione.");
    return false;
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


#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <Security/SecRandom.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define DOCUMENT_LIMIT (10U * 1024U * 1024U)
#define VALUE_LIMIT INT64_C(999999999999)

bool procurement_open(const char *db_path) {
    procurement_close();
    if (!db_path || !*db_path || db_open_v2(db_path, &database,
        DB_OPEN_READWRITE | DB_OPEN_FULLMUTEX, NULL) != DB_OK ||
        db_busy_timeout(database, 5000) != DB_OK
#ifndef PUNTO_MYSQL
        || db_exec(database, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != DB_OK
#endif
    ) { procurement_close(); return false; }
    return true;
}

void procurement_close(void) {
    if (database) (void)db_close(database);
    database = NULL;
}

static bool nullable_text(DbStmt *statement, int index, const char *text) {
    return text && *text ? bind_text(statement, index, text) :
        db_bind_null(statement, index) == DB_OK;
}

static void nullable_integer(Json *json, DbStmt *statement, int column) {
    if (db_column_type(statement, column) == DB_NULL) append(json, "null");
    else append_integer(json, db_column_int64(statement, column));
}

static void nullable_string(Json *json, DbStmt *statement, int column) {
    if (db_column_type(statement, column) == DB_NULL) append(json, "null");
    else append_string(json, column_text(statement, column));
}

static bool parse_value(const char *text, DbInt *value) {
    if (!text || !*text) { *value = -1; return true; }
    DbInt result = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        int digit = *p - '0';
        if (result > (VALUE_LIMIT - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

static void success(WorkspaceResponse *response, int status, DbInt id) {
    Json json = {0};
    append(&json, "{\"ok\":true,\"id\":"); append_integer(&json, id); append(&json, "}");
    finish_json(response, status, &json);
}

static bool row_limit(const char *table, WorkspaceResponse *response) {
    const char *sql = strcmp(table, "suppliers") == 0 ?
        "SELECT COUNT(*) FROM suppliers" : "SELECT COUNT(*) FROM contracts";
    DbStmt *statement = NULL;
    bool okay = prepare(&statement, sql) && db_step(statement) == DB_ROW;
    DbInt count = okay ? db_column_int64(statement, 0) : 0;
    db_finalize(statement);
    if (!okay) server_error(response);
    else if (count >= RESULT_LIMIT) error(response, 409, "Hai raggiunto il limite di 1000 elementi.");
    return okay && count < RESULT_LIMIT;
}

static bool preserve_fields(Field *fields, size_t count, const char *sql, DbInt id) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, sql) || !bind_id(statement, 1, id) || db_step(statement) != DB_ROW) {
        db_finalize(statement); return false;
    }
    bool okay = true;
    for (size_t i = 0; okay && i < count; i++) {
        if (fields[i].value) continue;
        if (db_column_type(statement, (int)i) == DB_INTEGER) {
            char value[32];
            (void)snprintf(value, sizeof(value), "%lld", (long long)db_column_int64(statement, (int)i));
            fields[i].value = duplicate(value);
        } else fields[i].value = duplicate(column_text(statement, (int)i));
        okay = fields[i].value != NULL;
    }
    db_finalize(statement);
    return okay;
}

static const char supplier_select[] =
    "SELECT id,code,name,vat_number,email,phone,address,notes,active FROM suppliers";
static const char contract_select[] =
    "SELECT c.id,c.code,c.title,c.supplier_id,c.status,c.starts_on,c.ends_on,"
    "c.value_cents,c.currency,c.notes,s.name,(SELECT COUNT(*) FROM contract_documents d WHERE d.contract_id=c.id) "
    "FROM contracts c JOIN suppliers s ON s.id=c.supplier_id";

static void supplier_json(Json *json, DbStmt *statement) {
    static const char *const names[] = {"code","name","vat_number","email","phone","address","notes"};
    append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        append(json, ","); append_string(json, names[i]); append(json, ":");
        append_string(json, column_text(statement, (int)i + 1));
    }
    append(json, ",\"active\":"); append_integer(json, db_column_int64(statement, 8)); append(json, "}");
}

static void contract_json(Json *json, DbStmt *statement) {
    append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"code\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"title\":"); append_string(json, column_text(statement, 2));
    append(json, ",\"supplier_id\":"); append_integer(json, db_column_int64(statement, 3));
    append(json, ",\"status\":"); append_string(json, column_text(statement, 4));
    append(json, ",\"starts_on\":"); nullable_string(json, statement, 5);
    append(json, ",\"ends_on\":"); nullable_string(json, statement, 6);
    append(json, ",\"value_cents\":"); nullable_integer(json, statement, 7);
    append(json, ",\"currency\":"); append_string(json, column_text(statement, 8));
    append(json, ",\"notes\":"); append_string(json, column_text(statement, 9));
    append(json, ",\"supplier_name\":"); append_string(json, column_text(statement, 10));
    append(json, ",\"documents_count\":"); append_integer(json, db_column_int64(statement, 11));
    append(json, "}");
}

static int snapshot(bool supplier, DbInt id, Json *json) {
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s WHERE %sid=?", supplier ? supplier_select : contract_select,
                   supplier ? "" : "c.");
    DbStmt *statement = NULL;
    if (!prepare(&statement, sql) || !bind_id(statement, 1, id)) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement);
    if (result == DB_ROW) {
        if (supplier) supplier_json(json, statement); else contract_json(json, statement);
    }
    db_finalize(statement);
    if (json->failed) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static void list_records(bool supplier, WorkspaceResponse *response) {
    char sql[512];
    (void)snprintf(sql, sizeof(sql), "%s ORDER BY %sid DESC LIMIT 1001", supplier ? supplier_select : contract_select,
                   supplier ? "" : "c.");
    DbStmt *statement = NULL;
    if (!prepare(&statement, sql)) { server_error(response); return; }
    Json json = {0};
    append(&json, supplier ? "{\"suppliers\":[" : "{\"contracts\":[");
    int count = 0, result;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > RESULT_LIMIT) break;
        if (count > 1) append(&json, ",");
        if (supplier) supplier_json(&json, statement); else contract_json(&json, statement);
    }
    db_finalize(statement);
    if (count > RESULT_LIMIT || result != DB_DONE) {
        free(json.data);
        if (count > RESULT_LIMIT) error(response, 409, "Sono presenti troppi elementi: il limite è di 1000.");
        else server_error(response);
        return;
    }
    append(&json, "]}"); finish_json(response, 200, &json);
}

static bool valid_email(const char *text) {
    if (!*text) return true;
    const char *at = strchr(text, '@');
    return at && at != text && at[1] && !strchr(at + 1, '@') && !strpbrk(text, " \r\n\t");
}

static void write_supplier(const char *body, DbInt actor_id, DbInt id,
                           WorkspaceResponse *response) {
    Field fields[] = {{"code",NULL},{"name",NULL},{"vat_number",NULL},{"email",NULL},
                      {"phone",NULL},{"address",NULL},{"notes",NULL},{"active",NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    if (!require_procurement(actor_id, NULL, true, response)) {
        rollback(); free_fields(fields, count); return;
    }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    if (id) {
        int found = snapshot(true, id, &before);
        if (found < 0) goto failure;
        if (!found) { rollback(); error(response, 404, "Fornitore non trovato."); goto cleanup; }
        if (!preserve_fields(fields, count,
            "SELECT code,name,vat_number,email,phone,address,notes,active FROM suppliers WHERE id=?", id)) goto failure;
    } else if (!row_limit("suppliers", response)) { rollback(); goto cleanup; }
    for (size_t i = 0; i < count; i++) if (i != 6) trim(fields[i].value);
    static const size_t limits[] = {40,120,32,254,40,240,1000};
    for (size_t i = 0; i < 7; i++) {
        if (!valid_text(field_value(&fields[i]), limits[i], i < 2, i == 6)) {
            rollback(); error(response, 400, "Controlla i campi del fornitore e le relative lunghezze."); goto cleanup;
        }
    }
    const char *active = fields[7].value ? fields[7].value : "1";
    if ((strcmp(active, "0") != 0 && strcmp(active, "1") != 0) || !valid_email(field_value(&fields[3]))) {
        rollback(); error(response, 400, "Email o stato del fornitore non validi."); goto cleanup;
    }
    if (!prepare(&statement, id ?
        "UPDATE suppliers SET code=?,name=?,vat_number=?,email=?,phone=?,address=?,notes=?,active=? WHERE id=?" :
        "INSERT INTO suppliers(code,name,vat_number,email,phone,address,notes,active) VALUES(?,?,?,?,?,?,?,?)")) goto failure;
    for (int i = 0; i < 7; i++) if (!bind_text(statement, i + 1, field_value(&fields[i]))) goto failure;
    if (db_bind_int(statement, 8, active[0] - '0') != DB_OK || (id && !bind_id(statement, 9, id))) goto failure;
    int result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) {
        int extended = db_extended_errcode(database);
        rollback();
        if (extended == DB_CONSTRAINT_UNIQUE) error(response, 409, "Esiste già un fornitore con questo codice.");
        else server_error(response);
        goto cleanup;
    }
    DbInt saved = id ? id : db_last_insert_rowid(database);
    if (snapshot(true, saved, &after) != 1 ||
        !storage_audit(database, actor_id, id ? "suppliers.update" : "suppliers.create",
                       "supplier", saved, before.data, after.data)) goto failure;
    if (commit_transaction(response)) success(response, id ? 200 : 201, saved);
    goto cleanup;
failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data); free(after.data); free_fields(fields, count);
}

static bool contract_status(const char *value) {
    return strcmp(value,"draft") == 0 || strcmp(value,"active") == 0 || strcmp(value,"expired") == 0 ||
        strcmp(value,"closed") == 0 || strcmp(value,"cancelled") == 0;
}

static void write_contract(const char *body, DbInt actor_id, DbInt id,
                           WorkspaceResponse *response) {
    Field fields[] = {{"code",NULL},{"title",NULL},{"supplier_id",NULL},{"status",NULL},
                      {"starts_on",NULL},{"ends_on",NULL},{"value_cents",NULL},{"currency",NULL},{"notes",NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    if (!require_procurement(actor_id, NULL, true, response)) {
        rollback(); free_fields(fields, count); return;
    }
    Json before = {0}, after = {0};
    DbStmt *statement = NULL;
    DbInt old_supplier = 0;
    if (id) {
        int found = snapshot(false, id, &before);
        if (found < 0) goto failure;
        if (!found) { rollback(); error(response, 404, "Contratto non trovato."); goto cleanup; }
        if (!preserve_fields(fields, count,
            "SELECT code,title,supplier_id,status,starts_on,ends_on,value_cents,currency,notes FROM contracts WHERE id=?", id) ||
            !prepare(&statement, "SELECT supplier_id FROM contracts WHERE id=?") ||
            !bind_id(statement, 1, id) || db_step(statement) != DB_ROW) goto failure;
        old_supplier = db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
    } else if (!row_limit("contracts", response)) { rollback(); goto cleanup; }
    for (size_t i = 0; i < count; i++) if (i != 8) trim(fields[i].value);
    const char *status = fields[3].value ? fields[3].value : "draft";
    const char *currency = fields[7].value ? fields[7].value : "EUR";
    const char *start = field_value(&fields[4]), *end = field_value(&fields[5]);
    DbInt supplier_id = 0, value = -1;
    if (!valid_text(field_value(&fields[0]), 40, true, false) ||
        !valid_text(field_value(&fields[1]), 120, true, false) ||
        !parse_positive(field_value(&fields[2]), &supplier_id) || !contract_status(status) ||
        (*start && !valid_date(start)) || (*end && !valid_date(end)) ||
        (*start && *end && strcmp(start, end) > 0) || !parse_value(field_value(&fields[6]), &value) ||
        strcmp(currency,"EUR") != 0 || !valid_text(field_value(&fields[8]),1000,false,true)) {
        rollback(); error(response, 400, "Controlla codice, titolo, fornitore, stato, date e importo del contratto."); goto cleanup;
    }
    if (!prepare(&statement, "SELECT active FROM suppliers WHERE id=?") || !bind_id(statement,1,supplier_id)) goto failure;
    int found = db_step(statement);
    bool active = found == DB_ROW && db_column_int(statement,0) == 1;
    db_finalize(statement); statement = NULL;
    if (found != DB_ROW && found != DB_DONE) goto failure;
    if (found == DB_DONE || (!active && supplier_id != old_supplier)) {
        rollback(); error(response, 400, "Seleziona un fornitore attivo esistente."); goto cleanup;
    }
    if (!prepare(&statement, id ?
        "UPDATE contracts SET code=?,title=?,supplier_id=?,status=?,starts_on=?,ends_on=?,value_cents=?,currency=?,notes=? WHERE id=?" :
        "INSERT INTO contracts(code,title,supplier_id,status,starts_on,ends_on,value_cents,currency,notes) VALUES(?,?,?,?,?,?,?,?,?)") ||
        !bind_text(statement,1,field_value(&fields[0])) || !bind_text(statement,2,field_value(&fields[1])) ||
        !bind_id(statement,3,supplier_id) || !bind_text(statement,4,status) || !nullable_text(statement,5,start) ||
        !nullable_text(statement,6,end) || (value < 0 ? db_bind_null(statement,7) != DB_OK : !bind_id(statement,7,value)) ||
        !bind_text(statement,8,currency) || !bind_text(statement,9,field_value(&fields[8])) || (id && !bind_id(statement,10,id))) goto failure;
    int result = db_step(statement);
    db_finalize(statement); statement = NULL;
    if (result != DB_DONE) {
        int extended = db_extended_errcode(database); rollback();
        if (extended == DB_CONSTRAINT_UNIQUE) error(response,409,"Esiste già un contratto con questo codice.");
        else server_error(response);
        goto cleanup;
    }
    DbInt saved = id ? id : db_last_insert_rowid(database);
    if (snapshot(false,saved,&after) != 1 ||
        !storage_audit(database,actor_id,id ? "contracts.update" : "contracts.create","contract",saved,before.data,after.data)) goto failure;
    if (commit_transaction(response)) success(response,id ? 200 : 201,saved);
    goto cleanup;
failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data); free(after.data); free_fields(fields,count);
}

static void document_json(Json *json, DbStmt *statement) {
    append(json,"{\"id\":"); append_integer(json,db_column_int64(statement,0));
    append(json,",\"contract_id\":"); append_integer(json,db_column_int64(statement,1));
    append(json,",\"file_name\":"); append_string(json,column_text(statement,2));
    append(json,",\"byte_size\":"); append_integer(json,db_column_int64(statement,3));
    append(json,",\"sha256\":"); append_string(json,column_text(statement,4));
    append(json,",\"uploaded_by\":"); append_integer(json,db_column_int64(statement,5));
    append(json,",\"created_at\":"); append_string(json,column_text(statement,6)); append(json,"}");
}

static int contract_exists(DbInt id) {
    DbStmt *statement = NULL;
    if (!prepare(&statement,"SELECT id FROM contracts WHERE id=?") || !bind_id(statement,1,id)) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement); db_finalize(statement);
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static void list_documents(DbInt contract_id, WorkspaceResponse *response) {
    int found = contract_exists(contract_id);
    if (found < 0) { server_error(response); return; }
    if (!found) { error(response,404,"Contratto non trovato."); return; }
    DbStmt *statement = NULL;
    if (!prepare(&statement,
        "SELECT id,contract_id,file_name,byte_size,sha256,uploaded_by,created_at FROM contract_documents "
        "WHERE contract_id=? ORDER BY id DESC LIMIT 1001") || !bind_id(statement,1,contract_id)) {
        db_finalize(statement); server_error(response); return;
    }
    Json json = {0}; append(&json,"{\"documents\":[");
    int count = 0, result;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > RESULT_LIMIT) break;
        if (count > 1) append(&json,",");
        document_json(&json,statement);
    }
    db_finalize(statement);
    if (count > RESULT_LIMIT || result != DB_DONE) {
        free(json.data);
        if (count > RESULT_LIMIT) error(response,409,"Limite di documenti raggiunto."); else server_error(response);
        return;
    }
    append(&json,"]}"); finish_json(response,200,&json);
}

static bool digest(const unsigned char *data, size_t size, char output[65]) {
    unsigned char bytes[32];
#ifdef __APPLE__
    if (size > UINT32_MAX || !CC_SHA256(data,(CC_LONG)size,bytes)) return false;
#else
    unsigned int length = 0;
    if (EVP_Digest(data,size,bytes,&length,EVP_sha256(),NULL) != 1 || length != sizeof(bytes)) return false;
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        output[i * 2] = hex[bytes[i] >> 4]; output[i * 2 + 1] = hex[bytes[i] & 15];
    }
    output[64] = '\0'; return true;
}

static bool storage_key(char output[65]) {
    unsigned char bytes[32];
#ifdef __APPLE__
    if (SecRandomCopyBytes(kSecRandomDefault,sizeof(bytes),bytes) != errSecSuccess) return false;
#else
    if (RAND_bytes(bytes,(int)sizeof(bytes)) != 1) return false;
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        output[i * 2] = hex[bytes[i] >> 4]; output[i * 2 + 1] = hex[bytes[i] & 15];
    }
    output[64] = '\0'; return true;
}

static bool valid_pdf_filename(const char *filename) {
    if (!filename) return false;
    size_t length = strlen(filename);
    if (length < 5 || length > 180 || filename[0] == '.' ||
        !valid_text(filename,180,true,false) || strpbrk(filename,"/\\\"")) return false;
    const char *end = filename + length - 4;
    return end[0] == '.' && (end[1] == 'p' || end[1] == 'P') &&
        (end[2] == 'd' || end[2] == 'D') && (end[3] == 'f' || end[3] == 'F');
}

void procurement_upload(DbInt contract_id, const char *filename,
                        const unsigned char *data, size_t size, DbInt actor_id,
                        const char *role, WorkspaceResponse *response) {
    if (!response) return;
    response->status = 0; response->body = NULL;
    if (!require_procurement(actor_id, role, true, response)) return;
    if (contract_id <= 0 || !valid_pdf_filename(filename) ||
        !data || size < 5 || size > DOCUMENT_LIMIT || memcmp(data,"%PDF-",5) != 0) {
        error(response,400,"Carica un PDF valido fino a 10 MiB con un nome file valido (max 180 byte)."); return;
    }
    int found = contract_exists(contract_id);
    if (found < 0) { server_error(response); return; }
    if (!found) { error(response,404,"Contratto non trovato."); return; }
    char key[65], sha256[65];
    if (!storage_key(key) || !digest(data,size,sha256)) { server_error(response); return; }
    if (!documents_store(key,filename,data,size,sha256)) { error(response,503,"Archivio documenti non disponibile. Riprova."); return; }
    if (!begin_transaction(response)) { (void)documents_remove(key); return; }
    if (!require_procurement(actor_id, role, true, response)) {
        rollback(); (void)documents_remove(key); return;
    }
    DbStmt *statement = NULL;
    Json after = {0};
    if (!prepare(&statement,"SELECT COUNT(*) FROM contract_documents WHERE contract_id=?") ||
        !bind_id(statement,1,contract_id) || db_step(statement) != DB_ROW) goto failure;
    DbInt existing = db_column_int64(statement,0);
    db_finalize(statement); statement = NULL;
    if (existing >= RESULT_LIMIT) {
        rollback(); (void)documents_remove(key); error(response,409,"Hai raggiunto il limite di 1000 documenti per contratto."); return;
    }
    if (contract_exists(contract_id) != 1 || !prepare(&statement,
#ifdef PUNTO_MYSQL
        "INSERT INTO contract_documents(contract_id,storage_key,file_name,byte_size,sha256,uploaded_by,created_at) "
        "VALUES(?,?,?,?,?,?,DATE_FORMAT(UTC_TIMESTAMP(),'%Y-%m-%dT%H:%i:%sZ'))"
#else
        "INSERT INTO contract_documents(contract_id,storage_key,file_name,byte_size,sha256,uploaded_by) VALUES(?,?,?,?,?,?)"
#endif
        ) || !bind_id(statement,1,contract_id) || !bind_text(statement,2,key) || !bind_text(statement,3,filename) ||
        !bind_id(statement,4,(DbInt)size) || !bind_text(statement,5,sha256) || !bind_id(statement,6,actor_id) ||
        db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    DbInt id = db_last_insert_rowid(database);
    if (!prepare(&statement,"SELECT id,contract_id,file_name,byte_size,sha256,uploaded_by,created_at FROM contract_documents WHERE id=?") ||
        !bind_id(statement,1,id) || db_step(statement) != DB_ROW) goto failure;
    document_json(&after,statement);
    db_finalize(statement); statement = NULL;
    if (after.failed || !storage_audit(database,actor_id,"contract_documents.create","contract_document",id,NULL,after.data)) goto failure;
    /* A transport error during COMMIT can leave its outcome unknown. Keep the
       new blob so an acknowledged SQL commit can never reference deleted bytes;
       recovery can later reconcile any unreferenced object. */
    if (commit_transaction(response)) success(response,201,id);
    free(after.data); return;
failure:
    db_finalize(statement); rollback(); (void)documents_remove(key); free(after.data); server_error(response);
}

int procurement_download(DbInt document_id, DbInt actor_id, const char *role,
                         unsigned char **data, size_t *size, char filename[181]) {
    if (!data || !size || !filename) return 503;
    *data = NULL; *size = 0; filename[0] = '\0';
    int permitted = procurement_permission(actor_id, role, false);
    if (permitted != 200) return permitted >= 500 ? 503 : permitted;
    if (document_id <= 0) return 404;
    DbStmt *statement = NULL;
    if (!prepare(&statement,"SELECT storage_key,file_name,byte_size,sha256 FROM contract_documents WHERE id=?") ||
        !bind_id(statement,1,document_id)) { db_finalize(statement); return 503; }
    int found = db_step(statement);
    if (found != DB_ROW) { db_finalize(statement); return found == DB_DONE ? 404 : 503; }
    char key[65], sha256[65];
    DbInt bytes = db_column_int64(statement,2);
    const char *stored_key = column_text(statement,0), *stored_name = column_text(statement,1), *stored_hash = column_text(statement,3);
    if (strlen(stored_key) != 64 || strlen(stored_hash) != 64 || !valid_pdf_filename(stored_name) || bytes < 5 || bytes > (DbInt)DOCUMENT_LIMIT) {
        db_finalize(statement); return 503;
    }
    memcpy(key,stored_key,65); memcpy(sha256,stored_hash,65); memcpy(filename,stored_name,strlen(stored_name)+1);
    db_finalize(statement);
    unsigned char *loaded = NULL;
    if (!documents_load(key,(size_t)bytes,&loaded)) return 503;
    char actual[65];
    if (!loaded || memcmp(loaded,"%PDF-",5) != 0 || !digest(loaded,(size_t)bytes,actual) || strcmp(actual,sha256) != 0) {
        free(loaded); return 503;
    }
    permitted = procurement_permission(actor_id, role, false);
    if (permitted != 200) {
        free(loaded); filename[0] = '\0'; return permitted >= 500 ? 503 : permitted;
    }
    *data = loaded; *size = (size_t)bytes; return 200;
}

bool procurement_handle(const char *method, const char *target, const char *query,
                        const char *body, DbInt actor_id, const char *role,
                        WorkspaceResponse *response) {
    if (!method || !target || !response) return false;
    bool suppliers = strcmp(target,"/api/suppliers") == 0;
    bool supplier_item = strncmp(target,"/api/suppliers/",15) == 0;
    bool contracts = strcmp(target,"/api/contracts") == 0;
    bool contract_item = strncmp(target,"/api/contracts/",15) == 0;
    if (!suppliers && !supplier_item && !contracts && !contract_item) return false;
    response->status = 0; response->body = NULL;
    if (!require_procurement(actor_id, role, strcmp(method, "GET") != 0, response)) return true;
    if (query && *query) { error(response,400,"Parametri della richiesta non validi."); return true; }
    DbInt id = 0;
    bool docs = false;
    if (supplier_item || contract_item) {
        const char *path = target + 15;
        const char *slash = strchr(path,'/');
        if (slash && contract_item && strcmp(slash,"/documents") == 0) {
            size_t length = (size_t)(slash-path);
            char value[20];
            if (!length || length >= sizeof(value)) { error(response,400,"Identificativo non valido."); return true; }
            memcpy(value,path,length); value[length] = '\0';
            docs = parse_positive(value,&id);
            if (!docs) { error(response,400,"Identificativo non valido."); return true; }
        } else if (!parse_positive(path,&id)) { error(response,400,"Identificativo non valido."); return true; }
    }
    if (docs && strcmp(method,"GET") == 0) list_documents(id,response);
    else if (docs) error(response,405,"Metodo non consentito.");
    else if ((suppliers || contracts) && strcmp(method,"GET") == 0) list_records(suppliers,response);
    else if ((suppliers || supplier_item) && strcmp(method,"POST") == 0) write_supplier(body ? body : "",actor_id,id,response);
    else if ((contracts || contract_item) && strcmp(method,"POST") == 0) write_contract(body ? body : "",actor_id,id,response);
    else error(response,405,"Metodo non consentito.");
    return true;
}
