/* Personal expected work and calendar exceptions; included by workspace.c. */
static bool schedule_minutes(const char *text, DbInt *minutes) {
    if (strcmp(text, "0") == 0) { *minutes = 0; return true; }
    return parse_positive(text, minutes) && *minutes <= 1440;
}

static void schedule_row_json(Json *json, DbStmt *statement, int kind) {
    if (kind == 2) {
        append(json, "{\"date\":"); append_string(json, column_text(statement, 0));
        append(json, ",\"name\":"); append_string(json, column_text(statement, 1));
    } else {
        append(json, "{\"id\":"); append_integer(json, db_column_int64(statement, 0));
        append(json, ",\"user_id\":"); append_integer(json, db_column_int64(statement, 1));
        append(json, ",\"user_name\":"); append_string(json, column_text(statement, 2));
        if (kind == 0) {
            static const char *days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
            append(json, ",\"starts_on\":"); append_string(json, column_text(statement, 3));
            for (int i = 0; i < 7; i++) {
                append(json, ",\""); append(json, days[i]); append(json, "\":");
                append_integer(json, db_column_int64(statement, 4 + i));
            }
            append(json, ",\"revision\":"); append_integer(json, db_column_int64(statement,11));
        } else {
            append(json, ",\"date\":"); append_string(json, column_text(statement, 3));
            append(json, ",\"expected_minutes\":"); append_integer(json, db_column_int64(statement, 4));
            append(json, ",\"reason\":"); append_string(json, column_text(statement, 5));
        }
    }
    append(json, "}");
}

static const char *schedule_collection(int kind) {
    return kind == 0 ? "schedules" : kind == 1 ? "exceptions" : "holidays";
}

static void schedule_list(const char *query, DbInt actor, bool manager, int kind, WorkspaceResponse *response) {
    Field fields[] = {{"user_id", NULL}};
    if (!parse_fields(query, fields, 1, response)) return;
    DbInt user = manager ? 0 : actor;
    if (*field_value(fields) && (!parse_positive(field_value(fields), &user) || (!manager && user != actor))) {
        free_fields(fields, 1); error(response, manager ? 400 : 403, "Utente non consentito."); return;
    }
    if (kind == 2 && *query) { free_fields(fields, 1); error(response, 400, "Le festività sono comuni a tutti."); return; }
    free_fields(fields, 1);
    const char *sql = kind == 0 ? "SELECT s.id,s.user_id,u.name,s.starts_on,s.mon,s.tue,s.wed,s.thu,s.fri,s.sat,s.sun,s.revision "
        "FROM user_work_schedules s JOIN users u ON u.id=s.user_id WHERE (?=0 OR s.user_id=?) ORDER BY s.user_id,s.starts_on DESC" :
        kind == 1 ? "SELECT e.id,e.user_id,u.name,e.date,e.expected_minutes,e.reason FROM user_work_exceptions e "
        "JOIN users u ON u.id=e.user_id WHERE (?=0 OR e.user_id=?) ORDER BY e.date DESC,e.user_id" :
        "SELECT date,name FROM business_holidays ORDER BY date DESC";
    DbStmt *statement = NULL; Json json = {0};
    if (!prepare(&statement, sql) || (kind != 2 && (!bind_id(statement, 1, user) || !bind_id(statement, 2, user)))) goto failure;
    append(&json, "{\"ok\":true,\""); append(&json, schedule_collection(kind)); append(&json, "\":[");
    int result, count = 0;
    while ((result = db_step(statement)) == DB_ROW) {
        if(kind!=2){int visible=access_policy_can_view_time(database,actor,db_column_int64(statement,1),manager);if(visible<0)goto failure;if(!visible)continue;}
        if (++count > RESULT_LIMIT) { db_finalize(statement); free(json.data); error(response, 409, "Troppi risultati: seleziona un utente."); return; }
        if (count > 1) append(&json, ",");
        schedule_row_json(&json, statement, kind);
    }
    if (result != DB_DONE) goto failure;
    db_finalize(statement); append(&json, "]}"); finish_json(response, 200, &json); return;
failure:
    db_finalize(statement); free(json.data); server_error(response);
}

static void schedule_write(const char *body, DbInt actor, int kind, WorkspaceResponse *response) {
    Field fields[] = {{"user_id", NULL}, {"starts_on", NULL}, {"mon", NULL}, {"tue", NULL}, {"wed", NULL},
        {"thu", NULL}, {"fri", NULL}, {"sat", NULL}, {"sun", NULL}, {"date", NULL},
        {"expected_minutes", NULL}, {"reason", NULL}, {"name", NULL}, {"revision", NULL}};
    if (!parse_fields(body, fields, 14, response)) return;
    trim(fields[11].value); trim(fields[12].value);
    DbInt user = 0, minutes[7] = {0}, revision=0;
    if(fields[13].value&&(!parse_positive(field_value(&fields[13]),&revision)||revision==INT64_MAX)){free_fields(fields,14);error(response,400,"Revisione orario non valida.");return;}
    const char *date = field_value(&fields[kind == 0 ? 1 : 9]);
    bool okay = valid_date(date) && (kind == 2 || parse_positive(field_value(fields), &user));
    for (int i = 0; kind == 0 && i < 7; i++) okay = schedule_minutes(field_value(&fields[2 + i]), &minutes[i]) && okay;
    if (kind == 1) okay = schedule_minutes(field_value(&fields[10]), &minutes[0]) &&
        valid_text(field_value(&fields[11]), 1000, true, true) && okay;
    if (kind == 2) okay = valid_text(field_value(&fields[12]), 160, true, false) && okay;
    /* A shared parser must not silently accept fields from another resource. */
    for (int i = 0; i < 14; i++) {
        bool allowed = kind == 0 ? i <= 8 || i == 13 : kind == 1 ? i == 0 || (i >= 9 && i <= 11) : i == 9 || i == 12;
        if (!allowed && fields[i].value) okay = false;
    }
    if (!okay) { free_fields(fields, 14); error(response, 400, "Controlla utente, data e minuti attesi da 0 a 1440."); return; }
    if (!begin_transaction(response)) { free_fields(fields, 14); return; }
    DbStmt *statement = NULL; Json audit = {0}, before={0}; DbInt id = 0;
    AccessPolicy live;
    if(access_policy_resolve(database,actor,NULL,&live)!=1)goto failure;
    int permitted=kind==2?(live.admin?1:0):access_policy_can_manage_user(database,actor,user);
    if(permitted!=1){rollback();if(permitted<0)server_error(response);else error(response,403,"Questa persona non rientra nella tua delega.");goto cleanup;}
    if (kind != 2) {
        if (!prepare(&statement, "SELECT id FROM users WHERE id=?") || !bind_id(statement, 1, user)) goto failure;
        int found = db_step(statement); db_finalize(statement); statement = NULL;
        if (found == DB_DONE) { rollback(); error(response, 404, "Utente non trovato."); goto cleanup; }
        if (found != DB_ROW) goto failure;
    }
    const char *lookup = kind == 0 ? "SELECT s.id,s.user_id,u.name,s.starts_on,s.mon,s.tue,s.wed,s.thu,s.fri,s.sat,s.sun,s.revision FROM user_work_schedules s JOIN users u ON u.id=s.user_id WHERE s.user_id=? AND s.starts_on=?" :
        kind == 1 ? "SELECT e.id,e.user_id,u.name,e.date,e.expected_minutes,e.reason FROM user_work_exceptions e JOIN users u ON u.id=e.user_id WHERE e.user_id=? AND e.date=?" : "SELECT date,name FROM business_holidays WHERE date=?";
    if (!prepare(&statement, lookup) || (kind != 2 && !bind_id(statement, 1, user)) ||
        !bind_text(statement, kind == 2 ? 1 : 2, date)) goto failure;
    int found = db_step(statement); DbInt current=0;
    if (found == DB_ROW) {id=kind==2?1:db_column_int64(statement,0);schedule_row_json(&before,statement,kind);if(kind==0)current=db_column_int64(statement,11);}
    db_finalize(statement); statement = NULL;
    if (found != DB_ROW && found != DB_DONE) goto failure;
    if(kind==0&&((id&&current!=revision)||(!id&&revision))){rollback();error(response,409,"L’orario è cambiato. Ricarica la versione prima di salvarla.");goto cleanup;}
    if (kind == 0) {
        if (!prepare(&statement, id ? "UPDATE user_work_schedules SET mon=?,tue=?,wed=?,thu=?,fri=?,sat=?,sun=?,revision=revision+1 WHERE user_id=? AND starts_on=?" :
            "INSERT INTO user_work_schedules(mon,tue,wed,thu,fri,sat,sun,user_id,starts_on) VALUES(?,?,?,?,?,?,?,?,?)")) goto failure;
        for (int i = 0; i < 7; i++) if (!bind_id(statement, i + 1, minutes[i])) goto failure;
        if (!bind_id(statement, 8, user) || !bind_text(statement, 9, date)) goto failure;
    } else if (kind == 1) {
        if (!prepare(&statement, id ? "UPDATE user_work_exceptions SET expected_minutes=?,reason=? WHERE user_id=? AND date=?" :
            "INSERT INTO user_work_exceptions(expected_minutes,reason,user_id,date) VALUES(?,?,?,?)") ||
            !bind_id(statement, 1, minutes[0]) || !bind_text(statement, 2, field_value(&fields[11])) ||
            !bind_id(statement, 3, user) || !bind_text(statement, 4, date)) goto failure;
    } else {
        if (!prepare(&statement, id ? "UPDATE business_holidays SET name=? WHERE date=?" :
            "INSERT INTO business_holidays(name,date) VALUES(?,?)") || !bind_text(statement, 1, field_value(&fields[12])) ||
            !bind_text(statement, 2, date)) goto failure;
    }
    if (db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    bool created = !id;
    if (!id && kind != 2) id = db_last_insert_rowid(database);
    append(&audit, "{\"user_id\":"); append_integer(&audit, user);
    append(&audit, ",\"date\":"); append_string(&audit, date);
    if (kind == 0) {
        static const char *days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
        for (int i = 0; i < 7; i++) { append(&audit, ",\""); append(&audit, days[i]); append(&audit, "\":"); append_integer(&audit, minutes[i]); }
    } else if (kind == 1) {
        append(&audit, ",\"expected_minutes\":"); append_integer(&audit, minutes[0]);
        append(&audit, ",\"reason\":"); append_string(&audit, field_value(&fields[11]));
    } else { append(&audit, ",\"name\":"); append_string(&audit, field_value(&fields[12])); }
    if(kind==0){append(&audit,",\"revision\":");append_integer(&audit,revision+1);}
    append(&audit, "}");
    const char *action = kind == 0 ? "work_schedules.save" : kind == 1 ? "work_exceptions.save" : "business_holidays.save";
    if (audit.failed || !storage_audit(database, actor, action, kind == 0 ? "work_schedule" : kind == 1 ? "work_exception" : "business_holiday", id, before.data, audit.data)) goto failure;
    if (commit_transaction(response)) success(response, created ? 201 : 200, id);
    goto cleanup;
failure:
    db_finalize(statement); rollback(); server_error(response);
cleanup:
    free(audit.data); free(before.data); free_fields(fields, 14);
}

static void schedule_delete(const char *path, const char *body, DbInt actor, int kind, WorkspaceResponse *response) {
    if (*body) { error(response, 400, "La cancellazione non richiede campi."); return; }
    size_t length = strlen(path);
    if (length < 8 || strcmp(path + length - 7, "/delete") != 0 || length - 7 >= 32) {
        error(response, 400, "Percorso non valido."); return;
    }
    char identifier[32]; memcpy(identifier, path, length - 7); identifier[length - 7] = '\0';
    DbInt id = 0;
    if ((kind == 2 && !valid_date(identifier)) || (kind == 1 && !parse_positive(identifier, &id))) {
        error(response, 400, "Identificativo non valido."); return;
    }
    if (!begin_transaction(response)) return;
    DbStmt *statement = NULL;
    if(kind==1){if(!prepare(&statement,"SELECT user_id FROM user_work_exceptions WHERE id=?")||!bind_id(statement,1,id))goto failure;int found=db_step(statement);DbInt user=found==DB_ROW?db_column_int64(statement,0):0;db_finalize(statement);statement=NULL;if(found!=DB_ROW&&found!=DB_DONE)goto failure;int permitted=user?access_policy_can_manage_user(database,actor,user):0;if(permitted!=1){rollback();if(permitted<0)server_error(response);else error(response,404,"Voce non trovata.");return;}}
    else {AccessPolicy p;if(access_policy_resolve(database,actor,NULL,&p)!=1||!p.admin){rollback();error(response,403,"Le festività richiedono l’amministrazione dello spazio.");return;}}
    if (!prepare(&statement, kind == 2 ? "DELETE FROM business_holidays WHERE date=?" : "DELETE FROM user_work_exceptions WHERE id=?") ||
        (kind == 2 ? !bind_text(statement, 1, identifier) : !bind_id(statement, 1, id)) || db_step(statement) != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (!db_changes(database)) { rollback(); error(response, 404, "Voce non trovata."); return; }
    Json audit = {0}; append(&audit, "{\"identifier\":"); append_string(&audit, identifier); append(&audit, "}");
    bool audited = !audit.failed && storage_audit(database, actor, kind == 2 ? "business_holidays.delete" : "work_exceptions.delete",
        kind == 2 ? "business_holiday" : "work_exception", id, audit.data, NULL);
    free(audit.data);
    if (!audited) goto failure;
    if (commit_transaction(response)) success(response, 200, id);
    return;
failure:
    db_finalize(statement); rollback(); server_error(response);
}

static bool work_schedules_handle(const char *method, const char *target, const char *query,
                                  const char *body, DbInt actor, bool manager, WorkspaceResponse *response) {
    int kind = strcmp(target, "/api/work-schedules") == 0 ? 0 :
        strncmp(target, "/api/work-exceptions", 20) == 0 && (target[20] == '\0' || target[20] == '/') ? 1 :
        strncmp(target, "/api/business-holidays", 22) == 0 && (target[22] == '\0' || target[22] == '/') ? 2 : -1;
    if (kind < 0) return false;
    response->status = 0; response->body = NULL;
    if (!database) { server_error(response); return true; }
    if (actor <= 0) { error(response, 401, "Accedi per continuare."); return true; }
    if (!query) query = "";
    if (!body) body = "";
    const char *item = kind == 1 ? target + 20 : kind == 2 ? target + 22 : "";
    if (!*item && strcmp(method, "GET") == 0) schedule_list(query, actor, manager, kind, response);
    else if (!manager) error(response, 403, "Solo il gestore può configurare orari, assenze e festività.");
    else if (*query) error(response, 400, "Parametri della richiesta non validi.");
    else if (strcmp(method, "POST") != 0) error(response, 405, "Metodo non consentito.");
    else if (*item) schedule_delete(item + 1, body, actor, kind, response);
    else schedule_write(body, actor, kind, response);
    return true;
}
