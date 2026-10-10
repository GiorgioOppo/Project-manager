"""Opt-in organization/access test against a running MySQL deployment.

Set PUNTO_GROUPS_TEST_URL and PUNTO_MYSQL_TEST_EMAIL/PASSWORD (or
PUNTO_EMAIL/PASSWORD). No .env file is loaded. Unique codex-test fixtures
are retained for audit; the new worker is disabled in a finally block.
"""

from datetime import datetime, timezone
import os
import secrets
import unittest
from urllib.parse import urlsplit

import test_procurement_mysql as procurement


TEST_URL = os.environ.get("PUNTO_GROUPS_TEST_URL", "").strip()


@unittest.skipUnless(TEST_URL, "Set PUNTO_GROUPS_TEST_URL to test live MySQL group access")
class GroupMySQLIntegrationTests(unittest.TestCase):
    raw_request = procurement.ProcurementMySQLIntegrationTests.raw_request
    json_request = procurement.ProcurementMySQLIntegrationTests.json_request
    login = procurement.ProcurementMySQLIntegrationTests.login

    def setUp(self):
        self.url = urlsplit(TEST_URL)
        if (self.url.scheme not in ("http", "https") or not self.url.hostname or
                self.url.username or self.url.password or self.url.query or self.url.fragment or
                self.url.path not in ("", "/")):
            self.fail("PUNTO_GROUPS_TEST_URL must be an HTTP(S) origin without credentials")
        email = os.environ.get("PUNTO_MYSQL_TEST_EMAIL") or os.environ.get("PUNTO_EMAIL")
        password = os.environ.get("PUNTO_MYSQL_TEST_PASSWORD") or os.environ.get("PUNTO_PASSWORD")
        if not email or not password:
            self.fail("Set existing manager credentials in the documented environment variables")
        self.origin = "%s://%s" % (self.url.scheme, self.url.netloc)
        self.secret_values = [password]
        self.prefix = "codex-test-g-%s-%s" % (
            datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S"), secrets.token_hex(2))
        health = self.json_request("GET", "/healthz")
        self.assertEqual(health.get("database"), "mysql")
        self.manager = self.login(email, password)

    def post(self, path, fields, expected=201):
        return self.json_request("POST", path, fields, cookie=self.manager, expected=expected)

    def test_inherited_access_readers_role_changes_and_revocation(self):
        worker_id = None
        worker_name = self.prefix + " Operatore"
        try:
            worker_email = self.prefix + "@example.test"
            worker_password = secrets.token_urlsafe(24)
            self.secret_values.append(worker_password)
            worker_id = self.post("/api/users", {"name": worker_name, "email": worker_email,
                "password": worker_password, "role": "worker"})["id"]
            worker = self.login(worker_email, worker_password)
            parent = self.post("/api/groups", {"name": self.prefix + " Area"})["id"]
            child = self.post("/api/groups", {"name": self.prefix + " Team", "parent_id": parent})["id"]
            self.post("/api/groups/%d/members" % child, {"user_id": worker_id})
            job_id = self.post("/api/jobs", {"code": self.prefix + "-A", "name": self.prefix + " Riservata",
                "status": "active"})["id"]
            reader_job = self.post("/api/jobs", {"code": self.prefix + "-B", "name": self.prefix + " Lettura",
                "status": "planned"})["id"]
            self.assertEqual(self.json_request("GET", "/api/jobs", cookie=worker)["jobs"], [])
            self.post("/api/jobs/%d/group-grants" % job_id, {"group_id": parent, "role_id": 2})
            self.post("/api/jobs/%d/assignments" % reader_job, {"user_id": worker_id, "role_id": 1})
            jobs = {row["id"]: row for row in self.json_request("GET", "/api/jobs", cookie=worker)["jobs"]}
            self.assertEqual(set(jobs), {job_id, reader_job})
            self.assertIs(jobs[job_id]["can_log_time"], True)
            self.assertIs(jobs[reader_job]["can_log_time"], False)
            today = datetime.now().astimezone().date().isoformat()
            time_fields = {"job_id": job_id, "date": today, "minutes": 15, "note": "Fixture gruppi MySQL"}
            entry_id = self.json_request("POST", "/api/time-entries", time_fields,
                                         cookie=worker, expected=201)["id"]
            self.json_request("POST", "/api/time-entries", dict(time_fields, job_id=reader_job),
                              cookie=worker, expected=403)
            for path in ("/api/groups", "/api/job-roles", "/api/groups/%d/members" % child,
                         "/api/jobs/%d/group-grants" % job_id):
                self.json_request("GET", path, cookie=worker, expected=403)
                self.json_request("GET", path, expected=401)

            custom = self.post("/api/job-roles", {"code": self.prefix + "-r", "name": "Consultazione test",
                "can_log_time": 0})["id"]
            self.post("/api/jobs/%d/assignments" % job_id, {"user_id": worker_id, "role_id": custom})
            # A weaker direct grant does not cancel the inherited contributor grant.
            jobs = self.json_request("GET", "/api/jobs", cookie=worker)["jobs"]
            self.assertIs(next(row for row in jobs if row["id"] == job_id)["can_log_time"], True)
            self.post("/api/groups/%d" % child, {"parent_id": ""}, expected=200)
            jobs = self.json_request("GET", "/api/jobs", cookie=worker)["jobs"]
            self.assertIs(next(row for row in jobs if row["id"] == job_id)["can_log_time"], False)
            self.json_request("POST", "/api/time-entries", time_fields, cookie=worker, expected=403)
            self.post("/api/job-roles/%d" % custom, {"can_log_time": 1}, expected=200)
            self.json_request("POST", "/api/time-entries", time_fields, cookie=worker, expected=201)
            self.post("/api/job-roles/%d" % custom, {"can_log_time": 0}, expected=200)
            self.json_request("POST", "/api/time-entries", time_fields, cookie=worker, expected=403)
            self.json_request("DELETE", "/api/jobs/%d/assignments/%d" % (job_id, worker_id), cookie=self.manager)
            self.json_request("DELETE", "/api/jobs/%d/assignments/%d" % (reader_job, worker_id), cookie=self.manager)
            self.assertEqual(self.json_request("GET", "/api/jobs", cookie=worker)["jobs"], [])
            history = self.json_request("GET", "/api/time-entries?date=" + today, cookie=worker)
            self.assertIn(entry_id, [row["id"] for row in history["entries"]])
            self.assertEqual(history["total_minutes"], 30)
            for entity in ("group", "group_member", "job_role", "group_grant"):
                events = self.json_request("GET", "/api/audit?entity_type=" + entity, cookie=self.manager)["events"]
                self.assertTrue(events, "Missing organization audit events for " + entity)
        finally:
            if worker_id is not None:
                self.post("/api/users/%d" % worker_id,
                          {"name": worker_name, "role": "worker", "active": 0}, expected=200)


if __name__ == "__main__":
    unittest.main()
