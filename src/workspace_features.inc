/* Personal work queue and guided, transactional job setup. */
static void features_finish(WorkspaceResponse *response,int status,struct json_object *value){
    if(!value){server_error(response);return;}
    response->body=duplicate(json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN));
    response->status=response->body?status:500;json_object_put(value);
}

static struct json_object *features_row(DbStmt *st,const char **names,int count){
    struct json_object *row=json_object_new_object();if(!row)return NULL;
    for(int i=0;i<count;i++){
        struct json_object *value=db_column_type(st,i)==DB_NULL?NULL:
            db_column_type(st,i)==DB_INTEGER?json_object_new_int64(db_column_int64(st,i)):json_object_new_string(column_text(st,i));
        json_object_object_add(row,names[i],value);
    }
    return row;
}

static void personal_work(DbInt actor_id,bool manager,WorkspaceResponse *response){
    char today[11];if(!today_date(today)){server_error(response);return;}
    OrganizationAccess access={0};
    (void)manager;
    if(!organization_access_list(database,actor_id,today,NULL,&access)){server_error(response);return;}
    struct json_object *out=json_object_new_object(),*tasks=json_object_new_array(),*blocks=json_object_new_array();DbStmt *st=NULL;
    if(!out||!tasks||!blocks)goto failure;
    json_object_object_add(out,"today",json_object_new_string(today));
    for(int kind=0;kind<2;kind++){
        const char *sql=kind?
            "SELECT b.id,b.job_id,j.code,j.name,b.title,b.status,b.impact,b.due_date,b.next_action FROM job_blockers b JOIN jobs j ON j.id=b.job_id WHERE (b.owner_id=? OR b.created_by=?) AND b.status<>'resolved' AND j.archived=0 ORDER BY CASE WHEN b.due_date IS NULL THEN 1 ELSE 0 END,b.due_date,b.id":
            "SELECT t.id,t.job_id,j.code,j.name,t.title,t.status,t.priority,t.due_date,t.revision FROM job_tasks t JOIN jobs j ON j.id=t.job_id WHERE t.assigned_user_id=? AND j.archived=0 AND t.status<>'done' ORDER BY CASE WHEN t.due_date IS NULL THEN 1 ELSE 0 END,t.due_date,t.id";
        if(!prepare(&st,sql)||!bind_id(st,1,actor_id)||(kind&&!bind_id(st,2,actor_id)))goto failure;
        const char *names[]={"id","job_id","job_code","job_name","title","status",kind?"impact":"priority","due_date",kind?"next_action":"revision"};
        int result,count=0;
        while((result=db_step(st))==DB_ROW){
            DbInt job_id=db_column_int64(st,1);int managed=access_policy_can_manage_job(database,actor_id,job_id);if(managed<0)goto failure;bool allowed=managed==1;
            for(size_t i=0;!allowed&&i<access.count;i++)if(access.job_ids[i]==job_id&&(access.permissions[i]&1))allowed=true;
            if(!allowed) continue;
            count++;
            if(count<=200){struct json_object *row=features_row(st,names,9);if(!row)goto failure;json_object_array_add(kind?blocks:tasks,row);}
        }
        db_finalize(st);st=NULL;if(result!=DB_DONE)goto failure;
        json_object_object_add(out,kind?"blockers_count":"tasks_count",json_object_new_int(count));
        json_object_object_add(out,kind?"blockers_more":"tasks_more",json_object_new_boolean(count>200));
    }
    if(!prepare(&st,"SELECT COALESCE(SUM(minutes),0) FROM time_entries WHERE user_id=? AND date=?")||!bind_id(st,1,actor_id)||!bind_text(st,2,today)||db_step(st)!=DB_ROW)goto failure;
    json_object_object_add(out,"today_minutes",json_object_new_int64(db_column_int64(st,0)));db_finalize(st);st=NULL;
    json_object_object_add(out,"tasks",tasks);tasks=NULL;json_object_object_add(out,"blockers",blocks);blocks=NULL;
    features_finish(response,200,out);return;
failure:db_finalize(st);if(out)json_object_put(out);if(tasks)json_object_put(tasks);if(blocks)json_object_put(blocks);server_error(response);
}

static bool features_dates(struct json_object *item,const char **starts,const char **ends){
    *starts=workflow_text(item,"starts_on");*ends=workflow_text(item,"ends_on");
    return *starts&&*ends&&(!**starts||valid_date(*starts))&&(!**ends||valid_date(*ends))&&(!**starts||!**ends||strcmp(*starts,*ends)<=0);
}

static bool features_array(struct json_object *value,size_t maximum){return value&&json_object_is_type(value,json_type_array)&&json_object_array_length(value)<=maximum;}

static void job_templates(const char *method,const char *body,DbInt actor_id,WorkspaceResponse *response){
    AccessPolicy policy;
    if(!workspace_policy(actor_id,&policy)){server_error(response);return;}
    DbStmt *st=NULL;
    if(!strcmp(method,"GET")){
        if(!prepare(&st,"SELECT id,name,definition_json,created_by,created_at,revision,archived FROM job_templates WHERE archived=0 AND (?=1 OR created_by=?) ORDER BY name LIMIT 201")||!bind_id(st,1,policy.admin?1:0)||!bind_id(st,2,actor_id))goto failure;
        struct json_object *out=json_object_new_object(),*items=json_object_new_array();int result,count=0;
        if(!out||!items){if(out)json_object_put(out);if(items)json_object_put(items);goto failure;}
        const char *names[]={"id","name","definition_json","created_by","created_at","revision","archived"};
        while((result=db_step(st))==DB_ROW){
            if(++count>200)break;
            struct json_object *row=features_row(st,names,7);
            if(row&&!policy.sales&&!policy.finance){
                struct json_object *definition=workflow_json(column_text(st,2));
                if(!definition){json_object_put(row);json_object_put(out);json_object_put(items);goto failure;}
                json_object_object_del(definition,"contract_id");
                json_object_object_add(row,"definition_json",json_object_new_string(json_object_to_json_string_ext(definition,JSON_C_TO_STRING_PLAIN)));
                json_object_put(definition);
            }
            json_object_array_add(items,row);
        }
        db_finalize(st);st=NULL;
        if(result!=DB_DONE){json_object_put(out);json_object_put(items);error(response,409,"Sono presenti troppi modelli.");return;}
        json_object_object_add(out,"templates",items);features_finish(response,200,out);return;
    }
    if(strcmp(method,"POST")){error(response,405,"Metodo non consentito.");return;}
    Field fields[]={{"name",NULL},{"definition",NULL}};
    if(!parse_fields(body,fields,2,response))return;
    trim(fields[0].value);const char *name=field_value(&fields[0]);struct json_object *definition=workflow_json(field_value(&fields[1]));
    bool valid=valid_text(name,120,true,false)&&definition&&json_object_is_type(definition,json_type_object)&&strlen(field_value(&fields[1]))<=32768;
    if(valid){json_object_object_foreach(definition,key,value){(void)value;if(strcmp(key,"name")&&strcmp(key,"client")&&strcmp(key,"description")&&strcmp(key,"status")&&strcmp(key,"contract_id")&&strcmp(key,"is_common")&&strcmp(key,"assignments")&&strcmp(key,"groups")&&strcmp(key,"tasks"))valid=false;}}
    if(valid&&!policy.sales){
        struct json_object *contract=NULL;
        if(json_object_object_get_ex(definition,"contract_id",&contract)&&contract&&
           strcmp(json_object_get_string(contract),"")&&strcmp(json_object_get_string(contract),"0")){
            json_object_put(definition);free_fields(fields,2);error(response,403,"Solo Commerciali può modificare il contratto collegato alla commessa.");return;
        }
    }
    if(!valid){if(definition)json_object_put(definition);free_fields(fields,2);error(response,400,"Il modello deve contenere soltanto dati di commessa, squadra e attività iniziali.");return;}
    if(!begin_transaction(response)){json_object_put(definition);free_fields(fields,2);return;}
    if(!prepare(&st,"SELECT COUNT(*) FROM job_templates")||db_step(st)!=DB_ROW)goto template_failure;
    DbInt existing=db_column_int64(st,0);db_finalize(st);st=NULL;
    if(existing>=200){rollback();error(response,409,"Hai raggiunto il limite di 200 modelli.");goto template_done;}
    if(!prepare(&st,"INSERT INTO job_templates(name,definition_json,created_by) VALUES(?,?,?)")||!bind_text(st,1,name)||!bind_text(st,2,json_object_to_json_string_ext(definition,JSON_C_TO_STRING_PLAIN))||!bind_id(st,3,actor_id))goto template_failure;
    int result=db_step(st);db_finalize(st);st=NULL;
    if(result!=DB_DONE){int code=db_extended_errcode(database);rollback();if(code==DB_CONSTRAINT_UNIQUE)error(response,409,"Esiste già un modello con questo nome.");else server_error(response);goto template_done;}
    DbInt id=db_last_insert_rowid(database);
    if(!storage_audit(database,actor_id,"job_templates.create","job_template",id,NULL,json_object_to_json_string_ext(definition,JSON_C_TO_STRING_PLAIN)))goto template_failure;
    if(commit_transaction(response)) success(response,201,id);
    goto template_done;
template_failure:db_finalize(st);rollback();server_error(response);
template_done:json_object_put(definition);free_fields(fields,2);return;
failure:db_finalize(st);server_error(response);
}

static void guided_job(const char *body,DbInt actor_id,WorkspaceResponse *response){
    AccessPolicy policy;
    if(!workspace_policy(actor_id,&policy)){server_error(response);return;}
    Field f[]={{"code",NULL},{"name",NULL},{"client",NULL},{"description",NULL},{"status",NULL},{"contract_id",NULL},{"is_common",NULL},{"assignments",NULL},{"groups",NULL},{"tasks",NULL},{"request_key",NULL}};
    if(!parse_fields(body,f,11,response))return;
    trim(f[0].value);trim(f[1].value);trim(f[2].value);
    DbInt contract_id=0;int common=!strcmp(field_value(&f[6]),"1");
    const char *request_key=field_value(&f[10]);size_t key_length=strlen(request_key);bool key_valid=key_length>=8&&key_length<=100;
    for(size_t i=0;i<key_length;i++)if(!((request_key[i]>='a'&&request_key[i]<='z')||(request_key[i]>='A'&&request_key[i]<='Z')||(request_key[i]>='0'&&request_key[i]<='9')||request_key[i]=='-'||request_key[i]=='_'))key_valid=false;
    struct json_object *users=workflow_json(*field_value(&f[7])?f[7].value:"[]"),*groups=workflow_json(*field_value(&f[8])?f[8].value:"[]"),*tasks=workflow_json(*field_value(&f[9])?f[9].value:"[]");
    if(*field_value(&f[5])&&strcmp(field_value(&f[5]),"0")&&!policy.sales){error(response,403,"Solo Commerciali può modificare il contratto collegato alla commessa.");goto cleanup;}
    if(!key_valid||!valid_text(field_value(&f[0]),40,true,false)||!valid_text(field_value(&f[1]),120,true,false)||!valid_text(field_value(&f[2]),120,false,false)||!valid_text(field_value(&f[3]),1000,false,true)||!valid_status(field_value(&f[4]))||
       (*field_value(&f[5])&&!parse_positive(f[5].value,&contract_id))||(*field_value(&f[6])&&strcmp(f[6].value,"0")&&strcmp(f[6].value,"1"))||!features_array(users,200)||!features_array(groups,200)||!features_array(tasks,100))goto invalid;
    if(!organization_groups_available()&&json_object_array_length(groups)){error(response,409,"I gruppi aziendali non sono disponibili: riprova dopo la sincronizzazione.");goto cleanup;}
    unsigned char digest[SHA256_DIGEST_LENGTH];char hash[65],receipt[128];SHA256((const unsigned char*)body,strlen(body),digest);
    for(size_t i=0;i<sizeof(digest);i++)(void)snprintf(hash+2*i,3,"%02x",digest[i]);
    (void)snprintf(receipt,sizeof(receipt),"wizard:%s",request_key);
    if(!begin_transaction(response))goto cleanup;
    DbStmt *st=NULL;Json snapshot={0};
    if(access_policy_can_manage_job(database,actor_id,0)!=1){rollback();error(response,403,"Operazione riservata ai gestori delegati.");goto cleanup;}
#ifdef PUNTO_MYSQL
    if(!prepare(&st,"SELECT id FROM users WHERE id=? FOR UPDATE")||!bind_id(st,1,actor_id)||db_step(st)!=DB_ROW)goto failure;
    db_finalize(st);st=NULL;
#endif
    if(!prepare(&st,"SELECT payload_hash,ids_json FROM time_entry_batches WHERE user_id=? AND request_key=?")||!bind_id(st,1,actor_id)||!bind_text(st,2,receipt))goto failure;
    int result=db_step(st);
    if(result==DB_ROW){bool same=!strcmp(hash,column_text(st,0));struct json_object *saved=workflow_json(column_text(st,1));DbInt id=saved&&json_object_array_length(saved)==1?json_object_get_int64(json_object_array_get_idx(saved,0)):0;db_finalize(st);st=NULL;rollback();if(saved)json_object_put(saved);if(same&&id)success(response,200,id);else error(response,409,"Identificativo già utilizzato per una diversa creazione.");goto cleanup;}
    db_finalize(st);st=NULL;if(result!=DB_DONE)goto failure;
    if(!prepare(&st,"SELECT COUNT(*) FROM jobs")||db_step(st)!=DB_ROW)goto failure;
    DbInt count=db_column_int64(st,0);db_finalize(st);st=NULL;
    if(count>=1000){rollback();error(response,409,"Hai raggiunto il limite di 1000 commesse.");goto cleanup;}
    if(contract_id){
        if(!prepare(&st,"SELECT 1 FROM contracts WHERE id=? AND status NOT IN ('closed','cancelled')")||!bind_id(st,1,contract_id))goto failure;
        result=db_step(st);db_finalize(st);st=NULL;if(result==DB_DONE)goto transaction_invalid;if(result!=DB_ROW)goto failure;
    }
    if(!prepare(&st,"INSERT INTO jobs(code,name,client,description,status,contract_id,is_common) VALUES(?,?,?,?,?,?,?)")||!bind_text(st,1,field_value(&f[0]))||!bind_text(st,2,field_value(&f[1]))||!bind_text(st,3,field_value(&f[2]))||!bind_text(st,4,field_value(&f[3]))||!bind_text(st,5,field_value(&f[4]))||(contract_id?!bind_id(st,6,contract_id):db_bind_null(st,6)!=DB_OK)||!bind_id(st,7,common))goto failure;
    result=db_step(st);db_finalize(st);st=NULL;
    if(result!=DB_DONE){int code=db_extended_errcode(database);rollback();if(code==DB_CONSTRAINT_UNIQUE)error(response,409,"Esiste già una commessa con questo codice.");else server_error(response);goto cleanup;}
    DbInt job_id=db_last_insert_rowid(database);
    for(int kind=0;kind<2;kind++){
        struct json_object *array=kind?groups:users;
        for(size_t i=0;i<json_object_array_length(array);i++){
            struct json_object *item=json_object_array_get_idx(array,i);DbInt subject,role_id;const char *start,*end;
            if(!json_object_is_type(item,json_type_object)||!workflow_integer(item,kind?"group_id":"user_id",&subject,false)||subject<1||!workflow_integer(item,"role_id",&role_id,false)||role_id<1||!features_dates(item,&start,&end))goto transaction_invalid;
            int managed=kind?access_policy_can_manage_group(database,actor_id,subject):access_policy_can_manage_user(database,actor_id,subject);
            if(managed<0)goto failure;
            if(managed!=1){rollback();error(response,403,"La squadra selezionata non rientra nella tua delega.");goto cleanup;}
            if(!prepare(&st,kind?"SELECT 1 FROM organization_groups WHERE id=?":"SELECT 1 FROM users WHERE id=? AND active=1")||!bind_id(st,1,subject))goto failure;
            result=db_step(st);db_finalize(st);st=NULL;if(result==DB_DONE)goto transaction_invalid;if(result!=DB_ROW)goto failure;
            if(!prepare(&st,"SELECT 1 FROM job_roles WHERE id=?")||!bind_id(st,1,role_id))goto failure;
            result=db_step(st);db_finalize(st);st=NULL;if(result==DB_DONE)goto transaction_invalid;if(result!=DB_ROW)goto failure;
            if(!prepare(&st,kind?"INSERT INTO job_group_grants(job_id,group_id,role_id,starts_on,ends_on) VALUES(?,?,?,?,?)":"INSERT INTO job_assignments(job_id,user_id,role_id,starts_on,ends_on) VALUES(?,?,?,?,?)")||!bind_id(st,1,job_id)||!bind_id(st,2,subject)||!bind_id(st,3,role_id)||!bind_date(st,4,start)||!bind_date(st,5,end))goto failure;
            result=db_step(st);db_finalize(st);st=NULL;if(result!=DB_DONE){if(db_extended_errcode(database)==DB_CONSTRAINT_UNIQUE)goto transaction_invalid;goto failure;}
            const char *details=json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN);
            if(!storage_audit(database,actor_id,kind?"group_grants.create":"assignments.create",kind?"group_grant":"assignment",job_id,NULL,details))goto failure;
        }
    }
    int scoped=access_policy_can_manage_job(database,actor_id,job_id);
    if(scoped<0)goto failure;
    if(scoped!=1){rollback();error(response,403,"La squadra della commessa non appartiene al tuo ambito di gestione.");goto cleanup;}
    char today[11],utc[21];time_t now=time(NULL);struct tm tm;
    if(!today_date(today)||!gmtime_r(&now,&tm)||!strftime(utc,sizeof(utc),"%Y-%m-%dT%H:%M:%SZ",&tm))goto failure;
    for(size_t i=0;i<json_object_array_length(tasks);i++){
        struct json_object *item=json_object_array_get_idx(tasks,i);DbInt assignee=0;
        if(!json_object_is_type(item,json_type_object)||!workflow_integer(item,"assigned_user_id",&assignee,true))goto transaction_invalid;
        const char *title=workflow_text(item,"title"),*priority=workflow_text(item,"priority"),*due=workflow_text(item,"due_date");
        if(!title||!priority||!due||!valid_text(title,160,true,false)||(*due&&!valid_date(due)))goto transaction_invalid;
        if(!*priority)priority="normal";
        if(strcmp(priority,"low")&&strcmp(priority,"normal")&&strcmp(priority,"high")&&strcmp(priority,"urgent"))goto transaction_invalid;
        if(assignee){
            if(!prepare(&st,"SELECT active,role FROM users WHERE id=?")||!bind_id(st,1,assignee))goto failure;
            result=db_step(st);bool active=result==DB_ROW&&db_column_int(st,0),is_manager=result==DB_ROW&&!strcmp(column_text(st,1),"manager");db_finalize(st);st=NULL;
            if(result!=DB_ROW&&result!=DB_DONE) goto failure;
            if(!active) goto transaction_invalid;
            int managed=is_manager?access_policy_can_manage_job(database,assignee,job_id):0;if(managed<0)goto failure;
            if(managed!=1){int permission=organization_job_access(database,job_id,assignee,today,NULL);if(permission<0)goto failure;if(!(permission&2))goto transaction_invalid;}
        }
        if(!prepare(&st,"INSERT INTO job_tasks(job_id,assigned_user_id,title,priority,due_date,created_at,updated_at) VALUES(?,?,?,?,?,?,?)")||!bind_id(st,1,job_id)||(assignee?!bind_id(st,2,assignee):db_bind_null(st,2)!=DB_OK)||!bind_text(st,3,title)||!bind_text(st,4,priority)||!bind_date(st,5,due)||!bind_text(st,6,utc)||!bind_text(st,7,utc)||db_step(st)!=DB_DONE)goto failure;
        DbInt task_id=db_last_insert_rowid(database);db_finalize(st);st=NULL;
        if(!storage_audit(database,actor_id,"tasks.create","task",task_id,NULL,json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN)))goto failure;
        if(assignee){
            char event_key[64];
            (void)snprintf(event_key,sizeof(event_key),"assigned:%lld:1",(long long)task_id);
            if(!prepare(&st,"INSERT INTO work_notifications(user_id,job_id,task_id,event_key,type,title,message,work_date,created_at) VALUES(?,?,?,?,?,?,?,?,?)")||
               !bind_id(st,1,assignee)||!bind_id(st,2,job_id)||!bind_id(st,3,task_id)||!bind_text(st,4,event_key)||
               !bind_text(st,5,"assigned")||!bind_text(st,6,"Nuova attività assegnata")||!bind_text(st,7,title)||
               !bind_date(st,8,due)||!bind_text(st,9,utc)||db_step(st)!=DB_DONE)goto failure;
            db_finalize(st);st=NULL;
        }
    }
    if(job_snapshot(job_id,&snapshot)!=1||!storage_audit(database,actor_id,"jobs.create","job",job_id,NULL,snapshot.data))goto failure;
    char ids[32];(void)snprintf(ids,sizeof(ids),"[%lld]",(long long)job_id);
    if(!prepare(&st,"INSERT INTO time_entry_batches(user_id,request_key,payload_hash,ids_json) VALUES(?,?,?,?)")||!bind_id(st,1,actor_id)||!bind_text(st,2,receipt)||!bind_text(st,3,hash)||!bind_text(st,4,ids)||db_step(st)!=DB_DONE)goto failure;
    db_finalize(st);st=NULL;if(commit_transaction(response))success(response,201,job_id);free(snapshot.data);goto cleanup;
transaction_invalid:db_finalize(st);rollback();free(snapshot.data);error(response,400,"Controlla contratto, persone, gruppi, ruoli e attività. Nessuna commessa è stata creata.");goto cleanup;
failure:db_finalize(st);rollback();free(snapshot.data);server_error(response);goto cleanup;
invalid:error(response,400,"Completa dati, squadra e attività con valori validi e un identificativo univoco della richiesta.");
cleanup:if(users)json_object_put(users);if(groups)json_object_put(groups);if(tasks)json_object_put(tasks);free_fields(f,11);
}

static bool workspace_features_handle(const char *method,const char *target,const char *query,const char *body,DbInt actor_id,bool manager,WorkspaceResponse *response){
    bool mine=!strcmp(target,"/api/my-work"),wizard=!strcmp(target,"/api/jobs/wizard"),templates=!strcmp(target,"/api/job-templates");
    if(!mine&&!wizard&&!templates)return false;
    if(*query){error(response,400,"Parametri non validi.");return true;}
    if(mine){if(!strcmp(method,"GET"))personal_work(actor_id,manager,response);else error(response,405,"Metodo non consentito.");return true;}
    int manage=access_policy_can_manage_job(database,actor_id,0);
    if(manage!=1){if(manage<0)server_error(response);else error(response,403,"Operazione riservata ai gestori delegati.");return true;}
    if(templates)job_templates(method,body,actor_id,response);
    else if(!strcmp(method,"POST"))guided_job(body,actor_id,response);else error(response,405,"Metodo non consentito.");
    return true;
}
