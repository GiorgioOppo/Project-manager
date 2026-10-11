/* Direct asset responsibility is independent of identity membership and custody. */
static bool subjects_migrate(void) {
    return execute(
        "CREATE TABLE inventory_asset_subjects(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "asset_id INTEGER NOT NULL REFERENCES assets(id),person_id INTEGER REFERENCES inventory_people(id),identity_id INTEGER REFERENCES inventory_identities(id),"
        "role TEXT NOT NULL DEFAULT 'user' CHECK(role IN('owner','user','administrator')),notes TEXT NOT NULL DEFAULT '' CHECK(length(CAST(notes AS BLOB))<=2000),"
        "subject_snapshot TEXT NOT NULL CHECK(json_valid(subject_snapshot)),actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,"
        "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),removed_at TEXT,removed_actor_id INTEGER,"
        "CHECK((person_id>0 AND identity_id IS NULL) OR (identity_id>0 AND person_id IS NULL)),"
        "CHECK(person_id IS NOT NULL OR identity_id IS NOT NULL),"
        "CHECK((active=1 AND removed_at IS NULL AND removed_actor_id IS NULL) OR (active=0 AND removed_at IS NOT NULL AND removed_actor_id>0)));"
        "CREATE UNIQUE INDEX inventory_asset_subjects_person_pair ON inventory_asset_subjects(asset_id,person_id) WHERE active=1 AND person_id IS NOT NULL;"
        "CREATE UNIQUE INDEX inventory_asset_subjects_identity_pair ON inventory_asset_subjects(asset_id,identity_id) WHERE active=1 AND identity_id IS NOT NULL;"
        "CREATE INDEX inventory_asset_subjects_asset ON inventory_asset_subjects(asset_id,active,id);"
        "CREATE INDEX inventory_asset_subjects_person ON inventory_asset_subjects(person_id,active,id);"
        "CREATE INDEX inventory_asset_subjects_identity ON inventory_asset_subjects(identity_id,active,id);");
}
#define SUBJECT_COLUMNS "id,asset_id,person_id,identity_id,role,notes,subject_snapshot,actor_id,created_at,active,removed_at,removed_actor_id"
static json_object *subject_row(sqlite3_stmt *s, bool *error) {
    bool identity=sqlite3_column_type(s,3)!=SQLITE_NULL;
    sqlite3_int64 subject_id=sqlite3_column_int64(s,identity ? 3 : 2);
    json_object *subject=directory_load(identity,subject_id,error);
    json_object *asset=load_asset(sqlite3_column_int64(s,1),error);
    json_object *snapshot=json_tokener_parse((const char *)sqlite3_column_text(s,6));
    if (!subject || !asset || !snapshot || *error) {
        if (subject) json_object_put(subject);
        if (asset) json_object_put(asset);
        if (snapshot) json_object_put(snapshot);
        *error=true; return NULL;
    }
    json_object *o=json_object_new_object(),*compact=json_object_new_object();
    addint(o,"id",sqlite3_column_int64(s,0)); addstr(o,"subject_kind",identity ? "identity" : "person");
    addint(o,"subject_id",subject_id); json_object_object_add(o,"subject",subject);
    json_object_object_add(o,"subject_snapshot",snapshot);
    addint(compact,"id",jint(asset,"id"));
    const char *keys[]={"asset_tag","name","asset_type","status"};
    for (size_t i=0;i<4;i++) addstr(compact,keys[i],jstr(asset,keys[i]));
    json_object_put(asset); json_object_object_add(o,"asset",compact);
    addstr(o,"role",(const char *)sqlite3_column_text(s,4)); addstr(o,"notes",(const char *)sqlite3_column_text(s,5));
    addint(o,"actor_id",sqlite3_column_int64(s,7)); addstr(o,"created_at",(const char *)sqlite3_column_text(s,8));
    json_object_object_add(o,"active",json_object_new_boolean(sqlite3_column_int(s,9)));
    if (sqlite3_column_type(s,10)==SQLITE_NULL) json_object_object_add(o,"removed_at",NULL);
    else addstr(o,"removed_at",(const char *)sqlite3_column_text(s,10));
    column_id(o,"removed_actor_id",s,11); return o;
}
static json_object *subject_load(sqlite3_int64 id, sqlite3_int64 asset_id, bool *error) {
    sqlite3_stmt *s=prepare("SELECT " SUBJECT_COLUMNS " FROM inventory_asset_subjects WHERE id=? AND asset_id=?");
    if (!s) { *error=true; return NULL; }
    sqlite3_bind_int64(s,1,id); sqlite3_bind_int64(s,2,asset_id);
    int result=sqlite3_step(s);
    json_object *link=result==SQLITE_ROW ? subject_row(s,error) : NULL;
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) *error=true;
    sqlite3_finalize(s); return link;
}
static json_object *subject_list(sqlite3_int64 id, int scope, const char *query, int *status) {
    Form form; int limit; sqlite3_int64 after;
    if (!page_query(query,true,&form,&limit,&after)) return failure(status,400,"Paginazione collegamenti non valida.");
    form_free(&form);
    bool error=false;
    json_object *parent=scope ? directory_load(scope==2,id,&error) : load_asset(id,&error);
    if (!parent) return failure(status,error ? 500 : 404,"Scheda non trovata.");
    json_object_put(parent);
    sqlite3_stmt *s=prepare(scope==1 ? "SELECT " SUBJECT_COLUMNS " FROM inventory_asset_subjects WHERE person_id=?1 AND active=1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3" :
        scope==2 ? "SELECT " SUBJECT_COLUMNS " FROM inventory_asset_subjects WHERE identity_id=?1 AND active=1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3" :
        "SELECT " SUBJECT_COLUMNS " FROM inventory_asset_subjects WHERE asset_id=?1 AND active=1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3");
    if (!s) return failure(status,500,"Impossibile leggere i collegamenti.");
    sqlite3_bind_int64(s,1,id); sqlite3_bind_int64(s,2,after); sqlite3_bind_int(s,3,limit+1);
    json_object *o=ok_json(),*items=json_object_new_array();
    json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int result=SQLITE_DONE,count=0; sqlite3_int64 last=0; bool more=false;
    size_t bytes=strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        json_object *item=subject_row(s,&error);
        if (!item || error) break;
        size_t length=strlen(json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN));
        if (bytes+length+64>RESPONSE_LIMIT) { json_object_put(item); more=true; break; }
        bytes+=length+1; last=sqlite3_column_int64(s,0); json_object_array_add(items,item); count++;
    }
    sqlite3_finalize(s);
    if (error || (result!=SQLITE_ROW && result!=SQLITE_DONE) || (more && !last)) {
        json_object_put(o); return failure(status,500,"Lettura collegamenti interrotta.");
    }
    if (more) addint(o,"next_after_id",last);
    return o;
}
static json_object *subject_save(sqlite3_int64 asset_id, sqlite3_int64 remove_id, const Form *form, int *status) {
    static const char *const allowed[]={"version","actor_id","subject_kind","subject_id","role","notes"};
    sqlite3_int64 version=0,actor=0,subject_id=0;
    if (!only_fields(form,allowed,remove_id ? 2 : 6) ||
        !integer(field(form,"version"),1,INT64_MAX-1,&version) || !integer(field(form,"actor_id"),1,INT64_MAX,&actor))
        return failure(status,400,"Versione, autore o campi collegamento non validi.");
    bool identity=!strcmp(field(form,"subject_kind"),"identity");
    const char *role=has_field(form,"role") ? field(form,"role") : "user",*notes=field(form,"notes");
    if (!remove_id && ((!identity && strcmp(field(form,"subject_kind"),"person")) ||
        !integer(field(form,"subject_id"),1,INT64_MAX,&subject_id) ||
        (strcmp(role,"owner") && strcmp(role,"user") && strcmp(role,"administrator")) || strlen(notes)>2000 || !utf8_text(notes,true)))
        return failure(status,400,"Soggetto, ruolo o note non validi.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,500,"Archivio inventario occupato.");
    int code=500; const char *message="Collegamento non salvato; operazione annullata.";
    bool error=false; json_object *old=load_asset(asset_id,&error),*asset=NULL,*subject=NULL,*link=NULL,*change=NULL;
    if (!old) { code=error ? 500 : 404; message="Bene non trovato."; goto fail; }
    if (jint(old,"version")!=version) { code=409; message="Il bene è cambiato. Ricaricare la scheda."; goto fail; }
    if (!remove_id) {
        subject=directory_load(identity,subject_id,&error);
        if (!subject) { code=error ? 500 : 404; message="Persona o identità non trovata."; goto fail; }
        if (!directory_active(subject) || !strcmp(jstr(old,"status"),"retired") || !strcmp(jstr(old,"status"),"lost")) {
            code=409; message="Nuovi collegamenti richiedono soggetti attivi e un bene non dismesso o smarrito."; goto fail;
        }
    } else {
        link=subject_load(remove_id,asset_id,&error);
        if (!link || !jint(link,"active")) { code=error ? 500 : 404; message="Collegamento attivo non trovato."; goto fail; }
        json_object_put(link); link=NULL;
    }
    char timestamp[32]; if (!timestamp_now(timestamp)) goto fail;
    sqlite3_stmt *s=prepare(remove_id ? "UPDATE inventory_asset_subjects SET active=0,removed_at=?,removed_actor_id=? WHERE id=? AND asset_id=? AND active=1" :
        "INSERT INTO inventory_asset_subjects(asset_id,person_id,identity_id,role,notes,subject_snapshot,actor_id,created_at) VALUES(?,?,?,?,?,?,?,?)");
    if (!s) goto fail;
    if (remove_id) {
        sqlite3_bind_text(s,1,timestamp,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,actor);
        sqlite3_bind_int64(s,3,remove_id); sqlite3_bind_int64(s,4,asset_id);
    } else {
        sqlite3_bind_int64(s,1,asset_id); bind_id(s,2,identity ? 0 : subject_id); bind_id(s,3,identity ? subject_id : 0);
        sqlite3_bind_text(s,4,role,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,5,notes,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,6,json_object_to_json_string_ext(subject,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(s,7,actor); sqlite3_bind_text(s,8,timestamp,-1,SQLITE_TRANSIENT);
    }
    int result=sqlite3_step(s); sqlite3_finalize(s);
    if (result!=SQLITE_DONE || sqlite3_changes(database)!=1) {
        if ((result&0xff)==SQLITE_CONSTRAINT) { code=409; message="Il soggetto è già collegato al bene."; } goto fail;
    }
    sqlite3_int64 link_id=remove_id ? remove_id : sqlite3_last_insert_rowid(database);
    s=prepare("UPDATE assets SET actor_id=?,version=version+1,updated_at=? WHERE id=? AND version=?");
    if (!s) goto fail;
    sqlite3_bind_int64(s,1,actor); sqlite3_bind_text(s,2,timestamp,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,3,asset_id); sqlite3_bind_int64(s,4,version);
    bool ok=sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1; sqlite3_finalize(s); if (!ok) goto fail;
    asset=load_asset(asset_id,&error); link=subject_load(link_id,asset_id,&error);
    if (!asset || !link || error) goto fail;
    change=json_object_new_object(); addstr(change,"operation",remove_id ? "subject_removed" : "subject_added");
    json_object_object_add(change,"link",json_object_get(link));
    if (!history_write(asset_id,"updated",actor,version+1,timestamp,asset,change) || !execute("COMMIT")) goto fail;
    json_object_put(old); if (subject) json_object_put(subject); json_object_put(change);
    json_object *o=ok_json(); json_object_object_add(o,"asset",asset); json_object_object_add(o,"link",link);
    *status=remove_id ? 200 : 201; return o;
fail:
    (void)execute("ROLLBACK");
    if (old) json_object_put(old);
    if (asset) json_object_put(asset);
    if (subject) json_object_put(subject);
    if (link) json_object_put(link);
    if (change) json_object_put(change);
    return failure(status,code,message);
}
static json_object *subjects_route(Request *request, const char *query, int *status) {
    const char *start=NULL; int scope=0;
    if (!strncmp(request->path,"/v1/assets/",11)) start=request->path+11;
    else if (!strncmp(request->path,"/v1/people/",11)) { start=request->path+11; scope=1; }
    else if (!strncmp(request->path,"/v1/identities/",15)) { start=request->path+15; scope=2; }
    else return NULL;
    const char *suffix=strchr(start,'/');
    if (!suffix || (scope ? strcmp(suffix,"/assets") : strncmp(suffix,"/subjects",9))) return NULL;
    char identifier[32]; size_t length=(size_t)(suffix-start); sqlite3_int64 id=0,remove_id=0;
    if (!length || length>=sizeof(identifier)) return failure(status,400,"Identificativo non valido.");
    memcpy(identifier,start,length); identifier[length]=0;
    if (!integer(identifier,1,INT64_MAX,&id)) return failure(status,400,"Identificativo non valido.");
    if (!scope && strcmp(suffix,"/subjects")) {
        if (strncmp(suffix,"/subjects/",10)) return failure(status,404,"Percorso collegamento non trovato.");
        const char *tail=strchr(suffix+10,'/'); length=tail ? (size_t)(tail-(suffix+10)) : 0;
        if (!length || length>=sizeof(identifier) || strcmp(tail,"/remove")) return failure(status,404,"Percorso collegamento non trovato.");
        memcpy(identifier,suffix+10,length); identifier[length]=0;
        if (!integer(identifier,1,INT64_MAX,&remove_id)) return failure(status,400,"Identificativo collegamento non valido.");
    }
    bool get=!strcmp(request->method,"GET");
    if (get) {
        if (remove_id) return failure(status,405,"La rimozione richiede POST.");
        return subject_list(id,scope,query,status);
    }
    if (scope) return failure(status,405,"L'elenco beni è consultabile in sola lettura.");
    if (*query) return failure(status,400,"Parametri inattesi per il collegamento.");
    if (strcasecmp(request->content_type,"application/x-www-form-urlencoded")) return failure(status,415,"Formato richiesto: application/x-www-form-urlencoded.");
    Form form;
    if (!form_parse(request->body,&form)) return failure(status,400,"Modulo non valido o campi duplicati.");
    json_object *o=subject_save(id,remove_id,&form,status); form_free(&form); return o;
}
