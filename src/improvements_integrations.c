#include "improvements_integrations.h"
#include "alerts.h"
#include "calendar_client.h"
#include "improvements.h"
#include "inventory_client.h"
#include "organization.h"
#include "storage.h"
#include <ctype.h>
#include <limits.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define II_PREFIX "/api/improvements/integrations/"
#define II_MONEY 1000000000000LL
#ifdef PUNTO_MYSQL
#define II_ID "BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY"
#define II_TEXT "LONGTEXT"
#define II_TAIL " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci"
#else
#define II_ID "INTEGER PRIMARY KEY AUTOINCREMENT"
#define II_TEXT "TEXT"
#define II_TAIL ""
#endif
static Db *mapping_database;
static bool prepare(Db *db, DbStmt **s, const char *sql) {
    return db_prepare_v2(db, sql, -1, s, NULL) == DB_OK;
}
static bool number(DbStmt *s, int i, DbInt v) { return db_bind_int64(s, i, v) == DB_OK; }
static bool text(DbStmt *s, int i, const char *v) { return db_bind_text(s, i, v, -1, DB_TRANSIENT) == DB_OK; }
static const char *column(DbStmt *s, int i) {
    const unsigned char *v = db_column_text(s, i);
    return v ? (const char *)v : "";
}
static void addn(struct json_object *o, const char *k, DbInt v) {
    json_object_object_add(o, k, json_object_new_int64(v));
}
static void adds(struct json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v ? v : ""));
}
static void addb(struct json_object *o, const char *k, bool v) {
    json_object_object_add(o, k, json_object_new_boolean(v));
}
static struct json_object *okay(void) {
    struct json_object *o = json_object_new_object();
    addb(o, "ok", true);
    return o;
}
static void failure(WorkspaceResponse *r) {
    im_error(r, 500, "Operazione non riuscita: nessuna modifica confermata.");
}
static bool stamp(char out[21]) {
    time_t n = time(NULL);
    struct tm t;
    return n != (time_t)-1 && gmtime_r(&n, &t) && strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &t) == 20;
}
static bool bounded(const char *v, size_t max, bool required) {
    size_t n = strlen(v);
    if (n > max || (required && !n))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char first = (unsigned char)v[i];
        if (first < 32 && first != '\n' && first != '\r' && first != '\t')
            return false;
        if (first < 128)
            continue;
        unsigned count;
        unsigned code;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            code = first & 31U;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            code = first & 15U;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            code = first & 7U;
        } else
            return false;
        if (i + count >= n)
            return false;
        for (unsigned j = 0; j < count; j++) {
            unsigned char c = (unsigned char)v[++i];
            if ((c & 0xc0U) != 0x80U)
                return false;
            code = (code << 6) | (c & 63U);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) || (count == 3 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
static bool fields_only(struct json_object *f, const char *allowed) {
    json_object_object_foreach(f, key, value) {
        (void)value;
        char needle[90];
        snprintf(needle, sizeof(needle), "|%s|", key);
        if (!strstr(allowed, needle))
            return false;
    }
    return true;
}
static struct json_object *parse_object(const char *v) {
    struct json_object *o = json_tokener_parse(v);
    if (!o || !json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        return NULL;
    }
    return o;
}
static struct json_object *parse_rows(const char *value) {
    struct json_tokener *parser = json_tokener_new();
    if (!parser)
        return NULL;
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT);
    size_t length = strlen(value);
    struct json_object *rows = json_tokener_parse_ex(parser, value, (int)length);
    size_t end = json_tokener_get_parse_end(parser);
    bool valid = json_tokener_get_error(parser) == json_tokener_success;
    while (end < length && strchr(" \t\r\n", value[end]))
        end++;
    json_tokener_free(parser);
    if (!valid || end != length) {
        json_object_put(rows);
        return NULL;
    }
    return rows;
}
static bool policy(Db *db, DbInt actor, AccessPolicy *p, WorkspaceResponse *r) {
    int n = access_policy_resolve(db, actor, NULL, p);
    if (n == 1)
        return true;
    im_error(r, n < 0 ? 503 : 401, n < 0 ? "Impossibile verificare gli accessi." : "Accedi per continuare.");
    return false;
}
static bool job_visible(Db *db, DbInt actor, const AccessPolicy *p, DbInt job, bool write) {
    if (write)
        return access_policy_can_manage_job(db, actor, job) == 1;
    char today[11];
    return im_today(today) && (p->admin || access_policy_can_manage_job(db, actor, job) == 1 ||
                               organization_job_access(db, job, actor, today, NULL) > 0);
}
static bool financial(const AccessPolicy *p, WorkspaceResponse *r) {
    if (p->finance)
        return true;
    im_error(r, 403, "Occorre il profilo Finance.");
    return false;
}
static bool commercial(const AccessPolicy *p, bool write, WorkspaceResponse *r) {
    if (p->sales || (!write && p->finance))
        return true;
    im_error(r, 403, "Il profilo non consente questa operazione contrattuale.");
    return false;
}

bool improvements_integrations_schema(Db *db) {
    static const char *const sql[] = {
        "CREATE TABLE IF NOT EXISTS ii_contract_revisions(id " II_ID
        ",contract_id BIGINT NOT NULL,sequence_no BIGINT NOT NULL,status VARCHAR(16) NOT NULL,revision "
        "BIGINT NOT NULL DEFAULT 1,snapshot_json " II_TEXT
        " NOT NULL,reason VARCHAR(1000) NOT NULL,created_by BIGINT NOT NULL,created_at VARCHAR(20) NOT "
        "NULL,published_at VARCHAR(20),UNIQUE(contract_id,sequence_no))" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_job_contract_revisions(job_id BIGINT PRIMARY KEY,revision_id BIGINT "
        "NOT NULL,assigned_by BIGINT NOT NULL,assigned_at VARCHAR(20) NOT NULL)" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_economic_periods(id " II_ID
        ",name VARCHAR(120) NOT NULL,starts_on VARCHAR(10) NOT NULL,ends_on VARCHAR(10) NOT "
        "NULL,snapshot_json " II_TEXT " NOT NULL,reason VARCHAR(1000) NOT NULL,approved_by BIGINT NOT "
        "NULL,approved_at VARCHAR(20) NOT NULL)" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_economic_corrections(id " II_ID
        ",period_id BIGINT NOT NULL,job_id BIGINT NOT NULL,delta_cents BIGINT NOT NULL,reason VARCHAR(1000) "
        "NOT NULL,created_by BIGINT NOT NULL,created_at VARCHAR(20) NOT NULL)" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_job_budgets(job_id BIGINT PRIMARY KEY,budget_cents BIGINT NOT "
        "NULL,remaining_cents BIGINT NOT NULL,revision BIGINT NOT NULL,reason VARCHAR(1000) NOT "
        "NULL,updated_by BIGINT NOT NULL,updated_at VARCHAR(20) NOT NULL)" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_time_drafts(id " II_ID
        ",user_id BIGINT NOT NULL,source VARCHAR(16) NOT NULL,source_ref VARCHAR(512) NOT "
        "NULL,entries_json " II_TEXT
        " NOT NULL,revision BIGINT NOT NULL DEFAULT 1,state VARCHAR(16) NOT NULL DEFAULT 'draft',request_key "
        "VARCHAR(64) NOT NULL,created_at VARCHAR(20) NOT NULL,confirmed_at "
        "VARCHAR(20),UNIQUE(request_key))" II_TAIL,
        "CREATE TABLE IF NOT EXISTS ii_inventory_person_accounts(person_id BIGINT PRIMARY KEY,user_id BIGINT "
        "NOT NULL UNIQUE,person_name VARCHAR(160) NOT NULL,revision BIGINT NOT NULL,verified_by BIGINT NOT "
        "NULL,verified_at VARCHAR(20) NOT NULL)" II_TAIL};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); i++)
        if (!im_exec(db, sql[i]))
            return false;
    return alerts_outbox_controls_schema(db);
}
static DbInt mapped_person(DbInt user) {
    DbStmt *s = NULL;
    DbInt id = 0;
    if (mapping_database &&
        prepare(mapping_database, &s,
                "SELECT m.person_id FROM ii_inventory_person_accounts m JOIN users u ON u.id=m.user_id WHERE "
                "m.user_id=? AND u.active=1") &&
        number(s, 1, user)) {
        int step = db_step(s);
        if (step == DB_ROW)
            id = db_column_int64(s, 0);
        else if (step != DB_DONE)
            id = -1;
    } else if (mapping_database)
        id = -1;
    db_finalize(s);
    return id;
}
void improvements_integrations_attach(Db *db) {
    mapping_database = db;
    inventory_client_set_person_lookup(db ? mapped_person : NULL);
}
bool improvements_integrations_time_open(Db *db, DbInt job, const char *date) {
    (void)job;
    DbStmt *s = NULL;
    bool open = false;
    if (im_date(date) &&
        prepare(db, &s, "SELECT 1 FROM ii_economic_periods WHERE starts_on<=? AND ends_on>=? LIMIT 1") &&
        text(s, 1, date) && text(s, 2, date))
        open = db_step(s) == DB_DONE;
    db_finalize(s);
    return open;
}

static struct json_object *contract_snapshot(Db *db, DbInt id) {
    DbStmt *s = NULL;
    struct json_object *o = NULL, *docs = NULL;
    if (!prepare(db, &s,
                 "SELECT "
                 "c.id,c.code,c.title,c.supplier_id,c.status,c.starts_on,c.ends_on,c.value_cents,c.currency,"
                 "c.notes,s.name FROM contracts c JOIN suppliers s ON s.id=c.supplier_id WHERE c.id=?") ||
        !number(s, 1, id) || db_step(s) != DB_ROW)
        goto done;
    o = json_object_new_object();
    static const char *keys[] = {"id",       "code",      "title",        "supplier_id",
                                 "status",   "starts_on", "ends_on",      "value_cents",
                                 "currency", "notes",     "supplier_name"};
    for (int i = 0; i < 11; i++)
        if (db_column_type(s, i) == DB_NULL)
            json_object_object_add(o, keys[i], NULL);
        else if (i == 0 || i == 3 || i == 7)
            addn(o, keys[i], db_column_int64(s, i));
        else
            adds(o, keys[i], column(s, i));
    db_finalize(s);
    s = NULL;
    docs = json_object_new_array();
    if (!prepare(
            db, &s,
            "SELECT id,file_name,byte_size,sha256 FROM contract_documents WHERE contract_id=? ORDER BY id") ||
        !number(s, 1, id))
        goto fail;
    int step, count = 0;
    while ((step = db_step(s)) == DB_ROW) {
        if (++count > 1000)
            goto fail;
        struct json_object *d = json_object_new_object();
        addn(d, "id", db_column_int64(s, 0));
        adds(d, "file_name", column(s, 1));
        addn(d, "byte_size", db_column_int64(s, 2));
        adds(d, "sha256", column(s, 3));
        json_object_array_add(docs, d);
    }
    if (step != DB_DONE)
        goto fail;
    json_object_object_add(o, "documents", docs);
    docs = NULL;
    goto done;
fail:
    json_object_put(o);
    o = NULL;
done:
    db_finalize(s);
    json_object_put(docs);
    return o;
}
static void revisions(Db *db, const char *method, struct json_object *f, DbInt actor, const AccessPolicy *p,
                      WorkspaceResponse *r, bool publish) {
    bool write = !strcmp(method, "POST");
    if (!commercial(p, write, r))
        return;
    if (!fields_only(f, "|contract_id||id||revision||reason||title||notes||value_cents|")) {
        im_error(r, 400, "Campi della revisione non validi.");
        return;
    }
    DbInt contract = 0, id = 0, version = 0;
    DbStmt *s = NULL;
    char now[21];
    struct json_object *o = NULL;
    if (!write) {
        if (!im_integer(f, "contract_id", 1, LLONG_MAX, &contract)) {
            im_error(r, 422, "Seleziona un contratto.");
            return;
        }
        o = okay();
        struct json_object *items = json_object_new_array();
        if (!prepare(db, &s,
                     "SELECT id,sequence_no,status,revision,snapshot_json,reason,created_at,published_at "
                     "FROM ii_contract_revisions WHERE contract_id=? ORDER BY sequence_no DESC LIMIT 200") ||
            !number(s, 1, contract))
            goto fail;
        int step;
        while ((step = db_step(s)) == DB_ROW) {
            struct json_object *item = json_object_new_object();
            addn(item, "id", db_column_int64(s, 0));
            addn(item, "sequence", db_column_int64(s, 1));
            adds(item, "status", column(s, 2));
            addn(item, "revision", db_column_int64(s, 3));
            json_object_object_add(item, "snapshot", parse_object(column(s, 4)));
            adds(item, "reason", column(s, 5));
            adds(item, "created_at", column(s, 6));
            adds(item, "published_at", column(s, 7));
            json_object_array_add(items, item);
        }
        if (step != DB_DONE) {
            json_object_put(items);
            goto fail;
        }
        json_object_object_add(o, "revisions", items);
        db_finalize(s);
        im_reply(r, 200, o);
        return;
    }
    if (!bounded(im_string(f, "reason"), 1000, true) || !stamp(now)) {
        im_error(r, 422, "Indica una motivazione per la revisione.");
        return;
    }
    if (!im_begin(db, r))
        return;
    AccessPolicy live;
    if (!policy(db, actor, &live, r) || !commercial(&live, true, r)) {
        im_rollback(db);
        return;
    }
    if (publish) {
        if (!im_integer(f, "id", 1, LLONG_MAX, &id) || !im_integer(f, "revision", 1, LLONG_MAX, &version)) {
            im_rollback(db);
            im_error(r, 422, "Revisione richiesta.");
            return;
        }
        if (!prepare(
                db, &s,
                "SELECT contract_id,snapshot_json,status,revision FROM ii_contract_revisions WHERE id=?") ||
            !number(s, 1, id))
            goto rollback;
        int step = db_step(s);
        if (step != DB_ROW) {
            db_finalize(s);
            im_rollback(db);
            im_error(r, 404, "Revisione non trovata.");
            return;
        }
        contract = db_column_int64(s, 0);
        if (strcmp(column(s, 2), "draft") || db_column_int64(s, 3) != version) {
            db_finalize(s);
            im_rollback(db);
            im_error(r, 409, "La revisione è già pubblicata o è cambiata. Ricarica.");
            return;
        }
        o = parse_object(column(s, 1));
        db_finalize(s);
        s = NULL;
        if (!o)
            goto rollback;
        /* Recheck document ownership immediately before publication. */
        struct json_object *docs = NULL;
        json_object_object_get_ex(o, "documents", &docs);
        for (size_t i = 0; docs && i < json_object_array_length(docs); i++) {
            struct json_object *d = json_object_array_get_idx(docs, i), *v = NULL;
            json_object_object_get_ex(d, "id", &v);
            if (!prepare(db, &s, "SELECT 1 FROM contract_documents WHERE id=? AND contract_id=?") ||
                !number(s, 1, json_object_get_int64(v)) || !number(s, 2, contract) || db_step(s) != DB_ROW)
                goto rollback;
            db_finalize(s);
            s = NULL;
        }
        if (!prepare(db, &s,
                     "UPDATE ii_contract_revisions SET "
                     "status='published',published_at=?,reason=?,revision=revision+1 WHERE id=? AND "
                     "status='draft' AND revision=?") ||
            !text(s, 1, now) || !text(s, 2, im_string(f, "reason")) || !number(s, 3, id) ||
            !number(s, 4, version) || db_step(s) != DB_DONE || db_changes(db) != 1)
            goto rollback;
    } else {
        if (!im_integer(f, "contract_id", 1, LLONG_MAX, &contract)) {
            im_rollback(db);
            im_error(r, 422, "Seleziona un contratto.");
            return;
        }
        o = contract_snapshot(db, contract);
        if (!o) {
            im_rollback(db);
            im_error(r, 404, "Contratto non trovato.");
            return;
        }
        struct json_object *present = NULL;
        if (json_object_object_get_ex(f, "title", &present)) {
            if (!bounded(im_string(f, "title"), 160, true))
                goto invalid;
            adds(o, "title", im_string(f, "title"));
        }
        if (json_object_object_get_ex(f, "notes", &present)) {
            if (!bounded(im_string(f, "notes"), 4000, false))
                goto invalid;
            adds(o, "notes", im_string(f, "notes"));
        }
        DbInt cents;
        if (json_object_object_get_ex(f, "value_cents", &present)) {
            if (!im_integer(f, "value_cents", 0, II_MONEY, &cents))
                goto invalid;
            addn(o, "value_cents", cents);
        }
        if (!prepare(
                db, &s,
                "SELECT COALESCE(MAX(sequence_no),0)+1 FROM ii_contract_revisions WHERE contract_id=?") ||
            !number(s, 1, contract) || db_step(s) != DB_ROW)
            goto rollback;
        DbInt seq = db_column_int64(s, 0);
        db_finalize(s);
        s = NULL;
        if (!prepare(db, &s,
                     "INSERT INTO "
                     "ii_contract_revisions(contract_id,sequence_no,status,revision,snapshot_json,reason,"
                     "created_by,created_at) VALUES(?,?,'draft',1,?,?,?,?)") ||
            !number(s, 1, contract) || !number(s, 2, seq) ||
            !text(s, 3, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN)) ||
            !text(s, 4, im_string(f, "reason")) || !number(s, 5, actor) || !text(s, 6, now) ||
            db_step(s) != DB_DONE)
            goto rollback;
        id = db_last_insert_rowid(db);
    }
    db_finalize(s);
    s = NULL;
    addn(o, "revision_id", id);
    if (!im_audit_commit(db, actor, publish ? "contract_revisions.publish" : "contract_revisions.draft",
                         "contract", contract, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    json_object_put(o);
    o = okay();
    addn(o, "id", id);
    im_reply(r, publish ? 200 : 201, o);
    return;
invalid:
    db_finalize(s);
    im_rollback(db);
    json_object_put(o);
    im_error(r, 422, "Dati contrattuali non validi.");
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    json_object_put(o);
    failure(r);
    return;
fail:
    db_finalize(s);
    json_object_put(o);
    failure(r);
}

static void job_revision(Db *db, const char *method, struct json_object *f, DbInt actor,
                         const AccessPolicy *p, WorkspaceResponse *r) {
    bool write = !strcmp(method, "POST");
    if (!commercial(p, write, r))
        return;
    DbInt job = 0, id = 0;
    DbStmt *s = NULL;
    char now[21];
    if (!fields_only(f, "|job_id||revision_id|") || !im_integer(f, "job_id", 1, LLONG_MAX, &job)) {
        im_error(r, 422, "Seleziona una commessa.");
        return;
    }
    if (!job_visible(db, actor, p, job, write)) {
        im_error(r, 403, "Non puoi gestire questa commessa.");
        return;
    }
    if (!write) {
        if (!prepare(db, &s,
                     "SELECT r.id,r.contract_id,r.sequence_no,r.snapshot_json,j.contract_id FROM "
                     "ii_job_contract_revisions a JOIN ii_contract_revisions r ON r.id=a.revision_id JOIN "
                     "jobs j ON j.id=a.job_id WHERE a.job_id=?") ||
            !number(s, 1, job))
            goto fail;
        int step = db_step(s);
        struct json_object *o = okay();
        json_object_object_add(o, "contract_revision", NULL);
        if (step == DB_ROW) {
            struct json_object *v = parse_object(column(s, 3));
            if (!v) {
                json_object_put(o);
                goto fail;
            }
            addn(v, "revision_id", db_column_int64(s, 0));
            addn(v, "sequence", db_column_int64(s, 2));
            addb(v, "current_contract_matches", db_column_int64(s, 1) == db_column_int64(s, 4));
            json_object_object_add(o, "contract_revision", v);
        } else if (step != DB_DONE) {
            json_object_put(o);
            goto fail;
        }
        db_finalize(s);
        im_reply(r, 200, o);
        return;
    }
    if (!im_integer(f, "revision_id", 1, LLONG_MAX, &id) || !stamp(now)) {
        im_error(r, 422, "Seleziona una revisione pubblicata.");
        return;
    }
    if (!im_begin(db, r))
        return;
    AccessPolicy live;
    if (!policy(db, actor, &live, r) || !commercial(&live, true, r)) {
        im_rollback(db);
        return;
    }
    if (access_policy_can_manage_job(db, actor, job) != 1) {
        im_rollback(db);
        im_error(r, 403, "Non puoi gestire questa commessa.");
        return;
    }
    if (!prepare(db, &s,
                 "SELECT 1 FROM jobs j JOIN ii_contract_revisions r ON r.contract_id=j.contract_id WHERE "
                 "j.id=? AND r.id=? AND r.status='published'") ||
        !number(s, 1, job) || !number(s, 2, id))
        goto rollback;
    if (db_step(s) != DB_ROW) {
        db_finalize(s);
        im_rollback(db);
        im_error(r, 422, "La revisione deve essere pubblicata e appartenere al contratto della commessa.");
        return;
    }
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s, "DELETE FROM ii_job_contract_revisions WHERE job_id=?") || !number(s, 1, job) ||
        db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s,
                 "INSERT INTO ii_job_contract_revisions(job_id,revision_id,assigned_by,assigned_at) "
                 "VALUES(?,?,?,?)") ||
        !number(s, 1, job) || !number(s, 2, id) || !number(s, 3, actor) || !text(s, 4, now) ||
        db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    struct json_object *o = okay();
    addn(o, "job_id", job);
    addn(o, "revision_id", id);
    if (!im_audit_commit(db, actor, "job_contract_revision.assign", "job", job, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 200, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    failure(r);
    return;
fail:
    db_finalize(s);
    failure(r);
}

static struct json_object *costs(Db *db, const char *from, const char *to, DbInt job, bool open_only) {
    DbStmt *s = NULL;
    struct json_object *o = okay(), *rows = json_object_new_array();
    DbInt total = 0, minutes = 0, unpriced = 0;
    const char *sql =
        "SELECT j.id,j.code,j.name,COALESCE(SUM(t.minutes),0),COALESCE(SUM(CASE WHEN r.id IS NULL THEN "
        "t.minutes ELSE 0 END),0),COALESCE(SUM(CASE WHEN r.id IS NULL THEN 0 ELSE "
        "((t.minutes*r.hourly_cents+30)-((t.minutes*r.hourly_cents+30)%60))/60 END),0),COUNT(t.id) FROM jobs "
        "j LEFT JOIN time_entries t ON t.job_id=j.id AND t.date>=? AND t.date<=? AND (?=0 OR NOT "
        "EXISTS(SELECT 1 FROM ii_economic_periods p WHERE t.date>=p.starts_on AND t.date<=p.ends_on)) LEFT "
        "JOIN labor_rates r ON r.user_id=t.user_id AND r.starts_on<=t.date AND (r.ends_on IS NULL OR "
        "r.ends_on>=t.date) WHERE (?=0 OR j.id=?) GROUP BY j.id,j.code,j.name ORDER BY j.code,j.id";
    if (!prepare(db, &s, sql) || !text(s, 1, from) || !text(s, 2, to) || !number(s, 3, open_only) ||
        !number(s, 4, job) || !number(s, 5, job))
        goto fail;
    int step, count = 0;
    while ((step = db_step(s)) == DB_ROW) {
        if (++count > 20000)
            goto fail;
        struct json_object *row = json_object_new_object();
        addn(row, "job_id", db_column_int64(s, 0));
        adds(row, "code", column(s, 1));
        adds(row, "job_name", column(s, 2));
        addn(row, "minutes", db_column_int64(s, 3));
        addn(row, "unpriced_minutes", db_column_int64(s, 4));
        addn(row, "cost_cents", db_column_int64(s, 5));
        addn(row, "entry_count", db_column_int64(s, 6));
        json_object_array_add(rows, row);
        minutes += db_column_int64(s, 3);
        unpriced += db_column_int64(s, 4);
        total += db_column_int64(s, 5);
    }
    if (step != DB_DONE)
        goto fail;
    db_finalize(s);
    adds(o, "currency", "EUR");
    adds(o, "from", from);
    adds(o, "to", to);
    addn(o, "minutes", minutes);
    addn(o, "unpriced_minutes", unpriced);
    addn(o, "cost_cents", total);
    json_object_object_add(o, "rows", rows);
    return o;
fail:
    db_finalize(s);
    json_object_put(o);
    json_object_put(rows);
    return NULL;
}
static void periods(Db *db, const char *method, struct json_object *f, DbInt actor, const AccessPolicy *p,
                    WorkspaceResponse *r, bool correction) {
    if (!financial(p, r))
        return;
    DbStmt *s = NULL;
    struct json_object *o = NULL;
    char now[21];
    DbInt id = 0, job = 0, delta = 0;
    if (!fields_only(f, "|from||to||name||reason||action||preview_hash||period_id||job_id||delta_cents|")) {
        im_error(r, 400, "Campi economici non validi.");
        return;
    }
    if (!strcmp(method, "GET")) {
        o = okay();
        struct json_object *items = json_object_new_array(), *adjustments = json_object_new_array();
        if (!prepare(db, &s,
                     "SELECT id,name,starts_on,ends_on,snapshot_json,reason,approved_at FROM "
                     "ii_economic_periods ORDER BY starts_on DESC,id DESC LIMIT 200"))
            goto fail;
        int step;
        while ((step = db_step(s)) == DB_ROW) {
            struct json_object *v = parse_object(column(s, 4));
            if (!v) {
                json_object_put(items);
                json_object_put(adjustments);
                goto fail;
            }
            addn(v, "id", db_column_int64(s, 0));
            adds(v, "name", column(s, 1));
            adds(v, "reason", column(s, 5));
            adds(v, "approved_at", column(s, 6));
            json_object_array_add(items, v);
        }
        if (step != DB_DONE) {
            json_object_put(items);
            json_object_put(adjustments);
            goto fail;
        }
        db_finalize(s);
        s = NULL;
        if (!prepare(
                db, &s,
                "SELECT a.id,a.period_id,a.job_id,j.code,a.delta_cents,a.reason,a.created_at FROM "
                "ii_economic_corrections a JOIN jobs j ON j.id=a.job_id ORDER BY a.id DESC LIMIT 1000")) {
            json_object_put(items);
            json_object_put(adjustments);
            goto fail;
        }
        while ((step = db_step(s)) == DB_ROW) {
            struct json_object *v = json_object_new_object();
            addn(v, "id", db_column_int64(s, 0));
            addn(v, "period_id", db_column_int64(s, 1));
            addn(v, "job_id", db_column_int64(s, 2));
            adds(v, "code", column(s, 3));
            addn(v, "delta_cents", db_column_int64(s, 4));
            adds(v, "reason", column(s, 5));
            adds(v, "created_at", column(s, 6));
            json_object_array_add(adjustments, v);
        }
        if (step != DB_DONE) {
            json_object_put(items);
            json_object_put(adjustments);
            goto fail;
        }
        db_finalize(s);
        json_object_object_add(o, "periods", items);
        json_object_object_add(o, "corrections", adjustments);
        im_reply(r, 200, o);
        return;
    }
    if (correction) {
        if (!im_integer(f, "period_id", 1, LLONG_MAX, &id) || !im_integer(f, "job_id", 1, LLONG_MAX, &job) ||
            !im_integer(f, "delta_cents", -II_MONEY, II_MONEY, &delta) || !delta ||
            !bounded(im_string(f, "reason"), 1000, true) || !stamp(now)) {
            im_error(r, 422, "Indica periodo, commessa, rettifica non nulla e motivazione.");
            return;
        }
        if (!im_begin(db, r))
            return;
        AccessPolicy live;
        if (!policy(db, actor, &live, r) || !financial(&live, r)) {
            im_rollback(db);
            return;
        }
        if (!prepare(db, &s, "SELECT 1 FROM ii_economic_periods p JOIN jobs j ON j.id=? WHERE p.id=?") ||
            !number(s, 1, job) || !number(s, 2, id) || db_step(s) != DB_ROW)
            goto rollback;
        db_finalize(s);
        s = NULL;
        if (!prepare(db, &s,
                     "INSERT INTO "
                     "ii_economic_corrections(period_id,job_id,delta_cents,reason,created_by,created_at) "
                     "VALUES(?,?,?,?,?,?)") ||
            !number(s, 1, id) || !number(s, 2, job) || !number(s, 3, delta) ||
            !text(s, 4, im_string(f, "reason")) || !number(s, 5, actor) || !text(s, 6, now) ||
            db_step(s) != DB_DONE)
            goto rollback;
        id = db_last_insert_rowid(db);
        o = okay();
        addn(o, "id", id);
        addn(o, "delta_cents", delta);
        adds(o, "reason", im_string(f, "reason"));
    } else {
        const char *from = im_string(f, "from"), *to = im_string(f, "to"), *action = im_string(f, "action");
        if (!im_date(from) || !im_date(to) || strcmp(from, to) > 0 ||
            (strcmp(action, "preview") && strcmp(action, "approve"))) {
            im_error(r, 422, "Indica un periodo valido e anteprima o approvazione.");
            return;
        }
        if (!strcmp(action, "preview")) {
            o = costs(db, from, to, 0, false);
            if (!o)
                goto fail;
            unsigned char digest[SHA256_DIGEST_LENGTH];
            const char *serialized = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
            SHA256((const unsigned char *)serialized, strlen(serialized), digest);
            char hash[65];
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
                snprintf(hash + i * 2, 3, "%02x", digest[i]);
            adds(o, "preview_hash", hash);
            im_reply(r, 200, o);
            return;
        }
        if (!bounded(im_string(f, "name"), 120, true) || !bounded(im_string(f, "reason"), 1000, true) ||
            !stamp(now)) {
            im_error(r, 422, "Indica nome e motivazione del periodo.");
            return;
        }
        if (!im_begin(db, r))
            return;
        AccessPolicy live;
        if (!policy(db, actor, &live, r) || !financial(&live, r)) {
            im_rollback(db);
            return;
        }
        if (!prepare(db, &s, "SELECT 1 FROM ii_economic_periods WHERE starts_on<=? AND ends_on>=? LIMIT 1") ||
            !text(s, 1, to) || !text(s, 2, from))
            goto rollback;
        if (db_step(s) != DB_DONE) {
            db_finalize(s);
            im_rollback(db);
            im_error(r, 409, "Il periodo si sovrappone a una chiusura già approvata.");
            return;
        }
        db_finalize(s);
        s = NULL;
        o = costs(db, from, to, 0, false);
        if (!o)
            goto rollback;
        struct json_object *u = NULL;
        json_object_object_get_ex(o, "unpriced_minutes", &u);
        unsigned char digest[SHA256_DIGEST_LENGTH];
        const char *serialized = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
        SHA256((const unsigned char *)serialized, strlen(serialized), digest);
        char hash[65];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            snprintf(hash + i * 2, 3, "%02x", digest[i]);
        if (strcmp(hash, im_string(f, "preview_hash"))) {
            json_object_put(o);
            im_rollback(db);
            im_error(r, 409, "I costi sono cambiati dopo l’anteprima. Ricalcola prima di approvare.");
            return;
        }
        if (json_object_get_int64(u) > 0) {
            json_object_put(o);
            im_rollback(db);
            im_error(r, 409, "Completa le tariffe mancanti prima di approvare il periodo.");
            return;
        }
        if (!prepare(db, &s,
                     "INSERT INTO "
                     "ii_economic_periods(name,starts_on,ends_on,snapshot_json,reason,approved_by,approved_"
                     "at) VALUES(?,?,?,?,?,?,?)") ||
            !text(s, 1, im_string(f, "name")) || !text(s, 2, from) || !text(s, 3, to) ||
            !text(s, 4, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN)) ||
            !text(s, 5, im_string(f, "reason")) || !number(s, 6, actor) || !text(s, 7, now) ||
            db_step(s) != DB_DONE)
            goto rollback;
        id = db_last_insert_rowid(db);
        addn(o, "id", id);
    }
    db_finalize(s);
    s = NULL;
    if (!im_audit_commit(db, actor, correction ? "economic_periods.correction" : "economic_periods.approve",
                         "economic_period", id, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 201, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    json_object_put(o);
    failure(r);
    return;
fail:
    db_finalize(s);
    json_object_put(o);
    failure(r);
}
static void budget(Db *db, const char *method, struct json_object *f, DbInt actor, const AccessPolicy *p,
                   WorkspaceResponse *r) {
    if (!financial(p, r))
        return;
    DbInt job = 0, total = 0, remaining = 0, revision = 0;
    DbStmt *s = NULL;
    char now[21];
    if (!strcmp(method, "GET") && !*im_string(f, "job_id") && fields_only(f, "")) {
        struct json_object *o = okay(), *jobs = json_object_new_array();
        if (!prepare(db, &s, "SELECT id,code,name FROM jobs ORDER BY code,id LIMIT 20001")) {
            json_object_put(o);
            json_object_put(jobs);
            goto fail;
        }
        int step;
        size_t count = 0;
        while ((step = db_step(s)) == DB_ROW && count++ < 20000) {
            struct json_object *job = json_object_new_object();
            addn(job, "id", db_column_int64(s, 0));
            adds(job, "code", column(s, 1));
            adds(job, "name", column(s, 2));
            json_object_array_add(jobs, job);
        }
        if (step != DB_DONE) {
            json_object_put(o);
            json_object_put(jobs);
            goto fail;
        }
        db_finalize(s);
        json_object_object_add(o, "jobs", jobs);
        im_reply(r, 200, o);
        return;
    }
    if (!fields_only(f, "|job_id||budget_cents||remaining_cents||revision||reason|") ||
        !im_integer(f, "job_id", 1, LLONG_MAX, &job)) {
        im_error(r, 422, "Seleziona una commessa.");
        return;
    }
    if (!strcmp(method, "GET")) {
        struct json_object *o = costs(db, "1900-01-01", "9999-12-31", job, true);
        if (!o)
            goto fail;
        struct json_object *rows = NULL;
        json_object_object_get_ex(o, "rows", &rows);
        if (!json_object_array_length(rows)) {
            json_object_put(o);
            im_error(r, 404, "Commessa non trovata.");
            return;
        }
        if (!prepare(db, &s,
                     "SELECT budget_cents,remaining_cents,revision,reason,updated_at FROM ii_job_budgets "
                     "WHERE job_id=?") ||
            !number(s, 1, job)) {
            json_object_put(o);
            goto fail;
        }
        int step = db_step(s);
        if (step == DB_ROW) {
            total = db_column_int64(s, 0);
            remaining = db_column_int64(s, 1);
            revision = db_column_int64(s, 2);
            adds(o, "reason", column(s, 3));
            adds(o, "updated_at", column(s, 4));
        } else if (step != DB_DONE) {
            json_object_put(o);
            goto fail;
        }
        db_finalize(s);
        s = NULL;
        DbInt adjustments = 0;
        if (!prepare(db, &s,
                     "SELECT COALESCE(SUM(delta_cents),0) FROM ii_economic_corrections WHERE job_id=?") ||
            !number(s, 1, job) || db_step(s) != DB_ROW) {
            json_object_put(o);
            goto fail;
        }
        adjustments = db_column_int64(s, 0);
        db_finalize(s);
        s = NULL;
        DbInt approved = 0, approved_minutes = 0;
        if (!prepare(db, &s, "SELECT snapshot_json FROM ii_economic_periods")) {
            json_object_put(o);
            goto fail;
        }
        while ((step = db_step(s)) == DB_ROW) {
            struct json_object *snapshot = parse_object(column(s, 0)), *closed_rows = NULL;
            if (!snapshot || !json_object_object_get_ex(snapshot, "rows", &closed_rows)) {
                json_object_put(snapshot);
                json_object_put(o);
                goto fail;
            }
            for (size_t i = 0; i < json_object_array_length(closed_rows); i++) {
                struct json_object *row = json_object_array_get_idx(closed_rows, i), *v = NULL;
                json_object_object_get_ex(row, "job_id", &v);
                if (json_object_get_int64(v) == job) {
                    json_object_object_get_ex(row, "cost_cents", &v);
                    approved += json_object_get_int64(v);
                    json_object_object_get_ex(row, "minutes", &v);
                    approved_minutes += json_object_get_int64(v);
                }
            }
            json_object_put(snapshot);
        }
        if (step != DB_DONE) {
            json_object_put(o);
            goto fail;
        }
        db_finalize(s);
        s = NULL;
        struct json_object *v = NULL;
        json_object_object_get_ex(o, "cost_cents", &v);
        DbInt actual = json_object_get_int64(v) + approved;
        addn(o, "approved_cents", approved);
        addn(o, "approved_minutes", approved_minutes);
        addn(o, "job_id", job);
        addn(o, "budget_cents", total);
        addn(o, "remaining_cents", remaining);
        addn(o, "revision", revision);
        addn(o, "corrections_cents", adjustments);
        addn(o, "actual_cents", actual + adjustments);
        addn(o, "forecast_cents", actual + adjustments + remaining);
        addn(o, "variance_cents", total - actual - adjustments - remaining);
        im_reply(r, 200, o);
        return;
    }
    if (!im_integer(f, "budget_cents", 0, II_MONEY, &total) ||
        !im_integer(f, "remaining_cents", 0, II_MONEY, &remaining) ||
        !im_integer(f, "revision", 0, LLONG_MAX, &revision) || !bounded(im_string(f, "reason"), 1000, true) ||
        !stamp(now)) {
        im_error(r, 422, "Indica budget, costo residuo previsto, revisione e motivo.");
        return;
    }
    if (!im_begin(db, r))
        return;
    AccessPolicy live;
    if (!policy(db, actor, &live, r) || !financial(&live, r)) {
        im_rollback(db);
        return;
    }
    if (!prepare(db, &s, "SELECT 1 FROM jobs WHERE id=?") || !number(s, 1, job) || db_step(s) != DB_ROW)
        goto rollback;
    db_finalize(s);
    s = NULL;
    if (revision) {
        if (!prepare(db, &s,
                     "UPDATE ii_job_budgets SET "
                     "budget_cents=?,remaining_cents=?,revision=revision+1,reason=?,updated_by=?,updated_at=?"
                     " WHERE job_id=? AND revision=?") ||
            !number(s, 1, total) || !number(s, 2, remaining) || !text(s, 3, im_string(f, "reason")) ||
            !number(s, 4, actor) || !text(s, 5, now) || !number(s, 6, job) || !number(s, 7, revision) ||
            db_step(s) != DB_DONE)
            goto rollback;
        if (db_changes(db) != 1) {
            db_finalize(s);
            im_rollback(db);
            im_error(r, 409, "Il budget è cambiato. Ricarica prima di salvare.");
            return;
        }
    } else if (!prepare(db, &s,
                        "INSERT INTO "
                        "ii_job_budgets(job_id,budget_cents,remaining_cents,revision,reason,updated_by,"
                        "updated_at) VALUES(?,?,?,1,?,?,?)") ||
               !number(s, 1, job) || !number(s, 2, total) || !number(s, 3, remaining) ||
               !text(s, 4, im_string(f, "reason")) || !number(s, 5, actor) || !text(s, 6, now) ||
               db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    struct json_object *o = okay();
    addn(o, "job_id", job);
    addn(o, "budget_cents", total);
    addn(o, "remaining_cents", remaining);
    adds(o, "reason", im_string(f, "reason"));
    if (!im_audit_commit(db, actor, "budgets.update", "job", job, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 200, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    failure(r);
    return;
fail:
    db_finalize(s);
    failure(r);
}

static char *encode(const char *value) {
    size_t n = strlen(value);
    char *out = malloc(n * 3 + 1), *at = out;
    static const char h[] = "0123456789ABCDEF";
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            *at++ = (char)c;
        else {
            *at++ = '%';
            *at++ = h[c >> 4];
            *at++ = h[c & 15];
        }
    }
    *at = 0;
    return out;
}
static const char *jtext(struct json_object *o, const char *key) {
    struct json_object *v = NULL;
    return json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string)
               ? json_object_get_string(v)
               : "";
}
static DbInt jint(struct json_object *o, const char *key) {
    struct json_object *v = NULL;
    return json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_int)
               ? json_object_get_int64(v)
               : -1;
}
/* Civil-day arithmetic avoids process timezone changes while parsing offsets. */
static long long civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}
static bool instant(const char *v, time_t *out) {
    int y, m, d, h, mi, se, n = 0, oh = 0, om = 0;
    char date[11];
    if (strlen(v) < 20)
        return false;
    memcpy(date, v, 10);
    date[10] = 0;
    if (!im_date(date) || v[10] != 'T' || v[13] != ':' || v[16] != ':')
        return false;
    if (sscanf(v, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &m, &d, &h, &mi, &se, &n) != 6 || n != 19 || h > 23 ||
        mi > 59 || se > 59 || h < 0 || mi < 0 || se < 0)
        return false;
    const char *at = v + n;
    if (*at == '.') {
        at++;
        if (!isdigit((unsigned char)*at))
            return false;
        while (isdigit((unsigned char)*at))
            at++;
    }
    int offset = 0;
    if (!strcmp(at, "Z"))
        offset = 0;
    else if ((*at == '+' || *at == '-') && strlen(at) == 6 && at[3] == ':' && isdigit((unsigned char)at[1]) &&
             isdigit((unsigned char)at[2]) && isdigit((unsigned char)at[4]) &&
             isdigit((unsigned char)at[5]) && sscanf(at + 1, "%2d:%2d", &oh, &om) == 2 && oh <= 23 &&
             om <= 59)
        offset = (oh * 60 + om) * 60 * (*at == '-' ? -1 : 1);
    else
        return false;
    long long seconds = civil_days(y, (unsigned)m, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se - offset;
    *out = (time_t)seconds;
    return (long long)*out == seconds;
}
static struct json_object *calendar_rows(DbInt actor, DbInt job, const char *ref, WorkspaceResponse *r) {
    if (!bounded(ref, 512, true)) {
        im_error(r, 422, "Seleziona un evento personale.");
        return NULL;
    }
    char *encoded = encode(ref);
    if (!encoded) {
        failure(r);
        return NULL;
    }
    size_t capacity = strlen(encoded) + 20;
    char *query = malloc(capacity);
    if (!query) {
        free(encoded);
        failure(r);
        return NULL;
    }
    snprintf(query, capacity, "event_id=%s", encoded);
    free(encoded);
    CalendarResponse response = {0};
    calendar_client_call(CALENDAR_EVENT, actor, query, NULL, &response);
    free(query);
    if (response.status != 200) {
        im_error(r, response.status >= 500 ? 503 : 422,
                 "Impossibile verificare l’evento nel calendario personale.");
        calendar_response_free(&response);
        return NULL;
    }
    struct json_object *root = parse_object(response.body), *event = NULL, *start = NULL, *end = NULL;
    calendar_response_free(&response);
    if (root)
        json_object_object_get_ex(root, "event", &event);
    if (!event || !strcmp(jtext(event, "status"), "cancelled") ||
        !json_object_object_get_ex(event, "start", &start) ||
        !json_object_object_get_ex(event, "end", &end)) {
        json_object_put(root);
        im_error(r, 422, "L’evento non contiene un intervallo utilizzabile.");
        return NULL;
    }
    time_t from, to;
    if (!instant(jtext(start, "dateTime"), &from) || !instant(jtext(end, "dateTime"), &to) || to <= from ||
        to - from > 31 * 86400) {
        json_object_put(root);
        im_error(r, 422, "Scegli un evento con orario e durata non superiore a 31 giorni.");
        return NULL;
    }
    from -= from % 60;
    to -= to % 60;
    if (to <= from) {
        json_object_put(root);
        im_error(r, 422, "L’evento dura meno di un minuto.");
        return NULL;
    }
    struct json_object *rows = json_object_new_array();
    while (from < to) {
        struct tm local;
        if (!localtime_r(&from, &local)) {
            json_object_put(root);
            json_object_put(rows);
            failure(r);
            return NULL;
        }
        char date[11];
        if (strftime(date, sizeof(date), "%Y-%m-%d", &local) != 10) {
            json_object_put(root);
            json_object_put(rows);
            failure(r);
            return NULL;
        }
        struct tm next = local;
        next.tm_hour = next.tm_min = next.tm_sec = 0;
        next.tm_mday++;
        next.tm_isdst = -1;
        time_t boundary = mktime(&next);
        time_t until = boundary < to ? boundary : to;
        DbInt minutes = (DbInt)(until - from) / 60;
        if (boundary <= from || minutes < 1 || minutes > 1440) {
            json_object_put(root);
            json_object_put(rows);
            im_error(r, 422, "L’intervallo giornaliero supera i limiti delle ore. Modifica la proposta.");
            return NULL;
        }
        struct json_object *row = json_object_new_object();
        addn(row, "job_id", job);
        adds(row, "date", date);
        addn(row, "minutes", minutes);
        const char *note = jtext(event, "summary");
        char shortened[1001];
        snprintf(shortened, sizeof(shortened), "%s", note);
        adds(row, "note", shortened);
        json_object_array_add(rows, row);
        from = until;
    }
    json_object_put(root);
    return rows;
}
static bool validate_rows(Db *db, struct json_object *rows, DbInt actor, const AccessPolicy *p,
                          WorkspaceResponse *r) {
    if (!rows || !json_object_is_type(rows, json_type_array) || !json_object_array_length(rows) ||
        json_object_array_length(rows) > 100) {
        im_error(r, 422, "La bozza richiede da 1 a 100 righe.");
        return false;
    }
    char today[11];
    if (!im_today(today)) {
        failure(r);
        return false;
    }
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        struct json_object *row = json_object_array_get_idx(rows, i);
        DbInt job = jint(row, "job_id"), minutes = jint(row, "minutes");
        const char *date = jtext(row, "date");
        struct json_object *note = NULL;
        bool valid_note = json_object_object_get_ex(row, "note", &note) &&
                          json_object_is_type(note, json_type_string) &&
                          (size_t)json_object_get_string_len(note) == strlen(json_object_get_string(note));
        if (!json_object_is_type(row, json_type_object) || !valid_note ||
            !fields_only(row, "|job_id||date||minutes||note|") || job < 1 || minutes < 1 || minutes > 1440 ||
            !im_date(date) || !bounded(jtext(row, "note"), 1000, false)) {
            im_error(r, 422, "Controlla commessa, data, minuti e nota di ogni riga.");
            return false;
        }
        int permission = organization_job_access(db, job, actor, today, date);
        if (!p->admin && access_policy_can_manage_job(db, actor, job) != 1 && permission != 3) {
            im_error(r, 403, "Non puoi registrare ore su una delle commesse della bozza.");
            return false;
        }
        DbStmt *s = NULL;
        bool open = prepare(db, &s, "SELECT 1 FROM jobs WHERE id=? AND time_closed=0 AND archived=0") &&
                    number(s, 1, job) && db_step(s) == DB_ROW;
        db_finalize(s);
        if (!open || !improvements_integrations_time_open(db, job, date)) {
            im_error(r, 409, "Commessa o periodo chiuso: correggi la bozza prima di confermare.");
            return false;
        }
    }
    return true;
}
static void drafts(Db *db, const char *method, struct json_object *f, DbInt actor, const AccessPolicy *p,
                   WorkspaceResponse *r, bool confirm, bool candidates) {
    DbStmt *s = NULL;
    struct json_object *o = NULL, *rows = NULL;
    DbInt id = 0, revision = 0, job = 0;
    char now[21];
    if (!fields_only(f, "|id||revision||source||source_ref||job_id||entries||from||to|")) {
        im_error(r, 400, "Campi della bozza non validi.");
        return;
    }
    if (candidates) {
        const char *from = im_string(f, "from"), *to = im_string(f, "to");
        if (!im_date(from) || !im_date(to) || strcmp(from, to) > 0) {
            im_error(r, 422, "Seleziona un intervallo valido.");
            return;
        }
        char input[80];
        snprintf(input, sizeof(input), "from=%s&to=%s&limit=100", from, to);
        CalendarResponse response = {0};
        calendar_client_call(CALENDAR_EVENTS, actor, input, NULL, &response);
        r->status = response.status;
        r->body = response.body;
        return;
    }
    if (!strcmp(method, "GET")) {
        o = okay();
        rows = json_object_new_array();
        if (!prepare(db, &s,
                     "SELECT id,source,source_ref,entries_json,revision,state,created_at FROM ii_time_drafts "
                     "WHERE user_id=? ORDER BY id DESC LIMIT 200") ||
            !number(s, 1, actor))
            goto fail;
        int step;
        while ((step = db_step(s)) == DB_ROW) {
            struct json_object *v = json_object_new_object();
            addn(v, "id", db_column_int64(s, 0));
            adds(v, "source", column(s, 1));
            adds(v, "source_ref", column(s, 2));
            json_object_object_add(v, "entries", json_tokener_parse(column(s, 3)));
            addn(v, "revision", db_column_int64(s, 4));
            adds(v, "state", column(s, 5));
            adds(v, "created_at", column(s, 6));
            json_object_array_add(rows, v);
        }
        if (step != DB_DONE)
            goto fail;
        db_finalize(s);
        json_object_object_add(o, "drafts", rows);
        im_reply(r, 200, o);
        return;
    }
    if (confirm) {
        if (!im_integer(f, "id", 1, LLONG_MAX, &id) || !im_integer(f, "revision", 1, LLONG_MAX, &revision)) {
            im_error(r, 422, "Seleziona una bozza e la revisione.");
            return;
        }
        if (!prepare(db, &s,
                     "SELECT entries_json,request_key,revision,state FROM ii_time_drafts WHERE id=? AND "
                     "user_id=?") ||
            !number(s, 1, id) || !number(s, 2, actor))
            goto fail;
        if (db_step(s) != DB_ROW) {
            db_finalize(s);
            im_error(r, 404, "Bozza non trovata.");
            return;
        }
        if (db_column_int64(s, 2) != revision) {
            db_finalize(s);
            im_error(r, 409, "La bozza è cambiata. Ricarica.");
            return;
        }
        if (!strcmp(column(s, 3), "confirmed")) {
            db_finalize(s);
            im_reply(r, 200, okay());
            return;
        }
        rows = json_tokener_parse(column(s, 0));
        char *key = strdup(column(s, 1));
        db_finalize(s);
        s = NULL;
        if (!key)
            goto fail;
        if (!prepare(db, &s, "SELECT 1 FROM time_entry_batches WHERE user_id=? AND request_key=?") ||
            !number(s, 1, actor) || !text(s, 2, key)) {
            free(key);
            goto fail;
        }
        int previous_batch = db_step(s);
        db_finalize(s);
        s = NULL;
        if (previous_batch != DB_ROW && previous_batch != DB_DONE) {
            free(key);
            goto fail;
        }
        if (previous_batch == DB_DONE && !validate_rows(db, rows, actor, p, r)) {
            free(key);
            json_object_put(rows);
            return;
        }
        if (!prepare(db, &s,
                     "UPDATE ii_time_drafts SET state='confirming' WHERE id=? AND user_id=? AND revision=? "
                     "AND state IN ('draft','confirming')") ||
            !number(s, 1, id) || !number(s, 2, actor) || !number(s, 3, revision) || db_step(s) != DB_DONE) {
            free(key);
            goto fail;
        }
        db_finalize(s);
        s = NULL;
        /* A parallel edit may have changed the revision before the claim. A
           replay of the same confirming revision is safe; another revision is not. */
        if (!prepare(db, &s,
                     "SELECT 1 FROM ii_time_drafts WHERE id=? AND user_id=? AND revision=? AND "
                     "state='confirming'") ||
            !number(s, 1, id) || !number(s, 2, actor) || !number(s, 3, revision)) {
            free(key);
            goto fail;
        }
        int claimed = db_step(s);
        db_finalize(s);
        s = NULL;
        if (claimed != DB_ROW) {
            free(key);
            json_object_put(rows);
            im_error(r, 409, "La bozza è cambiata. Ricarica.");
            return;
        }
        if (!prepare(db, &s, "SELECT email,role FROM users WHERE id=? AND active=1") ||
            !number(s, 1, actor) || db_step(s) != DB_ROW) {
            free(key);
            goto fail;
        }
        char *owner = strdup(column(s, 0)), *role = strdup(column(s, 1));
        db_finalize(s);
        s = NULL;
        char *payload = encode(json_object_to_json_string_ext(rows, JSON_C_TO_STRING_PLAIN));
        json_object_put(rows);
        rows = NULL;
        size_t cap = payload ? strlen(payload) + strlen(key) + 40 : 0;
        char *body = cap ? malloc(cap) : NULL;
        if (!owner || !role || !body) {
            free(owner);
            free(role);
            free(payload);
            free(key);
            free(body);
            goto fail;
        }
        snprintf(body, cap, "request_key=%s&entries=%s", key, payload);
        free(payload);
        free(key);
        bool handled = workspace_handle("POST", "/api/time-entries/batch", "", body, actor, owner, role, r);
        free(body);
        free(owner);
        free(role);
        if (!handled) {
            failure(r);
            return;
        }
        if (r->status >= 400) {
            if (prepare(db, &s,
                        "UPDATE ii_time_drafts SET state='draft' WHERE id=? AND user_id=? AND revision=? AND "
                        "state='confirming'") &&
                number(s, 1, id) && number(s, 2, actor) && number(s, 3, revision))
                (void)db_step(s);
            db_finalize(s);
            return;
        }
        if (!stamp(now) ||
            !prepare(db, &s,
                     "UPDATE ii_time_drafts SET state='confirmed',confirmed_at=? WHERE id=? AND user_id=? "
                     "AND revision=?") ||
            !text(s, 1, now) || !number(s, 2, id) || !number(s, 3, actor) || !number(s, 4, revision) ||
            db_step(s) != DB_DONE) {
            db_finalize(s); /* Batch is idempotent; retry confirms its existing result. */
            return;
        }
        db_finalize(s);
        return;
    }
    const char *source = im_string(f, "source"), *ref = im_string(f, "source_ref");
    if ((strcmp(source, "calendar") && strcmp(source, "ticket") && strcmp(source, "manual")) ||
        !bounded(ref, 512, false) || !stamp(now)) {
        im_error(r, 422, "Indica una provenienza valida per la bozza.");
        return;
    }
    if (*im_string(f, "entries"))
        rows = parse_rows(im_string(f, "entries"));
    if (!strcmp(source, "calendar")) {
        if (!im_integer(f, "job_id", 1, LLONG_MAX, &job)) {
            json_object_put(rows);
            im_error(r, 422, "Seleziona la commessa dell’evento.");
            return;
        }
        struct json_object *verified = calendar_rows(actor, job, ref, r);
        if (!verified) {
            json_object_put(rows);
            return;
        }
        if (rows)
            json_object_put(verified);
        else
            rows = verified;
    }
    if (!validate_rows(db, rows, actor, p, r)) {
        json_object_put(rows);
        return;
    }
    if (!im_begin(db, r)) {
        json_object_put(rows);
        return;
    }
    AccessPolicy live;
    if (!policy(db, actor, &live, r) || !validate_rows(db, rows, actor, &live, r)) {
        im_rollback(db);
        json_object_put(rows);
        return;
    }
    if (*im_string(f, "id")) {
        if (!im_integer(f, "id", 1, LLONG_MAX, &id) || !im_integer(f, "revision", 1, LLONG_MAX, &revision)) {
            im_rollback(db);
            json_object_put(rows);
            im_error(r, 422, "Revisione della bozza richiesta.");
            return;
        }
        if (!prepare(db, &s,
                     "UPDATE ii_time_drafts SET entries_json=?,source=?,source_ref=?,revision=revision+1 "
                     "WHERE id=? AND user_id=? "
                     "AND revision=? AND state='draft'") ||
            !text(s, 1, json_object_to_json_string_ext(rows, JSON_C_TO_STRING_PLAIN)) ||
            !text(s, 2, source) || !text(s, 3, ref) || !number(s, 4, id) || !number(s, 5, actor) ||
            !number(s, 6, revision) || db_step(s) != DB_DONE)
            goto rollback;
        if (db_changes(db) != 1) {
            db_finalize(s);
            im_rollback(db);
            json_object_put(rows);
            im_error(r, 409, "La bozza è già confermata o è cambiata.");
            return;
        }
    } else {
        if (!prepare(db, &s,
                     "INSERT INTO "
                     "ii_time_drafts(user_id,source,source_ref,entries_json,revision,state,request_key,"
                     "created_at) VALUES(?,?,?,?,1,'draft',?,?)") ||
            !number(s, 1, actor) || !text(s, 2, source) || !text(s, 3, ref) ||
            !text(s, 4, json_object_to_json_string_ext(rows, JSON_C_TO_STRING_PLAIN)))
            goto rollback;
        /* Random key, immutable for this draft; replaying confirmation cannot duplicate hours. */
        unsigned char random[16];
        FILE *random_file = fopen("/dev/urandom", "rb");
        bool random_ok = random_file && fread(random, 1, sizeof(random), random_file) == sizeof(random);
        if (random_file)
            fclose(random_file);
        if (!random_ok)
            goto rollback;
        char key[40] = "draft_";
        for (int i = 0; i < 16; i++)
            snprintf(key + 6 + i * 2, 3, "%02x", random[i]);
        if (!text(s, 5, key) || !text(s, 6, now) || db_step(s) != DB_DONE)
            goto rollback;
        id = db_last_insert_rowid(db);
    }
    db_finalize(s);
    s = NULL;
    o = okay();
    addn(o, "id", id);
    addn(o, "revision", revision + 1);
    json_object_object_add(o, "entries", rows);
    rows = NULL;
    if (!im_audit_commit(db, actor, "time_drafts.save", "time_draft", id, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 201, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    json_object_put(rows);
    failure(r);
    return;
fail:
    db_finalize(s);
    json_object_put(o);
    json_object_put(rows);
    failure(r);
}

static void person_accounts(Db *db, const char *method, struct json_object *f, DbInt actor,
                            const AccessPolicy *p, WorkspaceResponse *r) {
    if (!p->admin && !p->manager) {
        im_error(r, 403, "Occorre una responsabilità amministrativa.");
        return;
    }
    if (!fields_only(f, "|person_id||user_id||revision||remove|")) {
        im_error(r, 400, "Campi del collegamento non validi.");
        return;
    }
    DbStmt *s = NULL;
    DbInt person = 0, user = 0, revision = 0;
    char now[21];
    struct json_object *o = NULL;
    if (!strcmp(method, "GET")) {
        o = okay();
        struct json_object *items = json_object_new_array(), *users = json_object_new_array();
        if (!prepare(
                db, &s,
                "SELECT m.person_id,m.user_id,m.person_name,m.revision,m.verified_at,u.name,u.active FROM "
                "ii_inventory_person_accounts m JOIN users u ON u.id=m.user_id ORDER BY m.person_name"))
            goto listfail;
        int step;
        while ((step = db_step(s)) == DB_ROW) {
            if (access_policy_can_manage_user(db, actor, db_column_int64(s, 1)) != 1)
                continue;
            struct json_object *v = json_object_new_object();
            addn(v, "person_id", db_column_int64(s, 0));
            addn(v, "user_id", db_column_int64(s, 1));
            adds(v, "person_name", column(s, 2));
            addn(v, "revision", db_column_int64(s, 3));
            adds(v, "verified_at", column(s, 4));
            adds(v, "user_name", column(s, 5));
            addb(v, "active", db_column_int(s, 6) != 0);
            json_object_array_add(items, v);
        }
        if (step != DB_DONE)
            goto listfail;
        db_finalize(s);
        s = NULL;
        if (!prepare(db, &s, "SELECT id,name,email FROM users WHERE active=1 ORDER BY name LIMIT 1000"))
            goto listfail;
        while ((step = db_step(s)) == DB_ROW) {
            if (access_policy_can_manage_user(db, actor, db_column_int64(s, 0)) != 1)
                continue;
            struct json_object *v = json_object_new_object();
            addn(v, "id", db_column_int64(s, 0));
            adds(v, "name", column(s, 1));
            adds(v, "email", column(s, 2));
            json_object_array_add(users, v);
        }
        if (step != DB_DONE)
            goto listfail;
        db_finalize(s);
        json_object_object_add(o, "mappings", items);
        json_object_object_add(o, "users", users);
        im_reply(r, 200, o);
        return;
    listfail:
        db_finalize(s);
        json_object_put(items);
        json_object_put(users);
        json_object_put(o);
        failure(r);
        return;
    }
    if (!im_integer(f, "person_id", 1, LLONG_MAX, &person) ||
        !im_integer(f, "user_id", 1, LLONG_MAX, &user) ||
        !im_integer(f, "revision", 0, LLONG_MAX, &revision) || !stamp(now)) {
        im_error(r, 422, "Seleziona una persona e un account attivo.");
        return;
    }
    if (access_policy_can_manage_user(db, actor, user) != 1) {
        im_error(r, 403, "Non puoi gestire questo account.");
        return;
    }
    bool remove = !strcmp(im_string(f, "remove"), "1");
    char *name = NULL;
    if (!remove) {
        InventoryResponse remote = {0};
        inventory_client_call(INVENTORY_PERSON_DETAIL, person, 0, actor, "", &remote);
        struct json_object *v = remote.body ? parse_object(remote.body) : NULL, *item = NULL, *active = NULL;
        if (v) {
            json_object_object_get_ex(v, "person", &item);
            if (!item)
                json_object_object_get_ex(v, "item", &item);
        }
        if (item)
            json_object_object_get_ex(item, "active", &active);
        if (remote.status != 200 || !item || jint(item, "id") != person || !json_object_get_boolean(active) ||
            !bounded(jtext(item, "display_name"), 160, true)) {
            int status = remote.status >= 500 ? 503 : 422;
            json_object_put(v);
            inventory_response_free(&remote);
            im_error(r, status, "La persona inventario non è disponibile o attiva.");
            return;
        }
        name = strdup(jtext(item, "display_name"));
        json_object_put(v);
        inventory_response_free(&remote);
        if (!name)
            goto fail;
    }
    if (!im_begin(db, r)) {
        free(name);
        return;
    }
    if (access_policy_can_manage_user(db, actor, user) != 1) {
        im_rollback(db);
        free(name);
        im_error(r, 403, "Non puoi gestire questo account.");
        return;
    }
    if (!prepare(db, &s, "SELECT user_id,revision FROM ii_inventory_person_accounts WHERE person_id=?") ||
        !number(s, 1, person))
        goto rollback;
    int step = db_step(s);
    if (step == DB_ROW) {
        DbInt old = db_column_int64(s, 0);
        if (db_column_int64(s, 1) != revision || access_policy_can_manage_user(db, actor, old) != 1 ||
            (remove && old != user)) {
            db_finalize(s);
            im_rollback(db);
            free(name);
            im_error(r, 409, "Collegamento cambiato o non gestibile. Ricarica.");
            return;
        }
    } else if (step != DB_DONE)
        goto rollback;
    else if (revision || remove) {
        db_finalize(s);
        im_rollback(db);
        free(name);
        im_error(r, 409, "Collegamento non più presente.");
        return;
    }
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s,
                 remove ? "SELECT 1 FROM users WHERE id=?" : "SELECT 1 FROM users WHERE id=? AND active=1") ||
        !number(s, 1, user) || db_step(s) != DB_ROW)
        goto rollback;
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s, "DELETE FROM ii_inventory_person_accounts WHERE person_id=?") ||
        !number(s, 1, person) || db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    s = NULL;
    if (!remove &&
        (!prepare(db, &s,
                  "INSERT INTO "
                  "ii_inventory_person_accounts(person_id,user_id,person_name,revision,verified_by,verified_"
                  "at) VALUES(?,?,?,?,?,?)") ||
         !number(s, 1, person) || !number(s, 2, user) || !text(s, 3, name) || !number(s, 4, revision + 1) ||
         !number(s, 5, actor) || !text(s, 6, now) || db_step(s) != DB_DONE))
        goto rollback;
    db_finalize(s);
    free(name);
    o = okay();
    addn(o, "person_id", person);
    addn(o, "user_id", user);
    addb(o, "removed", remove);
    if (!im_audit_commit(db, actor, "inventory_person_accounts.update", "user", user, NULL,
                         json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 200, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    free(name);
    failure(r);
    return;
fail:
    free(name);
    failure(r);
}

bool improvements_integrations_handle(Db *db, const char *method, const char *target, const char *query,
                                      const char *body, DbInt actor, WorkspaceResponse *r) {
    if (strncmp(target, II_PREFIX, strlen(II_PREFIX)))
        return false;
    const char *route = target + strlen(II_PREFIX);
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (!get && !post) {
        im_error(r, 405, "Metodo non consentito.");
        return true;
    }
    if (post && query && *query) {
        im_error(r, 400, "I dati di salvataggio devono essere nel modulo.");
        return true;
    }
    AccessPolicy p;
    if (!policy(db, actor, &p, r))
        return true;
    struct json_object *f = im_fields(get ? query : body);
    if (!f) {
        im_error(r, 400, "Campi non validi o duplicati.");
        return true;
    }
    if (!strcmp(route, "contract-revisions") || (!strcmp(route, "contract-revisions/publish") && post))
        revisions(db, method, f, actor, &p, r, strstr(route, "/publish") != NULL);
    else if (!strcmp(route, "job-contract-revision"))
        job_revision(db, method, f, actor, &p, r);
    else if (!strcmp(route, "economic-periods") || (!strcmp(route, "economic-corrections") && post))
        periods(db, method, f, actor, &p, r, !strcmp(route, "economic-corrections"));
    else if (!strcmp(route, "budget"))
        budget(db, method, f, actor, &p, r);
    else if (!strcmp(route, "time-drafts") || (!strcmp(route, "time-drafts/confirm") && post) ||
             (!strcmp(route, "calendar-candidates") && get))
        drafts(db, method, f, actor, &p, r, !strcmp(route, "time-drafts/confirm"),
               !strcmp(route, "calendar-candidates"));
    else if (!strcmp(route, "inventory-person-accounts"))
        person_accounts(db, method, f, actor, &p, r);
    else if (!strcmp(route, "email-outbox") || (!strcmp(route, "email-outbox/decision") && post))
        alerts_outbox_handle(db, method, f, actor, r);
    else
        im_error(r, 404, "Percorso non trovato.");
    json_object_put(f);
    return true;
}

void alerts_outbox_handle(Db *db, const char *method, struct json_object *f, DbInt actor,
                          WorkspaceResponse *r) {
    AccessPolicy p;
    if (!policy(db, actor, &p, r))
        return;
    if (!p.admin && !p.manager) {
        im_error(r, 403, "La coda email richiede una responsabilità amministrativa.");
        return;
    }
    if (!fields_only(f, "|after_id||id||revision||status||decision||reason||outcome|")) {
        im_error(r, 400, "Campi della coda email non validi.");
        return;
    }
    DbStmt *s = NULL;
    DbInt id = 0, revision = 0;
    char now[21];
    if (!strcmp(method, "GET")) {
        DbInt after = 0;
        if (*im_string(f, "after_id") && !im_integer(f, "after_id", 1, LLONG_MAX, &after)) {
            im_error(r, 422, "Cursore email non valido.");
            return;
        }
        const char *filter = im_string(f, "status");
        if (*filter && strcmp(filter, "pending_configuration") && strcmp(filter, "retrying") &&
            strcmp(filter, "sent") && strcmp(filter, "delivery_unknown") &&
            strcmp(filter, "delivery_failed") && strcmp(filter, "cancelled") && strcmp(filter, "sending")) {
            im_error(r, 422, "Filtro email non valido.");
            return;
        }
        const char *sql = "SELECT "
                          "e.id,e.rule_id,e.email,e.title,e.message,e.status,e.created_at,e.subject_user_id,"
                          "r.owner_id,r.name,COALESCE(c.state,'normal'),COALESCE(c.revision,0),COALESCE(c."
                          "review_outcome,''),COALESCE(c.reason,'') FROM alert_email_outbox e JOIN "
                          "alert_rules r ON r.id=e.rule_id LEFT JOIN ii_email_controls c ON c.outbox_id=e.id "
                          "WHERE (?=0 OR e.id<?) AND (?=1 OR r.owner_id=?) ORDER BY e.id DESC";
        if (!prepare(db, &s, sql) || !number(s, 1, after) || !number(s, 2, after) || !number(s, 3, p.admin) ||
            !number(s, 4, actor))
            goto fail;
        struct json_object *o = okay(), *items = json_object_new_array();
        int step, count = 0;
        DbInt next = 0;
        while ((step = db_step(s)) == DB_ROW) {
            DbInt subject = db_column_int64(s, 7);
            if (subject > 0 && access_policy_can_view_time(db, actor, subject, p.manager) != 1)
                continue;
            const char *state = column(s, 10), *status = strcmp(state, "normal") ? state : column(s, 5);
            if (*filter && strcmp(filter, status))
                continue;
            if (count++ == 100) {
                next = db_column_int64(s, 0) + 1;
                break;
            }
            struct json_object *v = json_object_new_object();
            addn(v, "id", db_column_int64(s, 0));
            addn(v, "rule_id", db_column_int64(s, 1));
            adds(v, "email", column(s, 2));
            adds(v, "title", column(s, 3));
            adds(v, "message", column(s, 4));
            adds(v, "status", status);
            adds(v, "delivery_status", column(s, 5));
            adds(v, "created_at", column(s, 6));
            adds(v, "rule_name", column(s, 9));
            addn(v, "revision", db_column_int64(s, 11));
            adds(v, "review_outcome", column(s, 12));
            adds(v, "reason", column(s, 13));
            addb(v, "can_cancel",
                 !strcmp(state, "normal") &&
                     (!strcmp(column(s, 5), "pending_configuration") || !strcmp(column(s, 5), "retrying")));
            addb(v, "can_review", !strcmp(state, "normal") && !strcmp(column(s, 5), "delivery_unknown"));
            json_object_array_add(items, v);
        }
        if (step != DB_DONE && count <= 100) {
            json_object_put(o);
            json_object_put(items);
            goto fail;
        }
        db_finalize(s);
        json_object_object_add(o, "items", items);
        if (next)
            addn(o, "next_after_id", next);
        else
            json_object_object_add(o, "next_after_id", NULL);
        im_reply(r, 200, o);
        return;
    }
    const char *decision = im_string(f, "decision"), *reason = im_string(f, "reason"),
               *outcome = im_string(f, "outcome"), *expected = im_string(f, "status");
    if (!im_integer(f, "id", 1, LLONG_MAX, &id) || !im_integer(f, "revision", 0, LLONG_MAX, &revision) ||
        !bounded(reason, 1000, true) || !stamp(now) ||
        (strcmp(decision, "cancel") && strcmp(decision, "review"))) {
        im_error(r, 422, "Indica email, revisione, decisione e motivazione.");
        return;
    }
    bool cancel = !strcmp(decision, "cancel");
    if (!cancel && strcmp(outcome, "confirmed_sent") && strcmp(outcome, "confirmed_not_sent") &&
        strcmp(outcome, "unresolved")) {
        im_error(r, 422, "Indica l’esito verificato presso il fornitore.");
        return;
    }
    if (!im_begin(db, r))
        return;
    if (!policy(db, actor, &p, r)) {
        im_rollback(db);
        return;
    }
    if (!p.admin && !p.manager) {
        im_rollback(db);
        im_error(r, 403, "La coda email richiede una responsabilità amministrativa.");
        return;
    }
    if (!prepare(
            db, &s,
            "SELECT e.status,e.subject_user_id,r.owner_id,COALESCE(c.state,'normal'),COALESCE(c.revision,0) "
            "FROM alert_email_outbox e JOIN alert_rules r ON r.id=e.rule_id LEFT JOIN ii_email_controls c ON "
            "c.outbox_id=e.id WHERE e.id=?") ||
        !number(s, 1, id))
        goto rollback;
    if (db_step(s) != DB_ROW) {
        db_finalize(s);
        im_rollback(db);
        im_error(r, 404, "Email non trovata.");
        return;
    }
    if ((!p.admin && db_column_int64(s, 2) != actor) ||
        (db_column_int64(s, 1) > 0 &&
         access_policy_can_view_time(db, actor, db_column_int64(s, 1), p.manager) != 1)) {
        db_finalize(s);
        im_rollback(db);
        im_error(r, 403, "Non puoi consultare questa email.");
        return;
    }
    bool matches = !strcmp(column(s, 3), "normal") && db_column_int64(s, 4) == revision &&
                   !strcmp(column(s, 0), expected);
    bool permitted =
        cancel ? (!strcmp(column(s, 0), "pending_configuration") || !strcmp(column(s, 0), "retrying"))
               : !strcmp(column(s, 0), "delivery_unknown");
    if (!matches || !permitted) {
        db_finalize(s);
        im_rollback(db);
        im_error(r, 409, "Lo stato email è cambiato o l’invio è già iniziato. Ricarica.");
        return;
    }
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s, "DELETE FROM ii_email_controls WHERE outbox_id=?") || !number(s, 1, id) ||
        db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    s = NULL;
    if (!prepare(db, &s,
                 "INSERT INTO "
                 "ii_email_controls(outbox_id,state,revision,review_outcome,reason,updated_by,updated_at) "
                 "VALUES(?,?,?,?,?,?,?)") ||
        !number(s, 1, id) || !text(s, 2, cancel ? "cancelled" : "normal") || !number(s, 3, revision + 1) ||
        !text(s, 4, cancel ? "" : outcome) || !text(s, 5, reason) || !number(s, 6, actor) ||
        !text(s, 7, now) || db_step(s) != DB_DONE)
        goto rollback;
    db_finalize(s);
    struct json_object *o = okay();
    addn(o, "id", id);
    adds(o, "decision", decision);
    adds(o, "outcome", outcome);
    adds(o, "reason", reason);
    addb(o, "resend", false);
    if (!im_audit_commit(db, actor, cancel ? "alert_emails.cancel" : "alert_emails.review", "alert_email", id,
                         NULL, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), r)) {
        json_object_put(o);
        return;
    }
    im_reply(r, 200, o);
    return;
rollback:
    db_finalize(s);
    im_rollback(db);
    failure(r);
    return;
fail:
    db_finalize(s);
    failure(r);
}
