#ifndef PUNTO_TICKET_BRIDGE_H
#define PUNTO_TICKET_BRIDGE_H

#include "accounts.h"
#include "workspace.h"
#include <time.h>

/* Catalog and explicitly delegated calendar bridge. The controller validates the opaque grant and
   supplies a live account; identity is never selected by browser parameters. */
bool ticket_bridge_open(const char *db_path);
void ticket_bridge_close(void);
void ticket_bridge_read(const char *resource, const char *query,
                        const Account *actor, const char *state, time_t expires, bool calendar_access,
                        WorkspaceResponse *response);
void ticket_bridge_calendar(const char *resource, const char *input,
                            const Account *actor, WorkspaceResponse *response);

#endif
