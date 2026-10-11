#include "access_control.h"
#include "organization.h"

#include <string.h>

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
    if (db_prepare_v2(db, "SELECT id,parent_id FROM organization_groups ORDER BY id LIMIT 201",
                     -1, &statement, NULL) != DB_OK) goto failure;
    int step;
    while ((step = db_step(statement)) == DB_ROW) {
        if (group_count == ACCESS_GROUP_LIMIT) goto failure;
        AccessGroup *group = &groups[group_count++];
        group->id = db_column_int64(statement, 0);
        group->parent = db_column_type(statement, 1) == DB_NULL ? 0 : db_column_int64(statement, 1);
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
        groups[at].direct = true;
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
        if (!groups[at].visible) continue;
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
