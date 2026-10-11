"""Isolated HTTP lifecycle tests for jobs and daily timesheets (stdlib only)."""

import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
EMAIL = "timesheet@punto.local"
PASSWORD = "Timesheet-only!2026"


class WorkspaceIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.binary = ROOT / "build" / "punto"
        if not self.binary.is_file():
            raise RuntimeError("Missing build/punto: run make before the tests")
        self.fixture = tempfile.TemporaryDirectory(prefix="punto-workspace-tests-")
        self.addCleanup(self.fixture.cleanup)
        self.public = Path(self.fixture.name) / "public"
        self.public.mkdir()
        for name in ("index.html", "app.html"):
            (self.public / name).write_text("<html>test fixture</html>", encoding="utf-8")
        self.db_path = Path(self.fixture.name) / "workspace.db"
        self.server = None
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.addCleanup(self.log.close)
        self.addCleanup(self.stop_server)
        self.start_server()
        self.cookie = self.login()

    def start_server(self, email=EMAIL):
        self.email = email
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            self.port = reserved.getsockname()[1]
        self.origin = "http://127.0.0.1:%d" % self.port
        env = dict(os.environ, PORT=str(self.port), PUBLIC_DIR=str(self.public),
                   PUNTO_EMAIL=email, PUNTO_PASSWORD=PASSWORD, DB_PATH=str(self.db_path))
        self.server = subprocess.Popen([str(self.binary)], cwd=ROOT, env=env,
                                       stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                self.log.seek(0)
                self.fail("Server exited during startup: " +
                          self.log.read().decode("utf-8", "replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.025)
        self.fail("Server did not listen within five seconds")

    def stop_server(self):
        if self.server is not None and self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=3)

    def restart_server(self, email=EMAIL):
        self.stop_server()
        self.start_server(email)
        self.cookie = self.login()

    def api(self, method, path, fields=None, *, authenticated=True, headers=None,
            body=None):
        request_headers = {}
        if authenticated:
            request_headers["Cookie"] = self.cookie
        if method in ("POST", "DELETE"):
            request_headers["Origin"] = self.origin
        if fields is not None:
            body = urlencode(fields)
            request_headers["Content-Type"] = "application/x-www-form-urlencoded"
        request_headers.update(headers or {})
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            payload = response.read()
            response_headers = dict(response.getheaders())
            status = response.status
        finally:
            connection.close()
        self.assertIn("application/json", response_headers.get("Content-Type", ""),
                      payload.decode("utf-8", "replace"))
        self.assertNotIn(PASSWORD.encode(), payload)
        return status, json.loads(payload), response_headers

    def login(self):
        status, data, headers = self.api("POST", "/api/login", {
            "email": self.email, "password": PASSWORD}, authenticated=False)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        return headers["Set-Cookie"].split(";", 1)[0]

    def job_fields(self, **overrides):
        fields = {"code": "CM-001", "name": "Sito cliente", "client": "Acme",
                  "description": "Sviluppo della nuova area clienti", "status": "planned"}
        fields.update(overrides)
        return fields

    def create_job(self, **overrides):
        status, data, _ = self.api("POST", "/api/jobs", self.job_fields(**overrides))
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        self.assertIsInstance(data.get("id"), int)
        self.assertGreater(data["id"], 0)
        return data["id"]

    def jobs(self):
        status, data, _ = self.api("GET", "/api/jobs")
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data.get("jobs"), list)
        return data["jobs"]

    def time_fields(self, job_id, **overrides):
        fields = {"job_id": job_id, "date": "2026-10-10", "minutes": 90,
                  "note": "Analisi e sviluppo"}
        fields.update(overrides)
        return fields

    def create_time(self, job_id, **overrides):
        status, data, _ = self.api("POST", "/api/time-entries",
                                   self.time_fields(job_id, **overrides))
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        self.assertIsInstance(data.get("id"), int)
        self.assertGreater(data["id"], 0)
        return data["id"]

    def day(self, date="2026-10-10"):
        status, data, _ = self.api("GET", "/api/time-entries?" + urlencode({"date": date}))
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data.get("entries"), list)
        self.assertIsInstance(data.get("total_minutes"), int)
        self.assertEqual(data["total_minutes"], sum(entry["minutes"] for entry in data["entries"]))
        return data

    def assert_error(self, expected, method, path, fields=None, **kwargs):
        status, data, _ = self.api(method, path, fields, **kwargs)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), False)
        self.assertTrue(data.get("error"))

    def test_create_job_and_change_every_status(self):
        self.assertEqual(self.jobs(), [])
        job_id = self.create_job()
        created = self.jobs()[0]
        for name, value in self.job_fields().items():
            self.assertEqual(created[name], value)
        self.assertEqual(created["id"], job_id)
        self.assertEqual(created["total_minutes"], 0)
        for state in ("active", "paused", "completed", "cancelled", "planned"):
            with self.subTest(state=state):
                fields = self.job_fields(code="CM-RENAMED", name="Portale aggiornato",
                                         client="Nuovo cliente", description="Nuovo ambito",
                                         status=state)
                status, data, _ = self.api("POST", "/api/jobs/%d" % job_id, fields)
                self.assertEqual(status, 200, data)
                self.assertIs(data.get("ok"), True)
                self.assertEqual(len(self.jobs()), 1)
                for name, value in fields.items():
                    self.assertEqual(self.jobs()[0][name], value)

    def test_job_code_is_unique_case_insensitively_on_create_and_edit(self):
        first = self.create_job(code="Unique-42")
        self.assert_error(409, "POST", "/api/jobs", self.job_fields(code="uNIQUE-42"))
        second = self.create_job(code="CM-002")
        self.assert_error(409, "POST", "/api/jobs/%d" % second,
                          self.job_fields(code="UNIQUE-42"))
        by_id = {job["id"]: job for job in self.jobs()}
        self.assertEqual(by_id[first]["code"], "Unique-42")
        self.assertEqual(by_id[second]["code"], "CM-002")

    def test_job_required_fields_and_lengths_are_validated(self):
        cases = ({"code": ""}, {"name": ""}, {"status": "unknown"},
                 {"code": "x" * 41}, {"name": "x" * 121},
                 {"client": "x" * 121}, {"description": "x" * 1001})
        for changes in cases:
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/jobs", self.job_fields(**changes))
        for missing in ("code", "name", "status"):
            with self.subTest(missing=missing):
                fields = self.job_fields()
                del fields[missing]
                self.assert_error(400, "POST", "/api/jobs", fields)
        self.assertEqual(self.jobs(), [])
        self.create_job(code="c" * 40, name="n" * 120, client="a" * 120,
                        description="d" * 1000)

    def test_optional_job_fields_can_be_omitted_or_cleared(self):
        status, data, _ = self.api("POST", "/api/jobs", {
            "code": "SHORT", "name": "Commessa essenziale", "status": "active"})
        self.assertEqual(status, 201, data)
        self.assertEqual(self.jobs()[0]["client"], "")
        self.assertEqual(self.jobs()[0]["description"], "")
        status, data, _ = self.api("POST", "/api/jobs/%d" % data["id"],
                                   self.job_fields(client="", description=""))
        self.assertEqual(status, 200, data)
        self.assertEqual(self.jobs()[0]["client"], "")
        self.assertEqual(self.jobs()[0]["description"], "")

    def test_daily_time_lifecycle_updates_both_day_and_job_totals(self):
        first = self.create_job()
        second = self.create_job(code="CM-002", name="App mobile")
        entry = self.create_time(first, minutes=90)
        second_entry = self.create_time(second, minutes=30)
        day = self.day()
        self.assertEqual(day["total_minutes"], 120)
        by_id = {item["id"]: item for item in day["entries"]}
        self.assertEqual(by_id[entry]["job_id"], first)
        self.assertEqual(by_id[entry]["job_code"], "CM-001")
        self.assertEqual(by_id[entry]["job_name"], "Sito cliente")
        self.assertEqual(by_id[entry]["date"], "2026-10-10")
        self.assertEqual(by_id[entry]["note"], "Analisi e sviluppo")
        self.assertEqual(by_id[entry]["owner"], EMAIL)
        status, data, _ = self.api("POST", "/api/time-entries/%d" % entry,
                                   self.time_fields(second, minutes=45, note="Revisione"))
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        day = self.day()
        self.assertEqual(day["total_minutes"], 75)
        updated = next(item for item in day["entries"] if item["id"] == entry)
        self.assertEqual(updated["job_id"], second)
        self.assertEqual(updated["note"], "Revisione")
        jobs = {job["id"]: job for job in self.jobs()}
        self.assertEqual(jobs[first]["total_minutes"], 0)
        self.assertEqual(jobs[second]["total_minutes"], 75)
        status, data, _ = self.api("DELETE", "/api/time-entries/%d" % second_entry)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        self.assertEqual(self.day()["total_minutes"], 45)
        self.assert_error(404, "DELETE", "/api/time-entries/%d" % second_entry)

    def test_timesheets_are_partitioned_by_selected_day(self):
        job_id = self.create_job()
        self.create_time(job_id, minutes=60, date="2026-10-09")
        self.create_time(job_id, minutes=120, date="2026-10-10")
        self.assertEqual(self.day("2026-10-09")["total_minutes"], 60)
        self.assertEqual(self.day("2026-10-10")["total_minutes"], 120)
        empty_day = self.day("2026-10-11")
        self.assertEqual(empty_day["entries"], [])
        self.assertEqual(empty_day["total_minutes"], 0)
        self.assertEqual(self.jobs()[0]["total_minutes"], 180)

    def test_daily_limit_is_enforced_and_edit_subtracts_the_old_entry(self):
        first = self.create_job()
        second = self.create_job(code="CM-002")
        entry = self.create_time(first, minutes=1000)
        self.create_time(second, minutes=440)
        self.assert_error(409, "POST", "/api/time-entries", self.time_fields(first, minutes=1))
        self.assert_error(409, "POST", "/api/time-entries/%d" % entry,
                          self.time_fields(first, minutes=1001))
        status, data, _ = self.api("POST", "/api/time-entries/%d" % entry,
                                   self.time_fields(first, minutes=1000))
        self.assertEqual(status, 200, data)
        self.assertEqual(self.day()["total_minutes"], 1440)
        self.create_time(first, minutes=1440, date="2026-10-11")
        self.assert_error(409, "POST", "/api/time-entries/%d" % entry,
                          self.time_fields(first, date="2026-10-11", minutes=1000))
        self.assertEqual(self.day()["total_minutes"], 1440)
        status, data, _ = self.api("POST", "/api/time-entries/%d" % entry,
                                   self.time_fields(first, date="2026-10-12", minutes=1000))
        self.assertEqual(status, 200, data)
        self.assertEqual(self.day()["total_minutes"], 440)
        self.assertEqual(self.day("2026-10-12")["total_minutes"], 1000)

    def test_calendar_dates_include_correct_gregorian_leap_years(self):
        job_id = self.create_job()
        invalid = ("2026-02-29", "1900-02-29", "2026-04-31", "2026-00-10",
                   "2026-13-01", "2026-01-00", "0000-01-01", "2026-10-1",
                   "26-10-10", "2026/10/10", "2026-10-10suffix", "")
        for date in invalid:
            with self.subTest(date=date):
                self.assert_error(400, "POST", "/api/time-entries",
                                  self.time_fields(job_id, date=date))
                self.assert_error(400, "GET", "/api/time-entries?" + urlencode({"date": date}))
        for date in ("2000-02-29", "2024-02-29", "2026-01-31", "2026-04-30"):
            with self.subTest(date=date):
                self.create_time(job_id, date=date, minutes=1)
                self.assertEqual(self.day(date)["total_minutes"], 1)
        self.assert_error(400, "GET", "/api/time-entries")

    def test_minutes_and_note_lengths_are_validated(self):
        job_id = self.create_job()
        for minutes in (0, -1, 1441, "1.5", "hello", "", "90minutes"):
            with self.subTest(minutes=minutes):
                self.assert_error(400, "POST", "/api/time-entries",
                                  self.time_fields(job_id, minutes=minutes))
        self.assert_error(400, "POST", "/api/time-entries",
                          self.time_fields(job_id, note="x" * 1001))
        for missing in ("job_id", "date", "minutes"):
            with self.subTest(missing=missing):
                fields = self.time_fields(job_id)
                del fields[missing]
                self.assert_error(400, "POST", "/api/time-entries", fields)
        self.create_time(job_id, minutes=1, note="n" * 1000)
        status, data, _ = self.api("POST", "/api/time-entries", {
            "job_id": job_id, "date": "2026-10-11", "minutes": 1})
        self.assertEqual(status, 201, data)
        self.assertEqual(self.day("2026-10-11")["entries"][0]["note"], "")

    def test_unknown_job_and_entry_ids_do_not_mutate_data(self):
        job_id = self.create_job()
        entry = self.create_time(job_id)
        self.assert_error(404, "POST", "/api/jobs/999999", self.job_fields())
        self.assert_error(404, "POST", "/api/time-entries", self.time_fields(999999))
        self.assert_error(404, "POST", "/api/time-entries/%d" % entry,
                          self.time_fields(999999))
        self.assert_error(404, "POST", "/api/time-entries/999999", self.time_fields(job_id))
        self.assert_error(404, "DELETE", "/api/time-entries/999999")
        self.assertEqual(len(self.jobs()), 1)
        self.assertEqual(self.day()["total_minutes"], 90)

    def test_all_workspace_routes_require_authentication(self):
        job_id = self.create_job()
        entry = self.create_time(job_id)
        routes = (("GET", "/api/jobs", None),
                  ("POST", "/api/jobs", self.job_fields()),
                  ("POST", "/api/jobs/%d" % job_id, self.job_fields()),
                  ("GET", "/api/time-entries?date=2026-10-10", None),
                  ("POST", "/api/time-entries", self.time_fields(job_id)),
                  ("POST", "/api/time-entries/%d" % entry, self.time_fields(job_id)),
                  ("DELETE", "/api/time-entries/%d" % entry, None))
        for method, path, fields in routes:
            for cookie_headers in ({}, {"Cookie": "punto_session=forged"}):
                with self.subTest(method=method, path=path, cookie=cookie_headers):
                    self.assert_error(401, method, path, fields, authenticated=False,
                                      headers=cookie_headers)
        self.assertEqual(self.day()["total_minutes"], 90)

    def test_workspace_writes_require_same_origin_and_form_content(self):
        job_id = self.create_job()
        entry = self.create_time(job_id)
        routes = (("POST", "/api/jobs", self.job_fields(code="CM-002")),
                  ("POST", "/api/jobs/%d" % job_id, self.job_fields()),
                  ("POST", "/api/time-entries", self.time_fields(job_id)),
                  ("POST", "/api/time-entries/%d" % entry, self.time_fields(job_id)),
                  ("DELETE", "/api/time-entries/%d" % entry, None))
        for method, path, fields in routes:
            with self.subTest(method=method, path=path):
                self.assert_error(403, method, path, fields,
                                  headers={"Origin": "https://attacker.example"})
                if method == "POST":
                    self.assert_error(415, method, path, headers={
                        "Content-Type": "application/json"}, body=json.dumps(fields))
        self.assertEqual(len(self.jobs()), 1)
        self.assertEqual(self.day()["total_minutes"], 90)

    def test_quoted_unicode_multiline_and_sql_like_text_round_trip(self):
        code = "SQL-'42"
        name = 'Progetto "Qualità" & ricerca'
        client = "L'azienda + partner"
        description = "Prima riga\nSeconda riga: \\\" <prova>\n'); DROP TABLE jobs; --"
        note = "Lavoro completato ✓\nQuote: \" e backslash: \\\n<script>alert('test')</script>"
        job_id = self.create_job(code=code, name=name, client=client, description=description)
        job = self.jobs()[0]
        self.assertEqual((job["code"], job["name"], job["client"], job["description"]),
                         (code, name, client, description))
        self.create_time(job_id, note=note)
        self.assertEqual(self.day()["entries"][0]["note"], note)
        self.create_job(code="TABLE-SURVIVED")
        self.assertEqual(len(self.jobs()), 2)
        # Percent encoding expands each Unicode character into 9 or 12 bytes.
        # Valid 1,000-character fields must survive both create and edit requests.
        boundary_texts = ("界" * 1000, "🚀" * 1000)
        for index, text in enumerate(boundary_texts):
            with self.subTest(boundary=index):
                boundary_code = "MAX-UTF8-%d" % index
                boundary_job = self.create_job(code=boundary_code, description=text)
                created_job = next(job for job in self.jobs() if job["id"] == boundary_job)
                self.assertEqual(created_job["description"], text)
                boundary_entry = self.create_time(boundary_job, note=text)
                created_entry = next(item for item in self.day()["entries"]
                                     if item["id"] == boundary_entry)
                self.assertEqual(created_entry["note"], text)
                updated_text = boundary_texts[1 - index]
                status, data, _ = self.api("POST", "/api/jobs/%d" % boundary_job,
                                           self.job_fields(code=boundary_code,
                                                           description=updated_text))
                self.assertEqual(status, 200, data)
                updated_job = next(job for job in self.jobs() if job["id"] == boundary_job)
                self.assertEqual(updated_job["description"], updated_text)
                status, data, _ = self.api("POST", "/api/time-entries/%d" % boundary_entry,
                                           self.time_fields(boundary_job, note=updated_text))
                self.assertEqual(status, 200, data)
                updated_entry = next(item for item in self.day()["entries"]
                                     if item["id"] == boundary_entry)
                self.assertEqual(updated_entry["note"], updated_text)

    def test_jobs_and_timesheets_persist_but_sessions_end_after_restart(self):
        job_id = self.create_job(status="paused")
        entry = self.create_time(job_id, minutes=75, note="Da conservare")
        old_cookie = self.cookie
        self.stop_server()
        self.start_server()
        self.assert_error(401, "GET", "/api/jobs", authenticated=False,
                          headers={"Cookie": old_cookie})
        self.cookie = self.login()
        self.assertEqual(self.jobs()[0]["id"], job_id)
        self.assertEqual(self.jobs()[0]["status"], "paused")
        self.assertEqual(self.jobs()[0]["total_minutes"], 75)
        self.assertEqual(self.day()["entries"][0]["id"], entry)
        self.assertEqual(self.day()["entries"][0]["note"], "Da conservare")

    def test_users_share_jobs_but_cannot_read_or_change_others_timesheets(self):
        job_id = self.create_job()
        first_entry = self.create_time(job_id, minutes=1440, note="Privato primo utente")
        status, data, _ = self.api("POST", "/api/users", {
            "name": "Secondo gestore", "email": "second@punto.local",
            "password": PASSWORD, "role": "manager"})
        self.assertEqual(status, 201, data)
        status, result, _ = self.api("POST", "/api/improvements/governance/admins", {
            "user_id": data["id"], "admin": "1", "reason": "Explicit test appointment"})
        self.assertEqual(status, 200, result)
        self.restart_server("second@punto.local")
        self.assertEqual(self.jobs()[0]["id"], job_id)
        empty_day = self.day()
        self.assertEqual(empty_day["entries"], [])
        self.assertEqual(empty_day["total_minutes"], 0)
        self.assert_error(404, "POST", "/api/time-entries/%d" % first_entry,
                          self.time_fields(job_id, note="Non autorizzato"))
        self.assert_error(404, "DELETE", "/api/time-entries/%d" % first_entry)
        second_entry = self.create_time(job_id, minutes=60, note="Secondo utente")
        self.assertEqual(self.day()["entries"][0]["owner"], "second@punto.local")
        self.assertEqual(self.day()["total_minutes"], 60)
        self.restart_server()
        day = self.day()
        self.assertEqual(day["total_minutes"], 1440)
        self.assertEqual([item["id"] for item in day["entries"]], [first_entry])
        self.assertEqual(day["entries"][0]["note"], "Privato primo utente")
        self.assert_error(404, "DELETE", "/api/time-entries/%d" % second_entry)


if __name__ == "__main__":
    unittest.main()
