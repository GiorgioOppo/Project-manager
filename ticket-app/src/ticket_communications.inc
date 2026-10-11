/* Customer correspondence, confirmed work logs, and reusable operator content. */
static bool comm_no_query(Request *r) {
    if (!r->query || !*r->query) return true;
    error_response(r,400,"Parametri non validi."); return false;
}
static bool comm_source_access(Request *r,Auth *a,sqlite3_int64 id) {
    int access=raw_correspondence_access(id,a);
    if (access==1) return true;
    if (access<0) db_error(r);
    else error_response(r,404,"Ticket non trovato.");
    return false;
}
static json_object *comm_ticket(Request *r,Auth *a,sqlite3_int64 id) {
    json_object *ticket=fetch_ticket(id);
    if (!ticket) { error_response(r,404,"Ticket non trovato."); return NULL; }
    if (!punto_ticket_access(r,a,ticket) || !raw_ticket_scope(r,a,ticket)) { json_object_put(ticket); return NULL; }
    if (!comm_source_access(r,a,id)) { json_object_put(ticket); return NULL; }
    return ticket;
}
static bool comm_headersafe(const char *s) { return s && !strchr(s,'\r') && !strchr(s,'\n'); }
static bool comm_textsafe(const char *s,bool body) {
    for (const unsigned char *p=(const unsigned char *)s;*p;p++)
        if (*p==127 || (*p<32 && (!body || !strchr("\r\n\t",*p)))) return false;
    return true;
}
static void comm_target(char out[700]) {
    const char *url=getenv("MAIL_API_URL");
    snprintf(out,700,"%s|%s",mail_client_namespace(),url?url:"");
}
static json_object *comm_message(sqlite3_stmt *st) {
    json_object *o=json_object_new_object();
    const char *names[]={"to_email","subject","body","state","message_id","in_reply_to","references","created_at","updated_at","author_name","error"};
    add_int(o,"id",sqlite3_column_int64(st,0)); add_int(o,"ticket_id",sqlite3_column_int64(st,1));
    for (int i=0;i<11;i++) add_string(o,names[i],column_text(st,i+2));
    add_int(o,"revision",sqlite3_column_int64(st,13)); add_string(o,"direction","outgoing");
    if (sqlite3_column_type(st,14)!=SQLITE_NULL) add_int(o,"sent_at",sqlite3_column_int64(st,14));
    else json_object_object_add(o,"sent_at",NULL);
    return o;
}
#define COMM_MESSAGE_SELECT "SELECT m.id,m.ticket_id,m.to_email,m.subject,m.body,m.state,m.message_id,m.in_reply_to,m.refs,m.created_at,m.updated_at,u.name,m.error,m.revision,m.sent_at FROM ticket_messages m JOIN users u ON u.id=m.author_id "
static json_object *comm_fetch_message(sqlite3_int64 ticket,sqlite3_int64 id) {
    sqlite3_stmt *st=prepare(COMM_MESSAGE_SELECT "WHERE m.ticket_id=?1 AND m.id=?2");
    if (!st) return NULL;
    sqlite3_bind_int64(st,1,ticket); sqlite3_bind_int64(st,2,id);
    json_object *o=sqlite3_step(st)==SQLITE_ROW?comm_message(st):NULL;
    sqlite3_finalize(st); return o;
}
static bool comm_thread_id(const char *raw,char out[258]) {
    size_t n=raw?strlen(raw):0;
    if (n>=2 && raw[0]=='<' && raw[n-1]=='>') { raw++; n-=2; }
    if (!n || n>253 || !memchr(raw,'@',n)) return false;
    unsigned ats=0;
    for (size_t i=0;i<n;i++) {
        if (raw[i]=='@') ats++;
        if ((unsigned char)raw[i]<=32 || (unsigned char)raw[i]>=127 || strchr("<>\"()\\[],:;",raw[i])) return false;
    }
    if (ats!=1 || raw[0]=='@' || raw[n-1]=='@') return false;
    out[0]='<'; memcpy(out+1,raw,n); out[n+1]='>'; out[n+2]=0; return true;
}
static bool comm_latest_raw(Auth *a,sqlite3_int64 id,char recipient[255],char reply[258],char refs[1025]) {
    recipient[0]=reply[0]=refs[0]=0;
    sqlite3_stmt *st=prepare("SELECT m.from_email,m.message_id,m.references_json FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id WHERE l.ticket_id=?2 AND " RAW_VISIBLE_SQL " ORDER BY m.received_at DESC,m.id DESC LIMIT 1");
    if (!st) return false;
    sqlite3_bind_int(st,1,raw_admin(a)); sqlite3_bind_int64(st,2,id);
    int rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) {
        char normalized[255];
        if (email_normalize(column_text(st,0),normalized)) bounded_copy(recipient,255,normalized);
        comm_thread_id(column_text(st,1),reply);
        json_object *array=json_tokener_parse(column_text(st,2));
        if (array && json_object_is_type(array,json_type_array)) {
            unsigned count=0;
            for (size_t i=0;i<json_object_array_length(array) && count<31;i++) {
                json_object *v=json_object_array_get_idx(array,i); char safe[258];
                if (v && json_object_is_type(v,json_type_string) && comm_thread_id(json_object_get_string(v),safe) && strlen(refs)+strlen(safe)+2<1025) {
                    if (*refs) strcat(refs," ");
                    strcat(refs,safe);
                    count++;
                }
            }
        }
        if (array) json_object_put(array);
        if (*reply && strlen(refs)+strlen(reply)+2<1025) { if (*refs) strcat(refs," "); strcat(refs,reply); }
    }
    sqlite3_finalize(st); return rc==SQLITE_ROW || rc==SQLITE_DONE;
}
static void comm_conversation(Request *r,Auth *a,sqlite3_int64 id) {
    json_object *ticket=comm_ticket(r,a,id);
    if (!ticket) return;
    json_object *o=json_object_new_object(),*items=json_object_new_array(),*email=json_object_new_object(),*recipient=json_object_new_object();
    json_object_object_add(o,"items",items); json_object_object_add(o,"email",email); json_object_object_add(o,"recipient",recipient);
    mail_client_refresh(time(NULL));
    json_object_object_add(email,"configured",json_object_new_boolean(mail_client_enabled()));
    json_object_object_add(email,"smtp_configured",json_object_new_boolean(mail_client_smtp_configured()));
    char to[255],reply[258],refs[1025];
    if (!comm_latest_raw(a,id,to,reply,refs)) goto fail;
    if (!*to) { const char *requester=json_object_get_string(json_object_object_get(ticket,"requester")); email_normalize(requester?requester:"",to); }
    add_string(recipient,"email",*to?to:NULL); add_string(recipient,"source",*reply?"raw":"requester");
    sqlite3_stmt *st=prepare("SELECT m.id,m.from_email,m.subject,m.body_text,m.message_id,m.in_reply_to,m.references_json,m.received_at FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id WHERE l.ticket_id=?2 AND " RAW_VISIBLE_SQL " ORDER BY m.received_at,m.id");
    if (!st) goto fail;
    sqlite3_bind_int(st,1,raw_admin(a)); sqlite3_bind_int64(st,2,id); int rc;
    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        json_object *row=json_object_new_object(); add_int(row,"id",sqlite3_column_int64(st,0)); add_int(row,"raw_message_id",sqlite3_column_int64(st,0));
        add_string(row,"direction","incoming"); add_string(row,"state","received");
        add_string(row,"from_email",column_text(st,1)); add_string(row,"subject",column_text(st,2)); add_string(row,"body",column_text(st,3));
        add_string(row,"message_id",column_text(st,4)); add_string(row,"in_reply_to",column_text(st,5)); add_string(row,"references",column_text(st,6));
        time_t received=(time_t)sqlite3_column_int64(st,7); struct tm utc; char iso[32]="";
        if (gmtime_r(&received,&utc)) strftime(iso,sizeof(iso),"%Y-%m-%dT%H:%M:%SZ",&utc);
        add_string(row,"created_at",iso); add_int(row,"revision",1); json_object_array_add(items,row);
    }
    sqlite3_finalize(st); if (rc!=SQLITE_DONE) goto fail;
    st=prepare(COMM_MESSAGE_SELECT "WHERE m.ticket_id=?1 ORDER BY m.id");
    if (!st) goto fail;
    sqlite3_bind_int64(st,1,id);
    while ((rc=sqlite3_step(st))==SQLITE_ROW) json_object_array_add(items,comm_message(st));
    sqlite3_finalize(st); if (rc!=SQLITE_DONE) goto fail;
    json_object_put(ticket); send_json(r,200,o); return;
fail:
    json_object_put(ticket); json_object_put(o); db_error(r);
}
static void comm_save_message(Request *r,Auth *a,sqlite3_int64 ticket_id,sqlite3_int64 id) {
    json_object *ticket=comm_ticket(r,a,ticket_id); if (!ticket) return;
    json_object_put(ticket);
    json_object *input=parse_json(r); if (!input) return;
    const char *const keys[]={"subject","body","to_email","revision",NULL};
    const char *subject=NULL,*body=NULL,*to=NULL; char normalized[255]; sqlite3_int64 revision=0;
    if (!allowed_keys(input,keys) || !get_string(input,"subject",&subject,250,true,true) || !comm_headersafe(subject) || !comm_textsafe(subject,false) || strlen(subject)>250 ||
        !get_string(input,"body",&body,8000,true,true) || !comm_textsafe(body,true) || strlen(body)>8192 ||
        !get_string(input,"to_email",&to,254,true,true) || !email_normalize(to,normalized) ||
        (id && !punto_integer(input,"revision",1,&revision))) {
        json_object_put(input); error_response(r,400,"Indica un destinatario valido, un oggetto entro 250 byte e un testo entro 8192 byte."); return;
    }
    char key[65],message_id[100],recipient[255],reply[258],refs[1025],target[700];
    if (!random_hex(key,32) || !comm_latest_raw(a,ticket_id,recipient,reply,refs)) { json_object_put(input); db_error(r); return; }
    snprintf(message_id,sizeof(message_id),"<ticket-%s@punto.invalid>",key); comm_target(target);
    if (!exec_sql("BEGIN IMMEDIATE")) { json_object_put(input); db_error(r); return; }
    if (!comm_source_access(r,a,ticket_id)) { exec_sql("ROLLBACK"); json_object_put(input); return; }
    sqlite3_stmt *st=prepare(id?"UPDATE ticket_messages SET to_email=?1,subject=?2,body=?3,delivery_key=?7,message_id=?8,revision=revision+1,state='draft',error='',updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE ticket_id=?4 AND id=?5 AND revision=?6 AND state IN('draft','failed')":"INSERT INTO ticket_messages(to_email,subject,body,ticket_id,author_id,message_id,in_reply_to,refs,delivery_key,namespace) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
    if (!st) goto fail;
    bind_text(st,1,normalized); bind_text(st,2,subject); bind_text(st,3,body); sqlite3_bind_int64(st,4,ticket_id);
    if (id) { sqlite3_bind_int64(st,5,id); sqlite3_bind_int64(st,6,revision); bind_text(st,7,key); bind_text(st,8,message_id); }
    else { sqlite3_bind_int64(st,5,a->id); bind_text(st,6,message_id); bind_text(st,7,reply); bind_text(st,8,refs); bind_text(st,9,key); bind_text(st,10,mail_client_enabled()?target:""); }
    int rc=sqlite3_step(st),changed=sqlite3_changes(db); sqlite3_finalize(st);
    if (rc!=SQLITE_DONE) goto fail;
    if (!changed) { exec_sql("ROLLBACK"); json_object_put(input); error_response(r,409,"La bozza è cambiata o non è modificabile."); return; }
    if (!id) id=sqlite3_last_insert_rowid(db);
    if (!add_history(ticket_id,a,"customer_draft","Salvata una bozza per il cliente.") || !exec_sql("COMMIT")) goto fail;
    json_object_put(input); json_object *o=json_object_new_object(); json_object_object_add(o,"message",comm_fetch_message(ticket_id,id)); send_json(r,revision?200:201,o); return;
fail:
    exec_sql("ROLLBACK"); json_object_put(input); db_error(r);
}
static void comm_send_message(Request *r,Auth *a,sqlite3_int64 ticket_id,sqlite3_int64 id) {
    json_object *ticket=comm_ticket(r,a,ticket_id); if (!ticket) return; json_object_put(ticket);
    json_object *input=parse_json(r); if (!input) return;
    const char *const keys[]={"revision",NULL}; sqlite3_int64 revision;
    bool valid=allowed_keys(input,keys) && punto_integer(input,"revision",1,&revision); json_object_put(input);
    if (!valid) { error_response(r,400,"Revisione della bozza non valida."); return; }
    mail_client_refresh(time(NULL));
    if (!mail_client_enabled() || !mail_client_smtp_configured()) { error_response(r,503,"Invio email non configurato. La bozza è conservata."); return; }
    char target[700]; comm_target(target);
    if (!exec_sql("BEGIN IMMEDIATE")) { db_error(r); return; }
    if (!comm_source_access(r,a,ticket_id)) { exec_sql("ROLLBACK"); return; }
    sqlite3_stmt *st=prepare("SELECT to_email,subject,body,message_id,in_reply_to,refs,delivery_key,namespace,state,revision FROM ticket_messages WHERE ticket_id=?1 AND id=?2");
    if (!st) goto dbfail;
    sqlite3_bind_int64(st,1,ticket_id); sqlite3_bind_int64(st,2,id);
    int rc=sqlite3_step(st); char *values[8]={0}; bool ready=false;
    if (rc==SQLITE_ROW) {
        ready=sqlite3_column_int64(st,9)==revision && !strcmp(column_text(st,8),"draft") && (!*column_text(st,7) || !strcmp(column_text(st,7),target));
        for (int i=0;i<8;i++) { values[i]=strdup(column_text(st,i)); if (!values[i]) ready=false; }
    }
    sqlite3_finalize(st);
    if (!ready) {
        exec_sql("ROLLBACK"); for (int i=0;i<8;i++) free(values[i]);
        error_response(r,rc==SQLITE_DONE?404:409,"La bozza è cambiata, è già stata inviata o richiede verifica della consegna/configurazione."); return;
    }
    st=prepare("UPDATE ticket_messages SET state='sending',namespace=?1,revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id=?2 AND revision=?3");
    if (!st) { for (int i=0;i<8;i++) free(values[i]); goto dbfail; }
    bind_text(st,1,target); sqlite3_bind_int64(st,2,id); sqlite3_bind_int64(st,3,revision);
    rc=sqlite3_step(st); sqlite3_finalize(st);
    if (rc!=SQLITE_DONE || !exec_sql("COMMIT")) { for (int i=0;i<8;i++) free(values[i]); goto dbfail; }
    MailDelivery delivery=mail_client_send_threaded(values[6],values[0],values[1],values[2],values[3],values[4],values[5]);
    for (int i=0;i<8;i++) free(values[i]);
    const char *state=delivery==MAIL_DELIVERY_SENT?"sent":delivery==MAIL_DELIVERY_UNCONFIGURED?"draft":delivery==MAIL_DELIVERY_FAILED?"failed":"unknown";
    const char *error=delivery==MAIL_DELIVERY_SENT?"":delivery==MAIL_DELIVERY_UNCONFIGURED?"Invio email non configurato.":delivery==MAIL_DELIVERY_FAILED?"Invio rifiutato dal servizio email. Verifica prima di preparare una nuova bozza.":"Consegna da verificare: nessun reinvio automatico.";
    if (!exec_sql("BEGIN IMMEDIATE")) { db_error(r); return; }
    st=prepare("UPDATE ticket_messages SET state=?1,error=?2,sent_at=?3,revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id=?4 AND state='sending'");
    if (!st) goto dbfail;
    bind_text(st,1,state); bind_text(st,2,error);
    if (delivery==MAIL_DELIVERY_SENT) sqlite3_bind_int64(st,3,(sqlite3_int64)time(NULL)); else sqlite3_bind_null(st,3);
    sqlite3_bind_int64(st,4,id); rc=sqlite3_step(st); sqlite3_finalize(st);
    if (rc!=SQLITE_DONE || (delivery==MAIL_DELIVERY_SENT && !operations_first_response(ticket_id,(sqlite3_int64)time(NULL))) ||
        !add_history(ticket_id,a,"customer_message",delivery==MAIL_DELIVERY_SENT?"Risposta inviata al cliente.":error) || !exec_sql("COMMIT")) goto dbfail;
    json_object *o=json_object_new_object(); json_object_object_add(o,"message",comm_fetch_message(ticket_id,id)); send_json(r,200,o); return;
dbfail:
    exec_sql("ROLLBACK"); db_error(r);
}
static json_object *comm_template(sqlite3_stmt *st) {
    json_object *row=json_object_new_object(); add_int(row,"id",sqlite3_column_int64(st,0));
    add_string(row,"title",column_text(st,1)); add_string(row,"subject",column_text(st,2)); add_string(row,"body",column_text(st,3));
    json_object_object_add(row,"active",json_object_new_boolean(sqlite3_column_int(st,4))); add_int(row,"revision",sqlite3_column_int64(st,5)); add_string(row,"updated_at",column_text(st,6)); return row;
}
static void comm_templates(Request *r,sqlite3_int64 id,bool get) {
    if (get) {
        sqlite3_stmt *st=prepare("SELECT id,title,subject,body,active,revision,updated_at FROM response_templates ORDER BY title,id");
        if (!st) { db_error(r); return; }
        json_object *o=json_object_new_object(),*items=json_object_new_array(); json_object_object_add(o,"items",items); int rc;
        while ((rc=sqlite3_step(st))==SQLITE_ROW) json_object_array_add(items,comm_template(st));
        sqlite3_finalize(st); if (rc!=SQLITE_DONE) { json_object_put(o); db_error(r); return; } send_json(r,200,o); return;
    }
    json_object *input=parse_json(r); if (!input) return;
    const char *const keys[]={"title","subject","body","active","revision",NULL}; const char *title=NULL,*subject=NULL,*body=NULL; sqlite3_int64 revision=0; json_object *active=NULL;
    bool has_active=json_object_object_get_ex(input,"active",&active);
    if (!allowed_keys(input,keys) || !get_string(input,"title",&title,160,true,true) || !get_string(input,"subject",&subject,250,true,true) || !comm_headersafe(subject) ||
        !get_string(input,"body",&body,8000,true,true) || (id && !punto_integer(input,"revision",1,&revision)) ||
        (has_active && (!active || !json_object_is_type(active,json_type_boolean)))) { json_object_put(input); error_response(r,400,"Modello non valido."); return; }
    sqlite3_stmt *st=prepare(id?"UPDATE response_templates SET title=?1,subject=?2,body=?3,active=?4,revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id=?5 AND revision=?6":"INSERT INTO response_templates(title,subject,body,active) VALUES(?1,?2,?3,?4)");
    if (!st) { json_object_put(input); db_error(r); return; }
    bind_text(st,1,title); bind_text(st,2,subject); bind_text(st,3,body); sqlite3_bind_int(st,4,!has_active || json_object_get_boolean(active));
    if (id) { sqlite3_bind_int64(st,5,id); sqlite3_bind_int64(st,6,revision); }
    int rc=sqlite3_step(st),changed=sqlite3_changes(db); sqlite3_finalize(st); json_object_put(input);
    if (rc!=SQLITE_DONE) { db_error(r); return; } if (!changed) { error_response(r,409,"Il modello è cambiato. Ricaricalo."); return; }
    bool creating=!id; if (!id) id=sqlite3_last_insert_rowid(db);
    st=prepare("SELECT id,title,subject,body,active,revision,updated_at FROM response_templates WHERE id=?1");
    if (!st) { db_error(r); return; } sqlite3_bind_int64(st,1,id); rc=sqlite3_step(st);
    json_object *o=json_object_new_object(); if (rc==SQLITE_ROW) json_object_object_add(o,"template",comm_template(st)); sqlite3_finalize(st);
    if (rc!=SQLITE_ROW) { json_object_put(o); db_error(r); return; } send_json(r,creating?201:200,o);
}
static json_object *comm_solution(sqlite3_stmt *st) {
    json_object *o=json_object_new_object(); add_int(o,"id",sqlite3_column_int64(st,0)); add_int(o,"ticket_id",sqlite3_column_int64(st,1));
    add_string(o,"title",column_text(st,2)); add_string(o,"body",column_text(st,3)); add_string(o,"author_name",column_text(st,4));
    add_string(o,"created_at",column_text(st,5)); add_int(o,"revision",sqlite3_column_int64(st,6)); return o;
}
static void comm_save_solution(Request *r,Auth *a,sqlite3_int64 id) {
    json_object *ticket=comm_ticket(r,a,id); if (!ticket) return;
    const char *status=json_object_get_string(json_object_object_get(ticket,"status"));
    bool resolved=!strcmp(status,"resolved") || !strcmp(status,"closed"); json_object_put(ticket);
    if (!resolved) { error_response(r,409,"Salva una soluzione dopo aver risolto il ticket."); return; }
    json_object *input=parse_json(r); if (!input) return; const char *const keys[]={"title","body",NULL}; const char *title=NULL,*body=NULL;
    if (!allowed_keys(input,keys) || !get_string(input,"title",&title,160,true,true) || !get_string(input,"body",&body,8000,true,true)) { json_object_put(input); error_response(r,400,"Indica titolo e soluzione."); return; }
    sqlite3_stmt *st=prepare("INSERT INTO ticket_solutions(ticket_id,author_id,title,body) VALUES(?1,?2,?3,?4) ON CONFLICT(ticket_id) DO UPDATE SET title=excluded.title,body=excluded.body,author_id=excluded.author_id,revision=revision+1");
    if (!st) { json_object_put(input); db_error(r); return; }
    sqlite3_bind_int64(st,1,id); sqlite3_bind_int64(st,2,a->id); bind_text(st,3,title); bind_text(st,4,body);
    int rc=sqlite3_step(st); sqlite3_finalize(st); json_object_put(input); if (rc!=SQLITE_DONE) { db_error(r); return; }
    st=prepare("SELECT s.id,s.ticket_id,s.title,s.body,u.name,s.created_at,s.revision FROM ticket_solutions s JOIN users u ON u.id=s.author_id WHERE s.ticket_id=?1");
    if (!st) { db_error(r); return; } sqlite3_bind_int64(st,1,id); rc=sqlite3_step(st);
    json_object *o=json_object_new_object(); if (rc==SQLITE_ROW) json_object_object_add(o,"solution",comm_solution(st)); sqlite3_finalize(st);
    if (rc!=SQLITE_ROW) { json_object_put(o); db_error(r); return; } send_json(r,201,o);
}
static void comm_solutions(Request *r,Auth *a) {
    char search[801]=""; unsigned limit=50; bool seen_search=false,seen_limit=false;
    if (r->query && *r->query) {
        char *copy=strdup(r->query); if (!copy) { db_error(r); return; } char *part=copy; bool valid=true;
        while (part && valid) {
            char *next=strchr(part,'&'); if (next) *next++=0; char *equal=strchr(part,'=');
            if (!equal || equal==part) { valid=false; break; } *equal++=0;
            if (!strcmp(part,"search") && !seen_search) { seen_search=true; valid=url_decode(equal,search,sizeof(search),true) && utf8_length(search)<=200; }
            else if (!strcmp(part,"limit") && !seen_limit) { long long value; seen_limit=true; valid=parse_uint(equal,100,&value) && value>0; if (valid) limit=(unsigned)value; }
            else valid=false;
            part=next;
        }
        free(copy); if (!valid) { error_response(r,400,"Ricerca non valida."); return; }
    }
    PuntoScope scope; if (!punto_load_scope(r,a,&scope)) return; OPENSSL_cleanse(&scope,sizeof(scope));
    /* SQL visibility covers linked job/asset; RAW-derived restrictions are checked per source below. */
    sqlite3_stmt *st=prepare("SELECT s.id,s.ticket_id,s.title,s.body,u.name,s.created_at,s.revision FROM ticket_solutions s JOIN users u ON u.id=s.author_id JOIN tickets t ON t.id=s.ticket_id WHERE " PUNTO_VISIBLE_SQL " AND (?1='' OR instr(lower(s.title||' '||s.body),lower(?1))>0) ORDER BY s.id DESC LIMIT 1000");
    if (!st) { db_error(r); return; } bind_text(st,1,search); json_object *rows=json_object_new_array(); int rc;
    while ((rc=sqlite3_step(st))==SQLITE_ROW) json_object_array_add(rows,comm_solution(st));
    sqlite3_finalize(st);
    if (rc!=SQLITE_DONE) { json_object_put(rows); db_error(r); return; }
    json_object *o=json_object_new_object(),*items=json_object_new_array(); json_object_object_add(o,"items",items);
    for (size_t i=0;i<json_object_array_length(rows) && json_object_array_length(items)<limit;i++) {
        json_object *row=json_object_array_get_idx(rows,i);
        /* A local source with bound RAW cannot be published outside its live ACL. */
        sqlite3_stmt *check=prepare("SELECT EXISTS(SELECT 1 FROM ticket_raw_links l JOIN raw_messages m ON m.id=l.raw_message_id JOIN raw_mailboxes b ON b.id=m.mailbox_id WHERE l.ticket_id=?1 AND (NOT " RAW_JOB_VISIBLE_SQL("m.job_id") " OR NOT " RAW_JOB_VISIBLE_SQL("b.job_id") " OR EXISTS(SELECT 1 FROM raw_senders rs WHERE rs.mailbox_id=m.mailbox_id AND rs.email=m.from_email COLLATE NOCASE AND NOT " RAW_JOB_VISIBLE_SQL("rs.job_id") ")))");
        if (!check) { json_object_put(rows); json_object_put(o); db_error(r); return; }
        sqlite3_bind_int64(check,1,json_object_get_int64(json_object_object_get(row,"ticket_id"))); bool blocked=sqlite3_step(check)!=SQLITE_ROW || sqlite3_column_int(check,0); sqlite3_finalize(check);
        if (!blocked) json_object_array_add(items,json_object_get(row));
    }
    json_object_put(rows); send_json(r,200,o);
}
static json_object *comm_time_row(sqlite3_stmt *st) {
    json_object *o=json_object_new_object();
    const char *ints[]={"id","ticket_id","owner_user_id","job_id","minutes"};
    for (int i=0;i<5;i++) add_int(o,ints[i],sqlite3_column_int64(st,i));
    const char *strings[]={"date","description","state","request_key","error","created_at"};
    for (int i=0;i<6;i++) add_string(o,strings[i],column_text(st,i+5));
    if (sqlite3_column_type(st,11)!=SQLITE_NULL) add_int(o,"punto_entry_id",sqlite3_column_int64(st,11)); else json_object_object_add(o,"punto_entry_id",NULL);
    add_int(o,"revision",sqlite3_column_int64(st,12)); add_string(o,"author_name",column_text(st,13)); return o;
}
#define COMM_TIME_SELECT "SELECT e.id,e.ticket_id,e.owner_user_id,e.job_id,e.minutes,e.date,e.description,e.state,e.request_key,e.error,e.created_at,e.punto_entry_id,e.revision,u.name,e.author_id,e.idempotency_key FROM ticket_time_entries e JOIN users u ON u.id=e.author_id "
static json_object *comm_fetch_time(sqlite3_int64 id) {
    sqlite3_stmt *st=prepare(COMM_TIME_SELECT "WHERE e.id=?1"); if (!st) return NULL;
    sqlite3_bind_int64(st,1,id); json_object *o=sqlite3_step(st)==SQLITE_ROW?comm_time_row(st):NULL; sqlite3_finalize(st); return o;
}
static void comm_suggestion_date(time_t value,const char *zone,char out[16]) {
    /* The HTTP service is single-threaded; restore its timezone immediately. */
    const char *current=getenv("TZ"); char *saved=current?strdup(current):NULL;
    struct tm local; bool changed=zone && schedule_zone(zone) && setenv("TZ",zone,1)==0;
    if (changed) tzset();
    if (localtime_r(&value,&local)) strftime(out,16,"%Y-%m-%d",&local); else out[0]=0;
    if (changed) { if (saved) setenv("TZ",saved,1); else unsetenv("TZ"); tzset(); }
    free(saved);
}
static void comm_time_list(Request *r,Auth *a,sqlite3_int64 id) {
    json_object *ticket=comm_ticket(r,a,id); if (!ticket) return;
    PuntoScope scope; if (!punto_load_scope(r,a,&scope)) { json_object_put(ticket); return; }
    json_object *o=json_object_new_object(),*items=json_object_new_array(),*suggestion=json_object_new_object();
    json_object_object_add(o,"items",items); json_object_object_add(o,"suggestion",suggestion);
    json_object_object_add(o,"configured",json_object_new_boolean(punto_client_configured()));
    json_object_object_add(o,"connected",json_object_new_boolean(scope.connected)); json_object_object_add(o,"time_access",json_object_new_boolean(scope.time_access));
    sqlite3_int64 start=json_object_get_int64(json_object_object_get(ticket,"scheduled_start")),end=json_object_get_int64(json_object_object_get(ticket,"scheduled_end"));
    const char *zone=json_object_get_string(json_object_object_get(ticket,"scheduled_timezone")); char date[16];
    comm_suggestion_date(start?(time_t)start:time(NULL),start?zone:"Europe/Rome",date);
    add_string(suggestion,"date",date); add_int(suggestion,"minutes",start && end>start?(end-start)/60:30);
    json_object_object_add(suggestion,"job_id",json_object_get(json_object_object_get(ticket,"job_id")));
    add_string(suggestion,"description",json_object_get_string(json_object_object_get(ticket,"title"))); add_string(suggestion,"source",start?"agenda_proposal":"ticket");
    add_string(suggestion,"timezone",start?zone:"Europe/Rome"); json_object_object_add(suggestion,"requires_confirmation",json_object_new_boolean(true));
    sqlite3_stmt *st=prepare(COMM_TIME_SELECT "WHERE e.ticket_id=?1 AND ?4=1 AND e.owner_user_id=?2 AND e.origin=?3 AND e.job_id>0 AND ((SELECT all_jobs FROM punto_access)=1 OR EXISTS(SELECT 1 FROM punto_visible_jobs p WHERE p.job_id=e.job_id)) ORDER BY e.id DESC");
    if (!st) { json_object_put(ticket); json_object_put(o); db_error(r); return; }
    sqlite3_bind_int64(st,1,id); sqlite3_bind_int64(st,2,scope.user_id); bind_text(st,3,punto_client_origin());
    sqlite3_bind_int(st,4,scope.connected && scope.time_access); int rc;
    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        json_object *entry=comm_time_row(st);
        const char *state=column_text(st,7);
        bool retry=sqlite3_column_int64(st,14)==a->id && sqlite3_column_int64(st,3)==json_object_get_int64(json_object_object_get(ticket,"job_id")) &&
            (!strcmp(state,"pending") || !strcmp(state,"unknown"));
        json_object_object_add(entry,"can_retry",json_object_new_boolean(retry));
        if (retry) add_string(entry,"idempotency_key",column_text(st,15));
        json_object_array_add(items,entry);
    }
    sqlite3_finalize(st); json_object_put(ticket); OPENSSL_cleanse(&scope,sizeof(scope));
    if (rc!=SQLITE_DONE) { json_object_put(o); db_error(r); return; } send_json(r,200,o);
}
static bool comm_time_applied(json_object *response,sqlite3_int64 job,const char *date,sqlite3_int64 minutes,const char *note,sqlite3_int64 *remote) {
    json_object *items=NULL;
    if (!response || !json_object_object_get_ex(response,"items",&items) || !items || !json_object_is_type(items,json_type_array) || json_object_array_length(items)!=1) return false;
    json_object *entry=json_object_array_get_idx(items,0); sqlite3_int64 source_job,source_minutes; const char *source_date=NULL,*source_note=NULL;
    return punto_integer(entry,"id",1,remote) && punto_integer(entry,"job_id",1,&source_job) && source_job==job &&
        punto_integer(entry,"minutes",1,&source_minutes) && source_minutes==minutes && get_string(entry,"date",&source_date,10,true,true) && !strcmp(source_date,date) &&
        get_string(entry,"note",&source_note,1000,true,false) && !strcmp(source_note,note);
}
static void comm_time_create(Request *r,Auth *a,sqlite3_int64 ticket_id) {
    json_object *ticket=comm_ticket(r,a,ticket_id); if (!ticket) return;
    sqlite3_int64 job=json_object_get_int64(json_object_object_get(ticket,"job_id")); json_object_put(ticket);
    if (!job) { error_response(r,409,"Collega il ticket a una commessa prima di registrare le ore."); return; }
    PuntoScope scope; if (!punto_load_scope(r,a,&scope)) return;
    if (!scope.connected || !scope.time_access || !punto_job_allowed(job,&scope)) { OPENSSL_cleanse(&scope,sizeof(scope)); error_response(r,403,"Collega Punto e autorizza esplicitamente la registrazione delle ore."); return; }
    json_object *input=parse_json(r); if (!input) { OPENSSL_cleanse(&scope,sizeof(scope)); return; }
    const char *const keys[]={"idempotency_key","date","minutes","description","confirmed",NULL};
    const char *key=NULL,*date=NULL,*description=NULL; sqlite3_int64 minutes=0; json_object *confirmed=NULL;
    bool valid=allowed_keys(input,keys) && get_string(input,"idempotency_key",&key,64,true,true) && punto_hex(key) &&
        get_string(input,"date",&date,10,true,true) && valid_date(date) && punto_integer(input,"minutes",1,&minutes) && minutes<=1440 &&
        get_string(input,"description",&description,1000,true,true) && json_object_object_get_ex(input,"confirmed",&confirmed) && confirmed &&
        json_object_is_type(confirmed,json_type_boolean) && json_object_get_boolean(confirmed);
    if (!valid) { json_object_put(input); OPENSSL_cleanse(&scope,sizeof(scope)); error_response(r,400,"Conferma data, minuti e descrizione dell'attività svolta."); return; }
    char request_key[65],seed[180]; snprintf(seed,sizeof(seed),"%lld:%s",(long long)ticket_id,key);
    if (!digest_hex(seed,request_key) || !exec_sql("BEGIN IMMEDIATE")) goto fail;
    sqlite3_stmt *st=prepare("SELECT id,owner_user_id,origin,job_id,date,minutes,description,state,author_id FROM ticket_time_entries WHERE ticket_id=?1 AND idempotency_key=?2");
    if (!st) goto rollback;
    sqlite3_bind_int64(st,1,ticket_id); bind_text(st,2,key); int rc=sqlite3_step(st); sqlite3_int64 local_id=0; bool same=true,applied=false;
    if (rc==SQLITE_ROW) {
        local_id=sqlite3_column_int64(st,0);
        same=sqlite3_column_int64(st,8)==a->id && sqlite3_column_int64(st,1)==scope.user_id && !strcmp(column_text(st,2),punto_client_origin()) && sqlite3_column_int64(st,3)==job &&
            !strcmp(column_text(st,4),date) && sqlite3_column_int64(st,5)==minutes && !strcmp(column_text(st,6),description);
        applied=!strcmp(column_text(st,7),"applied");
    }
    sqlite3_finalize(st);
    if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) goto rollback;
    if (!same) { exec_sql("ROLLBACK"); json_object_put(input); OPENSSL_cleanse(&scope,sizeof(scope)); error_response(r,409,"La chiave identifica già ore diverse o un altro account Punto."); return; }
    if (!local_id) {
        st=prepare("INSERT INTO ticket_time_entries(ticket_id,author_id,owner_user_id,origin,job_id,idempotency_key,request_key,date,minutes,description,state) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,'pending')");
        if (!st) goto rollback;
        sqlite3_bind_int64(st,1,ticket_id); sqlite3_bind_int64(st,2,a->id); sqlite3_bind_int64(st,3,scope.user_id); bind_text(st,4,punto_client_origin());
        sqlite3_bind_int64(st,5,job); bind_text(st,6,key); bind_text(st,7,request_key); bind_text(st,8,date); sqlite3_bind_int64(st,9,minutes); bind_text(st,10,description);
        rc=sqlite3_step(st); sqlite3_finalize(st); if (rc!=SQLITE_DONE) goto rollback; local_id=sqlite3_last_insert_rowid(db);
    }
    if (!exec_sql("COMMIT")) goto rollback;
    if (applied) {
        json_object *o=json_object_new_object(); json_object_object_add(o,"entry",comm_fetch_time(local_id)); json_object_object_add(o,"replayed",json_object_new_boolean(true));
        json_object_put(input); OPENSSL_cleanse(&scope,sizeof(scope)); send_json(r,200,o); return;
    }
    char query[140]; snprintf(query,sizeof(query),"request_key=%s&job_id=%lld",request_key,(long long)job);
    json_object *response=NULL; int status=punto_client_get(scope.grant,"time/entries",query,&response); sqlite3_int64 remote=0;
    bool recovered=status==200 && comm_time_applied(response,job,date,minutes,description,&remote);
    json_object *found=NULL; bool definitely_absent=status==200 && response && json_object_object_get_ex(response,"found",&found) && found && json_object_is_type(found,json_type_boolean) && !json_object_get_boolean(found);
    if (response) json_object_put(response);
    response=NULL;
    if (!recovered && definitely_absent) {
        json_object *entries=json_object_new_array(),*entry=json_object_new_object(); json_object_array_add(entries,entry);
        add_int(entry,"job_id",job); add_string(entry,"date",date); add_int(entry,"minutes",minutes); add_string(entry,"note",description);
        char *encoded=calendar_encoded(json_object_to_json_string_ext(entries,JSON_C_TO_STRING_PLAIN)); json_object_put(entries);
        if (!encoded) goto fail;
        size_t size=strlen(encoded)+strlen(request_key)+32; char *form=malloc(size);
        if (!form) { free(encoded); goto fail; }
        snprintf(form,size,"request_key=%s&entries=%s",request_key,encoded); free(encoded);
        status=punto_client_post(scope.grant,"time/create",form,&response); free(form);
        /* The authoritative exact-key lookup also validates the returned actor/job payload. */
        if (response) json_object_put(response);
        response=NULL;
        if (status==200 || status==201) {
            status=punto_client_get(scope.grant,"time/entries",query,&response);
            recovered=status==200 && comm_time_applied(response,job,date,minutes,description,&remote);
        }
    }
    if (response) json_object_put(response);
    const char *state=recovered?"applied":status>=400 && status<500?"failed":"unknown";
    const char *error=recovered?"":status==409?"Punto ha rifiutato la registrazione: verifica commessa, data e limite giornaliero.":"Esito da verificare in Punto. Riprova con la stessa chiave, senza duplicare l'attività.";
    if (!exec_sql("BEGIN IMMEDIATE")) goto fail;
    st=prepare("UPDATE ticket_time_entries SET state=?1,punto_entry_id=?2,error=?3,revision=revision+1 WHERE id=?4");
    if (!st) goto rollback;
    bind_text(st,1,state); if (recovered) sqlite3_bind_int64(st,2,remote); else sqlite3_bind_null(st,2); bind_text(st,3,error); sqlite3_bind_int64(st,4,local_id);
    rc=sqlite3_step(st); sqlite3_finalize(st);
    if (rc!=SQLITE_DONE || !add_history(ticket_id,a,"work_logged",recovered?"Ore effettive registrate nella commessa Punto.":error) || !exec_sql("COMMIT")) goto rollback;
    json_object_put(input); OPENSSL_cleanse(&scope,sizeof(scope));
    if (!recovered) { error_response(r,status>=400 && status<500?status:503,error); return; }
    json_object *o=json_object_new_object(); json_object_object_add(o,"entry",comm_fetch_time(local_id)); json_object_object_add(o,"replayed",json_object_new_boolean(recovered && definitely_absent==false)); send_json(r,201,o); return;
rollback:
    exec_sql("ROLLBACK");
fail:
    json_object_put(input); OPENSSL_cleanse(&scope,sizeof(scope)); db_error(r);
}
static bool communications_route(Request *r,Auth *a) {
    bool get=!strcmp(r->method,"GET"),post=!strcmp(r->method,"POST"),put=!strcmp(r->method,"PUT");
    sqlite3_int64 id; const char *suffix;
    if (!strcmp(r->path,"/api/response-templates")) {
        if (!comm_no_query(r)) return true;
        if (get || post) comm_templates(r,0,get); else error_response(r,405,"Metodo non consentito.");
        return true;
    }
    if (path_id(r->path,"/api/response-templates/",&id,&suffix) && !*suffix) {
        if (!comm_no_query(r)) return true;
        if (put) comm_templates(r,id,false); else error_response(r,405,"Metodo non consentito.");
        return true;
    }
    if (!strcmp(r->path,"/api/solutions")) { if (get) comm_solutions(r,a); else error_response(r,405,"Metodo non consentito."); return true; }
    if (!path_id(r->path,"/api/tickets/",&id,&suffix)) return false;
    if (!strcmp(suffix,"/conversation") || !strcmp(suffix,"/messages") || !strcmp(suffix,"/time") || !strcmp(suffix,"/solution")) {
        if (!comm_no_query(r)) return true;
        if (!strcmp(suffix,"/conversation") && get) comm_conversation(r,a,id);
        else if (!strcmp(suffix,"/messages") && post) comm_save_message(r,a,id,0);
        else if (!strcmp(suffix,"/time") && get) comm_time_list(r,a,id);
        else if (!strcmp(suffix,"/time") && post) comm_time_create(r,a,id);
        else if (!strcmp(suffix,"/solution") && post) comm_save_solution(r,a,id);
        else error_response(r,405,"Metodo non consentito.");
        return true;
    }
    sqlite3_int64 message; const char *tail;
    if (path_id(suffix,"/messages/",&message,&tail)) {
        if (!comm_no_query(r)) return true;
        if (!*tail && put) comm_save_message(r,a,id,message);
        else if (!strcmp(tail,"/send") && post) comm_send_message(r,a,id,message);
        else error_response(r,*tail && strcmp(tail,"/send")?404:405,"Risorsa o metodo non consentito.");
        return true;
    }
    return false;
}
