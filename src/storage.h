#ifndef PUNTO_STORAGE_H
#define PUNTO_STORAGE_H

#include <stdbool.h>
#include "db.h"
#include "workspace.h"

/* Apply versioned migrations before accepting requests, preserving legacy data. */
bool storage_preflight(const char *db_path);
bool storage_open(const char *db_path);
void storage_close(void);
bool storage_health(void);

/* Call inside the mutation's transaction, using that same database connection. */
bool storage_audit(Db *db, DbInt actor_id, const char *action,
                   const char *entity_type, DbInt entity_id,
                   const char *before_json, const char *after_json);
bool storage_handle(const char *method, const char *target, const char *query,
                    DbInt actor_id, const char *role, WorkspaceResponse *response);

#endif
