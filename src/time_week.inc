#include "improvements_integrations.h"
/* Personal weekly timesheets and atomic, idempotent multi-row insertion.
   Included in workspace.c to share its validators and transaction connection. */
static struct json_object *workflow_json(const char *text) {
    struct json_tokener *tok=json_tokener_new();
    if(!tok) return NULL;
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    size_t length=strlen(text);
    struct json_object *value=json_tokener_parse_ex(tok,text,(int)length);
    size_t end=json_tokener_get_parse_end(tok);
    bool okay=json_tokener_get_error(tok)==json_tokener_success;
    while(end<length && (text[end]==' '||text[end]=='\n'||text[end]=='\r'||text[end]=='\t')) end++;
    json_tokener_free(tok);
    if(!okay||end!=length){if(value)json_object_put(value);return NULL;}
    return value;
}

static bool workflow_integer(struct json_object *obj,const char *key,DbInt *out,bool optional) {
    struct json_object *value=NULL;
    if(!json_object_object_get_ex(obj,key,&value)||json_object_is_type(value,json_type_null)){
        *out=0;return optional;
    }
    if(!json_object_is_type(value,json_type_int))return false;
    *out=json_object_get_int64(value);return *out>=0;
}

static const char *workflow_text(struct json_object *obj,const char *key) {
    struct json_object *value=NULL;
    if(!json_object_object_get_ex(obj,key,&value))return "";
    if(!json_object_is_type(value,json_type_string))return NULL;
    const char *text=json_object_get_string(value);
    return strlen(text)==(size_t)json_object_get_string_len(value)?text:NULL;
}

static void workflow_date_from_ordinal(int ordinal,char out[11]) {
    int low=1,high=9999;
    while(low<high){int mid=(low+high+1)/2;int prior=mid-1;
        int first=prior*365+prior/4-prior/100+prior/400+1;
        if(first<=ordinal)low=mid;else high=mid-1;
    }
    int year=low,prior=year-1,day=ordinal-(prior*365+prior/4-prior/100+prior/400),month=1;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    for(;month<12;month++){int n=days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));if(day<=n)break;day-=n;}
    (void)snprintf(out,11,"%04d-%02d-%02d",year,month,day);
}

static int workflow_job_permission(DbInt job_id,DbInt actor_id,bool manager,const char *today,const char *date) {
    DbStmt *st=NULL;
    if(!prepare(&st,"SELECT time_closed,archived FROM jobs WHERE id=?")||!bind_id(st,1,job_id)){db_finalize(st);return -1;}
    int result=db_step(st);
    bool closed=result==DB_ROW&&(db_column_int(st,0)||db_column_int(st,1));
    db_finalize(st);
    if(result==DB_DONE)return 0;
    if(result!=DB_ROW)return -1;
    if(closed)return 1;
    if(!improvements_integrations_time_open(database,job_id,date))return 1;
    if(manager){int managed=access_policy_can_manage_job(database,actor_id,job_id);if(managed<0)return -1;if(managed==1)return 3;}
    return organization_job_access(database,job_id,actor_id,today,date);
}

static void week_entries(const char *query,DbInt actor_id,bool manager,WorkspaceResponse *response) {
    Field fields[]={{"date",NULL},{"user_id",NULL}};DbInt target=actor_id;
    if(!parse_fields(query,fields,2,response))return;
    if(!valid_date(field_value(fields))||(fields[1].value&&!parse_positive(fields[1].value,&target))){free_fields(fields,2);error(response,400,"Seleziona una data valida.");return;}
    int visible=access_policy_can_view_time(database,actor_id,target,manager);
    if(visible!=1){free_fields(fields,2);if(visible<0)server_error(response);else error(response,403,"Puoi consultare solo le tue ore o quelle della tua squadra.");return;}
    actor_id=target;
    int ordinal=report_date_ordinal(fields[0].value),first=ordinal-(ordinal-1)%7;
    free_fields(fields,2);
    if(first+6>report_date_ordinal("9999-12-31")){error(response,400,"La settimana supera l'intervallo supportato.");return;}
    char dates[7][11];DbInt totals[7]={0},total=0;
    for(int i=0;i<7;i++)workflow_date_from_ordinal(first+i,dates[i]);
    DbStmt *st=NULL;
    if(!prepare(&st,"SELECT t.id FROM time_entries t WHERE t.user_id=? AND t.date>=? AND t.date<=? ORDER BY t.date,t.id LIMIT 7001")||
       !bind_id(st,1,actor_id)||!bind_text(st,2,dates[0])||!bind_text(st,3,dates[6]))goto failure;
    /* Collect identifiers before snapshot queries to remain portable between drivers. */
    DbInt ids[7000];int count=0,result;
    while((result=db_step(st))==DB_ROW){if(count>=7000)break;ids[count++]=db_column_int64(st,0);}
    db_finalize(st);st=NULL;
    if(result!=DB_DONE){error(response,409,"La settimana contiene troppe registrazioni.");return;}
    Json json={0};append(&json,"{\"ok\":true,\"start\":");append_string(&json,dates[0]);append(&json,",\"end\":");append_string(&json,dates[6]);append(&json,",\"entries\":[");
    for(int i=0;i<count;i++){
        Json entry={0};if(entry_snapshot(ids[i],actor_id,&entry)!=1){free(entry.data);free(json.data);goto failure;}
        struct json_object *value=workflow_json(entry.data);DbInt minutes=0;
        const char *date=value?workflow_text(value,"date"):NULL;
        if(!date||!workflow_integer(value,"minutes",&minutes,false)){if(value)json_object_put(value);free(entry.data);free(json.data);goto failure;}
        int day=report_date_ordinal(date)-first;if(day<0||day>6){json_object_put(value);free(entry.data);free(json.data);goto failure;}
        totals[day]+=minutes;total+=minutes;json_object_put(value);
        if(i) append(&json,",");
        append(&json,entry.data);free(entry.data);
    }
    append(&json,"],\"days\":[");for(int i=0;i<7;i++){if(i)append(&json,",");append(&json,"{\"date\":");append_string(&json,dates[i]);append(&json,",\"total_minutes\":");append_integer(&json,totals[i]);append(&json,"}");}
    append(&json,"],\"total_minutes\":");append_integer(&json,total);append(&json,"}");finish_json(response,200,&json);return;
failure:db_finalize(st);server_error(response);
}

typedef struct {DbInt job_id,minutes,task_id;const char *date,*note;} WorkflowEntry;

static void workflow_row_error(WorkspaceResponse *response, int status, const char *message,
                               size_t row, const char *date) {
    Json json = {0};
    append(&json, "{\"ok\":false,\"error\":");
    append_string(&json, message);
    append(&json, ",\"row\":");
    append_integer(&json, (DbInt)row);
    append(&json, ",\"date\":");
    if (date) append_string(&json, date);
    else append(&json, "null");
    append(&json, "}");
    finish_json(response, status, &json);
}

static void batch_entries(const char *body,DbInt actor_id,const char *owner,bool manager,WorkspaceResponse *response) {
    Field fields[]={{"request_key",NULL},{"entries",NULL}};
    if(!parse_fields(body,fields,2,response))return;
    const char *key=field_value(&fields[0]),*payload=field_value(&fields[1]);
    size_t key_length=strlen(key);bool key_valid=key_length>=8&&key_length<=128;
    for(size_t i=0;i<key_length;i++)if(!((key[i]>='a'&&key[i]<='z')||(key[i]>='A'&&key[i]<='Z')||(key[i]>='0'&&key[i]<='9')||key[i]=='-'||key[i]=='_'))key_valid=false;
    struct json_object *array=workflow_json(payload);WorkflowEntry entries[100];size_t count=array&&json_object_is_type(array,json_type_array)?json_object_array_length(array):0;
    if(!key_valid||count<1||count>100)goto invalid;
    for(size_t i=0;i<count;i++){
        struct json_object *item=json_object_array_get_idx(array,i);
        if(!json_object_is_type(item,json_type_object)||!workflow_integer(item,"job_id",&entries[i].job_id,false)||entries[i].job_id<1||
           !workflow_integer(item,"minutes",&entries[i].minutes,false)||entries[i].minutes<1||entries[i].minutes>1440||
           !workflow_integer(item,"task_id",&entries[i].task_id,true))goto invalid;
        entries[i].date=workflow_text(item,"date");entries[i].note=workflow_text(item,"note");
        if(!valid_date(entries[i].date)||!entries[i].note||!valid_text(entries[i].note,1000,false,true))goto invalid;
        json_object_object_foreach(item,field,value){(void)value;if(strcmp(field,"job_id")&&strcmp(field,"minutes")&&strcmp(field,"date")&&strcmp(field,"note")&&strcmp(field,"task_id"))goto invalid;}
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];char hash[65];SHA256((const unsigned char*)payload,strlen(payload),digest);
    for(size_t i=0;i<sizeof(digest);i++)(void)snprintf(hash+2*i,3,"%02x",digest[i]);
    char today[11];if(!today_date(today))goto early_failure;
    if(!begin_transaction(response))goto cleanup;
    DbStmt *st=NULL;Json ids={0};
#ifdef PUNTO_MYSQL
    if(!prepare(&st,"SELECT id FROM users WHERE id=? FOR UPDATE")||!bind_id(st,1,actor_id)||db_step(st)!=DB_ROW)goto failure;
    db_finalize(st);st=NULL;
#endif
    if(!prepare(&st,"SELECT payload_hash,ids_json FROM time_entry_batches WHERE user_id=? AND request_key=?")||!bind_id(st,1,actor_id)||!bind_text(st,2,key))goto failure;
    int result=db_step(st);
    if(result==DB_ROW){
        bool same=!strcmp(hash,column_text(st,0));char *saved=duplicate(column_text(st,1));db_finalize(st);st=NULL;rollback();
        if(!saved)server_error(response);else if(!same)error(response,409,"Questo identificativo è già stato usato per dati diversi.");
        else{Json out={0};append(&out,"{\"ok\":true,\"replayed\":true,\"ids\":");append(&out,saved);append(&out,"}");finish_json(response,200,&out);}free(saved);goto cleanup;
    }
    db_finalize(st);st=NULL;if(result!=DB_DONE)goto failure;
    for(size_t i=0;i<count;i++){
        int permission=workflow_job_permission(entries[i].job_id,actor_id,manager,today,entries[i].date);
        if(permission<0)goto failure;
        if(!(permission&2)){rollback();workflow_row_error(response,403,"Una riga usa una commessa chiusa, archiviata o non autorizzata nella data lavorata. Nessuna ora salvata.",i+1,entries[i].date);goto done;}
        if(entries[i].task_id){
            if(!prepare(&st,"SELECT 1 FROM job_tasks WHERE id=? AND job_id=?")||!bind_id(st,1,entries[i].task_id)||!bind_id(st,2,entries[i].job_id))goto failure;
            result=db_step(st);db_finalize(st);st=NULL;
            if(result==DB_DONE){rollback();workflow_row_error(response,400,"Una attività non appartiene alla commessa. Nessuna ora salvata.",i+1,entries[i].date);goto done;}if(result!=DB_ROW)goto failure;
        }
        if(!prepare(&st,"SELECT COALESCE(SUM(minutes),0),COUNT(*) FROM time_entries WHERE user_id=? AND date=?")||!bind_id(st,1,actor_id)||!bind_text(st,2,entries[i].date)||db_step(st)!=DB_ROW)goto failure;
        DbInt total=db_column_int64(st,0),rows=db_column_int64(st,1);db_finalize(st);st=NULL;
        if(total>1440-entries[i].minutes||rows>=1000){rollback();workflow_row_error(response,409,"Il totale supera 24 ore o 1000 registrazioni in una giornata. Nessuna ora salvata.",i+1,entries[i].date);goto done;}
        if(!prepare(&st,"INSERT INTO time_entries(job_id,user_id,owner,date,minutes,note,task_id) VALUES(?,?,?,?,?,?,?)")||
           !bind_id(st,1,entries[i].job_id)||!bind_id(st,2,actor_id)||!bind_text(st,3,owner)||!bind_text(st,4,entries[i].date)||!bind_id(st,5,entries[i].minutes)||!bind_text(st,6,entries[i].note)||
           (entries[i].task_id?!bind_id(st,7,entries[i].task_id):db_bind_null(st,7)!=DB_OK)||db_step(st)!=DB_DONE)goto failure;
        DbInt id=db_last_insert_rowid(database);db_finalize(st);st=NULL;
        Json snapshot={0};bool okay=entry_snapshot(id,actor_id,&snapshot)==1&&storage_audit(database,actor_id,"time_entries.create","time_entry",id,NULL,snapshot.data);free(snapshot.data);if(!okay)goto failure;
        if(i) append(&ids,",");
        append_integer(&ids,id);
    }
    if(ids.failed)goto failure;
    Json ids_array={0};append(&ids_array,"[");append(&ids_array,ids.data);append(&ids_array,"]");
    if(ids_array.failed||!prepare(&st,"INSERT INTO time_entry_batches(user_id,request_key,payload_hash,ids_json) VALUES(?,?,?,?)")||!bind_id(st,1,actor_id)||!bind_text(st,2,key)||!bind_text(st,3,hash)||!bind_text(st,4,ids_array.data)||db_step(st)!=DB_DONE){free(ids_array.data);goto failure;}
    db_finalize(st);st=NULL;
    if(commit_transaction(response)){Json out={0};append(&out,"{\"ok\":true,\"ids\":");append(&out,ids_array.data);append(&out,"}");finish_json(response,201,&out);}free(ids_array.data);goto done;
failure:db_finalize(st);rollback();server_error(response);
done:free(ids.data);
cleanup:if(array)json_object_put(array);free_fields(fields,2);return;
invalid:error(response,400,"Invia da 1 a 100 righe valide e un identificativo univoco della richiesta.");goto cleanup;
early_failure:server_error(response);goto cleanup;
}

static bool time_week_handle(const char *method,const char *target,const char *query,const char *body,DbInt actor_id,const char *owner,bool manager,WorkspaceResponse *response){
    bool week=!strcmp(target,"/api/time-entries/week"),batch=!strcmp(target,"/api/time-entries/batch");
    if(!week&&!batch)return false;
    if(week&&!strcmp(method,"GET"))week_entries(query,actor_id,manager,response);
    else if(batch&&!strcmp(method,"POST")&&!*query)batch_entries(body,actor_id,owner,manager,response);
    else error(response,*query&&batch?400:405,"Metodo o parametri non consentiti.");
    return true;
}
