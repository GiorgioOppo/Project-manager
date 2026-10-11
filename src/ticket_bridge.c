#include "ticket_bridge.h"
#include "access_control.h"
#include "organization.h"
#include "inventory_client.h"
#include "calendar_client.h"

#include <json-c/json.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CATALOG_LIMIT 10000
static Db *database;

typedef struct { const char *name; char *value; } QueryField;
typedef struct {
    OrganizationAccess jobs;
    bool all_jobs;
} BridgeAccess;

static void finish(WorkspaceResponse *response, int status, struct json_object *object) {
    response->status = status;
    response->body = object ? strdup(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN)) : NULL;
    if (object) json_object_put(object);
    if (!response->body) {
        response->status = 503;
        response->body = strdup("{\"ok\":false,\"error\":\"Integrazione temporaneamente non disponibile.\"}");
    }
}

static void fail(WorkspaceResponse *response, int status, const char *message) {
    struct json_object *object = json_object_new_object();
    if (object) {
        json_object_object_add(object, "ok", json_object_new_boolean(false));
        json_object_object_add(object, "error", json_object_new_string(message));
    }
    finish(response, status, object);
}

static void unavailable(WorkspaceResponse *response) {
    fail(response, 503, "Integrazione temporaneamente non disponibile. Riprova.");
}

static bool positive(const char *text, DbInt maximum, bool zero, DbInt *out) {
    if (!text || !*text) return false;
    DbInt value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > (maximum - (*p - '0')) / 10) return false;
        value = value * 10 + *p - '0';
    }
    if (!zero && !value) return false;
    *out = value;
    return true;
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode(const char *raw, size_t length) {
    char *decoded = malloc(length + 1);
    if (!decoded) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%') {
            if (i + 2 >= length || hex(raw[i + 1]) < 0 || hex(raw[i + 2]) < 0) goto invalid;
            c = (unsigned char)(hex(raw[i + 1]) * 16 + hex(raw[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) goto invalid;
        decoded[used++] = (char)c;
    }
    decoded[used] = 0;
    /* json-c validates UTF-8 for a standalone JSON string, including the last
       multi-byte scalar. Serialization escapes only the already bounded input. */
    struct json_object *string = json_object_new_string_len(decoded, (int)used);
    if (!string) goto invalid;
    const char *serialized = json_object_to_json_string_ext(string, JSON_C_TO_STRING_PLAIN);
    struct json_tokener *tokener = json_tokener_new();
    if (!tokener) { json_object_put(string); goto invalid; }
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *checked = json_tokener_parse_ex(tokener, serialized, (int)strlen(serialized));
    bool valid = checked && json_tokener_get_error(tokener) == json_tokener_success;
    if (checked) json_object_put(checked);
    json_tokener_free(tokener);
    json_object_put(string);
    if (!valid) goto invalid;
    return decoded;
invalid:
    free(decoded);
    return NULL;
}

static void free_query(QueryField *fields, size_t count) {
    for (size_t i = 0; i < count; i++) { free(fields[i].value); fields[i].value = NULL; }
}

static bool parse_fields(const char *query, QueryField *fields, size_t count, size_t maximum) {
    const char *p = query ? query : "";
    if (strlen(p) > maximum) return false;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (!equal || equal == p) goto invalid;
        char *name = decode(p, (size_t)(equal - p));
        if (!name) goto invalid;
        size_t field = 0;
        while (field < count && strcmp(name, fields[field].name)) field++;
        free(name);
        if (field == count || fields[field].value) goto invalid;
        fields[field].value = decode(equal + 1, (size_t)(end - equal - 1));
        if (!fields[field].value) goto invalid;
        if (!*end) break;
        p = end + 1;
        if (!*p) goto invalid;
    }
    return true;
invalid:
    free_query(fields, count);
    return false;
}

static bool parse_query(const char *query, QueryField *fields, size_t count) {
    return parse_fields(query, fields, count, 4096);
}

static bool today_date(char out[11]) {
    time_t now = time(NULL);
    struct tm today;
    return localtime_r(&now, &today) && strftime(out, 11, "%Y-%m-%d", &today) == 10;
}

bool ticket_bridge_open(const char *db_path) {
    ticket_bridge_close();
    if (db_open_v2(db_path, &database, DB_OPEN_READWRITE | DB_OPEN_FULLMUTEX, NULL) != DB_OK ||
        db_busy_timeout(database, 5000) != DB_OK) {
        ticket_bridge_close();
        return false;
    }
    return true;
}

void ticket_bridge_close(void) {
    if (database) db_close(database);
    database = NULL;
}

static bool prepare(DbStmt **statement, const char *sql) {
    return db_prepare_v2(database, sql, -1, statement, NULL) == DB_OK;
}

static const char *text(DbStmt *statement, int index) {
    const unsigned char *value = db_column_text(statement, index);
    return value ? (const char *)value : "";
}

static bool access_list(const Account *actor, BridgeAccess *access) {
    AccessPolicy policy;
    char today[11];
    memset(access, 0, sizeof(*access));
    if (access_policy_resolve(database, actor->id, NULL, &policy) != 1) return false;
    access->all_jobs = policy.admin;
    if (policy.admin) return true;
    if (!today_date(today) || !organization_access_list(database, actor->id, today, NULL, &access->jobs)) return false;
    if (!policy.manager) return true;

    /* Match the live workspace list: an area manager sees explicitly granted
       jobs plus those covered by a current management delegation. The account
       role alone does not confer global access. */
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id FROM jobs ORDER BY id LIMIT 10001")) return false;
    int result, scanned = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++scanned > CATALOG_LIMIT) break;
        DbInt id = db_column_int64(statement, 0);
        int management = access_policy_can_manage_job(database, actor->id, id);
        if (management < 0) { db_finalize(statement); return false; }
        if (!management) continue;
        size_t index = 0;
        while (index < access->jobs.count && access->jobs.job_ids[index] != id) index++;
        if (index == access->jobs.count) {
            if (index == sizeof(access->jobs.job_ids) / sizeof(access->jobs.job_ids[0])) {
                db_finalize(statement);
                return false;
            }
            access->jobs.job_ids[index] = id;
            access->jobs.count++;
        }
        access->jobs.permissions[index] = 3;
    }
    db_finalize(statement);
    return scanned <= CATALOG_LIMIT && result == DB_DONE;
}

static bool allowed(const BridgeAccess *access, DbInt id) {
    if (access->all_jobs) return true;
    for (size_t i = 0; i < access->jobs.count; i++)
        if (access->jobs.job_ids[i] == id) return (access->jobs.permissions[i] & 1) != 0;
    return false;
}

static void scope(const Account *actor, const char *state, time_t expires, bool calendar_access, bool time_access,
                  WorkspaceResponse *response) {
    BridgeAccess access;
    if (!access_list(actor, &access)) { unavailable(response); return; }
    struct json_object *object = json_object_new_object();
    struct json_object *user = json_object_new_object();
    struct json_object *ids = json_object_new_array();
    json_object_object_add(object, "state", json_object_new_string(state));
    json_object_object_add(user, "id", json_object_new_int64(actor->id));
    json_object_object_add(user, "name", json_object_new_string(actor->name));
    json_object_object_add(object, "user", user);
    json_object_object_add(object, "all_jobs", json_object_new_boolean(access.all_jobs));
    for (size_t i = 0; i < access.jobs.count; i++) json_object_array_add(ids, json_object_new_int64(access.jobs.job_ids[i]));
    json_object_object_add(object, "job_ids", ids);
    json_object_object_add(object, "expires_at", json_object_new_int64((int64_t)expires));
    json_object_object_add(object, "calendar_access", json_object_new_boolean(calendar_access));
    json_object_object_add(object, "time_access", json_object_new_boolean(time_access));
    finish(response, 200, object);
}

static bool contains_folded(const char *text_value, const char *search) {
    if (!*search) return true;
    size_t length = strlen(search);
    for (const char *p = text_value; *p; p++) if (!strncasecmp(p, search, length)) return true;
    return false;
}

static struct json_object *job_row(DbStmt *statement, bool with_status) {
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "id", json_object_new_int64(db_column_int64(statement, 0)));
    json_object_object_add(object, "code", json_object_new_string(text(statement, 1)));
    json_object_object_add(object, "name", json_object_new_string(text(statement, 2)));
    if (with_status) json_object_object_add(object, "status", json_object_new_string(text(statement, 3)));
    return object;
}

static void jobs(const char *query, const Account *actor, WorkspaceResponse *response) {
    QueryField fields[] = {{"search", NULL}, {"limit", NULL}, {"offset", NULL}};
    if (!parse_query(query, fields, 3)) { fail(response, 400, "Parametri commesse non validi."); return; }
    DbInt limit = 30, offset = 0;
    const char *search = fields[0].value ? fields[0].value : "";
    if (strlen(search) > 800 || (fields[1].value && !positive(fields[1].value, 100, false, &limit)) ||
        (fields[2].value && !positive(fields[2].value, 1000000, true, &offset))) {
        free_query(fields, 3);
        fail(response, 400, "Ricerca o paginazione non valida.");
        return;
    }
    BridgeAccess access;
    if (!access_list(actor, &access)) { free_query(fields, 3); unavailable(response); return; }
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,code,name,status FROM jobs ORDER BY code COLLATE NOCASE,id LIMIT 10001")) {
        free_query(fields, 3); unavailable(response); return;
    }
    struct json_object *object = json_object_new_object(), *items = json_object_new_array();
    json_object_object_add(object, "items", items);
    DbInt total = 0;
    int result, scanned = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++scanned > CATALOG_LIMIT) break;
        if (!allowed(&access, db_column_int64(statement, 0)) ||
            (!contains_folded(text(statement, 1), search) && !contains_folded(text(statement, 2), search))) continue;
        if (total >= offset && total - offset < limit) json_object_array_add(items, job_row(statement, true));
        total++;
    }
    db_finalize(statement);
    free_query(fields, 3);
    if (scanned > CATALOG_LIMIT || result != DB_DONE) { json_object_put(object); unavailable(response); return; }
    json_object_object_add(object, "total", json_object_new_int64(total));
    finish(response, 200, object);
}

static bool string_member(struct json_object *object, const char *key, const char **out) {
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_string)) return false;
    *out = json_object_get_string(value);
    return strlen(*out) == (size_t)json_object_get_string_len(value);
}

static struct json_object *asset_row(struct json_object *asset, bool with_status) {
    struct json_object *id = NULL;
    const char *tag, *name, *status, *type;
    if (!json_object_is_type(asset, json_type_object) || !json_object_object_get_ex(asset, "id", &id) ||
        !json_object_is_type(id, json_type_int) || json_object_get_int64(id) <= 0 ||
        !string_member(asset, "asset_tag", &tag) || !string_member(asset, "name", &name) ||
        (with_status && (!string_member(asset, "status", &status) || !string_member(asset, "asset_type", &type)))) return NULL;
    struct json_object *object = json_object_new_object();
    if (!object) return NULL;
    json_object_object_add(object, "id", json_object_new_int64(json_object_get_int64(id)));
    json_object_object_add(object, "asset_tag", json_object_new_string(tag));
    json_object_object_add(object, "name", json_object_new_string(name));
    if (with_status) {
        json_object_object_add(object, "status", json_object_new_string(status));
        json_object_object_add(object, "asset_type", json_object_new_string(type));
    }
    return object;
}

static struct json_object *inventory_request(InventoryOperation operation, DbInt id, const char *query,
                                             const Account *actor, WorkspaceResponse *response) {
    InventoryResponse received = {0};
    inventory_client_call(operation, id, 0, actor->id, query, &received);
    struct json_object *object = received.status == 200 && received.body ? json_tokener_parse(received.body) : NULL;
    int status = received.status;
    inventory_response_free(&received);
    if (!object || !json_object_is_type(object, json_type_object)) {
        if (object) json_object_put(object);
        if (status == 404) fail(response, 404, "Asset non trovato.");
        else unavailable(response);
        return NULL;
    }
    return object;
}

static void assets(const char *query, const Account *actor, WorkspaceResponse *response) {
    QueryField fields[] = {{"q", NULL}, {"limit", NULL}, {"after_id", NULL}};
    if (!parse_query(query, fields, 3)) { fail(response, 400, "Parametri inventario non validi."); return; }
    DbInt limit = 30, after = 0;
    bool valid = (!fields[0].value || strlen(fields[0].value) <= 200) &&
        (!fields[1].value || positive(fields[1].value, 100, false, &limit)) &&
        (!fields[2].value || positive(fields[2].value, INT64_MAX, false, &after));
    free_query(fields, 3);
    if (!valid) { fail(response, 400, "Ricerca o paginazione inventario non valida."); return; }
    struct json_object *received = inventory_request(INVENTORY_ASSETS, 0, query, actor, response);
    if (!received) return;
    struct json_object *rows = NULL, *next = NULL;
    valid = json_object_object_get_ex(received, "items", &rows) && json_object_is_type(rows, json_type_array) &&
        json_object_array_length(rows) <= (size_t)limit && json_object_object_get_ex(received, "next_after_id", &next) &&
        (!next || (json_object_is_type(next, json_type_int) && json_object_get_int64(next) > 0));
    struct json_object *object = json_object_new_object(), *items = json_object_new_array();
    json_object_object_add(object, "items", items);
    if (valid) for (size_t i = 0; i < json_object_array_length(rows); i++) {
        struct json_object *row = asset_row(json_object_array_get_idx(rows, i), true);
        if (!row) { valid = false; break; }
        json_object_array_add(items, row);
    }
    if (valid) json_object_object_add(object, "next_after_id", next ? json_object_get(next) : NULL);
    json_object_put(received);
    if (!valid) { json_object_put(object); unavailable(response); return; }
    finish(response, 200, object);
}

static void validate(const char *query, const Account *actor, WorkspaceResponse *response) {
    QueryField fields[] = {{"job_id", NULL}, {"asset_id", NULL}};
    if (!parse_query(query, fields, 2)) { fail(response, 400, "Collegamenti non validi."); return; }
    DbInt job_id = 0, asset_id = 0;
    bool valid = (!fields[0].value || positive(fields[0].value, INT64_MAX, false, &job_id)) &&
        (!fields[1].value || positive(fields[1].value, INT64_MAX, false, &asset_id));
    free_query(fields, 2);
    if (!valid) { fail(response, 400, "Identificativi dei collegamenti non validi."); return; }
    struct json_object *object = json_object_new_object();
    json_object_object_add(object, "job", NULL);
    json_object_object_add(object, "asset", NULL);
    if (job_id) {
        DbStmt *statement = NULL;
        if (!prepare(&statement, "SELECT id,code,name,status FROM jobs WHERE id=?") ||
            db_bind_int64(statement, 1, job_id) != DB_OK) {
            db_finalize(statement); json_object_put(object); unavailable(response); return;
        }
        int result = db_step(statement);
        struct json_object *row = result == DB_ROW ? job_row(statement, false) : NULL;
        db_finalize(statement);
        if (result != DB_ROW) {
            json_object_put(object);
            if (result == DB_DONE) fail(response, 404, "Commessa non trovata.");
            else unavailable(response);
            return;
        }
        BridgeAccess access;
        if (!access_list(actor, &access)) { json_object_put(row); json_object_put(object); unavailable(response); return; }
        if (!allowed(&access, job_id)) {
            json_object_put(row); json_object_put(object);
            fail(response, 403, "Non sei abilitato a questa commessa.");
            return;
        }
        json_object_object_add(object, "job", row);
    }
    if (asset_id) {
        struct json_object *received = inventory_request(INVENTORY_DETAIL, asset_id, "", actor, response);
        if (!received) { json_object_put(object); return; }
        struct json_object *asset = NULL;
        struct json_object *row = json_object_object_get_ex(received, "asset", &asset) ? asset_row(asset, false) : NULL;
        struct json_object *returned_id = NULL;
        if (row && (!json_object_object_get_ex(row, "id", &returned_id) || json_object_get_int64(returned_id) != asset_id)) {
            json_object_put(row);
            row = NULL;
        }
        json_object_put(received);
        if (!row) { json_object_put(object); unavailable(response); return; }
        json_object_object_add(object, "asset", row);
    }
    finish(response, 200, object);
}

void ticket_bridge_read(const char *resource, const char *query,
                        const Account *actor, const char *state, time_t expires, bool calendar_access, bool time_access,
                        WorkspaceResponse *response) {
    if (!database || !actor || !state) { unavailable(response); return; }
    if (!query) query = "";
    if (!strcmp(resource, "scope")) {
        if (*query) fail(response, 400, "Parametri non consentiti.");
        else scope(actor, state, expires, calendar_access, time_access, response);
    } else if (!strcmp(resource, "jobs")) jobs(query, actor, response);
    else if (!strcmp(resource, "assets")) assets(query, actor, response);
    else if (!strcmp(resource, "validate")) validate(query, actor, response);
    else fail(response, 404, "Risorsa non trovata.");
}

/* The grant actor supplies the private calendar identity. A job reference is
   authorization context only and never crosses into the calendar service. */
void ticket_bridge_calendar(const char *resource, const char *input,
                            const Account *actor, WorkspaceResponse *response) {
    static const struct { const char *name; CalendarOperation operation; bool write; } routes[] = {
        {"status", CALENDAR_STATUS, false}, {"events", CALENDAR_EVENTS, false},
        {"changes", CALENDAR_CHANGES, false}, {"event", CALENDAR_EVENT, false},
        {"change", CALENDAR_CHANGE, false}, {"create", CALENDAR_EVENT_CREATE, true},
        {"update", CALENDAR_EVENT_UPDATE, true}, {"delete", CALENDAR_EVENT_DELETE, true},
        {"events/create", CALENDAR_EVENT_CREATE, true}, {"events/update", CALENDAR_EVENT_UPDATE, true},
        {"events/delete", CALENDAR_EVENT_DELETE, true}
    };
    size_t index = 0;
    while (index < sizeof(routes) / sizeof(routes[0]) && strcmp(resource, routes[index].name)) index++;
    if (!database || !actor) { unavailable(response); return; }
    if (index == sizeof(routes) / sizeof(routes[0])) { fail(response, 404, "Risorsa non trovata."); return; }
    if (!input) input = "";
    char *canonical = NULL;
    if (routes[index].write) {
        QueryField fields[] = {{"idempotency_key", NULL}, {"event", NULL}, {"event_id", NULL}, {"etag", NULL}, {"job_id", NULL},
                               {"expected_connection_id", NULL}, {"expected_calendar_id", NULL}};
        if (!parse_fields(input, fields, 7, 32768)) { fail(response, 400, "Parametri calendario non validi."); return; }
        if (fields[4].value) {
            DbInt job;
            if (!positive(fields[4].value, INT64_MAX, false, &job)) {
                free_query(fields, 7); fail(response, 400, "Commessa non valida."); return;
            }
            DbStmt *statement = NULL;
            if (!prepare(&statement, "SELECT 1 FROM jobs WHERE id=?") || db_bind_int64(statement, 1, job) != DB_OK) {
                db_finalize(statement); free_query(fields, 7); unavailable(response); return;
            }
            int found = db_step(statement);
            db_finalize(statement);
            if (found != DB_ROW) {
                free_query(fields, 7);
                if (found == DB_DONE) fail(response, 404, "Commessa non trovata."); else unavailable(response);
                return;
            }
            BridgeAccess access;
            if (!access_list(actor, &access)) { free_query(fields, 7); unavailable(response); return; }
            if (!allowed(&access, job)) {
                free_query(fields, 7); fail(response, 403, "Non sei abilitato a questa commessa."); return;
            }
        }
        canonical = calloc(1, strlen(input) * 3 + 128);
        if (!canonical) { free_query(fields, 7); unavailable(response); return; }
        char *out = canonical;
        const char digits[] = "0123456789ABCDEF";
        for (size_t field = 0; field < 7; field++) if (field != 4 && fields[field].value) {
            if (out != canonical) *out++ = '&';
            size_t name_length = strlen(fields[field].name);
            memcpy(out, fields[field].name, name_length); out += name_length; *out++ = '=';
            for (const unsigned char *p = (const unsigned char *)fields[field].value; *p; p++) {
                if (*p < 128 && (isalnum(*p) || strchr("-_.~", *p))) *out++ = (char)*p;
                else { *out++ = '%'; *out++ = digits[*p >> 4]; *out++ = digits[*p & 15]; }
            }
        }
        *out = 0;
        free_query(fields, 7);
        input = canonical;
    }
    CalendarResponse received = {0};
    calendar_client_call(routes[index].operation, actor->id, input, NULL, &received);
    free(canonical);
    response->status = received.status;
    response->body = received.body;
    received.body = NULL;
    calendar_response_free(&received);
}

/* Hours are recorded as the live delegated Punto actor. Reuse the canonical
   batch path for date-specific ACL, closed periods, audit and idempotency. */
static bool ticket_time_key(const char *key) {
    size_t n = key ? strlen(key) : 0;
    if (n < 8 || n > 100) return false;
    for (size_t i=0;i<n;i++) if (!isalnum((unsigned char)key[i]) && key[i]!='-' && key[i]!='_') return false;
    return true;
}
static char *ticket_time_form(const char *key, const char *entries) {
    size_t n=strlen(entries), k=strlen(key);
    if(n>16000 || k>100) return NULL;
    char *out=malloc(3*n+k+64);
    if(!out) return NULL;
    int head=snprintf(out,3*n+k+64,"request_key=ticket-%s&entries=",key);
    char *p=out+head; const char hexes[]="0123456789ABCDEF";
    for(const unsigned char *s=(const unsigned char*)entries;*s;s++) {
        if(*s<128 && (isalnum(*s)||strchr("-_.~",*s))) *p++=(char)*s;
        else { *p++='%'; *p++=hexes[*s>>4]; *p++=hexes[*s&15]; }
    }
    *p=0; return out;
}
void ticket_bridge_time(const char *resource,const char *input,const Account *actor,WorkspaceResponse *response) {
    if(!database||!actor) {unavailable(response);return;}
    if(!input)input="";
    if(!strcmp(resource,"status")) {
        if(*input){fail(response,400,"Parametri non consentiti.");return;}
        struct json_object *o=json_object_new_object();
        json_object_object_add(o,"enabled",json_object_new_boolean(true));
        json_object_object_add(o,"user_id",json_object_new_int64(actor->id));
        json_object_object_add(o,"actor_name",json_object_new_string(actor->name));
        finish(response,200,o);return;
    }
    if(!strcmp(resource,"create")) {
        QueryField fields[]={{"request_key",NULL},{"entries",NULL}};
        if(!parse_fields(input,fields,2,20000)||!ticket_time_key(fields[0].value)||!fields[1].value) {
            free_query(fields,2);fail(response,400,"Registrazione ore non valida.");return;
        }
        struct json_tokener *tok=json_tokener_new();
        if(!tok){free_query(fields,2);unavailable(response);return;}
        json_tokener_set_flags(tok,JSON_TOKENER_STRICT|JSON_TOKENER_VALIDATE_UTF8);
        struct json_object *rows=json_tokener_parse_ex(tok,fields[1].value,(int)strlen(fields[1].value));
        size_t end=json_tokener_get_parse_end(tok),length=strlen(fields[1].value);
        while(end<length && isspace((unsigned char)fields[1].value[end]))end++;
        bool valid=json_tokener_get_error(tok)==json_tokener_success&&end==length&&rows&&
            json_object_is_type(rows,json_type_array)&&json_object_array_length(rows)==1;
        json_tokener_free(tok);
        struct json_object *row=valid?json_object_array_get_idx(rows,0):NULL,*job_value=NULL;
        valid=valid&&row&&json_object_is_type(row,json_type_object)&&json_object_object_get_ex(row,"job_id",&job_value)&&
            job_value&&json_object_is_type(job_value,json_type_int)&&json_object_get_int64(job_value)>0;
        DbInt job=valid?json_object_get_int64(job_value):0;
        if(rows)json_object_put(rows);
        char *form=valid?ticket_time_form(fields[0].value,fields[1].value):NULL;
        free_query(fields,2);
        if(!valid){fail(response,400,"Conferma una sola registrazione di ore effettive.");return;}
        if(!form){unavailable(response);return;}
        BridgeAccess access;
        if(!access_list(actor,&access)){free(form);unavailable(response);return;}
        if(!allowed(&access,job)){free(form);fail(response,403,"Commessa non autorizzata.");return;}
        bool handled=workspace_handle("POST","/api/time-entries/batch","",form,actor->id,actor->email,actor->role,response);
        free(form);if(!handled)unavailable(response);return;
    }
    if(strcmp(resource,"entries")){fail(response,404,"Risorsa non trovata.");return;}
    QueryField fields[]={{"job_id",NULL},{"request_key",NULL}};
    DbInt job=0;
    if(!parse_query(input,fields,2)||!positive(fields[0].value,INT64_MAX,false,&job)||
       (fields[1].value&&!ticket_time_key(fields[1].value))) {
        free_query(fields,2);fail(response,400,"Parametri ore non validi.");return;
    }
    BridgeAccess access;
    if(!access_list(actor,&access)){free_query(fields,2);unavailable(response);return;}
    if(!allowed(&access,job)){free_query(fields,2);fail(response,403,"Commessa non autorizzata.");return;}
    DbStmt *st=NULL;struct json_object *wanted=NULL;
    bool found=false;
    if(fields[1].value) {
        char key[112];snprintf(key,sizeof(key),"ticket-%s",fields[1].value);
        if(!prepare(&st,"SELECT ids_json FROM time_entry_batches WHERE user_id=? AND request_key=?")||
            db_bind_int64(st,1,actor->id)!=DB_OK||db_bind_text(st,2,key,-1,DB_TRANSIENT)!=DB_OK)goto failure;
        int result=db_step(st);
        if(result==DB_ROW){wanted=json_tokener_parse(text(st,0));found=true;}
        db_finalize(st);st=NULL;
        if((result!=DB_ROW&&result!=DB_DONE)||(found&&(!wanted||!json_object_is_type(wanted,json_type_array)||json_object_array_length(wanted)>100)))goto failure;
    }
    struct json_object *o=json_object_new_object(),*items=json_object_new_array(),*ids=json_object_new_array();
    json_object_object_add(o,"items",items);json_object_object_add(o,"ids",ids);
    json_object_object_add(o,"found",json_object_new_boolean(found));
    if(wanted) {
        for(size_t i=0;i<json_object_array_length(wanted);i++) {
            struct json_object *id=json_object_array_get_idx(wanted,i);
            if(!id||!json_object_is_type(id,json_type_int)||json_object_get_int64(id)<1){json_object_put(o);goto failure;}
            json_object_array_add(ids,json_object_get(id));
        }
    }
    char sql[4600]="SELECT id,job_id,date,minutes,note FROM time_entries WHERE user_id=? AND job_id=?";
    if(fields[1].value) {
        strcat(sql," AND id IN (0");
        if(wanted)for(size_t i=0;i<json_object_array_length(wanted);i++) {
            char number[32];snprintf(number,sizeof(number),",%lld",(long long)json_object_get_int64(json_object_array_get_idx(wanted,i)));
            strcat(sql,number);
        }
        strcat(sql,")");
    }
    strcat(sql," ORDER BY date DESC,id DESC LIMIT 201");
    if(!prepare(&st,sql)||
       db_bind_int64(st,1,actor->id)!=DB_OK||db_bind_int64(st,2,job)!=DB_OK){json_object_put(o);goto failure;}
    int result,count=0;
    while((result=db_step(st))==DB_ROW) {
        DbInt id=db_column_int64(st,0);bool matches=!fields[1].value;
        if(wanted)for(size_t i=0;i<json_object_array_length(wanted);i++)if(json_object_get_int64(json_object_array_get_idx(wanted,i))==id)matches=true;
        if(!matches)continue;
        if(++count>200)break;
        struct json_object *row=json_object_new_object();
        json_object_object_add(row,"id",json_object_new_int64(id));
        json_object_object_add(row,"job_id",json_object_new_int64(db_column_int64(st,1)));
        json_object_object_add(row,"date",json_object_new_string(text(st,2)));
        json_object_object_add(row,"minutes",json_object_new_int64(db_column_int64(st,3)));
        json_object_object_add(row,"note",json_object_new_string(text(st,4)));
        json_object_array_add(items,row);
    }
    db_finalize(st);st=NULL;
    if(result!=DB_DONE&&count<=200){json_object_put(o);goto failure;}
    if(wanted)json_object_put(wanted);
    free_query(fields,2);finish(response,200,o);return;
failure:
    db_finalize(st);if(wanted)json_object_put(wanted);free_query(fields,2);unavailable(response);
}
