/* Agenda and durable Google mirror. Only explicit POST actions deliver writes. */
static void ticket_agenda(Request *r,Auth *a) {
    Filters f;
    if (!read_filters_mode(r->query,&f,true)) { error_response(r,400,"Indica un intervallo agenda valido, di massimo 93 giorni."); return; }
    PuntoScope scope;
    if (!punto_load_scope(r,a,&scope)) return;
    OPENSSL_cleanse(&scope,sizeof(scope));
    json_object *o=json_object_new_object(),*items=json_object_new_array();
    json_object_object_add(o,"items",items); add_int(o,"from",f.agenda_from); add_int(o,"to",f.agenda_to);
    if (!exec_sql("BEGIN")) goto fail;
    if (!operations_prepare_filter(r,&f,a)) { exec_sql("ROLLBACK"); json_object_put(o); return; }
    sqlite3_stmt *st=prepare("SELECT count(*) FROM tickets t " FILTER_SQL "AND t.scheduled_start<?15 AND t.scheduled_end>?14");
    if (!st) goto rollback;
    bind_filters(st,&f,a); sqlite3_bind_int64(st,14,f.agenda_from); sqlite3_bind_int64(st,15,f.agenda_to);
    int rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) add_int(o,"total",sqlite3_column_int64(st,0));
    sqlite3_finalize(st);
    if (rc!=SQLITE_ROW) goto rollback;
    st=prepare("SELECT " TICKET_COLUMNS TICKET_FROM FILTER_SQL "AND t.scheduled_start<?15 AND t.scheduled_end>?14 ORDER BY t.scheduled_start,t.id LIMIT ?10 OFFSET ?11");
    if (!st) goto rollback;
    bind_filters(st,&f,a); sqlite3_bind_int64(st,14,f.agenda_from); sqlite3_bind_int64(st,15,f.agenda_to);
    sqlite3_bind_int(st,10,f.limit); sqlite3_bind_int(st,11,f.offset);
    while ((rc=sqlite3_step(st))==SQLITE_ROW) json_object_array_add(items,ticket_json(st));
    sqlite3_finalize(st);
    if (rc!=SQLITE_DONE || !exec_sql("COMMIT")) goto rollback;
    send_json(r,200,o); return;
rollback:
    exec_sql("ROLLBACK");
fail:
    json_object_put(o); db_error(r);
}

typedef struct {
    bool exists;
    sqlite3_int64 owner,revision;
    char *calendar,*event_id,*etag,*link,*state,*error,*operation,*key,*payload,*hash,*remote,*connection;
} CalendarMirror;
static void mirror_free(CalendarMirror *m) {
    free(m->calendar); free(m->event_id); free(m->etag); free(m->link); free(m->state); free(m->error);
    free(m->operation); free(m->key); free(m->payload); free(m->hash); free(m->remote); free(m->connection);
    memset(m,0,sizeof(*m));
}
static bool mirror_load(sqlite3_int64 id,CalendarMirror *m) {
    memset(m,0,sizeof(*m));
    sqlite3_stmt *st=prepare("SELECT owner_id,calendar_id,event_id,etag,event_link,state,error,operation,key,payload,desired_hash,revision,remote_event,coalesce((SELECT connection_id FROM ticket_calendar_connections c WHERE c.ticket_id=ticket_calendar.ticket_id),'') FROM ticket_calendar WHERE ticket_id=?1");
    if (!st) return false;
    sqlite3_bind_int64(st,1,id); int rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) {
        m->exists=true; m->owner=sqlite3_column_int64(st,0); m->revision=sqlite3_column_int64(st,11);
        char **dest[]={&m->calendar,&m->event_id,&m->etag,&m->link,&m->state,&m->error,&m->operation,&m->key,&m->payload,&m->hash,&m->remote,&m->connection};
        int cols[]={1,2,3,4,5,6,7,8,9,10,12,13};
        for (size_t i=0;i<sizeof(cols)/sizeof(cols[0]);i++) {
            *dest[i]=strdup(column_text(st,cols[i]));
            if (!*dest[i]) { sqlite3_finalize(st); mirror_free(m); return false; }
        }
    }
    sqlite3_finalize(st); return rc==SQLITE_ROW || rc==SQLITE_DONE;
}
static bool mirror_pending(CalendarMirror *m) { return m->exists && (!strcmp(m->state,"queued") || !strcmp(m->state,"unknown")); }
static char *calendar_encoded(const char *s) {
    size_t len=strlen(s);
    if (len>(SIZE_MAX-1)/3) return NULL;
    char *out=malloc(len*3+1); if (!out) return NULL;
    char *p=out; const char hex[]="0123456789ABCDEF";
    for (const unsigned char *c=(const unsigned char*)s;*c;c++) {
        if ((*c>='a'&&*c<='z') || (*c>='A'&&*c<='Z') || (*c>='0'&&*c<='9') || strchr("-._~",*c)) *p++=(char)*c;
        else { *p++='%'; *p++=hex[*c>>4]; *p++=hex[*c&15]; }
    }
    *p=0; return out;
}
static json_object *calendar_event_body(Request *r,json_object *ticket) {
    sqlite3_int64 begin=json_object_get_int64(json_object_object_get(ticket,"scheduled_start"));
    sqlite3_int64 end=json_object_get_int64(json_object_object_get(ticket,"scheduled_end"));
    const char *zone=json_object_get_string(json_object_object_get(ticket,"scheduled_timezone"));
    if (!begin || !end || !zone) return NULL;
    time_t times[]={(time_t)begin,(time_t)end}; const char *keys[]={"start","end"};
    json_object *o=json_object_new_object();
    char summary[1600],description[2400];
    const char *code=json_object_get_string(json_object_object_get(ticket,"job_code"));
    snprintf(summary,sizeof(summary),"Ticket #%lld%s%s · %s",(long long)json_object_get_int64(json_object_object_get(ticket,"id")),code?" · ":"",code?code:"",json_object_get_string(json_object_object_get(ticket,"title")));
    /* The provider limits summaries to 1000 UTF-8 bytes. Keep a complete
       scalar at the boundary even for long multilingual job codes/titles. */
    if (strlen(summary)>1000) {
        size_t length=997;
        while (((unsigned char)summary[length]&0xc0)==0x80) length--;
        memcpy(summary+length,"…",4);
    }
    snprintf(description,sizeof(description),"Ticket Punto\n%s/?ticket=%lld",r->request_origin,(long long)json_object_get_int64(json_object_object_get(ticket,"id")));
    add_string(o,"summary",summary); add_string(o,"description",description);
    for (int i=0;i<2;i++) {
        char iso[32]; struct tm utc;
        if (!gmtime_r(&times[i],&utc) || strftime(iso,sizeof(iso),"%Y-%m-%dT%H:%M:%SZ",&utc)!=20) { json_object_put(o); return NULL; }
        json_object *boundary=json_object_new_object(); add_string(boundary,"dateTime",iso); add_string(boundary,"timeZone",zone);
        json_object_object_add(o,keys[i],boundary);
    }
    return o;
}
static bool calendar_epoch(const char *value,sqlite3_int64 *epoch) {
    if (!value || strlen(value)<20) return false;
    char date[11];memcpy(date,value,10);date[10]=0;
    if (!valid_date(date) || value[10]!='T' || value[13]!=':' || value[16]!=':') return false;
    const int positions[]={11,12,14,15,17,18};
    for (size_t i=0;i<6;i++) if (!isdigit((unsigned char)value[positions[i]])) return false;
    int hour=(value[11]-'0')*10+value[12]-'0',minute=(value[14]-'0')*10+value[15]-'0',second=(value[17]-'0')*10+value[18]-'0';
    if (hour>23 || minute>59 || second>59) return false;
    const char *zone=value+19;
    if (*zone=='.') {zone++;if (!isdigit((unsigned char)*zone))return false;while(isdigit((unsigned char)*zone)) {if (*zone!='0')return false;zone++;}}
    int offset=0;
    if (*zone=='Z' && !zone[1]) offset=0;
    else {
        if (strlen(zone)!=6 || (*zone!='+'&&*zone!='-') || zone[3]!=':' || !isdigit((unsigned char)zone[1]) || !isdigit((unsigned char)zone[2]) || !isdigit((unsigned char)zone[4]) || !isdigit((unsigned char)zone[5]))return false;
        int h=(zone[1]-'0')*10+zone[2]-'0',m=(zone[4]-'0')*10+zone[5]-'0';
        if (h>14 || m>59 || (h==14&&m))return false;
        offset=(h*60+m)*60*(*zone=='-'?-1:1);
    }
    int year=atoi(date),month=atoi(date+5),day=atoi(date+8);year-=month<=2;
    int era=(year>=0?year:year-399)/400;unsigned y=(unsigned)(year-era*400);
    unsigned d=(153U*(unsigned)(month+(month>2?-3:9))+2)/5+(unsigned)day-1;
    sqlite3_int64 days=(sqlite3_int64)era*146097+(sqlite3_int64)(y*365+y/4-y/100+d)-719468;
    *epoch=days*86400+hour*3600+minute*60+second-offset;return true;
}
static bool calendar_same_event(json_object *remote,json_object *desired) {
    if (!remote || !desired || !json_object_is_type(remote,json_type_object))return false;
    const char *a=json_object_get_string(json_object_object_get(remote,"summary")),*b=json_object_get_string(json_object_object_get(desired,"summary"));
    if (!a || !b || strcmp(a,b))return false;
    const char *keys[]={"start","end"};
    for (int i=0;i<2;i++) {
        json_object *left=json_object_object_get(remote,keys[i]),*right=json_object_object_get(desired,keys[i]);
        sqlite3_int64 x,y;
        if (!left || !right || !calendar_epoch(json_object_get_string(json_object_object_get(left,"dateTime")),&x) ||
            !calendar_epoch(json_object_get_string(json_object_object_get(right,"dateTime")),&y) || x!=y)return false;
    }
    return true;
}
static bool mirror_save_state(sqlite3_int64 id,CalendarMirror *m,const char *state,const char *error,const char *event_id) {
    sqlite3_stmt *st=prepare("UPDATE ticket_calendar SET state=?1,error=?2,event_id=coalesce(?3,event_id),etag=CASE WHEN ?1='removed' THEN NULL ELSE etag END,event_link=CASE WHEN ?1='removed' THEN NULL ELSE event_link END,remote_event=CASE WHEN ?1='removed' THEN NULL ELSE remote_event END WHERE ticket_id=?4 AND key=?5");
    if (!st) return false;
    bind_text(st,1,state); bind_text(st,2,error?error:""); bind_text(st,3,event_id); sqlite3_bind_int64(st,4,id); bind_text(st,5,m->key);
    bool ok=sqlite3_step(st)==SQLITE_DONE && sqlite3_changes(db)==1; sqlite3_finalize(st); return ok;
}
static bool calendar_text(json_object *o,const char *key,const char **out,size_t maximum,bool required) {
    *out="";json_object *value=NULL;
    if (!required && json_object_object_get_ex(o,key,&value) && !value) return true;
    return get_string(o,key,out,maximum,required,required);
}
/* Operation lookups also verify the immutable remote association. */
static bool mirror_accept_change(sqlite3_int64 id,CalendarMirror *m,json_object *change) {
    const char *key,*state,*event_id,*calendar,*error="";
    if (!calendar_text(change,"idempotency_key",&key,128,true) || strcmp(key,m->key) ||
        !calendar_text(change,"state",&state,16,true) ||
        (strcmp(state,"queued")&&strcmp(state,"unknown")&&strcmp(state,"applied")&&strcmp(state,"conflict")&&strcmp(state,"failed")) ||
        !calendar_text(change,"event_id",&event_id,1024,true) ||
        !calendar_text(change,"calendar_id",&calendar,1024,true) || strcmp(calendar,m->calendar) ||
        !calendar_text(change,"error",&error,1000,false) || (*m->event_id && strcmp(event_id,m->event_id))) return false;
    return mirror_save_state(id,m,!strcmp(state,"applied")&&!strcmp(m->operation,"delete")?"removed":state,error,event_id);
}
static int calendar_get(PuntoScope *scope,const char *endpoint,const char *key,const char *value,json_object **out) {
    char *encoded=calendar_encoded(value); if (!encoded) return 503;
    size_t size=strlen(key)+strlen(encoded)+2; char *query=malloc(size);
    if (!query) { free(encoded); return 503; }
    snprintf(query,size,"%s=%s",key,encoded); free(encoded);
    int status=punto_client_get(scope->grant,endpoint,query,out); free(query); return status;
}
static bool mirror_read_event(sqlite3_int64 id,CalendarMirror *m,PuntoScope *scope) {
    if (!*m->event_id || !strcmp(m->state,"removed")) return true;
    json_object *response=NULL; int status=calendar_get(scope,"calendar/event","event_id",m->event_id,&response);
    if (status==404) {
        if (response) json_object_put(response);
        sqlite3_stmt *st=prepare("UPDATE ticket_calendar SET etag=NULL,event_link=NULL,remote_event=NULL,state=CASE WHEN state='applied' THEN 'missing' ELSE state END,error=CASE WHEN state='applied' THEN 'Evento non presente nella copia del calendario Google.' ELSE error END WHERE ticket_id=?1");
        if (!st) return false;
        sqlite3_bind_int64(st,1,id); bool ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
    }
    if (status!=200) { if (response) json_object_put(response); return false; }
    json_object *event=NULL; const char *event_id,*etag,*calendar,*link="";
    bool valid=json_object_object_get_ex(response,"event",&event) && json_object_is_type(event,json_type_object) &&
        calendar_text(event,"id",&event_id,1024,true) && !strcmp(event_id,m->event_id) &&
        calendar_text(event,"etag",&etag,200,true) && calendar_text(response,"calendar_id",&calendar,1024,true) &&
        !strcmp(calendar,m->calendar) && calendar_text(event,"htmlLink",&link,4096,false);
    const char *provider_state=event?json_object_get_string(json_object_object_get(event,"status")):NULL;
    if (valid && provider_state && !strcmp(provider_state,"cancelled")) {
        sqlite3_stmt *st=prepare("UPDATE ticket_calendar SET etag=NULL,event_link=NULL,remote_event=NULL,state=CASE WHEN state IN ('queued','unknown') THEN state ELSE 'missing' END,error='Evento cancellato su Google; la programmazione interna è conservata.' WHERE ticket_id=?1 AND event_id=?2");
        valid=st!=NULL;
        if (st) {sqlite3_bind_int64(st,1,id);bind_text(st,2,m->event_id);valid=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);}
        json_object_put(response);return valid;
    }
    if (valid) {
        /* A provider URL is displayed only when HTTPS. */
        if (strncmp(link,"https://",8)) link="";
        sqlite3_stmt *st=prepare("UPDATE ticket_calendar SET etag=?1,event_link=?2,remote_event=?3 WHERE ticket_id=?4 AND event_id=?5");
        valid=st!=NULL;
        if (st) {
            bind_text(st,1,etag); bind_text(st,2,link); bind_text(st,3,json_object_to_json_string_ext(event,JSON_C_TO_STRING_PLAIN));
            sqlite3_bind_int64(st,4,id); bind_text(st,5,m->event_id); valid=sqlite3_step(st)==SQLITE_DONE; sqlite3_finalize(st);
        }
    }
    json_object_put(response); return valid;
}
static bool mirror_refresh(sqlite3_int64 id,CalendarMirror *m,PuntoScope *scope,bool same_connection) {
    if (!m->exists) return true;
    if (mirror_pending(m)) {
        json_object *change=NULL; int status=calendar_get(scope,"calendar/change","idempotency_key",m->key,&change);
        if (status==200) {
            bool ok=mirror_accept_change(id,m,change); json_object_put(change); if (!ok) return false;
            mirror_free(m); if (!mirror_load(id,m)) return false;
        } else {
            if (change) json_object_put(change);
            if (status!=404) return false;
            if (same_connection) return mirror_save_state(id,m,"unknown","operation_retryable: Google non ha ricevuto l'operazione; puoi riprovare lo stesso invio.",NULL);
            if (!mirror_save_state(id,m,"unknown","operation_missing: storico Google non disponibile; nessun nuovo invio automatico.",NULL) ||
                !mirror_read_event(id,m,scope)) return false;
            mirror_free(m); if (!mirror_load(id,m)) return false;
            if (*m->remote && *m->etag) {
                if (!mirror_save_state(id,m,"conflict","Storico Google non disponibile: evento ritrovato. Verifica le differenze prima di sincronizzare.",NULL)) return false;
            }
            return true;
        }
    }
    return mirror_pending(m) || mirror_read_event(id,m,scope);
}
static bool mirror_connection_save(sqlite3_int64 id,const char *connection) {
    sqlite3_stmt *st=prepare("INSERT INTO ticket_calendar_connections(ticket_id,connection_id) VALUES(?1,?2) ON CONFLICT(ticket_id) DO UPDATE SET connection_id=excluded.connection_id");
    if (!st) return false;
    sqlite3_bind_int64(st,1,id);bind_text(st,2,connection);
    bool ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);return ok;
}
static bool mirror_prepare(Request *r,Auth *a,sqlite3_int64 id,json_object *ticket,PuntoScope *scope,CalendarMirror *m,const char *action,const char *calendar,const char *connection) {
    bool remove=!strcmp(action,"remove"),create=!m->exists || !strcmp(m->state,"removed");
    const char *operation=remove?"delete":create?"create":"update";
    if (remove && (!m->exists || !strcmp(m->state,"removed"))) return true;
    if (!create && (!*m->event_id || !*m->etag)) { error_response(r,409,"L'evento Google non ha una versione disponibile. Verifica Google prima di riprovare."); return false; }
    json_object *event=remove?NULL:calendar_event_body(r,ticket);
    if (!remove && !event) { error_response(r,400,"Programma prima il ticket nell'agenda interna."); return false; }
    if (!remove && m->exists && !strcmp(m->state,"applied") && *m->remote) {
        json_object *remote=json_tokener_parse(m->remote);bool same=calendar_same_event(remote,event);
        if (remote)json_object_put(remote);
        if (same) {json_object_put(event);return true;}
    }
    char random[33],key[128],hash[65];
    if (!random_hex(random,16)) { if (event) json_object_put(event); db_error(r); return false; }
    snprintf(key,sizeof(key),"ticket:%lld:%s",(long long)id,random);
    const char *body=event?json_object_to_json_string_ext(event,JSON_C_TO_STRING_PLAIN):"";
    if (!digest_hex(body,hash)) { if (event) json_object_put(event); db_error(r); return false; }
    char *ekey=calendar_encoded(key),*ebody=calendar_encoded(body),*eid=calendar_encoded(create?"":m->event_id),*etag=calendar_encoded(create?"":m->etag);
    if (!ekey || !ebody || !eid || !etag) { free(ekey);free(ebody);free(eid);free(etag);if(event)json_object_put(event);db_error(r);return false; }
    char *ecalendar=calendar_encoded(calendar);
    if (!ecalendar) {free(ekey);free(ebody);free(eid);free(etag);if(event)json_object_put(event);db_error(r);return false;}
    size_t size=strlen(ekey)+strlen(ebody)+strlen(eid)+strlen(etag)+strlen(ecalendar)+strlen(connection)+240;
    char *form=malloc(size);
    if (!form) { free(ecalendar);free(ekey);free(ebody);free(eid);free(etag);if(event)json_object_put(event);db_error(r);return false; }
    int n=snprintf(form,size,"idempotency_key=%s&expected_connection_id=%s&expected_calendar_id=%s%s%s%s%s%s%s",ekey,connection,ecalendar,remove?"":"&event=",remove?"":ebody,
        create?"":"&event_id=",create?"":eid,create?"":"&etag=",create?"":etag);
    sqlite3_int64 job=json_object_get_int64(json_object_object_get(ticket,"job_id"));
    if (job && n>0 && (size_t)n<size) snprintf(form+n,size-(size_t)n,"&job_id=%lld",(long long)job);
    free(ecalendar);free(ekey);free(ebody);free(eid);free(etag);if(event)json_object_put(event);
    if (!exec_sql("BEGIN IMMEDIATE")) { free(form);db_error(r);return false; }
    sqlite3_stmt *version=prepare("SELECT revision FROM tickets WHERE id=?1");
    if (!version) { exec_sql("ROLLBACK");free(form);db_error(r);return false; }
    sqlite3_bind_int64(version,1,id);
    bool current=sqlite3_step(version)==SQLITE_ROW && sqlite3_column_int64(version,0)==json_object_get_int64(json_object_object_get(ticket,"revision"));
    sqlite3_finalize(version);
    if (!current) { exec_sql("ROLLBACK");free(form);error_response(r,409,"Il ticket è stato modificato. Ricaricalo prima di aggiornare Google.");return false; }
    sqlite3_stmt *prior=prepare("SELECT key,state FROM ticket_calendar WHERE ticket_id=?1");
    if (!prior) {exec_sql("ROLLBACK");free(form);db_error(r);return false;}
    sqlite3_bind_int64(prior,1,id);int prior_result=sqlite3_step(prior);
    bool unchanged=m->exists?(prior_result==SQLITE_ROW && !strcmp(column_text(prior,0),m->key) && !strcmp(column_text(prior,1),m->state)):prior_result==SQLITE_DONE;
    sqlite3_finalize(prior);
    if (!unchanged) {exec_sql("ROLLBACK");free(form);error_response(r,409,"È già cambiata un'operazione Google per questo ticket. Verifica Google prima di riprovare.");return false;}
    sqlite3_stmt *st=prepare("INSERT INTO ticket_calendar(ticket_id,owner_id,calendar_id,state,operation,key,payload,desired_hash,revision) VALUES(?1,?2,?3,'unknown',?4,?5,?6,?7,?8) ON CONFLICT(ticket_id) DO UPDATE SET owner_id=excluded.owner_id,calendar_id=excluded.calendar_id,event_id=CASE WHEN ?9 THEN NULL ELSE event_id END,etag=CASE WHEN ?9 THEN NULL ELSE etag END,event_link=CASE WHEN ?9 THEN NULL ELSE event_link END,remote_event=CASE WHEN ?9 THEN NULL ELSE remote_event END,state='unknown',error='',operation=excluded.operation,key=excluded.key,payload=excluded.payload,desired_hash=excluded.desired_hash,revision=excluded.revision");
    bool ok=st!=NULL;
    if (st) {
        sqlite3_bind_int64(st,1,id);sqlite3_bind_int64(st,2,scope->user_id);bind_text(st,3,calendar);bind_text(st,4,operation);
        bind_text(st,5,key);bind_text(st,6,form);bind_text(st,7,hash);sqlite3_bind_int64(st,8,json_object_get_int64(json_object_object_get(ticket,"revision")));sqlite3_bind_int(st,9,create);
        ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);
    }
    free(form);
    if (ok && create) {
        char identity[200],event_id[65];snprintf(identity,sizeof(identity),"%lld:%s",(long long)scope->user_id,key);
        ok=digest_hex(identity,event_id);
        st=ok?prepare("UPDATE ticket_calendar SET event_id=?1 WHERE ticket_id=?2"):NULL;
        if (st) {bind_text(st,1,event_id);sqlite3_bind_int64(st,2,id);ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);} else ok=false;
    }
    if (!ok || !mirror_connection_save(id,connection) || !add_history(id,a,"calendar_requested",remove?"Richiesta cancellazione evento Google.":"Richiesta sincronizzazione con Google.") || !exec_sql("COMMIT")) {
        exec_sql("ROLLBACK");db_error(r);return false;
    }
    mirror_free(m); if (!mirror_load(id,m)) { db_error(r);return false; }
    return true;
}
static void ticket_calendar(Request *r,Auth *a,sqlite3_int64 id,bool post) {
    json_object *ticket=fetch_ticket(id),*input=NULL,*status_object=NULL;
    CalendarMirror mirror={0}; PuntoScope scope={0};
    const char *action="refresh"; int response_status=200;
    if (!ticket) { error_response(r,404,"Ticket non trovato.");return; }
    if (!punto_ticket_access(r,a,ticket)) goto done;
    if (r->query && *r->query) { error_response(r,400,"Parametri non consentiti.");goto done; }
    if (post) {
        input=parse_json(r); if (!input) goto done;
        const char *const keys[]={"action","revision",NULL};sqlite3_int64 revision;
        if (!allowed_keys(input,keys) || !get_string(input,"action",&action,16,true,true) ||
            (strcmp(action,"sync")&&strcmp(action,"remove")&&strcmp(action,"refresh")) || !punto_integer(input,"revision",1,&revision)) { error_response(r,400,"Azione calendario non valida.");goto done; }
        if (revision!=json_object_get_int64(json_object_object_get(ticket,"revision"))) { error_response(r,409,"Il ticket è stato modificato. Ricaricalo prima di aggiornare Google.");goto done; }
    }
    if (!punto_load_scope(r,a,&scope)) goto done;
    if (!mirror_load(id,&mirror)) { db_error(r);goto done; }
    bool owner_mismatch=mirror.exists && scope.connected && mirror.owner!=scope.user_id;
    bool connected=false,stale=false,service_error=false; const char *calendar="",*connection="";
    if (scope.connected && scope.calendar_access && !owner_mismatch) {
        int status=punto_client_get(scope.grant,"calendar/status",NULL,&status_object);
        json_object *value=NULL;
        if (status==200 && status_object && json_object_object_get_ex(status_object,"connected",&value) && json_object_is_type(value,json_type_boolean) &&
            calendar_text(status_object,"calendar_id",&calendar,1024,false) && calendar_text(status_object,"connection_id",&connection,64,false) && (!*connection || punto_hex(connection))) {
            connected=json_object_get_boolean(value)!=0 && *calendar;
            if (json_object_object_get_ex(status_object,"reauth_required",&value) && json_object_get_boolean(value)) connected=false;
            stale=mirror.exists && strcmp(mirror.state,"removed") && strcmp(mirror.calendar,calendar);
        } else service_error=true;
    }
    bool read_ready=scope.connected && scope.calendar_access && connected && !owner_mismatch && !stale;
    bool cycle_mismatch=read_ready && mirror.exists && strcmp(mirror.state,"removed") && (!*connection || !*mirror.connection || strcmp(connection,mirror.connection));
    if (read_ready && mirror.exists) {
        if (cycle_mismatch) {
            if (!mirror_read_event(id,&mirror,&scope)) service_error=true;
        } else if (!mirror_refresh(id,&mirror,&scope,true)) service_error=true;
        mirror_free(&mirror);if (!mirror_load(id,&mirror)) {db_error(r);goto done;}
    }
    /* Rebinding a prior OAuth lifecycle requires an explicit verification and
       an exact existing remote event. A missing event never becomes a create. */
    if (post && !strcmp(action,"refresh") && read_ready && cycle_mismatch && !service_error && *connection && *mirror.etag && *mirror.remote) {
        if (!exec_sql("BEGIN IMMEDIATE")) {db_error(r);goto done;}
        sqlite3_stmt *version=prepare("SELECT revision FROM tickets WHERE id=?1");
        bool current=version!=NULL;
        if (version) {sqlite3_bind_int64(version,1,id);current=sqlite3_step(version)==SQLITE_ROW && sqlite3_column_int64(version,0)==json_object_get_int64(json_object_object_get(ticket,"revision"));sqlite3_finalize(version);}
        if (!current) {exec_sql("ROLLBACK");error_response(r,409,"Il ticket è stato modificato. Ricaricalo prima di verificare Google.");goto done;}
        if (!mirror_connection_save(id,connection) || !mirror_save_state(id,&mirror,"conflict","Collegamento Google verificato nuovamente. Controlla le differenze prima di sincronizzare.",NULL) ||
            !add_history(id,a,"calendar_reconciled","Evento Google ritrovato e collegamento verificato esplicitamente.") || !exec_sql("COMMIT")) {exec_sql("ROLLBACK");db_error(r);goto done;}
        mirror_free(&mirror);if (!mirror_load(id,&mirror)) {db_error(r);goto done;}cycle_mismatch=false;
    }
    bool ready=read_ready && !cycle_mismatch && *connection;
    if (post && strcmp(action,"refresh")) {
        if (!ready || service_error) { error_response(r,service_error?503:409,owner_mismatch?"L'evento appartiene a un altro account Punto.":stale?"Il calendario Google è cambiato. Seleziona il calendario originale prima di riprovare.":cycle_mismatch?"Il collegamento Google è cambiato. Usa Verifica Google per ritrovare l’evento originale; un evento mancante non verrà ricreato.":"Collega Punto con accesso calendario e seleziona un calendario Google scrivibile.");goto done; }
        bool deliver=false;
        if (mirror_pending(&mirror)) {
            bool same=(!strcmp(action,"remove") && !strcmp(mirror.operation,"delete")) || (!strcmp(action,"sync") && strcmp(mirror.operation,"delete"));
            if (!same) { error_response(r,409,"È già presente un'operazione Google da completare. Verifica Google e riprova.");goto done; }
            if (!strncmp(mirror.error,"operation_retryable:",20) && !strcmp(mirror.state,"unknown")) deliver=true;
            else if (!strncmp(mirror.error,"operation_missing:",18)) {
                error_response(r,409,"Lo storico dell'operazione Google non è disponibile. Nessun evento verrà ricreato: verifica il calendario originale e usa Verifica Google.");goto done;
            }
        } else {
            if (!mirror_prepare(r,a,id,ticket,&scope,&mirror,action,calendar,connection)) goto done;
            deliver=mirror_pending(&mirror);
        }
        if (deliver) {
            char endpoint[64];snprintf(endpoint,sizeof(endpoint),"calendar/%s",mirror.operation);
            json_object *change=NULL;int status=punto_client_post(scope.grant,endpoint,mirror.payload,&change);
            if ((status==200 || status==202) && change && mirror_accept_change(id,&mirror,change)) {
                if (change) json_object_put(change);
            } else {
                if (change) json_object_put(change);
                /* A timeout, invalid response or lost acknowledgement preserves
                   the exact durable intent; a retry must reuse key and body. */
                const char *next=status==409?"conflict":(status==400 || status==401 || status==403 || status==422)?"failed":"unknown";
                if (!mirror_save_state(id,&mirror,next,status==409?"Google ha rifiutato la versione o la chiave dell'operazione.":!strcmp(next,"failed")?"Google ha rifiutato la richiesta. Controlla autorizzazioni e dati.":"Esito Google non ancora verificato. Verifica la stessa operazione.",NULL)) {db_error(r);goto done;}
            }
            mirror_free(&mirror); if (!mirror_load(id,&mirror)) { db_error(r);goto done; }
            if (!mirror_pending(&mirror) && !mirror_read_event(id,&mirror,&scope)) service_error=true;
            mirror_free(&mirror); if (!mirror_load(id,&mirror)) { db_error(r);goto done; }
        }
    }
    json_object *o=json_object_new_object();
    json_object_object_add(o,"configured",json_object_new_boolean(punto_client_configured()));
    json_object_object_add(o,"connected",json_object_new_boolean(connected));
    json_object_object_add(o,"calendar_access",json_object_new_boolean(scope.calendar_access));
    bool private_access=read_ready;
    add_string(o,"calendar_id",private_access?(mirror.exists?mirror.calendar:(*calendar?calendar:NULL)):NULL);
    const char *state=mirror.exists?mirror.state:"idle";
    if (stale || cycle_mismatch) state="stale";
    add_string(o,"state",state);
    add_string(o,"error",service_error?"Servizio Google temporaneamente non disponibile.":owner_mismatch?"L'evento appartiene a un altro account Punto.":stale?"Il calendario selezionato è cambiato.":cycle_mismatch?"Il collegamento Google è cambiato. Usa Verifica Google: solo un evento originale ritrovato può essere ricollegato; gli eventi mancanti non vengono ricreati.":private_access&&mirror.exists&&*mirror.error?mirror.error:NULL);
    add_string(o,"event_id",private_access&&mirror.exists&&*mirror.event_id?mirror.event_id:NULL);
    add_string(o,"event_link",private_access&&mirror.exists&&*mirror.link?mirror.link:NULL);
    if (mirror.exists) add_int(o,"owner_id",mirror.owner);else json_object_object_add(o,"owner_id",NULL);
    json_object_object_add(o,"owner_mismatch",json_object_new_boolean(owner_mismatch));
    bool pending=mirror_pending(&mirror);
    json_object_object_add(o,"pending",json_object_new_boolean(pending));
    bool can_retry=ready && !service_error && mirror.exists && !strcmp(mirror.state,"unknown") && !strncmp(mirror.error,"operation_retryable:",20);
    json_object_object_add(o,"can_retry",json_object_new_boolean(can_retry));
    add_string(o,"retry_action",can_retry?(!strcmp(mirror.operation,"delete")?"remove":"sync"):NULL);
    bool slot=json_object_get_int64(json_object_object_get(ticket,"scheduled_start"))>0;
    json_object_object_add(o,"can_sync",json_object_new_boolean(ready && !service_error && slot && !pending &&
        (!mirror.exists || !strcmp(mirror.state,"removed") || *mirror.etag)));
    bool diff=false;json_object *remote=private_access && mirror.exists && *mirror.remote?json_tokener_parse(mirror.remote):NULL;
    json_object *desired=calendar_event_body(r,ticket);
    if (private_access && mirror.exists && strcmp(mirror.state,"removed")) {
        char hash[65];const char *plain=desired?json_object_to_json_string_ext(desired,JSON_C_TO_STRING_PLAIN):"";
        diff=digest_hex(plain,hash) && strcmp(hash,mirror.hash);
        if (remote && desired) {
            if (!calendar_same_event(remote,desired)) diff=true;
        }
    }
    if (desired) json_object_put(desired);
    json_object_object_add(o,"remote_event",remote);
    json_object_object_add(o,"remote_diff",json_object_new_boolean(diff));
    if (pending) response_status=202;
    send_json(r,response_status,o);
done:
    if (status_object) json_object_put(status_object);
    if (input) json_object_put(input);
    json_object_put(ticket);mirror_free(&mirror);OPENSSL_cleanse(&scope,sizeof(scope));
}
