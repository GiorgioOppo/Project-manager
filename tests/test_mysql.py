"""Opt-in HTTP integration smoke test for an already running MySQL deployment.

Start the Compose app, then set PUNTO_MYSQL_TEST_URL to its HTTP origin and
PUNTO_MYSQL_TEST_EMAIL / PUNTO_MYSQL_TEST_PASSWORD to an existing manager.
The credential variables fall back to PUNTO_EMAIL / PUNTO_PASSWORD; no built-in
credentials are assumed. Run with ``python3 tests/test_mysql.py -v``.

The test creates uniquely named codex-test-* records and retains the worker's
history and audit. It never clears tables or alters existing accounts. Set
PUNTO_MYSQL_TEST_MANIFEST to a file path to save non-secret fixture IDs for a
separate persistence check after restarting the containers.
"""

from datetime import datetime, timedelta, timezone
import http.client
import json
import os
from pathlib import Path
import secrets
import unittest
from urllib.parse import urlencode, urlsplit
from zoneinfo import ZoneInfo


TEST_URL = os.environ.get("PUNTO_MYSQL_TEST_URL", "").strip()


@unittest.skipUnless(TEST_URL, "Set PUNTO_MYSQL_TEST_URL to test a running MySQL deployment")
class MySQLIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.url = urlsplit(TEST_URL)
        if (self.url.scheme not in ("http", "https") or not self.url.hostname or
                self.url.username or self.url.password or self.url.query or
                self.url.fragment or self.url.path not in ("", "/")):
            self.fail("PUNTO_MYSQL_TEST_URL must be an HTTP(S) origin without credentials")
        self.email = os.environ.get("PUNTO_MYSQL_TEST_EMAIL") or os.environ.get("PUNTO_EMAIL")
        self.password = os.environ.get("PUNTO_MYSQL_TEST_PASSWORD") or os.environ.get("PUNTO_PASSWORD")
        if not self.email or not self.password:
            self.fail("Set PUNTO_MYSQL_TEST_EMAIL and PUNTO_MYSQL_TEST_PASSWORD, or PUNTO_EMAIL and PUNTO_PASSWORD")
        self.origin = "%s://%s" % (self.url.scheme, self.url.netloc)
        self.secret_values = [self.password]
        self.prefix = "codex-test-%s-%s" % (
            datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S"), secrets.token_hex(3))
        self.worker_email = self.prefix + "@punto.local"
        self.worker_password = secrets.token_urlsafe(24)
        self.secret_values.append(self.worker_password)
        health = self.request("GET", "/healthz")
        self.assertIs(health.get("ok"), True)
        self.assertEqual(health.get("database"), "mysql", "This test requires the real MySQL backend")
        self.manager = self.login(self.email, self.password)
        session = self.request("GET", "/api/session", cookie=self.manager)
        self.assertIs(session.get("authenticated"), True)
        self.assertEqual(session.get("role"), "manager")

    def request(self, method, path, fields=None, *, cookie=None, expected=200):
        headers = {"Accept": "application/json"}
        if cookie:
            headers["Cookie"] = cookie
        body = None
        if method in ("POST", "DELETE"):
            headers["Origin"] = self.origin
        if fields is not None:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
            body = urlencode(fields)
        connection_type = (http.client.HTTPSConnection if self.url.scheme == "https"
                           else http.client.HTTPConnection)
        connection = connection_type(self.url.hostname, self.url.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            status = response.status
            response_headers = dict(response.getheaders())
        finally:
            connection.close()
        # Avoid including a secret in assertion diagnostics if a regression leaks it.
        self.assertTrue(all(secret.encode("utf-8") not in payload for secret in self.secret_values),
                        "An API response exposed a credential")
        self.assertIn("application/json", response_headers.get("Content-Type", ""))
        try:
            data = json.loads(payload)
        except (UnicodeError, ValueError):
            self.fail("API did not return valid JSON")
        self.assertEqual(status, expected, {"method": method, "path": path, "response": data})
        if expected >= 400:
            self.assertIs(data.get("ok"), False)
            self.assertTrue(data.get("error"))
        if path == "/api/login" and expected == 200:
            self.assertIs(data.get("ok"), True)
            self.assertIn("Set-Cookie", response_headers)
            return response_headers["Set-Cookie"].split(";", 1)[0]
        return data

    def login(self, email, password):
        return self.request("POST", "/api/login", {"email": email, "password": password})

    def jobs(self, cookie):
        return self.request("GET", "/api/jobs", cookie=cookie)["jobs"]

    def entries(self, cookie, date):
        data = self.request("GET", "/api/time-entries?" + urlencode({"date": date}), cookie=cookie)
        self.assertEqual(data["total_minutes"], sum(entry["minutes"] for entry in data["entries"]))
        return data["entries"]

    def audit(self):
        # Read the newest fixture events without assuming the database is empty.
        data = self.request("GET", "/api/audit?limit=100", cookie=self.manager)
        ids = [event["id"] for event in data["events"]]
        self.assertEqual(ids, sorted(ids, reverse=True))
        self.assertEqual(len(ids), len(set(ids)))
        return data["events"]

    def assert_no_credential_fields(self, value):
        if isinstance(value, dict):
            for key, item in value.items():
                self.assertFalse(any(part in key.lower() for part in ("password", "hash", "salt")),
                                 "Audit includes a credential field")
                self.assert_no_credential_fields(item)
        elif isinstance(value, list):
            for item in value:
                self.assert_no_credential_fields(item)

    def test_mysql_commesse_users_assignments_timesheets_and_audit(self):
        users = self.request("GET", "/api/users", cookie=self.manager)["users"]
        manager_id = next(user["id"] for user in users if user["email"].lower() == self.email.lower())
        user = self.request("POST", "/api/users", {
            "name": self.prefix + " Operatore qualità ✓", "email": self.worker_email,
            "password": self.worker_password, "role": "worker"}, cookie=self.manager, expected=201)
        worker_id = user["id"]
        self.assertIsInstance(worker_id, int)
        self.request("POST", "/api/users", {
            "name": "Duplicato di prova", "email": self.worker_email.upper(),
            "password": self.worker_password, "role": "worker"}, cookie=self.manager, expected=409)
        worker = self.login(self.worker_email, self.worker_password)
        worker_session = self.request("GET", "/api/session", cookie=worker)
        self.assertEqual(worker_session["email"], self.worker_email)
        self.assertEqual(worker_session["role"], "worker")
        self.assertFalse(worker_session["permissions"]["manage_jobs"])
        self.assertFalse(worker_session["permissions"]["view_audit"])
        self.assertEqual(self.jobs(worker), [])

        name = self.prefix + " Commessa qualità 🚀"
        unicode_pattern = 'Qualità 🚀 日本語 "note";\n'
        long_text = (unicode_pattern * (1000 // len(unicode_pattern) + 1))[:1000]
        self.assertEqual(len(long_text), 1000)
        self.assertGreater(len(long_text.encode("utf-8")), 1000)
        job_fields = {"code": self.prefix + "-A", "name": name, "client": "Cliente prova è",
                      "description": long_text,
                      "status": "active"}
        job_id = self.request("POST", "/api/jobs", job_fields,
                              cookie=self.manager, expected=201)["id"]
        self.request("POST", "/api/jobs", dict(job_fields, code=job_fields["code"].upper()),
                     cookie=self.manager, expected=409)
        self.request("POST", "/api/jobs/%d" % job_id, dict(job_fields, description=long_text + "界"),
                     cookie=self.manager, expected=400)
        private_job = self.request("POST", "/api/jobs", {
            "code": self.prefix + "-B", "name": self.prefix + " Non assegnata", "status": "planned"},
            cookie=self.manager, expected=201)["id"]
        self.assertEqual(self.jobs(worker), [])
        self.request("POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": worker_id, "starts_on": "", "ends_on": ""}, cookie=self.manager, expected=201)
        assignments = self.request("GET", "/api/jobs/%d/assignments" % job_id,
                                   cookie=self.manager)["assignments"]
        self.assertEqual([assignment["user_id"] for assignment in assignments], [worker_id])
        self.assertEqual(assignments[0]["email"], self.worker_email)
        assigned_jobs = self.jobs(worker)
        self.assertEqual([job["id"] for job in assigned_jobs], [job_id])
        self.assertEqual(assigned_jobs[0]["name"], name)
        self.assertEqual(assigned_jobs[0]["description"], job_fields["description"])

        # An unusual valid date avoids interfering with the manager's normal daily total.
        work_date = "2080-%02d-%02d" % (1 + secrets.randbelow(12), 1 + secrets.randbelow(28))
        note = long_text
        time_fields = {"job_id": job_id, "date": work_date, "minutes": 60, "note": note}
        worker_entry = self.request("POST", "/api/time-entries", time_fields,
                                    cookie=worker, expected=201)["id"]
        worker_entries = self.entries(worker, work_date)
        self.assertEqual(len(worker_entries), 1)
        self.assertEqual(worker_entries[0]["id"], worker_entry)
        self.assertEqual(worker_entries[0]["user_id"], worker_id)
        self.assertEqual(worker_entries[0]["owner"], self.worker_email)
        self.assertEqual(worker_entries[0]["note"], note)
        self.assertEqual(worker_entries[0]["minutes"], 60)
        self.assertFalse(any(entry["id"] == worker_entry for entry in self.entries(self.manager, work_date)))
        self.assertEqual(self.jobs(worker)[0]["total_minutes"], 60)
        self.request("POST", "/api/time-entries", dict(time_fields, job_id=private_job),
                     cookie=worker, expected=403)
        self.request("GET", "/api/audit", cookie=worker, expected=403)
        self.request("POST", "/api/time-entries", dict(time_fields, user_id=manager_id),
                     cookie=worker, expected=400)
        self.request("POST", "/api/time-entries", dict(time_fields, note=note + "界"),
                     cookie=worker, expected=400)
        self.request("POST", "/api/time-entries/%d" % worker_entry, dict(time_fields, minutes=30),
                     cookie=worker, expected=403)
        self.request("DELETE", "/api/time-entries/%d" % worker_entry, cookie=worker, expected=403)
        self.request("POST", "/api/jobs/%d" % job_id, dict(job_fields, status="completed"),
                     cookie=worker, expected=403)
        self.request("POST", "/api/jobs", dict(job_fields, code=self.prefix + "-DENIED"),
                     cookie=worker, expected=403)

        today = datetime.now(ZoneInfo("Europe/Rome")).date()
        starts_on = (today - timedelta(days=1)).isoformat()
        ends_on = (today + timedelta(days=1)).isoformat()
        self.request("POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": worker_id, "starts_on": starts_on, "ends_on": ends_on}, cookie=self.manager)
        bounded = self.request("GET", "/api/jobs/%d/assignments" % job_id,
                               cookie=self.manager)["assignments"]
        self.assertEqual(len(bounded), 1)
        self.assertEqual(bounded[0]["starts_on"], starts_on)
        self.assertEqual(bounded[0]["ends_on"], ends_on)
        self.assertEqual([job["id"] for job in self.jobs(worker)], [job_id])
        self.request("POST", "/api/time-entries", time_fields, cookie=worker, expected=403)
        # Omitting the optional dates clears both boundaries on the same assignment.
        self.request("POST", "/api/jobs/%d/assignments" % job_id, {"user_id": worker_id}, cookie=self.manager)
        unbounded = self.request("GET", "/api/jobs/%d/assignments" % job_id,
                                 cookie=self.manager)["assignments"]
        self.assertEqual(len(unbounded), 1)
        self.assertEqual(unbounded[0]["starts_on"], "")
        self.assertEqual(unbounded[0]["ends_on"], "")

        self.request("POST", "/api/jobs/%d" % job_id, dict(job_fields, status="paused"), cookie=self.manager)
        self.assertEqual(self.jobs(worker)[0]["status"], "paused")
        manager_entry = self.request("POST", "/api/time-entries", dict(time_fields, minutes=15),
                                     cookie=self.manager, expected=201)["id"]
        self.request("POST", "/api/time-entries/%d" % manager_entry,
                     dict(time_fields, minutes=30, note="Correzione prova ✓"), cookie=self.manager)
        self.assertEqual(next(entry for entry in self.entries(self.manager, work_date)
                              if entry["id"] == manager_entry)["minutes"], 30)
        self.request("DELETE", "/api/time-entries/%d" % manager_entry, cookie=self.manager)
        self.assertFalse(any(entry["id"] == manager_entry for entry in self.entries(self.manager, work_date)))

        # Dates use the same Gregorian range as SQLite, including years before 1000.
        ancient_fields = dict(time_fields, date="0001-01-01", minutes=1, note="Confine calendario")
        ancient_entry = self.request("POST", "/api/time-entries", ancient_fields,
                                     cookie=self.manager, expected=201)["id"]
        self.assertEqual(next(entry for entry in self.entries(self.manager, "0001-01-01")
                              if entry["id"] == ancient_entry)["date"], "0001-01-01")
        self.request("POST", "/api/time-entries/%d" % ancient_entry,
                     dict(ancient_fields, date="0001-01-02", minutes=2), cookie=self.manager)
        self.assertEqual(next(entry for entry in self.entries(self.manager, "0001-01-02")
                              if entry["id"] == ancient_entry)["minutes"], 2)
        self.request("DELETE", "/api/time-entries/%d" % ancient_entry, cookie=self.manager)
        self.assertFalse(any(entry["id"] == ancient_entry for entry in self.entries(self.manager, "0001-01-02")))

        self.request("DELETE", "/api/jobs/%d/assignments/%d" % (job_id, worker_id), cookie=self.manager)
        self.assertEqual(self.jobs(worker), [])
        self.request("POST", "/api/time-entries", time_fields, cookie=worker, expected=403)
        retained = self.entries(worker, work_date)
        self.assertEqual([entry["id"] for entry in retained], [worker_entry])
        self.assertEqual(retained[0]["minutes"], 60)

        events = self.audit()
        self.assert_no_credential_fields(events)
        worker_create = next(event for event in events if event["action"] == "time_entries.create"
                             and event["entity_id"] == worker_entry)
        self.assertEqual(worker_create["actor_id"], worker_id)
        self.assertEqual(worker_create["actor_email"], self.worker_email)
        self.assertIsNone(worker_create["before"])
        self.assertEqual(worker_create["after"]["user_id"], worker_id)
        self.assertEqual(worker_create["after"]["note"], note)
        job_update = next(event for event in events if event["action"] == "jobs.update"
                          and event["entity_id"] == job_id)
        self.assertEqual(job_update["actor_id"], manager_id)
        self.assertEqual(job_update["before"]["status"], "active")
        self.assertEqual(job_update["after"]["status"], "paused")
        time_update = next(event for event in events if event["action"] == "time_entries.update"
                           and event["entity_id"] == manager_entry)
        self.assertEqual(time_update["before"]["minutes"], 15)
        self.assertEqual(time_update["after"]["minutes"], 30)
        time_delete = next(event for event in events if event["action"] == "time_entries.delete"
                           and event["entity_id"] == manager_entry)
        self.assertEqual(time_delete["before"]["minutes"], 30)
        self.assertIsNone(time_delete["after"])
        revocation = next(event for event in events if event["action"] == "assignments.delete"
                          and event["before"]["job_id"] == job_id
                          and event["before"]["user_id"] == worker_id)
        self.assertEqual(revocation["actor_id"], manager_id)
        self.assertIsNone(revocation["after"])
        self.assertTrue(any(event["action"] == "users.create" and event["entity_id"] == worker_id
                            for event in events))

        manifest = os.environ.get("PUNTO_MYSQL_TEST_MANIFEST")
        if manifest:
            Path(manifest).write_text(json.dumps({
                "prefix": self.prefix, "job_id": job_id, "unassigned_job_id": private_job,
                "worker_id": worker_id, "worker_email": self.worker_email,
                "worker_entry_id": worker_entry, "work_date": work_date,
                "worker_note": note, "audit_event_ids": [worker_create["id"], job_update["id"], revocation["id"]],
            }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
