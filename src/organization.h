#ifndef PUNTO_ORGANIZATION_H
#define PUNTO_ORGANIZATION_H

#include "workspace.h"
#include <stddef.h>

typedef struct {
    DbInt job_ids[1000];
    unsigned char permissions[1000];
    size_t count;
} OrganizationAccess;

bool organization_open(const char *db_path);
void organization_close(void);
bool organization_handle(const char *method, const char *target, const char *query,
                         const char *body, DbInt actor_id, const char *role,
                         WorkspaceResponse *response);

/* Evaluate live direct and inherited grants on the caller's connection.
   0 denied, 1 visible, 3 visible + time logging, -1 database/graph failure.
   A logging grant must cover both today and work_date; NULL uses today.
   Global manager access is deliberately handled by the authenticated caller. */
int organization_job_access(Db *db, DbInt job_id, DbInt user_id,
                            const char *today, const char *work_date);
/* A request-local snapshot, never retained across requests or mutations. */
bool organization_access_list(Db *db, DbInt user_id, const char *today,
                              const char *work_date, OrganizationAccess *out);

#endif
