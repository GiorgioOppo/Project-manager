/* One filtered population feeds the catalog, export and operational dashboard. */
#define OPERATIONAL_CTE \
    "WITH operational AS (SELECT assets.*,c.assignee_user_id,c.assignee_user_name,c.assignee_person_id,c.site_id," \
    "COALESCE(site.name,c.site_name,'') AS site_name,c.department_id,COALESCE(department.name,c.department_name,'') AS department_name," \
    "cu.id AS custody_id,cu.due_on AS custody_due_on,v.id AS verification_id,v.verified_on,v.location_matches," \
    "(assets.asset_type IN ('it_device','network_device','physical') AND assets.status NOT IN ('retired','lost')) AS actionable " \
    "FROM assets LEFT JOIN asset_context c ON c.asset_id=assets.id " \
    "LEFT JOIN inventory_places site ON site.id=c.site_id LEFT JOIN inventory_places department ON department.id=c.department_id " \
    "LEFT JOIN asset_custody cu ON cu.asset_id=assets.id AND cu.active=1 " \
    "LEFT JOIN asset_verifications v ON v.id=(SELECT latest.id FROM asset_verifications latest WHERE latest.asset_id=assets.id ORDER BY latest.verified_on DESC,latest.id DESC LIMIT 1)) "
#define FILTERED_CTE \
    ",filtered AS (SELECT * FROM operational WHERE (?1=0 OR id<?1) AND (?2='' OR status=?2) AND (?5='' OR asset_type=?5) " \
    "AND ((?6='' AND ?14='') OR assignee_user_id=CAST(?6 AS INTEGER) OR (?14<>'' AND (assignee_person_id=CAST(?14 AS INTEGER) " \
    "OR EXISTS(SELECT 1 FROM inventory_asset_subjects mine_subject WHERE mine_subject.asset_id=operational.id AND mine_subject.active=1 AND mine_subject.person_id=CAST(?14 AS INTEGER))))) " \
    "AND (?7='' OR site_id=CAST(?7 AS INTEGER)) AND (?8='' OR department_id=CAST(?8 AS INTEGER)) " \
    "AND (?12='' OR EXISTS(SELECT 1 FROM inventory_asset_subjects subjects WHERE subjects.asset_id=operational.id AND subjects.active=1 AND subjects.person_id=CAST(?12 AS INTEGER))) " \
    "AND (?13='' OR EXISTS(SELECT 1 FROM inventory_asset_subjects subjects WHERE subjects.asset_id=operational.id AND subjects.active=1 AND subjects.identity_id=CAST(?13 AS INTEGER))) " \
    "AND (?3='' OR instr(lower(asset_tag),lower(?3))>0 OR instr(lower(name),lower(?3))>0 OR instr(lower(category),lower(?3))>0 " \
    "OR instr(lower(serial_number),lower(?3))>0 OR instr(lower(manufacturer),lower(?3))>0 OR instr(lower(model),lower(?3))>0 " \
    "OR instr(lower(location),lower(?3))>0 OR instr(lower(assignee),lower(?3))>0 OR instr(lower(notes),lower(?3))>0 " \
    "OR instr(lower(site_name),lower(?3))>0 OR instr(lower(department_name),lower(?3))>0 OR instr(lower(assignee_user_name),lower(?3))>0 " \
    "OR EXISTS(SELECT 1 FROM json_each(operational.details) typed WHERE typed.type='text' AND instr(lower(typed.value),lower(?3))>0)) " \
    "AND (?9='' OR (actionable AND ((?9='open_custody' AND custody_id IS NOT NULL) " \
    "OR (?9='overdue_returns' AND custody_due_on<>'' AND custody_due_on<?10) OR (?9='never_verified' AND verification_id IS NULL) " \
    "OR (?9='verification_due' AND date(verified_on,'+'||?11||' days')<=?10) OR (?9='location_mismatch' AND location_matches=0))))) "

static int date_day_number(const char *date);
static bool operational_filter_valid(const Form *form) {
    sqlite3_int64 id=0;
    int days=90;
    const char *ids[]={"site_id","department_id","person_id","identity_id","mapped_person_id"};
    for (size_t i=0;i<5;i++) if (has_field(form,ids[i]) && !integer(field(form,ids[i]),1,INT64_MAX,&id)) return false;
    if (has_field(form,"verification_days") && !number(field(form,"verification_days"),1,365,&days)) return false;
    if (has_field(form,"today") && (!*field(form,"today") || !valid_date(field(form,"today")))) return false;
    if (has_field(form,"attention")) {
        const char *allowed[]={"open_custody","overdue_returns","never_verified","verification_due","location_mismatch"};
        bool found=false;
        for (size_t i=0;i<5;i++) if (!strcmp(field(form,"attention"),allowed[i])) found=true;
        if (!found) return false;
    }
    return true;
}
static bool operational_filter_bind(sqlite3_stmt *s, const Form *form, sqlite3_int64 after, int limit) {
    char today[11];
    if (!business_date(today)) return false;
    int days=90;
    if (has_field(form,"verification_days") && !number(field(form,"verification_days"),1,365,&days)) return false;
    bool ok=sqlite3_bind_int64(s,1,after)==SQLITE_OK && sqlite3_bind_int(s,4,limit)==SQLITE_OK;
    const char *keys[]={"status","q","asset_type","assignee_user_id","site_id","department_id","attention","person_id","identity_id","mapped_person_id"};
    const int indexes[]={2,3,5,6,7,8,9,12,13,14};
    for (size_t i=0;ok && i<10;i++) ok=sqlite3_bind_text(s,indexes[i],field(form,keys[i]),-1,SQLITE_TRANSIENT)==SQLITE_OK;
    return ok && sqlite3_bind_text(s,10,has_field(form,"today") ? field(form,"today") : today,-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_int(s,11,days)==SQLITE_OK;
}
static bool dashboard_groups(json_object *o, const char *key, const char *id, const char *name, const Form *form, bool *truncated) {
    char sql[8192];
    int length=snprintf(sql,sizeof(sql),OPERATIONAL_CTE FILTERED_CTE
        "SELECT %s,COALESCE(MAX(%s),''),COUNT(*) FROM filtered GROUP BY %s ORDER BY COUNT(*) DESC,%s LIMIT 21",id,name,id,id);
    if (length<0 || (size_t)length>=sizeof(sql)) return false;
    sqlite3_stmt *s=prepare(sql);
    if (!s || !operational_filter_bind(s,form,0,0)) { sqlite3_finalize(s); return false; }
    json_object *groups=json_object_new_array();
    json_object_object_add(o,key,groups);
    int result=SQLITE_DONE;
    bool has_unassigned=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (json_object_array_length(groups)==20) { *truncated=true; break; }
        json_object *group=json_object_new_object();
        if (sqlite3_column_type(s,0)==SQLITE_NULL) { json_object_object_add(group,"id",NULL); has_unassigned=true; }
        else addint(group,"id",sqlite3_column_int64(s,0));
        addstr(group,"name",(const char *)sqlite3_column_text(s,1)); addint(group,"count",sqlite3_column_int64(s,2));
        json_object_array_add(groups,group);
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) return false;
    /* Keep the unassigned bucket visible even when it falls outside the top 20. */
    if (*truncated && !has_unassigned) {
        length=snprintf(sql,sizeof(sql),OPERATIONAL_CTE FILTERED_CTE "SELECT COUNT(*) FROM filtered WHERE %s IS NULL",id);
        if (length<0 || (size_t)length>=sizeof(sql)) return false;
        s=prepare(sql);
        if (!s || !operational_filter_bind(s,form,0,0)) { sqlite3_finalize(s); return false; }
        result=sqlite3_step(s);
        if (result==SQLITE_ROW && sqlite3_column_int64(s,0)>0) {
            json_object *group=json_object_new_object();
            json_object_object_add(group,"id",NULL); addstr(group,"name",""); addint(group,"count",sqlite3_column_int64(s,0));
            json_object_array_put_idx(groups,19,group);
        }
        sqlite3_finalize(s);
        if (result!=SQLITE_ROW) return false;
    }
    return true;
}
static json_object *operational_dashboard(const char *query, int *status) {
    static const char *const allowed[]={"q","status","asset_type","assignee_user_id","site_id","department_id","attention","verification_days","today","person_id","identity_id","mapped_person_id"};
    Form form;
    if (!form_parse(query,&form)) return failure(status,400,"Filtri cruscotto non validi.");
    bool ok=only_fields(&form,allowed,sizeof(allowed)/sizeof(allowed[0])) && operational_filter_valid(&form);
    sqlite3_int64 id=0;
    if (has_field(&form,"assignee_user_id") && !integer(field(&form,"assignee_user_id"),1,INT64_MAX,&id)) ok=false;
    if (strlen(field(&form,"q"))>200 || !utf8_text(field(&form,"q"),false)) ok=false;
    if (has_field(&form,"status")) {
        bool found=false;
        for (size_t i=0;i<5;i++) if (!strcmp(field(&form,"status"),statuses[i])) found=true;
        if (!found) ok=false;
    }
    if (has_field(&form,"asset_type")) {
        bool found=false;
        for (size_t i=0;i<5;i++) if (!strcmp(field(&form,"asset_type"),asset_types[i])) found=true;
        if (!found) ok=false;
    }
    char today[11];
    if (!business_date(today)) ok=false;
    if (!ok) { form_free(&form); return failure(status,400,"Filtri cruscotto non validi."); }
    json_object *o=ok_json(),*counts=json_object_new_object(),*by_status=json_object_new_object(),*by_type=json_object_new_object();
    addstr(o,"today",has_field(&form,"today") ? field(&form,"today") : today);
    int days=90;
    if (has_field(&form,"verification_days")) (void)number(field(&form,"verification_days"),1,365,&days);
    addint(o,"verification_days",days);
    json_object_object_add(o,"counts",counts); json_object_object_add(o,"by_status",by_status); json_object_object_add(o,"by_type",by_type);
    sqlite3_stmt *s=prepare(OPERATIONAL_CTE FILTERED_CTE
        "SELECT COUNT(*),COALESCE(SUM(actionable AND custody_id IS NOT NULL),0),COALESCE(SUM(actionable AND custody_due_on<>'' AND custody_due_on<?10),0),"
        "COALESCE(SUM(actionable AND verification_id IS NULL),0),COALESCE(SUM(actionable AND date(verified_on,'+'||?11||' days')<=?10),0),"
        "COALESCE(SUM(actionable AND location_matches=0),0) FROM filtered");
    ok=s && operational_filter_bind(s,&form,0,0) && sqlite3_step(s)==SQLITE_ROW;
    const char *count_keys[]={"total","open_custody","overdue_returns","never_verified","verification_due","location_mismatch"};
    if (ok) for (int i=0;i<6;i++) addint(counts,count_keys[i],sqlite3_column_int64(s,i));
    sqlite3_finalize(s);
    if (ok) {
        s=prepare(OPERATIONAL_CTE FILTERED_CTE "SELECT status,asset_type,COUNT(*) FROM filtered GROUP BY status,asset_type");
        ok=s && operational_filter_bind(s,&form,0,0);
        for (size_t i=0;i<5;i++) { addint(by_status,statuses[i],0); addint(by_type,asset_types[i],0); }
        int result=SQLITE_DONE;
        while (ok && (result=sqlite3_step(s))==SQLITE_ROW) {
            const char *state=(const char *)sqlite3_column_text(s,0),*type=(const char *)sqlite3_column_text(s,1);
            sqlite3_int64 count=sqlite3_column_int64(s,2);
            addint(by_status,state,jint(by_status,state)+count); addint(by_type,type,jint(by_type,type)+count);
        }
        if (result!=SQLITE_DONE) ok=false;
        sqlite3_finalize(s);
    }
    bool truncated=false;
    if (ok) ok=dashboard_groups(o,"by_site","site_id","site_name",&form,&truncated) &&
        dashboard_groups(o,"by_department","department_id","department_name",&form,&truncated) &&
        dashboard_groups(o,"by_user","assignee_user_id","assignee_user_name",&form,&truncated);
    form_free(&form);
    if (!ok) { json_object_put(o); return failure(status,500,"Impossibile leggere il cruscotto inventario."); }
    json_object_object_add(o,"groups_truncated",json_object_new_boolean(truncated));
    return o;
}
static json_object *operational_due(const char *query, int *status) {
    static const char *const allowed[]={"kind","today","verification_days","limit","after_id"};
    Form form;
    if (!form_parse(query,&form)) return failure(status,400,"Filtri adempimenti non validi.");
    char today[11];
    bool ok=only_fields(&form,allowed,5) && business_date(today) && operational_filter_valid(&form);
    bool returns=!strcmp(field(&form,"kind"),"return");
    if (!returns && strcmp(field(&form,"kind"),"verification")) ok=false;
    int limit=30,days=90;
    sqlite3_int64 after=0;
    if (has_field(&form,"limit") && !number(field(&form,"limit"),1,100,&limit)) ok=false;
    if (has_field(&form,"after_id") && !integer(field(&form,"after_id"),1,INT64_MAX,&after)) ok=false;
    if (has_field(&form,"verification_days")) (void)number(field(&form,"verification_days"),1,365,&days);
    if (!ok) { form_free(&form); return failure(status,400,"Filtri adempimenti non validi."); }
    if (has_field(&form,"today")) memcpy(today,field(&form,"today"),11);
    sqlite3_stmt *s=prepare(returns ? OPERATIONAL_CTE
        "SELECT id,asset_tag,name,asset_type,custody_due_on,custody_id FROM operational WHERE actionable AND (?1=0 OR id<?1) AND custody_due_on<>'' AND custody_due_on<?2 ORDER BY id DESC LIMIT ?3" : OPERATIONAL_CTE
        "SELECT id,asset_tag,name,asset_type,CASE WHEN verification_id IS NULL THEN substr(created_at,1,10) ELSE date(verified_on,'+'||?4||' days') END,COALESCE(verification_id,0) FROM operational WHERE actionable AND (?1=0 OR id<?1) AND ((verification_id IS NULL AND substr(created_at,1,10)<=?2) OR date(verified_on,'+'||?4||' days')<=?2) ORDER BY id DESC LIMIT ?3");
    ok=s && sqlite3_bind_int64(s,1,after)==SQLITE_OK && sqlite3_bind_text(s,2,today,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int(s,3,limit+1)==SQLITE_OK;
    if (ok && !returns) ok=sqlite3_bind_int(s,4,days)==SQLITE_OK;
    form_free(&form);
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere gli adempimenti inventario."); }
    json_object *o=ok_json(),*items=json_object_new_array();
    json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int result=SQLITE_DONE,count=0,today_number=date_day_number(today);
    sqlite3_int64 last=0;
    bool more=false;
    size_t bytes=strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        const char *due=(const char *)sqlite3_column_text(s,4);
        if (!due || !*due || !valid_date(due)) { result=SQLITE_CORRUPT; break; }
        json_object *event=json_object_new_object();
        addint(event,"asset_id",sqlite3_column_int64(s,0)); addstr(event,"asset_tag",(const char *)sqlite3_column_text(s,1));
        addstr(event,"name",(const char *)sqlite3_column_text(s,2)); addstr(event,"asset_type",(const char *)sqlite3_column_text(s,3));
        addstr(event,"due_kind",returns ? "return_due" : "verification_due"); addstr(event,"due_on",due);
        addint(event,"source_id",sqlite3_column_int64(s,5)); addint(event,"days_remaining",date_day_number(due)-today_number);
        size_t length=strlen(json_object_to_json_string_ext(event,JSON_C_TO_STRING_PLAIN));
        if (bytes+length+64>RESPONSE_LIMIT) { json_object_put(event); more=true; break; }
        bytes+=length+1; last=sqlite3_column_int64(s,0); json_object_array_add(items,event); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_DONE && result!=SQLITE_ROW) { json_object_put(o); return failure(status,500,"Lettura adempimenti interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
