#ifndef PUNTO_IMPROVEMENTS_OPERATIONS_H
#define PUNTO_IMPROVEMENTS_OPERATIONS_H
#include "workspace.h"
bool improvements_operations_schema(Db *db);
int improvements_operations_unresolved(Db *db, DbInt actor, DbInt job, DbInt *count);
bool improvements_operations_handle(Db *db, const char *method, const char *target,
                                    const char *query, const char *body, DbInt actor,
                                    WorkspaceResponse *response);
#endif
