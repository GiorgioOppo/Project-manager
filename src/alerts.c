#include "alerts.h"
#include "storage.h"
#include "mail_client.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORM_LIMIT 32768U
#define JSON_LIMIT (4U * 1024U * 1024U)
#define RECIPIENT_LIMIT 50
#define RULE_LIMIT 100

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} Json;

typedef struct {
    const char *name;
    char *value;
} Field;

static Db *database;
static DbInt last_delivery_id;

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static bool append_bytes(Json *json, const char *text, size_t length) {
    if (json->failed) return false;
    if (length > JSON_LIMIT || json->length > JSON_LIMIT - length) {
        json->failed = true;
        return false;
    }
    size_t needed = json->length + length + 1;
    if (needed > json->capacity) {
        size_t capacity = json->capacity ? json->capacity : 256;
        while (capacity < needed) capacity *= 2;
        char *data = realloc(json->data, capacity);
        if (!data) {
            json->failed = true;
            return false;
        }
        json->data = data;
        json->capacity = capacity;
    }
    memcpy(json->data + json->length, text, length);
    json->length += length;
    json->data[json->length] = '\0';
    return true;
}

static void append(Json *json, const char *text) {
    (void)append_bytes(json, text, strlen(text));
}

static void append_integer(Json *json, DbInt value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length < 0 || (size_t)length >= sizeof(text)) json->failed = true;
    else (void)append_bytes(json, text, (size_t)length);
}

static void append_string(Json *json, const char *text) {
    append(json, "\"");
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p; p++) {
        switch (*p) {
            case '"': append(json, "\\\""); break;
            case '\\': append(json, "\\\\"); break;
            case '\b': append(json, "\\b"); break;
            case '\f': append(json, "\\f"); break;
            case '\n': append(json, "\\n"); break;
            case '\r': append(json, "\\r"); break;
            case '\t': append(json, "\\t"); break;
            case '<': append(json, "\\u003c"); break;
            case '>': append(json, "\\u003e"); break;
            case '&': append(json, "\\u0026"); break;
            default:
                if (*p < 32) {
                    char escaped[7];
                    (void)snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    append(json, escaped);
                } else if (*p == 0xe2 && p[1] == 0x80 &&
                           (p[2] == 0xa8 || p[2] == 0xa9)) {
                    append(json, p[2] == 0xa8 ? "\\u2028" : "\\u2029");
                    p += 2;
                } else {
                    (void)append_bytes(json, (const char *)p, 1);
                }
                break;
        }
    }
    append(json, "\"");
}

static void error(WorkspaceResponse *response, int status, const char *message) {
    Json json = {0};
    append(&json, "{\"ok\":false,\"error\":");
    append_string(&json, message);
    append(&json, "}");
    response->status = json.failed ? 500 : status;
    if (json.failed) {
        free(json.data);
        response->body = duplicate("{\"ok\":false,\"error\":\"Errore del server. Riprova.\"}");
    } else response->body = json.data;
}

static void server_error(WorkspaceResponse *response) {
    error(response, 500, "Errore del server. Riprova.");
}

static void finish_json(WorkspaceResponse *response, int status, Json *json) {
    if (json->failed) {
        free(json->data);
        server_error(response);
    } else {
        response->status = status;
        response->body = json->data;
    }
}

static bool prepare(DbStmt **statement, const char *sql) {
    return db_prepare_v2(database, sql, -1, statement, NULL) == DB_OK;
}

static bool bind_text(DbStmt *statement, int index, const char *text) {
    return db_bind_text(statement, index, text, -1, DB_TRANSIENT) == DB_OK;
}

static bool bind_id(DbStmt *statement, int index, DbInt value) {
    return db_bind_int64(statement, index, value) == DB_OK;
}

static const char *column_text(DbStmt *statement, int column) {
    const unsigned char *text = db_column_text(statement, column);
    return text ? (const char *)text : "";
}

static bool begin_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL) == DB_OK)
        return true;
    server_error(response);
    return false;
}

static void rollback(void) {
    (void)db_exec(database, "ROLLBACK", NULL, NULL, NULL);
}

static bool commit_transaction(WorkspaceResponse *response) {
    if (db_exec(database, "COMMIT", NULL, NULL, NULL) == DB_OK) return true;
    rollback();
    server_error(response);
    return false;
}

/* Check canonical UTF-8, count characters, and reject forbidden control values. */
static bool utf8_length(const char *text, size_t *length) {
    const unsigned char *p = (const unsigned char *)text;
    size_t count = 0;
    while (*p) {
        uint32_t codepoint;
        size_t continuation;
        unsigned char first = *p++;
        if (first < 0x80) { codepoint = first; continuation = 0; }
        else if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1fU;
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0fU;
            continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07U;
            continuation = 3;
        } else return false;
        for (size_t i = 0; i < continuation; i++) {
            if ((*p & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (*p++ & 0x3fU);
        }
        if ((continuation == 1 && codepoint < 0x80) ||
            (continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            (codepoint < 32 && codepoint != '\r' && codepoint != '\n' && codepoint != '\t') ||
            (codepoint >= 0x7f && codepoint <= 0x9f)) return false;
        count++;
    }
    *length = count;
    return true;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode(const char *input, size_t length, bool *out_of_memory) {
    char *output = malloc(length + 1);
    if (!output) { *out_of_memory = true; return NULL; }
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (length - i < 3) { free(output); return NULL; }
            int high = hex_digit((unsigned char)input[i + 1]);
            int low = hex_digit((unsigned char)input[i + 2]);
            if (high < 0 || low < 0) { free(output); return NULL; }
            c = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (c == '+') c = ' ';
        if (!c) { free(output); return NULL; }
        output[used++] = (char)c;
    }
    output[used] = '\0';
    size_t characters;
    if (!utf8_length(output, &characters)) { free(output); return NULL; }
    return output;
}

static void free_fields(Field *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(fields[i].value);
        fields[i].value = NULL;
    }
}

static bool parse_fields(const char *input, Field *fields, size_t count,
                         WorkspaceResponse *response) {
    bool out_of_memory = false;
    if (!input || strlen(input) > FORM_LIMIT) goto invalid;
    const char *p = input;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *equal = memchr(p, '=', (size_t)(end - p));
        if (!equal || equal == p) goto invalid;
        char *name = decode(p, (size_t)(equal - p), &out_of_memory);
        if (!name) goto invalid;
        size_t field = 0;
        while (field < count && strcmp(name, fields[field].name) != 0) field++;
        free(name);
        if (field == count || fields[field].value) goto invalid;
        fields[field].value = decode(equal + 1, (size_t)(end - equal - 1), &out_of_memory);
        if (!fields[field].value) goto invalid;
        if (!*end) break;
        p = end + 1;
        if (!*p) goto invalid;
    }
    return true;
invalid:
    free_fields(fields, count);
    if (out_of_memory) server_error(response);
    else error(response, 400, "Dati non validi: controlla i campi inviati.");
    return false;
}

static char *field_value(Field *field) {
    return field->value ? field->value : "";
}

static void trim(char *text) {
    if (!text) return;
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    size_t length = strlen(start);
    while (length && (start[length - 1] == ' ' || start[length - 1] == '\t' ||
                      start[length - 1] == '\r' || start[length - 1] == '\n')) length--;
    memmove(text, start, length);
    text[length] = '\0';
}

static bool valid_text(const char *text, size_t maximum, bool required,
                       bool multiline) {
    size_t length;
    if (!utf8_length(text, &length) || length > maximum || (required && !length))
        return false;
    return multiline || !strpbrk(text, "\r\n\t");
}

static bool parse_positive(const char *text, DbInt *value) {
    if (!text || !*text) return false;
    DbInt result = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        int digit = *p - '0';
        if (result > (INT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    if (result <= 0) return false;
    *value = result;
    return true;
}

static bool valid_date(const char *date) {
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (size_t i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
               (date[2] - '0') * 10 + date[3] - '0';
    int month = (date[5] - '0') * 10 + date[6] - '0';
    int day = (date[8] - '0') * 10 + date[9] - '0';
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    int maximum = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) maximum++;
    return day <= maximum;
}


static bool clock_text(time_t now, char day[11], char utc[21]) {
    struct tm local, universal;
    return now != (time_t)-1 && localtime_r(&now,&local) && gmtime_r(&now,&universal) &&
        strftime(day,11,"%Y-%m-%d",&local) == 10 &&
        strftime(utc,21,"%Y-%m-%dT%H:%M:%SZ",&universal) == 20;
}

static bool valid_time(const char *value) {
    return value && strlen(value)==5 && value[2]==':' &&
        value[0]>='0' && value[0]<='2' && value[1]>='0' && value[1]<='9' &&
        value[3]>='0' && value[3]<='5' && value[4]>='0' && value[4]<='9' &&
        (value[0]!='2' || value[1]<='3');
}

static bool date_tm(const char *date, struct tm *out) {
    if (!valid_date(date)) return false;
    memset(out,0,sizeof(*out));
    out->tm_year=(date[0]-'0')*1000+(date[1]-'0')*100+(date[2]-'0')*10+date[3]-'0'-1900;
    out->tm_mon=(date[5]-'0')*10+date[6]-'0'-1;
    out->tm_mday=(date[8]-'0')*10+date[9]-'0'; out->tm_isdst=-1;
    return true;
}

bool alerts_workday_due(const char *date, time_t now) {
    struct tm local;
    if (now==(time_t)-1 || !date_tm(date,&local)) return false;
    time_t midnight=mktime(&local);
    if (midnight==(time_t)-1 || local.tm_wday==0 || local.tm_wday==6) return false;
    local.tm_mday++; local.tm_isdst=-1;
    time_t end=mktime(&local);
    return end!=(time_t)-1 && difftime(now,end)>=48.0*60.0*60.0;
}

static time_t today_run(time_t now, const char *run_time) {
    struct tm local;
    if (!valid_time(run_time) || !localtime_r(&now,&local)) return (time_t)-1;
    local.tm_hour=(run_time[0]-'0')*10+run_time[1]-'0';
    local.tm_min=(run_time[3]-'0')*10+run_time[4]-'0'; local.tm_sec=0; local.tm_isdst=-1;
    return mktime(&local);
}

bool alerts_next_run(time_t now, const char *run_time, bool today_already_run,
                     time_t *out) {
    struct tm local;
    if (!out || !valid_time(run_time) || !localtime_r(&now,&local)) return false;
    time_t candidate=today_run(now,run_time);
    if (candidate==(time_t)-1) return false;
    if (today_already_run || candidate<=now) {
        local.tm_mday++; local.tm_hour=(run_time[0]-'0')*10+run_time[1]-'0';
        local.tm_min=(run_time[3]-'0')*10+run_time[4]-'0'; local.tm_sec=0; local.tm_isdst=-1;
        candidate=mktime(&local);
    }
    if (candidate==(time_t)-1) return false;
    *out=candidate; return true;
}

static bool next_day(char day[11]) {
    struct tm local;
    if (!date_tm(day,&local)) return false;
    local.tm_mday++; local.tm_hour=12; local.tm_isdst=-1;
    time_t next=mktime(&local);
    return next!=(time_t)-1 && strftime(day,11,"%Y-%m-%d",&local)==10;
}

static void next_run_json(Json *json, time_t now, const char *run_time,
                          const char *last_run_on, bool enabled) {
    char day[11], utc[21], value[32]; time_t next; struct tm local;
    if (!enabled || !clock_text(now,day,utc) ||
        !alerts_next_run(now,run_time,strcmp(day,last_run_on)==0,&next) ||
        !localtime_r(&next,&local) || !strftime(value,sizeof(value),"%Y-%m-%dT%H:%M:%S%z",&local)) {
        append(json,"null"); return;
    }
    append_string(json,value);
}

bool alerts_open(const char *db_path) {
    alerts_close();
    if (setenv("TZ","Europe/Rome",1)!=0) return false;
    tzset();
    if (!db_path || !*db_path || db_open_v2(db_path,&database,
        DB_OPEN_READWRITE|DB_OPEN_FULLMUTEX,NULL)!=DB_OK || db_busy_timeout(database,5000)!=DB_OK
#ifndef PUNTO_MYSQL
        || db_exec(database,"PRAGMA foreign_keys=ON",NULL,NULL,NULL)!=DB_OK
#endif
    ) { alerts_close(); return false; }
    if (!mail_client_open()) {
        fprintf(stderr,"Configurazione add-on email non valida. Controlla URL interno, CA e file del token.\n");
        alerts_close();
        return false;
    }
    return true;
}

void alerts_close(void) {
    mail_client_close();
    if (database) (void)db_close(database);
    database=NULL;
    last_delivery_id=0;
}

typedef struct {
    DbInt users[RECIPIENT_LIMIT]; size_t user_count;
    char emails[RECIPIENT_LIMIT][255]; size_t email_count;
} Recipients;

static bool valid_email(char *email) {
    size_t length=strlen(email);
    if (length<3 || length>254) return false;
    char *at=strchr(email,'@');
    if (!at || at==email || at-email>64 || !at[1] || strchr(at+1,'@') || !strchr(at+1,'.')) return false;
    for (char *p=email;*p;p++) {
        unsigned char c=(unsigned char)*p;
        if (c<=32 || c>=127 || (p<at && !isalnum(c) && !strchr(".!#$%&*+-/=?^_`{|}~",c)) ||
            (p>at && !isalnum(c) && c!='.' && c!='-')) return false;
        *p=(char)tolower(c);
    }
    if (email[0]=='.' || at[-1]=='.' || at[1]=='.' || email[length-1]=='.' || strstr(email,"..")) return false;
    const char *label=at+1;
    for (const char *p=label;;p++) {
        if (!*p || *p=='.') {
            if (p==label || p-label>63 || *label=='-' || p[-1]=='-') return false;
            if (!*p) break;
            label=p+1;
        }
    }
    return true;
}

static bool parse_recipients(char *users, char *emails, Recipients *out) {
    memset(out,0,sizeof(*out));
    if (users && *users) {
        char *p=users;
        while (p) {
            char *end=strchr(p,','); if (end) *end='\0'; trim(p);
            DbInt id;
            if (!parse_positive(p,&id) || out->user_count>=RECIPIENT_LIMIT) return false;
            for (size_t i=0;i<out->user_count;i++) if (out->users[i]==id) return false;
            out->users[out->user_count++]=id;
            p=end ? end+1 : NULL;
        }
    }
    if (emails && *emails) {
        char *p=emails;
        while (p) {
            char *end=strpbrk(p,",\r\n"); if (end) *end='\0'; trim(p);
            if (*p) {
                if (out->email_count>=RECIPIENT_LIMIT || !valid_email(p)) return false;
                for (size_t i=0;i<out->email_count;i++) if (strcmp(out->emails[i],p)==0) return false;
                (void)snprintf(out->emails[out->email_count++],255,"%s",p);
            }
            p=end ? end+1 : NULL;
        }
    }
    return out->user_count+out->email_count>0;
}

static bool rule_json(DbStmt *statement, Json *json, time_t now, bool private_details) {
    DbInt id=db_column_int64(statement,0);
    append(json,"{\"id\":"); append_integer(json,id);
    static const char *const keys[]={"name","type","run_time"};
    for (int i=0;i<3;i++) { append(json,",\""); append(json,keys[i]); append(json,"\":"); append_string(json,column_text(statement,i+1)); }
    append(json,",\"enabled\":"); append(json,db_column_int(statement,4) ? "true" : "false");
    append(json,",\"created_at\":"); append_string(json,column_text(statement,5));
    append(json,",\"updated_at\":"); append_string(json,column_text(statement,6));
    append(json,",\"last_run_at\":");
    if (db_column_type(statement,7)==DB_NULL) append(json,"null"); else append_string(json,column_text(statement,7));
    append(json,",\"last_run_on\":");
    if (db_column_type(statement,8)==DB_NULL) append(json,"null"); else append_string(json,column_text(statement,8));
    append(json,",\"next_run_at\":"); next_run_json(json,now,column_text(statement,3),column_text(statement,8),db_column_int(statement,4)!=0);
    append(json,",\"recipient_user_ids\":[");
    DbStmt *rows=NULL;
    if (!prepare(&rows,"SELECT user_id FROM alert_rule_users WHERE rule_id=? ORDER BY user_id") || !bind_id(rows,1,id)) { db_finalize(rows); return false; }
    int result; size_t count=0;
    while ((result=db_step(rows))==DB_ROW) { if (count++) append(json,","); append_integer(json,db_column_int64(rows,0)); }
    db_finalize(rows); rows=NULL; if (result!=DB_DONE) return false;
    append(json,private_details ? "],\"recipient_emails\":[" : "]");
    if (!prepare(&rows,"SELECT email FROM alert_rule_emails WHERE rule_id=? ORDER BY email") || !bind_id(rows,1,id)) { db_finalize(rows); return false; }
    count=0;
    while ((result=db_step(rows))==DB_ROW) {
        if (private_details) { if (count) append(json,","); append_string(json,column_text(rows,0)); }
        count++;
    }
    db_finalize(rows); if (result!=DB_DONE) return false;
    if (private_details) append(json,"]}");
    else { append(json,",\"recipient_email_count\":"); append_integer(json,(DbInt)count); append(json,"}"); }
    return !json->failed;
}

static const char rule_select[]="SELECT id,name,type,run_time,enabled,created_at,updated_at,last_run_at,last_run_on FROM alert_rules ";

static int rule_snapshot(DbInt id, DbInt owner, Json *json) {
    DbStmt *statement=NULL; char sql[256];
    (void)snprintf(sql,sizeof(sql),"%s WHERE id=? AND owner_id=?",rule_select);
    if (!prepare(&statement,sql) || !bind_id(statement,1,id) || !bind_id(statement,2,owner)) { db_finalize(statement); return -1; }
    int result=db_step(statement);
    bool okay=result!=DB_ROW || rule_json(statement,json,time(NULL),false);
    db_finalize(statement);
    return !okay ? -1 : result==DB_ROW ? 1 : result==DB_DONE ? 0 : -1;
}

static void list_rules(DbInt owner, WorkspaceResponse *response) {
    Json json={0}; DbStmt *statement=NULL; char sql[256];
    (void)snprintf(sql,sizeof(sql),"%s WHERE owner_id=? ORDER BY id DESC LIMIT 101",rule_select);
    if (!prepare(&statement,sql) || !bind_id(statement,1,owner)) goto failure;
    append(&json,"{\"rules\":["); int result; size_t count=0;
    while ((result=db_step(statement))==DB_ROW) {
        if (++count>RULE_LIMIT) goto failure;
        if (count>1) append(&json,",");
        if (!rule_json(statement,&json,time(NULL),true)) goto failure;
    }
    db_finalize(statement); statement=NULL; if (result!=DB_DONE) goto failure;
    append(&json,"],\"users\":[");
    if (!prepare(&statement,"SELECT id,name,email,role FROM users WHERE active=1 ORDER BY name,id")) goto failure;
    count=0;
    while ((result=db_step(statement))==DB_ROW) {
        if (count++) append(&json,",");
        append(&json,"{\"id\":"); append_integer(&json,db_column_int64(statement,0));
        append(&json,",\"name\":"); append_string(&json,column_text(statement,1));
        append(&json,",\"email\":"); append_string(&json,column_text(statement,2));
        append(&json,",\"role\":"); append_string(&json,column_text(statement,3)); append(&json,"}");
    }
    db_finalize(statement); statement=NULL; if (result!=DB_DONE) goto failure;
    append(&json,"],\"smtp_configured\":");
    append(&json,mail_client_smtp_configured() ? "true" : "false");
    append(&json,",\"timezone\":\"Europe/Rome\",\"daily_target_minutes\":480,\"grace_hours\":48}");
    finish_json(response,200,&json); return;
failure:
    db_finalize(statement); free(json.data); server_error(response);
}

static void write_rule(const char *body, DbInt owner, DbInt id, WorkspaceResponse *response) {
    Field fields[]={{"name",NULL},{"type",NULL},{"run_time",NULL},{"enabled",NULL},
        {"recipient_user_ids",NULL},{"recipient_emails",NULL}};
    const size_t field_count=sizeof(fields)/sizeof(fields[0]);
    if (!parse_fields(body,fields,field_count,response)) return;
    trim(fields[0].value);
    const char *name=field_value(&fields[0]), *type=field_value(&fields[1]), *run_time=field_value(&fields[2]);
    Recipients recipients;
    if (!valid_text(name,120,true,false) ||
        (strcmp(type,"invalid_job")!=0 && strcmp(type,"missing_hours")!=0) || !valid_time(run_time) ||
        (strcmp(field_value(&fields[3]),"0")!=0 && strcmp(field_value(&fields[3]),"1")!=0) ||
        !parse_recipients(fields[4].value,fields[5].value,&recipients)) {
        error(response,400,"Inserisci nome (max 120 caratteri), tipo, orario HH:MM, stato 0/1 e almeno un destinatario valido (massimo 50 utenti e 50 email, senza duplicati).");
        free_fields(fields,field_count); return;
    }
    char day[11],utc[21];
    if (!clock_text(time(NULL),day,utc)) { free_fields(fields,field_count); server_error(response); return; }
    if (!begin_transaction(response)) { free_fields(fields,field_count); return; }
    Json before={0},after={0}; DbStmt *statement=NULL; bool reset=false,schedule_changed=false;
    if (id) {
        int found=rule_snapshot(id,owner,&before);
        if (found<0) goto failure;
        if (!found) { rollback(); error(response,404,"Regola non trovata."); goto done; }
        if (!prepare(&statement,"SELECT type,run_time FROM alert_rules WHERE id=? AND owner_id=?") ||
            !bind_id(statement,1,id) || !bind_id(statement,2,owner) || db_step(statement)!=DB_ROW) goto failure;
        reset=strcmp(column_text(statement,0),type)!=0;
        schedule_changed=strcmp(column_text(statement,1),run_time)!=0;
        db_finalize(statement); statement=NULL;
    } else {
        if (!prepare(&statement,"SELECT COUNT(*) FROM alert_rules WHERE owner_id=?") ||
            !bind_id(statement,1,owner) || db_step(statement)!=DB_ROW) goto failure;
        bool full=db_column_int64(statement,0)>=RULE_LIMIT;
        db_finalize(statement); statement=NULL;
        if (full) { rollback(); error(response,409,"Hai raggiunto il limite di 100 regole personali."); goto done; }
    }
    for (size_t i=0;i<recipients.user_count;i++) {
        if (!prepare(&statement,"SELECT 1 FROM users WHERE id=? AND active=1") ||
            !bind_id(statement,1,recipients.users[i])) goto failure;
        int result=db_step(statement); db_finalize(statement); statement=NULL;
        if (result!=DB_ROW && result!=DB_DONE) goto failure;
        if (result!=DB_ROW) { rollback(); error(response,400,"Seleziona soltanto utenti esistenti e attivi."); goto done; }
    }
    if (!prepare(&statement,id ?
        "UPDATE alert_rules SET name=?,type=?,run_time=?,enabled=?,updated_at=? WHERE id=? AND owner_id=?" :
        "INSERT INTO alert_rules(name,type,run_time,enabled,updated_at,owner_id,created_at,last_event_id) "
        "VALUES(?,?,?,?,?,?,?,(SELECT COALESCE(MAX(id),0) FROM alert_events))") ||
        !bind_text(statement,1,name) || !bind_text(statement,2,type) || !bind_text(statement,3,run_time) ||
        !bind_id(statement,4,strcmp(fields[3].value,"1")==0) || !bind_text(statement,5,utc) ||
        !bind_id(statement,6,id ? id : owner) ||
        (id ? !bind_id(statement,7,owner) : !bind_text(statement,7,utc)) || db_step(statement)!=DB_DONE) goto failure;
    db_finalize(statement); statement=NULL;
    DbInt saved=id ? id : db_last_insert_rowid(database);
    if (id) {
        if (!prepare(&statement,"DELETE FROM alert_rule_users WHERE rule_id=?") ||
            !bind_id(statement,1,id) || db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
        if (!prepare(&statement,"DELETE FROM alert_rule_emails WHERE rule_id=?") ||
            !bind_id(statement,1,id) || db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
    }
    for (size_t i=0;i<recipients.user_count;i++) {
        if (!prepare(&statement,"INSERT INTO alert_rule_users(rule_id,user_id) VALUES(?,?)") ||
            !bind_id(statement,1,saved) || !bind_id(statement,2,recipients.users[i]) ||
            db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
    }
    for (size_t i=0;i<recipients.email_count;i++) {
        if (!prepare(&statement,"INSERT INTO alert_rule_emails(rule_id,email) VALUES(?,?)") ||
            !bind_id(statement,1,saved) || !bind_text(statement,2,recipients.emails[i]) ||
            db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
    }
    if (reset) {
        if (!prepare(&statement,"DELETE FROM alert_subjects WHERE rule_id=?") ||
            !bind_id(statement,1,saved) || db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
        if (!prepare(&statement,"UPDATE alert_rules SET last_event_id=(SELECT COALESCE(MAX(id),0) FROM alert_events),"
            "last_run_on=NULL,last_run_at=NULL WHERE id=?") || !bind_id(statement,1,saved) || db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
    }
    time_t slot=today_run(time(NULL),run_time);
    if (slot==(time_t)-1) goto failure;
    if ((!id || reset || schedule_changed) && slot<=time(NULL)) {
        if (!prepare(&statement,"UPDATE alert_rules SET last_run_on=? WHERE id=?") ||
            !bind_text(statement,1,day) || !bind_id(statement,2,saved) || db_step(statement)!=DB_DONE) goto failure;
        db_finalize(statement); statement=NULL;
    }
    if (!prepare(&statement,"INSERT INTO alert_subjects(rule_id,user_id,starts_on) "
        "SELECT ?,u.id,? FROM users u WHERE u.active=1 AND NOT EXISTS(SELECT 1 FROM alert_subjects s WHERE s.rule_id=? AND s.user_id=u.id)") ||
        !bind_id(statement,1,saved) || !bind_text(statement,2,day) || !bind_id(statement,3,saved) || db_step(statement)!=DB_DONE) goto failure;
    db_finalize(statement); statement=NULL;
    if (rule_snapshot(saved,owner,&after)!=1 || !storage_audit(database,owner,id ? "alert_rules.update" : "alert_rules.create",
        "alert_rule",saved,before.data,after.data)) goto failure;
    if (commit_transaction(response)) {
        Json json={0}; append(&json,"{\"ok\":true,\"id\":"); append_integer(&json,saved); append(&json,"}");
        finish_json(response,id ? 200 : 201,&json);
    }
    goto done;
failure:
    db_finalize(statement); rollback(); server_error(response);
done:
    free(before.data); free(after.data); free_fields(fields,field_count);
}

void alerts_record_invalid_job(DbInt user_id, DbInt job_id, const char *work_date, DbInt minutes) {
    if (!database || user_id<=0 || job_id<=0 || !valid_date(work_date) || minutes<=0 || minutes>1440) return;
    char day[11],utc[21]; DbStmt *statement=NULL;
    if (!clock_text(time(NULL),day,utc) || db_exec(database,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=DB_OK) return;
    bool okay=prepare(&statement,"INSERT INTO alert_events(user_id,job_id,work_date,minutes,created_at) VALUES(?,?,?,?,?)") &&
        bind_id(statement,1,user_id) && bind_id(statement,2,job_id) && bind_text(statement,3,work_date) &&
        bind_id(statement,4,minutes) && bind_text(statement,5,utc) && db_step(statement)==DB_DONE;
    db_finalize(statement);
    if (!okay || db_exec(database,"COMMIT",NULL,NULL,NULL)!=DB_OK) {
        rollback(); fprintf(stderr,"Impossibile memorizzare un evento di commessa inesistente.\n");
    }
}

typedef struct { DbInt user_id; bool manager; } RecipientUser;
typedef struct {
    RecipientUser users[RECIPIENT_LIMIT]; size_t user_count;
    char emails[RECIPIENT_LIMIT][255]; size_t email_count;
    DbInt notifications,emails_queued;
} Delivery;

static bool load_delivery(DbInt id, Delivery *delivery) {
    memset(delivery,0,sizeof(*delivery)); DbStmt *statement=NULL;
    if (!prepare(&statement,"SELECT u.id,u.role FROM alert_rule_users r JOIN users u ON u.id=r.user_id "
        "WHERE r.rule_id=? AND u.active=1 ORDER BY u.id") || !bind_id(statement,1,id)) { db_finalize(statement); return false; }
    int result;
    while ((result=db_step(statement))==DB_ROW) {
        if (delivery->user_count>=RECIPIENT_LIMIT) { db_finalize(statement); return false; }
        RecipientUser *user=&delivery->users[delivery->user_count++];
        user->user_id=db_column_int64(statement,0); user->manager=strcmp(column_text(statement,1),"manager")==0;
    }
    db_finalize(statement); statement=NULL; if (result!=DB_DONE) return false;
    if (!prepare(&statement,"SELECT email FROM alert_rule_emails WHERE rule_id=? ORDER BY email") || !bind_id(statement,1,id)) { db_finalize(statement); return false; }
    while ((result=db_step(statement))==DB_ROW) {
        if (delivery->email_count>=RECIPIENT_LIMIT) { db_finalize(statement); return false; }
        (void)snprintf(delivery->emails[delivery->email_count++],255,"%s",column_text(statement,0));
    }
    db_finalize(statement); return result==DB_DONE;
}

static bool deliver(DbInt rule_id, Delivery *delivery, DbInt subject, const char *key,
                    const char *title, const char *message, const char *day,
                    DbInt minutes, const char *utc) {
    DbStmt *statement=NULL;
    for (size_t i=0;i<delivery->user_count;i++) {
        RecipientUser *user=&delivery->users[i];
        if (!user->manager && user->user_id!=subject) continue;
        if (!prepare(&statement,"INSERT INTO alert_notifications(rule_id,recipient_id,subject_user_id,event_key,title,message,work_date,recorded_minutes,created_at) "
            "SELECT ?,?,?,?,?,?,?,?,? WHERE NOT EXISTS(SELECT 1 FROM alert_notifications WHERE rule_id=? AND recipient_id=? AND event_key=?)") ||
            !bind_id(statement,1,rule_id) || !bind_id(statement,2,user->user_id) || !bind_id(statement,3,subject) ||
            !bind_text(statement,4,key) || !bind_text(statement,5,title) || !bind_text(statement,6,message) ||
            !bind_text(statement,7,day) || !bind_id(statement,8,minutes) || !bind_text(statement,9,utc) ||
            !bind_id(statement,10,rule_id) || !bind_id(statement,11,user->user_id) || !bind_text(statement,12,key) ||
            db_step(statement)!=DB_DONE) { db_finalize(statement); return false; }
        delivery->notifications+=db_changes(database); db_finalize(statement); statement=NULL;
    }
    for (size_t i=0;i<delivery->email_count;i++) {
        if (!prepare(&statement,"INSERT INTO alert_email_outbox(rule_id,email,subject_user_id,event_key,title,message,created_at,status) "
            "SELECT ?,?,?,?,?,?,?,'pending_configuration' WHERE NOT EXISTS(SELECT 1 FROM alert_email_outbox WHERE rule_id=? AND email=? AND event_key=?)") ||
            !bind_id(statement,1,rule_id) || !bind_text(statement,2,delivery->emails[i]) || !bind_id(statement,3,subject) ||
            !bind_text(statement,4,key) || !bind_text(statement,5,title) || !bind_text(statement,6,message) ||
            !bind_text(statement,7,utc) || !bind_id(statement,8,rule_id) || !bind_text(statement,9,delivery->emails[i]) ||
            !bind_text(statement,10,key) || db_step(statement)!=DB_DONE) { db_finalize(statement); return false; }
        delivery->emails_queued+=db_changes(database); db_finalize(statement); statement=NULL;
    }
    return true;
}

static bool run_invalid(DbInt rule_id, DbInt *cursor, Delivery *delivery, const char *utc) {
    DbStmt *events=NULL;
    if (!prepare(&events,"SELECT e.id,e.user_id,e.job_id,e.work_date,e.minutes,u.name,u.active "
        "FROM alert_events e JOIN users u ON u.id=e.user_id WHERE e.id>? ORDER BY e.id LIMIT 1000") ||
        !bind_id(events,1,*cursor)) { db_finalize(events); return false; }
    int result;
    while ((result=db_step(events))==DB_ROW) {
        *cursor=db_column_int64(events,0);
        if (!db_column_int(events,6)) continue;
        DbInt subject=db_column_int64(events,1),job=db_column_int64(events,2),minutes=db_column_int64(events,4);
        const char *day=column_text(events,3); char key[128],message[900];
        (void)snprintf(key,sizeof(key),"invalid:%lld:%lld:%s",(long long)subject,(long long)job,day);
        (void)snprintf(message,sizeof(message),"%s ha tentato di registrare %lld minuti il %s sulla commessa inesistente #%lld. La registrazione è stata rifiutata.",
            column_text(events,5),(long long)minutes,day,(long long)job);
        if (!deliver(rule_id,delivery,subject,key,"Commessa inesistente: registrazione rifiutata",message,day,minutes,utc)) {
            db_finalize(events); return false;
        }
    }
    db_finalize(events); return result==DB_DONE;
}

static bool run_missing(DbInt rule_id, Delivery *delivery, const char *today,
                        const char *utc, time_t now) {
    DbStmt *statement=NULL;
    if (!prepare(&statement,"INSERT INTO alert_subjects(rule_id,user_id,starts_on) "
        "SELECT ?,u.id,? FROM users u WHERE u.active=1 AND NOT EXISTS(SELECT 1 FROM alert_subjects s WHERE s.rule_id=? AND s.user_id=u.id)") ||
        !bind_id(statement,1,rule_id) || !bind_text(statement,2,today) || !bind_id(statement,3,rule_id) ||
        db_step(statement)!=DB_DONE) { db_finalize(statement); return false; }
    db_finalize(statement); statement=NULL;
    DbStmt *subjects=NULL;
    if (!prepare(&subjects,"SELECT s.user_id,s.starts_on,s.last_checked_on,u.name "
        "FROM alert_subjects s JOIN users u ON u.id=s.user_id WHERE s.rule_id=? AND u.active=1 ORDER BY s.user_id") ||
        !bind_id(subjects,1,rule_id)) { db_finalize(subjects); return false; }
    int result;
    while ((result=db_step(subjects))==DB_ROW) {
        DbInt subject=db_column_int64(subjects,0); char day[11],checked[11]="";
        const char *last=column_text(subjects,2);
        (void)snprintf(day,sizeof(day),"%s",*last ? last : column_text(subjects,1));
        if (!valid_date(day) || (*last && !next_day(day))) { db_finalize(subjects); return false; }
        unsigned scanned=0;
        while (strcmp(day,today)<0 && scanned++<3660) {
            struct tm boundary;
            if (!date_tm(day,&boundary)) { db_finalize(subjects); return false; }
            boundary.tm_mday++; time_t end=mktime(&boundary);
            if (end==(time_t)-1) { db_finalize(subjects); return false; }
            if (difftime(now,end)<48.0*60.0*60.0) break;
            if (alerts_workday_due(day,now)) {
                if (!prepare(&statement,"SELECT COALESCE(SUM(minutes),0) FROM time_entries WHERE user_id=? AND date=?") ||
                    !bind_id(statement,1,subject) || !bind_text(statement,2,day) || db_step(statement)!=DB_ROW) {
                    db_finalize(statement); db_finalize(subjects); return false;
                }
                DbInt minutes=db_column_int64(statement,0); db_finalize(statement); statement=NULL;
                if (minutes<480) {
                    char key[96],message[900];
                    (void)snprintf(key,sizeof(key),"missing:%lld:%s",(long long)subject,day);
                    (void)snprintf(message,sizeof(message),"%s: il %s risultano registrati %lld minuti su 480 previsti. Sono trascorse almeno 48 ore dalla fine della giornata lavorativa.",
                        column_text(subjects,3),day,(long long)minutes);
                    if (!deliver(rule_id,delivery,subject,key,"Ore giornaliere incomplete",message,day,minutes,utc)) { db_finalize(subjects); return false; }
                }
            }
            (void)snprintf(checked,sizeof(checked),"%s",day);
            if (!next_day(day)) { db_finalize(subjects); return false; }
        }
        if (*checked) {
            if (!prepare(&statement,"UPDATE alert_subjects SET last_checked_on=? WHERE rule_id=? AND user_id=?") ||
                !bind_text(statement,1,checked) || !bind_id(statement,2,rule_id) || !bind_id(statement,3,subject) ||
                db_step(statement)!=DB_DONE) { db_finalize(statement); db_finalize(subjects); return false; }
            db_finalize(statement); statement=NULL;
        }
    }
    db_finalize(subjects); return result==DB_DONE;
}

static void run_rule(DbInt id, DbInt owner, time_t now, bool scheduled, WorkspaceResponse *response) {
    char today[11],utc[21],run_time[6],type[20];
    if (!clock_text(now,today,utc)) { server_error(response); return; }
    if (!begin_transaction(response)) return;
    DbStmt *statement=NULL;
    if (!prepare(&statement,"SELECT type,run_time,enabled,last_run_on,last_event_id FROM alert_rules "
        "WHERE id=? AND owner_id=? AND EXISTS(SELECT 1 FROM users WHERE id=owner_id AND active=1 AND role='manager')") ||
        !bind_id(statement,1,id) || !bind_id(statement,2,owner)) goto failure;
    int result=db_step(statement);
    if (result!=DB_ROW) {
        db_finalize(statement); statement=NULL;
        if (result!=DB_DONE) goto failure;
        rollback(); error(response,404,"Regola non trovata."); return;
    }
    (void)snprintf(type,sizeof(type),"%s",column_text(statement,0));
    (void)snprintf(run_time,sizeof(run_time),"%s",column_text(statement,1));
    bool enabled=db_column_int(statement,2)!=0;
    bool ran_today=strcmp(column_text(statement,3),today)==0;
    DbInt cursor=db_column_int64(statement,4);
    db_finalize(statement); statement=NULL;
    if (scheduled) {
        time_t slot=today_run(now,run_time);
        if (slot==(time_t)-1) goto failure;
        if (!enabled || ran_today || now<slot) {
            rollback(); response->status=200; response->body=duplicate("{\"ok\":true,\"skipped\":true}"); return;
        }
    }
    Delivery delivery;
    if (!load_delivery(id,&delivery) ||
        (strcmp(type,"invalid_job")==0 ? !run_invalid(id,&cursor,&delivery,utc) :
         strcmp(type,"missing_hours")==0 ? !run_missing(id,&delivery,today,utc,now) : true)) goto failure;
    if (!prepare(&statement,"UPDATE alert_rules SET last_run_at=?,last_run_on=?,last_event_id=? WHERE id=? AND owner_id=?") ||
        !bind_text(statement,1,utc) || !bind_text(statement,2,today) || !bind_id(statement,3,cursor) ||
        !bind_id(statement,4,id) || !bind_id(statement,5,owner) || db_step(statement)!=DB_DONE) goto failure;
    db_finalize(statement); statement=NULL;
    Json details={0};
    append(&details,"{\"notifications_created\":"); append_integer(&details,delivery.notifications);
    append(&details,",\"emails_queued\":"); append_integer(&details,delivery.emails_queued);
    append(&details,",\"last_run_at\":"); append_string(&details,utc);
    append(&details,",\"last_run_on\":"); append_string(&details,today); append(&details,"}");
    bool audited=!details.failed && storage_audit(database,scheduled ? 0 : owner,
        scheduled ? "alert_rules.trigger" : "alert_rules.run","alert_rule",id,NULL,details.data);
    free(details.data);
    if (!audited) goto failure;
    if (commit_transaction(response)) {
        Json json={0}; append(&json,"{\"ok\":true,\"notifications_created\":"); append_integer(&json,delivery.notifications);
        append(&json,",\"emails_queued\":"); append_integer(&json,delivery.emails_queued);
        append(&json,",\"next_run_at\":"); next_run_json(&json,now,run_time,today,enabled); append(&json,"}");
        finish_json(response,200,&json);
    }
    return;
failure:
    db_finalize(statement); rollback(); server_error(response);
}

static void update_email_status(DbInt id, DbInt rule, const char *previous,
                                const char *status) {
    if (strcmp(previous,status)==0) return;
    WorkspaceResponse response={0};
    if (!begin_transaction(&response)) goto done;
    DbStmt *statement=NULL;
    bool updated=prepare(&statement,"UPDATE alert_email_outbox SET status=? WHERE id=? AND status=?") &&
        bind_text(statement,1,status) && bind_id(statement,2,id) && bind_text(statement,3,previous) &&
        db_step(statement)==DB_DONE;
    int changed=updated ? db_changes(database) : 0;
    db_finalize(statement);
    if (!updated) { rollback(); server_error(&response); goto done; }
    if (changed) {
        Json details={0};
        append(&details,"{\"outbox_id\":"); append_integer(&details,id);
        append(&details,",\"status\":"); append_string(&details,status); append(&details,"}");
        bool audited=!details.failed && storage_audit(database,0,"alert_emails.delivery",
            "alert_rule",rule,NULL,details.data);
        free(details.data);
        if (!audited) { rollback(); server_error(&response); goto done; }
    }
    (void)commit_transaction(&response);
done:
    if (response.status>=500) fprintf(stderr,"Aggiornamento stato email temporaneamente non riuscito.\n");
    workspace_response_free(&response);
}

static void dispatch_email(time_t now) {
    if (!mail_client_enabled()) return;
    /* All network I/O happens outside the database write transaction and its
       cluster guard. The API ledger makes retries with the same key safe. */
    mail_client_refresh(now);
    if (!mail_client_smtp_configured()) return;
    DbStmt *statement=NULL;
    int result=DB_DONE;
    for (int pass=0;pass<2;pass++) {
        if (!prepare(&statement,"SELECT e.id,e.rule_id,e.email,e.title,e.message,e.status "
            "FROM alert_email_outbox e JOIN alert_rules r ON r.id=e.rule_id JOIN users u ON u.id=r.owner_id "
            "JOIN alert_rule_emails recipients ON recipients.rule_id=e.rule_id AND recipients.email=e.email "
            "WHERE e.status IN ('pending_configuration','retrying') AND e.id>? AND u.active=1 "
            "AND u.role='manager' ORDER BY e.id LIMIT 1") || !bind_id(statement,1,last_delivery_id)) {
            db_finalize(statement); return;
        }
        result=db_step(statement);
        if (result==DB_ROW || last_delivery_id==0) break;
        db_finalize(statement); statement=NULL;
        if (result!=DB_DONE) return;
        last_delivery_id=0;
    }
    if (result!=DB_ROW) { db_finalize(statement); return; }
    DbInt id=db_column_int64(statement,0),rule=db_column_int64(statement,1);
    char *email=duplicate(column_text(statement,2));
    char *title=duplicate(column_text(statement,3));
    char *message=duplicate(column_text(statement,4));
    char *previous=duplicate(column_text(statement,5));
    db_finalize(statement);
    if (!email || !title || !message || !previous) goto done;
    last_delivery_id=id;
    char key[128];
    int length=snprintf(key,sizeof(key),"%s:outbox:%lld",mail_client_namespace(),(long long)id);
    if (length<0 || (size_t)length>=sizeof(key)) goto done;
    MailDelivery delivery=mail_client_send(key,email,title,message);
    const char *status=delivery==MAIL_DELIVERY_SENT ? "sent" :
        delivery==MAIL_DELIVERY_UNKNOWN ? "delivery_unknown" :
        delivery==MAIL_DELIVERY_FAILED ? "delivery_failed" :
        delivery==MAIL_DELIVERY_UNCONFIGURED ? "pending_configuration" : "retrying";
    update_email_status(id,rule,previous,status);
done:
    free(email); free(title); free(message); free(previous);
}

void alerts_tick_at(time_t now) {
    if (!database) return;
    char today[11],utc[21];
    if (!clock_text(now,today,utc)) return;
    DbStmt *statement=NULL;
    if (!prepare(&statement,"SELECT r.id,r.owner_id,r.run_time,r.last_run_on FROM alert_rules r JOIN users u ON u.id=r.owner_id "
        "WHERE r.enabled=1 AND u.active=1 AND u.role='manager' ORDER BY r.id")) return;
    /* Read IDs first: the scheduler's transaction must not run under an open
       SQLite reader on another statement, and retries recheck persisted dates. */
    typedef struct { DbInt id,owner; } Rule;
    Rule *rules=NULL; size_t count=0,capacity=0; int result;
    while ((result=db_step(statement))==DB_ROW) {
        time_t slot=today_run(now,column_text(statement,2));
        if (strcmp(column_text(statement,3),today)==0 || slot==(time_t)-1 || now<slot) continue;
        if (count==capacity) {
            size_t grown=capacity ? capacity*2 : 16;
            if (grown>20000) grown=20000;
            if (grown==capacity) { result=DB_ERROR; break; }
            Rule *data=realloc(rules,grown*sizeof(*rules));
            if (!data) { result=DB_NOMEM; break; }
            rules=data; capacity=grown;
        }
        rules[count++]=(Rule){db_column_int64(statement,0),db_column_int64(statement,1)};
    }
    db_finalize(statement);
    if (result==DB_DONE) {
        for (size_t i=0;i<count;i++) {
            WorkspaceResponse response={0}; run_rule(rules[i].id,rules[i].owner,now,true,&response);
            if (response.status>=500) fprintf(stderr,"Esecuzione alert temporaneamente non riuscita.\n");
            workspace_response_free(&response);
        }
    }
    free(rules);
    /* A single delivery per tick bounds time spent in the background and the
       round-robin cursor prevents an unavailable destination starving others. */
    dispatch_email(now);
}

void alerts_tick(void) { alerts_tick_at(time(NULL)); }

static void list_notifications(DbInt actor, bool manager, WorkspaceResponse *response) {
    DbStmt *statement=NULL; Json json={0};
    if (!prepare(&statement,"SELECT COUNT(*) FROM alert_notifications WHERE recipient_id=? AND read_at IS NULL "
        "AND (?=1 OR subject_user_id=?)") || !bind_id(statement,1,actor) || !bind_id(statement,2,manager) ||
        !bind_id(statement,3,actor) || db_step(statement)!=DB_ROW) goto failure;
    DbInt unread=db_column_int64(statement,0); db_finalize(statement); statement=NULL;
    if (!prepare(&statement,"SELECT id,rule_id,subject_user_id,title,message,work_date,recorded_minutes,created_at,read_at "
        "FROM alert_notifications WHERE recipient_id=? AND (?=1 OR subject_user_id=?) ORDER BY id DESC LIMIT 100") ||
        !bind_id(statement,1,actor) || !bind_id(statement,2,manager) || !bind_id(statement,3,actor)) goto failure;
    append(&json,"{\"notifications\":["); int result; size_t count=0;
    while ((result=db_step(statement))==DB_ROW) {
        if (count++) append(&json,",");
        append(&json,"{\"id\":"); append_integer(&json,db_column_int64(statement,0));
        append(&json,",\"rule_id\":"); append_integer(&json,db_column_int64(statement,1));
        append(&json,",\"subject_user_id\":"); append_integer(&json,db_column_int64(statement,2));
        append(&json,",\"title\":"); append_string(&json,column_text(statement,3));
        append(&json,",\"message\":"); append_string(&json,column_text(statement,4));
        append(&json,",\"work_date\":");
        if (db_column_type(statement,5)==DB_NULL) append(&json,"null"); else append_string(&json,column_text(statement,5));
        append(&json,",\"recorded_minutes\":");
        if (db_column_type(statement,6)==DB_NULL) append(&json,"null"); else append_integer(&json,db_column_int64(statement,6));
        append(&json,",\"created_at\":"); append_string(&json,column_text(statement,7));
        append(&json,",\"read_at\":");
        if (db_column_type(statement,8)==DB_NULL) append(&json,"null"); else append_string(&json,column_text(statement,8));
        append(&json,"}");
    }
    db_finalize(statement); statement=NULL;
    if (result!=DB_DONE) goto failure;
    append(&json,"],\"unread_count\":"); append_integer(&json,unread); append(&json,"}"); finish_json(response,200,&json); return;
failure:
    db_finalize(statement); free(json.data); server_error(response);
}

static void read_notification(DbInt id, DbInt actor, bool manager, const char *body, WorkspaceResponse *response) {
    if (*body) { error(response,400,"La richiesta non deve contenere campi."); return; }
    char day[11],utc[21];
    if (!clock_text(time(NULL),day,utc)) { server_error(response); return; }
    if (!begin_transaction(response)) return;
    DbStmt *statement=NULL;
    if (!prepare(&statement,"SELECT 1 FROM alert_notifications WHERE id=? AND recipient_id=? AND (?=1 OR subject_user_id=?)") ||
        !bind_id(statement,1,id) || !bind_id(statement,2,actor) || !bind_id(statement,3,manager) || !bind_id(statement,4,actor)) goto failure;
    int result=db_step(statement); db_finalize(statement); statement=NULL;
    if (result==DB_DONE) { rollback(); error(response,404,"Notifica non trovata."); return; }
    if (result!=DB_ROW) goto failure;
    if (!prepare(&statement,"UPDATE alert_notifications SET read_at=COALESCE(read_at,?) WHERE id=? AND recipient_id=?") ||
        !bind_text(statement,1,utc) || !bind_id(statement,2,id) || !bind_id(statement,3,actor) || db_step(statement)!=DB_DONE) goto failure;
    db_finalize(statement); statement=NULL;
    if (commit_transaction(response)) { response->status=200; response->body=duplicate("{\"ok\":true}"); }
    return;
failure:
    db_finalize(statement); rollback(); server_error(response);
}

static bool item_path(const char *value, DbInt *id, const char **suffix) {
    const char *end=value;
    while (*end>='0' && *end<='9') end++;
    size_t length=(size_t)(end-value);
    if (!length || length>=24) return false;
    char number[24]; memcpy(number,value,length); number[length]='\0';
    if (!parse_positive(number,id)) return false;
    *suffix=end; return true;
}

bool alerts_handle(const char *method, const char *target, const char *query,
                   const char *body, DbInt actor_id, const char *role,
                   WorkspaceResponse *response) {
    if (!method || !target || !response) return false;
    bool rules=strcmp(target,"/api/alert-rules")==0;
    bool rule_item=strncmp(target,"/api/alert-rules/",17)==0;
    bool notifications=strcmp(target,"/api/notifications")==0;
    bool notification_item=strncmp(target,"/api/notifications/",19)==0;
    if (!rules && !rule_item && !notifications && !notification_item) return false;
    response->status=0; response->body=NULL;
    if (!database) { server_error(response); return true; }
    if (actor_id<=0) { error(response,401,"Accedi per continuare."); return true; }
    bool manager=role && strcmp(role,"manager")==0;
    bool worker=role && strcmp(role,"worker")==0;
    if ((!manager && !worker) || ((rules || rule_item) && !manager)) {
        error(response,403,"Solo i gestori possono configurare gli alert."); return true;
    }
    if (!query) query="";
    if (!body) body="";
    if (*query) { error(response,400,"Parametri della richiesta non validi."); return true; }
    DbInt id=0; const char *suffix="";
    if ((rule_item && !item_path(target+17,&id,&suffix)) ||
        (notification_item && !item_path(target+19,&id,&suffix))) {
        error(response,400,"Identificativo non valido."); return true;
    }
    if (rules && strcmp(method,"GET")==0) list_rules(actor_id,response);
    else if ((rules || (rule_item && !*suffix)) && strcmp(method,"POST")==0) write_rule(body,actor_id,id,response);
    else if (rule_item && strcmp(suffix,"/run")==0 && strcmp(method,"POST")==0) {
        if (*body) error(response,400,"La richiesta non deve contenere campi.");
        else run_rule(id,actor_id,time(NULL),false,response);
    } else if (notifications && strcmp(method,"GET")==0) list_notifications(actor_id,manager,response);
    else if (notification_item && strcmp(suffix,"/read")==0 && strcmp(method,"POST")==0) read_notification(id,actor_id,manager,body,response);
    else error(response,405,"Metodo non consentito.");
    return true;
}
