"""Persistent accounts and server-enforced role permissions, over real HTTP."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
MANAGER_EMAIL = "manager@punto.local"
MANAGER_PASSWORD = "Manager-only!2026"
WORKER_EMAIL = "worker@punto.local"
WORKER_PASSWORD = "Worker-only!2026"


class RoleIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.binary = ROOT / "build" / "punto"
        if not self.binary.is_file():
            raise RuntimeError("Missing build/punto: run make before the tests")
        self.fixture = tempfile.TemporaryDirectory(prefix="punto-roles-tests-")
        self.addCleanup(self.fixture.cleanup)
        self.public = Path(self.fixture.name) / "public"
        self.public.mkdir()
        for name in ("index.html", "app.html"):
            (self.public / name).write_text("<html>roles fixture</html>", encoding="utf-8")
        self.db_path = Path(self.fixture.name) / "accounts.db"
        self.server = None
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.addCleanup(self.log.close)
        self.addCleanup(self.stop_server)
        self.secrets = {MANAGER_PASSWORD, WORKER_PASSWORD}
        self.start_server()
        self.manager_cookie = self.login(MANAGER_EMAIL, MANAGER_PASSWORD)

    def start_server(self, email=MANAGER_EMAIL, password=MANAGER_PASSWORD):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            self.port = reserved.getsockname()[1]
        self.origin = "http://127.0.0.1:%d" % self.port
        env = dict(os.environ, PORT=str(self.port), PUBLIC_DIR=str(self.public),
                   PUNTO_EMAIL=email, PUNTO_PASSWORD=password, DB_PATH=str(self.db_path))
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

    def api(self, method, path, fields=None, *, cookie=None, headers=None, body=None):
        request_headers = {}
        if cookie is not None:
            request_headers["Cookie"] = cookie
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
            status, response_headers = response.status, dict(response.getheaders())
        finally:
            connection.close()
        self.assertIn("application/json", response_headers.get("Content-Type", ""),
                      payload.decode("utf-8", "replace"))
        for secret in self.secrets:
            self.assertNotIn(secret.encode(), payload, "A response leaked a password")
        return status, json.loads(payload), response_headers

    def login(self, email, password):
        status, data, headers = self.api("POST", "/api/login", {
            "email": email, "password": password})
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        return headers["Set-Cookie"].split(";", 1)[0]

    def assert_error(self, expected, method, path, fields=None, **kwargs):
        status, data, _ = self.api(method, path, fields, **kwargs)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), False)
        self.assertTrue(data.get("error"))

    def create_user(self, email=WORKER_EMAIL, password=WORKER_PASSWORD,
                    name="Operatore", role="worker"):
        self.secrets.add(password)
        status, data, _ = self.api("POST", "/api/users", {
            "name": name, "email": email, "password": password, "role": role},
            cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        self.assertIsInstance(data.get("id"), int)
        self.assertGreater(data["id"], 0)
        return data["id"]

    def users(self):
        status, data, _ = self.api("GET", "/api/users", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data.get("users"), list)
        return data["users"]

    def edit_user(self, user_id, name="Operatore", role="worker", active="1"):
        return self.api("POST", "/api/users/%d" % user_id, {
            "name": name, "role": role, "active": active}, cookie=self.manager_cookie)

    def session(self, cookie):
        status, data, _ = self.api("GET", "/api/session", cookie=cookie)
        self.assertEqual(status, 200, data)
        return data

    def create_job(self):
        status, data, _ = self.api("POST", "/api/jobs", {
            "code": "ROLE-001", "name": "Commessa condivisa", "status": "active"},
            cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        return data["id"]

    def assign_job(self, job_id, user_id):
        status, data, _ = self.api("POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": user_id, "starts_on": "", "ends_on": ""},
            cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)

    def time_fields(self, job_id, **changes):
        fields = {"job_id": job_id, "date": "2026-10-10", "minutes": 60,
                  "note": "Attività svolta"}
        fields.update(changes)
        return fields

    def create_time(self, cookie, job_id, **changes):
        status, data, _ = self.api("POST", "/api/time-entries",
                                   self.time_fields(job_id, **changes), cookie=cookie)
        self.assertEqual(status, 201, data)
        return data["id"]

    def day(self, cookie):
        status, data, _ = self.api("GET", "/api/time-entries?date=2026-10-10", cookie=cookie)
        self.assertEqual(status, 200, data)
        return data

    def test_bootstrap_manager_has_identity_and_explicit_permissions(self):
        session = self.session(self.manager_cookie)
        self.assertIs(session.get("authenticated"), True)
        self.assertEqual(session.get("email"), MANAGER_EMAIL)
        self.assertTrue(session.get("name"))
        self.assertEqual(session.get("role"), "manager")
        self.assertEqual(session.get("permissions"), {
            "manage_jobs": True, "manage_users": True, "edit_time_entries": True,
            "manage_assignments": True, "view_audit": True})
        users = self.users()
        self.assertEqual(len(users), 1)
        self.assertEqual(users[0]["email"], MANAGER_EMAIL)
        self.assertEqual(users[0]["role"], "manager")
        self.assertIs(users[0]["active"], True)

    def test_two_simultaneous_logins_keep_separate_identities(self):
        self.create_user(name='Mario "Qualità" ✓')
        with ThreadPoolExecutor(max_workers=2) as pool:
            manager = pool.submit(self.login, MANAGER_EMAIL, MANAGER_PASSWORD)
            worker = pool.submit(self.login, WORKER_EMAIL.upper(), WORKER_PASSWORD)
            manager_cookie, worker_cookie = manager.result(), worker.result()
        self.assertNotEqual(manager_cookie, worker_cookie)
        self.assertEqual(self.session(manager_cookie)["email"], MANAGER_EMAIL)
        worker = self.session(worker_cookie)
        self.assertEqual(worker["email"], WORKER_EMAIL)
        self.assertEqual(worker["name"], 'Mario "Qualità" ✓')
        self.assertEqual(worker["role"], "worker")
        self.assertEqual(worker.get("permissions"), {
            "manage_jobs": False, "manage_users": False, "edit_time_entries": False,
            "manage_assignments": False, "view_audit": False})
        self.assertEqual(self.session(self.manager_cookie)["role"], "manager")

    def test_worker_sees_jobs_and_records_only_own_daily_work(self):
        worker_id = self.create_user()
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        job_id = self.create_job()
        status, data, _ = self.api("GET", "/api/jobs", cookie=worker)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["jobs"], [])
        self.assign_job(job_id, worker_id)
        manager_entry = self.create_time(self.manager_cookie, job_id, minutes=120)
        status, data, _ = self.api("GET", "/api/jobs", cookie=worker)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["jobs"][0]["id"], job_id)
        self.assertEqual(data["jobs"][0]["total_minutes"], 0)
        self.assertEqual(self.day(worker)["entries"], [])
        worker_entry = self.create_time(worker, job_id, minutes=45)
        worker_day = self.day(worker)
        self.assertEqual(worker_day["total_minutes"], 45)
        self.assertEqual([entry["id"] for entry in worker_day["entries"]], [worker_entry])
        self.assertEqual(worker_day["entries"][0]["owner"], WORKER_EMAIL)
        self.assertEqual([entry["id"] for entry in self.day(self.manager_cookie)["entries"]],
                         [manager_entry])
        self.assert_error(404, "DELETE", "/api/time-entries/%d" % worker_entry,
                          cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/time-entries",
                          self.time_fields(job_id, owner=MANAGER_EMAIL), cookie=worker)
        self.assert_error(400, "GET", "/api/time-entries?date=2026-10-10&owner=" + MANAGER_EMAIL,
                          cookie=worker)
        self.assertEqual(self.day(self.manager_cookie)["total_minutes"], 120)

    def test_worker_cannot_manage_jobs_users_or_modify_even_own_hours(self):
        worker_id = self.create_user()
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        job_id = self.create_job()
        self.assign_job(job_id, worker_id)
        entry = self.create_time(worker, job_id)
        routes = (
            ("POST", "/api/jobs", {"code": "FORBIDDEN", "name": "Nuova", "status": "active"}),
            ("POST", "/api/jobs/%d" % job_id, {"code": "ROLE-001", "name": "Alterata", "status": "paused"}),
            ("POST", "/api/time-entries/%d" % entry, self.time_fields(job_id, minutes=30)),
            ("DELETE", "/api/time-entries/%d" % entry, None),
            ("GET", "/api/users", None),
            ("GET", "/api/users/%d" % worker_id, None),
            ("POST", "/api/users", {"name": "Forzato", "email": "forged@punto.local", "password": WORKER_PASSWORD, "role": "manager"}),
            ("POST", "/api/users/%d" % worker_id, {"name": "Operatore", "role": "manager", "active": "1"}),
        )
        for method, path, fields in routes:
            with self.subTest(method=method, path=path):
                self.assert_error(403, method, path, fields, cookie=worker)
        self.assertEqual(self.day(worker)["total_minutes"], 60)
        self.assertEqual(len(self.users()), 2)

    def test_worker_cannot_forge_role_through_query_form_or_cookie(self):
        self.create_user()
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        fields = {"code": "FORGED", "name": "Forzata", "status": "active"}
        for path, changes, cookie in (
                ("/api/jobs?role=manager", {}, worker),
                ("/api/jobs", {"role": "manager"}, worker),
                ("/api/jobs", {"permissions": "manage_jobs"}, worker),
                ("/api/jobs", {}, worker + "; role=manager; user_id=1")):
            with self.subTest(path=path, changes=changes, cookie=cookie):
                self.assert_error(403, "POST", path, dict(fields, **changes), cookie=cookie)
        self.assertEqual(self.session(worker)["role"], "worker")
        status, data, _ = self.api("GET", "/api/jobs", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["jobs"], [])

    def test_role_changes_apply_to_existing_sessions_immediately(self):
        worker_id = self.create_user()
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        status, data, _ = self.edit_user(worker_id, name="Nuovo gestore", role="manager")
        self.assertEqual(status, 200, data)
        self.assertEqual(self.session(worker)["role"], "manager")
        self.assertEqual(self.session(worker)["name"], "Nuovo gestore")
        status, data, _ = self.api("POST", "/api/jobs", {
            "code": "PROMOTED", "name": "Da utente promosso", "status": "planned"}, cookie=worker)
        self.assertEqual(status, 201, data)
        job_id = data["id"]
        status, data, _ = self.edit_user(worker_id)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.session(worker)["role"], "worker")
        self.assert_error(403, "GET", "/api/users", cookie=worker)
        self.assert_error(403, "POST", "/api/jobs/%d" % job_id, {
            "code": "PROMOTED", "name": "Modifica vietata", "status": "active"}, cookie=worker)
        self.assign_job(job_id, worker_id)
        self.create_time(worker, job_id)

    def test_disabling_an_account_invalidates_existing_sessions(self):
        worker_id = self.create_user()
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        job_id = self.create_job()
        status, data, _ = self.edit_user(worker_id, active="0")
        self.assertEqual(status, 200, data)
        self.assertIs(self.session(worker).get("authenticated"), False)
        self.assert_error(401, "GET", "/api/jobs", cookie=worker)
        self.assert_error(401, "POST", "/api/time-entries", self.time_fields(job_id), cookie=worker)
        self.assert_error(401, "POST", "/api/login", {
            "email": WORKER_EMAIL, "password": WORKER_PASSWORD})
        self.assertIs(next(user for user in self.users() if user["id"] == worker_id)["active"], False)
        status, data, _ = self.edit_user(worker_id, active="1")
        self.assertEqual(status, 200, data)
        self.assertEqual(self.session(self.login(WORKER_EMAIL, WORKER_PASSWORD))["role"], "worker")

    def test_disable_then_reenable_does_not_restore_unobserved_old_sessions(self):
        worker_id = self.create_user()
        first_cookie = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        second_cookie = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        status, data, _ = self.edit_user(worker_id, active="0")
        self.assertEqual(status, 200, data)
        # Neither old session makes a request before the account is re-enabled.
        status, data, _ = self.edit_user(worker_id, active="1")
        self.assertEqual(status, 200, data)
        for cookie in (first_cookie, second_cookie):
            with self.subTest(cookie=cookie):
                self.assertIs(self.session(cookie).get("authenticated"), False)
                self.assert_error(401, "GET", "/api/jobs", cookie=cookie)
        new_cookie = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        self.assertNotIn(new_cookie, (first_cookie, second_cookie))
        self.assertIs(self.session(new_cookie).get("authenticated"), True)
        self.assertEqual(self.session(new_cookie)["role"], "worker")

    def test_last_active_manager_cannot_be_demoted_or_disabled(self):
        manager = self.users()[0]
        for role, active in (("worker", "1"), ("manager", "0"), ("worker", "0")):
            with self.subTest(role=role, active=active):
                status, data, _ = self.edit_user(manager["id"], name=manager["name"],
                                                 role=role, active=active)
                self.assertEqual(status, 409, data)
                self.assertIs(data.get("ok"), False)
        second = self.create_user(email="manager2@punto.local", role="manager")
        status, data, _ = self.edit_user(second, role="manager", active="0")
        self.assertEqual(status, 200, data)
        status, data, _ = self.edit_user(manager["id"], name=manager["name"], role="worker")
        self.assertEqual(status, 409, data)
        status, data, _ = self.edit_user(second, role="manager", active="1")
        self.assertEqual(status, 200, data)
        status, data, _ = self.edit_user(manager["id"], name=manager["name"], role="worker")
        self.assertEqual(status, 200, data)
        self.assertEqual(self.session(self.manager_cookie)["role"], "worker")
        self.assert_error(403, "GET", "/api/users", cookie=self.manager_cookie)

    def test_user_creation_validates_required_fields_roles_and_password_bytes(self):
        valid = {"name": "Operatore", "email": WORKER_EMAIL,
                 "password": WORKER_PASSWORD, "role": "worker"}
        for changes in ({"name": ""}, {"name": "界" * 121},
                        {"email": "not-an-email"}, {"role": "admin"},
                        {"password": "1234567"}, {"password": "x" * 1025},
                        {"password": "界" * 342}, {"password": "Password\n2026"},
                        {"password": "Password\r2026"}, {"password": "Password\t2026"}):
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/users", dict(valid, **changes),
                                  cookie=self.manager_cookie)
        for missing in valid:
            fields = dict(valid)
            del fields[missing]
            with self.subTest(missing=missing):
                self.assert_error(400, "POST", "/api/users", fields, cookie=self.manager_cookie)
        for index, password in enumerate(("12345678", "x" * 1024, "界" * 341)):
            email = "boundary%d@punto.local" % index
            name = "界" * 120 if index == 2 else "Operatore"
            self.create_user(email=email, password=password, name=name)
            session = self.session(self.login(email, password))
            self.assertEqual(session["email"], email)
            self.assertEqual(session["name"], name)

    def test_email_is_unique_case_insensitively_and_immutable_on_edit(self):
        user_id = self.create_user()
        self.assert_error(409, "POST", "/api/users", {
            "name": "Duplicato", "email": WORKER_EMAIL.upper(),
            "password": WORKER_PASSWORD, "role": "manager"}, cookie=self.manager_cookie)
        self.assert_error(409, "POST", "/api/users", {
            "name": "Duplicato gestore", "email": MANAGER_EMAIL.upper(),
            "password": WORKER_PASSWORD, "role": "worker"}, cookie=self.manager_cookie)
        for extra in ({"email": "changed@punto.local"}, {"password": "Changed-only!2026"},
                      {"unexpected": "value"}):
            with self.subTest(extra=extra):
                self.assert_error(400, "POST", "/api/users/%d" % user_id, {
                    "name": "Operatore", "role": "worker", "active": "1", **extra},
                    cookie=self.manager_cookie)
        for changes in ({"role": "admin"}, {"active": "yes"}, {"active": "2"}):
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/users/%d" % user_id, {
                    "name": "Operatore", "role": "worker", "active": "1", **changes},
                    cookie=self.manager_cookie)
        self.assertEqual(self.session(self.login(WORKER_EMAIL, WORKER_PASSWORD))["email"], WORKER_EMAIL)
        self.assertEqual(len(self.users()), 2)

    def test_user_api_requires_authentication_origin_and_form_content(self):
        user_id = self.create_user()
        create_fields = {"name": "Altro", "email": "other@punto.local",
                         "password": WORKER_PASSWORD, "role": "worker"}
        edit_fields = {"name": "Operatore", "role": "worker", "active": "1"}
        routes = (("GET", "/api/users", None), ("GET", "/api/users/%d" % user_id, None),
                  ("POST", "/api/users", create_fields),
                  ("POST", "/api/users/%d" % user_id, edit_fields))
        for method, path, fields in routes:
            for cookie in (None, "punto_session=forged"):
                with self.subTest(method=method, path=path, cookie=cookie):
                    self.assert_error(401, method, path, fields, cookie=cookie)
            if method == "POST":
                self.assert_error(403, method, path, fields, cookie=self.manager_cookie,
                                  headers={"Origin": "https://attacker.example"})
                self.assert_error(415, method, path, cookie=self.manager_cookie,
                                  headers={"Content-Type": "application/json"}, body=json.dumps(fields))
                for body in (urlencode(fields) + "&role=manager", "name=%ZZ"):
                    self.assert_error(400, method, path, cookie=self.manager_cookie,
                                      headers={"Content-Type": "application/x-www-form-urlencoded"},
                                      body=body)
        self.assert_error(404, "POST", "/api/users/999999", edit_fields, cookie=self.manager_cookie)
        self.assertEqual(len(self.users()), 2)

    def test_user_responses_do_not_expose_passwords_hashes_or_salts(self):
        self.create_user()
        for user in self.users():
            self.assertEqual(set(user), {"id", "name", "email", "role", "active"})
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        for cookie in (worker, self.manager_cookie):
            session = self.session(cookie)
            serialized = json.dumps(session).lower()
            for forbidden in ("password", "hash", "salt"):
                self.assertNotIn(forbidden, serialized)
        database_bytes = self.db_path.read_bytes()
        self.assertNotIn(MANAGER_PASSWORD.encode(), database_bytes)
        self.assertNotIn(WORKER_PASSWORD.encode(), database_bytes)

    def test_accounts_roles_and_credentials_persist_without_bootstrap_overwrite(self):
        worker_id = self.create_user(name="Persistente")
        disabled_id = self.create_user(email="disabled@punto.local", name="Disabilitato")
        status, data, _ = self.edit_user(disabled_id, name="Disabilitato", active="0")
        self.assertEqual(status, 200, data)
        old_cookie = self.manager_cookie
        replacement_password = "Replacement-only!2026"
        self.secrets.add(replacement_password)
        self.stop_server()
        self.start_server(email=MANAGER_EMAIL, password=replacement_password)
        self.assert_error(401, "GET", "/api/users", cookie=old_cookie)
        self.manager_cookie = self.login(MANAGER_EMAIL, MANAGER_PASSWORD)
        self.assert_error(401, "POST", "/api/login", {
            "email": MANAGER_EMAIL, "password": replacement_password})
        worker = self.login(WORKER_EMAIL, WORKER_PASSWORD)
        self.assertEqual(self.session(worker)["name"], "Persistente")
        by_id = {user["id"]: user for user in self.users()}
        self.assertEqual(by_id[worker_id]["role"], "worker")
        self.assertIs(by_id[disabled_id]["active"], False)
        self.stop_server()
        self.start_server(email="new-bootstrap@punto.local", password=replacement_password)
        self.manager_cookie = self.login(MANAGER_EMAIL, MANAGER_PASSWORD)
        self.assertEqual(len(self.users()), 3)
        self.assert_error(401, "POST", "/api/login", {
            "email": "new-bootstrap@punto.local", "password": replacement_password})
        self.assert_error(401, "POST", "/api/login", {
            "email": "disabled@punto.local", "password": WORKER_PASSWORD})

    def test_legacy_workspace_history_survives_mixed_case_account_migration(self):
        job_id = self.create_job()
        entry_id = self.create_time(self.manager_cookie, job_id, minutes=75,
                                    note="Ore precedenti ai ruoli")
        self.stop_server()
        # The earlier application had jobs and time_entries, without a users table.
        # Reuse their existing schema to exercise an actual upgrade on startup.
        with closing(sqlite3.connect(self.db_path)) as database:
            # Rebuild the genuine pre-user-ID schema rather than leave new
            # foreign keys pointing to the table being removed.
            database.execute("CREATE TABLE legacy_entries ("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                             "job_id INTEGER NOT NULL REFERENCES jobs(id),"
                             "owner TEXT NOT NULL,date TEXT NOT NULL,"
                             "minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),"
                             "note TEXT NOT NULL DEFAULT '')")
            database.execute("INSERT INTO legacy_entries(id,job_id,owner,date,minutes,note) "
                             "SELECT id,job_id,owner,date,minutes,note FROM time_entries")
            database.execute("DROP TABLE time_entries")
            database.execute("ALTER TABLE legacy_entries RENAME TO time_entries")
            for table in database.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
                if table[0] not in ("jobs", "time_entries", "sqlite_sequence"):
                    database.execute('DROP TABLE "' + table[0].replace('"', '""') + '"')
            database.execute("PRAGMA user_version=0")
            database.execute("UPDATE time_entries SET owner=? WHERE id=?",
                             ("MaNaGeR@PuNtO.LoCaL", entry_id))
            other_entry = database.execute(
                "INSERT INTO time_entries(job_id,owner,date,minutes,note) VALUES(?,?,?,?,?)",
                (job_id, "unrelated@punto.local", "2026-10-10", 30, "Altro storico"))
            other_entry_id = other_entry.lastrowid
            database.commit()
        self.start_server(email=MANAGER_EMAIL.upper())
        self.manager_cookie = self.login(MANAGER_EMAIL, MANAGER_PASSWORD)
        self.assertEqual(len(self.users()), 1)
        self.assertEqual(self.session(self.manager_cookie)["email"], MANAGER_EMAIL)
        day = self.day(self.manager_cookie)
        self.assertEqual(day["total_minutes"], 75)
        self.assertEqual([entry["id"] for entry in day["entries"]], [entry_id])
        self.assertEqual(day["entries"][0]["owner"], "MaNaGeR@PuNtO.LoCaL")
        self.assertEqual(day["entries"][0]["note"], "Ore precedenti ai ruoli")
        status, data, _ = self.api("GET", "/api/jobs", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["jobs"][0]["id"], job_id)
        self.assertEqual(data["jobs"][0]["total_minutes"], 75)
        with closing(sqlite3.connect(self.db_path)) as database:
            self.assertEqual(database.execute(
                "SELECT owner,minutes,note FROM time_entries WHERE id=?",
                (other_entry_id,)).fetchone(), ("unrelated@punto.local", 30, "Altro storico"))


if __name__ == "__main__":
    unittest.main()
