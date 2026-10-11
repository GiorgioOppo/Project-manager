/* Included by workspace.c: correction requests share its transaction/JSON helpers. */
#include "access_control.h"
typedef struct {
    DbInt id, job_id, user_id, minutes, revision;
    char *date, *note;
    Json snapshot;
} CorrectionEntry;

typedef struct {
    DbInt id, entry_id, original_entry_id, requester_id, revision;
    DbInt original_job_id, original_minutes, job_id, minutes;
    bool delete_requested;
    char *original_date, *original_note, *before_json;
    char *date, *note, *reason, *status;
} CorrectionRequest;

static void correction_entry_free(CorrectionEntry *entry) {
    free(entry->date); free(entry->note); free(entry->snapshot.data);
    memset(entry, 0, sizeof(*entry));
}

static void correction_request_free(CorrectionRequest *request) {
    free(request->original_date); free(request->original_note); free(request->before_json);
    free(request->date); free(request->note); free(request->reason); free(request->status);
    memset(request, 0, sizeof(*request));
}

static int correction_entry_load(DbInt id, CorrectionEntry *entry) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,job_id,user_id,date,minutes,note,revision FROM time_entries WHERE id=?") ||
        !bind_id(statement, 1, id)) { db_finalize(statement); return -1; }
    int result = db_step(statement);
    if (result == DB_ROW) {
        entry->id = db_column_int64(statement, 0);
        entry->job_id = db_column_int64(statement, 1);
        entry->user_id = db_column_int64(statement, 2);
        entry->date = duplicate(column_text(statement, 3));
        entry->minutes = db_column_int64(statement, 4);
        entry->note = duplicate(column_text(statement, 5));
        entry->revision = db_column_int64(statement, 6);
    }
    db_finalize(statement);
    if (result == DB_ROW && (!entry->date || !entry->note ||
        entry_snapshot(id, entry->user_id, &entry->snapshot) != 1)) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

static int correction_request_load(DbInt id, CorrectionRequest *request) {
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT id,entry_id,original_entry_id,requester_id,entry_revision,"
        "original_job_id,original_date,original_minutes,original_note,before_json,job_id,date,minutes,"
        "note,reason,delete_requested,status FROM time_corrections WHERE id=?") ||
        !bind_id(statement, 1, id)) { db_finalize(statement); return -1; }
    int result = db_step(statement);
    if (result == DB_ROW) {
        request->id = db_column_int64(statement, 0);
        request->entry_id = db_column_int64(statement, 1);
        request->original_entry_id = db_column_int64(statement, 2);
        request->requester_id = db_column_int64(statement, 3);
        request->revision = db_column_int64(statement, 4);
        request->original_job_id = db_column_int64(statement, 5);
        request->original_date = duplicate(column_text(statement, 6));
        request->original_minutes = db_column_int64(statement, 7);
        request->original_note = duplicate(column_text(statement, 8));
        request->before_json = duplicate(column_text(statement, 9));
        request->job_id = db_column_int64(statement, 10);
        request->date = duplicate(column_text(statement, 11));
        request->minutes = db_column_int64(statement, 12);
        request->note = duplicate(column_text(statement, 13));
        request->reason = duplicate(column_text(statement, 14));
        request->delete_requested = db_column_int(statement, 15) == 1;
        request->status = duplicate(column_text(statement, 16));
    }
    db_finalize(statement);
    if (result == DB_ROW && (!request->original_date || !request->original_note || !request->before_json ||
        !request->date || !request->note || !request->reason || !request->status)) return -1;
    return result == DB_ROW ? 1 : result == DB_DONE ? 0 : -1;
}

/* Access is always checked for the owner, never the approving manager. A real
   reduction/removal on the original day remains possible after access closes. */
static int correction_change_allowed(const CorrectionRequest *request) {
    if (request->job_id == request->original_job_id &&
        strcmp(request->date, request->original_date) == 0 &&
        (request->delete_requested || request->minutes < request->original_minutes)) return 1;
    if (request->delete_requested) return 0;
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT j.time_closed,j.archived,u.role,u.active FROM jobs j "
        "JOIN users u ON u.id=? WHERE j.id=?") || !bind_id(statement, 1, request->requester_id) ||
        !bind_id(statement, 2, request->job_id)) { db_finalize(statement); return -1; }
    int result = db_step(statement);
    bool open = result == DB_ROW && db_column_int(statement, 0) == 0 &&
                db_column_int(statement, 1) == 0 && db_column_int(statement, 3) == 1;
    bool manager = result == DB_ROW && strcmp(column_text(statement, 2), "manager") == 0;
    db_finalize(statement);
    if (result != DB_ROW && result != DB_DONE) return -1;
    if (!open) return 0;
    if (manager) return 1;
    char today[11];
    if (!today_date(today)) return -1;
    int permission = organization_job_access(database, request->job_id, request->requester_id, today, request->date);
    return permission < 0 ? -1 : (permission & 2) != 0;
}

static void correction_result(WorkspaceResponse *response, int code, DbInt id, const char *status) {
    Json json = {0};
    append(&json, "{\"ok\":true,\"id\":"); append_integer(&json, id);
    append(&json, ",\"status\":"); append_string(&json, status); append(&json, "}");
    finish_json(response, code, &json);
}

static void correction_audit_json(Json *json, const CorrectionRequest *request, const char *status,
                                  DbInt decided_by, const char *decision_reason) {
    append(json, "{\"id\":"); append_integer(json, request->id);
    append(json, ",\"original_entry_id\":"); append_integer(json, request->original_entry_id);
    append(json, ",\"requester_id\":"); append_integer(json, request->requester_id);
    append(json, ",\"revision\":"); append_integer(json, request->revision);
    append(json, ",\"job_id\":"); append_integer(json, request->job_id);
    append(json, ",\"date\":"); append_string(json, request->date);
    append(json, ",\"minutes\":"); append_integer(json, request->minutes);
    append(json, ",\"note\":"); append_string(json, request->note);
    append(json, ",\"reason\":"); append_string(json, request->reason);
    append(json, ",\"delete_requested\":"); append(json, request->delete_requested ? "true" : "false");
    append(json, ",\"status\":"); append_string(json, status);
    append(json, ",\"decided_by\":"); if (decided_by) append_integer(json, decided_by); else append(json, "null");
    append(json, ",\"decision_reason\":"); append_string(json, decision_reason);
    append(json, ",\"before\":"); append(json, request->before_json); append(json, "}");
}

static void correction_list(const char *query, DbInt actor_id, bool manager, WorkspaceResponse *response) {
    Field fields[] = {{"status", NULL}, {"pending", NULL}};
    if (!parse_fields(query, fields, 2, response)) return;
    const char *status = field_value(&fields[0]), *pending = field_value(&fields[1]);
    if ((*status && strcmp(status, "pending") != 0) || (*pending && strcmp(pending, "1") != 0)) {
        free_fields(fields, 2); error(response, 400, "Filtro rettifiche non valido."); return;
    }
    bool only_pending = *status || *pending;
    DbStmt *statement = NULL;
    Json json = {0};
    if (!prepare(&statement, "SELECT c.id,c.entry_id,c.original_entry_id,c.requester_id,u.name,u.email,"
        "c.entry_revision,c.job_id,j.code,j.name,c.date,c.minutes,c.note,c.reason,c.delete_requested,"
        "c.status,c.decision_reason,c.decided_by,c.created_at,c.decided_at,c.before_json "
        "FROM time_corrections c JOIN users u ON u.id=c.requester_id LEFT JOIN jobs j ON j.id=c.job_id "
        "WHERE (?=1 OR c.requester_id=?) AND (?=0 OR c.status='pending') ORDER BY c.id DESC") ||
        !bind_id(statement, 1, manager ? 1 : 0) || !bind_id(statement, 2, actor_id) ||
        !bind_id(statement, 3, only_pending ? 1 : 0)) goto database_failure;
    append(&json, "{\"ok\":true,\"scope\":"); append_string(&json, manager ? "team" : "personal");
    append(&json, ",\"requests\":[");
    int result, rows = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        int visible = access_policy_can_view_time(database, actor_id, db_column_int64(statement, 3), manager);
        if (visible < 0) goto database_failure;
        if (!visible) continue;
        if (++rows > RESULT_LIMIT) {
            db_finalize(statement); free(json.data); free_fields(fields, 2);
            error(response, 409, "Oltre 1000 rettifiche: usa il filtro delle richieste in attesa."); return;
        }
        if (rows > 1) append(&json, ",");
        append(&json, "{\"id\":"); append_integer(&json, db_column_int64(statement, 0));
        append(&json, ",\"entry_id\":");
        if (db_column_type(statement, 1) == DB_NULL) append(&json, "null"); else append_integer(&json, db_column_int64(statement, 1));
        append(&json, ",\"original_entry_id\":"); append_integer(&json, db_column_int64(statement, 2));
        append(&json, ",\"requester_id\":"); append_integer(&json, db_column_int64(statement, 3));
        append(&json, ",\"requester_name\":"); append_string(&json, column_text(statement, 4));
        append(&json, ",\"requester_email\":"); append_string(&json, column_text(statement, 5));
        append(&json, ",\"revision\":"); append_integer(&json, db_column_int64(statement, 6));
        append(&json, ",\"job_id\":"); append_integer(&json, db_column_int64(statement, 7));
        append(&json, ",\"job_code\":"); append_string(&json, column_text(statement, 8));
        append(&json, ",\"job_name\":"); append_string(&json, column_text(statement, 9));
        append(&json, ",\"date\":"); append_string(&json, column_text(statement, 10));
        append(&json, ",\"minutes\":"); append_integer(&json, db_column_int64(statement, 11));
        append(&json, ",\"note\":"); append_string(&json, column_text(statement, 12));
        append(&json, ",\"reason\":"); append_string(&json, column_text(statement, 13));
        append(&json, ",\"delete_requested\":"); append(&json, db_column_int(statement, 14) ? "true" : "false");
        append(&json, ",\"status\":"); append_string(&json, column_text(statement, 15));
        append(&json, ",\"decision_reason\":"); append_string(&json, column_text(statement, 16));
        append(&json, ",\"decided_by\":");
        if (db_column_type(statement, 17) == DB_NULL) append(&json, "null"); else append_integer(&json, db_column_int64(statement, 17));
        append(&json, ",\"created_at\":"); append_string(&json, column_text(statement, 18));
        append(&json, ",\"decided_at\":");
        if (db_column_type(statement, 19) == DB_NULL) append(&json, "null"); else append_string(&json, column_text(statement, 19));
        append(&json, ",\"before\":"); append(&json, column_text(statement, 20));
        bool active = strcmp(column_text(statement, 15), "pending") == 0;
        bool author = db_column_int64(statement, 3) == actor_id;
        append(&json, ",\"can_decide\":"); append(&json, active && manager && !author ? "true" : "false");
        append(&json, ",\"can_cancel\":"); append(&json, active && author ? "true" : "false"); append(&json, "}");
    }
    if (result != DB_DONE) goto database_failure;
    db_finalize(statement); append(&json, "]}"); free_fields(fields, 2); finish_json(response, 200, &json); return;
database_failure:
    db_finalize(statement); free(json.data); free_fields(fields, 2); server_error(response);
}

static void correction_create(const char *body, DbInt actor_id, WorkspaceResponse *response) {
    Field fields[] = {{"entry_id", NULL}, {"revision", NULL}, {"job_id", NULL}, {"date", NULL},
        {"minutes", NULL}, {"note", NULL}, {"reason", NULL}, {"delete_requested", NULL}};
    if (!parse_fields(body, fields, 8, response)) return;
    trim(fields[6].value);
    CorrectionRequest request = {0};
    const char *delete_text = field_value(&fields[7]);
    if (!parse_positive(field_value(&fields[0]), &request.original_entry_id) ||
        !parse_positive(field_value(&fields[1]), &request.revision) ||
        !parse_positive(field_value(&fields[2]), &request.job_id) || !valid_date(field_value(&fields[3])) ||
        !parse_positive(field_value(&fields[4]), &request.minutes) || request.minutes > 1440 ||
        !valid_text(field_value(&fields[5]), 1000, false, true) ||
        !valid_text(field_value(&fields[6]), 1000, true, true) ||
        (*delete_text && strcmp(delete_text, "0") != 0 && strcmp(delete_text, "1") != 0)) {
        free_fields(fields, 8); error(response, 400, "Indica registrazione, revisione, commessa, data, durata e motivo della rettifica."); return;
    }
    request.requester_id = actor_id;
    request.entry_id = request.original_entry_id;
    request.date = duplicate(field_value(&fields[3])); request.note = duplicate(field_value(&fields[5]));
    request.reason = duplicate(field_value(&fields[6])); request.delete_requested = strcmp(delete_text, "1") == 0;
    free_fields(fields, 8);
    if (!request.date || !request.note || !request.reason) { correction_request_free(&request); server_error(response); return; }
    if (!begin_transaction(response)) { correction_request_free(&request); return; }
    CorrectionEntry entry = {0}; DbStmt *statement = NULL; Json audit = {0};
    int found = correction_entry_load(request.entry_id, &entry);
    if (found < 0) goto database_failure;
    if (!found || entry.user_id != actor_id) {
        rollback(); error(response, 404, "Registrazione personale non trovata."); goto cleanup;
    }
    if (entry.revision != request.revision) {
        rollback(); error(response, 409, "La registrazione è cambiata. Ricarica il registro prima di richiedere una rettifica."); goto cleanup;
    }
    request.original_job_id = entry.job_id; request.original_minutes = entry.minutes;
    request.original_date = duplicate(entry.date); request.original_note = duplicate(entry.note);
    request.before_json = duplicate(entry.snapshot.data);
    if (!request.original_date || !request.original_note || !request.before_json) goto database_failure;
    if (!prepare(&statement, "SELECT id FROM time_corrections WHERE entry_id=? AND status='pending'") ||
        !bind_id(statement, 1, request.entry_id)) goto database_failure;
    int result = db_step(statement); db_finalize(statement); statement = NULL;
    if (result == DB_ROW) { rollback(); error(response, 409, "Esiste già una rettifica in attesa per questa registrazione."); goto cleanup; }
    if (result != DB_DONE) goto database_failure;
    int allowed = correction_change_allowed(&request);
    if (allowed < 0) goto database_failure;
    if (!allowed) {
        rollback(); error(response, 403, "La destinazione non è abilitata o è chiusa. Puoi ancora ridurre o eliminare le ore sulla commessa e data originali."); goto cleanup;
    }
    if (!prepare(&statement, "INSERT INTO time_corrections(entry_id,original_entry_id,requester_id,entry_revision,"
        "original_job_id,original_date,original_minutes,original_note,before_json,job_id,date,minutes,note,reason,delete_requested) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)") || !bind_id(statement, 1, request.entry_id) ||
        !bind_id(statement, 2, request.original_entry_id) || !bind_id(statement, 3, actor_id) ||
        !bind_id(statement, 4, request.revision) || !bind_id(statement, 5, request.original_job_id) ||
        !bind_text(statement, 6, request.original_date) || !bind_id(statement, 7, request.original_minutes) ||
        !bind_text(statement, 8, request.original_note) || !bind_text(statement, 9, request.before_json) ||
        !bind_id(statement, 10, request.job_id) || !bind_text(statement, 11, request.date) ||
        !bind_id(statement, 12, request.minutes) || !bind_text(statement, 13, request.note) ||
        !bind_text(statement, 14, request.reason) || !bind_id(statement, 15, request.delete_requested ? 1 : 0) ||
        db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL; request.id = db_last_insert_rowid(database);
    correction_audit_json(&audit, &request, "pending", 0, "");
    if (audit.failed || !storage_audit(database, actor_id, "time_corrections.create", "time_correction", request.id, NULL, audit.data)) goto database_failure;
    if (commit_transaction(response)) correction_result(response, 201, request.id, "pending");
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(audit.data); correction_entry_free(&entry); correction_request_free(&request);
}

static void correction_decide(const char *body, DbInt actor_id, bool manager, DbInt id,
                               WorkspaceResponse *response) {
    Field fields[] = {{"decision", NULL}, {"reason", NULL}};
    if (!parse_fields(body, fields, 2, response)) return;
    trim(fields[1].value);
    const char *decision = field_value(&fields[0]), *reason = field_value(&fields[1]);
    bool approve = strcmp(decision, "approved") == 0, cancel = strcmp(decision, "cancelled") == 0;
    if ((!approve && !cancel && strcmp(decision, "rejected") != 0) ||
        !valid_text(reason, 1000, strcmp(decision, "rejected") == 0, true)) {
        free_fields(fields, 2); error(response, 400, "Scegli una decisione valida; per il rifiuto indica un motivo."); return;
    }
    if (!begin_transaction(response)) { free_fields(fields, 2); return; }
    CorrectionRequest request = {0}; CorrectionEntry entry = {0};
    DbStmt *statement = NULL; Json before = {0}, after = {0}, entry_after = {0};
    AccessPolicy policy = {0};
    int resolved = access_policy_resolve(database, actor_id, NULL, &policy);
    if (resolved < 0) goto database_failure;
    if (!resolved) { rollback(); error(response, 403, "Il tuo ruolo non consente questa operazione."); goto cleanup; }
    manager = policy.manager;
    int found = correction_request_load(id, &request);
    if (found < 0) goto database_failure;
    int visible = found ? access_policy_can_view_time(database, actor_id, request.requester_id, manager) : 0;
    if (visible < 0) goto database_failure;
    if (!found || !visible || (!manager && request.requester_id != actor_id)) {
        rollback(); error(response, 404, "Rettifica non trovata."); goto cleanup;
    }
    if ((cancel && request.requester_id != actor_id) || (!cancel && (!manager || request.requester_id == actor_id))) {
        rollback(); error(response, 403, "Solo un altro gestore può approvare o rifiutare; soltanto l’autore può annullare."); goto cleanup;
    }
    if (strcmp(request.status, "pending") != 0) {
        rollback(); error(response, 409, "Questa rettifica ha già una decisione. Ricarica l’elenco."); goto cleanup;
    }
    const char *final_status = decision;
    bool conflict = false;
    if (approve) {
        found = request.entry_id ? correction_entry_load(request.entry_id, &entry) : 0;
        if (found < 0) goto database_failure;
        conflict = !found || entry.user_id != request.requester_id || entry.revision != request.revision ||
            entry.job_id != request.original_job_id || strcmp(entry.date, request.original_date) != 0 ||
            entry.minutes != request.original_minutes || strcmp(entry.note, request.original_note) != 0;
        if (conflict) final_status = "conflict";
        else {
            int allowed = correction_change_allowed(&request);
            if (allowed < 0) goto database_failure;
            if (!allowed) {
                rollback(); error(response, 403, "I permessi o la chiusura della commessa sono cambiati: aumento e spostamento non sono consentiti."); goto cleanup;
            }
            if (!request.delete_requested) {
                if (entry.revision == INT64_MAX) {
                    rollback(); error(response, 409, "Limite revisioni raggiunto per questa registrazione."); goto cleanup;
                }
                if (!prepare(&statement, "SELECT COALESCE(SUM(minutes),0),COUNT(*) FROM time_entries WHERE user_id=? AND date=? AND id<>?") ||
                    !bind_id(statement, 1, request.requester_id) || !bind_text(statement, 2, request.date) ||
                    !bind_id(statement, 3, request.entry_id) || db_step(statement) != DB_ROW) goto database_failure;
                DbInt total = db_column_int64(statement, 0), count = db_column_int64(statement, 1);
                db_finalize(statement); statement = NULL;
                if (total > 1440 - request.minutes || count >= RESULT_LIMIT) {
                    rollback(); error(response, 409, total > 1440 - request.minutes ?
                        "La rettifica supererebbe le 24 ore nella giornata." : "La giornata contiene già 1000 registrazioni."); goto cleanup;
                }
            }
            if (!prepare(&statement, request.delete_requested ?
                "DELETE FROM time_entries WHERE id=? AND user_id=? AND revision=?" :
                "UPDATE time_entries SET task_id=CASE WHEN job_id=? THEN task_id ELSE NULL END,job_id=?,date=?,minutes=?,note=?,revision=revision+1 WHERE id=? AND user_id=? AND revision=?")) goto database_failure;
            int offset = 0;
            if (!request.delete_requested) {
                if (!bind_id(statement, 1, request.job_id) || !bind_id(statement, 2, request.job_id) ||
                    !bind_text(statement, 3, request.date) || !bind_id(statement, 4, request.minutes) ||
                    !bind_text(statement, 5, request.note)) goto database_failure;
                offset = 5;
            }
            if (!bind_id(statement, offset + 1, request.entry_id) || !bind_id(statement, offset + 2, request.requester_id) ||
                !bind_id(statement, offset + 3, request.revision) || db_step(statement) != DB_DONE) goto database_failure;
            db_finalize(statement); statement = NULL;
            if (db_changes(database) != 1) goto database_failure;
            if ((!request.delete_requested && entry_snapshot(request.entry_id, request.requester_id, &entry_after) != 1) ||
                !storage_audit(database, actor_id, request.delete_requested ? "time_entries.delete" : "time_entries.update",
                    "time_entry", request.original_entry_id, entry.snapshot.data, request.delete_requested ? NULL : entry_after.data)) goto database_failure;
        }
    }
    correction_audit_json(&before, &request, "pending", 0, "");
    const char *final_reason = conflict ? "La registrazione originale è stata modificata o eliminata." : reason;
    time_t now = time(NULL); struct tm utc; char stamp[21];
    if (now == (time_t)-1 || !gmtime_r(&now, &utc) || strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc) != 20) goto database_failure;
    if (!prepare(&statement, "UPDATE time_corrections SET status=?,decision_reason=?,decided_by=?,decided_at=? WHERE id=? AND status='pending'") ||
        !bind_text(statement, 1, final_status) || !bind_text(statement, 2, final_reason) ||
        !bind_id(statement, 3, actor_id) || !bind_text(statement, 4, stamp) || !bind_id(statement, 5, id) || db_step(statement) != DB_DONE) goto database_failure;
    db_finalize(statement); statement = NULL;
    if (db_changes(database) != 1) goto database_failure;
    correction_audit_json(&after, &request, final_status, actor_id, final_reason);
    if (before.failed || after.failed || !storage_audit(database, actor_id, "time_corrections.decide", "time_correction", id, before.data, after.data)) goto database_failure;
    if (commit_transaction(response)) {
        if (conflict) error(response, 409, "La registrazione originale è cambiata o è stata eliminata. La rettifica è conservata come conflitto.");
        else correction_result(response, 200, id, final_status);
    }
    goto cleanup;
database_failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(before.data); free(after.data); free(entry_after.data);
    correction_entry_free(&entry); correction_request_free(&request); free_fields(fields, 2);
}

static bool time_corrections_handle(const char *method, const char *target, const char *query,
                                    const char *body, DbInt actor_id, const char *owner,
                                    bool manager, WorkspaceResponse *response) {
    const char *base = "/api/time-corrections";
    size_t length = strlen(base);
    bool collection = strcmp(target, base) == 0;
    bool item = strncmp(target, base, length) == 0 && target[length] == '/';
    if (!collection && !item) return false;
    response->status = 0; response->body = NULL;
    if (!database) { server_error(response); return true; }
    if (actor_id <= 0 || !owner || !*owner) { error(response, 401, "Accedi per continuare."); return true; }
    AccessPolicy policy = {0};
    int resolved = access_policy_resolve(database, actor_id, NULL, &policy);
    if (resolved < 0) { server_error(response); return true; }
    if (!resolved) { error(response, 403, "Il tuo ruolo non consente questa operazione."); return true; }
    manager = policy.manager;
    if (!query) query = "";
    if (!body) body = "";
    if (collection && strcmp(method, "GET") == 0) { correction_list(query, actor_id, manager, response); return true; }
    if (*query) { error(response, 400, "Parametri della richiesta non validi."); return true; }
    if (collection && strcmp(method, "POST") == 0) { correction_create(body, actor_id, response); return true; }
    if (item) {
        const char *path = target + length + 1, *suffix = strchr(path, '/');
        DbInt id; char text[20];
        size_t id_length = suffix ? (size_t)(suffix - path) : 0;
        if (!id_length || id_length >= sizeof(text) || strcmp(suffix, "/decision") != 0) {
            error(response, 400, "Identificativo rettifica non valido."); return true;
        }
        memcpy(text, path, id_length); text[id_length] = '\0';
        if (!parse_positive(text, &id)) { error(response, 400, "Identificativo rettifica non valido."); return true; }
        if (strcmp(method, "POST") == 0) correction_decide(body, actor_id, manager, id, response);
        else error(response, 405, "Metodo non consentito.");
    } else error(response, 405, "Metodo non consentito.");
    return true;
}
