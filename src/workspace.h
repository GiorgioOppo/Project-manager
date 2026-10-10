#ifndef PUNTO_WORKSPACE_H
#define PUNTO_WORKSPACE_H

#include <stdbool.h>
#include "db.h"

typedef struct {
    int status;
    char *body;
} WorkspaceResponse;

/* The caller supplies the authenticated owner and the server-side account role. */
bool workspace_open(const char *db_path);
void workspace_close(void);
bool workspace_handle(const char *method, const char *target, const char *query,
                      const char *body, DbInt actor_id,
                      const char *owner, const char *role,
                      WorkspaceResponse *response);
void workspace_response_free(WorkspaceResponse *response);

#endif
