#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "access_control.h"
#include "organization.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#define ACCESS_GROUP_LIMIT 200U
#define ACCESS_GROUP_DEPTH 16U

int access_policy_resolve(Db *db, DbInt actor_id, const char *role,
                          AccessPolicy *out) {
    (void)role;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!db || actor_id <= 0) return 0;
    DbStmt *statement = NULL;
    AccessPolicy resolved = {0};
    if (db_prepare_v2(db, "SELECT role,active FROM users WHERE id=?", -1,
                     &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, actor_id) != DB_OK) goto failure;
    int step = db_step(statement);
    if (step == DB_DONE) { db_finalize(statement); return 0; }
    if (step != DB_ROW) goto failure;
    int active = db_column_int(statement, 1);
    const char *stored_role = (const char *)db_column_text(statement, 0);
    if (!stored_role || (strcmp(stored_role, "manager") != 0 &&
                         strcmp(stored_role, "worker") != 0) ||
        (active != 0 && active != 1)) goto failure;
    resolved.manager = strcmp(stored_role, "manager") == 0;
    db_finalize(statement); statement = NULL;
    if (!active) return 0;
    if (db_prepare_v2(db, "SELECT 1 FROM access_space_admins WHERE user_id=?",
                     -1, &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, actor_id) != DB_OK) goto failure;
    step = db_step(statement);
    if (step != DB_ROW && step != DB_DONE) goto failure;
    resolved.admin = step == DB_ROW && resolved.manager;
    db_finalize(statement); statement = NULL;
    if (db_prepare_v2(db, "SELECT group_code FROM access_group_members WHERE user_id=?",
                     -1, &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, actor_id) != DB_OK) goto failure;
    while ((step = db_step(statement)) == DB_ROW) {
        const char *code = (const char *)db_column_text(statement, 0);
        if (code && strcmp(code, "sales") == 0) resolved.sales = true;
        else if (code && strcmp(code, "finance") == 0) resolved.finance = true;
        else goto failure;
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement);
    *out = resolved;
    return 1;
failure:
    db_finalize(statement);
    return -1;
}

typedef struct {
    DbInt id;
    DbInt parent;
    bool direct;
    bool visible;
    bool active;
} AccessGroup;

static size_t find_group(const AccessGroup *groups, size_t count, DbInt id) {
    size_t index = 0;
    while (index < count && groups[index].id != id) index++;
    return index;
}

bool access_policy_visible_users(Db *db, DbInt actor_id, bool manager,
                                 DbInt ids[ACCESS_POLICY_USER_CAPACITY],
                                 size_t *count) {
    if (!count || !ids) return false;
    *count = 0;
    AccessPolicy policy;
    int resolved = access_policy_resolve(db, actor_id, NULL, &policy);
    if (resolved < 0) return false;
    if (!resolved) return true;
    ids[0] = actor_id;
    *count = 1;
    if (!manager || !policy.manager || !organization_groups_available()) return true;
    AccessGroup groups[ACCESS_GROUP_LIMIT] = {{0}};
    size_t group_count = 0;
    DbStmt *statement = NULL;
    if (db_prepare_v2(db, organization_groups_managed() ?
                     "SELECT g.id,g.parent_id,COALESCE(o.active,0) FROM organization_groups g LEFT JOIN oidc_groups o ON o.group_id=g.id ORDER BY g.id LIMIT 201" :
                     "SELECT id,parent_id,1 FROM organization_groups ORDER BY id LIMIT 201",
                     -1, &statement, NULL) != DB_OK) goto failure;
    int step;
    while ((step = db_step(statement)) == DB_ROW) {
        if (group_count == ACCESS_GROUP_LIMIT) goto failure;
        AccessGroup *group = &groups[group_count++];
        group->id = db_column_int64(statement, 0);
        group->parent = db_column_type(statement, 1) == DB_NULL ? 0 : db_column_int64(statement, 1);
        group->active = db_column_int(statement,2)==1;
        if (group->id <= 0 || group->parent < 0) goto failure;
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (db_prepare_v2(db, "SELECT group_id FROM group_members WHERE user_id=? LIMIT 201",
                     -1, &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, actor_id) != DB_OK) goto failure;
    size_t memberships = 0;
    while ((step = db_step(statement)) == DB_ROW) {
        size_t at = find_group(groups, group_count, db_column_int64(statement, 0));
        if (at == group_count || ++memberships > ACCESS_GROUP_LIMIT) goto failure;
        groups[at].direct = groups[at].active;
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    /* Validate every ancestry path before using any team expansion. */
    for (size_t index = 0; index < group_count; index++) {
        bool visited[ACCESS_GROUP_LIMIT] = {false};
        size_t at = index;
        unsigned depth = 0;
        while (at < group_count) {
            if (visited[at] || ++depth > ACCESS_GROUP_DEPTH) goto failure;
            visited[at] = true;
            if (groups[at].direct) groups[index].visible = true;
            if (!groups[at].parent) break;
            at = find_group(groups, group_count, groups[at].parent);
            if (at == group_count) goto failure;
        }
    }
    if (db_prepare_v2(db, "SELECT DISTINCT m.user_id,m.group_id FROM group_members m "
                     "JOIN users u ON u.id=m.user_id ORDER BY m.user_id,m.group_id",
                     -1, &statement, NULL) != DB_OK) goto failure;
    size_t rows = 0;
    while ((step = db_step(statement)) == DB_ROW) {
        if (++rows > ACCESS_GROUP_LIMIT * ACCESS_GROUP_LIMIT) goto failure;
        DbInt user = db_column_int64(statement, 0);
        size_t at = find_group(groups, group_count, db_column_int64(statement, 1));
        if (user <= 0 || at == group_count) goto failure;
        if (!groups[at].visible || !groups[at].active) continue;
        size_t index = 0;
        while (index < *count && ids[index] != user) index++;
        if (index != *count) continue;
        if (*count >= ACCESS_POLICY_USER_CAPACITY) goto failure;
        ids[(*count)++] = user;
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement);
    return true;
failure:
    db_finalize(statement);
    *count = 0;
    return false;
}

int access_policy_can_view_time(Db *db, DbInt actor_id, DbInt target_id,
                                bool manager) {
    if (target_id <= 0) return 0;
    if (target_id == actor_id) {
        AccessPolicy policy;
        return access_policy_resolve(db, actor_id, NULL, &policy);
    }
    DbInt ids[ACCESS_POLICY_USER_CAPACITY];
    size_t count = 0;
    if (!access_policy_visible_users(db, actor_id, manager, ids, &count)) return -1;
    for (size_t index = 0; index < count; index++)
        if (ids[index] == target_id) return 1;
    return 0;
}

static int delegated_group(Db *db, DbInt actor, DbInt target, const char *capability) {
    if (!organization_groups_available()) return 0;
    char today[11], sql[700];
    time_t now = time(NULL); struct tm local;
    if (now == (time_t)-1 || !localtime_r(&now, &local) ||
        strftime(today, sizeof(today), "%Y-%m-%d", &local) != 10) return -1;
    int n = snprintf(sql, sizeof(sql), "SELECT d.group_id FROM access_area_delegations d "
        "WHERE d.manager_id=? AND d.active=1 AND d.%s=1 AND d.starts_on<=? "
        "AND (d.ends_on='' OR d.ends_on>=?) %s ORDER BY d.id LIMIT 201", capability,
        organization_groups_managed()?"AND EXISTS(SELECT 1 FROM oidc_groups o WHERE o.group_id=d.group_id AND o.active=1)":"");
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
    DbStmt *statement = NULL;
    DbInt delegated[200]; size_t count = 0;
    if (db_prepare_v2(db, sql, -1, &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, actor) != DB_OK ||
        db_bind_text(statement, 2, today, -1, DB_TRANSIENT) != DB_OK ||
        db_bind_text(statement, 3, today, -1, DB_TRANSIENT) != DB_OK) goto failure;
    int step;
    while ((step = db_step(statement)) == DB_ROW) {
        if (count == 200) goto failure;
        delegated[count++] = db_column_int64(statement, 0);
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement); statement = NULL;
    if (!target) return count ? 1 : 0;
    DbInt seen[16]; size_t depth = 0;
    while (target) {
        if (depth == 16) return -1;
        for (size_t i = 0; i < depth; i++) if (seen[i] == target) return -1;
        seen[depth++] = target;
        if (db_prepare_v2(db, organization_groups_managed()?
                         "SELECT g.parent_id,COALESCE(o.active,0) FROM organization_groups g LEFT JOIN oidc_groups o ON o.group_id=g.id WHERE g.id=?":
                         "SELECT parent_id,1 FROM organization_groups WHERE id=?", -1,
                         &statement, NULL) != DB_OK || db_bind_int64(statement, 1, target) != DB_OK) goto failure;
        step = db_step(statement);
        if (step != DB_ROW) goto failure;
        if (db_column_int(statement,1)!=1) {db_finalize(statement);return 0;}
        target = db_column_type(statement, 0) == DB_NULL ? 0 : db_column_int64(statement, 0);
        db_finalize(statement); statement = NULL;
    }
    for (size_t i = 0; i < depth; i++)
        for (size_t j = 0; j < count; j++) if (seen[i] == delegated[j]) return 1;
    return 0;
failure:
    db_finalize(statement); return -1;
}

static int can_manage(Db *db, DbInt actor, DbInt target, int kind) {
    AccessPolicy policy;
    int resolved = access_policy_resolve(db, actor, NULL, &policy);
    if (resolved != 1) return resolved;
    if (policy.admin) return 1;
    if (!policy.manager) return 0;
    const char *capability = kind == 0 ? "can_manage_groups" :
        kind == 1 ? "can_manage_users" : kind == 2 ? "can_manage_jobs" : "can_approve_corrections";
    if (kind == 0 || !target) return delegated_group(db, actor, target, capability);
    const char *sql = kind == 2 ?
        "SELECT group_id FROM job_group_grants WHERE job_id=? ORDER BY group_id LIMIT 201" :
        "SELECT group_id FROM group_members WHERE user_id=? ORDER BY group_id LIMIT 201";
    DbStmt *statement = NULL;
    if (db_prepare_v2(db, sql, -1, &statement, NULL) != DB_OK ||
        db_bind_int64(statement, 1, target) != DB_OK) goto failure;
    int step, allowed = 0; size_t count = 0;
    while ((step = db_step(statement)) == DB_ROW) {
        if (++count > 200) goto failure;
        int match = delegated_group(db, actor, db_column_int64(statement, 0), capability);
        if (match < 0) goto failure;
        if (match) allowed = 1;
    }
    if (step != DB_DONE) goto failure;
    db_finalize(statement); return allowed;
failure:
    db_finalize(statement); return -1;
}

int access_policy_can_manage_job(Db *db, DbInt actor, DbInt job) { return can_manage(db, actor, job, 2); }
int access_policy_can_manage_group(Db *db, DbInt actor, DbInt group) { return can_manage(db, actor, group, 0); }
int access_policy_can_manage_user(Db *db, DbInt actor, DbInt user) { return can_manage(db, actor, user, 1); }
int access_policy_can_approve_correction(Db *db, DbInt actor, DbInt requester) {
    if (actor == requester || requester <= 0) return 0;
    AccessPolicy policy;
    int active = access_policy_resolve(db, actor, NULL, &policy);
    if (active != 1) return active;
    int visible = access_policy_can_view_time(db, actor, requester, policy.manager);
    return visible != 1 ? visible : can_manage(db, actor, requester, 3);
}
