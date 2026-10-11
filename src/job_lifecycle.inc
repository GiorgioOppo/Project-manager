/* Included by workspace.c after its shared form, transaction and JSON helpers. */

static int job_lifecycle_snapshot(DbInt job_id, Json *json, bool successful,
                                  const char *reason, const char *scope) {
    DbStmt *statement = NULL;
    char predicate[5100], sql[17000];
    int length = scope ? snprintf(predicate, sizeof(predicate), "e.user_id IN (%s)", scope) :
        snprintf(predicate, sizeof(predicate), "1=1");
    if (length < 0 || (size_t)length >= sizeof(predicate)) return -1;
    length = snprintf(sql, sizeof(sql),
        "SELECT j.id,j.code,j.name,j.status,j.time_closed,j.archived,j.lifecycle_revision,"
        "(SELECT COUNT(*) FROM job_tasks t WHERE t.job_id=j.id AND t.status<>'done'),"
        "(SELECT COUNT(*) FROM job_blockers b WHERE b.job_id=j.id AND b.status<>'resolved'),"
        "(SELECT COUNT(*) FROM time_entries e WHERE e.job_id=j.id AND %s),"
        "(SELECT COALESCE(SUM(e.minutes),0) FROM time_entries e WHERE e.job_id=j.id AND %s),"
        "(SELECT COUNT(DISTINCT e.user_id) FROM time_entries e WHERE e.job_id=j.id AND %s) "
        "FROM jobs j WHERE j.id=?", predicate, predicate, predicate);
    if (length < 0 || (size_t)length >= sizeof(sql) ||
        !prepare(&statement, sql) || !bind_id(statement, 1, job_id)) {
        db_finalize(statement); return -1;
    }
    int result = db_step(statement);
    if (result != DB_ROW) {
        db_finalize(statement); return result == DB_DONE ? 0 : -1;
    }
    append(json, successful ? "{\"ok\":true,\"job_id\":" : "{\"job_id\":");
    append_integer(json, db_column_int64(statement, 0));
    append(json, ",\"code\":"); append_string(json, column_text(statement, 1));
    append(json, ",\"name\":"); append_string(json, column_text(statement, 2));
    append(json, ",\"status\":"); append_string(json, column_text(statement, 3));
    append(json, ",\"time_closed\":"); append(json, db_column_int(statement, 4) ? "true" : "false");
    append(json, ",\"archived\":"); append(json, db_column_int(statement, 5) ? "true" : "false");
    static const char *const names[] = {
        ",\"lifecycle_revision\":", ",\"open_tasks\":", ",\"open_blockers\":",
        ",\"time_entries\":", ",\"total_minutes\":", ",\"users_with_hours\":"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        append(json, names[i]); append_integer(json, db_column_int64(statement, (int)i + 6));
    }
    if (reason) { append(json, ",\"reason\":"); append_string(json, reason); }
    append(json, "}");
    db_finalize(statement);
    return json->failed ? -1 : 1;
}

static void job_lifecycle_read(DbInt job_id, DbInt actor_id, WorkspaceResponse *response) {
    Json json = {0};
    char scope[5000];
    if (!workspace_time_scope(actor_id, true, scope)) { server_error(response); return; }
    int found = job_lifecycle_snapshot(job_id, &json, false, NULL, scope);
    if (found == 1) finish_json(response, 200, &json);
    else {
        free(json.data);
        if (!found) error(response, 404, "Commessa non trovata.");
        else server_error(response);
    }
}

static void job_lifecycle_write(DbInt job_id, const char *body, DbInt actor_id,
                                WorkspaceResponse *response) {
    Field fields[] = {{"revision", NULL}, {"action", NULL}, {"reason", NULL}};
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (!parse_fields(body, fields, count, response)) return;
    trim(fields[2].value);
    DbInt revision = 0;
    const char *action = field_value(&fields[1]);
    const char *reason = field_value(&fields[2]);
    bool close = strcmp(action, "close") == 0;
    bool archive = strcmp(action, "archive") == 0;
    bool reopen = strcmp(action, "reopen") == 0;
    if (!parse_positive(field_value(&fields[0]), &revision) || revision == INT64_MAX ||
        (!close && !archive && !reopen) || !valid_text(reason, 1000, true, true)) {
        free_fields(fields, count);
        error(response, 400, "Indica un'azione valida, la revisione corrente e un motivo (massimo 1000 caratteri).");
        return;
    }
    if (!begin_transaction(response)) { free_fields(fields, count); return; }
    DbStmt *statement = NULL;
    Json before = {0}, after = {0};
    char scope[5000];
    AccessPolicy policy;
    if (!workspace_policy(actor_id, &policy) || !workspace_time_scope(actor_id, true, scope)) goto failure;
    if (!policy.manager) {
        rollback(); free_fields(fields, count);
        error(response, 403, "La chiusura delle commesse è riservata ai gestori."); return;
    }
    if (!prepare(&statement, "SELECT lifecycle_revision,archived FROM jobs WHERE id=?") ||
        !bind_id(statement, 1, job_id)) goto failure;
    int result = db_step(statement);
    if (result == DB_DONE) {
        db_finalize(statement); rollback(); free_fields(fields, count);
        error(response, 404, "Commessa non trovata."); return;
    }
    if (result != DB_ROW) goto failure;
    bool stale = db_column_int64(statement, 0) != revision;
    bool archived = db_column_int(statement, 1) != 0;
    db_finalize(statement); statement = NULL;
    if (stale || (close && archived)) {
        rollback(); free_fields(fields, count);
        error(response, 409, stale ? "La commessa è stata aggiornata. Ricarica il riepilogo prima di riprovare." :
              "La commessa è archiviata. Riaprila prima di cambiare la chiusura delle ore."); return;
    }
    if (job_lifecycle_snapshot(job_id, &before, false, NULL, NULL) != 1 ||
        !prepare(&statement,
            "UPDATE jobs SET time_closed=?,archived=?,lifecycle_revision=lifecycle_revision+1 "
            "WHERE id=? AND lifecycle_revision=?") ||
        !bind_id(statement, 1, reopen ? 0 : 1) || !bind_id(statement, 2, archive ? 1 : 0) ||
        !bind_id(statement, 3, job_id) || !bind_id(statement, 4, revision) ||
        db_step(statement) != DB_DONE) goto failure;
    if (db_changes(database) != 1) {
        db_finalize(statement); rollback(); free(before.data); free_fields(fields, count);
        error(response, 409, "La commessa è stata aggiornata. Ricarica il riepilogo prima di riprovare."); return;
    }
    db_finalize(statement); statement = NULL;
    if (job_lifecycle_snapshot(job_id, &after, true, reason, NULL) != 1 ||
        !storage_audit(database, actor_id, archive ? "jobs.archive" : reopen ? "jobs.reopen" : "jobs.close",
                       "job", job_id, before.data, after.data)) goto failure;
    free(after.data); memset(&after, 0, sizeof(after));
    if (job_lifecycle_snapshot(job_id, &after, true, reason, scope) != 1) goto failure;
    if (commit_transaction(response)) {
        finish_json(response, 200, &after);
        after.data = NULL;
    }
    free(before.data); free(after.data); free_fields(fields, count); return;
failure:
    db_finalize(statement); rollback(); free(before.data); free(after.data); free_fields(fields, count);
    server_error(response);
}

static bool job_lifecycle_handle(const char *method, const char *target, const char *body,
                                  DbInt actor_id, bool manager, WorkspaceResponse *response) {
    if (strncmp(target, "/api/jobs/", 10) != 0) return false;
    const char *suffix = strchr(target + 10, '/');
    if (!suffix || (strcmp(suffix, "/closure") != 0 && strcmp(suffix, "/lifecycle") != 0)) return false;
    if (actor_id <= 0) { error(response, 401, "Accedi per continuare."); return true; }
    if (!manager) { error(response, 403, "La chiusura delle commesse è riservata ai gestori."); return true; }
    size_t size = (size_t)(suffix - target - 10);
    char identifier[20];
    DbInt job_id = 0;
    if (!size || size >= sizeof(identifier)) {
        error(response, 400, "Identificativo non valido."); return true;
    }
    memcpy(identifier, target + 10, size); identifier[size] = '\0';
    if (!parse_positive(identifier, &job_id)) { error(response, 400, "Identificativo non valido."); return true; }
    if (strcmp(suffix, "/closure") == 0 && strcmp(method, "GET") == 0) job_lifecycle_read(job_id, actor_id, response);
    else if (strcmp(suffix, "/lifecycle") == 0 && strcmp(method, "POST") == 0)
        job_lifecycle_write(job_id, body ? body : "", actor_id, response);
    else error(response, 405, "Metodo non consentito.");
    return true;
}
