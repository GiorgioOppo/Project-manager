#include "access_control.h"

#include <assert.h>
#include <stdio.h>

static bool groups_available = true;
bool organization_groups_available(void) { return groups_available; }
bool organization_groups_managed(void) { return false; }

static void execute(Db *db, const char *sql) {
    assert(db_exec(db, sql, NULL, NULL, NULL) == DB_OK);
}

int main(void) {
    Db *db = NULL;
    assert(db_open_v2(":memory:", &db, DB_OPEN_READWRITE | DB_OPEN_CREATE, NULL) == DB_OK);
    execute(db, "CREATE TABLE users(id INTEGER PRIMARY KEY,role TEXT,active INTEGER);"
        "CREATE TABLE access_space_admins(user_id INTEGER PRIMARY KEY);"
        "CREATE TABLE access_group_members(group_code TEXT,user_id INTEGER);"
        "CREATE TABLE organization_groups(id INTEGER PRIMARY KEY,parent_id INTEGER);"
        "CREATE TABLE group_members(group_id INTEGER,user_id INTEGER);"
        "INSERT INTO users VALUES(1,'manager',1),(2,'worker',1),(3,'worker',1),"
        "(4,'worker',1),(5,'worker',1),(6,'worker',0);"
        "INSERT INTO organization_groups VALUES(9,NULL),(10,9),(11,10),(20,NULL);"
        "INSERT INTO group_members VALUES(10,1),(10,2),(11,3),(9,4),(20,5),(11,6);");
    AccessPolicy policy;
    assert(access_policy_resolve(db, 1, "worker", &policy) == 1);
    assert(policy.manager && !policy.sales && !policy.finance);
    execute(db, "INSERT INTO access_group_members VALUES('finance',1),('sales',3);");
    assert(access_policy_resolve(db, 1, NULL, &policy) == 1 && policy.finance && !policy.sales);
    assert(access_policy_can_view_time(db, 1, 1, true) == 1);
    assert(access_policy_can_view_time(db, 1, 2, true) == 1);
    assert(access_policy_can_view_time(db, 1, 3, true) == 1);
    assert(access_policy_can_view_time(db, 1, 6, true) == 1); /* historical inactive subject */
    assert(access_policy_can_view_time(db, 1, 4, true) == 0); /* parent is outside actor's scope */
    assert(access_policy_can_view_time(db, 1, 5, true) == 0);
    assert(access_policy_can_view_time(db, 2, 3, true) == 0); /* forged manager hint */
    assert(access_policy_can_view_time(db, 1, 2, false) == 0);
    groups_available = false;
    assert(access_policy_can_view_time(db, 1, 2, true) == 0);
    assert(access_policy_can_view_time(db, 1, 1, true) == 1);
    groups_available = true;
    execute(db, "DELETE FROM group_members WHERE user_id=1;");
    assert(access_policy_can_view_time(db, 1, 2, true) == 0);
    execute(db, "INSERT INTO group_members VALUES(10,1);UPDATE users SET role='worker' WHERE id=1;");
    assert(access_policy_resolve(db, 1, "manager", &policy) == 1 && !policy.manager && policy.finance);
    assert(access_policy_can_view_time(db, 1, 2, true) == 0);
    execute(db, "UPDATE users SET role='manager' WHERE id=1;UPDATE organization_groups SET parent_id=11 WHERE id=10;");
    DbInt ids[ACCESS_POLICY_USER_CAPACITY]; size_t count = 99;
    assert(!access_policy_visible_users(db, 1, true, ids, &count) && count == 0);
    assert(access_policy_can_view_time(db, 1, 2, true) == -1);
    assert(access_policy_can_view_time(db, 1, 1, true) == 1);
    execute(db, "UPDATE organization_groups SET parent_id=9 WHERE id=10;UPDATE users SET active=0 WHERE id=1;");
    assert(access_policy_resolve(db, 1, "manager", &policy) == 0);
    assert(!policy.manager && !policy.sales && !policy.finance);
    assert(access_policy_can_view_time(db, 1, 1, true) == 0);
    execute(db, "UPDATE users SET active=1 WHERE id=1;DELETE FROM organization_groups;DELETE FROM group_members;");
    for (int i = 1; i <= 17; i++) {
        char sql[160];
        snprintf(sql, sizeof(sql), "INSERT INTO organization_groups VALUES(%d,%s%d);", i,
            i == 1 ? "NULL+" : "", i - 1);
        execute(db, sql);
    }
    execute(db, "INSERT INTO group_members VALUES(1,1),(17,2);");
    assert(!access_policy_visible_users(db, 1, true, ids, &count));
    execute(db, "DELETE FROM organization_groups WHERE id=17;UPDATE group_members SET group_id=16 WHERE user_id=2;");
    assert(access_policy_can_view_time(db, 1, 2, true) == 1);
    assert(db_close(db) == DB_OK);
    puts("live functional policy and bounded organizational scope passed");
    return 0;
}
