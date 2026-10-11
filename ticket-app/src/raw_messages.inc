/* Original email bytes and MIME parts are immutable. Only local workflow changes. */
#define RAW_JOB_VISIBLE_SQL(column)                                                                                     \
    "(" column " IS NULL OR ((SELECT connected FROM punto_access)=1 AND ((SELECT all_jobs FROM punto_access)=1 OR "   \
    "EXISTS(SELECT 1 FROM punto_visible_jobs p WHERE p.job_id=" column "))))"
#define RAW_VISIBLE_SQL                                                                                                \
    "((m.state<>'quarantine') OR ?1=1) AND " RAW_JOB_VISIBLE_SQL("m.job_id")                                              \
    " AND EXISTS(SELECT 1 FROM raw_mailboxes rb WHERE rb.id=m.mailbox_id AND " RAW_JOB_VISIBLE_SQL("rb.job_id") ") AND " \
    "NOT EXISTS(SELECT 1 FROM raw_senders rs WHERE rs.mailbox_id=m.mailbox_id AND rs.email=m.from_email COLLATE NOCASE " \
    "AND NOT " RAW_JOB_VISIBLE_SQL("rs.job_id") ") AND "                                                               \
    "(m.ticket_id IS NULL OR EXISTS(SELECT 1 FROM tickets t WHERE t.id=m.ticket_id AND " PUNTO_VISIBLE_SQL "))"
#define RAW_COLUMNS                                                                                                    \
    "m.id,m.mailbox_id,b.name,m.from_email,m.from_name,m.subject,m.received_at,m.state,m.job_id,m.claimed_by,u.name,"  \
    "m.ticket_id,m.revision,(SELECT count(*) FROM raw_attachments x WHERE "                                            \
    "x.raw_message_id=m.id),m.text_preview,length(m.raw_eml),m.body_text,m.headers_json,m.message_id,m.in_reply_to,m." \
    "references_json,m.auth_status,m.auth_reason,m.quarantine_reason,m.parse_error"
#define RAW_FROM                                                                                                       \
    " FROM raw_messages m JOIN raw_mailboxes b ON b.id=m.mailbox_id LEFT JOIN users u ON u.id=m.claimed_by "
static bool raw_admin(Auth *a) {
    return !strcmp(a->role, "admin");
}
static bool raw_empty_query(Request *r) {
    if (!r->query || !*r->query)
        return true;
    error_response(r, 400, "Parametri non consentiti.");
    return false;
}
static bool raw_scope(Request *r, Auth *a) {
    PuntoScope scope;
    bool ok = punto_load_scope(r, a, &scope);
    OPENSSL_cleanse(&scope, sizeof(scope));
    return ok;
}
/* A local ticket and its shared helpdesk mail must stay readable during a
 * Punto outage. Fetch live permissions only for email with a project binding. */
static bool raw_ticket_scope(Request *r, Auth *a, json_object *ticket) {
    if (json_object_get_int64(json_object_object_get(ticket, "job_id")) ||
        json_object_get_int64(json_object_object_get(ticket, "asset_id")))
        return true; /* punto_ticket_access has already loaded this request's ACL. */
    if (!punto_reset_acl()) {
        db_error(r);
        return false;
    }
    sqlite3_stmt *st = prepare(
        "SELECT EXISTS(SELECT 1 FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id "
        "JOIN raw_mailboxes b ON b.id=m.mailbox_id WHERE l.ticket_id=?1 AND (m.job_id IS NOT NULL OR "
        "b.job_id IS NOT NULL OR EXISTS(SELECT 1 FROM raw_senders s WHERE s.mailbox_id=m.mailbox_id "
        "AND s.email=m.from_email COLLATE NOCASE AND s.job_id IS NOT NULL)))");
    if (!st) {
        db_error(r);
        return false;
    }
    sqlite3_bind_int64(st, 1, json_object_get_int64(json_object_object_get(ticket, "id")));
    int rc = sqlite3_step(st);
    bool protected_mail = rc == SQLITE_ROW && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        db_error(r);
        return false;
    }
    return !protected_mail || raw_scope(r, a);
}
static bool raw_message_scope(Request *r, Auth *a, sqlite3_int64 id) {
    sqlite3_stmt *st = prepare(
        "SELECT (m.job_id IS NOT NULL OR b.job_id IS NOT NULL OR EXISTS(SELECT 1 FROM raw_senders s "
        "WHERE s.mailbox_id=m.mailbox_id AND s.email=m.from_email COLLATE NOCASE AND s.job_id IS NOT NULL) "
        "OR EXISTS(SELECT 1 FROM tickets t WHERE t.id=m.ticket_id AND (t.job_id IS NOT NULL OR t.asset_id IS NOT NULL))) "
        "FROM raw_messages m JOIN raw_mailboxes b ON b.id=m.mailbox_id WHERE m.id=?1");
    if (!st) {
        db_error(r);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    bool protected_mail = rc == SQLITE_ROW && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE)
            error_response(r, 404, "Messaggio non trovato.");
        else
            db_error(r);
        return false;
    }
    if (protected_mail)
        return raw_scope(r, a);
    if (!punto_reset_acl()) {
        db_error(r);
        return false;
    }
    return true;
}
static bool raw_audit(sqlite3_int64 id, Auth *a, const char *action, const char *detail) {
    sqlite3_stmt *st =
        prepare("INSERT INTO raw_history(raw_message_id,actor_id,actor_name,action,detail) VALUES(?1,?2,?3,?4,?5)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, a->id);
    bind_text(st, 3, a->name);
    bind_text(st, 4, action);
    bind_text(st, 5, detail);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
static void raw_nullable_int(json_object *o, const char *key, sqlite3_stmt *st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL)
        json_object_object_add(o, key, NULL);
    else
        add_int(o, key, sqlite3_column_int64(st, col));
}
static json_object *raw_json(sqlite3_stmt *st, bool full) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_int(o, "mailbox_id", sqlite3_column_int64(st, 1));
    add_string(o, "mailbox_name", column_text(st, 2));
    add_string(o, "from_email", column_text(st, 3));
    add_string(o, "from_name", column_text(st, 4));
    add_string(o, "subject", column_text(st, 5));
    add_int(o, "received_at", sqlite3_column_int64(st, 6));
    add_string(o, "state", column_text(st, 7));
    raw_nullable_int(o, "job_id", st, 8);
    raw_nullable_int(o, "claimed_by", st, 9);
    add_string(o, "claimed_name", sqlite3_column_type(st, 10) == SQLITE_NULL ? NULL : column_text(st, 10));
    raw_nullable_int(o, "ticket_id", st, 11);
    add_int(o, "revision", sqlite3_column_int64(st, 12));
    add_int(o, "attachment_count", sqlite3_column_int64(st, 13));
    add_string(o, "text_preview", column_text(st, 14));
    add_int(o, "size_bytes", sqlite3_column_int64(st, 15));
    add_string(o, "auth_status", column_text(st, 21));
    if (full) {
        add_string(o, "body_text", column_text(st, 16));
        add_string(o, "message_id", column_text(st, 18));
        add_string(o, "in_reply_to", column_text(st, 19));
        add_string(o, "auth_reason", column_text(st, 22));
        add_string(o, "quarantine_reason", column_text(st, 23));
        add_string(o, "parse_error", column_text(st, 24));
        json_object *headers = json_tokener_parse(column_text(st, 17)),
                    *references = json_tokener_parse(column_text(st, 20));
        if (!headers)
            headers = json_object_new_array();
        if (!references)
            references = json_object_new_array();
        json_object_object_add(o, "headers", headers);
        json_object_object_add(o, "references", references);
    }
    return o;
}
static json_object *raw_fetch(sqlite3_int64 id, Auth *a, bool full) {
    sqlite3_stmt *st = prepare("SELECT " RAW_COLUMNS RAW_FROM "WHERE m.id=?2 AND " RAW_VISIBLE_SQL);
    if (!st)
        return NULL;
    sqlite3_bind_int(st, 1, raw_admin(a));
    sqlite3_bind_int64(st, 2, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? raw_json(st, full) : NULL;
    sqlite3_finalize(st);
    return o;
}
static bool raw_allowed(sqlite3_int64 id, Auth *a) {
    json_object *o = raw_fetch(id, a, false);
    if (!o)
        return false;
    json_object_put(o);
    return true;
}
/* Customer replies can quote any linked source. Keep the entire correspondence
 * behind every source's current ACL, even when the local ticket stays shared. */
static int raw_correspondence_access(sqlite3_int64 ticket_id, Auth *a) {
    sqlite3_stmt *st = prepare(
        "SELECT EXISTS(SELECT 1 FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id "
        "WHERE l.ticket_id=?2 AND NOT (" RAW_VISIBLE_SQL "))");
    if (!st)
        return -1;
    sqlite3_bind_int(st, 1, raw_admin(a));
    sqlite3_bind_int64(st, 2, ticket_id);
    int rc = sqlite3_step(st);
    int access = rc == SQLITE_ROW ? !sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return access;
}
static json_object *raw_attachment_json(sqlite3_stmt *st, sqlite3_int64 raw_id, bool ticket, sqlite3_int64 ticket_id) {
    json_object *o = json_object_new_object();
    sqlite3_int64 id = sqlite3_column_int64(st, 0);
    char url[160];
    add_int(o, "id", id);
    add_int(o, "raw_message_id", raw_id);
    add_string(o, "filename", column_text(st, 1));
    add_string(o, "content_type", column_text(st, 2));
    add_int(o, "size", sqlite3_column_int64(st, 3));
    add_string(o, "sha256", column_text(st, 4));
    add_string(o, "scan_status", "not_scanned");
    if (ticket)
        snprintf(url, sizeof(url), "/api/tickets/%lld/attachments/%lld", (long long)ticket_id, (long long)id);
    else
        snprintf(url, sizeof(url), "/api/raw/messages/%lld/attachments/%lld", (long long)raw_id, (long long)id);
    add_string(o, "download_url", url);
    return o;
}
static bool raw_sender_authorized(sqlite3_int64 id, sqlite3_int64 ticket_job, sqlite3_int64 *sender_id,
                                  bool require_auth);
static void raw_detail(Request *r, Auth *a, sqlite3_int64 id) {
    if (!raw_empty_query(r) || !raw_message_scope(r, a, id))
        return;
    json_object *raw = raw_fetch(id, a, true);
    if (!raw) {
        error_response(r, 404, "Messaggio non trovato.");
        return;
    }
    sqlite3_int64 sender_id = 0;
    bool sender_ok = raw_sender_authorized(id, 0, &sender_id, false);
    json_object_object_add(raw, "sender_authorized", json_object_new_boolean(sender_ok));
    if (sender_id)
        add_int(raw, "sender_id", sender_id);
    else
        json_object_object_add(raw, "sender_id", NULL);
    json_object *o = json_object_new_object(), *files = json_object_new_array(), *history = json_object_new_array();
    json_object_object_add(o, "raw", raw);
    json_object_object_add(o, "attachments", files);
    json_object_object_add(o, "history", history);
    char url[100];
    snprintf(url, sizeof(url), "/api/raw/messages/%lld/eml", (long long)id);
    add_string(o, "eml_url", url);
    sqlite3_stmt *st =
        prepare("SELECT id,filename,content_type,size,sha256 FROM raw_attachments WHERE raw_message_id=?1 ORDER BY id");
    if (!st)
        goto fail;
    sqlite3_bind_int64(st, 1, id);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        json_object_array_add(files, raw_attachment_json(st, id, false, 0));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto fail;
    st = prepare(
        "SELECT id,actor_name,action,detail,created_at FROM raw_history WHERE raw_message_id=?1 ORDER BY id DESC");
    if (!st)
        goto fail;
    sqlite3_bind_int64(st, 1, id);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json_object *row = json_object_new_object();
        add_int(row, "id", sqlite3_column_int64(st, 0));
        add_string(row, "actor_name", column_text(st, 1));
        add_string(row, "action", column_text(st, 2));
        add_string(row, "detail", column_text(st, 3));
        add_string(row, "created_at", column_text(st, 4));
        json_object_array_add(history, row);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto fail;
    send_json(r, 200, o);
    return;
fail:
    json_object_put(o);
    db_error(r);
}
typedef struct {
    char state[16], search[801];
    sqlite3_int64 mailbox;
    int limit, offset;
} RawFilters;
static bool raw_filters(const char *query, RawFilters *f) {
    memset(f, 0, sizeof(*f));
    strcpy(f->state, "new");
    f->limit = 50;
    if (!query || !*query)
        return true;
    size_t len = strlen(query);
    if (len >= 4096 || query[0] == '&' || query[len - 1] == '&' || strstr(query, "&&"))
        return false;
    char copy[4096];
    bounded_copy(copy, sizeof(copy), query);
    char *save = NULL;
    unsigned seen = 0;
    for (char *part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        char *equal = strchr(part, '=');
        if (!equal)
            return false;
        *equal++ = 0;
        char key[32], value[1024];
        long long number;
        unsigned bit;
        if (!url_decode(part, key, sizeof(key), true) || !url_decode(equal, value, sizeof(value), true))
            return false;
        if (!strcmp(key, "state")) {
            bit = 1;
            const char *states[] = {"all", "new", "reviewing", "converted", "linked", "discarded", "quarantine", NULL};
            bool valid = false;
            for (size_t i = 0; states[i]; i++)
                if (!strcmp(value, states[i]))
                    valid = true;
            if (!valid)
                return false;
            bounded_copy(f->state, sizeof(f->state), value);
        } else if (!strcmp(key, "search")) {
            bit = 2;
            if (utf8_length(value) > 200)
                return false;
            bounded_copy(f->search, sizeof(f->search), value);
        } else if (!strcmp(key, "mailbox_id")) {
            bit = 4;
            if (!parse_uint(value, LLONG_MAX, &number) || number < 1)
                return false;
            f->mailbox = number;
        } else if (!strcmp(key, "limit")) {
            bit = 8;
            if (!parse_uint(value, 100, &number) || number < 1)
                return false;
            f->limit = (int)number;
        } else if (!strcmp(key, "offset")) {
            bit = 16;
            if (!parse_uint(value, 1000000, &number))
                return false;
            f->offset = (int)number;
        } else
            return false;
        if (seen & bit)
            return false;
        seen |= bit;
    }
    return true;
}
#define RAW_FILTER_SQL                                                                                                 \
    " WHERE " RAW_VISIBLE_SQL " AND (?2=0 OR m.mailbox_id=?2) AND (?3='all' OR m.state=?3) AND (?4='' OR "             \
                              "instr(lower(m.subject),lower(?4))>0 OR instr(lower(m.from_email),lower(?4))>0 OR "      \
                              "instr(lower(m.from_name),lower(?4))>0 OR instr(lower(m.text_preview),lower(?4))>0) "
static void raw_bind_filters(sqlite3_stmt *st, Auth *a, RawFilters *f) {
    sqlite3_bind_int(st, 1, raw_admin(a));
    sqlite3_bind_int64(st, 2, f->mailbox);
    bind_text(st, 3, f->state);
    bind_text(st, 4, f->search);
    sqlite3_bind_int(st, 5, f->limit);
    sqlite3_bind_int(st, 6, f->offset);
}
static void raw_list(Request *r, Auth *a) {
    RawFilters f;
    if (!raw_filters(r->query, &f)) {
        error_response(r, 400, "Filtri RAW non validi.");
        return;
    }
    if (!raw_scope(r, a))
        return;
    json_object *o = json_object_new_object(), *items = json_object_new_array(), *stats = json_object_new_object();
    json_object_object_add(o, "items", items);
    json_object_object_add(o, "stats", stats);
    const char *states[] = {"new", "reviewing", "converted", "linked", "discarded", "quarantine"};
    for (size_t i = 0; i < 6; i++)
        add_int(stats, states[i], 0);
    if (!exec_sql("BEGIN"))
        goto fail;
    sqlite3_stmt *st = prepare("SELECT count(*)" RAW_FROM RAW_FILTER_SQL);
    if (!st)
        goto rollback;
    raw_bind_filters(st, a, &f);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        add_int(o, "total", sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW)
        goto rollback;
    st = prepare("SELECT " RAW_COLUMNS RAW_FROM RAW_FILTER_SQL
                 "ORDER BY m.received_at DESC,m.id DESC LIMIT ?5 OFFSET ?6");
    if (!st)
        goto rollback;
    raw_bind_filters(st, a, &f);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        json_object_array_add(items, raw_json(st, false));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto rollback;
    st = prepare("SELECT m.state,count(*)" RAW_FROM "WHERE " RAW_VISIBLE_SQL " GROUP BY m.state");
    if (!st)
        goto rollback;
    sqlite3_bind_int(st, 1, raw_admin(a));
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        add_int(stats, column_text(st, 0), sqlite3_column_int64(st, 1));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto rollback;
    json_object *boxes = json_object_new_array();
    json_object_object_add(o, "mailboxes", boxes);
    st = prepare("SELECT b.id,b.name FROM raw_mailboxes b WHERE b.job_id IS NULL OR ((SELECT connected FROM "
                 "punto_access)=1 AND ((SELECT all_jobs FROM punto_access)=1 OR EXISTS(SELECT 1 FROM "
                 "punto_visible_jobs p WHERE p.job_id=b.job_id))) ORDER BY b.name,b.id");
    if (!st)
        goto rollback;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json_object *box = json_object_new_object();
        add_int(box, "id", sqlite3_column_int64(st, 0));
        add_string(box, "name", column_text(st, 1));
        json_object_array_add(boxes, box);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || !exec_sql("COMMIT"))
        goto rollback;
    send_json(r, 200, o);
    return;
rollback:
    exec_sql("ROLLBACK");
fail:
    json_object_put(o);
    db_error(r);
}
static bool raw_no_controls(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p == 127)
            return false;
    return true;
}
static bool raw_secret_name(const char *s) {
    if (!*s || strlen(s) > 80 || s[0] == '.' || strstr(s, ".."))
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("_.-", *s))
            return false;
    return true;
}
static bool raw_host(const char *s) {
    if (!*s || strlen(s) > 253)
        return false;
    if (strchr(s, ':')) {
        struct in6_addr address;
        return inet_pton(AF_INET6, s, &address) == 1;
    }
    if (s[0] == '.' || s[0] == '-' || s[strlen(s) - 1] == '.' || s[strlen(s) - 1] == '-' || strstr(s, ".."))
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-')
            return false;
    return true;
}
static bool raw_optional_id(json_object *o, const char *key, sqlite3_int64 *number) {
    json_object *v = NULL;
    if (!json_object_object_get_ex(o, key, &v))
        return true;
    if (!v) {
        *number = 0;
        return true;
    }
    if (!json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 1)
        return false;
    *number = json_object_get_int64(v);
    return true;
}
static bool raw_optional_bool(json_object *o, const char *key, bool *boolean) {
    json_object *v = NULL;
    if (!json_object_object_get_ex(o, key, &v))
        return true;
    if (!v || !json_object_is_type(v, json_type_boolean))
        return false;
    *boolean = json_object_get_boolean(v) != 0;
    return true;
}
static json_object *raw_mailbox(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    const char *names[] = {"name", "address", "host", "username", "folder", "secret_name", "auth_mode", "authserv_id"};
    int columns[] = {1, 2, 3, 5, 6, 7, 8, 9};
    add_int(o, "id", sqlite3_column_int64(st, 0));
    for (size_t i = 0; i < 8; i++)
        add_string(o, names[i], column_text(st, columns[i]));
    add_int(o, "port", sqlite3_column_int(st, 4));
    json_object_object_add(o, "trust_auth_results", json_object_new_boolean(sqlite3_column_int(st, 10)));
    json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 11)));
    raw_nullable_int(o, "job_id", st, 12);
    add_int(o, "revision", sqlite3_column_int64(st, 13));
    json_object *worker = json_object_new_object();
    raw_nullable_int(worker, "last_polled_at", st, 14);
    add_string(worker, "last_error", column_text(st, 15));
    json_object_object_add(o, "worker", worker);
    return o;
}
#define MAILBOX_SELECT                                                                                                 \
    "SELECT "                                                                                                          \
    "b.id,b.name,b.address,b.host,b.port,b.username,b.folder,b.secret_name,b.auth_mode,b.authserv_id,b.trust_auth_"    \
    "results,b.active,b.job_id,b.revision,(SELECT last_polled_at FROM raw_mailbox_state s WHERE s.mailbox_id=b.id "    \
    "ORDER BY last_polled_at DESC LIMIT 1),coalesce((SELECT last_error FROM raw_mailbox_state s WHERE "                \
    "s.mailbox_id=b.id ORDER BY last_polled_at DESC LIMIT 1),'') FROM raw_mailboxes b "
static json_object *raw_mailbox_fetch(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare(MAILBOX_SELECT "WHERE b.id=?1");
    if (!st)
        return NULL;
    sqlite3_bind_int64(st, 1, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? raw_mailbox(st) : NULL;
    sqlite3_finalize(st);
    return o;
}
static json_object *raw_sender(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_int(o, "mailbox_id", sqlite3_column_int64(st, 1));
    add_string(o, "email", column_text(st, 2));
    add_string(o, "name", column_text(st, 3));
    json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 4)));
    raw_nullable_int(o, "valid_until", st, 5);
    raw_nullable_int(o, "job_id", st, 6);
    add_int(o, "revision", sqlite3_column_int64(st, 7));
    return o;
}
#define SENDER_SELECT "SELECT id,mailbox_id,email,name,active,valid_until,job_id,revision FROM raw_senders "
static json_object *raw_sender_fetch(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare(SENDER_SELECT "WHERE id=?1");
    if (!st)
        return NULL;
    sqlite3_bind_int64(st, 1, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? raw_sender(st) : NULL;
    sqlite3_finalize(st);
    return o;
}
static void raw_config_list(Request *r, bool mailboxes) {
    sqlite3_int64 mailbox = 0;
    if (r->query && *r->query) {
        long long value;
        if (mailboxes || strncmp(r->query, "mailbox_id=", 11) || !parse_uint(r->query + 11, LLONG_MAX, &value) ||
            value < 1) {
            error_response(r, 400, "Filtro casella non valido.");
            return;
        }
        mailbox = value;
    }
    sqlite3_stmt *st = prepare(mailboxes ? MAILBOX_SELECT "ORDER BY b.id"
                                         : SENDER_SELECT "WHERE (?1=0 OR mailbox_id=?1) ORDER BY mailbox_id,email,id");
    if (!st) {
        db_error(r);
        return;
    }
    if (!mailboxes)
        sqlite3_bind_int64(st, 1, mailbox);
    json_object *o = json_object_new_object(), *items = json_object_new_array();
    json_object_object_add(o, "items", items);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        json_object_array_add(items, mailboxes ? raw_mailbox(st) : raw_sender(st));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_object_put(o);
        db_error(r);
        return;
    }
    send_json(r, 200, o);
}
static const char *raw_config_string(json_object *old, const char *key, const char *fallback) {
    json_object *v = old ? json_object_object_get(old, key) : NULL;
    return v ? json_object_get_string(v) : fallback;
}
static void raw_config_save(Request *r, Auth *a, sqlite3_int64 id, bool mailbox) {
    if (!raw_empty_query(r))
        return;
    json_object *input = parse_json(r), *old = NULL, *refs = NULL;
    if (!input)
        return;
    if (id) {
        old = mailbox ? raw_mailbox_fetch(id) : raw_sender_fetch(id);
        if (!old) {
            json_object_put(input);
            error_response(r, 404, "Configurazione non trovata.");
            return;
        }
    }
    const char *const mbkeys[] = {"name",   "address",     "host",      "port",        "username",
                                  "folder", "secret_name", "auth_mode", "authserv_id", "trust_auth_results",
                                  "active", "job_id",      "revision",  NULL};
    const char *const skeys[] = {"mailbox_id", "email", "name", "active", "valid_until", "job_id", "revision", NULL};
    sqlite3_int64 revision = 0, job = old ? json_object_get_int64(json_object_object_get(old, "job_id")) : 0,
                  valid_until = old ? json_object_get_int64(json_object_object_get(old, "valid_until")) : 0,
                  box = old ? json_object_get_int64(json_object_object_get(old, "mailbox_id")) : 0;
    bool active = old ? json_object_get_boolean(json_object_object_get(old, "active")) != 0 : !mailbox;
    if (!allowed_keys(input, mailbox ? mbkeys : skeys) || !raw_optional_id(input, "job_id", &job) ||
        !raw_optional_bool(input, "active", &active) || (id && !punto_integer(input, "revision", 1, &revision)))
        goto invalid;
    if (!id && json_object_object_get(input, "revision"))
        goto invalid;
    if (job && !punto_validate_refs(r, a, job, 0, &refs))
        goto done;
    const char *name = raw_config_string(old, "name", mailbox ? NULL : ""),
               *email = raw_config_string(old, mailbox ? "address" : "email", NULL);
    if (!get_string(input, "name", &name, 100, mailbox && !old, mailbox) ||
        !get_string(input, mailbox ? "address" : "email", &email, 254, !old, true) || !name || !email ||
        !raw_no_controls(name))
        goto invalid;
    char normalized[255];
    if (!email_normalize(email, normalized))
        goto invalid;
    sqlite3_stmt *st = NULL;
    if (mailbox) {
        const char *host = raw_config_string(old, "host", ""), *username = raw_config_string(old, "username", ""),
                   *folder = raw_config_string(old, "folder", "INBOX"),
                   *secret = raw_config_string(old, "secret_name", ""),
                   *mode = raw_config_string(old, "auth_mode", "password"),
                   *authserv = raw_config_string(old, "authserv_id", "");
        bool trust = old ? json_object_get_boolean(json_object_object_get(old, "trust_auth_results")) != 0 : false;
        sqlite3_int64 port = old ? json_object_get_int64(json_object_object_get(old, "port")) : 993;
        json_object *v = NULL;
        if (!get_string(input, "host", &host, 253, false, false) ||
            !get_string(input, "username", &username, 255, false, false) ||
            !get_string(input, "folder", &folder, 200, false, true) ||
            !get_string(input, "secret_name", &secret, 80, false, false) ||
            !get_string(input, "auth_mode", &mode, 16, false, true) ||
            !get_string(input, "authserv_id", &authserv, 253, false, false) ||
            !raw_optional_bool(input, "trust_auth_results", &trust))
            goto invalid;
        if (json_object_object_get_ex(input, "port", &v)) {
            if (!v || !json_object_is_type(v, json_type_int))
                goto invalid;
            port = json_object_get_int64(v);
        }
        if (port < 1 || port > 65535 || (*host && !raw_host(host)) || (*secret && !raw_secret_name(secret)) ||
            (*authserv && !raw_host(authserv)) || !raw_no_controls(username) || !raw_no_controls(folder) ||
            (strcmp(mode, "password") && strcmp(mode, "xoauth2")) || (trust && !*authserv) ||
            (active && (!*host || !*username || !*secret || !*folder)))
            goto invalid;
        /* Authentication-Results uses a case-insensitive DNS authority. */
        char canonical_authserv[254];
        size_t authserv_length = strlen(authserv);
        for (size_t i = 0; i <= authserv_length; i++)
            canonical_authserv[i] = (char)tolower((unsigned char)authserv[i]);
        st = prepare(
            id ? "UPDATE raw_mailboxes SET "
                 "name=?1,address=?2,host=?3,port=?4,username=?5,folder=?6,secret_name=?7,auth_mode=?8,authserv_id=?9,"
                 "trust_auth_results=?10,active=?11,job_id=?12,revision=revision+1 WHERE id=?13 AND revision=?14"
               : "INSERT INTO "
                 "raw_mailboxes(name,address,host,port,username,folder,secret_name,auth_mode,authserv_id,trust_auth_"
                 "results,active,job_id) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
        if (!st)
            goto database_error;
        bind_text(st, 1, name);
        bind_text(st, 2, normalized);
        bind_text(st, 3, host);
        sqlite3_bind_int64(st, 4, port);
        bind_text(st, 5, username);
        bind_text(st, 6, folder);
        bind_text(st, 7, secret);
        bind_text(st, 8, mode);
        bind_text(st, 9, canonical_authserv);
        sqlite3_bind_int(st, 10, trust);
        sqlite3_bind_int(st, 11, active);
        if (job)
            sqlite3_bind_int64(st, 12, job);
        else
            sqlite3_bind_null(st, 12);
        if (id) {
            sqlite3_bind_int64(st, 13, id);
            sqlite3_bind_int64(st, 14, revision);
        }
    } else {
        if (!raw_optional_id(input, "valid_until", &valid_until))
            goto invalid;
        if (id && json_object_object_get(input, "email"))
            goto invalid;
        json_object *mailbox_value = NULL;
        if (id && json_object_object_get_ex(input, "mailbox_id", &mailbox_value) &&
            (!mailbox_value || !json_object_is_type(mailbox_value, json_type_int) ||
             json_object_get_int64(mailbox_value) != box))
            goto invalid;
        if (!id && !punto_integer(input, "mailbox_id", 1, &box))
            goto invalid;
        json_object *mb = raw_mailbox_fetch(box);
        if (!mb)
            goto invalid;
        sqlite3_int64 bound = json_object_get_int64(json_object_object_get(mb, "job_id"));
        json_object_put(mb);
        if (job && bound && job != bound)
            goto invalid;
        st = prepare(
            id ? "UPDATE raw_senders SET name=?1,active=?2,valid_until=?3,job_id=?4,revision=revision+1 WHERE id=?5 "
                 "AND revision=?6"
               : "INSERT INTO raw_senders(name,active,valid_until,job_id,mailbox_id,email) VALUES(?1,?2,?3,?4,?5,?6)");
        if (!st)
            goto database_error;
        bind_text(st, 1, name);
        sqlite3_bind_int(st, 2, active);
        if (valid_until)
            sqlite3_bind_int64(st, 3, valid_until);
        else
            sqlite3_bind_null(st, 3);
        if (job)
            sqlite3_bind_int64(st, 4, job);
        else
            sqlite3_bind_null(st, 4);
        if (id) {
            sqlite3_bind_int64(st, 5, id);
            sqlite3_bind_int64(st, 6, revision);
        } else {
            sqlite3_bind_int64(st, 5, box);
            bind_text(st, 6, normalized);
        }
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            error_response(r, 409, "Mittente già presente o dati in conflitto.");
            goto done;
        }
        goto database_error;
    }
    if (id && sqlite3_changes(db) != 1) {
        error_response(r, 409, "La configurazione è stata modificata. Ricaricala.");
        goto done;
    }
    bool created = !id;
    if (created)
        id = sqlite3_last_insert_rowid(db);
    json_object *result = mailbox ? raw_mailbox_fetch(id) : raw_sender_fetch(id);
    if (!result)
        goto database_error;
    json_object *o = json_object_new_object();
    json_object_object_add(o, mailbox ? "mailbox" : "sender", result);
    send_json(r, created ? 201 : 200, o);
    goto done;
invalid:
    error_response(r, 400, "Configurazione non valida. Controlla indirizzi, casella e vincoli della commessa.");
    goto done;
database_error:
    db_error(r);
done:
    if (refs)
        json_object_put(refs);
    if (old)
        json_object_put(old);
    json_object_put(input);
}
static void raw_download(Request *r, Auth *a, sqlite3_int64 raw_id, sqlite3_int64 attachment, sqlite3_int64 ticket_id) {
    if (!raw_empty_query(r))
        return;
    if (ticket_id) {
        json_object *ticket = fetch_ticket(ticket_id);
        if (!ticket) {
            error_response(r, 404, "Allegato non trovato.");
            return;
        }
        bool access = punto_ticket_access(r, a, ticket);
        json_object_put(ticket);
        if (!access)
            return;
        sqlite3_stmt *link = prepare("SELECT x.raw_message_id FROM ticket_raw_attachments l JOIN raw_attachments x ON "
                                     "x.id=l.raw_attachment_id WHERE l.ticket_id=?1 AND x.id=?2");
        if (!link) {
            db_error(r);
            return;
        }
        sqlite3_bind_int64(link, 1, ticket_id);
        sqlite3_bind_int64(link, 2, attachment);
        raw_id = sqlite3_step(link) == SQLITE_ROW ? sqlite3_column_int64(link, 0) : 0;
        sqlite3_finalize(link);
    }
    if (!raw_id) {
        error_response(r, 404, "Allegato o messaggio non trovato.");
        return;
    }
    if (!raw_message_scope(r, a, raw_id))
        return;
    if (!raw_allowed(raw_id, a)) {
        error_response(r, 404, "Allegato o messaggio non trovato.");
        return;
    }
    sqlite3_stmt *st =
        prepare(attachment ? "SELECT content,filename FROM raw_attachments WHERE raw_message_id=?1 AND id=?2"
                           : "SELECT raw_eml,'messaggio.eml' FROM raw_messages WHERE id=?1");
    if (!st) {
        db_error(r);
        return;
    }
    sqlite3_bind_int64(st, 1, raw_id);
    if (attachment)
        sqlite3_bind_int64(st, 2, attachment);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        error_response(r, 404, "Allegato non trovato.");
        return;
    }
    const char *original = column_text(st, 1);
    char filename[181];
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)original; *p && used < sizeof(filename) - 1; p++)
        filename[used++] =
            (*p >= 32 && *p < 127 && *p != '"' && *p != '\\' && *p != '/' && *p != ';' && *p != '%') ? (char)*p : '_';
    filename[used] = 0;
    if (!used)
        strcpy(filename, attachment ? "allegato.bin" : "messaggio.eml");
    char extra[260];
    snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
    response(r, 200, "application/octet-stream", sqlite3_column_blob(st, 0), (size_t)sqlite3_column_bytes(st, 0),
             extra);
    sqlite3_finalize(st);
}
static void raw_workflow(Request *r, Auth *a, sqlite3_int64 id, const char *action) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *input = parse_json(r);
    if (!input)
        return;
    const char *const keys[] = {"revision", "reason", NULL};
    sqlite3_int64 revision;
    const char *reason = "";
    if (!allowed_keys(input, keys) || !punto_integer(input, "revision", 1, &revision) ||
        !get_string(input, "reason", &reason, 500, false, false)) {
        json_object_put(input);
        error_response(r, 400, "Azione RAW non valida.");
        return;
    }
    if (!strcmp(action, "discard") && (!*reason || whitespace_only(reason))) {
        json_object_put(input);
        error_response(r, 400, "Indica il motivo dello scarto.");
        return;
    }
    if (!exec_sql("BEGIN IMMEDIATE")) {
        json_object_put(input);
        db_error(r);
        return;
    }
    json_object *raw = raw_fetch(id, a, false);
    if (!raw) {
        exec_sql("ROLLBACK");
        json_object_put(input);
        error_response(r, 404, "Messaggio non trovato.");
        return;
    }
    const char *state = json_object_get_string(json_object_object_get(raw, "state"));
    sqlite3_int64 owner = json_object_get_int64(json_object_object_get(raw, "claimed_by"));
    if (revision != json_object_get_int64(json_object_object_get(raw, "revision"))) {
        error_response(r, 409, "Il messaggio è stato aggiornato. Ricaricalo.");
        goto rollback;
    }
    bool claim = !strcmp(action, "claim"), release = !strcmp(action, "release"), discard = !strcmp(action, "discard");
    if ((strcmp(state, "new") && strcmp(state, "reviewing")) || (!raw_admin(a) && owner && owner != a->id) ||
        (release && owner != a->id && !raw_admin(a))) {
        error_response(r, 409, "Il messaggio è già elaborato, in quarantena o preso in carico da un'altra persona.");
        goto rollback;
    }
    if (release && strcmp(state, "reviewing")) {
        error_response(r, 409, "Il messaggio non è preso in carico.");
        goto rollback;
    }
    if (!claim && !release && !discard) {
        error_response(r, 400, "Azione RAW non valida.");
        goto rollback;
    }
    sqlite3_stmt *st =
        prepare("UPDATE raw_messages SET state=?1,claimed_by=?2,revision=revision+1 WHERE id=?3 AND revision=?4");
    if (!st)
        goto fail;
    bind_text(st, 1, claim ? "reviewing" : release ? "new" : "discarded");
    if (claim)
        sqlite3_bind_int64(st, 2, a->id);
    else
        sqlite3_bind_null(st, 2);
    sqlite3_bind_int64(st, 3, id);
    sqlite3_bind_int64(st, 4, revision);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1 ||
        !raw_audit(id, a, action,
                   *reason   ? reason
                   : claim   ? "Messaggio preso in carico."
                   : release ? "Messaggio restituito alla coda."
                             : "Messaggio scartato.") ||
        !exec_sql("COMMIT"))
        goto fail;
    json_object_put(raw);
    json_object_put(input);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "raw", raw_fetch(id, a, false));
    send_json(r, 200, o);
    return;
fail:
    db_error(r);
rollback:
    exec_sql("ROLLBACK");
    json_object_put(raw);
    json_object_put(input);
}
/* Called under the same write lock that creates the ticket/link. */
static bool raw_sender_authorized(sqlite3_int64 id, sqlite3_int64 ticket_job, sqlite3_int64 *sender_id,
                                  bool require_auth) {
    sqlite3_stmt *st =
        prepare("SELECT "
                "s.id,m.job_id,b.job_id,s.job_id,m.auth_status,m.authserv_id,b.authserv_id,b.trust_auth_results,m."
                "parse_error,m.source FROM raw_messages m JOIN raw_mailboxes b ON b.id=m.mailbox_id JOIN raw_senders s "
                "ON s.mailbox_id=m.mailbox_id AND s.email=m.from_email COLLATE NOCASE WHERE m.id=?1 AND b.active=1 AND "
                "s.active=1 AND (s.valid_until IS NULL OR s.valid_until>?2)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    bool allowed = sqlite3_step(st) == SQLITE_ROW;
    if (allowed) {
        *sender_id = sqlite3_column_int64(st, 0);
        if (require_auth) {
            for (int col = 1; col <= 3; col++) {
                sqlite3_int64 bound = sqlite3_column_int64(st, col);
                if (bound && bound != ticket_job)
                    allowed = false;
            }
            allowed = allowed && !strcmp(column_text(st, 4), "pass") &&
                      !strcmp(column_text(st, 5), column_text(st, 6)) && *column_text(st, 6) &&
                      sqlite3_column_int(st, 7) && !*column_text(st, 8) && !strcmp(column_text(st, 9), "imap");
        }
    }
    sqlite3_finalize(st);
    return allowed;
}
static bool raw_selected_attachments(json_object *input, sqlite3_int64 raw_id, sqlite3_int64 ids[20], size_t *count) {
    *count = 0;
    json_object *files = NULL;
    if (!json_object_object_get_ex(input, "attachment_ids", &files))
        return true;
    if (!files || !json_object_is_type(files, json_type_array) || json_object_array_length(files) > 20)
        return false;
    sqlite3_stmt *st = prepare("SELECT size FROM raw_attachments WHERE id=?1 AND raw_message_id=?2");
    if (!st)
        return false;
    sqlite3_int64 total = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < json_object_array_length(files); i++) {
        json_object *v = json_object_array_get_idx(files, i);
        if (!v || !json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 1) {
            ok = false;
            break;
        }
        ids[i] = json_object_get_int64(v);
        for (size_t j = 0; j < i; j++)
            if (ids[j] == ids[i])
                ok = false;
        sqlite3_bind_int64(st, 1, ids[i]);
        sqlite3_bind_int64(st, 2, raw_id);
        if (sqlite3_step(st) != SQLITE_ROW)
            ok = false;
        else {
            sqlite3_int64 size = sqlite3_column_int64(st, 0);
            if (size < 0 || size > 5242880)
                ok = false;
            total += size;
            if (total > 10485760)
                ok = false;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);
    if (ok)
        *count = json_object_array_length(files);
    return ok;
}
static void raw_convert(Request *r, Auth *a, sqlite3_int64 id, bool linking) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *input = parse_json(r), *raw = NULL, *ticket = NULL, *draft = NULL;
    TicketFields fields = {0};
    bool transaction = false;
    if (!input)
        return;
    const char *const create_keys[] = {"revision", "ticket", "attachment_ids", NULL}, *const link_keys[] = {
                                                                                          "revision", "ticket_id",
                                                                                          "attachment_ids", NULL};
    sqlite3_int64 revision, ticket_id = 0;
    if (!allowed_keys(input, linking ? link_keys : create_keys) || !punto_integer(input, "revision", 1, &revision) ||
        (linking && !punto_integer(input, "ticket_id", 1, &ticket_id))) {
        error_response(r, 400, "Conversione RAW non valida.");
        goto done;
    }
    raw = raw_fetch(id, a, true);
    if (!raw) {
        error_response(r, 404, "Messaggio non trovato.");
        goto done;
    }
    sqlite3_int64 original_ticket = json_object_get_int64(json_object_object_get(raw, "ticket_id"));
    const char *initial_state = json_object_get_string(json_object_object_get(raw, "state"));
    bool replay = original_ticket && (!strcmp(initial_state, linking ? "linked" : "converted")) &&
                  (!linking || ticket_id == original_ticket);
    if (replay)
        ticket_id = original_ticket;
    if (linking || replay) {
        ticket = fetch_ticket(ticket_id);
        if (!ticket) {
            error_response(r, 404, "Ticket non trovato.");
            goto done;
        }
        if (!punto_ticket_access(r, a, ticket))
            goto done;
    } else {
        json_object *provided = NULL;
        if (!json_object_object_get_ex(input, "ticket", &provided) || !provided ||
            !json_object_is_type(provided, json_type_object)) {
            error_response(r, 400, "Indica i dati del ticket da creare.");
            goto done;
        }
        draft = json_tokener_parse(json_object_to_json_string_ext(provided, JSON_C_TO_STRING_PLAIN));
        sqlite3_int64 bound = json_object_get_int64(json_object_object_get(raw, "job_id"));
        if (bound && !json_object_object_get(draft, "job_id"))
            add_int(draft, "job_id", bound);
        if (!ticket_fields(draft, NULL, &fields, false)) {
            error_response(r, 400, "Dati del ticket non validi.");
            goto done;
        }
        if (!ticket_resolve_references(r, a, NULL, &fields))
            goto done;
    }
    /* HTTP calls have finished. Re-read workflow and sender policy while locked. */
    if (!exec_sql("BEGIN IMMEDIATE")) {
        db_error(r);
        goto done;
    }
    transaction = true;
    json_object_put(raw);
    raw = raw_fetch(id, a, true);
    if (!raw) {
        error_response(r, 404, "Messaggio non trovato.");
        goto done;
    }
    sqlite3_int64 current_ticket = json_object_get_int64(json_object_object_get(raw, "ticket_id"));
    const char *state = json_object_get_string(json_object_object_get(raw, "state"));
    bool current_replay =
        current_ticket && !strcmp(state, linking ? "linked" : "converted") && (!linking || ticket_id == current_ticket);
    if (current_replay && (!ticket || ticket_id != current_ticket)) {
        if (ticket)
            json_object_put(ticket);
        ticket = fetch_ticket(current_ticket);
        ticket_id = current_ticket;
        if (!ticket) {
            db_error(r);
            goto done;
        }
    }
    sqlite3_int64 job = (linking || replay || current_replay)
                            ? json_object_get_int64(json_object_object_get(ticket, "job_id"))
                            : fields.job_id,
                  sender_id = 0;
    if (!raw_sender_authorized(id, job, &sender_id, true)) {
        error_response(r, 403,
                       "Mittente, verifica del provider o vincolo della commessa non più autorizzati. Nessun ticket è "
                       "stato modificato.");
        goto done;
    }
    if (current_replay) {
        /* A concurrent conversion wins; its original ticket is the only result. */
        if (!linking && (!ticket || ticket_id != current_ticket)) {
            if (ticket)
                json_object_put(ticket);
            ticket = fetch_ticket(current_ticket);
            ticket_id = current_ticket;
        }
        if (!ticket) {
            db_error(r);
            goto done;
        }
        if (!exec_sql("COMMIT")) {
            db_error(r);
            goto done;
        }
        transaction = false;
        json_object *o = json_object_new_object();
        json_object_object_add(o, "ticket", json_object_get(ticket));
        json_object_object_add(o, "raw", json_object_get(raw));
        json_object_object_add(o, "replayed", json_object_new_boolean(true));
        send_json(r, 200, o);
        goto done;
    }
    if (current_ticket || strcmp(state, "reviewing") ||
        json_object_get_int64(json_object_object_get(raw, "claimed_by")) != a->id ||
        revision != json_object_get_int64(json_object_object_get(raw, "revision"))) {
        error_response(r, 409, "Prendi in carico il messaggio aggiornato prima di convertirlo o collegarlo.");
        goto done;
    }
    sqlite3_int64 selected[20];
    size_t selected_count;
    if (!raw_selected_attachments(input, id, selected, &selected_count)) {
        error_response(r, 400, "Selezione allegati non valida.");
        goto done;
    }
    sqlite3_stmt *st = NULL;
    if (!linking) {
        if (!assignee_active(fields.assignee_id)) {
            error_response(r, 400, "L'assegnatario deve essere attivo.");
            goto done;
        }
        st = prepare("INSERT INTO "
                     "tickets(title,description,requester,status,priority,assignee_id,due_date,created_by,job_id,job_"
                     "code,job_name,asset_id,asset_tag,asset_name,scheduled_start,scheduled_end,scheduled_timezone) "
                     "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17)");
        if (!st)
            goto database_error;
        bind_ticket(st, &fields);
        sqlite3_bind_int64(st, 8, a->id);
        bind_references(st, &fields, 9);
        bind_schedule(st, &fields, 15);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE)
            goto database_error;
        ticket_id = sqlite3_last_insert_rowid(db);
        if (!operations_apply(ticket_id,a,draft,NULL,&fields) || !add_history(ticket_id, a, "created", "Ticket creato da email RAW verificata."))
            goto database_error;
    } else {
        st = prepare("UPDATE tickets SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                     "id=?1 AND revision=?2");
        if (!st)
            goto database_error;
        sqlite3_bind_int64(st, 1, ticket_id);
        sqlite3_bind_int64(st, 2, json_object_get_int64(json_object_object_get(ticket, "revision")));
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE || sqlite3_changes(db) != 1) {
            error_response(r, 409, "Il ticket è stato modificato. Ricaricalo prima di collegare l'email.");
            goto done;
        }
    }
    st = prepare("INSERT INTO ticket_raw_links(raw_message_id,ticket_id,actor_id,action) VALUES(?1,?2,?3,?4)");
    if (!st)
        goto database_error;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, ticket_id);
    sqlite3_bind_int64(st, 3, a->id);
    bind_text(st, 4, linking ? "linked" : "converted");
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto database_error;
    for (size_t i = 0; i < selected_count; i++) {
        st = prepare("INSERT INTO ticket_raw_attachments(ticket_id,raw_attachment_id) VALUES(?1,?2)");
        if (!st)
            goto database_error;
        sqlite3_bind_int64(st, 1, ticket_id);
        sqlite3_bind_int64(st, 2, selected[i]);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE)
            goto database_error;
    }
    st = prepare("UPDATE raw_messages SET state=?1,ticket_id=?2,revision=revision+1 WHERE id=?3 AND revision=?4");
    if (!st)
        goto database_error;
    bind_text(st, 1, linking ? "linked" : "converted");
    sqlite3_bind_int64(st, 2, ticket_id);
    sqlite3_bind_int64(st, 3, id);
    sqlite3_bind_int64(st, 4, revision);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
        goto database_error;
    char audit[160];
    snprintf(audit, sizeof(audit), "Email RAW #%lld collegata al ticket #%lld; %zu allegati selezionati.",
             (long long)id, (long long)ticket_id, selected_count);
    if (!raw_audit(id, a, linking ? "linked" : "converted", audit) || !add_history(ticket_id, a, "raw_linked", audit) ||
        !exec_sql("COMMIT"))
        goto database_error;
    transaction = false;
    if (ticket)
        json_object_put(ticket);
    ticket = fetch_ticket(ticket_id);
    json_object_put(raw);
    raw = raw_fetch(id, a, true);
    if (!ticket || !raw)
        goto database_error;
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ticket", json_object_get(ticket));
    json_object_object_add(o, "raw", json_object_get(raw));
    json_object_object_add(o, "replayed", json_object_new_boolean(false));
    send_json(r, linking ? 200 : 201, o);
    goto done;
database_error:
    db_error(r);
done:
    if (transaction)
        exec_sql("ROLLBACK");
    if (fields.references)
        json_object_put(fields.references);
    if (draft)
        json_object_put(draft);
    if (ticket)
        json_object_put(ticket);
    if (raw)
        json_object_put(raw);
    json_object_put(input);
}
static bool raw_ticket_sources(Request *r, Auth *a, sqlite3_int64 id, json_object *o) {
    (void)r;
    json_object *sources = json_object_new_array(), *files = json_object_new_array();
    json_object_object_add(o, "raw_sources", sources);
    json_object_object_add(o, "attachments", files);
    sqlite3_stmt *st =
        prepare("SELECT m.id,m.subject,m.from_email,m.received_at,m.state FROM ticket_raw_links l JOIN raw_messages m "
                "ON m.id=l.raw_message_id WHERE l.ticket_id=?2 AND " RAW_VISIBLE_SQL " ORDER BY m.id");
    if (!st)
        return false;
    sqlite3_bind_int(st, 1, raw_admin(a));
    sqlite3_bind_int64(st, 2, id);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json_object *row = json_object_new_object();
        add_int(row, "id", sqlite3_column_int64(st, 0));
        add_string(row, "subject", column_text(st, 1));
        add_string(row, "from_email", column_text(st, 2));
        add_int(row, "received_at", sqlite3_column_int64(st, 3));
        add_string(row, "state", column_text(st, 4));
        json_object_array_add(sources, row);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return false;
    st = prepare("SELECT x.id,x.filename,x.content_type,x.size,x.sha256,m.id FROM ticket_raw_attachments l JOIN "
                 "raw_attachments x ON x.id=l.raw_attachment_id JOIN raw_messages m ON m.id=x.raw_message_id WHERE "
                 "l.ticket_id=?2 AND " RAW_VISIBLE_SQL " ORDER BY x.id");
    if (!st)
        return false;
    sqlite3_bind_int(st, 1, raw_admin(a));
    sqlite3_bind_int64(st, 2, id);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        json_object_array_add(files, raw_attachment_json(st, sqlite3_column_int64(st, 5), true, id));
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}
static bool raw_dispatch(Request *r, Auth *a, bool get, bool post, bool put) {
    bool raw_path = !strncmp(r->path, "/api/raw/", 9);
    sqlite3_int64 id, attachment;
    const char *suffix, *end;
    if (path_id(r->path, "/api/tickets/", &id, &suffix) && path_id(suffix, "/attachments/", &attachment, &end) &&
        !*end) {
        if (get)
            raw_download(r, a, 0, attachment, id);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!raw_path)
        return false;
    bool mailbox = !strcmp(r->path, "/api/raw/mailboxes"), sender = !strcmp(r->path, "/api/raw/senders");
    if (mailbox || sender) {
        if (!raw_admin(a)) {
            error_response(r, 403, "Sono richiesti i permessi di amministratore.");
            return true;
        }
        if (get)
            raw_config_list(r, mailbox);
        else if (post)
            raw_config_save(r, a, 0, mailbox);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if ((path_id(r->path, "/api/raw/mailboxes/", &id, &suffix) ||
         path_id(r->path, "/api/raw/senders/", &id, &suffix)) &&
        !*suffix) {
        if (!raw_admin(a))
            error_response(r, 403, "Sono richiesti i permessi di amministratore.");
        else if (put)
            raw_config_save(r, a, id, !strncmp(r->path, "/api/raw/mailboxes/", 19));
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path, "/api/raw/messages")) {
        if (get)
            raw_list(r, a);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (path_id(r->path, "/api/raw/messages/", &id, &suffix)) {
        if (!*suffix) {
            if (get)
                raw_detail(r, a, id);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (!strcmp(suffix, "/eml")) {
            if (get)
                raw_download(r, a, id, 0, 0);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (path_id(suffix, "/attachments/", &attachment, &end) && !*end) {
            if (get)
                raw_download(r, a, id, attachment, 0);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (!strcmp(suffix, "/claim") || !strcmp(suffix, "/release") || !strcmp(suffix, "/discard")) {
            if (post)
                raw_workflow(r, a, id, suffix + 1);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (!strcmp(suffix, "/convert") || !strcmp(suffix, "/link")) {
            if (post)
                raw_convert(r, a, id, !strcmp(suffix, "/link"));
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
    }
    error_response(r, 404, "Risorsa RAW non trovata.");
    return true;
}
