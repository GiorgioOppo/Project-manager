#ifndef PUNTO_IMPROVEMENTS_INTEGRATIONS_H
#define PUNTO_IMPROVEMENTS_INTEGRATIONS_H
#include "workspace.h"
bool improvements_integrations_schema(Db *db);
void improvements_integrations_attach(Db *db);
bool improvements_integrations_handle(Db *db, const char *method, const char *target,
                                     const char *query, const char *body, DbInt actor,
                                     WorkspaceResponse *response);
/* Global approved economic periods freeze underlying hours. Database failure
   fails closed; economic corrections are separate append-only adjustments. */
bool improvements_integrations_time_open(Db *db, DbInt job, const char *date);
#endif
