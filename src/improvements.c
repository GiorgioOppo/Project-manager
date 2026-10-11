#include "improvements.h"
#include "storage.h"
#include "improvements_governance.h"
#include "improvements_operations.h"
#include "improvements_integrations.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Db *extension_database;
static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    return -1;
}
static bool valid_utf8(const unsigned char *text) {
    while (*text) {
        unsigned char first=*text++;
        if (first<0x80) continue;
        unsigned count, value, minimum;
        if (first>=0xc2 && first<=0xdf) { count=1; value=first&0x1f; minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { count=2; value=first&0x0f; minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { count=3; value=first&0x07; minimum=0x10000; }
        else return false;
        for (unsigned i=0;i<count;i++) {
            unsigned char next=*text++;
            if ((next&0xc0)!=0x80) return false;
            value=(value<<6)|(next&0x3f);
        }
        if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}
static char *decode(const char *start, size_t length) {
    char *out = malloc(length+1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t i=0;i<length;i++) {
        unsigned char c = (unsigned char)start[i];
        if (c == '%') {
            if (i+2 >= length || hex_digit(start[i+1]) < 0 || hex_digit(start[i+2]) < 0) { free(out); return NULL; }
            c = (unsigned char)(hex_digit(start[i+1])*16+hex_digit(start[i+2])); i+=2;
        } else if (c == '+') c = ' ';
        if (!c) { free(out); return NULL; }
        out[used++] = (char)c;
    }
    out[used] = 0;
    if (!valid_utf8((const unsigned char *)out)) { free(out); return NULL; }
    return out;
}
struct json_object *im_fields(const char *encoded) {
    struct json_object *fields = json_object_new_object();
    if (!fields) return NULL;
    const char *at = encoded ? encoded : "";
    if (strlen(at) > 1024U*1024U) goto failure;
    unsigned count = 0;
    while (*at) {
        const char *end = strchr(at,'&'); if (!end) end = at+strlen(at);
        const char *equal = memchr(at,'=',(size_t)(end-at));
        if (!equal || equal == at || ++count > 64) goto failure;
        char *key = decode(at,(size_t)(equal-at));
        char *value = decode(equal+1,(size_t)(end-equal-1));
        struct json_object *existing = NULL;
        if (!key || !value || strlen(key)>64 || json_object_object_get_ex(fields,key,&existing)) { free(key); free(value); goto failure; }
        struct json_object *text = json_object_new_string(value);
        free(value);
        if (!text) { free(key); goto failure; }
        json_object_object_add(fields,key,text); free(key);
        if (!*end) break;
        at = end+1; if (!*at) goto failure;
    }
    return fields;
failure:
    json_object_put(fields); return NULL;
}
const char *im_string(struct json_object *fields, const char *key) {
    struct json_object *value = NULL;
    return fields && json_object_object_get_ex(fields,key,&value) && json_object_is_type(value,json_type_string) ? json_object_get_string(value) : "";
}
bool im_integer(struct json_object *fields, const char *key, DbInt minimum, DbInt maximum, DbInt *value) {
    const char *text = im_string(fields,key); char *end = NULL;
    const char *digits=text;
    if (*digits=='-' && minimum<0) digits++;
    if (!*digits) return false;
    for (;*digits;digits++) if (*digits<'0' || *digits>'9') return false;
    errno=0; long long parsed=strtoll(text,&end,10);
    if (errno || *end || parsed<minimum || parsed>maximum) return false;
    *value=(DbInt)parsed; return true;
}
bool im_date(const char *value) {
    if (!value || strlen(value)!=10 || value[4]!='-' || value[7]!='-') return false;
    for (int i=0;i<10;i++) if (i!=4 && i!=7 && (value[i]<'0' || value[i]>'9')) return false;
    int y=atoi(value),m=atoi(value+5),d=atoi(value+8);
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return y>=1900 && y<=9999 && m>=1 && m<=12 && d>=1 && d<=days[m-1]+(m==2 && (y%400==0 || (y%4==0 && y%100!=0)));
}
bool im_today(char value[11]) {
    time_t now=time(NULL); struct tm current;
    return now!=(time_t)-1 && localtime_r(&now,&current) && strftime(value,11,"%Y-%m-%d",&current)==10;
}
void im_reply(WorkspaceResponse *response, int status, struct json_object *value) {
    const char *text=value ? json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN) : NULL;
    response->body=text ? strdup(text) : NULL;
    response->status=response->body ? status : 500;
    if (value) json_object_put(value);
}
void im_error(WorkspaceResponse *response, int status, const char *message) {
    struct json_object *value=json_object_new_object();
    json_object_object_add(value,"ok",json_object_new_boolean(false));
    json_object_object_add(value,"error",json_object_new_string(message));
    im_reply(response,status,value);
}
bool im_exec(Db *db, const char *sql) { return db_exec(db,sql,NULL,NULL,NULL)==DB_OK; }
bool im_begin(Db *db, WorkspaceResponse *response) {
    if (im_exec(db,"BEGIN IMMEDIATE")) return true;
    im_error(response,503,"Impossibile avviare il salvataggio. Riprova."); return false;
}
void im_rollback(Db *db) { (void)im_exec(db,"ROLLBACK"); }
bool im_audit_commit(Db *db, DbInt actor, const char *action, const char *entity,
                     DbInt id, const char *before, const char *after, WorkspaceResponse *response) {
    if (storage_audit(db,actor,action,entity,id,before,after) && im_exec(db,"COMMIT")) return true;
    im_rollback(db); im_error(response,500,"Salvataggio non riuscito: nessuna modifica confermata."); return false;
}
bool im_column(Db *db, const char *table, const char *name, const char *sqlite_ddl, const char *mysql_ddl) {
    DbStmt *statement=NULL; bool exists=false,okay=false;
#ifdef PUNTO_MYSQL
    (void)sqlite_ddl;
    if (db_prepare_v2(db,"SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=? AND COLUMN_NAME=?",-1,&statement,NULL)==DB_OK && db_bind_text(statement,1,table,-1,DB_TRANSIENT)==DB_OK && db_bind_text(statement,2,name,-1,DB_TRANSIENT)==DB_OK && db_step(statement)==DB_ROW) { okay=true; exists=db_column_int(statement,0)==1; }
#else
    (void)mysql_ddl;
    char query[128]; int n=snprintf(query,sizeof(query),"PRAGMA table_info(%s)",table);
    if (n>0 && (size_t)n<sizeof(query) && db_prepare_v2(db,query,-1,&statement,NULL)==DB_OK) {
        int step; while ((step=db_step(statement))==DB_ROW) if (!strcmp((const char *)db_column_text(statement,1),name)) exists=true;
        okay=step==DB_DONE;
    }
#endif
    db_finalize(statement);
#ifdef PUNTO_MYSQL
    return okay && (exists || im_exec(db,mysql_ddl));
#else
    return okay && (exists || im_exec(db,sqlite_ddl));
#endif
}
bool improvements_schema(Db *db) {
    return improvements_governance_schema(db) && improvements_operations_schema(db) && improvements_integrations_schema(db);
}
bool improvements_open(const char *path) {
    improvements_close();
    bool okay = db_open_v2(path,&extension_database,DB_OPEN_READWRITE|DB_OPEN_FULLMUTEX,NULL)==DB_OK && db_busy_timeout(extension_database,5000)==DB_OK &&
#ifndef PUNTO_MYSQL
        im_exec(extension_database,"PRAGMA foreign_keys=ON") &&
#endif
        true;
    if (okay) improvements_integrations_attach(extension_database);
    return okay;
}
void improvements_close(void) { improvements_integrations_attach(NULL); if (extension_database) (void)db_close(extension_database); extension_database=NULL; }
bool improvements_handle(const char *method, const char *target, const char *query,
                         const char *body, DbInt actor, WorkspaceResponse *response) {
    if (strncmp(target,"/api/improvements/",18)) return false;
    AccessPolicy policy;
    int resolved=extension_database ? access_policy_resolve(extension_database,actor,NULL,&policy) : -1;
    if (resolved<0) { im_error(response,503,"Servizio temporaneamente non disponibile."); return true; }
    if (actor<=0 || resolved!=1) { im_error(response,401,"Accedi per continuare."); return true; }
    if (improvements_governance_handle(extension_database,method,target,query,body,actor,response) ||
        improvements_operations_handle(extension_database,method,target,query,body,actor,response) ||
        improvements_integrations_handle(extension_database,method,target,query,body,actor,response)) return true;
    im_error(response,404,"Percorso non trovato."); return true;
}
