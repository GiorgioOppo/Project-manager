/* Included by workspace.c; every aggregate respects the caller's live access. */

static bool job_overview_count(DbInt job_id, const char *table, const char *finished,
                               DbInt *total, DbInt *done) {
    char sql[256];
    int length = snprintf(sql, sizeof(sql),
        "SELECT COUNT(*),COALESCE(SUM(CASE WHEN status='%s' THEN 1 ELSE 0 END),0) FROM %s WHERE job_id=?",
        finished, table);
    DbStmt *statement = NULL;
    bool okay = length > 0 && (size_t)length < sizeof(sql) && prepare(&statement, sql) &&
        bind_id(statement, 1, job_id) && db_step(statement) == DB_ROW;
    if (okay) { *total = db_column_int64(statement, 0); *done = db_column_int64(statement, 1); }
    db_finalize(statement); return okay;
}

static void job_overview_read(DbInt job_id, DbInt actor_id, bool manager,
                              WorkspaceResponse *response) {
    char today[11];
    if (!today_date(today)) { server_error(response); return; }
    Json job = {0}, json = {0};
    DbStmt *statement = NULL;
    AccessPolicy policy;
    char scope[5000], sql[6200];
    if (!workspace_policy(actor_id, &policy) || !workspace_time_scope(actor_id, manager, scope)) goto failure;
    manager = policy.manager;
    int found = job_snapshot(job_id, &job);
    if (found < 0) goto failure;
    if (!found) { free(job.data); error(response, 404, "Commessa non trovata."); return; }
    int management = manager ? access_policy_can_manage_job(database,actor_id,job_id) : 0;
    if (management < 0) goto failure;
    int permission = management == 1 ? 3 : organization_job_access(database, job_id, actor_id, today, NULL);
    if (permission < 0) goto failure;
    if (!(permission & 1)) { free(job.data); error(response, 403, "Non hai accesso a questa commessa."); return; }
    if (!policy.sales && !policy.finance) {
        struct json_object *object = json_tokener_parse(job.data);
        if (!object || json_object_get_type(object) != json_type_object) {
            json_object_put(object); goto failure;
        }
        static const char *const hidden[] = {"contract_id", "contract_code", "contract_title", "supplier_name"};
        for (size_t index = 0; index < sizeof(hidden) / sizeof(hidden[0]); index++)
            json_object_object_del(object, hidden[index]);
        Json redacted = {0}; append(&redacted, json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN));
        json_object_put(object);
        if (redacted.failed) { free(redacted.data); goto failure; }
        free(job.data); job = redacted;
    }
    bool closed = false;
    if (!prepare(&statement, "SELECT time_closed,archived FROM jobs WHERE id=?") ||
        !bind_id(statement, 1, job_id) || db_step(statement) != DB_ROW) goto failure;
    closed = db_column_int(statement, 0) || db_column_int(statement, 1);
    db_finalize(statement); statement = NULL;
    DbInt tasks = 0, tasks_done = 0, blockers = 0, blockers_done = 0;
    int length = snprintf(sql, sizeof(sql),
        "SELECT COALESCE(SUM(minutes),0),COUNT(*) FROM time_entries WHERE job_id=? AND user_id IN (%s)", scope);
    if (length < 0 || (size_t)length >= sizeof(sql) ||
        !job_overview_count(job_id, "job_tasks", "done", &tasks, &tasks_done) ||
        !job_overview_count(job_id, "job_blockers", "resolved", &blockers, &blockers_done) ||
        !prepare(&statement, sql) || !bind_id(statement, 1, job_id) ||
        db_step(statement) != DB_ROW) goto failure;
    append(&json, "{\"job\":");
    (void)append_bytes(&json, job.data, job.length - 1);
    append(&json, ",\"can_view\":true,\"can_log_time\":");
    append(&json, (permission & 2) && !closed ? "true" : "false"); append(&json, "}");
    append(&json, ",\"scope\":"); append_string(&json, manager ? "team" : "personal");
    append(&json, ",\"total_minutes\":"); append_integer(&json, db_column_int64(statement, 0));
    append(&json, ",\"time_entries\":"); append_integer(&json, db_column_int64(statement, 1));
    db_finalize(statement); statement = NULL;
    append(&json, ",\"tasks\":{\"total\":"); append_integer(&json, tasks);
    append(&json, ",\"open\":"); append_integer(&json, tasks - tasks_done);
    append(&json, ",\"done\":"); append_integer(&json, tasks_done);
    append(&json, "},\"blockers\":{\"total\":"); append_integer(&json, blockers);
    append(&json, ",\"open\":"); append_integer(&json, blockers - blockers_done);
    append(&json, ",\"resolved\":"); append_integer(&json, blockers_done);
    append(&json, "},\"task_totals\":[");
    length = snprintf(sql, sizeof(sql),
        "SELECT t.id,t.title,t.status,COALESCE(SUM(e.minutes),0),COUNT(e.id) FROM job_tasks t "
        "LEFT JOIN time_entries e ON e.task_id=t.id AND e.job_id=t.job_id AND e.user_id IN (%s) "
        "WHERE t.job_id=? GROUP BY t.id,t.title,t.status ORDER BY t.id LIMIT 501", scope);
    if (length < 0 || (size_t)length >= sizeof(sql) || !prepare(&statement, sql) ||
        !bind_id(statement, 1, job_id)) goto failure;
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++count > 500) goto failure;
        if (count > 1) append(&json, ",");
        append(&json, "{\"task_id\":"); append_integer(&json, db_column_int64(statement, 0));
        append(&json, ",\"title\":"); append_string(&json, column_text(statement, 1));
        append(&json, ",\"status\":"); append_string(&json, column_text(statement, 2));
        append(&json, ",\"total_minutes\":"); append_integer(&json, db_column_int64(statement, 3));
        append(&json, ",\"time_entries\":"); append_integer(&json, db_column_int64(statement, 4)); append(&json, "}");
    }
    if (result != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    append(&json, "]");
    if (manager) {
        append(&json, ",\"collaborators\":[");
        if (!prepare(&statement, "SELECT id,name,email,role FROM users WHERE active=1 ORDER BY name,id LIMIT 201")) goto failure;
        int scanned = 0;
        count = 0;
        while ((result = db_step(statement)) == DB_ROW) {
            if (++scanned > 200) goto failure;
            DbInt user_id = db_column_int64(statement, 0);
            int access = strcmp(column_text(statement, 3), "manager") == 0 ? 3 :
                organization_job_access(database, job_id, user_id, today, NULL);
            if (access < 0) goto failure;
            if (!(access & 1)) continue;
            if (++count > 1) append(&json, ",");
            append(&json, "{\"id\":"); append_integer(&json, user_id);
            append(&json, ",\"name\":"); append_string(&json, column_text(statement, 1));
            append(&json, ",\"email\":"); append_string(&json, column_text(statement, 2));
            append(&json, ",\"role\":"); append_string(&json, column_text(statement, 3));
            append(&json, ",\"can_view\":true,\"can_log_time\":");
            append(&json, (access & 2) && !closed ? "true" : "false"); append(&json, "}");
        }
        if (result != DB_DONE) goto failure;
        db_finalize(statement); statement = NULL;
        append(&json, "]");
    }
    append(&json, "}"); free(job.data); finish_json(response, 200, &json); return;
failure:
    db_finalize(statement); free(job.data); free(json.data); server_error(response);
}

static void job_access_source(Json *json, const char *type, DbInt group_id,
                               const char *group_name, bool inherited, DbInt role_id,
                               const char *role_name, bool can_log, const char *starts,
                               const char *ends, bool valid_today, bool active) {
    append(json, "{\"type\":"); append_string(json, type);
    if (group_id) {
        append(json, ",\"group_id\":"); append_integer(json, group_id);
        append(json, ",\"group_name\":"); append_string(json, group_name);
        append(json, ",\"inherited\":"); append(json, inherited ? "true" : "false");
    }
    append(json, ",\"role_id\":"); append_integer(json, role_id);
    append(json, ",\"role_name\":"); append_string(json, role_name);
    append(json, ",\"can_log_time\":"); append(json, can_log ? "true" : "false");
    append(json, ",\"starts_on\":"); append_string(json, starts);
    append(json, ",\"ends_on\":"); append_string(json, ends);
    append(json, ",\"valid_today\":"); append(json, valid_today ? "true" : "false");
    append(json, ",\"active\":"); append(json, active ? "true" : "false"); append(json, "}");
}

static void job_access_read(DbInt job_id, const char *query, WorkspaceResponse *response) {
    Field fields[] = {{"user_id", NULL}};
    if (!parse_fields(query, fields, 1, response)) return;
    DbInt user_id = 0;
    if (!parse_positive(field_value(&fields[0]), &user_id)) {
        free_fields(fields, 1); error(response, 400, "Seleziona un utente valido."); return;
    }
    free_fields(fields, 1);
    char today[11];
    if (!today_date(today)) { server_error(response); return; }
    Json json = {0};
    DbStmt *statement = NULL;
    if (!prepare(&statement, "SELECT is_common,time_closed,archived FROM jobs WHERE id=?") ||
        !bind_id(statement, 1, job_id)) goto failure;
    int result = db_step(statement);
    if (result == DB_DONE) { db_finalize(statement); error(response, 404, "Commessa non trovata."); return; }
    if (result != DB_ROW) goto failure;
    bool common = db_column_int(statement, 0) != 0;
    bool time_closed = db_column_int(statement, 1) != 0;
    bool archived = db_column_int(statement, 2) != 0;
    db_finalize(statement); statement = NULL;
    if (!prepare(&statement, "SELECT name,email,role,active FROM users WHERE id=?") || !bind_id(statement, 1, user_id)) goto failure;
    result = db_step(statement);
    if (result == DB_DONE) { db_finalize(statement); error(response, 404, "Utente non trovato."); return; }
    if (result != DB_ROW) goto failure;
    bool active = db_column_int(statement, 3) != 0;
    int management = active ? access_policy_can_manage_job(database, user_id, job_id) : 0;
    if (management < 0) goto failure;
    bool global_manager = management == 1;
    append(&json, "{\"job_id\":"); append_integer(&json, job_id);
    append(&json, ",\"user\":{\"id\":"); append_integer(&json, user_id);
    append(&json, ",\"name\":"); append_string(&json, column_text(statement, 0));
    append(&json, ",\"email\":"); append_string(&json, column_text(statement, 1));
    append(&json, ",\"role\":"); append_string(&json, column_text(statement, 2));
    append(&json, ",\"active\":"); append(&json, active ? "true" : "false"); append(&json, "}");
    db_finalize(statement); statement = NULL;
    int permission = !active ? 0 : global_manager ? 3 : organization_job_access(database, job_id, user_id, today, NULL);
    if (permission < 0) goto failure;
    bool groups_available = organization_groups_available();
    append(&json, ",\"today\":"); append_string(&json, today);
    append(&json, ",\"time_closed\":"); append(&json, time_closed ? "true" : "false");
    append(&json, ",\"archived\":"); append(&json, archived ? "true" : "false");
    append(&json, ",\"groups_available\":"); append(&json, groups_available ? "true" : "false");
    append(&json, ",\"effective\":{\"can_view\":"); append(&json, permission & 1 ? "true" : "false");
    append(&json, ",\"can_log_time\":"); append(&json, (permission & 2) && !time_closed && !archived ? "true" : "false");
    append(&json, "},\"sources\":[");
    int count = 0;
    if (global_manager) {
        job_access_source(&json, "manager", 0, NULL, false, 0, "Gestore", true, "", "", true, active); count++;
    }
    if (common) {
        if (count++) append(&json, ",");
        job_access_source(&json, "common", 0, NULL, false, 0, "Commessa comune", true, "", "", true, active);
    }
    if (!prepare(&statement,
        "WITH RECURSIVE user_groups(id,depth) AS ("
        "SELECT group_id,1 FROM group_members WHERE user_id=? "
        "UNION ALL SELECT g.parent_id,u.depth+1 FROM organization_groups g JOIN user_groups u ON g.id=u.id "
        "WHERE g.parent_id IS NOT NULL AND u.depth<17),"
        "paths AS (SELECT id,MIN(depth) AS depth FROM user_groups WHERE depth<=16 GROUP BY id) "
        "SELECT 'direct',0,'',0,r.id,r.name,r.can_log_time,a.starts_on,a.ends_on FROM job_assignments a "
        "JOIN job_roles r ON r.id=COALESCE(a.role_id,2) WHERE a.job_id=? AND a.user_id=? "
        "UNION ALL SELECT 'group',g.id,g.name,CASE WHEN p.depth>1 THEN 1 ELSE 0 END,r.id,r.name,r.can_log_time,a.starts_on,a.ends_on "
        "FROM job_group_grants a JOIN paths p ON p.id=a.group_id JOIN organization_groups g ON g.id=a.group_id "
        "JOIN job_roles r ON r.id=a.role_id WHERE a.job_id=? ORDER BY 1,2 LIMIT 202") ||
        !bind_id(statement, 1, user_id) || !bind_id(statement, 2, job_id) ||
        !bind_id(statement, 3, user_id) || !bind_id(statement, 4, job_id)) goto failure;
    int scanned = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if (++scanned > 201) goto failure;
        const char *starts = column_text(statement, 7), *ends = column_text(statement, 8);
        bool valid_today = (!*starts || strcmp(starts, today) <= 0) && (!*ends || strcmp(ends, today) >= 0);
        bool group = db_column_int64(statement, 1) != 0;
        if (count++) append(&json, ",");
        job_access_source(&json, column_text(statement, 0), db_column_int64(statement, 1),
            column_text(statement, 2), db_column_int(statement, 3) != 0, db_column_int64(statement, 4),
            column_text(statement, 5), db_column_int(statement, 6) != 0, starts, ends, valid_today,
            active && valid_today && (!group || groups_available));
    }
    if (result != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    append(&json, "]}"); finish_json(response, 200, &json); return;
failure:
    db_finalize(statement); free(json.data); server_error(response);
}

static bool job_overview_handle(const char *method, const char *target, const char *query,
                                 DbInt actor_id, bool manager, WorkspaceResponse *response) {
    if (strncmp(target, "/api/jobs/", 10) != 0) return false;
    const char *suffix = strchr(target + 10, '/');
    if (!suffix || (strcmp(suffix, "/overview") != 0 && strcmp(suffix, "/access") != 0)) return false;
    if (actor_id <= 0) { error(response, 401, "Accedi per continuare."); return true; }
    if (strcmp(method, "GET") != 0) { error(response, 405, "Metodo non consentito."); return true; }
    bool access = strcmp(suffix, "/access") == 0;
    if (access && !manager) { error(response, 403, "La verifica degli accessi è riservata ai gestori."); return true; }
    if (!query) query = "";
    if (!access && *query) { error(response, 400, "Parametri della richiesta non validi."); return true; }
    size_t size = (size_t)(suffix - target - 10);
    char identifier[20];
    DbInt job_id = 0;
    if (!size || size >= sizeof(identifier)) { error(response, 400, "Identificativo non valido."); return true; }
    memcpy(identifier, target + 10, size); identifier[size] = '\0';
    if (!parse_positive(identifier, &job_id)) { error(response, 400, "Identificativo non valido."); return true; }
    if (access) job_access_read(job_id, query, response);
    else job_overview_read(job_id, actor_id, manager, response);
    return true;
}
