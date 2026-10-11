#ifndef PUNTO_IMPROVEMENTS_GOVERNANCE_H
#define PUNTO_IMPROVEMENTS_GOVERNANCE_H

#include "db.h"
#include "workspace.h"

/* Called after base schema/accounts initialization, before accepting requests.
   The one-shot seed preserves all pre-upgrade manager administrations. */
bool improvements_governance_schema(Db *database);
bool improvements_governance_handle(Db *database, const char *method,
    const char *target, const char *query, const char *body, DbInt actor_id,
    WorkspaceResponse *response);
/* Legacy profile POST routes through the same approval workflow. */
void improvements_governance_profiles(Db *database, DbInt actor_id,
    DbInt user_id, const char *body, WorkspaceResponse *response);

#endif
