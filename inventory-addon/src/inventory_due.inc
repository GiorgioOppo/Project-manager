/* Gregorian day numbers avoid daylight-saving and local-midnight arithmetic. */
static int date_day_number(const char *date) {
    int year=(date[0]-'0')*1000+(date[1]-'0')*100+(date[2]-'0')*10+date[3]-'0';
    unsigned month=(unsigned)((date[5]-'0')*10+date[6]-'0'),day=(unsigned)((date[8]-'0')*10+date[9]-'0');
    year -= month<=2;
    int era=year/400;
    unsigned yoe=(unsigned)(year-era*400);
    unsigned adjusted_month=month>2 ? month-3 : month+9;
    unsigned doy=(153*adjusted_month+2)/5+day-1;
    return era*146097+(int)(yoe*365+yoe/4-yoe/100+doy);
}
static json_object *list_due(const char *query, int *status) {
    static const char *const allowed[]={"today","days_before","limit","after_id"};
    Form form;
    if (!form_parse(query,&form)) return failure(status,400,"Filtri scadenze non validi.");
    char business_today[11];
    bool ok=only_fields(&form,allowed,4) && business_date(business_today);
    const char *today=has_field(&form,"today") ? field(&form,"today") : business_today;
    int days=30,limit=30;
    sqlite3_int64 after=0;
    if (!*today || !valid_date(today)) ok=false;
    if (has_field(&form,"days_before") && !number(field(&form,"days_before"),1,365,&days)) ok=false;
    if (has_field(&form,"limit") && !number(field(&form,"limit"),1,100,&limit)) ok=false;
    if (has_field(&form,"after_id") && !integer(field(&form,"after_id"),1,INT64_MAX,&after)) ok=false;
    if (!ok) { form_free(&form); return failure(status,400,"Filtri scadenze non validi."); }
    int today_number=date_day_number(today);
    sqlite3_stmt *s=prepare("SELECT " ASSET_COLUMNS " FROM assets WHERE (?1=0 OR id<?1) AND status NOT IN ('retired','lost') AND ("
        "(warranty_until<>'' AND julianday(warranty_until)-julianday(?2)<=?3) OR "
        "(asset_type='physical' AND COALESCE(json_extract(details,'$.maintenance_due'),'')<>'' AND julianday(json_extract(details,'$.maintenance_due'))-julianday(?2)<=?3) OR "
        "(asset_type='license' AND ((COALESCE(json_extract(details,'$.expires_on'),'')<>'' AND julianday(json_extract(details,'$.expires_on'))-julianday(?2)<=?3) OR "
        "(COALESCE(json_extract(details,'$.renewal_on'),'')<>'' AND julianday(json_extract(details,'$.renewal_on'))-julianday(?2)<=?3)))) ORDER BY id DESC LIMIT ?4");
    ok=s && sqlite3_bind_int64(s,1,after)==SQLITE_OK && sqlite3_bind_text(s,2,today,-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_int(s,3,days)==SQLITE_OK && sqlite3_bind_int(s,4,limit+1)==SQLITE_OK;
    form_free(&form);
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere le scadenze."); }
    json_object *o=ok_json(),*items=json_object_new_array();
    json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int result=SQLITE_DONE,count=0;
    sqlite3_int64 last=0;
    size_t bytes=strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    bool more=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        json_object *asset=asset_row(s),*details=NULL,*group=json_object_new_array();
        if (!asset) { json_object_put(group); result=SQLITE_CORRUPT; break; }
        json_object_object_get_ex(asset,"details",&details);
        const char *type=jstr(asset,"asset_type");
        const char *kinds[]={"warranty_until","maintenance_due","expires_on","renewal_on"};
        const char *dates[]={jstr(asset,"warranty_until"),!strcmp(type,"physical") ? jstr(details,"maintenance_due") : "",
            !strcmp(type,"license") ? jstr(details,"expires_on") : "",!strcmp(type,"license") ? jstr(details,"renewal_on") : ""};
        for (size_t d=0;d<4;d++) {
            if (!*dates[d] || !valid_date(dates[d])) continue;
            int remaining=date_day_number(dates[d])-today_number;
            if (remaining>days) continue;
            json_object *event=json_object_new_object();
            addint(event,"asset_id",jint(asset,"id")); addstr(event,"asset_tag",jstr(asset,"asset_tag"));
            addstr(event,"name",jstr(asset,"name")); addstr(event,"asset_type",type);
            addstr(event,"due_kind",kinds[d]); addstr(event,"due_on",dates[d]); addint(event,"days_remaining",remaining);
            json_object_array_add(group,event);
        }
        size_t group_bytes=strlen(json_object_to_json_string_ext(group,JSON_C_TO_STRING_PLAIN));
        if (bytes+group_bytes+64>RESPONSE_LIMIT) {
            more=true; json_object_put(group); json_object_put(asset); break;
        }
        bytes+=group_bytes;
        for (size_t d=0;d<json_object_array_length(group);d++)
            json_object_array_add(items,json_object_get(json_object_array_get_idx(group,d)));
        last=jint(asset,"id"); count++;
        json_object_put(group); json_object_put(asset);
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_DONE && result!=SQLITE_ROW) { json_object_put(o); return failure(status,500,"Lettura scadenze interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
