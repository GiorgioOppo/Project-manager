"""Functional profiles, live authorization and organizational time privacy."""
import json
import sqlite3
import subprocess
import tempfile
import unittest
from pathlib import Path

import test_roles as roles
import test_managed_groups_http as managed_groups


class AccessPolicyNativeTests(unittest.TestCase):
    def test_live_scope_and_fail_closed_graphs(self):
        with tempfile.TemporaryDirectory(prefix="punto-access-policy-") as fixture:
            binary = Path(fixture) / "policy-check"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-I", str(roles.ROOT / "src"), str(roles.ROOT / "src/access_control.c"),
                str(roles.ROOT / "tests/access_control_native_check.c"), "-lsqlite3", "-o", str(binary)],
                check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("passed", result.stdout)


class FunctionalGroupIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user
    create_job = roles.RoleIntegrationTests.create_job
    create_time = roles.RoleIntegrationTests.create_time
    time_fields = roles.RoleIntegrationTests.time_fields

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def assign(self, user, groups, expected=200):
        status, data, _ = self.api("POST", "/api/users/%d/access-groups" % user,
            {"groups": json.dumps(groups)}, cookie=self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def test_catalogue_and_live_derived_profiles_without_department_defaults(self):
        catalogue = self.get("/api/access-groups")["groups"]
        self.assertEqual([group["code"] for group in catalogue], ["operator", "manager", "sales", "finance"])
        self.assertEqual([group["member_count"] for group in catalogue], [1, 1, 0, 0])
        self.assertEqual([group["editable"] for group in catalogue], [False, False, True, True])
        worker = self.create_user()
        path = "/api/users/%d/access-groups" % worker
        self.assertEqual(self.get(path)["effective"], ["operator"])
        changed = self.assign(worker, ["finance", "sales"])
        self.assertEqual(changed["selected"], ["sales", "finance"])
        self.assertEqual(changed["effective"], ["operator", "sales", "finance"])
        self.assertEqual(next(user for user in self.users() if user["id"] == worker)["access_groups"],
                         ["operator", "sales", "finance"])
        status, data, _ = self.edit_user(worker, role="manager")
        self.assertEqual(status, 200, data)
        self.assertEqual(self.get(path)["effective"], ["operator", "manager", "sales", "finance"])
        status, data, _ = self.edit_user(worker, active="0")
        self.assertEqual(status, 200, data)
        inactive = self.get(path)
        self.assertEqual(inactive["effective"], [])
        self.assertEqual(inactive["selected"], ["sales", "finance"])
        self.assertEqual(next(user for user in self.users() if user["id"] == worker)["access_groups"], [])
        self.stop_server(); self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.get(path), inactive)

    def test_membership_validation_permissions_and_audit_rollback(self):
        worker = self.create_user()
        path = "/api/users/%d/access-groups" % worker
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        for groups in (["operator"], ["manager"], ["Sales"], ["sales", "sales"], [None], {}):
            self.assign(worker, groups, 400)
        for payload in ('["sales"]true', '["finance",]', 'null', '["sales\\u0000"]'):
            self.assert_error(400, "POST", path, {"groups": payload}, cookie=self.manager_cookie)
        self.assert_error(403, "GET", path, cookie=cookie)
        self.assert_error(403, "GET", "/api/access-groups", cookie=cookie)
        self.assert_error(403, "POST", path, {"groups": '["finance"]'}, cookie=cookie)
        self.assert_error(404, "POST", "/api/users/99999/access-groups", {"groups": "[]"}, cookie=self.manager_cookie)
        self.assign(worker, ["sales"])
        with sqlite3.connect(self.db_path) as database:
            audit = database.execute("SELECT before_json,after_json FROM audit_events "
                "WHERE action='users.access_groups.update' ORDER BY id DESC LIMIT 1").fetchone()
            self.assertEqual(json.loads(audit[0])["selected"], [])
            self.assertEqual(json.loads(audit[1])["selected"], ["sales"])
            database.execute("CREATE TRIGGER reject_profile_audit BEFORE INSERT ON audit_events "
                "WHEN NEW.action='users.access_groups.update' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        self.assign(worker, ["finance"], 500)
        self.assertEqual(self.get(path)["selected"], ["sales"])

    def test_schema13_upgrade_keeps_hours_and_assigns_no_departments(self):
        worker = self.create_user()
        job = self.create_job()
        entry = self.create_time(self.manager_cookie, job, minutes=37)
        self.stop_server()
        with sqlite3.connect(self.db_path) as database:
            database.executescript("DROP TABLE access_group_members;DROP TRIGGER access_groups_no_update;"
                "DROP TRIGGER access_groups_no_delete;DROP TABLE access_groups;PRAGMA user_version=12;")
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        with sqlite3.connect(self.db_path) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 13)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM access_group_members").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM users").fetchone()[0], 2)
            self.assertEqual(database.execute("SELECT id,minutes FROM time_entries").fetchall(), [(entry, 37)])
            with self.assertRaises(sqlite3.IntegrityError):
                database.execute("UPDATE access_groups SET name='Other' WHERE code='sales'")
            with self.assertRaises(sqlite3.IntegrityError):
                database.execute("INSERT INTO access_group_members VALUES('manager',?)", (worker,))
        self.assertEqual(self.get("/api/users/%d/access-groups" % worker)["selected"], [])


class ManagedFunctionalGroupTests(unittest.TestCase):
    setUp = managed_groups.ManagedGroupsHTTPTests.setUp
    start = managed_groups.ManagedGroupsHTTPTests.start
    stop = managed_groups.ManagedGroupsHTTPTests.stop
    request = managed_groups.ManagedGroupsHTTPTests.request
    sso = managed_groups.ManagedGroupsHTTPTests.sso
    session_cookie = managed_groups.ManagedGroupsHTTPTests.session_cookie
    local_cookie = managed_groups.ManagedGroupsHTTPTests.local_cookie
    managed_sso = managed_groups.ManagedGroupsHTTPTests.managed_sso
    db = managed_groups.ManagedGroupsHTTPTests.db
    publish = managed_groups.ManagedGroupsHTTPTests.publish
    get = managed_groups.ManagedGroupsHTTPTests.get

    @classmethod
    def setUpClass(cls):
        cls.provider = managed_groups.OIDCProvider()
        cls.addClassCleanup(cls.provider.close)

    def test_keycloak_names_never_assign_functional_profiles_and_local_policy_stays_editable(self):
        admin = self.local_cookie()
        self.publish([{"id": "finance-team", "parent_id": None, "name": "Finance", "local_id": None,
                       "members": ["functional-worker"]}])
        status, headers, _ = self.managed_sso(subject="functional-worker", email="functional@example.test", role="worker")
        self.assertEqual(status, 303)
        worker = self.session_cookie(headers)
        with self.db() as database:
            user_id = database.execute("SELECT id FROM users WHERE email='functional@example.test'").fetchone()[0]
        path = "/api/users/%d/access-groups" % user_id
        self.assertEqual(self.get(path, admin)["selected"], [])
        self.assertEqual(self.get(path, admin)["effective"], ["operator"])
        status, _, raw = self.request("POST", path, admin, {"groups": '["finance"]'}, {"Origin": self.origin})
        self.assertEqual(status, 200, raw)
        self.assertEqual(json.loads(raw)["effective"], ["operator", "finance"])
        status, _, _ = self.request("POST", "/api/users/%d" % user_id, admin,
            {"name": "Forbidden", "role": "manager", "active": "1"}, {"Origin": self.origin})
        self.assertEqual(status, 403)
        self.assertEqual(self.request("GET", path, worker)[0], 403)
        self.assertEqual(self.managed_sso(subject="functional-worker", email="functional@example.test", role="worker")[0], 303)
        self.assertEqual(self.get(path, admin)["selected"], ["finance"])


if __name__ == "__main__":
    unittest.main()
