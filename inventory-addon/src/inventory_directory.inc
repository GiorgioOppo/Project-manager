/* Separate people and digital identities. Associations are many-to-many and audited. */
static bool directory_migrate(void) {
    return execute(
        "CREATE TABLE inventory_people(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "person_code TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(CAST(person_code AS BLOB)) BETWEEN 1 AND 64),"
        "display_name TEXT NOT NULL CHECK(length(CAST(display_name AS BLOB)) BETWEEN 1 AND 160),"
        "email TEXT NOT NULL DEFAULT '' CHECK(length(CAST(email AS BLOB))<=254),"
        "organization TEXT NOT NULL DEFAULT '' CHECK(length(CAST(organization AS BLOB))<=160),"
        "job_title TEXT NOT NULL DEFAULT '' CHECK(length(CAST(job_title AS BLOB))<=120),"
        "notes TEXT NOT NULL DEFAULT '' CHECK(length(CAST(notes AS BLOB))<=2000),"
        "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),version INTEGER NOT NULL DEFAULT 1 CHECK(version>0),"
        "actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL);"
        "CREATE TABLE inventory_identities(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "realm TEXT NOT NULL COLLATE NOCASE CHECK(length(CAST(realm AS BLOB)) BETWEEN 1 AND 160),"
        "login TEXT NOT NULL COLLATE NOCASE CHECK(length(CAST(login AS BLOB)) BETWEEN 1 AND 160),"
        "display_name TEXT NOT NULL DEFAULT '' CHECK(length(CAST(display_name AS BLOB))<=160),"
        "kind TEXT NOT NULL DEFAULT 'personal' CHECK(kind IN('personal','shared','service')),"
        "notes TEXT NOT NULL DEFAULT '' CHECK(length(CAST(notes AS BLOB))<=2000),"
        "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),version INTEGER NOT NULL DEFAULT 1 CHECK(version>0),"
        "actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,UNIQUE(realm,login));"
        "CREATE TABLE inventory_identity_people(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "identity_id INTEGER NOT NULL REFERENCES inventory_identities(id),person_id INTEGER NOT NULL REFERENCES inventory_people(id),"
        "role TEXT NOT NULL DEFAULT 'user' CHECK(role IN('owner','user')),notes TEXT NOT NULL DEFAULT '' CHECK(length(CAST(notes AS BLOB))<=2000),"
        "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,"
        "removed_actor_id INTEGER,removed_at TEXT,CHECK((active=1 AND removed_actor_id IS NULL AND removed_at IS NULL) OR "
        "(active=0 AND removed_actor_id>0 AND removed_at IS NOT NULL)));"
        "CREATE UNIQUE INDEX inventory_identity_people_active_pair ON inventory_identity_people(identity_id,person_id) WHERE active=1;"
        "CREATE INDEX inventory_identity_people_person ON inventory_identity_people(person_id,active,id);"
        "CREATE INDEX inventory_identity_people_identity ON inventory_identity_people(identity_id,active,id);"
        "CREATE TABLE inventory_entity_history(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "entity_type TEXT NOT NULL CHECK(entity_type IN('person','identity')),entity_id INTEGER NOT NULL CHECK(entity_id>0),"
        "action TEXT NOT NULL CHECK(action IN('created','updated')),actor_id INTEGER NOT NULL CHECK(actor_id>0),"
        "version INTEGER NOT NULL CHECK(version>0),created_at TEXT NOT NULL,"
        "snapshot TEXT NOT NULL CHECK(json_valid(snapshot)),change TEXT CHECK(change IS NULL OR json_valid(change)),UNIQUE(entity_type,entity_id,version));"
        "CREATE INDEX inventory_entity_history_entity ON inventory_entity_history(entity_type,entity_id,id);");
}
#define DIRECTORY_PERSON_COLUMNS "p.id,p.person_code,p.display_name,p.email,p.organization,p.job_title,p.notes,p.active,p.version,p.actor_id,p.created_at,p.updated_at,(SELECT COUNT(*) FROM inventory_identity_people d WHERE d.person_id=p.id AND d.active=1)"
#define DIRECTORY_IDENTITY_COLUMNS "p.id,p.realm,p.login,p.display_name,p.kind,p.notes,p.active,p.version,p.actor_id,p.created_at,p.updated_at,(SELECT COUNT(*) FROM inventory_identity_people d WHERE d.identity_id=p.id AND d.active=1)"
static bool directory_active(json_object *entity) { return entity && jint(entity,"active")==1; }
static bool directory_kind(const char *kind) {
    return !strcmp(kind,"personal") || !strcmp(kind,"shared") || !strcmp(kind,"service");
}
static json_object *directory_row(bool identity, sqlite3_stmt *s) {
    static const char *const people[]={"person_code","display_name","email","organization","job_title","notes"};
    static const char *const identities[]={"realm","login","display_name","kind","notes"};
    const char *const *keys=identity ? identities : people;
    int count=identity ? 5 : 6;
    json_object *entity=json_object_new_object();
    addint(entity,"id",sqlite3_column_int64(s,0));
    for (int i=0;i<count;i++) addstr(entity,keys[i],(const char *)sqlite3_column_text(s,i+1));
    json_object_object_add(entity,"active",json_object_new_boolean(sqlite3_column_int(s,count+1)));
    addint(entity,"version",sqlite3_column_int64(s,count+2)); addint(entity,"actor_id",sqlite3_column_int64(s,count+3));
    addstr(entity,"created_at",(const char *)sqlite3_column_text(s,count+4));
    addstr(entity,"updated_at",(const char *)sqlite3_column_text(s,count+5));
    addint(entity,identity ? "people_count" : "identity_count",sqlite3_column_int64(s,count+6));
    return entity;
}
static json_object *directory_load(bool identity, sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s=prepare(identity ? "SELECT " DIRECTORY_IDENTITY_COLUMNS " FROM inventory_identities p WHERE p.id=?" :
        "SELECT " DIRECTORY_PERSON_COLUMNS " FROM inventory_people p WHERE p.id=?");
    if (!s) { *error=true; return NULL; }
    sqlite3_bind_int64(s,1,id); int result=sqlite3_step(s);
    json_object *entity=result==SQLITE_ROW ? directory_row(identity,s) : NULL;
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) *error=true;
    sqlite3_finalize(s); return entity;
}
/* Also used by asset associations so every directory change has a snapshot in the same transaction. */
static bool directory_history_write(bool identity, sqlite3_int64 id, const char *action, sqlite3_int64 actor,
                                    const char *timestamp, json_object *entity, json_object *change) {
    sqlite3_stmt *s=prepare("INSERT INTO inventory_entity_history(entity_type,entity_id,action,actor_id,version,created_at,snapshot,change) VALUES(?,?,?,?,?,?,?,?)");
    bool ok=s && sqlite3_bind_text(s,1,identity ? "identity" : "person",-1,SQLITE_STATIC)==SQLITE_OK &&
        sqlite3_bind_int64(s,2,id)==SQLITE_OK && sqlite3_bind_text(s,3,action,-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_int64(s,4,actor)==SQLITE_OK && sqlite3_bind_int64(s,5,jint(entity,"version"))==SQLITE_OK &&
        sqlite3_bind_text(s,6,timestamp,-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_text(s,7,json_object_to_json_string_ext(entity,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT)==SQLITE_OK;
    if (ok) ok=change ? sqlite3_bind_text(s,8,json_object_to_json_string_ext(change,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT)==SQLITE_OK :
        sqlite3_bind_null(s,8)==SQLITE_OK;
    if (ok) ok=sqlite3_step(s)==SQLITE_DONE;
    sqlite3_finalize(s); return ok;
}
static bool directory_bump(bool identity, sqlite3_int64 id, sqlite3_int64 version, sqlite3_int64 actor, const char *timestamp) {
    sqlite3_stmt *s=prepare(identity ? "UPDATE inventory_identities SET version=version+1,actor_id=?,updated_at=? WHERE id=? AND version=?" :
        "UPDATE inventory_people SET version=version+1,actor_id=?,updated_at=? WHERE id=? AND version=?");
    bool ok=s && version<INT64_MAX && sqlite3_bind_int64(s,1,actor)==SQLITE_OK &&
        sqlite3_bind_text(s,2,timestamp,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int64(s,3,id)==SQLITE_OK &&
        sqlite3_bind_int64(s,4,version)==SQLITE_OK && sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1;
    sqlite3_finalize(s); return ok;
}
static json_object *directory_save(bool identity, sqlite3_int64 id, Form *form, int *status) {
    static const char *const people_fields[]={"person_code","display_name","email","organization","job_title","notes","active","actor_id","version"};
    static const char *const identity_fields[]={"realm","login","display_name","kind","notes","active","actor_id","version"};
    static const size_t people_max[]={64,160,254,160,120,2000};
    static const size_t identity_max[]={160,160,160,16,2000};
    const char *const *keys=identity ? identity_fields : people_fields;
    const size_t *maxima=identity ? identity_max : people_max;
    int count=identity ? 5 : 6,active=1;
    sqlite3_int64 actor=0,version=0;
    if (!only_fields(form,keys,count+(id ? 3 : 2)) || !integer(field(form,"actor_id"),1,INT64_MAX,&actor) ||
        (id && !integer(field(form,"version"),1,INT64_MAX-1,&version)) ||
        (has_field(form,"active") && !number(field(form,"active"),0,1,&active)))
        return failure(status,400,"Anagrafica, autore o versione non validi.");
    const char *values[6];
    for (int i=0;i<count;i++) {
        values[i]=field(form,keys[i]);
        if (strlen(values[i])>maxima[i] || !utf8_text(values[i],i==count-1))
            return failure(status,400,"Campo anagrafica troppo lungo o con caratteri non validi.");
    }
    if (!nonblank(values[0]) || !nonblank(values[1])) return failure(status,400,identity ?
        "Ambito e login dell'identità sono obbligatori." : "Codice e nome della persona sono obbligatori.");
    if (identity && !*values[3]) values[3]="personal";
    if (identity && !directory_kind(values[3])) return failure(status,400,"Tipo identità non valido.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    bool error=false; int code=500; const char *message="Salvataggio anagrafica non riuscito.";
    json_object *old=id ? directory_load(identity,id,&error) : NULL,*entity=NULL;
    if (id && !old) { code=error ? 500 : 404; message="Anagrafica non trovata."; goto fail; }
    if (old && jint(old,"version")!=version) { code=409; message="L'anagrafica è stata modificata; ricaricare i dati."; goto fail; }
    if (old && !has_field(form,"active")) active=directory_active(old);
    char timestamp[32]; if (!timestamp_now(timestamp)) goto fail;
    const char *sql=identity ? (id ? "UPDATE inventory_identities SET realm=?1,login=?2,display_name=?3,kind=?4,notes=?5,active=?7,actor_id=?8,updated_at=?9,version=version+1 WHERE id=?10 AND version=?11" :
        "INSERT INTO inventory_identities(realm,login,display_name,kind,notes,active,actor_id,created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?7,?8,?9,?9)") :
        (id ? "UPDATE inventory_people SET person_code=?1,display_name=?2,email=?3,organization=?4,job_title=?5,notes=?6,active=?7,actor_id=?8,updated_at=?9,version=version+1 WHERE id=?10 AND version=?11" :
        "INSERT INTO inventory_people(person_code,display_name,email,organization,job_title,notes,active,actor_id,created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?9)");
    sqlite3_stmt *s=prepare(sql); bool ok=s!=NULL;
    for (int i=0;i<count && ok;i++) ok=sqlite3_bind_text(s,i+1,values[i],-1,SQLITE_TRANSIENT)==SQLITE_OK;
    if (ok) ok=sqlite3_bind_int(s,7,active)==SQLITE_OK && sqlite3_bind_int64(s,8,actor)==SQLITE_OK && sqlite3_bind_text(s,9,timestamp,-1,SQLITE_TRANSIENT)==SQLITE_OK;
    if (id && ok) ok=sqlite3_bind_int64(s,10,id)==SQLITE_OK && sqlite3_bind_int64(s,11,version)==SQLITE_OK;
    int result=ok ? sqlite3_step(s) : SQLITE_ERROR,extended=sqlite3_extended_errcode(database); sqlite3_finalize(s);
    if (result!=SQLITE_DONE || sqlite3_changes(database)!=1) {
        if (extended==SQLITE_CONSTRAINT_UNIQUE) { code=409; message=identity ? "Identità già presente per questo ambito e login." : "Codice persona già presente."; }
        goto fail;
    }
    bool created=!id; if (created) id=sqlite3_last_insert_rowid(database);
    entity=directory_load(identity,id,&error);
    json_object *change=json_object_new_object(); addstr(change,"operation",created ? "created" : "updated");
    ok=entity && !error && directory_history_write(identity,id,created ? "created" : "updated",actor,timestamp,entity,change);
    json_object_put(change);
    if (!ok || !execute("COMMIT")) { message="Storico anagrafica non salvato; modifica annullata."; goto fail; }
    if (old) json_object_put(old);
    json_object *o=ok_json(); json_object_object_add(o,identity ? "identity" : "person",entity); *status=created ? 201 : 200; return o;
fail:
    (void)execute("ROLLBACK"); if (old) json_object_put(old); if (entity) json_object_put(entity);
    return failure(status,code,message);
}
static bool directory_pagination(const char *query, Form *form, int *limit, sqlite3_int64 *after) {
    *limit=30; *after=0;
    if (!form_parse(query,form)) return false;
    bool ok=(!has_field(form,"limit") || number(field(form,"limit"),1,100,limit)) &&
        (!has_field(form,"after_id") || integer(field(form,"after_id"),1,INT64_MAX,after));
    if (!ok) form_free(form);
    return ok;
}
/* Reserve enough room for the cursor, commas and the response envelope. */
static bool directory_page_add(json_object *items, json_object *item, size_t *bytes) {
    size_t length=strlen(json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN));
    if (*bytes+length+1>RESPONSE_LIMIT-128) { json_object_put(item); return false; }
    *bytes+=length+1; json_object_array_add(items,item); return true;
}
static json_object *directory_list(bool identity, const char *query, int *status) {
    static const char *const fields[]={"limit","after_id","q","active","kind","person_id"};
    Form form; int limit; sqlite3_int64 after,person=0;
    if (!directory_pagination(query,&form,&limit,&after)) return failure(status,400,"Filtri anagrafiche non validi.");
    bool ok=only_fields(&form,fields,identity ? 6 : 4) && strlen(field(&form,"q"))<=200 && utf8_text(field(&form,"q"),false) &&
        (!has_field(&form,"active") || !strcmp(field(&form,"active"),"0") || !strcmp(field(&form,"active"),"1")) &&
        (!has_field(&form,"kind") || directory_kind(field(&form,"kind"))) &&
        (!has_field(&form,"person_id") || integer(field(&form,"person_id"),1,INT64_MAX,&person));
    if (!ok) { form_free(&form); return failure(status,400,"Filtri anagrafiche non validi."); }
    const char *sql=identity ? "SELECT " DIRECTORY_IDENTITY_COLUMNS " FROM inventory_identities p WHERE (?1=0 OR p.id<?1) AND (?2='' OR instr(lower(p.realm||' '||p.login||' '||p.display_name||' '||p.notes),lower(?2))>0) AND (?3='' OR p.active=CAST(?3 AS INTEGER)) AND (?4='' OR p.kind=?4) AND (?5=0 OR EXISTS(SELECT 1 FROM inventory_identity_people d WHERE d.identity_id=p.id AND d.person_id=?5 AND d.active=1)) ORDER BY p.id DESC LIMIT ?6" :
        "SELECT " DIRECTORY_PERSON_COLUMNS " FROM inventory_people p WHERE (?1=0 OR p.id<?1) AND (?2='' OR instr(lower(p.person_code||' '||p.display_name||' '||p.email||' '||p.organization||' '||p.job_title||' '||p.notes),lower(?2))>0) AND (?3='' OR p.active=CAST(?3 AS INTEGER)) ORDER BY p.id DESC LIMIT ?6";
    sqlite3_stmt *s=prepare(sql);
    ok=s && sqlite3_bind_int64(s,1,after)==SQLITE_OK && sqlite3_bind_text(s,2,field(&form,"q"),-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_text(s,3,field(&form,"active"),-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int(s,6,limit+1)==SQLITE_OK;
    if (identity && ok) ok=sqlite3_bind_text(s,4,field(&form,"kind"),-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int64(s,5,person)==SQLITE_OK;
    form_free(&form);
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere le anagrafiche."); }
    json_object *o=ok_json(),*items=json_object_new_array(); json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int result,count=0; sqlite3_int64 last=0; size_t bytes=0; bool more=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit || !directory_page_add(items,directory_row(identity,s),&bytes)) { more=true; break; }
        last=sqlite3_column_int64(s,0); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) { json_object_put(o); return failure(status,500,"Lettura anagrafiche interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
#define DIRECTORY_LINK_COLUMNS "id,identity_id,person_id,role,notes,active,actor_id,created_at,removed_actor_id,removed_at"
static json_object *directory_link_row(sqlite3_stmt *s, bool *error) {
    json_object *identity=directory_load(true,sqlite3_column_int64(s,1),error),*person=directory_load(false,sqlite3_column_int64(s,2),error);
    if (!identity || !person || *error) { if (identity) json_object_put(identity); if (person) json_object_put(person); *error=true; return NULL; }
    json_object *link=json_object_new_object(); addint(link,"id",sqlite3_column_int64(s,0));
    addint(link,"identity_id",sqlite3_column_int64(s,1)); addint(link,"person_id",sqlite3_column_int64(s,2));
    json_object_object_add(link,"identity",identity); json_object_object_add(link,"person",person);
    addstr(link,"role",(const char *)sqlite3_column_text(s,3)); addstr(link,"notes",(const char *)sqlite3_column_text(s,4));
    json_object_object_add(link,"active",json_object_new_boolean(sqlite3_column_int(s,5)));
    addint(link,"actor_id",sqlite3_column_int64(s,6)); addstr(link,"created_at",(const char *)sqlite3_column_text(s,7));
    column_id(link,"removed_actor_id",s,8);
    if (sqlite3_column_type(s,9)==SQLITE_NULL) json_object_object_add(link,"removed_at",NULL);
    else addstr(link,"removed_at",(const char *)sqlite3_column_text(s,9));
    return link;
}
static json_object *directory_link_load(sqlite3_int64 identity_id, sqlite3_int64 link_id, bool *error) {
    sqlite3_stmt *s=prepare("SELECT " DIRECTORY_LINK_COLUMNS " FROM inventory_identity_people WHERE identity_id=? AND id=?");
    if (!s) { *error=true; return NULL; }
    sqlite3_bind_int64(s,1,identity_id); sqlite3_bind_int64(s,2,link_id); int result=sqlite3_step(s);
    json_object *link=result==SQLITE_ROW ? directory_link_row(s,error) : NULL;
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) *error=true;
    sqlite3_finalize(s); return link;
}
static json_object *directory_links(bool identity, sqlite3_int64 id, const char *query, int *status) {
    static const char *const fields[]={"limit","after_id"};
    Form form; int limit; sqlite3_int64 after;
    if (!directory_pagination(query,&form,&limit,&after)) return failure(status,400,"Filtri collegamenti non validi.");
    bool ok=only_fields(&form,fields,2); form_free(&form);
    if (!ok) return failure(status,400,"Filtri collegamenti non validi.");
    bool error=false; json_object *entity=directory_load(identity,id,&error);
    if (!entity) return failure(status,error ? 500 : 404,"Anagrafica non trovata.");
    json_object_put(entity);
    sqlite3_stmt *s=prepare(identity ? "SELECT " DIRECTORY_LINK_COLUMNS " FROM inventory_identity_people WHERE identity_id=?1 AND active=1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3" :
        "SELECT " DIRECTORY_LINK_COLUMNS " FROM inventory_identity_people WHERE person_id=?1 AND active=1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3");
    ok=s && sqlite3_bind_int64(s,1,id)==SQLITE_OK && sqlite3_bind_int64(s,2,after)==SQLITE_OK && sqlite3_bind_int(s,3,limit+1)==SQLITE_OK;
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere i collegamenti."); }
    json_object *o=ok_json(),*items=json_object_new_array(); json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int count=0,result; sqlite3_int64 last=0; size_t bytes=0; bool more=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        json_object *link=directory_link_row(s,&error);
        if (!link || error) { result=SQLITE_ERROR; break; }
        if (!directory_page_add(items,link,&bytes)) { more=true; break; }
        last=sqlite3_column_int64(s,0); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) { json_object_put(o); return failure(status,500,"Lettura collegamenti interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
static json_object *directory_link_save(sqlite3_int64 identity_id, sqlite3_int64 remove_id, Form *form, int *status) {
    static const char *const fields[]={"version","actor_id","person_id","role","notes"};
    sqlite3_int64 actor,version,person_id=0;
    const char *role=field(form,"role"),*notes=field(form,"notes");
    if (!*role) role="user";
    if (!only_fields(form,fields,remove_id ? 2 : 5) || !integer(field(form,"version"),1,INT64_MAX-1,&version) ||
        !integer(field(form,"actor_id"),1,INT64_MAX,&actor) ||
        (!remove_id && (!integer(field(form,"person_id"),1,INT64_MAX,&person_id) || (strcmp(role,"owner") && strcmp(role,"user")) ||
        strlen(notes)>2000 || !utf8_text(notes,true)))) return failure(status,400,"Collegamento, autore o versione non validi.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    bool error=false; int code=500; const char *message="Salvataggio collegamento non riuscito.";
    json_object *identity=directory_load(true,identity_id,&error),*person=NULL,*link=NULL;
    if (!identity) { code=error ? 500 : 404; message="Identità non trovata."; goto fail; }
    if (jint(identity,"version")!=version) { code=409; message="L'identità è stata modificata; ricaricare i dati."; goto fail; }
    if (remove_id) {
        link=directory_link_load(identity_id,remove_id,&error);
        if (!link) { code=error ? 500 : 404; message="Collegamento non trovato."; goto fail; }
        if (!jint(link,"active")) { code=409; message="Collegamento già rimosso."; goto fail; }
        person_id=jint(link,"person_id");
    }
    person=directory_load(false,person_id,&error);
    if (!person) { code=error ? 500 : 404; message="Persona non trovata."; goto fail; }
    if (!remove_id && (!directory_active(identity) || !directory_active(person))) { code=409; message="Collegare solo persone e identità attive."; goto fail; }
    char timestamp[32]; if (!timestamp_now(timestamp)) goto fail;
    sqlite3_stmt *s=prepare(remove_id ? "UPDATE inventory_identity_people SET active=0,removed_actor_id=?1,removed_at=?2 WHERE id=?3 AND identity_id=?4 AND active=1" :
        "INSERT INTO inventory_identity_people(actor_id,created_at,identity_id,person_id,role,notes) VALUES(?1,?2,?4,?5,?6,?7)");
    bool ok=s && sqlite3_bind_int64(s,1,actor)==SQLITE_OK && sqlite3_bind_text(s,2,timestamp,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int64(s,4,identity_id)==SQLITE_OK;
    if (ok && remove_id) ok=sqlite3_bind_int64(s,3,remove_id)==SQLITE_OK;
    if (ok && !remove_id) ok=sqlite3_bind_int64(s,5,person_id)==SQLITE_OK && sqlite3_bind_text(s,6,role,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_text(s,7,notes,-1,SQLITE_TRANSIENT)==SQLITE_OK;
    int result=ok ? sqlite3_step(s) : SQLITE_ERROR,extended=sqlite3_extended_errcode(database); sqlite3_finalize(s);
    if (result!=SQLITE_DONE || sqlite3_changes(database)!=1) {
        if (extended==SQLITE_CONSTRAINT_UNIQUE) { code=409; message="Persona già collegata all'identità."; }
        goto fail;
    }
    sqlite3_int64 link_id=remove_id ? remove_id : sqlite3_last_insert_rowid(database);
    if (!directory_bump(true,identity_id,version,actor,timestamp) || !directory_bump(false,person_id,jint(person,"version"),actor,timestamp)) goto fail;
    json_object_put(identity); json_object_put(person); if (link) json_object_put(link);
    identity=directory_load(true,identity_id,&error); person=directory_load(false,person_id,&error); link=directory_link_load(identity_id,link_id,&error);
    if (!identity || !person || !link || error) goto fail;
    json_object *change=json_object_new_object(); addstr(change,"operation",remove_id ? "identity_person_removed" : "identity_person_added");
    json_object_object_add(change,"link",json_object_get(link));
    ok=directory_history_write(true,identity_id,"updated",actor,timestamp,identity,change) &&
        directory_history_write(false,person_id,"updated",actor,timestamp,person,change);
    json_object_put(change);
    if (!ok || !execute("COMMIT")) { message="Storico collegamento non salvato; modifica annullata."; goto fail; }
    json_object *o=ok_json(); json_object_object_add(o,"identity",identity); json_object_object_add(o,"person",person); json_object_object_add(o,"link",link);
    *status=remove_id ? 200 : 201; return o;
fail:
    (void)execute("ROLLBACK"); if (identity) json_object_put(identity); if (person) json_object_put(person); if (link) json_object_put(link);
    return failure(status,code,message);
}
static json_object *directory_history(bool identity, sqlite3_int64 id, const char *query, int *status) {
    static const char *const fields[]={"limit","after_id"};
    Form form; int limit; sqlite3_int64 after;
    if (!directory_pagination(query,&form,&limit,&after)) return failure(status,400,"Filtri storico non validi.");
    bool ok=only_fields(&form,fields,2); form_free(&form);
    if (!ok) return failure(status,400,"Filtri storico non validi.");
    bool error=false; json_object *entity=directory_load(identity,id,&error);
    if (!entity) return failure(status,error ? 500 : 404,"Anagrafica non trovata.");
    json_object_put(entity);
    sqlite3_stmt *s=prepare("SELECT id,action,actor_id,version,created_at,snapshot,change FROM inventory_entity_history WHERE entity_type=?1 AND entity_id=?2 AND (?3=0 OR id<?3) ORDER BY id DESC LIMIT ?4");
    ok=s && sqlite3_bind_text(s,1,identity ? "identity" : "person",-1,SQLITE_STATIC)==SQLITE_OK && sqlite3_bind_int64(s,2,id)==SQLITE_OK &&
        sqlite3_bind_int64(s,3,after)==SQLITE_OK && sqlite3_bind_int(s,4,limit+1)==SQLITE_OK;
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere lo storico."); }
    json_object *o=ok_json(),*items=json_object_new_array(); json_object_object_add(o,"items",items); json_object_object_add(o,"next_after_id",NULL);
    int result,count=0; sqlite3_int64 last=0; size_t bytes=0; bool more=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        json_object *snapshot=json_tokener_parse((const char *)sqlite3_column_text(s,5));
        json_object *change=sqlite3_column_type(s,6)==SQLITE_NULL ? NULL : json_tokener_parse((const char *)sqlite3_column_text(s,6));
        if (!snapshot || !json_object_is_type(snapshot,json_type_object) ||
            (sqlite3_column_type(s,6)!=SQLITE_NULL && (!change || !json_object_is_type(change,json_type_object)))) {
            if (snapshot) json_object_put(snapshot);
            if (change) json_object_put(change);
            result=SQLITE_CORRUPT; break;
        }
        json_object *item=json_object_new_object(); addint(item,"id",sqlite3_column_int64(s,0));
        addstr(item,"action",(const char *)sqlite3_column_text(s,1)); addint(item,"actor_id",sqlite3_column_int64(s,2));
        addint(item,"version",sqlite3_column_int64(s,3)); addstr(item,"created_at",(const char *)sqlite3_column_text(s,4));
        json_object_object_add(item,"snapshot",snapshot); if (change) json_object_object_add(item,"change",change);
        if (!directory_page_add(items,item,&bytes)) { more=true; break; }
        last=sqlite3_column_int64(s,0); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) { json_object_put(o); return failure(status,500,"Lettura storico interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
static json_object *directory_route(Request *request, const char *query, int *status) {
    const char *path=request->path,*rest=NULL; bool identity=false;
    if (!strcmp(path,"/v1/people") || !strncmp(path,"/v1/people/",11)) rest=path+10;
    else if (!strcmp(path,"/v1/identities") || !strncmp(path,"/v1/identities/",15)) { rest=path+14; identity=true; }
    else return NULL;
    bool get=!strcmp(request->method,"GET"),post=!strcmp(request->method,"POST");
    sqlite3_int64 id=0,remove=0; const char *suffix="";
    if (*rest) {
        const char *start=rest+1,*slash=strchr(start,'/'); size_t length=slash ? (size_t)(slash-start) : strlen(start);
        char identifier[32];
        if (length>=sizeof(identifier) || !length) return failure(status,400,"Identificativo anagrafica non valido.");
        memcpy(identifier,start,length); identifier[length]=0;
        if (!integer(identifier,1,INT64_MAX,&id)) return failure(status,400,"Identificativo anagrafica non valido.");
        suffix=slash ? slash+1 : "";
        if (slash && !*suffix) return failure(status,404,"Percorso anagrafica non trovato.");
        if (!strcmp(suffix,"assets")) return NULL;
    }
    bool history=!strcmp(suffix,"history"),links=!strcmp(suffix,identity ? "people" : "identities");
    if (identity && !strncmp(suffix,"people/",7)) {
        const char *start=suffix+7,*slash=strchr(start,'/'); char identifier[32];
        if (!slash || strcmp(slash,"/remove")) return failure(status,404,"Percorso collegamento non trovato.");
        size_t length=(size_t)(slash-start);
        if (!length || length>=sizeof(identifier)) return failure(status,400,"Identificativo collegamento non valido.");
        memcpy(identifier,start,length); identifier[length]=0;
        if (!integer(identifier,1,INT64_MAX,&remove)) return failure(status,400,"Identificativo collegamento non valido.");
        links=true;
    }
    if (*suffix && !history && !links) return failure(status,404,"Percorso anagrafica non trovato.");
    if (!get && !post) return failure(status,405,"Metodo non consentito; disattivare le anagrafiche tramite il loro stato.");
    if (get && request->length) return failure(status,400,"Corpo richiesta inatteso.");
    if (get) {
        if (remove) return failure(status,405,"La rimozione richiede POST.");
        if (history) return directory_history(identity,id,query,status);
        if (links) return directory_links(identity,id,query,status);
        if (!id) return directory_list(identity,query,status);
        if (*query) return failure(status,400,"Parametri inattesi per il dettaglio dell'anagrafica.");
        bool error=false; json_object *entity=directory_load(identity,id,&error);
        if (!entity) return failure(status,error ? 500 : 404,"Anagrafica non trovata.");
        json_object *o=ok_json(); json_object_object_add(o,identity ? "identity" : "person",entity); return o;
    }
    if (history || (links && !identity)) return failure(status,405,"Collegamenti e storico consultabili in sola lettura da questo percorso.");
    if (*query) return failure(status,400,"Parametri inattesi per il salvataggio.");
    if (strcasecmp(request->content_type,"application/x-www-form-urlencoded")) return failure(status,415,"Formato richiesto: application/x-www-form-urlencoded.");
    if (request->length>32768) return failure(status,413,"Richiesta anagrafica troppo grande.");
    Form form;
    if (!form_parse(request->body,&form)) return failure(status,400,"Modulo non valido o campi duplicati.");
    json_object *o=links ? directory_link_save(id,remove,&form,status) : directory_save(identity,id,&form,status);
    form_free(&form); return o;
}
