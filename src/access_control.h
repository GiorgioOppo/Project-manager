#ifndef PUNTO_ACCESS_CONTROL_H
#define PUNTO_ACCESS_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include "db.h"

typedef struct {
    bool admin;
    bool manager;
    bool sales;
    bool finance;
} AccessPolicy;

#define ACCESS_POLICY_USER_CAPACITY 201U

/* Resolve the active account and explicit local functional memberships live.
   1 active, 0 missing/inactive, -1 database or invalid stored data. On failure
   every flag is false. The database role is authoritative; role never grants
   permissions and is accepted only for compatibility with existing callers. */
int access_policy_resolve(Db *db, DbInt actor_id, const char *role,
                          AccessPolicy *out);

/* Personal hours are always visible to an active author. Other people's hours
   require a live manager role and an organizational membership in their group
   or an ancestor (maximum 16 levels). Inactive subjects remain visible for
   historical reporting. Functional groups never extend this scope.
   Unavailable organizational groups restrict the result to personal hours.
   Malformed/cyclic/oversized graphs fail closed. No result is cached. */
bool access_policy_visible_users(Db *db, DbInt actor_id, bool manager,
                                 DbInt ids[ACCESS_POLICY_USER_CAPACITY],
                                 size_t *count);
/* 1 visible, 0 denied, -1 database/graph failure. */
int access_policy_can_view_time(Db *db, DbInt actor_id, DbInt target_id,
                                bool manager);

/* Live governance checks: 1 allowed, 0 denied, -1 invalid graph/database.
   job/group/user == 0 asks whether any matching creation delegation exists.
   Space administration never expands the personal/team time scope. */
int access_policy_can_manage_job(Db *db, DbInt actor_id, DbInt job_id);
int access_policy_can_manage_group(Db *db, DbInt actor_id, DbInt group_id);
int access_policy_can_manage_user(Db *db, DbInt actor_id, DbInt user_id);
/* A different requester in the current team, and an active administration or
   correction-approval delegation covering their organizational group today. */
int access_policy_can_approve_correction(Db *db, DbInt actor_id, DbInt requester_id);

#endif
