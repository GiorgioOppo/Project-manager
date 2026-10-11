/* Structured assignments and append-only custody/verification records. */
/* Schema 6 keeps legacy application users while allowing real people without a login. */
static bool custody_people_migrate(void) {
    sqlite3_int64 sequence=0;
    sqlite3_stmt *s=prepare("SELECT seq FROM sqlite_sequence WHERE name='asset_custody'");
    if (!s) return false;
    int result=sqlite3_step(s);
    if (result==SQLITE_ROW) sequence=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) return false;
    /* Preserve also indexes added by an installation, not only the standard ones. */
    json_object *indexes=json_object_new_array();
    s=prepare("SELECT sql FROM sqlite_master WHERE type='index' AND tbl_name='asset_custody' AND sql IS NOT NULL ORDER BY name");
    if (!s) { json_object_put(indexes); return false; }
    while ((result=sqlite3_step(s))==SQLITE_ROW)
        json_object_array_add(indexes,json_object_new_string((const char *)sqlite3_column_text(s,0)));
    sqlite3_finalize(s);
    if (result!=SQLITE_DONE) { json_object_put(indexes); return false; }
    bool ok=execute(
        "ALTER TABLE asset_context ADD COLUMN assignee_person_id INTEGER REFERENCES inventory_people(id) CHECK(assignee_person_id>0);"
        "ALTER TABLE asset_context ADD COLUMN assignee_person_name TEXT NOT NULL DEFAULT '';"
        "CREATE INDEX asset_context_person ON asset_context(assignee_person_id,asset_id);"
        "CREATE TABLE asset_custody_next(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),"
        "user_id INTEGER CHECK(user_id>0),user_name TEXT NOT NULL DEFAULT '',checkout_on TEXT NOT NULL,due_on TEXT NOT NULL DEFAULT '',"
        "condition_out TEXT NOT NULL,notes_out TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,"
        "site_id INTEGER REFERENCES inventory_places(id),site_name TEXT NOT NULL DEFAULT '',department_id INTEGER REFERENCES inventory_places(id),department_name TEXT NOT NULL DEFAULT '',"
        "location_id INTEGER REFERENCES inventory_places(id),location_name TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),"
        "returned_on TEXT,condition_in TEXT,notes_in TEXT,returned_actor_id INTEGER,returned_at TEXT,"
        "person_id INTEGER REFERENCES inventory_people(id) CHECK(person_id>0),person_name TEXT NOT NULL DEFAULT '',"
        "CHECK((user_id IS NOT NULL AND person_id IS NULL AND person_name='') OR (user_id IS NULL AND person_id IS NOT NULL AND user_name='')));"
        "INSERT INTO asset_custody_next(id,asset_id,user_id,user_name,checkout_on,due_on,condition_out,notes_out,actor_id,created_at,site_id,site_name,department_id,department_name,location_id,location_name,active,returned_on,condition_in,notes_in,returned_actor_id,returned_at) "
        "SELECT id,asset_id,user_id,user_name,checkout_on,due_on,condition_out,notes_out,actor_id,created_at,site_id,site_name,department_id,department_name,location_id,location_name,active,returned_on,condition_in,notes_in,returned_actor_id,returned_at FROM asset_custody;"
        "DROP TABLE asset_custody;ALTER TABLE asset_custody_next RENAME TO asset_custody;");
    for (size_t i=0;ok && i<json_object_array_length(indexes);i++)
        ok=execute(json_object_get_string(json_object_array_get_idx(indexes,i)));
    json_object_put(indexes);
    if (ok) ok=execute("CREATE INDEX asset_custody_person ON asset_custody(person_id,active,id);");
    if (ok) {
        s=prepare("UPDATE sqlite_sequence SET seq=MAX(seq,?) WHERE name='asset_custody'");
        ok=s && sqlite3_bind_int64(s,1,sequence)==SQLITE_OK && sqlite3_step(s)==SQLITE_DONE;
        sqlite3_finalize(s);
    }
    return ok;
}
static void column_id(json_object *o, const char *key, sqlite3_stmt *s, int column) {
    if (sqlite3_column_type(s,column)==SQLITE_NULL) json_object_object_add(o,key,NULL);
    else addint(o,key,sqlite3_column_int64(s,column));
}
static void bind_id(sqlite3_stmt *s, int column, sqlite3_int64 id) {
    if (id) sqlite3_bind_int64(s,column,id); else sqlite3_bind_null(s,column);
}
static bool optional_id(const Form *form, const char *key, sqlite3_int64 *id) {
    *id=0;
    return !*field(form,key) || integer(field(form,key),1,INT64_MAX,id);
}
static json_object *context_row(sqlite3_stmt *s) {
    json_object *o=json_object_new_object();
    const char *ids[]={"assignee_user_id","site_id","department_id","location_id","assignee_person_id"};
    const char *labels[]={"assignee_user_name","site_name","department_name","location_name","assignee_person_name"};
    for (int i=0;i<5;i++) { column_id(o,ids[i],s,i*2); addstr(o,labels[i],(const char *)sqlite3_column_text(s,i*2+1)); }
    return o;
}
static json_object *empty_context(void) {
    json_object *o=json_object_new_object();
    const char *ids[]={"assignee_user_id","site_id","department_id","location_id","assignee_person_id"};
    const char *labels[]={"assignee_user_name","site_name","department_name","location_name","assignee_person_name"};
    for (int i=0;i<5;i++) { json_object_object_add(o,ids[i],NULL); addstr(o,labels[i],""); }
    return o;
}
#define CUSTODY_COLUMNS "id,user_id,user_name,checkout_on,due_on,condition_out,notes_out,actor_id,created_at,site_id,site_name,department_id,department_name,location_id,location_name,active,returned_on,condition_in,notes_in,returned_actor_id,returned_at,person_id,person_name"
static json_object *custody_row(sqlite3_stmt *s) {
    json_object *o=json_object_new_object();
    addint(o,"id",sqlite3_column_int64(s,0)); column_id(o,"user_id",s,1);
    column_id(o,"person_id",s,21); addstr(o,"person_name",(const char *)sqlite3_column_text(s,22));
    const char *keys[]={"user_name","checkout_on","due_on","condition_out","notes","created_at"};
    const int columns[]={2,3,4,5,6,8};
    for (int i=0;i<6;i++) addstr(o,keys[i],(const char *)sqlite3_column_text(s,columns[i]));
    addint(o,"actor_id",sqlite3_column_int64(s,7));
    column_id(o,"site_id",s,9); addstr(o,"site_name",(const char *)sqlite3_column_text(s,10));
    column_id(o,"department_id",s,11); addstr(o,"department_name",(const char *)sqlite3_column_text(s,12));
    column_id(o,"location_id",s,13); addstr(o,"location_name",(const char *)sqlite3_column_text(s,14));
    json_object_object_add(o,"active",json_object_new_boolean(sqlite3_column_int(s,15)));
    const char *returned[]={"returned_on","condition_in","return_notes","returned_at"};
    const int returned_columns[]={16,17,18,20};
    for (int i=0;i<4;i++) {
        if (sqlite3_column_type(s,returned_columns[i])==SQLITE_NULL) json_object_object_add(o,returned[i],NULL);
        else addstr(o,returned[i],(const char *)sqlite3_column_text(s,returned_columns[i]));
    }
    column_id(o,"returned_actor_id",s,19);
    return o;
}
#define VERIFICATION_COLUMNS "id,verified_on,condition,observed_location_id,observed_location_name,expected_location_id,expected_location_name,location_matches,notes,actor_id,created_at"
static json_object *verification_row(sqlite3_stmt *s) {
    json_object *o=json_object_new_object();
    addint(o,"id",sqlite3_column_int64(s,0)); addstr(o,"verified_on",(const char *)sqlite3_column_text(s,1));
    addstr(o,"condition",(const char *)sqlite3_column_text(s,2));
    column_id(o,"observed_location_id",s,3); addstr(o,"observed_location_name",(const char *)sqlite3_column_text(s,4));
    column_id(o,"expected_location_id",s,5); addstr(o,"expected_location_name",(const char *)sqlite3_column_text(s,6));
    if (sqlite3_column_type(s,7)==SQLITE_NULL) json_object_object_add(o,"location_matches",NULL);
    else json_object_object_add(o,"location_matches",json_object_new_boolean(sqlite3_column_int(s,7)));
    addstr(o,"notes",(const char *)sqlite3_column_text(s,8)); addint(o,"actor_id",sqlite3_column_int64(s,9));
    addstr(o,"created_at",(const char *)sqlite3_column_text(s,10));
    return o;
}
static bool enrich_query(json_object *asset, const char *key, const char *sql, json_object *(*row)(sqlite3_stmt *)) {
    sqlite3_stmt *s=prepare(sql);
    if (!s) return false;
    sqlite3_bind_int64(s,1,jint(asset,"id"));
    int result=sqlite3_step(s);
    if (result==SQLITE_ROW) json_object_object_add(asset,key,row(s));
    else json_object_object_add(asset,key,NULL);
    sqlite3_finalize(s);
    return result==SQLITE_ROW || result==SQLITE_DONE;
}
static bool custody_enrich(json_object *asset) {
    return enrich_query(asset,"context","SELECT assignee_user_id,assignee_user_name,site_id,site_name,department_id,department_name,location_id,location_name,assignee_person_id,assignee_person_name FROM asset_context WHERE asset_id=?",context_row) &&
        enrich_query(asset,"custody","SELECT " CUSTODY_COLUMNS " FROM asset_custody WHERE asset_id=? AND active=1",custody_row) &&
        enrich_query(asset,"last_verification","SELECT " VERIFICATION_COLUMNS " FROM asset_verifications WHERE asset_id=? ORDER BY verified_on DESC,id DESC LIMIT 1",verification_row);
}
static bool tangible(const char *type) {
    return !strcmp(type,"it_device") || !strcmp(type,"network_device") || !strcmp(type,"physical");
}
/* Keep a valid UTF-8 prefix in the legacy 160-byte display fields. */
static void display_name(char result[161], const char *name) {
    size_t size=strlen(name);
    if (size>160) { size=160; while (size && ((unsigned char)name[size]&0xc0)==0x80) size--; }
    memcpy(result,name,size); result[size]=0;
}
static const char *context_assignee_name(json_object *context) {
    return jint(context,"assignee_person_id") ? jstr(context,"assignee_person_name") : jstr(context,"assignee_user_name");
}
static bool custody_asset_compatible(json_object *old, const char *type, const char *const *values) {
    json_object *context=NULL,*custody=NULL;
    json_object_object_get_ex(old,"context",&context); json_object_object_get_ex(old,"custody",&custody);
    if (!context && !custody) return true;
    bool assigned=jint(context,"assignee_user_id") || jint(context,"assignee_person_id");
    bool placed=jint(context,"site_id") || jint(context,"department_id") || jint(context,"location_id");
    if (!assigned && !placed && !custody) return true;
    if (!tangible(type) || (custody && strcmp(type,jstr(old,"asset_type")))) return false;
    char user[161],location[161];
    display_name(user,context_assignee_name(context));
    display_name(location,*jstr(context,"location_name") ? jstr(context,"location_name") : jstr(context,"site_name"));
    if ((assigned || custody) && strcmp(values[8],user)) return false;
    if (placed && strcmp(values[7],location)) return false;
    if (!placed && custody && strcmp(values[7],jstr(old,"location"))) return false;
    if (assigned || custody) return !strcmp(values[6],"assigned");
    return true;
}
static json_object *place_row(sqlite3_stmt *s) {
    json_object *o=json_object_new_object();
    addint(o,"id",sqlite3_column_int64(s,0)); addstr(o,"kind",(const char *)sqlite3_column_text(s,1));
    addstr(o,"name",(const char *)sqlite3_column_text(s,2)); column_id(o,"parent_id",s,3);
    json_object_object_add(o,"active",json_object_new_boolean(sqlite3_column_int(s,4)));
    addint(o,"version",sqlite3_column_int64(s,5)); addint(o,"actor_id",sqlite3_column_int64(s,6));
    addstr(o,"created_at",(const char *)sqlite3_column_text(s,7)); addstr(o,"updated_at",(const char *)sqlite3_column_text(s,8));
    return o;
}
#define PLACE_COLUMNS "id,kind,name,parent_id,active,version,actor_id,created_at,updated_at"
static json_object *load_place(sqlite3_int64 id, bool *error) {
    sqlite3_stmt *s=prepare("SELECT " PLACE_COLUMNS " FROM inventory_places WHERE id=?");
    if (!s) { *error=true; return NULL; }
    sqlite3_bind_int64(s,1,id); int result=sqlite3_step(s);
    json_object *o=result==SQLITE_ROW ? place_row(s) : NULL;
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) *error=true;
    sqlite3_finalize(s); return o;
}
static bool place_kind(const char *kind) { return !strcmp(kind,"site") || !strcmp(kind,"department") || !strcmp(kind,"location"); }
static json_object *list_places(const char *query, int *status) {
    static const char *const allowed[]={"kind","active","limit","after_id"};
    Form form; int limit=30; sqlite3_int64 after=0;
    if (!form_parse(query,&form)) return failure(status,400,"Filtri anagrafiche non validi.");
    bool ok=only_fields(&form,allowed,4);
    if (has_field(&form,"kind") && !place_kind(field(&form,"kind"))) ok=false;
    if (has_field(&form,"active") && strcmp(field(&form,"active"),"0") && strcmp(field(&form,"active"),"1")) ok=false;
    if (has_field(&form,"limit") && !number(field(&form,"limit"),1,100,&limit)) ok=false;
    if (has_field(&form,"after_id") && !integer(field(&form,"after_id"),1,INT64_MAX,&after)) ok=false;
    if (!ok) { form_free(&form); return failure(status,400,"Filtri anagrafiche non validi."); }
    sqlite3_stmt *s=prepare("SELECT " PLACE_COLUMNS " FROM inventory_places WHERE (?1=0 OR id<?1) AND (?2='' OR kind=?2) AND (?3='' OR active=CAST(?3 AS INTEGER)) ORDER BY id DESC LIMIT ?4");
    ok=s && sqlite3_bind_int64(s,1,after)==SQLITE_OK && sqlite3_bind_text(s,2,field(&form,"kind"),-1,SQLITE_TRANSIENT)==SQLITE_OK &&
        sqlite3_bind_text(s,3,field(&form,"active"),-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int(s,4,limit+1)==SQLITE_OK;
    form_free(&form);
    if (!ok) { sqlite3_finalize(s); return failure(status,500,"Impossibile leggere le anagrafiche."); }
    json_object *o=ok_json(),*items=json_object_new_array();
    json_object_object_add(o,"places",items); json_object_object_add(o,"next_after_id",NULL);
    int result,count=0; sqlite3_int64 last=0;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { addint(o,"next_after_id",last); break; }
        json_object_array_add(items,place_row(s)); last=sqlite3_column_int64(s,0); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_DONE && result!=SQLITE_ROW) { json_object_put(o); return failure(status,500,"Lettura anagrafiche interrotta."); }
    return o;
}
static json_object *save_place(sqlite3_int64 id, Form *form, int *status) {
    static const char *const allowed[]={"kind","name","parent_id","active","version","actor_id"};
    sqlite3_int64 actor,version=0,parent=0;
    const char *kind=field(form,"kind"),*name=field(form,"name");
    int active=1;
    if (!only_fields(form,allowed,6) || !place_kind(kind) || !nonblank(name) || strlen(name)>160 || !utf8_text(name,false) ||
        !integer(field(form,"actor_id"),1,INT64_MAX,&actor) || !optional_id(form,"parent_id",&parent) ||
        (id && !integer(field(form,"version"),1,INT64_MAX-1,&version)) ||
        (has_field(form,"active") && !number(field(form,"active"),0,1,&active)) ||
        (!strcmp(kind,"site") && parent) || (!strcmp(kind,"location") && !parent) || (id && parent==id))
        return failure(status,400,"Anagrafica, autore o versione non validi.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    bool error=false; int code=500; const char *message="Salvataggio anagrafica non riuscito.";
    json_object *old=id ? load_place(id,&error) : NULL,*parent_place=NULL,*place=NULL;
    if (id && !old) { code=error ? 500 : 404; message="Anagrafica non trovata."; goto fail; }
    if (old && jint(old,"version")!=version) { code=409; message="L'anagrafica è stata modificata; ricaricare i dati."; goto fail; }
    if (old && strcmp(kind,jstr(old,"kind"))) { code=409; message="La tipologia dell'anagrafica non può essere modificata."; goto fail; }
    if (old && jint(old,"parent_id")!=parent) {
        sqlite3_stmt *references=prepare("SELECT 1 FROM asset_context WHERE site_id=?1 OR department_id=?1 OR location_id=?1 LIMIT 1");
        if (!references) goto fail;
        sqlite3_bind_int64(references,1,id); int referenced=sqlite3_step(references); sqlite3_finalize(references);
        if (referenced==SQLITE_ROW) { code=409; message="L'anagrafica è usata da beni: aggiornare prima le loro assegnazioni e ubicazioni."; goto fail; }
        if (referenced!=SQLITE_DONE) goto fail;
    }
    if (parent) {
        parent_place=load_place(parent,&error);
        if (!parent_place || strcmp(jstr(parent_place,"kind"),"site") ||
            (!jint(parent_place,"active") && (!old || jint(old,"parent_id")!=parent))) {
            code=error ? 500 : 400; message="Il padre deve essere una sede attiva."; goto fail;
        }
    }
    char timestamp[32]; if (!timestamp_now(timestamp)) goto fail;
    sqlite3_stmt *s=prepare(id ? "UPDATE inventory_places SET name=?1,parent_id=?2,active=?3,version=version+1,actor_id=?4,updated_at=?5 WHERE id=?6 AND version=?7" :
        "INSERT INTO inventory_places(name,parent_id,active,actor_id,created_at,updated_at,kind) VALUES(?1,?2,?3,?4,?5,?5,?8)");
    bool ok=s && sqlite3_bind_text(s,1,name,-1,SQLITE_TRANSIENT)==SQLITE_OK && sqlite3_bind_int(s,3,active)==SQLITE_OK &&
        sqlite3_bind_int64(s,4,actor)==SQLITE_OK && sqlite3_bind_text(s,5,timestamp,-1,SQLITE_TRANSIENT)==SQLITE_OK;
    if (s) bind_id(s,2,parent);
    if (id && s) { sqlite3_bind_int64(s,6,id); sqlite3_bind_int64(s,7,version); }
    if (!id && s) sqlite3_bind_text(s,8,kind,-1,SQLITE_TRANSIENT);
    int result=ok ? sqlite3_step(s) : SQLITE_ERROR,extended=sqlite3_extended_errcode(database); sqlite3_finalize(s);
    if (result!=SQLITE_DONE || sqlite3_changes(database)!=1) {
        if (extended==SQLITE_CONSTRAINT_UNIQUE) { code=409; message="Anagrafica già presente per questa sede."; } goto fail;
    }
    bool created=!id; if (created) id=sqlite3_last_insert_rowid(database);
    place=load_place(id,&error);
    if (!place || error || !execute("COMMIT")) goto fail;
    if (old) json_object_put(old);
    if (parent_place) json_object_put(parent_place);
    json_object *o=ok_json(); json_object_object_add(o,"place",place); *status=created ? 201 : 200; return o;
fail:
    (void)execute("ROLLBACK"); if (old) json_object_put(old); if (parent_place) json_object_put(parent_place); if (place) json_object_put(place);
    return failure(status,code,message);
}
static bool good_condition(const char *value) { return !strcmp(value,"good") || !strcmp(value,"fair") || !strcmp(value,"damaged"); }
static bool effective_date(const char *value, const char *today) { return *value && valid_date(value) && strcmp(value,today)<=0; }
static bool context_write(sqlite3_int64 asset_id, json_object *context) {
    sqlite3_stmt *s=prepare("INSERT INTO asset_context(asset_id,assignee_user_id,assignee_user_name,site_id,site_name,department_id,department_name,location_id,location_name,assignee_person_id,assignee_person_name) VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(asset_id) DO UPDATE SET assignee_user_id=excluded.assignee_user_id,assignee_user_name=excluded.assignee_user_name,site_id=excluded.site_id,site_name=excluded.site_name,department_id=excluded.department_id,department_name=excluded.department_name,location_id=excluded.location_id,location_name=excluded.location_name,assignee_person_id=excluded.assignee_person_id,assignee_person_name=excluded.assignee_person_name");
    if (!s) return false;
    sqlite3_bind_int64(s,1,asset_id);
    const char *ids[]={"assignee_user_id","site_id","department_id","location_id","assignee_person_id"};
    const char *labels[]={"assignee_user_name","site_name","department_name","location_name","assignee_person_name"};
    for (int i=0;i<5;i++) { bind_id(s,i*2+2,jint(context,ids[i])); sqlite3_bind_text(s,i*2+3,jstr(context,labels[i]),-1,SQLITE_TRANSIENT); }
    bool ok=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s); return ok;
}
static json_object *validated_context(Form *form, json_object *old, bool *db_error) {
    sqlite3_int64 ids[4]={0};
    const char *keys[]={"assignee_user_id","site_id","department_id","location_id"};
    const char *labels[]={"assignee_user_name","site_name","department_name","location_name"};
    for (int i=0;i<4;i++) if (!optional_id(form,keys[i],&ids[i])) return NULL;
    sqlite3_int64 person_id=0;
    if (!optional_id(form,"assignee_person_id",&person_id) || (person_id && ids[0])) return NULL;
    const char *user=field(form,"assignee_user_name");
    if (strlen(user)>480 || !utf8_text(user,false) || (ids[0] ? !nonblank(user) : *user!=0)) return NULL;
    json_object *context=empty_context(),*places[3]={NULL,NULL,NULL},*person=NULL;
    if (ids[0]) addint(context,keys[0],ids[0]);
    addstr(context,labels[0],user);
    bool valid=true;
    if (person_id) {
        person=directory_load(false,person_id,db_error);
        if (!person || (!directory_active(person) && jint(old,"assignee_person_id")!=person_id)) valid=false;
        else { addint(context,"assignee_person_id",person_id); addstr(context,"assignee_person_name",jstr(person,"display_name")); }
    }
    for (int i=0;i<3;i++) if (ids[i+1]) {
        places[i]=load_place(ids[i+1],db_error);
        const char *kind=i==0 ? "site" : i==1 ? "department" : "location";
        if (!places[i] || strcmp(jstr(places[i],"kind"),kind) ||
            (!jint(places[i],"active") && jint(old,keys[i+1])!=ids[i+1])) { valid=false; break; }
        addint(context,keys[i+1],ids[i+1]); addstr(context,labels[i+1],jstr(places[i],"name"));
    }
    if (places[1] && jint(places[1],"parent_id") && jint(places[1],"parent_id")!=ids[1]) valid=false;
    if (places[2] && jint(places[2],"parent_id")!=ids[1]) valid=false;
    for (int i=0;i<3;i++) if (places[i]) json_object_put(places[i]);
    if (person) json_object_put(person);
    if (!valid || *db_error) { json_object_put(context); return NULL; }
    return context;
}
static json_object *custody_mutation(sqlite3_int64 id, const char *operation, Form *form, int *status) {
    static const char *const context_fields[]={"version","actor_id","assignee_user_id","assignee_user_name","site_id","department_id","location_id","assignee_person_id"};
    static const char *const checkout_fields[]={"version","actor_id","user_id","user_name","checkout_on","due_on","condition_out","notes","person_id"};
    static const char *const checkin_fields[]={"version","actor_id","returned_on","condition_in","notes","status"};
    static const char *const verify_fields[]={"version","actor_id","verified_on","condition","observed_location_id","notes"};
    bool is_context=!strcmp(operation,"context"),is_out=!strcmp(operation,"checkout"),is_in=!strcmp(operation,"checkin");
    const char *const *allowed=is_context ? context_fields : is_out ? checkout_fields : is_in ? checkin_fields : verify_fields;
    size_t count=is_context ? 8 : is_out ? 9 : 6;
    sqlite3_int64 actor,version,user_id=0,person_id=0,observed_id=0;
    char today[11];
    const char *notes=field(form,"notes"),*user_name=field(form,"user_name"),*due=field(form,"due_on");
    const char *effective=field(form,is_out ? "checkout_on" : is_in ? "returned_on" : "verified_on");
    const char *condition=field(form,is_out ? "condition_out" : is_in ? "condition_in" : "condition");
    const char *return_status=has_field(form,"status") ? field(form,"status") : "available";
    if (!only_fields(form,allowed,count) || !integer(field(form,"actor_id"),1,INT64_MAX,&actor) ||
        !integer(field(form,"version"),1,INT64_MAX-1,&version) || !business_date(today) ||
        (!is_context && (strlen(notes)>2000 || !utf8_text(notes,true) || !effective_date(effective,today) || !good_condition(condition))) ||
        (is_out && (!optional_id(form,"user_id",&user_id) || !optional_id(form,"person_id",&person_id) || (user_id!=0)==(person_id!=0) ||
            (user_id ? !nonblank(user_name) : *user_name!=0) || strlen(user_name)>480 || !utf8_text(user_name,false) || !valid_date(due) || (*due && strcmp(due,effective)<0))) ||
        (is_in && strcmp(return_status,"available") && strcmp(return_status,"maintenance")) ||
        (!is_context && !is_out && !is_in && !optional_id(form,"observed_location_id",&observed_id)))
        return failure(status,400,"Dati, condizione, date o versione non validi.");
    if (!execute("BEGIN IMMEDIATE")) return failure(status,503,"Archivio inventario temporaneamente occupato.");
    bool error=false; int code=500; const char *message="Operazione inventario non riuscita.";
    json_object *old=load_asset(id,&error),*asset=NULL,*context=NULL,*observed=NULL,*record=NULL,*change=NULL,*active=NULL,*old_context=NULL,*person=NULL;
    if (!old) { code=error ? 500 : 404; message="Bene non trovato."; goto fail; }
    if (jint(old,"version")!=version) { code=409; message="Il bene è stato modificato: ricaricare i dati."; goto fail; }
    if (!tangible(jstr(old,"asset_type"))) { code=400; message="L'operazione richiede un bene fisico o un dispositivo."; goto fail; }
    json_object_object_get_ex(old,"context",&old_context); json_object_object_get_ex(old,"custody",&active);
    const char *new_status=jstr(old,"status");
    if (is_context) {
        context=validated_context(form,old_context,&error);
        if (!context) { code=error ? 500 : 400; message="Persona, utente o luoghi non validi oppure appartenenti a sedi diverse."; goto fail; }
        if (active && (jint(context,"assignee_user_id")!=jint(active,"user_id") || jint(context,"assignee_person_id")!=jint(active,"person_id"))) { code=409; message="Restituire il bene prima di cambiare assegnatario."; goto fail; }
        if (jint(context,"assignee_user_id") || jint(context,"assignee_person_id")) {
            if (strcmp(new_status,"available") && strcmp(new_status,"assigned")) { code=409; message="Il bene non è disponibile per l'assegnazione."; goto fail; }
            new_status="assigned";
        } else if (!strcmp(new_status,"assigned")) new_status="available";
    } else {
        if (old_context) {
            context=json_tokener_parse(json_object_to_json_string_ext(old_context,JSON_C_TO_STRING_PLAIN));
        } else context=empty_context();
        if (!context) goto fail;
    }
    if (is_out) {
        if (person_id) {
            person=directory_load(false,person_id,&error);
            if (!directory_active(person)) { code=error ? 500 : 400; message="La consegna richiede una persona attiva."; goto fail; }
        }
        bool same_assignment=!strcmp(new_status,"assigned") && jint(context,"assignee_user_id")==user_id && jint(context,"assignee_person_id")==person_id;
        if (active || (strcmp(new_status,"available") && !same_assignment)) { code=409; message="Il bene deve essere disponibile o già assegnato allo stesso destinatario, senza una consegna aperta."; goto fail; }
        if (user_id) addint(context,"assignee_user_id",user_id); else json_object_object_add(context,"assignee_user_id",NULL);
        addstr(context,"assignee_user_name",user_name);
        if (person_id) addint(context,"assignee_person_id",person_id); else json_object_object_add(context,"assignee_person_id",NULL);
        addstr(context,"assignee_person_name",jstr(person,"display_name")); new_status="assigned";
    }
    if (is_in) {
        if (!active) { code=409; message="Il bene non ha una consegna aperta."; goto fail; }
        if (strcmp(effective,jstr(active,"checkout_on"))<0) { code=400; message="La restituzione non può precedere la consegna."; goto fail; }
        json_object_object_add(context,"assignee_user_id",NULL); addstr(context,"assignee_user_name","");
        json_object_object_add(context,"assignee_person_id",NULL); addstr(context,"assignee_person_name",""); new_status=return_status;
    }
    if (!is_context && !is_out && !is_in && observed_id) {
        observed=load_place(observed_id,&error);
        if (!observed || strcmp(jstr(observed,"kind"),"location") || !jint(observed,"active")) {
            code=error ? 500 : 400; message="L'ubicazione osservata deve essere attiva."; goto fail;
        }
    }
    char timestamp[32]; if (!timestamp_now(timestamp)) goto fail;
    sqlite3_int64 record_id=0;
    sqlite3_stmt *s=NULL;
    if (is_out) {
        s=prepare("INSERT INTO asset_custody(asset_id,user_id,user_name,checkout_on,due_on,condition_out,notes_out,actor_id,created_at,site_id,site_name,department_id,department_name,location_id,location_name,person_id,person_name) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!s) goto fail;
        sqlite3_bind_int64(s,1,id); bind_id(s,2,user_id); sqlite3_bind_text(s,3,user_name,-1,SQLITE_TRANSIENT);
        bind_id(s,16,person_id); sqlite3_bind_text(s,17,jstr(person,"display_name"),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,4,effective,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,5,due,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,6,condition,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,7,notes,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,8,actor); sqlite3_bind_text(s,9,timestamp,-1,SQLITE_TRANSIENT);
        const char *ids[]={"site_id","department_id","location_id"},*labels[]={"site_name","department_name","location_name"};
        for (int i=0;i<3;i++) { bind_id(s,10+i*2,jint(context,ids[i])); sqlite3_bind_text(s,11+i*2,jstr(context,labels[i]),-1,SQLITE_TRANSIENT); }
    } else if (is_in) {
        record_id=jint(active,"id");
        s=prepare("UPDATE asset_custody SET active=0,returned_on=?,condition_in=?,notes_in=?,returned_actor_id=?,returned_at=? WHERE id=? AND asset_id=? AND active=1");
        if (!s) goto fail;
        sqlite3_bind_text(s,1,effective,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,condition,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,notes,-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(s,4,actor); sqlite3_bind_text(s,5,timestamp,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,6,record_id); sqlite3_bind_int64(s,7,id);
    } else if (!is_context) {
        sqlite3_int64 expected=jint(context,"location_id");
        s=prepare("INSERT INTO asset_verifications(asset_id,verified_on,condition,observed_location_id,observed_location_name,expected_location_id,expected_location_name,location_matches,notes,actor_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        if (!s) goto fail;
        sqlite3_bind_int64(s,1,id); sqlite3_bind_text(s,2,effective,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,condition,-1,SQLITE_TRANSIENT);
        bind_id(s,4,observed_id); sqlite3_bind_text(s,5,jstr(observed,"name"),-1,SQLITE_TRANSIENT); bind_id(s,6,expected);
        sqlite3_bind_text(s,7,jstr(context,"location_name"),-1,SQLITE_TRANSIENT);
        if (expected && observed_id) sqlite3_bind_int(s,8,expected==observed_id); else sqlite3_bind_null(s,8);
        sqlite3_bind_text(s,9,notes,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,10,actor); sqlite3_bind_text(s,11,timestamp,-1,SQLITE_TRANSIENT);
    }
    if (s) {
        bool ok=sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1; sqlite3_finalize(s);
        if (!ok) goto fail;
        if (!record_id) record_id=sqlite3_last_insert_rowid(database);
    }
    bool context_changed=is_context || is_out || is_in;
    if (context_changed && !context_write(id,context)) goto fail;
    char user_display[161],location_display[161];
    display_name(user_display,context_assignee_name(context));
    display_name(location_display,*jstr(context,"location_name") ? jstr(context,"location_name") : jstr(context,"site_name"));
    if ((is_out || is_in) && !jint(context,"site_id") && !jint(context,"location_id")) display_name(location_display,jstr(old,"location"));
    s=prepare(context_changed ? "UPDATE assets SET status=?,assignee=?,location=?,actor_id=?,version=version+1,updated_at=? WHERE id=? AND version=?" :
        "UPDATE assets SET actor_id=?4,version=version+1,updated_at=?5 WHERE id=?6 AND version=?7");
    if (!s) goto fail;
    if (context_changed) { sqlite3_bind_text(s,1,new_status,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,user_display,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,location_display,-1,SQLITE_TRANSIENT); }
    sqlite3_bind_int64(s,4,actor); sqlite3_bind_text(s,5,timestamp,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,6,id); sqlite3_bind_int64(s,7,version);
    bool ok=sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(database)==1; sqlite3_finalize(s);
    if (!ok) goto fail;
    asset=load_asset(id,&error); if (!asset || error) goto fail;
    change=json_object_new_object(); addstr(change,"operation",is_context ? "context_updated" : is_out ? "checked_out" : is_in ? "checked_in" : "verified");
    if (is_context) json_object_object_add(change,"context",json_object_get(context));
    else {
        s=prepare(is_out || is_in ? "SELECT " CUSTODY_COLUMNS " FROM asset_custody WHERE id=?" : "SELECT " VERIFICATION_COLUMNS " FROM asset_verifications WHERE id=?");
        if (!s) goto fail;
        sqlite3_bind_int64(s,1,record_id); int result=sqlite3_step(s);
        if (result==SQLITE_ROW) record=is_out || is_in ? custody_row(s) : verification_row(s);
        sqlite3_finalize(s); if (!record) goto fail;
        json_object_object_add(change,is_out || is_in ? "custody" : "verification",json_object_get(record));
    }
    if (!history_write(id,"updated",actor,version+1,timestamp,asset,change) || !execute("COMMIT")) {
        message="Storico inventario non salvato; operazione annullata."; goto fail;
    }
    json_object_put(old); json_object_put(context); if (observed) json_object_put(observed); if (person) json_object_put(person); json_object_put(change);
    json_object *o=ok_json(); json_object_object_add(o,"asset",asset);
    if (record) json_object_object_add(o,is_out || is_in ? "custody" : "verification",record);
    *status=is_out || (!is_context && !is_in) ? 201 : 200; return o;
fail:
    (void)execute("ROLLBACK"); if (old) json_object_put(old); if (asset) json_object_put(asset); if (context) json_object_put(context);
    if (observed) json_object_put(observed);
    if (record) json_object_put(record);
    if (change) json_object_put(change);
    if (person) json_object_put(person);
    return failure(status,code,message);
}
static json_object *custody_history(sqlite3_int64 id, bool verification, const char *query, int *status) {
    Form form; int limit; sqlite3_int64 after;
    if (!page_query(query,true,&form,&limit,&after)) return failure(status,400,"Filtri storico non validi.");
    form_free(&form);
    bool error=false; json_object *asset=load_asset(id,&error);
    if (!asset) return failure(status,error ? 500 : 404,"Bene non trovato.");
    json_object_put(asset);
    sqlite3_stmt *s=prepare(verification ? "SELECT " VERIFICATION_COLUMNS " FROM asset_verifications WHERE asset_id=?1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3" :
        "SELECT " CUSTODY_COLUMNS " FROM asset_custody WHERE asset_id=?1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT ?3");
    if (!s) return failure(status,500,"Impossibile leggere lo storico.");
    sqlite3_bind_int64(s,1,id); sqlite3_bind_int64(s,2,after); sqlite3_bind_int(s,3,limit+1);
    json_object *o=ok_json(),*items=json_object_new_array();
    json_object_object_add(o,verification ? "verifications" : "custody",items); json_object_object_add(o,"next_after_id",NULL);
    size_t bytes=strlen(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN));
    int result,count=0; sqlite3_int64 last=0; bool more=false;
    while ((result=sqlite3_step(s))==SQLITE_ROW) {
        if (count>=limit) { more=true; break; }
        json_object *item=verification ? verification_row(s) : custody_row(s);
        size_t length=strlen(json_object_to_json_string_ext(item,JSON_C_TO_STRING_PLAIN));
        if (bytes+length+64>RESPONSE_LIMIT) { more=true; json_object_put(item); break; }
        bytes+=length+1; json_object_array_add(items,item); last=sqlite3_column_int64(s,0); count++;
    }
    sqlite3_finalize(s);
    if (result!=SQLITE_DONE && result!=SQLITE_ROW) { json_object_put(o); return failure(status,500,"Lettura storico interrotta."); }
    if (more && last) addint(o,"next_after_id",last);
    return o;
}
