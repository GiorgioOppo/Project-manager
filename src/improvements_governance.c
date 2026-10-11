#include "improvements_governance.h"
#include "improvements.h"
#include "storage.h"
#include "organization.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IG_LIMIT 200

static bool ig_prepare(Db *db, DbStmt **s, const char *sql) {
    return db_prepare_v2(db,sql,-1,s,NULL)==DB_OK;
}
static bool ig_id(DbStmt *s,int i,DbInt v) { return db_bind_int64(s,i,v)==DB_OK;
}
static bool ig_text(DbStmt *s,int i,const char *v) { return db_bind_text(s,i,v,-1,DB_TRANSIENT)==DB_OK;
}
static const char *ig_column(DbStmt *s,int i) {
    const unsigned char *v=db_column_text(s,i);
    return v?(const char *)v:"";
}
static bool ig_scalar(Db *db,const char *sql,DbInt id,DbInt *out) {
    DbStmt *s=NULL;
    bool okay=ig_prepare(db,&s,sql) && (id<0 || ig_id(s,1,id)) && db_step(s)==DB_ROW;
    if (okay) *out=db_column_int64(s,0);
    db_finalize(s);
    return okay;
}
static bool ig_stamp(char out[21]) {
    time_t now=time(NULL);
    struct tm utc;
    return now!=(time_t)-1 && gmtime_r(&now,&utc) && strftime(out,21,"%Y-%m-%dT%H:%M:%SZ",&utc)==20;
}
static bool ig_fields_allowed(struct json_object *fields,const char *const *names) {
    if (!fields) return false;
    json_object_object_foreach(fields,key,value) {
        (void)value;
        size_t i=0;
        while (names[i] && strcmp(names[i],key)) i++;
        if (!names[i]) return false;
    }
    return true;
}
static bool ig_reason(const char *reason) {
    size_t n=strlen(reason);
    bool content=false;
    if (!n || n>2000) return false;
    for (size_t i=0;i<n;i++) { unsigned char c=(unsigned char)reason[i];
    if (c<32 && c!='\n' && c!='\t') return false;
    if (!isspace(c)) content=true;
    }
    return content;
}
static struct json_object *ig_groups(const char *text) {
    if (!text || !*text || strlen(text)>100) return NULL;
    struct json_tokener *t=json_tokener_new();
    if (!t) return NULL;
    json_tokener_set_flags(t,JSON_TOKENER_STRICT|JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *a=json_tokener_parse_ex(t,text,(int)strlen(text));
    size_t end=json_tokener_get_parse_end(t);
    bool okay=json_tokener_get_error(t)==json_tokener_success;
    while (text[end] && isspace((unsigned char)text[end])) end++;
    okay=okay && !text[end] && a && json_object_is_type(a,json_type_array) && json_object_array_length(a)<=2;
    bool sales=false,finance=false;
    for (size_t i=0;okay && i<json_object_array_length(a);i++) {
        struct json_object *v=json_object_array_get_idx(a,i);
        if (!json_object_is_type(v,json_type_string)) {okay=false;
        break;
        }
        const char *code=json_object_get_string(v);
        if ((size_t)json_object_get_string_len(v)!=strlen(code)) {okay=false;
        break;
        }
        if (!strcmp(code,"sales") && !sales) sales=true;
        else if (!strcmp(code,"finance") && !finance) finance=true;
        else okay=false;
    }
    json_tokener_free(t);
    if (a) json_object_put(a);
    if (!okay) return NULL;
    a=json_object_new_array();
    if (sales) json_object_array_add(a,json_object_new_string("sales"));
    if (finance) json_object_array_add(a,json_object_new_string("finance"));
    return a;
}
static int ig_policy(Db *db,DbInt actor,AccessPolicy *policy,WorkspaceResponse *r) {
    int found=access_policy_resolve(db,actor,NULL,policy);
    if (found<0) im_error(r,503,"La verifica dei permessi non è disponibile.");
    else if (!found) im_error(r,401,"Accedi per continuare.");
    return found;
}
static bool ig_admin(Db *db,DbInt actor,WorkspaceResponse *r) {
    AccessPolicy p;
    if (ig_policy(db,actor,&p,r)!=1) return false;
    if (p.admin) return true;
    im_error(r,403,"Questa operazione richiede l'amministratore dello spazio.");
    return false;
}
static DbInt ig_admin_count(Db *db) {
    DbInt count=-1;
    (void)ig_scalar(db,"SELECT COUNT(*) FROM access_space_admins a JOIN users u ON u.id=a.user_id WHERE u.active=1 AND u.role='manager'",-1,&count);
    return count;
}

bool improvements_governance_schema(Db *db) {
#ifdef PUNTO_MYSQL
    const char *ddl[]={
        "CREATE TABLE IF NOT EXISTS access_governance_state(id BIGINT NOT NULL PRIMARY KEY,seeded TINYINT NOT NULL DEFAULT 1) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS access_space_admins(user_id BIGINT NOT NULL PRIMARY KEY,created_by BIGINT NULL,created_at VARCHAR(20) NOT NULL DEFAULT '',FOREIGN KEY(user_id) REFERENCES users(id),FOREIGN KEY(created_by) REFERENCES users(id)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS access_area_delegations(id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,manager_id BIGINT NOT NULL,group_id BIGINT NOT NULL,can_manage_jobs TINYINT NOT NULL DEFAULT 0,can_manage_groups TINYINT NOT NULL DEFAULT 0,can_manage_users TINYINT NOT NULL DEFAULT 0,can_approve_corrections TINYINT NOT NULL DEFAULT 0,starts_on VARCHAR(10) NOT NULL,ends_on VARCHAR(10) NOT NULL DEFAULT '',active TINYINT NOT NULL DEFAULT 1,revision BIGINT NOT NULL DEFAULT 1,reason TEXT NOT NULL,created_by BIGINT NOT NULL,created_at VARCHAR(20) NOT NULL,FOREIGN KEY(manager_id) REFERENCES users(id),FOREIGN KEY(group_id) REFERENCES organization_groups(id),FOREIGN KEY(created_by) REFERENCES users(id),INDEX access_area_actor(manager_id,active)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS access_profile_revisions(user_id BIGINT NOT NULL PRIMARY KEY,revision BIGINT NOT NULL DEFAULT 1,FOREIGN KEY(user_id) REFERENCES users(id)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS access_profile_requests(id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,user_id BIGINT NOT NULL,requester_id BIGINT NOT NULL,groups_json TEXT NOT NULL,before_json TEXT NOT NULL,reason TEXT NOT NULL,base_revision BIGINT NOT NULL,status VARCHAR(16) NOT NULL DEFAULT 'pending',pending_user_id BIGINT NULL UNIQUE,approved_by BIGINT NULL,bootstrap TINYINT NOT NULL DEFAULT 0,created_at VARCHAR(20) NOT NULL,decided_at VARCHAR(20) NOT NULL DEFAULT '',decision_reason TEXT NOT NULL,FOREIGN KEY(user_id) REFERENCES users(id),FOREIGN KEY(requester_id) REFERENCES users(id),FOREIGN KEY(approved_by) REFERENCES users(id)) ENGINE=InnoDB",NULL};
#else
    const char *ddl[]={
        "CREATE TABLE IF NOT EXISTS access_governance_state(id INTEGER PRIMARY KEY CHECK(id=1),seeded INTEGER NOT NULL DEFAULT 1 CHECK(seeded=1))",
        "CREATE TABLE IF NOT EXISTS access_space_admins(user_id INTEGER PRIMARY KEY REFERENCES users(id),created_by INTEGER REFERENCES users(id),created_at TEXT NOT NULL DEFAULT '')",
        "CREATE TABLE IF NOT EXISTS access_area_delegations(id INTEGER PRIMARY KEY AUTOINCREMENT,manager_id INTEGER NOT NULL REFERENCES users(id),group_id INTEGER NOT NULL REFERENCES organization_groups(id),can_manage_jobs INTEGER NOT NULL DEFAULT 0 CHECK(can_manage_jobs IN (0,1)),can_manage_groups INTEGER NOT NULL DEFAULT 0 CHECK(can_manage_groups IN (0,1)),can_manage_users INTEGER NOT NULL DEFAULT 0 CHECK(can_manage_users IN (0,1)),can_approve_corrections INTEGER NOT NULL DEFAULT 0 CHECK(can_approve_corrections IN (0,1)),starts_on TEXT NOT NULL,ends_on TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),reason TEXT NOT NULL,created_by INTEGER NOT NULL REFERENCES users(id),created_at TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS access_area_actor ON access_area_delegations(manager_id,active)",
        "CREATE TABLE IF NOT EXISTS access_profile_revisions(user_id INTEGER PRIMARY KEY REFERENCES users(id),revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0))",
        "CREATE TABLE IF NOT EXISTS access_profile_requests(id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER NOT NULL REFERENCES users(id),requester_id INTEGER NOT NULL REFERENCES users(id),groups_json TEXT NOT NULL,before_json TEXT NOT NULL,reason TEXT NOT NULL,base_revision INTEGER NOT NULL,status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','rejected','conflict','cancelled')),pending_user_id INTEGER UNIQUE,approved_by INTEGER REFERENCES users(id),bootstrap INTEGER NOT NULL DEFAULT 0 CHECK(bootstrap IN (0,1)),created_at TEXT NOT NULL,decided_at TEXT NOT NULL DEFAULT '',decision_reason TEXT NOT NULL)",NULL};
#endif
    for (size_t i=0;ddl[i];i++) if (!im_exec(db,ddl[i])) return false;
    DbInt exists=0;
    if (!ig_scalar(db,"SELECT COUNT(*) FROM access_governance_state WHERE id=1",-1,&exists)) return false;
    if (exists) return true;
    /* Existing managers retain administration exactly once. A genuinely empty
       installation marks the migration, then accounts_open seeds its bootstrap. */
    return im_exec(db,"INSERT INTO access_space_admins(user_id,created_by,created_at) SELECT id,NULL,'' FROM users WHERE role='manager'") &&
        im_exec(db,"INSERT INTO access_governance_state(id,seeded) VALUES(1,1)");
}

static struct json_object *ig_profile(Db *db,DbInt user) {
    DbStmt *s=NULL;
    if (!ig_prepare(db,&s,"SELECT u.id,u.role,u.active,COALESCE(r.revision,1) FROM users u LEFT JOIN access_profile_revisions r ON r.user_id=u.id WHERE u.id=?") || !ig_id(s,1,user) || db_step(s)!=DB_ROW) {db_finalize(s);
    return NULL;
    }
    struct json_object *o=json_object_new_object();
    bool active=db_column_int(s,2)==1,manager=!strcmp(ig_column(s,1),"manager");
    json_object_object_add(o,"user_id",json_object_new_int64(user));
    json_object_object_add(o,"role",json_object_new_string(ig_column(s,1)));
    json_object_object_add(o,"active",json_object_new_boolean(active));
    json_object_object_add(o,"revision",json_object_new_int64(db_column_int64(s,3)));
    db_finalize(s);
    s=NULL;
    struct json_object *selected=json_object_new_array(),*effective=json_object_new_array();
    if (active) {json_object_array_add(effective,json_object_new_string("operator"));
    if (manager) json_object_array_add(effective,json_object_new_string("manager"));
    }
    if (!ig_prepare(db,&s,"SELECT group_code FROM access_group_members WHERE user_id=? ORDER BY CASE group_code WHEN 'sales' THEN 0 ELSE 1 END") || !ig_id(s,1,user)) goto failure;
    int step;
    while ((step=db_step(s))==DB_ROW) {
        const char *code=ig_column(s,0);
        if (strcmp(code,"sales") && strcmp(code,"finance")) goto failure;
        json_object_array_add(selected,json_object_new_string(code));
        if (active) json_object_array_add(effective,json_object_new_string(code));
    }
    if (step!=DB_DONE) goto failure;
    db_finalize(s);
    json_object_object_add(o,"selected",selected);
    json_object_object_add(o,"effective",effective);
    return o;
failure:
    db_finalize(s);
    json_object_put(o);
    json_object_put(selected);
    json_object_put(effective);
    return NULL;
}
static const char *ig_json(struct json_object *o) {return json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
}
static bool ig_apply_profiles(Db *db,DbInt user,struct json_object *groups,DbInt revision) {
    DbStmt *s=NULL;
    bool okay=ig_prepare(db,&s,"DELETE FROM access_group_members WHERE user_id=?") && ig_id(s,1,user) && db_step(s)==DB_DONE;
    db_finalize(s);
    s=NULL;
    for (size_t i=0;okay && i<json_object_array_length(groups);i++) {
        okay=ig_prepare(db,&s,"INSERT INTO access_group_members(group_code,user_id) VALUES(?,?)") && ig_text(s,1,json_object_get_string(json_object_array_get_idx(groups,i))) && ig_id(s,2,user) && db_step(s)==DB_DONE;
        db_finalize(s);
        s=NULL;
    }
    DbInt present=0;
    if (okay) okay=ig_scalar(db,"SELECT COUNT(*) FROM access_profile_revisions WHERE user_id=?",user,&present);
    if (okay) okay=ig_prepare(db,&s,present?"UPDATE access_profile_revisions SET revision=? WHERE user_id=?":"INSERT INTO access_profile_revisions(revision,user_id) VALUES(?,?)") && ig_id(s,1,revision+1) && ig_id(s,2,user) && db_step(s)==DB_DONE;
    db_finalize(s);
    return okay;
}

void improvements_governance_profiles(Db *db,DbInt actor,DbInt user,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"groups","reason","revision",NULL};
    struct json_object *f=im_fields(body),*groups=NULL,*before=NULL,*out=NULL;
    if (!ig_fields_allowed(f,names) || !(groups=ig_groups(im_string(f,"groups")))) {im_error(r,400,"Scegli soltanto Commerciali e Finance, senza duplicati.");
    goto done;
    }
    const char *reason=im_string(f,"reason");
    if (!*reason) reason="Richiesta di modifica dei profili funzionali";
    if (!ig_reason(reason)) {im_error(r,400,"Inserisci un motivo valido.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    if (!ig_admin(db,actor,r)) {im_rollback(db);
    goto done;
    }
    before=ig_profile(db,user);
    if (!before) {im_rollback(db);
    im_error(r,404,"Utente non trovato.");
    goto done;
    }
    if (!json_object_get_boolean(json_object_object_get(before,"active"))) {im_rollback(db);
    im_error(r,409,"Il destinatario deve essere attivo.");
    goto done;
    }
    DbInt revision=json_object_get_int64(json_object_object_get(before,"revision")),expected=revision;
    if ((*im_string(f,"revision") && !im_integer(f,"revision",1,INT64_MAX-1,&expected)) || expected!=revision || revision==INT64_MAX) {im_rollback(db);
    im_error(r,409,"I profili sono cambiati: ricarica la versione corrente.");
    goto done;
    }
    DbInt pending=0;
    if (!ig_scalar(db,"SELECT COUNT(*) FROM access_profile_requests WHERE pending_user_id=?",user,&pending)) goto failure;
    if (pending) {im_rollback(db);
    im_error(r,409,"Esiste già una richiesta in attesa per questa persona.");
    goto done;
    }
    DbInt admins=ig_admin_count(db);
    if (admins<1) goto failure;
    bool bootstrap=admins==1;
    char stamp[21];
    if (!ig_stamp(stamp)) goto failure;
    DbStmt *s=NULL;
    bool saved=ig_prepare(db,&s,"INSERT INTO access_profile_requests(user_id,requester_id,groups_json,before_json,reason,base_revision,status,pending_user_id,approved_by,bootstrap,created_at,decided_at,decision_reason) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)") &&
        ig_id(s,1,user) && ig_id(s,2,actor) && ig_text(s,3,ig_json(groups)) && ig_text(s,4,ig_json(json_object_object_get(before,"selected"))) && ig_text(s,5,reason) && ig_id(s,6,revision) && ig_text(s,7,bootstrap?"approved":"pending") &&
        (bootstrap?db_bind_null(s,8)==DB_OK:ig_id(s,8,user)) && (bootstrap?ig_id(s,9,actor):db_bind_null(s,9)==DB_OK) && ig_id(s,10,bootstrap) && ig_text(s,11,stamp) && ig_text(s,12,bootstrap?stamp:"") && ig_text(s,13,bootstrap?"Eccezione bootstrap: unico amministratore attivo":"") && db_step(s)==DB_DONE;
    db_finalize(s);
    if (!saved) goto failure;
    DbInt request=db_last_insert_rowid(db);
    if (bootstrap && !ig_apply_profiles(db,user,groups,revision)) goto failure;
    out=ig_profile(db,user);
    if (!out) goto failure;
    json_object_object_add(out,"ok",json_object_new_boolean(true));
    json_object_object_add(out,"pending",json_object_new_boolean(!bootstrap));
    json_object_object_add(out,"request_id",json_object_new_int64(request));
    json_object_object_add(out,"bootstrap",json_object_new_boolean(bootstrap));
    json_object_object_add(out,"reason",json_object_new_string(reason));
    json_object_object_add(out,"requested_groups",json_object_get(groups));
    if (im_audit_commit(db,actor,bootstrap?"governance.profiles.bootstrap":"governance.profiles.request","user",user,ig_json(before),ig_json(out),r)) {im_reply(r,bootstrap?200:202,out);
    out=NULL;
    }
    goto done;
failure:
    im_rollback(db);
    im_error(r,500,"Richiesta non salvata: nessuna modifica confermata.");
done:
    if (f) json_object_put(f);
    if (groups) json_object_put(groups);
    if (before) json_object_put(before);
    if (out) json_object_put(out);
}

static void ig_profile_decision(Db *db,DbInt actor,DbInt request,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"action","reason",NULL};
    struct json_object *f=im_fields(body),*before=NULL,*groups=NULL;
    const char *action=im_string(f,"action"),*reason=im_string(f,"reason");
    bool approve=!strcmp(action,"approve"),reject=!strcmp(action,"reject"),cancel=!strcmp(action,"cancel");
    if (!ig_fields_allowed(f,names) || (!approve && !reject && !cancel) || !ig_reason(reason)) {im_error(r,400,"Scegli una decisione e inserisci il motivo.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    if (!ig_admin(db,actor,r)) {im_rollback(db);
    goto done;
    }
    DbStmt *s=NULL;
    DbInt user=0,requester=0,revision=0;
    char *old=NULL;
    if (!ig_prepare(db,&s,"SELECT user_id,requester_id,groups_json,before_json,base_revision,status FROM access_profile_requests WHERE id=?") || !ig_id(s,1,request)) goto failure;
    int step=db_step(s);
    if (step==DB_DONE) {db_finalize(s);
    im_rollback(db);
    im_error(r,404,"Richiesta non trovata.");
    goto done;
    }
    if (step!=DB_ROW) goto failure;
    user=db_column_int64(s,0);
    requester=db_column_int64(s,1);
    revision=db_column_int64(s,4);
    groups=ig_groups(ig_column(s,2));
    old=strdup(ig_column(s,3));
    bool pending=!strcmp(ig_column(s,5),"pending");
    db_finalize(s);
    s=NULL;
    if (!groups || !old) {free(old);
    goto failure;
    }
    if (!pending) {free(old);
    im_rollback(db);
    im_error(r,409,"La richiesta è già stata decisa.");
    goto done;
    }
    if ((!cancel && actor==requester) || (cancel && actor!=requester)) {free(old);
    im_rollback(db);
    im_error(r,403,cancel?"Può annullare solo il proponente.":"Serve un secondo amministratore diverso dal proponente.");
    goto done;
    }
    before=ig_profile(db,user);
    AccessPolicy proposer;
    int proposer_active=access_policy_resolve(db,requester,NULL,&proposer);
    if (!before || proposer_active<0) {free(old);
    goto failure;
    }
    bool conflict=revision==INT64_MAX || json_object_get_int64(json_object_object_get(before,"revision"))!=revision ||
        strcmp(old,ig_json(json_object_object_get(before,"selected"))) || !json_object_get_boolean(json_object_object_get(before,"active")) || proposer_active!=1 || !proposer.admin;
    free(old);
    const char *status=conflict?"conflict":approve?"approved":reject?"rejected":"cancelled";
    if (approve && !conflict && !ig_apply_profiles(db,user,groups,revision)) goto failure;
    char stamp[21];
    if (!ig_stamp(stamp)) goto failure;
    if (!ig_prepare(db,&s,"UPDATE access_profile_requests SET status=?,pending_user_id=NULL,approved_by=?,decided_at=?,decision_reason=? WHERE id=? AND status='pending'") || !ig_text(s,1,status) || !ig_id(s,2,actor) || !ig_text(s,3,stamp) || !ig_text(s,4,reason) || !ig_id(s,5,request) || db_step(s)!=DB_DONE || db_changes(db)!=1) goto failure;
    db_finalize(s);
    s=NULL;
    struct json_object *out=ig_profile(db,user);
    if (!out) goto failure;
    json_object_object_add(out,"ok",json_object_new_boolean(!conflict));
    json_object_object_add(out,"status",json_object_new_string(status));
    json_object_object_add(out,"request_id",json_object_new_int64(request));
    json_object_object_add(out,"decision_reason",json_object_new_string(reason));
    if (conflict) json_object_object_add(out,"error",json_object_new_string("La richiesta è in conflitto con profili, account o proponente correnti: crea una nuova richiesta."));
    if (im_audit_commit(db,actor,"governance.profiles.decision","user",user,ig_json(before),ig_json(out),r)) im_reply(r,conflict?409:200,out);
    else json_object_put(out);
    goto done;
failure:
    db_finalize(s);
    im_rollback(db);
    im_error(r,500,"Decisione non salvata: nessuna modifica confermata.");
done:
    if (f) json_object_put(f);
    if (before) json_object_put(before);
    if (groups) json_object_put(groups);
}

static void ig_admin_write(Db *db,DbInt actor,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"user_id","admin","reason",NULL};
    struct json_object *f=im_fields(body);
    DbInt user=0,enabled=0;
    if (!ig_fields_allowed(f,names) || !im_integer(f,"user_id",1,INT64_MAX,&user) || !im_integer(f,"admin",0,1,&enabled) || !ig_reason(im_string(f,"reason"))) {im_error(r,400,"Controlla amministratore, stato e motivo.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    if (!ig_admin(db,actor,r)) {im_rollback(db);
    goto done;
    }
    DbStmt *s=NULL;
    DbInt exists=0;
    if (!ig_prepare(db,&s,"SELECT role,active FROM users WHERE id=?") || !ig_id(s,1,user)) goto failure;
    int step=db_step(s);
    if (step==DB_DONE) {db_finalize(s);
    im_rollback(db);
    im_error(r,404,"Utente non trovato.");
    goto done;
    }
    if (step!=DB_ROW) goto failure;
    bool eligible=!strcmp(ig_column(s,0),"manager") && db_column_int(s,1)==1;
    db_finalize(s);
    s=NULL;
    if (enabled && !eligible) {im_rollback(db);
    im_error(r,400,"Scegli un gestore attivo prima di attribuire l'amministrazione.");
    goto done;
    }
    if (!ig_scalar(db,"SELECT COUNT(*) FROM access_space_admins WHERE user_id=?",user,&exists)) goto failure;
    if (!enabled && exists && eligible && ig_admin_count(db)<=1) {im_rollback(db);
    im_error(r,409,"Deve rimanere almeno un amministratore attivo.");
    goto done;
    }
    char stamp[21];
    if (!ig_stamp(stamp)) goto failure;
    bool saved=true;
    if (enabled && !exists) saved=ig_prepare(db,&s,"INSERT INTO access_space_admins(user_id,created_by,created_at) VALUES(?,?,?)") && ig_id(s,1,user) && ig_id(s,2,actor) && ig_text(s,3,stamp) && db_step(s)==DB_DONE;
    if (!enabled && exists) saved=ig_prepare(db,&s,"DELETE FROM access_space_admins WHERE user_id=?") && ig_id(s,1,user) && db_step(s)==DB_DONE;
    db_finalize(s);
    s=NULL;
    if (!saved) goto failure;
    struct json_object *out=json_object_new_object();
    json_object_object_add(out,"ok",json_object_new_boolean(true));
    json_object_object_add(out,"user_id",json_object_new_int64(user));
    json_object_object_add(out,"admin",json_object_new_boolean(enabled));
    json_object_object_add(out,"reason",json_object_new_string(im_string(f,"reason")));
    if (im_audit_commit(db,actor,"governance.admins.update","user",user,NULL,ig_json(out),r)) im_reply(r,200,out);
    else json_object_put(out);
    goto done;
failure:
    db_finalize(s);
    im_rollback(db);
    im_error(r,500,"Amministratore non aggiornato.");
done:
    if (f) json_object_put(f);
}

static void ig_delegation_write(Db *db,DbInt actor,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"user_id","group_id","can_manage_jobs","can_manage_groups","can_manage_users","can_approve_corrections","starts_on","ends_on","reason",NULL};
    struct json_object *f=im_fields(body);
    DbInt user=0,group=0,flags[4]={0};
    bool okay=ig_fields_allowed(f,names) && im_integer(f,"user_id",1,INT64_MAX,&user) && im_integer(f,"group_id",1,INT64_MAX,&group) && im_date(im_string(f,"starts_on")) &&
        (!*im_string(f,"ends_on") || (im_date(im_string(f,"ends_on")) && strcmp(im_string(f,"starts_on"),im_string(f,"ends_on"))<=0)) && ig_reason(im_string(f,"reason"));
    const char *keys[]={"can_manage_jobs","can_manage_groups","can_manage_users","can_approve_corrections"};
    for (int i=0;i<4;i++) if (*im_string(f,keys[i]) && !im_integer(f,keys[i],0,1,&flags[i])) okay=false;
    if (!okay || !(flags[0]||flags[1]||flags[2]||flags[3])) {im_error(r,400,"Controlla gestore, gruppo, capacità e date inclusive della delega.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    if (!ig_admin(db,actor,r)) {im_rollback(db);
    goto done;
    }
    AccessPolicy target;
    int found=access_policy_resolve(db,user,NULL,&target);
    if (found<0) goto failure;
    if (found!=1 || !target.manager) {im_rollback(db);
    im_error(r,400,"La delega richiede un gestore attivo.");
    goto done;
    }
    DbInt exists=0,count=0;
    if (!ig_scalar(db,"SELECT COUNT(*) FROM organization_groups WHERE id=?",group,&exists) || !ig_scalar(db,"SELECT COUNT(*) FROM access_area_delegations WHERE active=1",-1,&count)) goto failure;
    if (!exists || !organization_groups_available()) {im_rollback(db);
    im_error(r,400,"Scegli un gruppo aziendale disponibile.");
    goto done;
    }
    if (count>=200) {im_rollback(db);
    im_error(r,409,"Il limite è di 200 deleghe attive.");
    goto done;
    }
    char stamp[21];
    if (!ig_stamp(stamp)) goto failure;
    DbStmt *s=NULL;
    bool saved=ig_prepare(db,&s,"INSERT INTO access_area_delegations(manager_id,group_id,can_manage_jobs,can_manage_groups,can_manage_users,can_approve_corrections,starts_on,ends_on,reason,created_by,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)") && ig_id(s,1,user) && ig_id(s,2,group);
    for (int i=0;saved && i<4;i++) saved=ig_id(s,i+3,flags[i]);
    saved=saved && ig_text(s,7,im_string(f,"starts_on")) && ig_text(s,8,im_string(f,"ends_on")) && ig_text(s,9,im_string(f,"reason")) && ig_id(s,10,actor) && ig_text(s,11,stamp) && db_step(s)==DB_DONE;
    db_finalize(s);
    if (!saved) goto failure;
    DbInt id=db_last_insert_rowid(db);
    json_object_object_add(f,"id",json_object_new_int64(id));
    json_object_object_add(f,"ok",json_object_new_boolean(true));
    if (im_audit_commit(db,actor,"governance.delegations.create","governance_delegation",id,NULL,ig_json(f),r)) {im_reply(r,201,f);
    f=NULL;
    }
    goto done;
failure:
    im_rollback(db);
    im_error(r,500,"Delega non salvata.");
done:
    if (f) json_object_put(f);
}

static void ig_delegation_revoke(Db *db,DbInt actor,DbInt id,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"revision","reason",NULL};
    struct json_object *f=im_fields(body);
    DbInt revision=0;
    if (!ig_fields_allowed(f,names) || !im_integer(f,"revision",1,INT64_MAX-1,&revision) || !ig_reason(im_string(f,"reason"))) {im_error(r,400,"Inserisci revisione e motivo della revoca.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    if (!ig_admin(db,actor,r)) {im_rollback(db);
    goto done;
    }
    DbStmt *s=NULL;
    bool saved=ig_prepare(db,&s,"UPDATE access_area_delegations SET active=0,revision=revision+1 WHERE id=? AND revision=? AND active=1") && ig_id(s,1,id) && ig_id(s,2,revision) && db_step(s)==DB_DONE;
    db_finalize(s);
    if (!saved) {im_rollback(db);
    im_error(r,500,"Revoca non salvata.");
    goto done;
    }
    if (db_changes(db)!=1) {im_rollback(db);
    im_error(r,409,"La delega è già cambiata o revocata.");
    goto done;
    }
    json_object_object_add(f,"ok",json_object_new_boolean(true));
    json_object_object_add(f,"id",json_object_new_int64(id));
    if (im_audit_commit(db,actor,"governance.delegations.revoke","governance_delegation",id,NULL,ig_json(f),r)) {im_reply(r,200,f);
    f=NULL;
    }
done:
    if (f) json_object_put(f);
}

static void ig_list(Db *db,DbInt actor,int kind,WorkspaceResponse *r) {
    AccessPolicy policy;
    if (ig_policy(db,actor,&policy,r)!=1) return;
    if (!policy.admin && kind!=1) {im_error(r,403,"L'elenco richiede l'amministratore dello spazio.");
    return;
    }
    const char *sql=kind==0?"SELECT a.user_id,u.name,u.email,u.active FROM access_space_admins a JOIN users u ON u.id=a.user_id ORDER BY a.user_id":
        kind==1?"SELECT d.id,d.manager_id,u.name,d.group_id,g.name,d.can_manage_jobs,d.can_manage_groups,d.can_manage_users,d.can_approve_corrections,d.starts_on,d.ends_on,d.active,d.revision,d.reason FROM access_area_delegations d JOIN users u ON u.id=d.manager_id JOIN organization_groups g ON g.id=d.group_id WHERE (?=1 OR d.manager_id=?) ORDER BY d.id DESC LIMIT 1001":
        "SELECT r.id,r.user_id,u.name,r.requester_id,p.name,r.groups_json,r.reason,r.base_revision,r.status,r.approved_by,r.bootstrap,r.created_at,r.decided_at,r.decision_reason FROM access_profile_requests r JOIN users u ON u.id=r.user_id JOIN users p ON p.id=r.requester_id ORDER BY r.id DESC LIMIT 200";
    DbStmt *s=NULL;
    if (!ig_prepare(db,&s,sql) || (kind==1 && (!ig_id(s,1,policy.admin) || !ig_id(s,2,actor)))) goto failure;
    struct json_object *out=json_object_new_object(),*rows=json_object_new_array();
    int step,count=0;
    while ((step=db_step(s))==DB_ROW) {
        if (++count>(kind==1?1000:200)) {db_finalize(s);
        json_object_put(out);
        json_object_put(rows);
        im_error(r,409,"Troppi risultati: archivia le richieste precedenti.");
        return;
        }
        struct json_object *o=json_object_new_object();
        if (kind==0) {
            json_object_object_add(o,"user_id",json_object_new_int64(db_column_int64(s,0)));
            json_object_object_add(o,"name",json_object_new_string(ig_column(s,1)));
            json_object_object_add(o,"email",json_object_new_string(ig_column(s,2)));
            json_object_object_add(o,"active",json_object_new_boolean(db_column_int(s,3)));
        } else if (kind==1) {
            const char *keys[]={"id","user_id","user_name","group_id","group_name","can_manage_jobs","can_manage_groups","can_manage_users","can_approve_corrections","starts_on","ends_on","active","revision","reason"};
            for (int i=0;i<14;i++) json_object_object_add(o,keys[i],i==2||i==4||i==9||i==10||i==13?json_object_new_string(ig_column(s,i)):json_object_new_int64(db_column_int64(s,i)));
        } else {
            const char *keys[]={"id","user_id","user_name","requester_id","requester_name","groups","reason","base_revision","status","approved_by","bootstrap","created_at","decided_at","decision_reason"};
            for (int i=0;i<14;i++) {
                struct json_object *value=i==5?ig_groups(ig_column(s,i)):(i==2||i==4||i==6||i==8||i>=11)?json_object_new_string(ig_column(s,i)):db_column_type(s,i)==DB_NULL?NULL:json_object_new_int64(db_column_int64(s,i));
                json_object_object_add(o,keys[i],value);
            }
        }
        json_object_array_add(rows,o);
    }
    db_finalize(s);
    s=NULL;
    if (step!=DB_DONE) {json_object_put(out);
    json_object_put(rows);
    goto failure;
    }
    json_object_object_add(out,"ok",json_object_new_boolean(true));
    json_object_object_add(out,kind==0?"admins":kind==1?"delegations":"requests",rows);
    json_object_object_add(out,"bootstrap_single_admin",json_object_new_boolean(ig_admin_count(db)==1));
    json_object_object_add(out,"history_limit",json_object_new_int(kind==1?1000:200));
    im_reply(r,200,out);
    return;
failure:
    db_finalize(s);
    im_error(r,500,"Elenco non disponibile.");
}

typedef struct {DbInt id;
char name[481];
bool manager;
bool active;
OrganizationAccess jobs;
DbInt hours[201];
size_t hour_count;
} IgImpactUser;
static bool ig_contains(const DbInt *ids,size_t count,DbInt value) {for (size_t i=0;i<count;i++) if (ids[i]==value) return true;
return false;
}
static struct json_object *ig_difference(const DbInt *left,size_t n,const DbInt *right,size_t m) {
    struct json_object *a=json_object_new_array();
    for (size_t i=0;i<n;i++) if (!ig_contains(right,m,left[i])) json_object_array_add(a,json_object_new_int64(left[i]));
    return a;
}
static void ig_group_impact(Db *db,DbInt actor,const char *body,WorkspaceResponse *r) {
    const char *const names[]={"group_id","operation","parent_id","user_id",NULL};
    struct json_object *f=im_fields(body);
    DbInt group=0,parent=0,user=0;
    const char *op=im_string(f,"operation");
    bool move=!strcmp(op,"move"),add=!strcmp(op,"add_member"),remove=!strcmp(op,"remove_member");
    if (!ig_fields_allowed(f,names) || !im_integer(f,"group_id",1,INT64_MAX,&group) || (!move&&!add&&!remove) ||
        (move && *im_string(f,"parent_id") && !im_integer(f,"parent_id",0,INT64_MAX,&parent)) ||
        (!move && !im_integer(f,"user_id",1,INT64_MAX,&user)) || (move && *im_string(f,"user_id")) || (!move && *im_string(f,"parent_id"))) {im_error(r,400,"Controlla gruppo e modifica da simulare.");
        goto done;
        }
    if (!organization_groups_available()) {im_error(r,503,"I gruppi aziendali non sono disponibili.");
    goto done;
    }
    if (!im_begin(db,r)) goto done;
    AccessPolicy policy;
    if (ig_policy(db,actor,&policy,r)!=1) {im_rollback(db);
    goto done;
    }
    int allowed=access_policy_can_manage_group(db,actor,group);
    if (allowed<0) goto failure;
    if (allowed!=1 || (move && !parent && !policy.admin) || (move && parent && access_policy_can_manage_group(db,actor,parent)!=1) ||
        (!move && !policy.admin && access_policy_can_manage_user(db,actor,user)!=1)) {im_rollback(db);
        im_error(r,403,"L'anteprima e la destinazione devono rientrare nella tua area delegata.");
        goto done;
        }
    DbInt exists=0;
    if (!ig_scalar(db,"SELECT COUNT(*) FROM organization_groups WHERE id=?",group,&exists)) goto failure;
    if (!exists) {im_rollback(db);
    im_error(r,404,"Gruppo non trovato.");
    goto done;
    }
    if (move && parent) {
        if (!ig_scalar(db,"SELECT COUNT(*) FROM organization_groups WHERE id=?",parent,&exists)) goto failure;
        if (!exists) {im_rollback(db);
        im_error(r,404,"Gruppo superiore non trovato.");
        goto done;
        }
    }
    if (!move) {
        if (!ig_scalar(db,"SELECT COUNT(*) FROM users WHERE id=?",user,&exists)) goto failure;
        if (!exists) {im_rollback(db);
        im_error(r,404,"Utente non trovato.");
        goto done;
        }
        if (add && (!ig_scalar(db,"SELECT active FROM users WHERE id=?",user,&exists) || exists!=1)) {im_rollback(db);
        im_error(r,400,"Seleziona un utente attivo.");
        goto done;
        }
    }
    IgImpactUser *people=calloc(IG_LIMIT,sizeof(*people));
    if (!people) goto failure;
    DbStmt *s=NULL;
    size_t count=0;
    char today[11];
    if (!im_today(today) || !ig_prepare(db,&s,"SELECT id,name,role,active FROM users ORDER BY id LIMIT 201")) {free(people);
    goto failure;
    }
    int step;
    while ((step=db_step(s))==DB_ROW) {
        if (count==IG_LIMIT) {db_finalize(s);
        free(people);
        im_rollback(db);
        im_error(r,409,"L'anteprima supporta fino a 200 persone.");
        goto done;
        }
        IgImpactUser *p=&people[count++];
        p->id=db_column_int64(s,0);
        snprintf(p->name,sizeof(p->name),"%s",ig_column(s,1));
        p->manager=!strcmp(ig_column(s,2),"manager");
        p->active=db_column_int(s,3)==1;
    }
    db_finalize(s);
    s=NULL;
    if (step!=DB_DONE) {free(people);
    goto failure;
    }
    for (size_t i=0;i<count;i++) if ((people[i].active && !organization_access_list(db,people[i].id,today,NULL,&people[i].jobs)) ||
        !access_policy_visible_users(db,people[i].id,people[i].manager,people[i].hours,&people[i].hour_count)) {free(people);
        goto failure;
        }
    bool changed=false;
    if (move) changed=ig_prepare(db,&s,"UPDATE organization_groups SET parent_id=? WHERE id=?") &&
        (parent?ig_id(s,1,parent):db_bind_null(s,1)==DB_OK) && ig_id(s,2,group) && db_step(s)==DB_DONE;
    else {
        DbInt existing=0;
        if (!ig_prepare(db,&s,"SELECT COUNT(*) FROM group_members WHERE group_id=? AND user_id=?") || !ig_id(s,1,group) || !ig_id(s,2,user) || db_step(s)!=DB_ROW) {db_finalize(s);
        free(people);
        goto failure;
        }
        existing=db_column_int64(s,0);
        db_finalize(s);
        s=NULL;
        if ((add && existing) || (remove && !existing)) changed=true;
        else changed=ig_prepare(db,&s,add?"INSERT INTO group_members(group_id,user_id) VALUES(?,?)":"DELETE FROM group_members WHERE group_id=? AND user_id=?") && ig_id(s,1,group) && ig_id(s,2,user) && db_step(s)==DB_DONE;
    }
    db_finalize(s);
    s=NULL;
    if (!changed) {free(people);
    goto failure;
    }
    struct json_object *out=json_object_new_object(),*changes=json_object_new_array();
    for (size_t i=0;i<count;i++) {
        OrganizationAccess after={0};
        DbInt hours[201];
        size_t hour_count=0;
        IgImpactUser *p=&people[i];
        if ((p->active && !organization_access_list(db,p->id,today,NULL,&after)) || !access_policy_visible_users(db,p->id,p->manager,hours,&hour_count)) {free(people);
        json_object_put(out);
        json_object_put(changes);
        im_rollback(db);
        im_error(r,400,"Lo spostamento creerebbe un ciclo o supererebbe i 16 livelli.");
        goto done;
        }
        struct json_object *added=ig_difference(after.job_ids,after.count,p->jobs.job_ids,p->jobs.count),*lost=ig_difference(p->jobs.job_ids,p->jobs.count,after.job_ids,after.count);
        struct json_object *hadded=ig_difference(hours,hour_count,p->hours,p->hour_count),*hlost=ig_difference(p->hours,p->hour_count,hours,hour_count);
        struct json_object *permissions=json_object_new_array();
        for (size_t x=0;x<after.count;x++) for (size_t y=0;y<p->jobs.count;y++) if (after.job_ids[x]==p->jobs.job_ids[y] && after.permissions[x]!=p->jobs.permissions[y]) {
            struct json_object *permission=json_object_new_object();
            json_object_object_add(permission,"job_id",json_object_new_int64(after.job_ids[x]));
            json_object_object_add(permission,"before_can_log_time",json_object_new_boolean((p->jobs.permissions[y]&2)!=0));
            json_object_object_add(permission,"after_can_log_time",json_object_new_boolean((after.permissions[x]&2)!=0));
            json_object_array_add(permissions,permission);
        }
        bool differs=json_object_array_length(added)||json_object_array_length(lost)||json_object_array_length(hadded)||json_object_array_length(hlost)||json_object_array_length(permissions);
        if (differs) {
            struct json_object *item=json_object_new_object();
            json_object_object_add(item,"user_id",json_object_new_int64(p->id));
            json_object_object_add(item,"user_name",json_object_new_string(p->name));
            json_object_object_add(item,"job_permission_changes",permissions);
            json_object_object_add(item,"job_access_added",added);
            json_object_object_add(item,"job_access_removed",lost);
            json_object_object_add(item,"hour_users_added",hadded);
            json_object_object_add(item,"hour_users_removed",hlost);
            json_object_array_add(changes,item);
        } else {json_object_put(added);
        json_object_put(lost);
        json_object_put(hadded);
        json_object_put(hlost);
        json_object_put(permissions);
        }
    }
    free(people);
    im_rollback(db);
    json_object_object_add(out,"ok",json_object_new_boolean(true));
    json_object_object_add(out,"preview",json_object_new_boolean(true));
    json_object_object_add(out,"persisted",json_object_new_boolean(false));
    json_object_object_add(out,"group_id",json_object_new_int64(group));
    json_object_object_add(out,"operation",json_object_new_string(op));
    json_object_object_add(out,"affected_count",json_object_new_int64((int64_t)json_object_array_length(changes)));
    json_object_object_add(out,"changes",changes);
    json_object_object_add(out,"notice",json_object_new_string("Simulazione senza salvataggio. Le commesse indicano i percorsi di abilitazione; i poteri amministrativi sono separati. Prima della conferma il server ricontrolla i permessi correnti."));
    im_reply(r,200,out);
    goto done;
failure:
    im_rollback(db);
    im_error(r,500,"Anteprima non disponibile: nessuna modifica salvata.");
done:
    if (f) json_object_put(f);
}

static bool ig_item(const char *path,const char *suffix,DbInt *id) {
    const char *end=strchr(path,'/');
    if (!end || strcmp(end,suffix)) return false;
    size_t n=(size_t)(end-path);
    if (!n || n>19) return false;
    char number[24];
    memcpy(number,path,n);
    number[n]=0;
    struct json_object *f=json_object_new_object();
    json_object_object_add(f,"id",json_object_new_string(number));
    bool okay=im_integer(f,"id",1,INT64_MAX,id);
    json_object_put(f);
    return okay;
}
bool improvements_governance_handle(Db *db,const char *method,const char *target,const char *query,const char *body,DbInt actor,WorkspaceResponse *r) {
    const char *prefix="/api/improvements/governance/";
    if (strncmp(target,prefix,strlen(prefix))) return false;
    const char *path=target+strlen(prefix);
    DbInt id=0;
    if (!query) query="";
    if (!body) body="";
    if (*query) {im_error(r,400,"Non sono consentiti parametri di query.");
    return true;
    }
    if (!strcmp(path,"status") && !strcmp(method,"GET")) {
        AccessPolicy p;
        if (ig_policy(db,actor,&p,r)!=1) return true;
        struct json_object *o=json_object_new_object();
        json_object_object_add(o,"ok",json_object_new_boolean(true));
        json_object_object_add(o,"admin",json_object_new_boolean(p.admin));
        json_object_object_add(o,"manager",json_object_new_boolean(p.manager));
        json_object_object_add(o,"bootstrap_single_admin",json_object_new_boolean(p.admin && ig_admin_count(db)==1));
        json_object_object_add(o,"notice",json_object_new_string("Solo l'amministratore gestisce i profili sensibili. Con due amministratori serve una seconda approvazione distinta; con un solo amministratore attivo opera l'eccezione bootstrap tracciata nello storico. Le deleghe non estendono la visibilità delle ore."));
        im_reply(r,200,o);
    } else if (!strcmp(path,"admins") && !strcmp(method,"GET")) ig_list(db,actor,0,r);
    else if (!strcmp(path,"admins") && !strcmp(method,"POST")) ig_admin_write(db,actor,body,r);
    else if (!strcmp(path,"delegations") && !strcmp(method,"GET")) ig_list(db,actor,1,r);
    else if (!strcmp(path,"delegations") && !strcmp(method,"POST")) ig_delegation_write(db,actor,body,r);
    else if (!strncmp(path,"delegations/",12) && ig_item(path+12,"/revoke",&id) && !strcmp(method,"POST")) ig_delegation_revoke(db,actor,id,body,r);
    else if (!strcmp(path,"profile-requests") && !strcmp(method,"GET")) ig_list(db,actor,2,r);
    else if (!strcmp(path,"profile-requests") && !strcmp(method,"POST")) {
        struct json_object *f=im_fields(body);
        const char *const names[]={"user_id","groups","reason","revision",NULL};
        DbInt user=0;
        if (!ig_fields_allowed(f,names) || !im_integer(f,"user_id",1,INT64_MAX,&user) || !ig_reason(im_string(f,"reason"))) im_error(r,400,"Inserisci persona, profili e motivo della richiesta.");
        else {
            /* Re-encode only the allowed profile fields; never forward identity. */
            const char *groups=im_string(f,"groups"),*reason=im_string(f,"reason"),*revision=im_string(f,"revision");
            size_t capacity=(strlen(groups)+strlen(reason)+strlen(revision))*3+60;
            char *encoded=calloc(1,capacity);
            if (!encoded) im_error(r,500,"Memoria insufficiente.");
            else {size_t used=0;
            const char *keys[]={"groups","reason","revision"},*values[]={groups,reason,revision};
                for (int i=0;i<3;i++) {if (i==2 && !*revision) continue;
                int n=snprintf(encoded+used,capacity-used,"%s%s=",used?"&":"",keys[i]);
                used+=(size_t)n;
                    for (const unsigned char *v=(const unsigned char *)values[i];*v;v++) {n=snprintf(encoded+used,capacity-used,"%%%02X",*v);
                    used+=(size_t)n;
                    }}
                improvements_governance_profiles(db,actor,user,encoded,r);
                free(encoded);
            }
        }
        if (f) json_object_put(f);
    } else if (!strncmp(path,"profile-requests/",17) && ig_item(path+17,"/decision",&id) && !strcmp(method,"POST")) ig_profile_decision(db,actor,id,body,r);
    else if (!strcmp(path,"group-impact") && !strcmp(method,"POST")) ig_group_impact(db,actor,body,r);
    else im_error(r,405,"Percorso o metodo di governance non consentito.");
    return true;
}
