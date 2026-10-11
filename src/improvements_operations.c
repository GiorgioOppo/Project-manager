#include "improvements_operations.h"
#include "improvements.h"
#include "organization.h"
#include "storage.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef PUNTO_MYSQL
#define IO_ID "BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY"
#define IO_ENGINE " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
#else
#define IO_ID "INTEGER PRIMARY KEY AUTOINCREMENT"
#define IO_ENGINE ""
#endif

bool improvements_operations_schema(Db *db) {
    const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS operations_absences(id " IO_ID
        ",user_id BIGINT NOT NULL,starts_on CHAR(10) NOT NULL,ends_on CHAR(10) NOT "
        "NULL,expected_minutes INTEGER NOT NULL,reason TEXT NOT NULL,status VARCHAR(16) NOT NULL "
        "DEFAULT 'pending',revision BIGINT NOT NULL DEFAULT 1,decided_by BIGINT "
        "NULL,decision_reason TEXT NULL,created_at CHAR(20) NOT NULL,decided_at CHAR(20) "
        "NULL,FOREIGN KEY(user_id) REFERENCES users(id),FOREIGN KEY(decided_by) REFERENCES "
        "users(id),CHECK(expected_minutes BETWEEN 0 AND 1440),CHECK(revision>=1),CHECK(status IN "
        "('pending','approved','rejected','cancelled')))" IO_ENGINE,
        "CREATE TABLE IF NOT EXISTS operations_template_versions(id " IO_ID
        ",template_id BIGINT NOT NULL,revision BIGINT NOT NULL,name VARCHAR(120) NOT "
        "NULL,definition_json TEXT NOT NULL,archived INTEGER NOT NULL,created_by BIGINT NOT "
        "NULL,created_at CHAR(20) NOT NULL,UNIQUE(template_id,revision),FOREIGN KEY(template_id) "
        "REFERENCES job_templates(id) ON DELETE CASCADE,FOREIGN KEY(created_by) REFERENCES "
        "users(id),CHECK(archived IN (0,1)))" IO_ENGINE,
        "CREATE TABLE IF NOT EXISTS operations_checklist(id " IO_ID
        ",job_id BIGINT NOT NULL,kind VARCHAR(16) NOT NULL,resource_id BIGINT NOT NULL,label "
        "VARCHAR(160) NOT NULL,status VARCHAR(16) NOT NULL DEFAULT 'open',revision BIGINT NOT NULL "
        "DEFAULT 1,created_by BIGINT NOT NULL,UNIQUE(job_id,kind,resource_id),FOREIGN KEY(job_id) "
        "REFERENCES jobs(id) ON DELETE CASCADE,FOREIGN KEY(created_by) REFERENCES "
        "users(id),CHECK(kind IN ('ticket','asset')),CHECK(status IN "
        "('open','done')),CHECK(revision>=1))" IO_ENGINE};
    for (size_t i = 0; i < sizeof(ddl) / sizeof(*ddl); i++)
        if (!im_exec(db, ddl[i]))
            return false;
    return im_column(db, "job_tasks", "estimated_minutes",
                     "ALTER TABLE job_tasks ADD COLUMN estimated_minutes INTEGER NOT NULL DEFAULT "
                     "0 CHECK(estimated_minutes BETWEEN 0 AND 1000000)",
                     "ALTER TABLE job_tasks ADD COLUMN estimated_minutes BIGINT NOT NULL DEFAULT 0 "
                     "CHECK(estimated_minutes BETWEEN 0 AND 1000000)") &&
           im_column(db, "user_work_schedules", "revision",
                     "ALTER TABLE user_work_schedules ADD COLUMN revision INTEGER NOT NULL DEFAULT "
                     "1 CHECK(revision>=1)",
                     "ALTER TABLE user_work_schedules ADD COLUMN revision BIGINT NOT NULL DEFAULT "
                     "1 CHECK(revision>=1)") &&
           im_column(db, "job_templates", "revision",
                     "ALTER TABLE job_templates ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 "
                     "CHECK(revision>=1)",
                     "ALTER TABLE job_templates ADD COLUMN revision BIGINT NOT NULL DEFAULT 1 "
                     "CHECK(revision>=1)") &&
           im_column(db, "job_templates", "archived",
                     "ALTER TABLE job_templates ADD COLUMN archived INTEGER NOT NULL DEFAULT 0 "
                     "CHECK(archived IN (0,1))",
                     "ALTER TABLE job_templates ADD COLUMN archived INTEGER NOT NULL DEFAULT 0 "
                     "CHECK(archived IN (0,1))");
}

static bool io_prepare(Db *db, DbStmt **st, const char *sql) {
    return db_prepare_v2(db, sql, -1, st, NULL) == DB_OK;
}
static bool io_id(DbStmt *st, int n, DbInt id) { return db_bind_int64(st, n, id) == DB_OK; }
static bool io_text(DbStmt *st, int n, const char *text) {
    return db_bind_text(st, n, text, -1, DB_TRANSIENT) == DB_OK;
}
static const char *io_column(DbStmt *st, int n) {
    const unsigned char *s = db_column_text(st, n);
    return s ? (const char *)s : "";
}
static bool io_stamp(char out[21]) {
    time_t now = time(NULL);
    struct tm utc;
    return now != (time_t)-1 && gmtime_r(&now, &utc) &&
           strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
}
static bool io_known(struct json_object *f, const char *const *keys) {
    json_object_object_foreach(f, key, value) {
        (void)value;
        bool found = false;
        for (size_t i = 0; keys[i]; i++)
            if (!strcmp(key, keys[i]))
                found = true;
        if (!found)
            return false;
    }
    return true;
}
static bool io_length(const char *s, size_t min, size_t max) {
    size_t n = strlen(s);
    bool nonblank = false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] > 32)
            nonblank = true;
    if (n < min || n > max || (min && !nonblank))
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
            return false;
    return true;
}
static struct json_object *io_json(const char *s) {
    struct json_tokener *tok = json_tokener_new();
    if (!tok)
        return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    struct json_object *v = json_tokener_parse_ex(tok, s, (int)strlen(s));
    size_t end = json_tokener_get_parse_end(tok);
    while (s[end] == ' ' || s[end] == '\n' || s[end] == '\r' || s[end] == '\t')
        end++;
    bool okay = json_tokener_get_error(tok) == json_tokener_success && !s[end];
    json_tokener_free(tok);
    if (!okay) {
        if (v)
            json_object_put(v);
        return NULL;
    }
    return v;
}
static struct json_object *io_row(DbStmt *st, const char *const *keys, int count) {
    struct json_object *out = json_object_new_object();
    if (!out)
        return NULL;
    for (int i = 0; i < count; i++) {
        struct json_object *v = db_column_type(st, i) == DB_NULL ? NULL
                                : db_column_type(st, i) == DB_INTEGER
                                    ? json_object_new_int64(db_column_int64(st, i))
                                    : json_object_new_string(io_column(st, i));
        json_object_object_add(out, keys[i], v);
    }
    return out;
}
static void io_ok(WorkspaceResponse *r, DbInt id, DbInt revision) {
    struct json_object *v = json_object_new_object();
    json_object_object_add(v, "ok", json_object_new_boolean(true));
    json_object_object_add(v, "id", json_object_new_int64(id));
    json_object_object_add(v, "revision", json_object_new_int64(revision));
    im_reply(r, 200, v);
}
static void io_failure(Db *db, WorkspaceResponse *r) {
    im_rollback(db);
    im_error(r, 500, "Salvataggio non riuscito: nessuna modifica confermata.");
}
static bool io_permission(int allowed, WorkspaceResponse *r) {
    if (allowed == 1)
        return true;
    im_error(r, allowed < 0 ? 503 : 403,
             allowed < 0 ? "Servizio temporaneamente non disponibile."
                         : "Non hai i permessi per questa operazione.");
    return false;
}
static int io_ordinal(const char *date) {
    int y = atoi(date), m = atoi(date + 5), d = atoi(date + 8), prior = y - 1;
    int out = prior * 365 + prior / 4 - prior / 100 + prior / 400 + d;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int i = 1; i < m; i++)
        out += days[i - 1] + (i == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return out;
}
static void io_next(char day[11]) {
    int y = atoi(day), m = atoi(day + 5), d = atoi(day + 8) + 1;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (d > max) {
        d = 1;
        if (++m > 12) {
            m = 1;
            y++;
        }
    }
    char next[64];
    int length = snprintf(next, sizeof(next), "%04d-%02d-%02d", y, m, d);
    if (length == 10)
        memcpy(day, next, 11);
}
static int io_job_visible(Db *db, DbInt actor, DbInt job) {
    int managed = access_policy_can_manage_job(db, actor, job);
    if (managed != 0)
        return managed;
    char today[11];
    if (!im_today(today))
        return -1;
    int access = organization_job_access(db, job, actor, today, NULL);
    return access < 0 ? -1 : (access & 1) != 0;
}
static bool io_scope(Db *db, DbInt actor, char out[5000]) {
    AccessPolicy p;
    DbInt ids[201];
    size_t n = 0, used = 0;
    if (access_policy_resolve(db, actor, NULL, &p) != 1 ||
        !access_policy_visible_users(db, actor, p.manager, ids, &n))
        return false;
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(out + used, 5000 - used, "%s%lld", i ? "," : "", (long long)ids[i]);
        if (len < 0 || (size_t)len >= 5000 - used)
            return false;
        used += (size_t)len;
    }
    return n > 0;
}

static int io_expected(Db *db, DbInt user, const char *day) {
    DbStmt *st = NULL;
    int result, minutes;
    if (!io_prepare(
            db, &st,
            "SELECT expected_minutes FROM user_work_exceptions WHERE user_id=? AND date=?") ||
        !io_id(st, 1, user) || !io_text(st, 2, day))
        goto failed;
    result = db_step(st);
    if (result == DB_ROW) {
        minutes = db_column_int(st, 0);
        db_finalize(st);
        return minutes;
    }
    db_finalize(st);
    st = NULL;
    if (result != DB_DONE)
        goto failed;
    if (!io_prepare(db, &st, "SELECT date FROM business_holidays WHERE date=?") ||
        !io_text(st, 1, day))
        goto failed;
    result = db_step(st);
    db_finalize(st);
    st = NULL;
    if (result == DB_ROW)
        return 0;
    if (result != DB_DONE)
        goto failed;
    if (!io_prepare(db, &st,
                    "SELECT mon,tue,wed,thu,fri,sat,sun FROM user_work_schedules WHERE user_id=? "
                    "AND starts_on<=? ORDER BY starts_on DESC LIMIT 1") ||
        !io_id(st, 1, user) || !io_text(st, 2, day))
        goto failed;
    result = db_step(st);
    int weekday = (io_ordinal(day) - 1) % 7;
    minutes = result == DB_ROW ? db_column_int(st, weekday) : weekday < 5 ? 480 : 0;
    db_finalize(st);
    return result == DB_ROW || result == DB_DONE ? minutes : -1;
failed:
    db_finalize(st);
    return -1;
}

static void io_capacity(Db *db, struct json_object *f, DbInt actor, WorkspaceResponse *r) {
    const char *const allowed[] = {"user_id", "from", "to", NULL};
    DbInt user = actor;
    const char *from = im_string(f, "from"), *to = im_string(f, "to");
    if (!io_known(f, allowed) || !im_date(from) || !im_date(to) ||
        io_ordinal(to) < io_ordinal(from) || io_ordinal(to) - io_ordinal(from) > 365 ||
        (*im_string(f, "user_id") && !im_integer(f, "user_id", 1, INT64_MAX, &user))) {
        im_error(r, 400, "Seleziona persona e periodo da 1 a 366 giorni.");
        return;
    }
    AccessPolicy p;
    if (access_policy_resolve(db, actor, NULL, &p) != 1) {
        im_error(r, 403, "Account non disponibile.");
        return;
    }
    if (!io_permission(access_policy_can_view_time(db, actor, user, p.manager), r))
        return;
    char day[11];
    memcpy(day, from, 11);
    DbInt expected = 0, recorded = 0, planned = 0;
    struct json_object *days = json_object_new_array();
    DbStmt *st = NULL;
    for (int i = 0; i <= io_ordinal(to) - io_ordinal(from); i++) {
        int minutes = io_expected(db, user, day);
        if (minutes < 0)
            goto failed;
        expected += minutes;
        struct json_object *item = json_object_new_object();
        json_object_object_add(item, "date", json_object_new_string(day));
        json_object_object_add(item, "expected_minutes", json_object_new_int(minutes));
        json_object_array_add(days, item);
        if (strcmp(day, to))
            io_next(day);
    }
    if (!io_prepare(db, &st,
                    "SELECT COALESCE(SUM(minutes),0) FROM time_entries WHERE user_id=? AND date>=? "
                    "AND date<=?") ||
        !io_id(st, 1, user) || !io_text(st, 2, from) || !io_text(st, 3, to) ||
        db_step(st) != DB_ROW)
        goto failed;
    recorded = db_column_int64(st, 0);
    db_finalize(st);
    st = NULL;
    if (!io_prepare(
            db, &st,
            "SELECT t.job_id,t.estimated_minutes FROM job_tasks t JOIN jobs j ON j.id=t.job_id "
            "WHERE t.assigned_user_id=? AND t.status<>'done' AND j.archived=0 "
            "AND (t.due_date IS NULL OR (t.due_date>=? AND t.due_date<=?))") ||
        !io_id(st, 1, user) || !io_text(st, 2, from) || !io_text(st, 3, to))
        goto failed;
    int result;
    while ((result = db_step(st)) == DB_ROW) {
        int visible = io_job_visible(db, user, db_column_int64(st, 0));
        if (visible < 0)
            goto failed;
        if (visible == 1)
            planned += db_column_int64(st, 1);
    }
    if (result != DB_DONE)
        goto failed;
    db_finalize(st);
    st = NULL;
    struct json_object *out = json_object_new_object();
    json_object_object_add(out, "user_id", json_object_new_int64(user));
    json_object_object_add(out, "expected_minutes", json_object_new_int64(expected));
    json_object_object_add(out, "recorded_minutes", json_object_new_int64(recorded));
    json_object_object_add(out, "planned_minutes", json_object_new_int64(planned));
    json_object_object_add(out, "available_minutes", json_object_new_int64(expected - planned));
    json_object_object_add(out, "days", days);
    im_reply(r, 200, out);
    return;
failed:
    db_finalize(st);
    json_object_put(days);
    im_error(r, 500, "Impossibile calcolare il lavoro atteso.");
}

static void io_task_plan(Db *db, const char *method, struct json_object *f, DbInt actor,
                         WorkspaceResponse *r) {
    const char *const allowed[] = {"task_id", "estimated_minutes", "revision", NULL};
    DbInt task = 0, estimate = 0, revision = 0;
    DbStmt *st = NULL;
    if (!io_known(f, allowed) || !im_integer(f, "task_id", 1, INT64_MAX, &task)) {
        im_error(r, 400, "Seleziona un’attività valida.");
        return;
    }
    if (!strcmp(method, "POST") && (!im_integer(f, "estimated_minutes", 0, 1000000, &estimate) ||
                                    !im_integer(f, "revision", 1, INT64_MAX - 1, &revision))) {
        im_error(r, 400, "Indica stima e revisione correnti.");
        return;
    }
    if (!strcmp(method, "POST") && !im_begin(db, r))
        return;
    if (!io_prepare(
            db, &st,
            "SELECT id,job_id,title,estimated_minutes,revision FROM job_tasks WHERE id=?") ||
        !io_id(st, 1, task))
        goto failed;
    int found = db_step(st);
    DbInt job = found == DB_ROW ? db_column_int64(st, 1) : 0;
    if (found != DB_ROW && found != DB_DONE)
        goto failed;
    int allowed_access =
        job ? (!strcmp(method, "GET") ? io_job_visible(db, actor, job)
                                      : access_policy_can_manage_job(db, actor, job))
            : 0;
    if (allowed_access != 1) {
        db_finalize(st);
        if (strcmp(method, "GET"))
            im_rollback(db);
        im_error(r, allowed_access < 0 ? 503 : 404, "Attività non disponibile.");
        return;
    }
    const char *const keys[] = {"id", "job_id", "title", "estimated_minutes", "revision"};
    struct json_object *before = io_row(st, keys, 5);
    DbInt current = db_column_int64(st, 4);
    db_finalize(st);
    st = NULL;
    if (!strcmp(method, "GET")) {
        im_reply(r, 200, before);
        return;
    }
    if (revision != current) {
        json_object_put(before);
        im_rollback(db);
        im_error(r, 409, "L’attività è cambiata. Ricarica prima di salvare la stima.");
        return;
    }
    char stamp[21];
    if (!io_stamp(stamp)) {
        json_object_put(before);
        goto failed;
    }
    if (!io_prepare(db, &st,
                    "UPDATE job_tasks SET estimated_minutes=?,revision=revision+1,updated_at=? "
                    "WHERE id=? AND revision=?") ||
        !io_id(st, 1, estimate) || !io_text(st, 2, stamp) || !io_id(st, 3, task) ||
        !io_id(st, 4, revision) || db_step(st) != DB_DONE || db_changes(db) != 1) {
        json_object_put(before);
        goto failed;
    }
    db_finalize(st);
    st = NULL;
    struct json_object *after = io_json(json_object_to_json_string(before));
    json_object_object_add(after, "estimated_minutes", json_object_new_int64(estimate));
    json_object_object_add(after, "revision", json_object_new_int64(revision + 1));
    bool saved =
        im_audit_commit(db, actor, "tasks.estimate", "task", task,
                        json_object_to_json_string(before), json_object_to_json_string(after), r);
    json_object_put(before);
    json_object_put(after);
    if (saved)
        io_ok(r, task, revision + 1);
    return;
failed:
    db_finalize(st);
    io_failure(db, r);
}

static void io_absence_list(Db *db, struct json_object *f, DbInt actor, WorkspaceResponse *r) {
    const char *const allowed[] = {"status", "after_id", "limit", NULL};
    const char *status = im_string(f, "status");
    DbInt after = 0, limit = 50;
    char scope[5000], sql[6400];
    if (!io_known(f, allowed) ||
        (*status && strcmp(status, "pending") && strcmp(status, "approved") &&
         strcmp(status, "rejected") && strcmp(status, "cancelled")) ||
        (*im_string(f, "after_id") && !im_integer(f, "after_id", 1, INT64_MAX, &after)) ||
        (*im_string(f, "limit") && !im_integer(f, "limit", 1, 100, &limit))) {
        im_error(r, 400, "Filtro assenze non valido.");
        return;
    }
    if (!io_scope(db, actor, scope)) {
        im_error(r, 503, "Servizio temporaneamente non disponibile.");
        return;
    }
    (void)snprintf(sql, sizeof(sql),
                   "SELECT "
                   "a.id,a.user_id,u.name,a.starts_on,a.ends_on,a.expected_minutes,a.reason,a."
                   "status,a.revision,a.decided_by,a.decision_reason,a.created_at,a.decided_at "
                   "FROM operations_absences a JOIN users u ON u.id=a.user_id WHERE a.user_id IN "
                   "(%s) AND (?='' OR a.status=?) AND (?=0 OR a.id<?) ORDER BY a.id DESC LIMIT 101",
                   scope);
    DbStmt *st = NULL;
    struct json_object *items = json_object_new_array();
    if (!io_prepare(db, &st, sql) || !io_text(st, 1, status) || !io_text(st, 2, status) ||
        !io_id(st, 3, after) || !io_id(st, 4, after))
        goto failed;
    const char *const keys[] = {"id",        "user_id",          "user_name",       "starts_on",
                                "ends_on",   "expected_minutes", "reason",          "status",
                                "revision",  "decided_by",       "decision_reason", "created_at",
                                "decided_at"};
    int step, count = 0;
    DbInt cursor = 0;
    bool more = false;
    while ((step = db_step(st)) == DB_ROW) {
        if (count++ >= limit) {
            more = true;
            break;
        }
        cursor = db_column_int64(st, 0);
        struct json_object *item = io_row(st, keys, 13);
        bool pending = !strcmp(io_column(st, 7), "pending");
        DbInt subject = db_column_int64(st, 1);
        json_object_object_add(
            item, "can_decide",
            json_object_new_boolean(pending && subject != actor &&
                                    access_policy_can_manage_user(db, actor, subject) == 1));
        json_object_object_add(item, "can_cancel",
                               json_object_new_boolean(pending && subject == actor));
        json_object_array_add(items, item);
    }
    if (step != DB_DONE && !more)
        goto failed;
    db_finalize(st);
    struct json_object *out = json_object_new_object();
    json_object_object_add(out, "requests", items);
    json_object_object_add(out, "has_more", json_object_new_boolean(more));
    json_object_object_add(out, "next_after_id", more ? json_object_new_int64(cursor) : NULL);
    im_reply(r, 200, out);
    return;
failed:
    db_finalize(st);
    json_object_put(items);
    im_error(r, 500, "Impossibile caricare le richieste di assenza.");
}

static void io_absence_create(Db *db, struct json_object *f, DbInt actor, WorkspaceResponse *r) {
    const char *const allowed[] = {"starts_on", "ends_on", "expected_minutes", "reason", NULL};
    const char *from = im_string(f, "starts_on"), *to = im_string(f, "ends_on"),
               *reason = im_string(f, "reason");
    DbInt minutes = 0;
    char stamp[21];
    if (!io_known(f, allowed) || !im_date(from) || !im_date(to) ||
        io_ordinal(to) < io_ordinal(from) || io_ordinal(to) - io_ordinal(from) > 365 ||
        !im_integer(f, "expected_minutes", 0, 1440, &minutes) || !io_length(reason, 1, 1000) ||
        !io_stamp(stamp)) {
        im_error(r, 400, "Indica intervallo fino a 366 giorni, minuti attesi e motivo.");
        return;
    }
    if (!im_begin(db, r))
        return;
    DbStmt *st = NULL;
    if (!io_prepare(db, &st,
                    "SELECT id FROM operations_absences WHERE user_id=? AND status='pending' AND "
                    "starts_on<=? AND ends_on>=?") ||
        !io_id(st, 1, actor) || !io_text(st, 2, to) || !io_text(st, 3, from))
        goto failed;
    int found = db_step(st);
    db_finalize(st);
    st = NULL;
    if (found == DB_ROW) {
        im_rollback(db);
        im_error(r, 409, "Esiste già una richiesta in attesa per queste giornate.");
        return;
    }
    if (found != DB_DONE)
        goto failed;
    if (!io_prepare(db, &st,
                    "INSERT INTO "
                    "operations_absences(user_id,starts_on,ends_on,expected_minutes,reason,created_"
                    "at) VALUES(?,?,?,?,?,?)") ||
        !io_id(st, 1, actor) || !io_text(st, 2, from) || !io_text(st, 3, to) ||
        !io_id(st, 4, minutes) || !io_text(st, 5, reason) || !io_text(st, 6, stamp) ||
        db_step(st) != DB_DONE)
        goto failed;
    DbInt id = db_last_insert_rowid(db);
    db_finalize(st);
    json_object_object_add(f, "user_id", json_object_new_int64(actor));
    if (im_audit_commit(db, actor, "absence_requests.create", "work_exception", id, NULL,
                        json_object_to_json_string(f), r))
        io_ok(r, id, 1);
    return;
failed:
    db_finalize(st);
    io_failure(db, r);
}

static void io_absence_decide(Db *db, struct json_object *f, DbInt actor, WorkspaceResponse *r) {
    const char *const allowed[] = {"id", "revision", "decision", "reason", NULL};
    DbInt id = 0, revision = 0;
    const char *decision = im_string(f, "decision"), *reason = im_string(f, "reason");
    bool approve = !strcmp(decision, "approved"), cancel = !strcmp(decision, "cancelled");
    char stamp[21];
    if (!io_known(f, allowed) || !im_integer(f, "id", 1, INT64_MAX, &id) ||
        !im_integer(f, "revision", 1, INT64_MAX - 1, &revision) ||
        (!approve && !cancel && strcmp(decision, "rejected")) ||
        !io_length(reason, !strcmp(decision, "rejected") ? 1 : 0, 1000) || !io_stamp(stamp)) {
        im_error(r, 400, "Indica richiesta, revisione e decisione; il rifiuto richiede un motivo.");
        return;
    }
    if (!im_begin(db, r))
        return;
    DbStmt *st = NULL;
    struct json_object *before = NULL, *after = NULL;
    if (!io_prepare(db, &st,
                    "SELECT id,user_id,starts_on,ends_on,expected_minutes,reason,status,revision "
                    "FROM operations_absences WHERE id=?") ||
        !io_id(st, 1, id))
        goto failed;
    int found = db_step(st);
    if (found != DB_ROW && found != DB_DONE)
        goto failed;
    DbInt user = found == DB_ROW ? db_column_int64(st, 1) : 0;
    AccessPolicy p;
    if (access_policy_resolve(db, actor, NULL, &p) != 1)
        goto failed;
    int visible = user ? access_policy_can_view_time(db, actor, user, p.manager) : 0;
    if (visible < 0)
        goto failed;
    if (!visible) {
        db_finalize(st);
        im_rollback(db);
        im_error(r, 404, "Richiesta non disponibile.");
        return;
    }
    const char *const keys[] = {"id",     "user_id", "starts_on", "ends_on", "expected_minutes",
                                "reason", "status",  "revision"};
    before = io_row(st, keys, 8);
    DbInt current = db_column_int64(st, 7), minutes = db_column_int64(st, 4);
    char from[11], to[11];
    memcpy(from, io_column(st, 2), 11);
    memcpy(to, io_column(st, 3), 11);
    bool pending = !strcmp(io_column(st, 6), "pending");
    db_finalize(st);
    st = NULL;
    int permitted = cancel          ? user == actor
                    : user != actor ? access_policy_can_manage_user(db, actor, user)
                                    : 0;
    if (permitted != 1) {
        im_rollback(db);
        json_object_put(before);
        io_permission(permitted, r);
        return;
    }
    if (!pending || current != revision) {
        im_rollback(db);
        json_object_put(before);
        im_error(r, 409, "La richiesta è già decisa o è cambiata. Ricarica l’elenco.");
        return;
    }
    if (approve) {
        char day[11];
        memcpy(day, from, 11);
        for (int i = 0; i <= io_ordinal(to) - io_ordinal(from); i++) {
            if (!io_prepare(db, &st,
                            "SELECT id,expected_minutes,reason FROM user_work_exceptions WHERE "
                            "user_id=? AND date=?") ||
                !io_id(st, 1, user) || !io_text(st, 2, day))
                goto failed;
            int exists = db_step(st);
            DbInt exception = exists == DB_ROW ? db_column_int64(st, 0) : 0;
            struct json_object *old = NULL;
            if (exists == DB_ROW) {
                const char *const oldkeys[] = {"id", "expected_minutes", "reason"};
                old = io_row(st, oldkeys, 3);
            }
            db_finalize(st);
            st = NULL;
            if (exists != DB_ROW && exists != DB_DONE) {
                json_object_put(old);
                goto failed;
            }
            const char *sql =
                exception
                    ? "UPDATE user_work_exceptions SET expected_minutes=?,reason=? WHERE user_id=? "
                      "AND date=?"
                    : "INSERT INTO user_work_exceptions(expected_minutes,reason,user_id,date) "
                      "VALUES(?,?,?,?)";
            if (!io_prepare(db, &st, sql) || !io_id(st, 1, minutes) ||
                !io_text(st, 2, im_string(before, "reason")) || !io_id(st, 3, user) ||
                !io_text(st, 4, day) || db_step(st) != DB_DONE) {
                json_object_put(old);
                goto failed;
            }
            if (!exception)
                exception = db_last_insert_rowid(db);
            db_finalize(st);
            st = NULL;
            struct json_object *changed = json_object_new_object();
            json_object_object_add(changed, "user_id", json_object_new_int64(user));
            json_object_object_add(changed, "date", json_object_new_string(day));
            json_object_object_add(changed, "expected_minutes", json_object_new_int64(minutes));
            json_object_object_add(changed, "absence_request_id", json_object_new_int64(id));
            bool audited = storage_audit(db, actor, "work_exceptions.save", "work_exception",
                                         exception, old ? json_object_to_json_string(old) : NULL,
                                         json_object_to_json_string(changed));
            json_object_put(old);
            json_object_put(changed);
            if (!audited)
                goto failed;
            if (strcmp(day, to))
                io_next(day);
        }
    }
    if (!io_prepare(db, &st,
                    "UPDATE operations_absences SET "
                    "status=?,revision=revision+1,decided_by=?,decision_reason=?,decided_at=? "
                    "WHERE id=? AND revision=? AND status='pending'") ||
        !io_text(st, 1, decision) || !io_id(st, 2, actor) || !io_text(st, 3, reason) ||
        !io_text(st, 4, stamp) || !io_id(st, 5, id) || !io_id(st, 6, revision) ||
        db_step(st) != DB_DONE || db_changes(db) != 1)
        goto failed;
    db_finalize(st);
    st = NULL;
    after = io_json(json_object_to_json_string(before));
    json_object_object_add(after, "status", json_object_new_string(decision));
    json_object_object_add(after, "revision", json_object_new_int64(revision + 1));
    json_object_object_add(after, "decision_reason", json_object_new_string(reason));
    if (im_audit_commit(db, actor, "absence_requests.decide", "work_exception", id,
                        json_object_to_json_string(before), json_object_to_json_string(after), r))
        io_ok(r, id, revision + 1);
    json_object_put(before);
    json_object_put(after);
    return;
failed:
    db_finalize(st);
    json_object_put(before);
    json_object_put(after);
    io_failure(db, r);
}

static void io_templates(Db *db, const char *method, struct json_object *f, DbInt actor,
                         WorkspaceResponse *r) {
    const char *const allowed[] = {"id",       "name",    "definition", "revision",
                                   "archived", "history", NULL};
    DbInt id = 0, revision = 0, archived = 0;
    AccessPolicy policy;
    if (access_policy_resolve(db, actor, NULL, &policy) != 1) {
        im_error(r, 403, "Non hai i permessi per questa operazione.");
        return;
    }
    if (!io_known(f, allowed) || (*im_string(f, "id") && !im_integer(f, "id", 1, INT64_MAX, &id))) {
        im_error(r, 400, "Modello non valido.");
        return;
    }
    DbStmt *st = NULL;
    struct json_object *before = NULL, *definition = NULL;
    if (!strcmp(method, "GET")) {
        const char *sql =
            *im_string(f, "history")
                ? "SELECT "
                  "v.id,v.template_id,v.name,v.definition_json,v.revision,v.archived,v.created_at "
                  "FROM operations_template_versions v JOIN job_templates t ON t.id=v.template_id "
                  "WHERE (?=1 OR t.created_by=?) AND v.template_id=? ORDER BY v.revision DESC"
                : "SELECT id,id,name,definition_json,revision,archived,created_at FROM "
                  "job_templates WHERE (?=1 OR created_by=?) AND (?=0 OR id=?) ORDER BY name";
        if (!io_prepare(db, &st, sql) || !io_id(st, 1, policy.admin ? 1 : 0) ||
            !io_id(st, 2, actor) || !io_id(st, 3, id) ||
            (!*im_string(f, "history") && !io_id(st, 4, id)))
            goto failed;
        struct json_object *items = json_object_new_array();
        int step;
        const char *const keys[] = {"version_id", "id",       "name",      "definition_json",
                                    "revision",   "archived", "created_at"};
        while ((step = db_step(st)) == DB_ROW) {
            struct json_object *item = io_row(st, keys, 7);
            if (!policy.sales && !policy.finance) {
                struct json_object *safe = io_json(io_column(st, 3));
                if (safe) {
                    json_object_object_del(safe, "contract_id");
                    json_object_object_add(
                        item, "definition_json",
                        json_object_new_string(json_object_to_json_string(safe)));
                    json_object_put(safe);
                }
            }
            json_object_array_add(items, item);
        }
        db_finalize(st);
        st = NULL;
        if (step != DB_DONE) {
            json_object_put(items);
            goto failed;
        }
        struct json_object *out = json_object_new_object();
        json_object_object_add(out, "templates", items);
        im_reply(r, 200, out);
        return;
    }
    if (!id || !im_integer(f, "revision", 1, INT64_MAX - 1, &revision) ||
        !im_integer(f, "archived", 0, 1, &archived) || !io_length(im_string(f, "name"), 1, 120) ||
        !(definition = io_json(im_string(f, "definition"))) ||
        !json_object_is_type(definition, json_type_object) ||
        strlen(im_string(f, "definition")) > 32768) {
        json_object_put(definition);
        im_error(r, 400, "Indica nome, definizione e revisione del modello.");
        return;
    }
    const char *const definition_keys[] = {"name",        "client",    "description", "status",
                                           "contract_id", "is_common", "assignments", "groups",
                                           "tasks",       NULL};
    if (!io_known(definition, definition_keys)) {
        json_object_put(definition);
        im_error(
            r, 400,
            "Il modello deve contenere soltanto dati di commessa, squadra e attività iniziali.");
        return;
    }
    if (!im_begin(db, r)) {
        json_object_put(definition);
        return;
    }
    if (!io_prepare(db, &st,
                    "SELECT id,name,definition_json,revision,archived,created_by FROM "
                    "job_templates WHERE id=?") ||
        !io_id(st, 1, id))
        goto failed;
    int found = db_step(st);
    if (found != DB_ROW && found != DB_DONE)
        goto failed;
    if (found == DB_DONE ||
        (!policy.admin && (!policy.manager || db_column_int64(st, 5) != actor))) {
        db_finalize(st);
        json_object_put(definition);
        im_rollback(db);
        im_error(r, 404, "Modello non disponibile.");
        return;
    }
    const char *const keys[] = {"id",       "name",     "definition_json",
                                "revision", "archived", "created_by"};
    before = io_row(st, keys, 6);
    DbInt current = db_column_int64(st, 3);
    db_finalize(st);
    st = NULL;
    if (!policy.sales) {
        struct json_object *original = io_json(im_string(before, "definition_json")),
                           *old_contract = NULL, *new_contract = NULL;
        if (!original)
            goto failed;
        bool had = json_object_object_get_ex(original, "contract_id", &old_contract),
             has = json_object_object_get_ex(definition, "contract_id", &new_contract);
        if (!has && had)
            json_object_object_add(definition, "contract_id", json_object_get(old_contract));
        else if (has && (!had ? !json_object_is_type(new_contract, json_type_null) &&
                                    strcmp(json_object_get_string(new_contract), "") &&
                                    strcmp(json_object_get_string(new_contract), "0")
                              : !json_object_equal(old_contract, new_contract))) {
            json_object_put(original);
            json_object_put(before);
            json_object_put(definition);
            im_rollback(db);
            im_error(r, 403,
                     "Solo Commerciali può modificare il contratto collegato alla commessa.");
            return;
        }
        json_object_put(original);
    }
    if (current != revision) {
        json_object_put(before);
        json_object_put(definition);
        im_rollback(db);
        im_error(r, 409, "Il modello è cambiato. Ricarica prima di salvarlo.");
        return;
    }
    if (!io_prepare(db, &st, "SELECT id FROM job_templates WHERE name=? AND id<>?") ||
        !io_text(st, 1, im_string(f, "name")) || !io_id(st, 2, id))
        goto failed;
    int duplicate = db_step(st);
    db_finalize(st);
    st = NULL;
    if (duplicate == DB_ROW) {
        json_object_put(before);
        json_object_put(definition);
        im_rollback(db);
        im_error(r, 409, "Esiste già un modello con questo nome.");
        return;
    }
    if (duplicate != DB_DONE)
        goto failed;
    char stamp[21];
    if (!io_stamp(stamp))
        goto failed;
    if (!io_prepare(
            db, &st,
            "INSERT INTO "
            "operations_template_versions(template_id,revision,name,definition_json,archived,"
            "created_by,created_at) SELECT id,revision,name,definition_json,archived,?,? FROM "
            "job_templates WHERE id=? AND NOT EXISTS(SELECT 1 FROM operations_template_versions "
            "WHERE template_id=? AND revision=?)") ||
        !io_id(st, 1, actor) || !io_text(st, 2, stamp) || !io_id(st, 3, id) || !io_id(st, 4, id) ||
        !io_id(st, 5, revision) || db_step(st) != DB_DONE)
        goto failed;
    db_finalize(st);
    st = NULL;
    if (!io_prepare(
            db, &st,
            "UPDATE job_templates SET name=?,definition_json=?,archived=?,revision=revision+1 "
            "WHERE id=? AND revision=?") ||
        !io_text(st, 1, im_string(f, "name")) ||
        !io_text(st, 2, json_object_to_json_string(definition)) || !io_id(st, 3, archived) ||
        !io_id(st, 4, id) || !io_id(st, 5, revision) || db_step(st) != DB_DONE ||
        db_changes(db) != 1)
        goto failed;
    db_finalize(st);
    st = NULL;
    if (!io_prepare(
            db, &st,
            "INSERT INTO "
            "operations_template_versions(template_id,revision,name,definition_json,archived,"
            "created_by,created_at) SELECT id,revision,name,definition_json,archived,?,? FROM "
            "job_templates WHERE id=?") ||
        !io_id(st, 1, actor) || !io_text(st, 2, stamp) || !io_id(st, 3, id) ||
        db_step(st) != DB_DONE)
        goto failed;
    db_finalize(st);
    st = NULL;
    if (im_audit_commit(db, actor, "job_templates.update", "job_template", id,
                        json_object_to_json_string(before), json_object_to_json_string(f), r))
        io_ok(r, id, revision + 1);
    json_object_put(before);
    json_object_put(definition);
    return;
failed:
    db_finalize(st);
    json_object_put(before);
    json_object_put(definition);
    io_failure(db, r);
}

int improvements_operations_unresolved(Db *db, DbInt actor, DbInt job, DbInt *count) {
    char scope[5000], sql[7000];
    DbStmt *st = NULL;
    if (!io_scope(db, actor, scope))
        return -1;
    (void)snprintf(
        sql, sizeof(sql),
        "SELECT (SELECT COUNT(*) FROM job_tasks WHERE job_id=? AND status<>'done')+(SELECT "
        "COUNT(*) FROM job_blockers WHERE job_id=? AND status<>'resolved')+(SELECT COUNT(*) FROM "
        "time_corrections WHERE (original_job_id=? OR job_id=?) AND status='pending' AND "
        "requester_id IN (%s))+(SELECT COUNT(*) FROM operations_checklist WHERE job_id=? AND "
        "status='open')",
        scope);
    if (!io_prepare(db, &st, sql) || !io_id(st, 1, job) || !io_id(st, 2, job) ||
        !io_id(st, 3, job) || !io_id(st, 4, job) || !io_id(st, 5, job) || db_step(st) != DB_ROW) {
        db_finalize(st);
        return -1;
    }
    *count = db_column_int64(st, 0);
    db_finalize(st);
    return 1;
}

static void io_checklist(Db *db, const char *method, struct json_object *f, DbInt actor,
                         WorkspaceResponse *r) {
    const char *const allowed[] = {"job_id", "kind",     "resource_id", "label",
                                   "status", "revision", NULL};
    DbInt job = 0, resource = 0, revision = 0;
    if (!io_known(f, allowed) || !im_integer(f, "job_id", 1, INT64_MAX, &job)) {
        im_error(r, 400, "Seleziona una commessa valida.");
        return;
    }
    if (!io_permission(access_policy_can_manage_job(db, actor, job), r))
        return;
    DbStmt *st = NULL;
    char scope[5000], sql[7000];
    if (!io_scope(db, actor, scope)) {
        im_error(r, 503, "Servizio temporaneamente non disponibile.");
        return;
    }
    if (!strcmp(method, "GET")) {
        (void)snprintf(
            sql, sizeof(sql),
            "SELECT (SELECT COUNT(*) FROM job_tasks WHERE job_id=? AND status<>'done'),(SELECT "
            "COUNT(*) FROM job_blockers WHERE job_id=? AND status<>'resolved'),(SELECT COUNT(*) "
            "FROM time_corrections WHERE (original_job_id=? OR job_id=?) AND status='pending' AND "
            "requester_id IN (%s))",
            scope);
        if (!io_prepare(db, &st, sql) || !io_id(st, 1, job) || !io_id(st, 2, job) ||
            !io_id(st, 3, job) || !io_id(st, 4, job) || db_step(st) != DB_ROW)
            goto failed;
        struct json_object *out = json_object_new_object();
        json_object_object_add(out, "job_id", json_object_new_int64(job));
        json_object_object_add(out, "open_tasks", json_object_new_int64(db_column_int64(st, 0)));
        json_object_object_add(out, "open_blockers", json_object_new_int64(db_column_int64(st, 1)));
        json_object_object_add(out, "pending_corrections",
                               json_object_new_int64(db_column_int64(st, 2)));
        DbInt pending = db_column_int64(st, 0) + db_column_int64(st, 1) + db_column_int64(st, 2);
        db_finalize(st);
        st = NULL;
        if (!io_prepare(db, &st,
                        "SELECT id,kind,resource_id,label,status,revision FROM "
                        "operations_checklist WHERE job_id=? ORDER BY id") ||
            !io_id(st, 1, job)) {
            json_object_put(out);
            goto failed;
        }
        struct json_object *items = json_object_new_array();
        const char *const keys[] = {"id", "kind", "resource_id", "label", "status", "revision"};
        int step;
        while ((step = db_step(st)) == DB_ROW) {
            if (!strcmp(io_column(st, 4), "open"))
                pending++;
            json_object_array_add(items, io_row(st, keys, 6));
        }
        db_finalize(st);
        st = NULL;
        if (step != DB_DONE) {
            json_object_put(items);
            json_object_put(out);
            goto failed;
        }
        json_object_object_add(out, "linked_resources", items);
        json_object_object_add(out, "unresolved_count", json_object_new_int64(pending));
        json_object_object_add(out, "ready", json_object_new_boolean(pending == 0));
        im_reply(r, 200, out);
        return;
    }
    const char *kind = im_string(f, "kind"), *status = im_string(f, "status"),
               *label = im_string(f, "label");
    if ((strcmp(kind, "ticket") && strcmp(kind, "asset")) ||
        (strcmp(status, "open") && strcmp(status, "done")) || !io_length(label, 1, 160) ||
        !im_integer(f, "resource_id", 1, INT64_MAX, &resource) ||
        !im_integer(f, "revision", 0, INT64_MAX - 1, &revision)) {
        im_error(r, 400, "Indica tipo, risorsa, etichetta, stato e revisione.");
        return;
    }
    if (!im_begin(db, r))
        return;
    if (!io_permission(access_policy_can_manage_job(db, actor, job), r)) {
        im_rollback(db);
        return;
    }
    if (!io_prepare(db, &st,
                    "SELECT id,revision,label,status FROM operations_checklist WHERE job_id=? AND "
                    "kind=? AND resource_id=?") ||
        !io_id(st, 1, job) || !io_text(st, 2, kind) || !io_id(st, 3, resource))
        goto failed;
    int found = db_step(st);
    DbInt id = found == DB_ROW ? db_column_int64(st, 0) : 0,
          current = found == DB_ROW ? db_column_int64(st, 1) : 0;
    const char *const oldkeys[] = {"id", "revision", "label", "status"};
    struct json_object *old = found == DB_ROW ? io_row(st, oldkeys, 4) : NULL;
    db_finalize(st);
    st = NULL;
    if (found != DB_ROW && found != DB_DONE) {
        json_object_put(old);
        goto failed;
    }
    if (current != revision) {
        json_object_put(old);
        im_rollback(db);
        im_error(r, 409, "La checklist è cambiata. Ricarica prima di salvare.");
        return;
    }
    const char *write =
        id ? "UPDATE operations_checklist SET label=?,status=?,revision=revision+1 WHERE job_id=? "
             "AND kind=? AND resource_id=?"
           : "INSERT INTO operations_checklist(label,status,job_id,kind,resource_id,created_by) "
             "VALUES(?,?,?,?,?,?)";
    if (!io_prepare(db, &st, write) || !io_text(st, 1, label) || !io_text(st, 2, status) ||
        !io_id(st, 3, job) || !io_text(st, 4, kind) || !io_id(st, 5, resource) ||
        (!id && !io_id(st, 6, actor)) || db_step(st) != DB_DONE) {
        json_object_put(old);
        goto failed;
    }
    if (!id)
        id = db_last_insert_rowid(db);
    db_finalize(st);
    st = NULL;
    bool okay = im_audit_commit(db, actor, "jobs.checklist", "job", job,
                                old ? json_object_to_json_string(old) : NULL,
                                json_object_to_json_string(f), r);
    json_object_put(old);
    if (okay)
        io_ok(r, id, revision + 1);
    return;
failed:
    db_finalize(st);
    io_failure(db, r);
}

bool improvements_operations_handle(Db *db, const char *method, const char *target,
                                    const char *query, const char *body, DbInt actor,
                                    WorkspaceResponse *r) {
    const char *prefix = "/api/improvements/operations/";
    size_t n = strlen(prefix);
    if (strncmp(target, prefix, n))
        return false;
    AccessPolicy live;
    int active = access_policy_resolve(db, actor, NULL, &live);
    if (active != 1) {
        im_error(r, active < 0 ? 503 : 403, "Account non disponibile.");
        return true;
    }
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (!get && !post) {
        im_error(r, 405, "Metodo non consentito.");
        return true;
    }
    if (post && query && *query) {
        im_error(r, 400, "Parametri della richiesta non validi.");
        return true;
    }
    struct json_object *f = im_fields(get ? query : body);
    if (!f) {
        im_error(r, 400, "Parametri della richiesta non validi.");
        return true;
    }
    const char *route = target + n;
    if (!strcmp(route, "capacity") && get)
        io_capacity(db, f, actor, r);
    else if (!strcmp(route, "task-plan"))
        io_task_plan(db, method, f, actor, r);
    else if (!strcmp(route, "absences")) {
        if (get)
            io_absence_list(db, f, actor, r);
        else
            io_absence_create(db, f, actor, r);
    } else if (!strcmp(route, "absence-decision") && post)
        io_absence_decide(db, f, actor, r);
    else if (!strcmp(route, "templates"))
        io_templates(db, method, f, actor, r);
    else if (!strcmp(route, "checklist"))
        io_checklist(db, method, f, actor, r);
    else
        im_error(r, 404, "Percorso non trovato.");
    json_object_put(f);
    return true;
}
