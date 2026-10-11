/* Session-local delegation to Punto; no grant or permissions enter the main DB. */
#define PUNTO_VISIBLE_SQL "((t.job_id IS NULL AND (t.asset_id IS NULL OR (SELECT connected FROM punto_access)=1)) OR ((SELECT connected FROM punto_access)=1 AND ((SELECT all_jobs FROM punto_access)=1 OR EXISTS(SELECT 1 FROM punto_visible_jobs p WHERE p.job_id=t.job_id))))"
typedef struct { bool connected, all_jobs; sqlite3_int64 expires_at; char grant[65], state[65], user_name[481]; } PuntoScope;
static bool url_decode(const char *raw, char *out, size_t cap, bool plus);
static bool punto_hex(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return false;
    return true;
}
static bool punto_integer(json_object *o, const char *key, sqlite3_int64 minimum, sqlite3_int64 *out) {
    json_object *v = NULL;
    if (!json_object_object_get_ex(o, key, &v) || !v || !json_object_is_type(v, json_type_int)) return false;
    *out = json_object_get_int64(v);
    return *out >= minimum;
}
static bool punto_clear(Auth *a, bool all) {
    sqlite3_stmt *st = prepare(all ? "DELETE FROM punto_connections WHERE token_hash=?1" : "UPDATE punto_connections SET grant=NULL,connected_state=NULL,expires_at=NULL,user_name=NULL WHERE token_hash=?1");
    if (!st) return false;
    bind_text(st, 1, a->token_hash);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
static bool punto_reset_acl(void) {
    return exec_sql("DELETE FROM punto_visible_jobs; UPDATE punto_access SET connected=0,all_jobs=0");
}
static bool punto_parse_scope(json_object *o, const char *expected, PuntoScope *scope, json_object **ids) {
    const char *state = NULL, *name = NULL;
    json_object *user = NULL, *all = NULL;
    sqlite3_int64 user_id;
    if (!get_string(o, "state", &state, 64, true, true) || !punto_hex(state) || !constant_equal(state, expected) ||
        !json_object_object_get_ex(o, "user", &user) || !json_object_is_type(user, json_type_object) ||
        !punto_integer(user, "id", 1, &user_id) || !get_string(user, "name", &name, 120, true, true) ||
        !json_object_object_get_ex(o, "all_jobs", &all) || !json_object_is_type(all, json_type_boolean) ||
        !punto_integer(o, "expires_at", 1, &scope->expires_at) ||
        !json_object_object_get_ex(o, "job_ids", ids) || !json_object_is_type(*ids, json_type_array) || json_object_array_length(*ids) > 10000) return false;
    for (size_t i = 0; i < json_object_array_length(*ids); i++) {
        json_object *v = json_object_array_get_idx(*ids, i);
        if (!v || !json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 1) return false;
    }
    scope->all_jobs = json_object_get_boolean(all) != 0;
    bounded_copy(scope->state, sizeof(scope->state), state);
    bounded_copy(scope->user_name, sizeof(scope->user_name), name);
    return true;
}
static bool punto_fill_acl(PuntoScope *scope, json_object *ids) {
    if (!punto_reset_acl()) return false;
    if (!scope->connected) return true;
    sqlite3_stmt *st = prepare("UPDATE punto_access SET connected=1,all_jobs=?1");
    if (!st) return false;
    sqlite3_bind_int(st, 1, scope->all_jobs);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) return false;
    st = prepare("INSERT OR IGNORE INTO punto_visible_jobs(job_id) VALUES(?1)");
    if (!st) return false;
    for (size_t i = 0; ok && i < json_object_array_length(ids); i++) {
        sqlite3_bind_int64(st, 1, json_object_get_int64(json_object_array_get_idx(ids, i)));
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return ok;
}
static bool punto_load_scope(Request *r, Auth *a, PuntoScope *scope) {
    memset(scope, 0, sizeof(*scope));
    if (!punto_reset_acl()) { db_error(r); return false; }
    sqlite3_stmt *st = prepare("SELECT grant,connected_state,expires_at FROM punto_connections WHERE token_hash=?1 AND grant IS NOT NULL");
    if (!st) { db_error(r); return false; }
    bind_text(st, 1, a->token_hash);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        bounded_copy(scope->grant, sizeof(scope->grant), column_text(st, 0));
        bounded_copy(scope->state, sizeof(scope->state), column_text(st, 1));
        scope->expires_at = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) return true;
    if (rc != SQLITE_ROW) { db_error(r); return false; }
    if (!punto_client_configured() || scope->expires_at <= (sqlite3_int64)time(NULL)) {
        if (!punto_clear(a, false)) { db_error(r); return false; }
        memset(scope, 0, sizeof(*scope)); return true;
    }
    json_object *response = NULL, *ids = NULL;
    int status = punto_client_get(scope->grant, "scope", NULL, &response);
    if (status == 401 || status == 403 || status == 404) {
        if (response) json_object_put(response);
        if (!punto_clear(a, false)) { db_error(r); return false; }
        memset(scope, 0, sizeof(*scope)); return true;
    }
    char expected[65]; bounded_copy(expected, sizeof(expected), scope->state);
    bool valid = status == 200 && punto_parse_scope(response, expected, scope, &ids);
    if (!valid) { if (response) json_object_put(response); error_response(r, 503, "Punto non è disponibile. Riprova tra poco."); return false; }
    if (scope->expires_at <= (sqlite3_int64)time(NULL)) {
        json_object_put(response);
        if (!punto_clear(a, false)) { db_error(r); return false; }
        memset(scope, 0, sizeof(*scope)); return true;
    }
    scope->connected = true;
    bool ok = punto_fill_acl(scope, ids);
    json_object_put(response);
    if (!ok) { punto_reset_acl(); db_error(r); return false; }
    return true;
}
static bool punto_job_allowed(sqlite3_int64 id, PuntoScope *scope) {
    if (!id || scope->all_jobs) return true;
    sqlite3_stmt *st = prepare("SELECT 1 FROM punto_visible_jobs WHERE job_id=?1");
    if (!st) return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}
static bool punto_ticket_access(Request *r, Auth *a, json_object *ticket) {
    sqlite3_int64 job = json_object_get_int64(json_object_object_get(ticket, "job_id"));
    sqlite3_int64 asset = json_object_get_int64(json_object_object_get(ticket, "asset_id"));
    if (!job && !asset) return true;
    PuntoScope scope;
    if (!punto_load_scope(r, a, &scope)) return false;
    bool allowed = scope.connected && punto_job_allowed(job, &scope);
    OPENSSL_cleanse(&scope, sizeof(scope));
    if (!allowed) error_response(r, 404, "Ticket non trovato.");
    return allowed;
}
static json_object *punto_status_json(PuntoScope *scope) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "configured", json_object_new_boolean(punto_client_configured()));
    json_object_object_add(o, "connected", json_object_new_boolean(scope->connected));
    add_string(o, "user_name", scope->connected ? scope->user_name : NULL);
    if (scope->connected) add_int(o, "expires_at", scope->expires_at);
    else json_object_object_add(o, "expires_at", NULL);
    add_string(o, "origin", punto_client_origin());
    return o;
}
static void punto_status(Request *r, Auth *a) {
    PuntoScope scope;
    if (!punto_load_scope(r, a, &scope)) return;
    send_json(r, 200, punto_status_json(&scope));
    OPENSSL_cleanse(&scope, sizeof(scope));
}
static bool punto_empty_body(Request *r) {
    json_object *input = parse_json(r);
    if (!input) return false;
    bool ok = json_object_object_length(input) == 0;
    json_object_put(input);
    if (!ok) error_response(r, 400, "Dati della richiesta non validi.");
    return ok;
}
static void punto_start(Request *r, Auth *a) {
    if (!punto_empty_body(r)) return;
    if (!punto_client_configured()) { error_response(r, 503, "Il collegamento a Punto non è configurato."); return; }
    char state[65], url[640];
    if (!random_hex(state, 32)) { db_error(r); return; }
    sqlite3_stmt *st = prepare("INSERT INTO punto_connections(token_hash,pending_state,pending_until) VALUES(?1,?2,?3) ON CONFLICT(token_hash) DO UPDATE SET pending_state=excluded.pending_state,pending_until=excluded.pending_until");
    if (!st) { db_error(r); return; }
    bind_text(st, 1, a->token_hash); bind_text(st, 2, state); sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL) + 600);
    bool ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st);
    if (!ok) { db_error(r); return; }
    snprintf(url, sizeof(url), "%s/#ticket-connect?state=%s", punto_client_origin(), state);
    json_object *o = json_object_new_object(); add_string(o, "state", state); add_string(o, "url", url); send_json(r, 200, o);
}
static void punto_connect(Request *r, Auth *a) {
    json_object *input = parse_json(r);
    if (!input) return;
    const char *state = NULL, *grant = NULL;
    const char *const keys[] = {"state", "grant", NULL};
    if (!allowed_keys(input, keys) || !get_string(input, "state", &state, 64, true, true) || !get_string(input, "grant", &grant, 64, true, true) || !punto_hex(state) || !punto_hex(grant)) {
        json_object_put(input); error_response(r, 400, "Collegamento non valido. Avvia nuovamente il collegamento da Ticket."); return;
    }
    sqlite3_stmt *st = prepare("SELECT pending_state,pending_until FROM punto_connections WHERE token_hash=?1");
    if (!st) { json_object_put(input); db_error(r); return; }
    bind_text(st, 1, a->token_hash);
    int rc = sqlite3_step(st);
    bool pending = rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL && constant_equal(column_text(st, 0), state) && sqlite3_column_int64(st, 1) > (sqlite3_int64)time(NULL);
    sqlite3_finalize(st);
    if (!pending) { json_object_put(input); error_response(r, 400, "Il collegamento è scaduto o appartiene a un'altra sessione. Ricomincia da Ticket."); return; }
    json_object *response = NULL, *ids = NULL;
    int status = punto_client_get(grant, "scope", NULL, &response);
    PuntoScope scope = {0};
    bool valid = status == 200 && punto_parse_scope(response, state, &scope, &ids) && scope.expires_at > (sqlite3_int64)time(NULL);
    if (!valid) {
        if (response) json_object_put(response);
        json_object_put(input);
        error_response(r, status == 503 ? 503 : 403, status == 503 ? "Punto non è disponibile. Riprova tra poco." : "Punto non ha autorizzato il collegamento. Ricomincia da Ticket."); return;
    }
    scope.connected = true;
    st = prepare("UPDATE punto_connections SET grant=?1,connected_state=?2,user_name=?3,expires_at=?4,pending_state=NULL,pending_until=NULL WHERE token_hash=?5");
    if (!st) { json_object_put(response); json_object_put(input); db_error(r); return; }
    bind_text(st, 1, grant); bind_text(st, 2, state); bind_text(st, 3, scope.user_name); sqlite3_bind_int64(st, 4, scope.expires_at); bind_text(st, 5, a->token_hash);
    bool ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st);
    json_object_put(response); json_object_put(input);
    if (!ok) { db_error(r); return; }
    send_json(r, 200, punto_status_json(&scope)); OPENSSL_cleanse(&scope, sizeof(scope));
}
static void punto_disconnect(Request *r, Auth *a) {
    if (!punto_empty_body(r)) return;
    if (!punto_clear(a, true) || !punto_reset_acl()) { db_error(r); return; }
    PuntoScope scope = {0}; send_json(r, 200, punto_status_json(&scope));
}
/* Decode, validate and re-encode lookup parameters before crossing the boundary. */
static bool punto_lookup_query(const char *query, const char *endpoint, char out[8192], sqlite3_int64 *job, sqlite3_int64 *asset) {
    char copy[2048]; unsigned seen = 0; size_t used = 0;
    out[0] = 0; *job = *asset = 0;
    if (!query || !*query) return strcmp(endpoint, "validate") != 0;
    size_t query_length = strlen(query);
    if (query_length >= sizeof(copy) || query[0] == '&' || query[query_length - 1] == '&' || strstr(query, "&&")) return false;
    bounded_copy(copy, sizeof(copy), query);
    char *save = NULL;
    for (char *part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(part, '='); if (!eq) return false; *eq++ = 0;
        char key[32], value[1024]; long long number = 0; unsigned bit = 0;
        if (!url_decode(part, key, sizeof(key), true) || !url_decode(eq, value, sizeof(value), true)) return false;
        if ((!strcmp(endpoint, "jobs") && !strcmp(key, "search")) || (!strcmp(endpoint, "assets") && !strcmp(key, "q"))) { bit = 1; if (utf8_length(value) > 200 || (!strcmp(endpoint, "assets") && strlen(value) > 200)) return false; }
        else if (strcmp(endpoint, "validate") && !strcmp(key, "limit")) { bit = 2; if (!parse_uint(value, 100, &number) || number < 1) return false; }
        else if (!strcmp(endpoint, "jobs") && !strcmp(key, "offset")) { bit = 4; if (!parse_uint(value, 1000000, &number)) return false; }
        else if (!strcmp(endpoint, "assets") && !strcmp(key, "after_id")) { bit = 4; if (!parse_uint(value, LLONG_MAX, &number) || number < 1) return false; }
        else if (!strcmp(endpoint, "validate") && !strcmp(key, "job_id")) { bit = 8; if (!parse_uint(value, LLONG_MAX, &number) || number < 1) return false; *job = number; }
        else if (!strcmp(endpoint, "validate") && !strcmp(key, "asset_id")) { bit = 16; if (!parse_uint(value, LLONG_MAX, &number) || number < 1) return false; *asset = number; }
        else return false;
        if (seen & bit) return false;
        seen |= bit;
        size_t need = strlen(key) + strlen(value) * 3 + 3;
        if (need >= 8192 - used) return false;
        if (used) out[used++] = '&';
        memcpy(out + used, key, strlen(key)); used += strlen(key); out[used++] = '=';
        static const char hex[] = "0123456789ABCDEF";
        for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
            if (isalnum(*p) && *p < 128) out[used++] = (char)*p;
            else { out[used++] = '%'; out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15]; }
        }
        out[used] = 0;
    }
    return strcmp(endpoint, "validate") || *job || *asset;
}
static bool punto_reference(json_object *response, const char *key, sqlite3_int64 expected) {
    json_object *item = NULL;
    if (!json_object_object_get_ex(response, key, &item)) return false;
    if (!expected) return item == NULL;
    sqlite3_int64 id;
    const char *name = NULL, *label = NULL;
    return item && json_object_is_type(item, json_type_object) && punto_integer(item, "id", 1, &id) && id == expected &&
        get_string(item, "name", &name, 1000, true, true) && get_string(item, !strcmp(key, "job") ? "code" : "asset_tag", &label, 200, true, true);
}
static bool punto_remote(Request *r, Auth *a, PuntoScope *scope, const char *endpoint, const char *query, json_object **response) {
    if (!scope->connected) { error_response(r, 403, "Collega il tuo account Punto per accedere a commesse e asset."); return false; }
    int status = punto_client_get(scope->grant, endpoint, query, response);
    if (status == 200) return true;
    if (*response) { json_object_put(*response); *response = NULL; }
    if (status == 403 && !strcmp(endpoint, "validate")) {
        error_response(r, 404, "Commessa o asset non disponibile.");
    } else if (status == 401 || status == 403) {
        punto_clear(a, false); punto_reset_acl(); error_response(r, 403, "Il collegamento a Punto non è più autorizzato. Collegalo nuovamente.");
    } else if (status == 404) error_response(r, 404, "Commessa o asset non disponibile.");
    else if (status == 400) error_response(r, 400, "Parametri di ricerca non validi.");
    else error_response(r, 503, "Punto non è disponibile. Riprova tra poco.");
    return false;
}
static bool punto_validate_refs(Request *r, Auth *a, sqlite3_int64 job, sqlite3_int64 asset, json_object **response) {
    *response = NULL;
    if (!job && !asset) return true;
    PuntoScope scope;
    if (!punto_load_scope(r, a, &scope)) return false;
    if (scope.connected && !punto_job_allowed(job, &scope)) { OPENSSL_cleanse(&scope, sizeof(scope)); error_response(r, 404, "Commessa o asset non disponibile."); return false; }
    char query[128];
    if (job && asset) snprintf(query, sizeof(query), "job_id=%lld&asset_id=%lld", (long long)job, (long long)asset);
    else snprintf(query, sizeof(query), "%s=%lld", job ? "job_id" : "asset_id", (long long)(job ? job : asset));
    bool ok = punto_remote(r, a, &scope, "validate", query, response);
    OPENSSL_cleanse(&scope, sizeof(scope));
    if (!ok) return false;
    if (!punto_reference(*response, "job", job) || !punto_reference(*response, "asset", asset)) {
        json_object_put(*response); *response = NULL; error_response(r, 503, "Punto ha restituito dati non validi. Riprova tra poco."); return false;
    }
    return true;
}
static bool punto_catalog_valid(json_object *response, const char *endpoint, PuntoScope *scope) {
    json_object *items = NULL;
    sqlite3_int64 n;
    if (!json_object_object_get_ex(response, "items", &items) || !json_object_is_type(items, json_type_array) || json_object_array_length(items) > 100) return false;
    if (!strcmp(endpoint, "jobs")) { if (!punto_integer(response, "total", 0, &n)) return false; }
    else {
        json_object *next = NULL;
        if (!json_object_object_get_ex(response, "next_after_id", &next) || (next && (!json_object_is_type(next, json_type_int) || json_object_get_int64(next) < 1))) return false;
    }
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        json_object *item = json_object_array_get_idx(items, i); const char *name = NULL, *label = NULL, *status = NULL, *type = NULL;
        if (!item || !json_object_is_type(item, json_type_object) || !punto_integer(item, "id", 1, &n) ||
            !get_string(item, "name", &name, 1000, true, true) || !get_string(item, !strcmp(endpoint, "jobs") ? "code" : "asset_tag", &label, 200, true, true) ||
            !get_string(item, "status", &status, 100, true, true)) return false;
        if (!strcmp(endpoint, "jobs") && !punto_job_allowed(n, scope)) return false;
        if (!strcmp(endpoint, "assets") && !get_string(item, "asset_type", &type, 200, true, false)) return false;
    }
    return true;
}
static void punto_lookup(Request *r, Auth *a, const char *endpoint) {
    char query[8192]; sqlite3_int64 job, asset;
    if (!punto_lookup_query(r->query, endpoint, query, &job, &asset)) { error_response(r, 400, "Parametri di ricerca non validi."); return; }
    if (!strcmp(endpoint, "validate")) {
        json_object *response = NULL;
        if (punto_validate_refs(r, a, job, asset, &response)) send_json(r, 200, response);
        return;
    }
    PuntoScope scope;
    if (!punto_load_scope(r, a, &scope)) return;
    json_object *response = NULL;
    if (!punto_remote(r, a, &scope, endpoint, query, &response)) { OPENSSL_cleanse(&scope, sizeof(scope)); return; }
    bool valid = punto_catalog_valid(response, endpoint, &scope);
    OPENSSL_cleanse(&scope, sizeof(scope));
    if (!valid) { json_object_put(response); error_response(r, 503, "Punto ha restituito dati non validi. Riprova tra poco."); return; }
    send_json(r, 200, response);
}
static bool punto_dispatch(Request *r, Auth *a, bool get, bool post) {
    const char *prefix = "/api/integrations/punto/";
    if (strncmp(r->path, prefix, strlen(prefix))) return false;
    const char *route = r->path + strlen(prefix);
    if (!strcmp(route, "status") && get) punto_status(r, a);
    else if (!strcmp(route, "start") && post) punto_start(r, a);
    else if (!strcmp(route, "connect") && post) punto_connect(r, a);
    else if (!strcmp(route, "disconnect") && post) punto_disconnect(r, a);
    else if ((!strcmp(route, "jobs") || !strcmp(route, "assets") || !strcmp(route, "validate")) && get) punto_lookup(r, a, route);
    else error_response(r, 405, "Metodo non consentito.");
    return true;
}
