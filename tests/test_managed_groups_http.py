"""Managed Keycloak group projections through Punto's real HTTP and SSO routes."""

import json
import time
import unittest

from oidc_fixture import OIDCProvider
import test_punto_oidc as legacy
import test_punto_managed_users as managed


class ManagedGroupsHTTPTests(unittest.TestCase):
    setUp = legacy.PuntoOIDCTests.setUp
    stop = legacy.PuntoOIDCTests.stop
    request = legacy.PuntoOIDCTests.request
    sso = legacy.PuntoOIDCTests.sso
    session_cookie = legacy.PuntoOIDCTests.session_cookie
    local_cookie = managed.PuntoManagedUserTests.local_cookie
    managed_sso = managed.PuntoManagedUserTests.managed_sso
    db = managed.PuntoManagedUserTests.db

    @classmethod
    def setUpClass(cls):
        cls.provider = OIDCProvider()
        cls.addClassCleanup(cls.provider.close)

    def start(self):
        self.groups_file = self.directory / "groups-snapshot.json"
        self.environment.update(AUTH_GROUP_MANAGEMENT="keycloak",
                                AUTH_GROUPS_SNAPSHOT_FILE=str(self.groups_file))
        if not self.groups_file.exists():
            self.publish([], {})
        managed.PuntoManagedUserTests.start(self)

    def publish(self, groups, bindings=None, *, stale=False):
        document = {"version": 1, "issuer": self.environment["OIDC_ISSUER_URL"],
                    "generated_at": int(time.time()) - (180 if stale else 0),
                    "groups": groups, "user_bindings": bindings or {}}
        target = self.groups_file.with_suffix(".tmp")
        target.write_text(json.dumps(document))
        target.replace(self.groups_file)

    def get(self, path, cookie):
        status, _, body = self.request("GET", path, cookie)
        self.assertEqual(status, 200, body)
        return json.loads(body)

    def test_migration_read_only_directory_signed_sso_and_live_inherited_revocation(self):
        admin = self.local_cookie()
        with self.db() as db:
            db.execute("INSERT INTO users(email,name,role,active,password_hash,salt) "
                       "SELECT 'worker@example.test','Worker','worker',1,password_hash,salt FROM users LIMIT 1")
            worker_id = db.execute("SELECT id FROM users WHERE email='worker@example.test'").fetchone()[0]
            db.execute("INSERT INTO organization_groups(id,name,parent_id) VALUES(10,'Area',NULL),(11,'Team',10)")
            db.execute("INSERT INTO group_members VALUES(11,?)", (worker_id,))
            db.execute("INSERT INTO jobs(id,code,name,status) VALUES(10,'GROUP-SSO','Group access','active')")
            db.execute("INSERT INTO job_group_grants(job_id,group_id,role_id) VALUES(10,10,2)")
        groups = [{"id": "area-uuid", "parent_id": None, "name": "Area", "local_id": 10, "members": []},
                  {"id": "team-uuid", "parent_id": "area-uuid", "name": "Team", "local_id": 11,
                   "members": ["worker-sub"]}]
        self.publish(groups, {"worker-sub": "worker@example.test"})
        time.sleep(1.05)
        directory = self.get("/api/groups", admin)["groups"]
        self.assertEqual([group["id"] for group in directory], [10, 11])
        config = json.loads(self.request("GET", "/api/auth/config")[2])
        self.assertIs(config["managed_groups"], True)
        for method, path, fields in (("POST", "/api/groups", {"name": "Local forbidden"}),
                                     ("POST", "/api/groups/10", {"name": "Local forbidden"}),
                                     ("POST", "/api/groups/11/members", {"user_id": 1}),
                                     ("DELETE", "/api/groups/11/members/" + str(worker_id), None)):
            self.assertEqual(self.request(method, path, admin, fields,
                                          {"Origin": self.origin})[0], 403)
        status, headers, _ = self.managed_sso(subject="worker-sub", email="worker@example.test", role="worker")
        self.assertEqual(status, 303)
        worker = self.session_cookie(headers)
        self.assertEqual([job["id"] for job in self.get("/api/jobs", worker)["jobs"]], [10])
        groups[0]["name"] = "Renamed area"
        groups[1]["parent_id"] = None
        self.publish(groups)
        time.sleep(1.05)
        self.assertEqual(self.get("/api/jobs", worker)["jobs"], [])
        self.assertEqual(self.get("/api/groups", admin)["groups"][0]["name"], "Renamed area")
        groups[1]["parent_id"] = "area-uuid"
        self.publish(groups)
        time.sleep(1.05)
        self.assertEqual([job["id"] for job in self.get("/api/jobs", worker)["jobs"]], [10])
        self.publish(groups, stale=True)
        time.sleep(1.05)
        self.assertEqual(self.get("/api/jobs", worker)["jobs"], [])
        self.assertEqual(self.request("GET", "/api/groups", admin)[0], 503)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT id FROM organization_groups ORDER BY id").fetchall(), [(10,), (11,)])
            self.assertEqual(db.execute("SELECT group_id FROM job_group_grants").fetchall(), [(10,)])

    def test_new_sso_identity_joins_group_without_email_matching(self):
        admin = self.local_cookie()
        groups = [{"id": "new-group-uuid", "parent_id": None, "name": "New group", "local_id": None,
                   "members": ["new-sso-subject"]}]
        self.publish(groups)
        time.sleep(1.05)
        group_id = self.get("/api/groups", admin)["groups"][0]["id"]
        self.assertEqual(self.get("/api/groups/" + str(group_id) + "/members", admin)["members"], [])
        status, headers, _ = self.managed_sso(subject="new-sso-subject", email="new-worker@example.test", role="worker")
        self.assertEqual(status, 303)
        self.session_cookie(headers)
        members = self.get("/api/groups/" + str(group_id) + "/members", admin)["members"]
        self.assertEqual([member["email"] for member in members], ["new-worker@example.test"])
        with self.db() as db:
            self.assertEqual(db.execute("SELECT issuer,subject FROM oidc_identities").fetchall(),
                             [(self.provider.issuer, "new-sso-subject")])


if __name__ == "__main__":
    unittest.main()
