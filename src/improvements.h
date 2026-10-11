#ifndef PUNTO_IMPROVEMENTS_H
#define PUNTO_IMPROVEMENTS_H
#include "workspace.h"
#include "access_control.h"
#include <json-c/json.h>

/* Form/query parsing rejects malformed escapes, duplicate keys and NUL bytes.
   Values are strings; callers validate every accepted field and reference. */
struct json_object *im_fields(const char *encoded);
const char *im_string(struct json_object *fields, const char *key);
bool im_integer(struct json_object *fields, const char *key, DbInt minimum, DbInt maximum, DbInt *value);
bool im_date(const char *value);
bool im_today(char value[11]);
void im_reply(WorkspaceResponse *response, int status, struct json_object *value);
void im_error(WorkspaceResponse *response, int status, const char *message);
bool im_exec(Db *db, const char *sql);
bool im_begin(Db *db, WorkspaceResponse *response);
void im_rollback(Db *db);
bool im_audit_commit(Db *db, DbInt actor, const char *action, const char *entity,
                     DbInt id, const char *before, const char *after, WorkspaceResponse *response);
/* Add a missing column without replacing data. DDL is caller-owned constant SQL. */
bool im_column(Db *db, const char *table, const char *name, const char *sqlite_ddl, const char *mysql_ddl);
bool improvements_schema(Db *db);
bool improvements_open(const char *path);
void improvements_close(void);
bool improvements_handle(const char *method, const char *target, const char *query,
                         const char *body, DbInt actor, WorkspaceResponse *response);
#endif
