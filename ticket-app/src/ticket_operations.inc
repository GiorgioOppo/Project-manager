/* Operational metadata is kept separate from the original ticket rows. */
typedef struct {
    sqlite3_int64 id;
    char name[401], zone[101];
    int weekdays, start, end, response, resolution;
} OperationsPolicy;
static sqlite3_int64 operations_epoch(const char *iso) {
    if (!iso || strlen(iso) < 19)
        return (sqlite3_int64)time(NULL);
    struct tm value = {0};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &value.tm_year, &value.tm_mon, &value.tm_mday, &value.tm_hour, &value.tm_min,
               &value.tm_sec) != 6)
        return (sqlite3_int64)time(NULL);
    value.tm_year -= 1900;
    value.tm_mon--;
    value.tm_isdst = -1;
    const char *before = getenv("TZ");
    char *saved = before ? strdup(before) : NULL;
    setenv("TZ", "UTC", 1);
    tzset();
    time_t result = mktime(&value);
    if (saved) {
        setenv("TZ", saved, 1);
        free(saved);
    } else
        unsetenv("TZ");
    tzset();
    return result > 0 ? (sqlite3_int64)result : (sqlite3_int64)time(NULL);
}
static const char *operations_priority(const char *impact, const char *urgency) {
    if (!strcmp(impact, "high") && !strcmp(urgency, "high"))
        return "urgent";
    if (!strcmp(impact, "high") || !strcmp(urgency, "high"))
        return "high";
    if (!strcmp(impact, "low") && !strcmp(urgency, "low"))
        return "low";
    return "normal";
}
static bool operations_level(const char *value) {
    return value && (!strcmp(value, "low") || !strcmp(value, "medium") || !strcmp(value, "high"));
}
static const char *operations_string(json_object *o, const char *key, const char *fallback) {
    json_object *v = o ? json_object_object_get(o, key) : NULL;
    return v ? json_object_get_string(v) : fallback;
}
static bool operations_fields(json_object *input, json_object *current, TicketFields *f) {
    f->impact = operations_string(current, "impact",
                                  !strcmp(f->priority, "low")      ? "low"
                                  : !strcmp(f->priority, "normal") ? "medium"
                                                                   : "high");
    f->urgency = operations_string(current, "urgency",
                                   !strcmp(f->priority, "low")      ? "low"
                                   : !strcmp(f->priority, "urgent") ? "high"
                                                                    : "medium");
    f->category = operations_string(current, "category", "");
    f->group_name = operations_string(current, "group_name", "");
    f->override_reason = operations_string(current, "priority_override_reason", "");
    f->waiting_reason = operations_string(current, "waiting_reason", NULL);
    f->follow_up_at = current ? json_object_get_int64(json_object_object_get(current, "follow_up_at")) : 0;
    f->assessed = current && json_object_get_boolean(json_object_object_get(current, "assessment_active"));
    bool assessment = json_object_object_get(input, "impact") || json_object_object_get(input, "urgency");
    if (!f->assessed && !assessment && json_object_object_get(input, "priority")) {
        f->impact = !strcmp(f->priority, "low") ? "low" : !strcmp(f->priority, "normal") ? "medium" : "high";
        f->urgency = !strcmp(f->priority, "low") ? "low" : !strcmp(f->priority, "urgent") ? "high" : "medium";
    }
    if (!get_string(input, "impact", &f->impact, 16, false, true) ||
        !get_string(input, "urgency", &f->urgency, 16, false, true) || !operations_level(f->impact) ||
        !operations_level(f->urgency) || !get_string(input, "category", &f->category, 80, false, false) ||
        !get_string(input, "group_name", &f->group_name, 80, false, false) ||
        !get_string(input, "priority_override_reason", &f->override_reason, 500, false, false))
        return false;
    if (assessment) {
        f->assessed = true;
        if (!json_object_object_get(input, "priority"))
            f->priority = operations_priority(f->impact, f->urgency);
    }
    if (f->assessed && strcmp(f->priority, operations_priority(f->impact, f->urgency)) &&
        (!*f->override_reason || whitespace_only(f->override_reason)))
        return false;
    if (!strcmp(f->priority, operations_priority(f->impact, f->urgency)))
        f->override_reason = "";
    json_object *reason = NULL, *follow = NULL;
    bool has_reason = json_object_object_get_ex(input, "waiting_reason", &reason);
    bool has_follow = json_object_object_get_ex(input, "follow_up_at", &follow);
    if (has_reason) {
        if (!reason)
            f->waiting_reason = NULL;
        else if (!json_object_is_type(reason, json_type_string) ||
                 strlen(json_object_get_string(reason)) != (size_t)json_object_get_string_len(reason))
            return false;
        else
            f->waiting_reason = json_object_get_string(reason);
    }
    if (f->waiting_reason && strcmp(f->waiting_reason, "customer") && strcmp(f->waiting_reason, "supplier") &&
        strcmp(f->waiting_reason, "spare_part") && strcmp(f->waiting_reason, "authorization"))
        return false;
    if (has_follow) {
        if (!follow)
            f->follow_up_at = 0;
        else if (!json_object_is_type(follow, json_type_int) || (f->follow_up_at = json_object_get_int64(follow)) < 1 ||
                 f->follow_up_at > SCHEDULE_MAX)
            return false;
    }
    if (!strcmp(f->status, "waiting")) {
        bool was_waiting = current && !strcmp(operations_string(current, "status", ""), "waiting");
        if ((!was_waiting || has_reason || has_follow) && (!f->waiting_reason || !f->follow_up_at))
            return false;
    } else {
        f->waiting_reason = NULL;
        f->follow_up_at = 0;
    }
    return true;
}
static void operations_policy_default(OperationsPolicy *p, const char *priority) {
    memset(p, 0, sizeof(*p));
    strcpy(p->name, "Standard");
    strcpy(p->zone, "Europe/Rome");
    p->weekdays = 62;
    p->start = 540;
    p->end = 1080;
    p->response = !strcmp(priority, "urgent") ? 15
                  : !strcmp(priority, "high") ? 60
                  : !strcmp(priority, "low")  ? 480
                                              : 240;
    p->resolution = !strcmp(priority, "urgent") ? 240
                    : !strcmp(priority, "high") ? 480
                    : !strcmp(priority, "low")  ? 2400
                                                : 960;
}
static bool operations_policy(OperationsPolicy *p, sqlite3_int64 job, const char *priority) {
    operations_policy_default(p, priority);
    sqlite3_stmt *st =
        prepare("SELECT id,name,timezone,weekdays,day_start,day_end,response_minutes,resolution_minutes FROM "
                "sla_policies WHERE active=1 AND (job_id IS NULL OR job_id=?1) AND (priority='all' OR priority=?2) "
                "ORDER BY (job_id IS NOT NULL) DESC,(priority<>'all') DESC,id DESC LIMIT 1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, job);
    bind_text(st, 2, priority);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        p->id = sqlite3_column_int64(st, 0);
        bounded_copy(p->name, sizeof(p->name), column_text(st, 1));
        bounded_copy(p->zone, sizeof(p->zone), column_text(st, 2));
        p->weekdays = sqlite3_column_int(st, 3);
        p->start = sqlite3_column_int(st, 4);
        p->end = sqlite3_column_int(st, 5);
        p->response = sqlite3_column_int(st, 6);
        p->resolution = sqlite3_column_int(st, 7);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}
/* mktime applies the chosen IANA timezone separately to each business day,
 * including 23/25-hour daylight-saving transitions. */
static sqlite3_int64 operations_business(OperationsPolicy *p, sqlite3_int64 from, sqlite3_int64 to,
                                         sqlite3_int64 add_minutes) {
    const char *before = getenv("TZ");
    char *saved = before ? strdup(before) : NULL;
    setenv("TZ", p->zone, 1);
    tzset();
    time_t stamp = (time_t)from;
    struct tm day;
    localtime_r(&stamp, &day);
    day.tm_hour = 0;
    day.tm_min = 0;
    day.tm_sec = 0;
    day.tm_isdst = -1;
    sqlite3_int64 result = 0, remaining = add_minutes * 60;
    for (int count = 0; count < 60000; count++) {
        struct tm start = day, end = day;
        start.tm_hour = p->start / 60;
        start.tm_min = p->start % 60;
        end.tm_hour = p->end / 60;
        end.tm_min = p->end % 60;
        start.tm_isdst = end.tm_isdst = -1;
        sqlite3_int64 lo = (sqlite3_int64)mktime(&start), hi = (sqlite3_int64)mktime(&end);
        if (lo > SCHEDULE_MAX)
            break;
        if (p->weekdays & (1 << start.tm_wday)) {
            if (lo < from)
                lo = from;
            if (add_minutes >= 0) {
                if (hi > lo) {
                    if (remaining <= hi - lo) {
                        result = lo + remaining;
                        break;
                    }
                    remaining -= hi - lo;
                }
            } else {
                if (hi > to)
                    hi = to;
                if (hi > lo)
                    result += hi - lo;
            }
        }
        if (add_minutes < 0 && hi >= to)
            break;
        day.tm_mday++;
        day.tm_isdst = -1;
        stamp = mktime(&day);
        if (stamp < 0)
            break;
        localtime_r(&stamp, &day);
    }
    if (saved) {
        setenv("TZ", saved, 1);
        free(saved);
    } else
        unsetenv("TZ");
    tzset();
    return add_minutes >= 0 ? (result ? result : SCHEDULE_MAX) : result / 60;
}
static bool operations_first_response(sqlite3_int64 id, sqlite3_int64 when) {
    sqlite3_stmt *st = prepare("UPDATE ticket_operations SET first_response_at=CASE WHEN first_response_at IS NULL OR "
                               "first_response_at>?2 THEN ?2 ELSE first_response_at END WHERE ticket_id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, when);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
static bool operations_apply(sqlite3_int64 id, Auth *a, json_object *input, json_object *current, TicketFields *f) {
    (void)input;
    OperationsPolicy p;
    if (!operations_policy(&p, f->job_id, f->priority))
        return false;
    sqlite3_int64 now = (sqlite3_int64)time(NULL),
                  started = current ? operations_epoch(operations_string(current, "created_at", NULL)) : now;
    bool old_done = current && (!strcmp(operations_string(current, "status", ""), "resolved") ||
                                !strcmp(operations_string(current, "status", ""), "closed"));
    bool done = !strcmp(f->status, "resolved") || !strcmp(f->status, "closed");
    bool old_wait = current && !strcmp(operations_string(current, "status", ""), "waiting");
    bool wait = !strcmp(f->status, "waiting");
    bool policy_changed = !current || strcmp(operations_string(current, "priority", "normal"), f->priority) ||
                          json_object_get_int64(json_object_object_get(current, "job_id")) != f->job_id;
    sqlite3_stmt *st = prepare(
        "INSERT INTO "
        "ticket_operations(ticket_id,impact,urgency,assessed,category,group_name,override_reason,waiting_reason,follow_"
        "up_at,resolved_at,policy_id,policy_name,sla_timezone,sla_weekdays,sla_day_start,sla_day_end,response_budget,"
        "resolution_budget,started_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19) ON "
        "CONFLICT(ticket_id) DO UPDATE SET "
        "impact=excluded.impact,urgency=excluded.urgency,assessed=excluded.assessed,category=excluded.category,group_"
        "name=excluded.group_name,override_reason=excluded.override_reason,waiting_reason=excluded.waiting_reason,"
        "follow_up_at=excluded.follow_up_at,resolved_at=CASE WHEN ?20=1 THEN NULL WHEN excluded.resolved_at IS NOT "
        "NULL THEN COALESCE(ticket_operations.resolved_at,excluded.resolved_at) ELSE ticket_operations.resolved_at "
        "END,reopened_count=reopened_count+?20,"
        "policy_id=CASE WHEN ?21 THEN excluded.policy_id ELSE ticket_operations.policy_id END,"
        "policy_name=CASE WHEN ?21 THEN excluded.policy_name ELSE ticket_operations.policy_name END,"
        "sla_timezone=CASE WHEN ?21 THEN excluded.sla_timezone ELSE ticket_operations.sla_timezone END,"
        "sla_weekdays=CASE WHEN ?21 THEN excluded.sla_weekdays ELSE ticket_operations.sla_weekdays END,"
        "sla_day_start=CASE WHEN ?21 THEN excluded.sla_day_start ELSE ticket_operations.sla_day_start END,"
        "sla_day_end=CASE WHEN ?21 THEN excluded.sla_day_end ELSE ticket_operations.sla_day_end END,"
        "response_budget=CASE WHEN ?21 THEN excluded.response_budget ELSE ticket_operations.response_budget END,"
        "resolution_budget=CASE WHEN ?21 THEN excluded.resolution_budget ELSE ticket_operations.resolution_budget END");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bind_text(st, 2, f->impact);
    bind_text(st, 3, f->urgency);
    sqlite3_bind_int(st, 4, f->assessed);
    bind_text(st, 5, f->category);
    bind_text(st, 6, f->group_name);
    bind_text(st, 7, f->override_reason);
    bind_text(st, 8, f->waiting_reason);
    if (f->follow_up_at)
        sqlite3_bind_int64(st, 9, f->follow_up_at);
    else
        sqlite3_bind_null(st, 9);
    if (done)
        sqlite3_bind_int64(st, 10, now);
    else
        sqlite3_bind_null(st, 10);
    sqlite3_bind_int64(st, 11, p.id);
    bind_text(st, 12, p.name);
    bind_text(st, 13, p.zone);
    sqlite3_bind_int(st, 14, p.weekdays);
    sqlite3_bind_int(st, 15, p.start);
    sqlite3_bind_int(st, 16, p.end);
    sqlite3_bind_int(st, 17, p.response);
    sqlite3_bind_int(st, 18, p.resolution);
    sqlite3_bind_int64(st, 19, started);
    sqlite3_bind_int(st, 20, old_done && !done);
    sqlite3_bind_int(st, 21, policy_changed);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;
    if (wait && !old_wait) {
        st = prepare("INSERT INTO ticket_wait_intervals(ticket_id,started_at) VALUES(?1,?2)");
        if (!st)
            return false;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, now);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (!wait && old_wait) {
        st = prepare("UPDATE ticket_wait_intervals SET ended_at=?2 WHERE ticket_id=?1 AND ended_at IS NULL");
        if (!st)
            return false;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, now);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (!ok)
        return false;
    char detail[4096];
    snprintf(detail, sizeof(detail), "Impatto %s; urgenza %s; priorità proposta %s; categoria %s; gruppo %s.%s%s%s%s",
             f->impact, f->urgency, operations_priority(f->impact, f->urgency), f->category, f->group_name,
             *f->override_reason ? " Override motivato: " : "", f->override_reason,
             f->waiting_reason ? " Attesa: " : "", f->waiting_reason ? f->waiting_reason : "");
    bool changed =
        !current || strcmp(operations_string(current, "impact", ""), f->impact) ||
        strcmp(operations_string(current, "urgency", ""), f->urgency) ||
        strcmp(operations_string(current, "category", ""), f->category) ||
        strcmp(operations_string(current, "group_name", ""), f->group_name) ||
        strcmp(operations_string(current, "priority_override_reason", ""), f->override_reason) ||
        strcmp(operations_string(current, "waiting_reason", ""), f->waiting_reason ? f->waiting_reason : "") ||
        json_object_get_int64(json_object_object_get(current, "follow_up_at")) != f->follow_up_at;
    return !changed || add_history(id, a, "operations_updated", detail);
}
/* SLA snapshots remain historical internal data. A public projection is only
   safe when every linked mail source is currently visible to its audience. */
static bool operations_public_sla(json_object *ticket, Auth *a) {
    json_object *sla = NULL;
    if (!ticket || !json_object_is_type(ticket, json_type_object) ||
        !json_object_object_get_ex(ticket, "sla", &sla) || !sla)
        return true;
    int access = raw_correspondence_access(json_object_get_int64(json_object_object_get(ticket, "id")), a);
    if (access != 1)
        json_object_object_add(ticket, "sla", NULL);
    return access >= 0;
}
static void operations_redact_response(json_object *o, bool admin) {
    if (!o || !json_object_is_type(o, json_type_object))
        return;
    Auth audience = {0};
    bounded_copy(audience.role, sizeof(audience.role), admin ? "admin" : "operator");
    /* These are the canonical ticket envelopes, including create/update,
       conversion, calendar agenda and paginated list responses. Do not walk
       private draft bodies or other arbitrary user-provided JSON. */
    (void)operations_public_sla(o, &audience);
    json_object *ticket = NULL, *items = NULL;
    if (json_object_object_get_ex(o, "ticket", &ticket))
        (void)operations_public_sla(ticket, &audience);
    if (json_object_object_get_ex(o, "items", &items) && items && json_object_is_type(items, json_type_array))
        for (size_t i = 0; i < json_object_array_length(items); i++)
            (void)operations_public_sla(json_object_array_get_idx(items, i), &audience);
}
static bool operations_enrich(json_object *ticket) {
    sqlite3_int64 id = json_object_get_int64(json_object_object_get(ticket, "id")), now = (sqlite3_int64)time(NULL),
                  started = operations_epoch(operations_string(ticket, "created_at", NULL)), response_at = 0,
                  resolved_at = 0, reopened = 0;
    const char *priority = operations_string(ticket, "priority", "normal");
    OperationsPolicy p;
    if (!operations_policy(&p, json_object_get_int64(json_object_object_get(ticket, "job_id")), priority))
        return false;
    add_string(ticket, "impact", !strcmp(priority, "low") ? "low" : !strcmp(priority, "normal") ? "medium" : "high");
    add_string(ticket, "urgency", !strcmp(priority, "low") ? "low" : !strcmp(priority, "urgent") ? "high" : "medium");
    add_string(ticket, "category", "");
    add_string(ticket, "group_name", "");
    add_string(ticket, "priority_override_reason", "");
    add_string(ticket, "waiting_reason", NULL);
    json_object_object_add(ticket, "follow_up_at", NULL);
    json_object_object_add(ticket, "assessment_active", json_object_new_boolean(false));
    sqlite3_stmt *st =
        prepare("SELECT "
                "impact,urgency,assessed,category,group_name,override_reason,waiting_reason,follow_up_at,first_"
                "response_at,resolved_at,reopened_count,policy_id,policy_name,sla_timezone,sla_weekdays,sla_day_start,"
                "sla_day_end,response_budget,resolution_budget,started_at FROM ticket_operations WHERE ticket_id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *keys[] = {"impact",        "urgency", "category", "group_name", "priority_override_reason",
                              "waiting_reason"};
        int cols[] = {0, 1, 3, 4, 5, 6};
        for (size_t i = 0; i < 6; i++)
            add_string(ticket, keys[i],
                       sqlite3_column_type(st, cols[i]) == SQLITE_NULL ? NULL : column_text(st, cols[i]));
        json_object_object_add(ticket, "assessment_active", json_object_new_boolean(sqlite3_column_int(st, 2)));
        raw_nullable_int(ticket, "follow_up_at", st, 7);
        response_at = sqlite3_column_int64(st, 8);
        resolved_at = sqlite3_column_int64(st, 9);
        reopened = sqlite3_column_int64(st, 10);
        p.id = sqlite3_column_int64(st, 11);
        bounded_copy(p.name, sizeof(p.name), column_text(st, 12));
        bounded_copy(p.zone, sizeof(p.zone), column_text(st, 13));
        p.weekdays = sqlite3_column_int(st, 14);
        p.start = sqlite3_column_int(st, 15);
        p.end = sqlite3_column_int(st, 16);
        p.response = sqlite3_column_int(st, 17);
        p.resolution = sqlite3_column_int(st, 18);
        started = sqlite3_column_int64(st, 19);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return false;
    bool done = !strcmp(operations_string(ticket, "status", ""), "resolved") ||
                !strcmp(operations_string(ticket, "status", ""), "closed");
    if (done && !resolved_at)
        resolved_at = operations_epoch(operations_string(ticket, "updated_at", NULL));
    sqlite3_int64 raw_start = 0;
    st = prepare("SELECT min(m.received_at) FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id WHERE "
                 "l.ticket_id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW)
        raw_start = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_int64 raw_wait = 0;
    if (raw_start > 0 && raw_start < started) {
        raw_wait = (started - raw_start) / 60;
        started = raw_start;
    }
    sqlite3_int64 paused = 0;
    st = prepare("SELECT started_at,coalesce(ended_at,?2) FROM ticket_wait_intervals WHERE ticket_id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, resolved_at ? resolved_at : now);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        sqlite3_int64 lo = sqlite3_column_int64(st, 0), hi = sqlite3_column_int64(st, 1);
        if (hi > lo)
            paused += operations_business(&p, lo, hi, -1);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return false;
    sqlite3_int64 response_due = operations_business(&p, started, 0, p.response),
                  resolution_due = operations_business(&p, started, 0, p.resolution + paused);
    add_string(ticket, "proposed_priority",
               operations_priority(operations_string(ticket, "impact", "medium"),
                                   operations_string(ticket, "urgency", "medium")));
    json_object *sla = json_object_new_object();
    json_object_object_add(ticket, "sla", sla);
    add_string(sla, "policy_name", p.name);
    add_int(sla, "policy_id", p.id);
    add_int(sla, "started_at", started);
    add_int(sla, "response_due_at", response_due);
    add_int(sla, "resolution_due_at", resolution_due);
    if (response_at)
        add_int(sla, "first_response_at", response_at);
    else
        json_object_object_add(sla, "first_response_at", NULL);
    if (resolved_at)
        add_int(sla, "resolved_at", resolved_at);
    else
        json_object_object_add(sla, "resolved_at", NULL);
    json_object_object_add(sla, "response_breached",
                           json_object_new_boolean((response_at   ? response_at
                                                    : resolved_at ? resolved_at
                                                                  : now) > response_due));
    json_object_object_add(sla, "resolution_breached",
                           json_object_new_boolean((resolved_at ? resolved_at : now) > resolution_due));
    json_object_object_add(sla, "paused",
                           json_object_new_boolean(!strcmp(operations_string(ticket, "status", ""), "waiting")));
    add_int(sla, "paused_minutes", paused);
    add_int(sla, "raw_wait_minutes", raw_wait);
    add_int(sla, "reopened_count", reopened);
    add_string(sla, "timezone", p.zone);
    return true;
}
static json_object *operations_config_row(sqlite3_stmt *st, bool rules) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_string(o, "name", column_text(st, 1));
    raw_nullable_int(o, "job_id", st, 2);
    if (rules) {
        raw_nullable_int(o, "mailbox_id", st, 3);
        add_string(o, "sender_email", column_text(st, 4));
        add_string(o, "contains", column_text(st, 5));
        add_string(o, "category", column_text(st, 6));
        add_string(o, "group_name", column_text(st, 7));
        add_int(o, "order", sqlite3_column_int(st, 8));
        json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 9)));
        add_int(o, "revision", sqlite3_column_int64(st, 10));
        add_string(o, "job_code", column_text(st, 11));
        add_string(o, "job_name", column_text(st, 12));
    } else {
        add_string(o, "priority", column_text(st, 3));
        add_int(o, "response_minutes", sqlite3_column_int(st, 4));
        add_int(o, "resolution_minutes", sqlite3_column_int(st, 5));
        add_string(o, "timezone", column_text(st, 6));
        json_object *days = json_object_new_array();
        for (int i = 1; i <= 7; i++)
            if (sqlite3_column_int(st, 7) & (1 << (i % 7)))
                json_object_array_add(days, json_object_new_int(i));
        json_object_object_add(o, "weekdays", days);
        char value[32];
        int start = sqlite3_column_int(st, 8), end = sqlite3_column_int(st, 9);
        snprintf(value, sizeof(value), "%02d:%02d", start / 60, start % 60);
        add_string(o, "day_start", value);
        snprintf(value, sizeof(value), "%02d:%02d", end / 60, end % 60);
        add_string(o, "day_end", value);
        json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 10)));
        add_int(o, "revision", sqlite3_column_int64(st, 11));
    }
    return o;
}
#define OPS_RULE_SELECT                                                                                                \
    "SELECT "                                                                                                          \
    "id,name,job_id,mailbox_id,sender_email,contains_text,category,group_name,sort_order,active,revision,job_code,"    \
    "job_name FROM operations_rules "
#define OPS_POLICY_SELECT                                                                                              \
    "SELECT "                                                                                                          \
    "id,name,job_id,priority,response_minutes,resolution_minutes,timezone,weekdays,day_start,day_end,active,revision " \
    "FROM sla_policies "
#define OPS_CONFIG_ACL(table)                                                                                          \
    "(" table ".job_id IS NULL OR ((SELECT connected FROM punto_access)=1 AND ((SELECT all_jobs FROM punto_access)=1 " \
    "OR EXISTS(SELECT 1 FROM punto_visible_jobs p WHERE p.job_id=" table ".job_id))))"
#define OPS_RULE_ACL                                                                                                   \
    OPS_CONFIG_ACL("operations_rules") " AND (operations_rules.mailbox_id IS NULL OR EXISTS(SELECT 1 FROM "          \
    "raw_mailboxes opbox WHERE opbox.id=operations_rules.mailbox_id AND " RAW_JOB_VISIBLE_SQL("opbox.job_id") "))"
static bool operations_admin(Request *r, Auth *a) {
    if (raw_admin(a))
        return true;
    error_response(r, 403, "Sono richiesti i permessi di amministratore.");
    return false;
}
static json_object *operations_config_fetch(sqlite3_int64 id, bool rules) {
    sqlite3_stmt *st = prepare(rules ? OPS_RULE_SELECT "WHERE id=?1" : OPS_POLICY_SELECT "WHERE id=?1");
    if (!st)
        return NULL;
    sqlite3_bind_int64(st, 1, id);
    json_object *o = sqlite3_step(st) == SQLITE_ROW ? operations_config_row(st, rules) : NULL;
    sqlite3_finalize(st);
    return o;
}
static bool operations_config_visible(sqlite3_int64 id, bool rules) {
    sqlite3_stmt *st = prepare(rules ? "SELECT 1 FROM operations_rules WHERE id=?1 AND " OPS_RULE_ACL
                                    : "SELECT 1 FROM sla_policies WHERE id=?1 AND " OPS_CONFIG_ACL("sla_policies"));
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool visible = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return visible;
}
static bool operations_mailbox_visible(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare("SELECT 1 FROM raw_mailboxes WHERE id=?1 AND " RAW_JOB_VISIBLE_SQL("raw_mailboxes.job_id"));
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool visible = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return visible;
}
static void operations_config_list(Request *r, Auth *a, bool rules) {
    if (!raw_empty_query(r) || !operations_admin(r, a) || !raw_scope(r, a))
        return;
    sqlite3_stmt *st =
        prepare(rules ? OPS_RULE_SELECT "WHERE " OPS_RULE_ACL " ORDER BY sort_order,id"
                      : OPS_POLICY_SELECT "WHERE " OPS_CONFIG_ACL("sla_policies") " ORDER BY job_id,priority,id");
    if (!st) {
        db_error(r);
        return;
    }
    json_object *o = json_object_new_object(), *items = json_object_new_array();
    json_object_object_add(o, "items", items);
    int rc, count = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && count++ < 1000)
        json_object_array_add(items, operations_config_row(st, rules));
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_object_put(o);
        error_response(r, 503, "Troppe configurazioni. Riduci le regole attive.");
        return;
    }
    send_json(r, 200, o);
}
static bool operations_number(json_object *input, const char *key, sqlite3_int64 *value, sqlite3_int64 min,
                              sqlite3_int64 max) {
    json_object *v = NULL;
    if (!json_object_object_get_ex(input, key, &v))
        return true;
    if (!v || !json_object_is_type(v, json_type_int))
        return false;
    *value = json_object_get_int64(v);
    return *value >= min && *value <= max;
}
static bool operations_hhmm(const char *v, int *minute) {
    if (!v || strlen(v) != 5 || v[2] != ':' || !isdigit((unsigned char)v[0]) || !isdigit((unsigned char)v[1]) ||
        !isdigit((unsigned char)v[3]) || !isdigit((unsigned char)v[4]))
        return false;
    int hour = (v[0] - '0') * 10 + v[1] - '0', min = (v[3] - '0') * 10 + v[4] - '0';
    if (hour > 24 || min > 59 || (hour == 24 && min))
        return false;
    *minute = hour * 60 + min;
    return true;
}
static void operations_config_save(Request *r, Auth *a, sqlite3_int64 id, bool rules) {
    if (!raw_empty_query(r) || !operations_admin(r, a) || !raw_scope(r, a))
        return;
    if (id && !operations_config_visible(id, rules)) {
        error_response(r, 404, "Configurazione non trovata.");
        return;
    }
    json_object *input = parse_json(r), *old = id ? operations_config_fetch(id, rules) : NULL, *refs = NULL;
    if (!input)
        return;
    if (id && !old) {
        error_response(r, 404, "Configurazione non trovata.");
        goto done;
    }
    const char *const rule_keys[] = {"name",       "job_id", "mailbox_id", "sender_email", "contains", "category",
                                     "group_name", "order",  "active",     "revision",     NULL};
    const char *const policy_keys[] = {"name",     "job_id",   "priority",  "response_minutes", "resolution_minutes",
                                       "timezone", "weekdays", "day_start", "day_end",          "active",
                                       "revision", NULL};
    sqlite3_int64 revision = 0, job = old ? json_object_get_int64(json_object_object_get(old, "job_id")) : 0;
    bool active = old ? json_object_get_boolean(json_object_object_get(old, "active")) : true;
    if (!allowed_keys(input, rules ? rule_keys : policy_keys) || !raw_optional_id(input, "job_id", &job) ||
        !raw_optional_bool(input, "active", &active) || (id && !punto_integer(input, "revision", 1, &revision)))
        goto invalid;
    if (job && !punto_validate_refs(r, a, job, 0, &refs))
        goto done;
    const char *name = operations_string(old, "name", "");
    if (!get_string(input, "name", &name, 100, !id, true))
        goto invalid;
    sqlite3_stmt *st = NULL;
    if (rules) {
        sqlite3_int64 mailbox = old ? json_object_get_int64(json_object_object_get(old, "mailbox_id")) : 0,
                      order = old ? json_object_get_int64(json_object_object_get(old, "order")) : 100;
        const char *sender = operations_string(old, "sender_email", ""),
                   *contains = operations_string(old, "contains", ""),
                   *category = operations_string(old, "category", ""),
                   *group = operations_string(old, "group_name", "");
        char normalized[255] = "";
        if (!raw_optional_id(input, "mailbox_id", &mailbox) || !operations_number(input, "order", &order, 0, 100000) ||
            !get_string(input, "sender_email", &sender, 254, false, false) ||
            !get_string(input, "contains", &contains, 1000, false, false) ||
            !get_string(input, "category", &category, 80, false, false) ||
            !get_string(input, "group_name", &group, 80, false, false) ||
            (*sender && !email_normalize(sender, normalized)) || (!*category && !*group && !job))
            goto invalid;
        if (mailbox) {
            json_object *box = raw_mailbox_fetch(mailbox);
            if (!box)
                goto invalid;
            json_object_put(box);
            if (!operations_mailbox_visible(mailbox)) {
                error_response(r, 404, "Casella non trovata.");
                goto done;
            }
        }
        st = prepare(
            id ? "UPDATE operations_rules SET "
                 "name=?1,job_id=?2,mailbox_id=?3,sender_email=?4,contains_text=?5,category=?6,group_name=?7,sort_"
                 "order=?8,active=?9,job_code=?10,job_name=?11,revision=revision+1 WHERE id=?12 AND revision=?13"
               : "INSERT INTO "
                 "operations_rules(name,job_id,mailbox_id,sender_email,contains_text,category,group_name,sort_order,"
                 "active,job_code,job_name) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
        if (!st)
            goto failed;
        bind_text(st, 1, name);
        if (job)
            sqlite3_bind_int64(st, 2, job);
        else
            sqlite3_bind_null(st, 2);
        if (mailbox)
            sqlite3_bind_int64(st, 3, mailbox);
        else
            sqlite3_bind_null(st, 3);
        bind_text(st, 4, normalized);
        bind_text(st, 5, contains);
        bind_text(st, 6, category);
        bind_text(st, 7, group);
        sqlite3_bind_int64(st, 8, order);
        sqlite3_bind_int(st, 9, active);
        json_object *j = refs ? json_object_object_get(refs, "job") : NULL;
        bind_text(st, 10, j ? operations_string(j, "code", NULL) : NULL);
        bind_text(st, 11, j ? operations_string(j, "name", NULL) : NULL);
        sqlite3_bind_int64(st, 12, id);
        sqlite3_bind_int64(st, 13, revision);
    } else {
        const char *priority = operations_string(old, "priority", "all"),
                   *zone = operations_string(old, "timezone", "Europe/Rome"),
                   *start = operations_string(old, "day_start", "09:00"),
                   *end = operations_string(old, "day_end", "18:00");
        sqlite3_int64 response = old ? json_object_get_int64(json_object_object_get(old, "response_minutes")) : 240,
                      resolution = old ? json_object_get_int64(json_object_object_get(old, "resolution_minutes")) : 960;
        int start_min, end_min, days = 62;
        if (!get_string(input, "priority", &priority, 16, false, true) ||
            (!valid_priority(priority) && strcmp(priority, "all")) ||
            !get_string(input, "timezone", &zone, 100, false, true) || !schedule_zone(zone) ||
            !get_string(input, "day_start", &start, 5, false, true) ||
            !get_string(input, "day_end", &end, 5, false, true) || !operations_hhmm(start, &start_min) ||
            !operations_hhmm(end, &end_min) || start_min >= end_min ||
            !operations_number(input, "response_minutes", &response, 1, 525600) ||
            !operations_number(input, "resolution_minutes", &resolution, 1, 525600))
            goto invalid;
        json_object *week = NULL;
        if (!json_object_object_get_ex(input, "weekdays", &week))
            week = old ? json_object_object_get(old, "weekdays") : NULL;
        if (week) {
            if (!json_object_is_type(week, json_type_array) || json_object_array_length(week) < 1 ||
                json_object_array_length(week) > 7)
                goto invalid;
            days = 0;
            for (size_t i = 0; i < json_object_array_length(week); i++) {
                json_object *v = json_object_array_get_idx(week, i);
                if (!v || !json_object_is_type(v, json_type_int))
                    goto invalid;
                int day = json_object_get_int(v);
                if (day < 1 || day > 7 || (days & (1 << (day % 7))))
                    goto invalid;
                days |= 1 << (day % 7);
            }
        }
        st =
            prepare(id ? "UPDATE sla_policies SET "
                         "name=?1,job_id=?2,priority=?3,response_minutes=?4,resolution_minutes=?5,timezone=?6,weekdays="
                         "?7,day_start=?8,day_end=?9,active=?10,revision=revision+1 WHERE id=?11 AND revision=?12"
                       : "INSERT INTO "
                         "sla_policies(name,job_id,priority,response_minutes,resolution_minutes,timezone,weekdays,day_"
                         "start,day_end,active) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        if (!st)
            goto failed;
        bind_text(st, 1, name);
        if (job)
            sqlite3_bind_int64(st, 2, job);
        else
            sqlite3_bind_null(st, 2);
        bind_text(st, 3, priority);
        sqlite3_bind_int64(st, 4, response);
        sqlite3_bind_int64(st, 5, resolution);
        bind_text(st, 6, zone);
        sqlite3_bind_int(st, 7, days);
        sqlite3_bind_int(st, 8, start_min);
        sqlite3_bind_int(st, 9, end_min);
        sqlite3_bind_int(st, 10, active);
        sqlite3_bind_int64(st, 11, id);
        sqlite3_bind_int64(st, 12, revision);
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    if (id && sqlite3_changes(db) != 1) {
        error_response(r, 409, "Configurazione modificata. Ricarica prima di salvare.");
        goto done;
    }
    bool created = !id;
    if (created)
        id = sqlite3_last_insert_rowid(db);
    json_object *o = json_object_new_object();
    json_object_object_add(o, rules ? "rule" : "policy", operations_config_fetch(id, rules));
    send_json(r, created ? 201 : 200, o);
    goto done;
invalid:
    error_response(r, 400, "Configurazione operativa non valida.");
    goto done;
failed:
    db_error(r);
done:
    if (refs)
        json_object_put(refs);
    if (old)
        json_object_put(old);
    json_object_put(input);
}
static void operations_suggestions(Request *r, Auth *a, sqlite3_int64 raw_id) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *input = raw_id ? raw_fetch(raw_id, a, true) : parse_json(r);
    if (!input) {
        if (raw_id)
            error_response(r, 404, "Messaggio non trovato.");
        return;
    }
    const char *title = raw_id ? operations_string(input, "subject", "") : "",
               *body = raw_id ? operations_string(input, "body_text", "") : "",
               *sender = raw_id ? operations_string(input, "from_email", "") : "", *impact = "medium",
               *urgency = "medium", *requester = "";
    sqlite3_int64 mailbox = raw_id ? json_object_get_int64(json_object_object_get(input, "mailbox_id")) : 0;
    if (!raw_id) {
        const char *const keys[] = {"title",        "description", "requester", "mailbox_id",
                                    "sender_email", "impact",      "urgency",   NULL};
        if (!allowed_keys(input, keys) || !get_string(input, "title", &title, 160, false, false) ||
            !get_string(input, "description", &body, 8000, false, false) ||
            !get_string(input, "requester", &requester, 200, false, false) ||
            !get_string(input, "sender_email", &sender, 254, false, false) ||
            !get_string(input, "impact", &impact, 16, false, true) ||
            !get_string(input, "urgency", &urgency, 16, false, true) || !operations_level(impact) ||
            !operations_level(urgency) || !raw_optional_id(input, "mailbox_id", &mailbox)) {
            json_object_put(input);
            error_response(r, 400, "Dati per i suggerimenti non validi.");
            return;
        }
    }
    sqlite3_stmt *st = prepare(OPS_RULE_SELECT "WHERE active=1 AND " OPS_RULE_ACL
                             " AND (mailbox_id IS NULL OR mailbox_id=?1) AND (sender_email='' OR sender_email=?2 "
                            "COLLATE NOCASE) AND (contains_text='' OR instr(lower(?3),lower(contains_text))>0 OR "
                            "instr(lower(?4),lower(contains_text))>0 OR instr(lower(?5),lower(contains_text))>0) ORDER "
                            "BY sort_order,id LIMIT 20");
    if (!st) {
        json_object_put(input);
        db_error(r);
        return;
    }
    sqlite3_bind_int64(st, 1, mailbox);
    bind_text(st, 2, sender);
    bind_text(st, 3, title);
    bind_text(st, 4, body);
    bind_text(st, 5, requester);
    json_object *o = json_object_new_object(), *matches = json_object_new_array();
    json_object_object_add(o, "matches", matches);
    json_object_object_add(o, "suggestion", NULL);
    add_string(o, "proposed_priority", operations_priority(impact, urgency));
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json_object *row = operations_config_row(st, true);
        add_int(row, "rule_id", sqlite3_column_int64(st, 0));
        json_object_array_add(matches, row);
        if (json_object_array_length(matches) == 1) {
            json_object *suggestion = json_object_new_object();
            add_string(suggestion, "category", column_text(st, 6));
            add_string(suggestion, "group_name", column_text(st, 7));
            raw_nullable_int(suggestion, "job_id", st, 2);
            json_object_object_add(o, "suggestion", suggestion);
        }
    }
    sqlite3_finalize(st);
    json_object_put(input);
    if (rc != SQLITE_DONE) {
        json_object_put(o);
        db_error(r);
        return;
    }
    send_json(r, 200, o);
}
static bool operations_view_filters(json_object *filters) {
    if (!filters || !json_object_is_type(filters, json_type_object))
        return false;
    char query[8192] = "";
    size_t used = 0;
    const char hex[] = "0123456789ABCDEF";
    json_object_object_foreach(filters, key, value) {
        if (!value || (!json_object_is_type(value, json_type_string) && !json_object_is_type(value, json_type_int)))
            return false;
        const char *text = json_object_get_string(value);
        if (json_object_is_type(value, json_type_string) && strlen(text) != (size_t)json_object_get_string_len(value))
            return false;
        if (used) {
            if (used + 1 >= sizeof(query))
                return false;
            query[used++] = '&';
        }
        for (int part = 0; part < 2; part++) {
            const unsigned char *p = (const unsigned char *)(part ? text : key);
            for (; *p; p++) {
                if (used + 3 >= sizeof(query))
                    return false;
                if (isalnum(*p) || strchr("_-.", *p))
                    query[used++] = (char)*p;
                else {
                    query[used++] = '%';
                    query[used++] = hex[*p >> 4];
                    query[used++] = hex[*p & 15];
                }
            }
            if (!part)
                query[used++] = '=';
        }
    }
    query[used] = 0;
    Filters parsed;
    return read_filters(query, &parsed);
}
static json_object *operations_view_row(sqlite3_stmt *st) {
    json_object *o = json_object_new_object();
    add_int(o, "id", sqlite3_column_int64(st, 0));
    add_string(o, "name", column_text(st, 1));
    json_object *f = json_tokener_parse(column_text(st, 2));
    json_object_object_add(o, "filters", f ? f : json_object_new_object());
    add_string(o, "view", column_text(st, 3));
    add_int(o, "owner_id", sqlite3_column_int64(st, 4));
    add_int(o, "revision", sqlite3_column_int64(st, 5));
    json_object_object_add(o, "active", json_object_new_boolean(sqlite3_column_int(st, 6)));
    return o;
}
static void operations_views(Request *r, Auth *a, sqlite3_int64 id, bool get) {
    if (!raw_empty_query(r))
        return;
    if (get) {
        sqlite3_stmt *st = prepare("SELECT id,name,filters,view,owner_id,revision,active FROM operations_views WHERE "
                                   "active=1 ORDER BY name,id LIMIT 1001");
        if (!st) {
            db_error(r);
            return;
        }
        json_object *o = json_object_new_object(), *items = json_object_new_array();
        json_object_object_add(o, "items", items);
        int rc, count = 0;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW && count++ < 1000)
            json_object_array_add(items, operations_view_row(st));
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            json_object_put(o);
            error_response(r, 503, "Troppe viste salvate.");
            return;
        }
        send_json(r, 200, o);
        return;
    }
    json_object *input = parse_json(r), *old = NULL;
    if (!input)
        return;
    if (id) {
        sqlite3_stmt *st =
            prepare("SELECT id,name,filters,view,owner_id,revision,active FROM operations_views WHERE id=?1");
        if (!st)
            goto failed;
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW)
            old = operations_view_row(st);
        sqlite3_finalize(st);
        if (!old) {
            error_response(r, 404, "Vista non trovata.");
            goto done;
        }
        if (!raw_admin(a) && json_object_get_int64(json_object_object_get(old, "owner_id")) != a->id) {
            error_response(r, 403, "Puoi modificare le tue viste condivise.");
            goto done;
        }
    }
    const char *const keys[] = {"name", "filters", "view", "revision", "active", NULL};
    const char *name = operations_string(old, "name", ""), *view = operations_string(old, "view", "list");
    sqlite3_int64 revision = 0;
    bool active = old ? json_object_get_boolean(json_object_object_get(old, "active")) : true;
    json_object *filters = old ? json_object_object_get(old, "filters") : NULL, *provided = NULL;
    if (json_object_object_get_ex(input, "filters", &provided))
        filters = provided;
    if (!allowed_keys(input, keys) || !get_string(input, "name", &name, 100, !id, true) ||
        !get_string(input, "view", &view, 16, false, true) ||
        (strcmp(view, "list") && strcmp(view, "board") && strcmp(view, "agenda") && strcmp(view, "operator")) ||
        !operations_view_filters(filters) || !raw_optional_bool(input, "active", &active) ||
        (id && !punto_integer(input, "revision", 1, &revision)))
        goto invalid;
    sqlite3_stmt *st =
        prepare(id ? "UPDATE operations_views SET name=?1,filters=?2,view=?3,active=?4,revision=revision+1 WHERE id=?5 "
                     "AND revision=?6"
                   : "INSERT INTO operations_views(name,filters,view,active,owner_id) VALUES(?1,?2,?3,?4,?5)");
    if (!st)
        goto failed;
    bind_text(st, 1, name);
    bind_text(st, 2, json_object_to_json_string_ext(filters, JSON_C_TO_STRING_PLAIN));
    bind_text(st, 3, view);
    sqlite3_bind_int(st, 4, active);
    sqlite3_bind_int64(st, 5, id ? id : a->id);
    sqlite3_bind_int64(st, 6, revision);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    if (id && sqlite3_changes(db) != 1) {
        error_response(r, 409, "Vista modificata. Ricaricala.");
        goto done;
    }
    bool created = !id;
    if (created)
        id = sqlite3_last_insert_rowid(db);
    json_object *o = json_object_new_object(), *v = json_object_new_object();
    add_int(v, "id", id);
    add_string(v, "name", name);
    add_string(v, "view", view);
    json_object_object_add(v, "filters", json_object_get(filters));
    add_int(v, "revision", created ? 1 : revision + 1);
    add_int(v, "owner_id", old ? json_object_get_int64(json_object_object_get(old, "owner_id")) : a->id);
    json_object_object_add(v, "active", json_object_new_boolean(active));
    json_object_object_add(o, "view", v);
    send_json(r, created ? 201 : 200, o);
    goto done;
invalid:
    error_response(r, 400, "Vista o filtri non validi.");
    goto done;
failed:
    db_error(r);
done:
    if (old)
        json_object_put(old);
    json_object_put(input);
}
static bool operations_access_loaded(sqlite3_int64 id) {
    sqlite3_stmt *st = prepare("SELECT 1 FROM tickets t WHERE t.id=?1 AND " PUNTO_VISIBLE_SQL);
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}
static json_object *operations_draft(sqlite3_int64 id, Auth *a) {
    sqlite3_stmt *st =
        prepare("SELECT revision,base_revision,body,updated_at FROM ticket_drafts WHERE ticket_id=?1 AND user_id=?2");
    if (!st)
        return NULL;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, a->id);
    json_object *o = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        o = json_object_new_object();
        add_int(o, "revision", sqlite3_column_int64(st, 0));
        add_int(o, "base_revision", sqlite3_column_int64(st, 1));
        json_object *body = json_tokener_parse(column_text(st, 2));
        json_object_object_add(o, "body", body ? body : json_object_new_object());
        add_int(o, "updated_at", sqlite3_column_int64(st, 3));
    }
    sqlite3_finalize(st);
    return o;
}
static void operations_presence(Request *r, Auth *a, sqlite3_int64 id, bool post) {
    if (!raw_empty_query(r))
        return;
    json_object *ticket = fetch_ticket(id);
    if (!ticket) {
        error_response(r, 404, "Ticket non trovato.");
        return;
    }
    bool access = punto_ticket_access(r, a, ticket);
    json_object_put(ticket);
    if (!access)
        return;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    if (post) {
        json_object *input = parse_json(r);
        if (!input)
            return;
        const char *const keys[] = {"editing", NULL};
        bool editing = false;
        json_object *v = NULL;
        if (!allowed_keys(input, keys) || !json_object_object_get_ex(input, "editing", &v) ||
            !raw_optional_bool(input, "editing", &editing)) {
            json_object_put(input);
            error_response(r, 400, "Presenza non valida.");
            return;
        }
        json_object_put(input);
        sqlite3_stmt *st = prepare(
            "INSERT INTO ticket_presence(ticket_id,user_id,token_hash,editing,last_seen) VALUES(?1,?2,?3,?4,?5) ON "
            "CONFLICT(ticket_id,token_hash) DO UPDATE SET editing=excluded.editing,last_seen=excluded.last_seen");
        if (!st) {
            db_error(r);
            return;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, a->id);
        bind_text(st, 3, a->token_hash);
        sqlite3_bind_int(st, 4, editing);
        sqlite3_bind_int64(st, 5, now);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            db_error(r);
            return;
        }
    }
    sqlite3_stmt *st =
        prepare("SELECT p.user_id,u.name,max(p.editing),max(p.last_seen) FROM ticket_presence p JOIN users u ON "
                "u.id=p.user_id JOIN sessions s ON s.token_hash=p.token_hash WHERE p.ticket_id=?1 AND p.last_seen>?2 "
                "AND s.expires_at>?3 AND u.active=1 GROUP BY p.user_id,u.name ORDER BY p.user_id");
    if (!st) {
        db_error(r);
        return;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, now - 90);
    sqlite3_bind_int64(st, 3, now);
    json_object *o = json_object_new_object(), *items = json_object_new_array();
    json_object_object_add(o, "items", items);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        json_object *row = json_object_new_object();
        add_int(row, "user_id", sqlite3_column_int64(st, 0));
        add_string(row, "name", column_text(st, 1));
        json_object_object_add(row, "editing", json_object_new_boolean(sqlite3_column_int(st, 2)));
        add_int(row, "last_seen", sqlite3_column_int64(st, 3));
        add_int(row, "expires_at", sqlite3_column_int64(st, 3) + 90);
        json_object_array_add(items, row);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        json_object_put(o);
        db_error(r);
        return;
    }
    json_object_object_add(o, "draft", operations_draft(id, a));
    send_json(r, 200, o);
}
static void operations_save_draft(Request *r, Auth *a, sqlite3_int64 id, bool discard) {
    if (!raw_empty_query(r))
        return;
    json_object *ticket = fetch_ticket(id);
    if (!ticket) {
        error_response(r, 404, "Ticket non trovato.");
        return;
    }
    if (!punto_ticket_access(r, a, ticket)) {
        json_object_put(ticket);
        return;
    }
    json_object *input = parse_json(r);
    if (!input) {
        json_object_put(ticket);
        return;
    }
    const char *const keys[] = {"revision", "base_revision", "body", NULL}, *const discard_keys[] = {"revision", NULL};
    sqlite3_int64 revision = 0, base = 0;
    json_object *body = NULL;
    bool ok = allowed_keys(input, discard ? discard_keys : keys) && json_object_object_get(input, "revision") &&
              operations_number(input, "revision", &revision, 0, LLONG_MAX - 1);
    if (!discard) {
        ok = ok && punto_integer(input, "base_revision", 1, &base) && json_object_object_get_ex(input, "body", &body) &&
             body && json_object_is_type(body, json_type_object) &&
             strlen(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN)) <= 16384;
        if (ok) {
            const char *const draft_keys[] = {"title",
                                              "description",
                                              "requester",
                                              "priority",
                                              "assignee_id",
                                              "due_date",
                                              "job_id",
                                              "asset_id",
                                              "scheduled_start",
                                              "scheduled_end",
                                              "scheduled_timezone",
                                              "impact",
                                              "urgency",
                                              "category",
                                              "group_name",
                                              "priority_override_reason",
                                              "waiting_reason",
                                              "follow_up_at",
                                              "status",
                                              NULL};
            ok = allowed_keys(body, draft_keys) &&
                 base <= json_object_get_int64(json_object_object_get(ticket, "revision"));
        }
    }
    json_object_put(ticket);
    if (!ok) {
        json_object_put(input);
        error_response(r, 400, "Bozza non valida.");
        return;
    }
    sqlite3_stmt *st = NULL;
    if (discard) {
        st = prepare("DELETE FROM ticket_drafts WHERE ticket_id=?1 AND user_id=?2 AND revision=?3");
        if (!st)
            goto failed;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, a->id);
        sqlite3_bind_int64(st, 3, revision);
    } else {
        st = prepare(revision ? "UPDATE ticket_drafts SET revision=revision+1,base_revision=?3,body=?4,updated_at=?5 "
                                "WHERE ticket_id=?1 AND user_id=?2 AND revision=?6"
                              : "INSERT INTO ticket_drafts(ticket_id,user_id,revision,base_revision,body,updated_at) "
                                "VALUES(?1,?2,1,?3,?4,?5) ON CONFLICT(ticket_id,user_id) DO NOTHING");
        if (!st)
            goto failed;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, a->id);
        sqlite3_bind_int64(st, 3, base);
        bind_text(st, 4, json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
        sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
        if (revision)
            sqlite3_bind_int64(st, 6, revision);
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    if (sqlite3_changes(db) != 1) {
        json_object_put(input);
        error_response(r, 409, "La bozza è stata modificata in un'altra scheda. Ricaricala.");
        return;
    }
    json_object_put(input);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "draft", discard ? NULL : operations_draft(id, a));
    send_json(r, 200, o);
    return;
failed:
    json_object_put(input);
    db_error(r);
}
static void operations_exclusion(json_object *items, sqlite3_int64 id, const char *reason) {
    json_object *o = json_object_new_object();
    add_int(o, "id", id);
    add_string(o, "reason", reason);
    json_object_array_add(items, o);
}
static bool operations_bulk_changes(json_object *changes) {
    const char *const keys[] = {
        "status",   "assignee_id", "priority", "priority_override_reason", "waiting_reason", "follow_up_at",
        "category", "group_name",  NULL};
    return changes && json_object_is_type(changes, json_type_object) && json_object_object_length(changes) > 0 &&
           allowed_keys(changes, keys);
}
static const char *operations_bulk_eligible(json_object *ticket, json_object *changes, sqlite3_int64 revision,
                                            TicketFields *f) {
    if (!ticket)
        return "not_found";
    if (json_object_get_int64(json_object_object_get(ticket, "revision")) != revision)
        return "revision_changed";
    if (!ticket_fields(changes, ticket, f, true))
        return "invalid_change";
    sqlite3_int64 old = json_object_get_int64(json_object_object_get(ticket, "assignee_id"));
    if (old != f->assignee_id && !assignee_active(f->assignee_id))
        return "inactive_assignee";
    return NULL;
}
static void operations_bulk_preview(Request *r, Auth *a) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *input = parse_json(r);
    if (!input)
        return;
    const char *const keys[] = {"tickets", "changes", NULL};
    json_object *targets = NULL, *changes = NULL;
    if (!allowed_keys(input, keys) || !json_object_object_get_ex(input, "tickets", &targets) || !targets ||
        !json_object_is_type(targets, json_type_array) || json_object_array_length(targets) < 1 ||
        json_object_array_length(targets) > 100 || !json_object_object_get_ex(input, "changes", &changes) ||
        !operations_bulk_changes(changes)) {
        json_object_put(input);
        error_response(r, 400, "Selezione multipla o modifiche non valide (massimo 100 ticket).");
        return;
    }
    sqlite3_int64 ids[100];
    json_object *o = json_object_new_object(), *eligible = json_object_new_array(), *excluded = json_object_new_array();
    json_object_object_add(o, "eligible", eligible);
    json_object_object_add(o, "excluded", excluded);
    json_object_object_add(o, "changes", json_object_get(changes));
    for (size_t i = 0; i < json_object_array_length(targets); i++) {
        json_object *row = json_object_array_get_idx(targets, i);
        const char *const rowkeys[] = {"id", "revision", NULL};
        sqlite3_int64 id, revision;
        if (!row || !json_object_is_type(row, json_type_object) || !allowed_keys(row, rowkeys) ||
            !punto_integer(row, "id", 1, &id) || !punto_integer(row, "revision", 1, &revision))
            goto invalid;
        for (size_t j = 0; j < i; j++)
            if (ids[j] == id)
                goto invalid;
        ids[i] = id;
        if (!operations_access_loaded(id)) {
            operations_exclusion(excluded, id, "not_found");
            continue;
        }
        json_object *ticket = fetch_ticket(id);
        TicketFields f = {0};
        const char *reason = operations_bulk_eligible(ticket, changes, revision, &f);
        if (reason)
            operations_exclusion(excluded, id, reason);
        else {
            json_object *item = json_object_new_object();
            add_int(item, "id", id);
            add_int(item, "revision", revision);
            add_string(item, "title", operations_string(ticket, "title", ""));
            json_object_array_add(eligible, item);
        }
        if (ticket)
            json_object_put(ticket);
    }
    char token[65];
    sqlite3_int64 expires = (sqlite3_int64)time(NULL) + 300;
    if (!random_hex(token, 32))
        goto failed;
    sqlite3_stmt *st = prepare("INSERT INTO bulk_previews(token,token_hash,expires_at,body) VALUES(?1,?2,?3,?4)");
    if (!st)
        goto failed;
    bind_text(st, 1, token);
    bind_text(st, 2, a->token_hash);
    sqlite3_bind_int64(st, 3, expires);
    bind_text(st, 4, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    add_string(o, "token", token);
    add_int(o, "expires_at", expires);
    json_object_put(input);
    send_json(r, 200, o);
    return;
invalid:
    json_object_put(o);
    json_object_put(input);
    error_response(r, 400, "Ticket duplicati o revisioni non valide.");
    return;
failed:
    json_object_put(o);
    json_object_put(input);
    db_error(r);
}
static bool operations_bulk_update(sqlite3_int64 id, sqlite3_int64 revision, TicketFields *f) {
    sqlite3_stmt *st =
        prepare("UPDATE tickets SET "
                "title=?1,description=?2,requester=?3,status=?4,priority=?5,assignee_id=?6,due_date=?7,updated_at="
                "strftime('%Y-%m-%dT%H:%M:%SZ','now'),revision=revision+1 WHERE id=?8 AND revision=?9");
    if (!st)
        return false;
    bind_ticket(st, f);
    sqlite3_bind_int64(st, 8, id);
    sqlite3_bind_int64(st, 9, revision);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}
static void operations_bulk_apply(Request *r, Auth *a) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *input = parse_json(r), *snapshot = NULL, *o = NULL;
    if (!input)
        return;
    const char *const keys[] = {"token", NULL};
    const char *token = "";
    if (!allowed_keys(input, keys) || !get_string(input, "token", &token, 64, true, true) || !punto_hex(token)) {
        json_object_put(input);
        error_response(r, 400, "Conferma non valida.");
        return;
    }
    if (!exec_sql("BEGIN IMMEDIATE"))
        goto failed;
    sqlite3_stmt *st =
        prepare("SELECT body,result FROM bulk_previews WHERE token=?1 AND token_hash=?2 AND expires_at>?3");
    if (!st)
        goto rollback;
    bind_text(st, 1, token);
    bind_text(st, 2, a->token_hash);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        snapshot = json_tokener_parse(column_text(st, 0));
        if (sqlite3_column_type(st, 1) != SQLITE_NULL)
            o = json_tokener_parse(column_text(st, 1));
    }
    sqlite3_finalize(st);
    if (!snapshot) {
        exec_sql("ROLLBACK");
        json_object_put(input);
        error_response(r, 409, "Anteprima scaduta o appartenente a un'altra sessione. Preparala nuovamente.");
        return;
    }
    if (o) {
        json_object_object_add(o, "replayed", json_object_new_boolean(true));
        if (!exec_sql("COMMIT"))
            goto rollback;
        json_object_put(snapshot);
        json_object_put(input);
        send_json(r, 200, o);
        return;
    }
    o = json_object_new_object();
    json_object *updated = json_object_new_array(),
                *excluded = json_object_get(json_object_object_get(snapshot, "excluded"));
    json_object_object_add(o, "updated", updated);
    json_object_object_add(o, "excluded", excluded);
    json_object_object_add(o, "replayed", json_object_new_boolean(false));
    json_object *targets = json_object_object_get(snapshot, "eligible"),
                *changes = json_object_object_get(snapshot, "changes");
    for (size_t i = 0; i < json_object_array_length(targets); i++) {
        json_object *target = json_object_array_get_idx(targets, i);
        sqlite3_int64 id = json_object_get_int64(json_object_object_get(target, "id")),
                      revision = json_object_get_int64(json_object_object_get(target, "revision"));
        if (!operations_access_loaded(id)) {
            operations_exclusion(excluded, id, "not_found");
            continue;
        }
        json_object *ticket = fetch_ticket(id);
        TicketFields f = {0};
        const char *reason = operations_bulk_eligible(ticket, changes, revision, &f);
        if (reason) {
            operations_exclusion(excluded, id, reason);
            if (ticket)
                json_object_put(ticket);
            continue;
        }
        bool ok = operations_bulk_update(id, revision, &f) && operations_apply(id, a, changes, ticket, &f) &&
                  add_history(id, a, "bulk_updated", json_object_to_json_string_ext(changes, JSON_C_TO_STRING_PLAIN));
        json_object_put(ticket);
        if (!ok)
            goto rollback;
        json_object_array_add(updated, json_object_new_int64(id));
    }
    st = prepare("UPDATE bulk_previews SET result=?2 WHERE token=?1");
    if (!st)
        goto rollback;
    bind_text(st, 1, token);
    bind_text(st, 2, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || !exec_sql("COMMIT"))
        goto rollback;
    json_object_put(snapshot);
    json_object_put(input);
    send_json(r, 200, o);
    return;
rollback:
    exec_sql("ROLLBACK");
failed:
    if (o)
        json_object_put(o);
    if (snapshot)
        json_object_put(snapshot);
    json_object_put(input);
    db_error(r);
}
static bool operations_same_day(const char *zone, sqlite3_int64 a, sqlite3_int64 b) {
    const char *before = getenv("TZ");
    char *saved = before ? strdup(before) : NULL;
    setenv("TZ", zone, 1);
    tzset();
    time_t x = (time_t)a, y = (time_t)b;
    struct tm dx, dy;
    localtime_r(&x, &dx);
    localtime_r(&y, &dy);
    bool same = dx.tm_year == dy.tm_year && dx.tm_yday == dy.tm_yday;
    if (saved) {
        setenv("TZ", saved, 1);
        free(saved);
    } else
        unsetenv("TZ");
    tzset();
    return same;
}
static bool operations_queue_match(json_object *ticket, int queue, Auth *a, sqlite3_int64 now, bool *matches) {
    *matches = false;
    const char *status = operations_string(ticket, "status", "");
    if (strcmp(status, "open") && strcmp(status, "in_progress") && strcmp(status, "waiting"))
        return true;
    json_object *sla = json_object_object_get(ticket, "sla");
    sqlite3_int64 first_response = json_object_get_int64(json_object_object_get(sla, "first_response_at"));
    switch (queue) {
    case 1: {
        sqlite3_stmt *st = prepare(
            "SELECT coalesce((SELECT max(sent_at) FROM ticket_messages WHERE ticket_id=?2 AND state='sent'),0),"
            "coalesce((SELECT max(m.received_at) FROM ticket_raw_links l JOIN raw_messages m ON "
            "m.id=l.raw_message_id WHERE l.ticket_id=?2 AND " RAW_VISIBLE_SQL "),0)");
        if (!st)
            return false;
        sqlite3_bind_int(st, 1, raw_admin(a));
        sqlite3_bind_int64(st, 2, json_object_get_int64(json_object_object_get(ticket, "id")));
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            sqlite3_int64 sent = sqlite3_column_int64(st, 0), received = sqlite3_column_int64(st, 1);
            if (first_response > sent)
                sent = first_response;
            *matches = !sent || received > sent;
        }
        sqlite3_finalize(st);
        return rc == SQLITE_ROW;
    }
    case 2:
        *matches = !strcmp(status, "waiting") &&
                   !strcmp(operations_string(ticket, "waiting_reason", ""), "customer");
        return true;
    case 3: {
        const char *zone = operations_string(sla, "timezone", "Europe/Rome");
        sqlite3_int64 response_due = json_object_get_int64(json_object_object_get(sla, "response_due_at")),
                      resolution_due = json_object_get_int64(json_object_object_get(sla, "resolution_due_at"));
        *matches = (!first_response && response_due && operations_same_day(zone, now, response_due)) ||
                   (resolution_due && operations_same_day(zone, now, resolution_due));
        return true;
    }
    case 4:
        *matches = !json_object_get_int64(json_object_object_get(ticket, "scheduled_start"));
        return true;
    case 5: {
        sqlite3_int64 follow = json_object_get_int64(json_object_object_get(ticket, "follow_up_at"));
        *matches = !strcmp(status, "waiting") && follow > 0 && follow <= now;
        return true;
    }
    default:
        *matches = true;
        return true;
    }
}
/* Evaluate timezone-aware deadlines before SQL count/offset. The temporary ID
 * set and final rows share one read snapshot and the current actor's live ACL. */
static bool operations_prepare_filter(Request *r, Filters *f, Auth *a) {
    if (!exec_sql("DELETE FROM operations_filter_tickets")) {
        db_error(r);
        return false;
    }
    if (!f->operations_queue)
        return true;
    char sql[8192];
    const char *agenda = f->agenda_from ? " AND t.scheduled_start<?15 AND t.scheduled_end>?14 " : " ";
    int length = snprintf(sql, sizeof(sql), "SELECT " TICKET_COLUMNS TICKET_FROM FILTER_SQL
                          " AND " ACTIVE_SQL "%s ORDER BY t.id LIMIT 10001", agenda);
    if (length < 0 || (size_t)length >= sizeof(sql)) {
        db_error(r);
        return false;
    }
    sqlite3_stmt *read = prepare(sql), *write = prepare("INSERT INTO operations_filter_tickets(id) VALUES(?1)");
    if (!read || !write) {
        sqlite3_finalize(read);
        sqlite3_finalize(write);
        db_error(r);
        return false;
    }
    Filters candidates = *f;
    candidates.operations_queue = 0;
    bind_filters(read, &candidates, a);
    if (f->agenda_from) {
        sqlite3_bind_int64(read, 14, f->agenda_from);
        sqlite3_bind_int64(read, 15, f->agenda_to);
    }
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    int rc, count = 0;
    bool ok = true, over_limit = false;
    while ((rc = sqlite3_step(read)) == SQLITE_ROW) {
        if (++count > 10000) {
            over_limit = true;
            break;
        }
        json_object *ticket = ticket_json(read);
        bool matches = false;
        ok = ticket && operations_public_sla(ticket, a) &&
             operations_queue_match(ticket, f->operations_queue, a, now, &matches);
        if (ok && matches) {
            sqlite3_bind_int64(write, 1, json_object_get_int64(json_object_object_get(ticket, "id")));
            ok = sqlite3_step(write) == SQLITE_DONE;
            sqlite3_reset(write);
        }
        if (ticket)
            json_object_put(ticket);
        if (!ok)
            break;
    }
    sqlite3_finalize(read);
    sqlite3_finalize(write);
    if (over_limit) {
        error_response(r, 503, "La vista operativa supera 10000 ticket. Restringi gli altri filtri.");
        return false;
    }
    if (!ok || rc != SQLITE_DONE) {
        db_error(r);
        return false;
    }
    return true;
}
static void operations_dashboard(Request *r, Auth *a) {
    if (!raw_empty_query(r) || !raw_scope(r, a))
        return;
    json_object *o = json_object_new_object(), *counts = json_object_new_object(), *ages = json_object_new_object(),
                *work = json_object_new_array();
    json_object_object_add(o, "counts", counts);
    json_object_object_add(o, "age_bands", ages);
    json_object_object_add(o, "workload", work);
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    add_int(o, "generated_at", now);
    const char *keys[] = {"active",
                          "assigned",
                          "unassigned",
                          "waiting",
                          "response_breached",
                          "resolution_breached",
                          "follow_up_due",
                          "reopened",
                          "response_due_today",
                          "resolution_due_today"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        add_int(counts, keys[i], 0);
    const char *bands[] = {"under_1_day", "days_1_7", "days_8_30", "over_30_days"};
    for (size_t i = 0; i < 4; i++)
        add_int(ages, bands[i], 0);
    sqlite3_stmt *st =
        prepare("SELECT " TICKET_COLUMNS TICKET_FROM "WHERE " PUNTO_VISIBLE_SQL " ORDER BY t.id LIMIT 10001");
    if (!st)
        goto failed;
    int rc, count = 0, resolved_count = 0, raw_count = 0, responded_count = 0;
    double resolution_sum = 0, raw_sum = 0, response_sum = 0;
    json_object *groups = json_object_new_array();
    json_object_object_add(o, "group_workload", groups);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (++count > 10000) {
            sqlite3_finalize(st);
            json_object_put(o);
            error_response(r, 503, "Cruscotto limitato a 10000 ticket. Archivia o filtra i dati prima di procedere.");
            return;
        }
        json_object *ticket = ticket_json(st);
        if (!ticket || !operations_public_sla(ticket, a)) {
            if (ticket)
                json_object_put(ticket);
            sqlite3_finalize(st);
            goto failed;
        }
        json_object *sla = json_object_object_get(ticket, "sla");
        const char *status = operations_string(ticket, "status", "");
        bool active = strcmp(status, "resolved") && strcmp(status, "closed");
        sqlite3_int64 assigned = json_object_get_int64(json_object_object_get(ticket, "assignee_id"));
        bool waiting = !strcmp(status, "waiting"),
             response = json_object_get_boolean(json_object_object_get(sla, "response_breached")),
             resolution = json_object_get_boolean(json_object_object_get(sla, "resolution_breached"));
        const char *increment[] = {active ? "active" : NULL,
                                   active ? (assigned ? "assigned" : "unassigned") : NULL,
                                   active && waiting ? "waiting" : NULL,
                                   active && response ? "response_breached" : NULL,
                                   active && resolution ? "resolution_breached" : NULL,
                                   active && waiting &&
                                           json_object_get_int64(json_object_object_get(ticket, "follow_up_at")) > 0 &&
                                           json_object_get_int64(json_object_object_get(ticket, "follow_up_at")) <= now
                                       ? "follow_up_due"
                                       : NULL};
        for (size_t i = 0; i < 6; i++)
            if (increment[i])
                add_int(counts, increment[i], json_object_get_int64(json_object_object_get(counts, increment[i])) + 1);
        add_int(counts, "reopened",
                json_object_get_int64(json_object_object_get(counts, "reopened")) +
                    json_object_get_int64(json_object_object_get(sla, "reopened_count")));
        sqlite3_int64 responded = json_object_get_int64(json_object_object_get(sla, "first_response_at")),
                      sla_start = json_object_get_int64(json_object_object_get(sla, "started_at"));
        if (responded >= sla_start && responded > 0) {
            responded_count++;
            response_sum += (double)(responded - sla_start) / 60;
        }
        if (active) {
            if (sla && !responded &&
                operations_same_day(operations_string(sla, "timezone", "Europe/Rome"),
                                    json_object_get_int64(json_object_object_get(sla, "response_due_at")), now))
                add_int(counts, "response_due_today",
                        json_object_get_int64(json_object_object_get(counts, "response_due_today")) + 1);
            if (sla && operations_same_day(operations_string(sla, "timezone", "Europe/Rome"),
                                    json_object_get_int64(json_object_object_get(sla, "resolution_due_at")), now))
                add_int(counts, "resolution_due_today",
                        json_object_get_int64(json_object_object_get(counts, "resolution_due_today")) + 1);
            const char *group = operations_string(ticket, "group_name", "");
            json_object *row = NULL;
            for (size_t k = 0; k < json_object_array_length(groups); k++) {
                json_object *item = json_object_array_get_idx(groups, k);
                if (!strcmp(operations_string(item, "group_name", ""), group)) {
                    row = item;
                    break;
                }
            }
            if (!row) {
                row = json_object_new_object();
                add_string(row, "group_name", group);
                add_int(row, "active", 0);
                add_int(row, "waiting", 0);
                add_int(row, "overdue", 0);
                json_object_array_add(groups, row);
            }
            add_int(row, "active", json_object_get_int64(json_object_object_get(row, "active")) + 1);
            if (waiting)
                add_int(row, "waiting", json_object_get_int64(json_object_object_get(row, "waiting")) + 1);
            if (resolution)
                add_int(row, "overdue", json_object_get_int64(json_object_object_get(row, "overdue")) + 1);
        }
        if (active) {
            sqlite3_int64 age = now - operations_epoch(operations_string(ticket, "created_at", NULL));
            const char *band = bands[age < 86400 ? 0 : age <= 7 * 86400 ? 1 : age <= 30 * 86400 ? 2 : 3];
            add_int(ages, band, json_object_get_int64(json_object_object_get(ages, band)) + 1);
            json_object *row = NULL;
            for (size_t i = 0; i < json_object_array_length(work); i++) {
                json_object *candidate = json_object_array_get_idx(work, i);
                if (json_object_get_int64(json_object_object_get(candidate, "user_id")) == assigned) {
                    row = candidate;
                    break;
                }
            }
            if (!row) {
                row = json_object_new_object();
                add_int(row, "user_id", assigned);
                add_string(row, "name", assigned ? operations_string(ticket, "assignee_name", "") : "Non assegnato");
                add_int(row, "assigned", 0);
                add_int(row, "waiting", 0);
                add_int(row, "overdue", 0);
                json_object_array_add(work, row);
            }
            add_int(row, "assigned", json_object_get_int64(json_object_object_get(row, "assigned")) + 1);
            if (waiting)
                add_int(row, "waiting", json_object_get_int64(json_object_object_get(row, "waiting")) + 1);
            if (resolution)
                add_int(row, "overdue", json_object_get_int64(json_object_object_get(row, "overdue")) + 1);
        } else if (sla) {
            sqlite3_int64 end = json_object_get_int64(json_object_object_get(sla, "resolved_at")),
                          start = json_object_get_int64(json_object_object_get(sla, "started_at"));
            if (end >= start) {
                resolution_sum += (double)(end - start) / 60;
                resolved_count++;
            }
        }
        sqlite3_int64 raw_wait = json_object_get_int64(json_object_object_get(sla, "raw_wait_minutes"));
        if (raw_wait > 0) {
            raw_sum += (double)raw_wait;
            raw_count++;
        }
        json_object_put(ticket);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    json_object *raw = json_object_new_object();
    json_object_object_add(o, "raw", raw);
    add_int(raw, "new", 0);
    add_int(raw, "reviewing", 0);
    add_int(raw, "backlog", 0);
    add_int(raw, "quarantine", 0);
    sqlite3_int64 oldest = 0;
    st = prepare("SELECT m.state,count(*),min(m.received_at)" RAW_FROM "WHERE " RAW_VISIBLE_SQL " GROUP BY m.state");
    if (!st)
        goto failed;
    sqlite3_bind_int(st, 1, raw_admin(a));
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *state = column_text(st, 0);
        sqlite3_int64 total = sqlite3_column_int64(st, 1), received = sqlite3_column_int64(st, 2);
        if (!strcmp(state, "new") || !strcmp(state, "reviewing")) {
            add_int(raw, state, total);
            add_int(raw, "backlog", json_object_get_int64(json_object_object_get(raw, "backlog")) + total);
            if (!oldest || received < oldest)
                oldest = received;
        } else if (!strcmp(state, "quarantine"))
            add_int(raw, "quarantine", total);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        goto failed;
    if (oldest)
        add_int(raw, "oldest_received_at", oldest);
    else
        json_object_object_add(raw, "oldest_received_at", NULL);
    add_int(raw, "oldest_age_minutes", oldest && now > oldest ? (now - oldest) / 60 : 0);
    add_int(o, "responded_count", responded_count);
    json_object_object_add(o, "average_response_minutes",
                           responded_count ? json_object_new_double(response_sum / responded_count) : NULL);
    add_int(o, "resolved_count", resolved_count);
    json_object_object_add(o, "average_resolution_minutes",
                           resolved_count ? json_object_new_double(resolution_sum / resolved_count) : NULL);
    json_object_object_add(o, "average_raw_wait_minutes",
                           raw_count ? json_object_new_double(raw_sum / raw_count) : NULL);
    send_json(r, 200, o);
    return;
failed:
    json_object_put(o);
    db_error(r);
}
static bool operations_route(Request *r, Auth *a, bool get, bool post, bool put) {
    sqlite3_int64 id;
    const char *suffix;
    if (!strcmp(r->path, "/api/operations/suggest")) {
        if (post)
            operations_suggestions(r, a, 0);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (path_id(r->path, "/api/raw/messages/", &id, &suffix) && !strcmp(suffix, "/suggestions")) {
        if (get)
            operations_suggestions(r, a, id);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    bool rule = !strcmp(r->path, "/api/operations/rules"), policy = !strcmp(r->path, "/api/operations/sla-policies");
    if (rule || policy) {
        if (get)
            operations_config_list(r, a, rule);
        else if (post)
            operations_config_save(r, a, 0, rule);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if ((path_id(r->path, "/api/operations/rules/", &id, &suffix) ||
         path_id(r->path, "/api/operations/sla-policies/", &id, &suffix)) &&
        !*suffix) {
        if (put)
            operations_config_save(r, a, id, !strncmp(r->path, "/api/operations/rules/", 22));
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path, "/api/operations/views")) {
        if (get || post)
            operations_views(r, a, 0, get);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (path_id(r->path, "/api/operations/views/", &id, &suffix) && !*suffix) {
        if (put)
            operations_views(r, a, id, false);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path, "/api/tickets/bulk/preview")) {
        if (post)
            operations_bulk_preview(r, a);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path, "/api/tickets/bulk/apply")) {
        if (post)
            operations_bulk_apply(r, a);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path, "/api/reports/operations")) {
        if (get)
            operations_dashboard(r, a);
        else
            error_response(r, 405, "Metodo non consentito.");
        return true;
    }
    if (path_id(r->path, "/api/tickets/", &id, &suffix)) {
        if (!strcmp(suffix, "/presence")) {
            if (get || post)
                operations_presence(r, a, id, post);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (!strcmp(suffix, "/draft")) {
            if (put)
                operations_save_draft(r, a, id, false);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
        if (!strcmp(suffix, "/draft/discard")) {
            if (post)
                operations_save_draft(r, a, id, true);
            else
                error_response(r, 405, "Metodo non consentito.");
            return true;
        }
    }
    return false;
}
