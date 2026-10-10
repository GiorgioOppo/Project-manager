"""Assignment enforcement, stable user ownership, migrations and atomic audit."""

from contextlib import closing
from datetime import datetime, timedelta
import json
import os
import sqlite3
import subprocess
import unittest
from zoneinfo import ZoneInfo

import test_roles as roles


class AssignmentIntegrationTests(unittest.TestCase):
    # Reuse the HTTP fixture without inheriting and running its role tests twice.
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user
    session = roles.RoleIntegrationTests.session
    create_job = roles.RoleIntegrationTests.create_job
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    day = roles.RoleIntegrationTests.day

    def today(self, days=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=days)).isoformat()

    def worker(self, **changes):
        user_id = self.create_user(**changes)
        email = changes.get("email", roles.WORKER_EMAIL)
        password = changes.get("password", roles.WORKER_PASSWORD)
        return user_id, self.login(email, password)

    def assign(self, job_id, user_id, starts_on="", ends_on="", expected=201):
        status, data, _ = self.api("POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": user_id, "starts_on": starts_on, "ends_on": ends_on},
            cookie=self.manager_cookie)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), True)
        return data

    def revoke(self, job_id, user_id):
        status, data, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (job_id, user_id),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)

    def jobs(self, cookie=None):
        status, data, _ = self.api("GET", "/api/jobs", cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data["jobs"]

    def audit(self, query=""):
        status, data, _ = self.api("GET", "/api/audit" + ("?" + query if query else ""),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data.get("events"), list)
        return data

    def events(self, query=""):
        return self.audit(query)["events"]

    def test_assignment_upsert_list_and_delete(self):
        user_id, _ = self.worker(name='Mario "Tecnico" ✓')
        job_id = self.create_job()
        status, data, _ = self.api("GET", "/api/jobs/%d/assignments" % job_id,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data, {"job_id": job_id, "assignments": []})
        self.assign(job_id, user_id, self.today(-2), self.today(2))
        status, data, _ = self.api("GET", "/api/jobs/%d/assignments" % job_id,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["job_id"], job_id)
        self.assertEqual(data["assignments"], [{
            "user_id": user_id, "name": 'Mario "Tecnico" ✓', "email": roles.WORKER_EMAIL,
            "role": "worker", "active": True, "starts_on": self.today(-2),
            "ends_on": self.today(2)}])
        self.assign(job_id, user_id, expected=200)
        status, data, _ = self.api("GET", "/api/jobs/%d/assignments" % job_id,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(len(data["assignments"]), 1)
        self.assertEqual(data["assignments"][0]["starts_on"], "")
        self.assertEqual(data["assignments"][0]["ends_on"], "")
        self.revoke(job_id, user_id)
        self.assert_error(404, "DELETE", "/api/jobs/%d/assignments/%d" % (job_id, user_id),
                          cookie=self.manager_cookie)

    def test_new_worker_has_no_implicit_access_and_only_sees_assigned_jobs(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        status, data, _ = self.api("POST", "/api/jobs", {
            "code": "UNASSIGNED", "name": "Riservata", "status": "active"},
            cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        private_job = data["id"]
        self.assertEqual(self.jobs(worker), [])
        for denied_job in (job_id, private_job, 999999):
            self.assert_error(403, "POST", "/api/time-entries", self.time_fields(denied_job),
                              cookie=worker)
        self.assign(job_id, user_id)
        self.assertEqual([job["id"] for job in self.jobs(worker)], [job_id])
        self.assertEqual({job["id"] for job in self.jobs()}, {job_id, private_job})
        self.create_time(worker, job_id)
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(private_job), cookie=worker)

    def test_date_limits_apply_to_today_and_to_the_recorded_work_date(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id, self.today(-1), self.today(1))
        for date in (self.today(-1), self.today(), self.today(1)):
            with self.subTest(allowed=date):
                self.create_time(worker, job_id, date=date)
        for date in (self.today(-2), self.today(2)):
            with self.subTest(denied=date):
                self.assert_error(403, "POST", "/api/time-entries",
                                  self.time_fields(job_id, date=date), cookie=worker)
        self.assign(job_id, user_id, self.today(), self.today(), expected=200)
        self.assertEqual([job["id"] for job in self.jobs(worker)], [job_id])
        self.create_time(worker, job_id, date=self.today(), minutes=30)
        self.assign(job_id, user_id, self.today(-2), self.today(-1), expected=200)
        self.assertEqual(self.jobs(worker), [])
        # A past work date inside an expired assignment cannot bypass revocation today.
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job_id, date=self.today(-1)), cookie=worker)
        self.assign(job_id, user_id, self.today(1), self.today(2), expected=200)
        self.assertEqual(self.jobs(worker), [])
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job_id, date=self.today(1)), cookie=worker)

    def test_revocation_keeps_history_but_blocks_new_work_immediately(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        entry_id = self.create_time(worker, job_id, minutes=75, note="Storico da conservare")
        self.revoke(job_id, user_id)
        self.assertEqual(self.jobs(worker), [])
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(job_id), cookie=worker)
        history = self.day(worker)
        self.assertEqual(history["total_minutes"], 75)
        self.assertEqual([entry["id"] for entry in history["entries"]], [entry_id])
        self.assertEqual(history["entries"][0]["note"], "Storico da conservare")
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assertEqual(self.jobs(worker), [])
        self.assertEqual(self.day(worker)["total_minutes"], 75)
        self.assign(job_id, user_id)
        self.create_time(worker, job_id, minutes=25)
        self.assertEqual(self.day(worker)["total_minutes"], 100)

    def test_manager_access_is_not_restricted_by_assignments(self):
        job_id = self.create_job()
        manager_id = self.users()[0]["id"]
        self.assign(job_id, manager_id, self.today(-5), self.today(-3))
        self.assertEqual([job["id"] for job in self.jobs()], [job_id])
        self.create_time(self.manager_cookie, job_id, date=self.today())
        self.revoke(job_id, manager_id)
        self.create_time(self.manager_cookie, job_id, date=self.today(), minutes=30)

    def test_assignment_api_enforces_identity_origin_and_form_validation(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        path = "/api/jobs/%d/assignments" % job_id
        fields = {"user_id": user_id, "starts_on": "", "ends_on": ""}
        for method, target, values in (("GET", path, None), ("POST", path, fields),
                                       ("DELETE", path + "/%d" % user_id, None)):
            self.assert_error(401, method, target, values)
            self.assert_error(403, method, target, values, cookie=worker)
            if method != "GET":
                self.assert_error(403, method, target, values, cookie=self.manager_cookie,
                                  headers={"Origin": "https://attacker.example"})
        self.assert_error(415, "POST", path, cookie=self.manager_cookie,
                          headers={"Content-Type": "application/json"}, body=json.dumps(fields))
        for changes in ({"user_id": "0"}, {"user_id": "-1"}, {"user_id": "1x"},
                        {"user_id": "9223372036854775808"}, {"starts_on": "2026-02-29"},
                        {"ends_on": "2026-13-01"}, {"starts_on": "2026-1-01"},
                        {"starts_on": "2026-10-11", "ends_on": "2026-10-10"},
                        {"role": "manager"}):
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", path, dict(fields, **changes), cookie=self.manager_cookie)
        self.assert_error(400, "POST", path, {"starts_on": "", "ends_on": ""}, cookie=self.manager_cookie)
        for body in ("user_id=%ZZ", "user_id=1&user_id=2&starts_on=&ends_on="):
            self.assert_error(400, "POST", path, cookie=self.manager_cookie,
                              headers={"Content-Type": "application/x-www-form-urlencoded"}, body=body)
        self.assert_error(400, "GET", path + "?user_id=" + str(user_id), cookie=self.manager_cookie)
        self.assert_error(404, "POST", path, dict(fields, user_id=999999), cookie=self.manager_cookie)
        self.assert_error(404, "GET", "/api/jobs/999999/assignments", cookie=self.manager_cookie)
        status, data, _ = self.api("POST", path, {"user_id": user_id}, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        for values in ({"user_id": user_id, "starts_on": ""},
                       {"user_id": user_id, "ends_on": ""}):
            status, data, _ = self.api("POST", path, values, cookie=self.manager_cookie)
            self.assertEqual(status, 200, data)
        status, data, _ = self.api("GET", path, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["assignments"][0]["starts_on"], "")
        self.assertEqual(data["assignments"][0]["ends_on"], "")
        status, data, _ = self.edit_user(user_id, active="0")
        self.assertEqual(status, 200, data)
        self.assert_error(400, "POST", path, fields, cookie=self.manager_cookie)

    def test_user_id_and_owner_cannot_be_supplied_by_the_operator(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        for extra in ({"user_id": self.users()[0]["id"]}, {"owner": roles.MANAGER_EMAIL}):
            self.assert_error(400, "POST", "/api/time-entries",
                              self.time_fields(job_id, **extra), cookie=worker)
        self.assert_error(400, "GET", "/api/time-entries?date=2026-10-10&user_id=" + str(user_id),
                          cookie=worker)
        self.assertEqual(self.day(worker)["entries"], [])

    def test_timesheets_use_user_id_after_email_snapshot_is_changed(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        entry_id = self.create_time(worker, job_id, minutes=45)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT user_id FROM time_entries WHERE id=?", (entry_id,)).fetchone(),
                             (user_id,))
            db.execute("UPDATE time_entries SET owner=? WHERE id=?", (roles.MANAGER_EMAIL, entry_id))
            db.commit()
        self.assertEqual(self.day(worker)["total_minutes"], 45)
        self.assertEqual(self.day(self.manager_cookie)["entries"], [])
        self.assertEqual(self.jobs(worker)[0]["total_minutes"], 45)
        self.assertEqual(self.jobs()[0]["total_minutes"], 0)
        self.assert_error(404, "DELETE", "/api/time-entries/%d" % entry_id, cookie=self.manager_cookie)
        self.assert_error(404, "POST", "/api/time-entries/%d" % entry_id,
                          self.time_fields(job_id, minutes=30), cookie=self.manager_cookie)

    def test_audit_records_actor_before_after_and_every_mutation_family(self):
        manager_id = self.users()[0]["id"]
        user_id, worker = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        entry_id = self.create_time(worker, job_id, minutes=45)
        self.assign(job_id, user_id, self.today(-10), self.today(10), expected=200)
        self.revoke(job_id, user_id)
        status, data, _ = self.api("POST", "/api/jobs/%d" % job_id, {
            "code": "ROLE-001", "name": "Commessa aggiornata", "status": "paused"},
            cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        status, data, _ = self.edit_user(user_id, name="Operatore aggiornato")
        self.assertEqual(status, 200, data)
        manager_entry = self.create_time(self.manager_cookie, job_id, minutes=30)
        status, data, _ = self.api("POST", "/api/time-entries/%d" % manager_entry,
                                   self.time_fields(job_id, minutes=60), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        status, data, _ = self.api("DELETE", "/api/time-entries/%d" % manager_entry,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        events = self.events("limit=100")
        actions = {event["action"] for event in events}
        self.assertTrue({"jobs.create", "jobs.update", "time_entries.create", "time_entries.update",
                         "time_entries.delete", "assignments.create", "assignments.update",
                         "assignments.delete", "users.create", "users.update", "system.migration"} <= actions)
        self.assertEqual([event["id"] for event in events], sorted((event["id"] for event in events), reverse=True))
        for event in events:
            self.assertEqual(set(event), {"id", "created_at", "actor_id", "actor_name", "actor_email",
                                          "action", "entity_type", "entity_id", "before", "after"})
            self.assertRegex(event["created_at"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
            for snapshot in (event["before"], event["after"]):
                self.assertTrue(snapshot is None or isinstance(snapshot, dict))
        time_create = next(event for event in events if event["action"] == "time_entries.create"
                           and event["entity_id"] == entry_id)
        self.assertEqual(time_create["actor_id"], user_id)
        self.assertEqual(time_create["actor_email"], roles.WORKER_EMAIL)
        self.assertIsNone(time_create["before"])
        self.assertEqual(time_create["after"]["minutes"], 45)
        job_update = next(event for event in events if event["action"] == "jobs.update")
        self.assertEqual(job_update["actor_id"], manager_id)
        self.assertEqual(job_update["before"]["name"], "Commessa condivisa")
        self.assertEqual(job_update["after"]["name"], "Commessa aggiornata")
        time_delete = next(event for event in events if event["action"] == "time_entries.delete")
        self.assertEqual(time_delete["before"]["minutes"], 60)
        self.assertIsNone(time_delete["after"])

    def test_audit_pagination_filters_persistence_and_secret_exclusion(self):
        user_id, _ = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        self.create_time(self.manager_cookie, job_id)
        self.revoke(job_id, user_id)
        all_events = self.events("limit=100")
        collected = []
        before = None
        for _ in range(len(all_events) + 1):
            data = self.audit("limit=2" + ("&before_id=" + str(before) if before else ""))
            self.assertLessEqual(len(data["events"]), 2)
            collected.extend(data["events"])
            before = data["next_before_id"]
            if before is None:
                break
            self.assertIsInstance(before, int)
            self.assertGreater(before, 0)
        self.assertEqual([event["id"] for event in collected], [event["id"] for event in all_events])
        self.assertEqual(len({event["id"] for event in collected}), len(collected))
        for entity_type in ("job", "time_entry", "assignment", "user", "system"):
            filtered = self.events("limit=100&entity_type=" + entity_type)
            self.assertTrue(filtered)
            self.assertTrue(all(event["entity_type"] == entity_type for event in filtered))
        serialized = json.dumps(all_events).lower()
        for forbidden in ("password", "hash", "salt", roles.MANAGER_PASSWORD.lower(), roles.WORKER_PASSWORD.lower()):
            self.assertNotIn(forbidden, serialized)
        with closing(sqlite3.connect(self.db_path)) as db:
            tables = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE '%audit%'")]
            self.assertTrue(tables)
            for table in tables:
                rows = db.execute('SELECT * FROM "' + table.replace('"', '""') + '"').fetchall()
                audit_storage = json.dumps(rows).lower()
                for forbidden in ("password", "hash", "salt", roles.MANAGER_PASSWORD.lower(), roles.WORKER_PASSWORD.lower()):
                    self.assertNotIn(forbidden, audit_storage)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.events("limit=100"), all_events)

    def test_audit_api_is_manager_only_and_strictly_validates_filters(self):
        _, worker = self.worker()
        self.assert_error(401, "GET", "/api/audit")
        self.assert_error(403, "GET", "/api/audit", cookie=worker)
        for query in ("limit=0", "limit=-1", "limit=101", "limit=abc", "limit=1&limit=2",
                      "before_id=0", "before_id=-1", "before_id=1x", "before_id=9223372036854775808",
                      "entity_type=users", "entity_type=unknown", "entity_type=job&entity_type=user",
                      "owner=manager@punto.local", "limit=%ZZ"):
            with self.subTest(query=query):
                self.assert_error(400, "GET", "/api/audit?" + query, cookie=self.manager_cookie)

    def test_rejected_mutations_do_not_create_audit_events(self):
        user_id, worker = self.worker()
        job_id = self.create_job()
        before = self.events("limit=100")
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(job_id), cookie=worker)
        self.assert_error(403, "POST", "/api/jobs", {"code": "DENIED", "name": "Vietata", "status": "active"}, cookie=worker)
        self.assert_error(400, "POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": user_id, "starts_on": "2026-02-30", "ends_on": ""}, cookie=self.manager_cookie)
        self.assert_error(409, "POST", "/api/jobs", {
            "code": "ROLE-001", "name": "Duplicata", "status": "active"}, cookie=self.manager_cookie)
        self.assertEqual(self.events("limit=100"), before)

    def test_audit_failure_rolls_back_business_mutations(self):
        user_id, _ = self.worker()
        job_id = self.create_job()
        self.assign(job_id, user_id)
        manager_entry = self.create_time(self.manager_cookie, job_id, minutes=45)
        before_events = self.events("limit=100")
        before_jobs = self.jobs()
        status, before_assignments, _ = self.api("GET", "/api/jobs/%d/assignments" % job_id,
                                                 cookie=self.manager_cookie)
        self.assertEqual(status, 200, before_assignments)
        with closing(sqlite3.connect(self.db_path)) as db:
            tables = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE '%audit%'")]
            self.assertEqual(len(tables), 1)
            table = '"' + tables[0].replace('"', '""') + '"'
            db.execute("CREATE TRIGGER block_audit BEFORE INSERT ON " + table +
                       " BEGIN SELECT RAISE(ABORT, 'fixture audit failure'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/jobs", {
            "code": "ROLLBACK", "name": "Da annullare", "status": "active"}, cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/jobs/%d" % job_id, {
            "code": "ROLE-001", "name": "Da annullare", "status": "paused"}, cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": self.users()[0]["id"], "starts_on": "", "ends_on": ""}, cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/jobs/%d/assignments" % job_id, {
            "user_id": user_id, "starts_on": self.today(), "ends_on": self.today()}, cookie=self.manager_cookie)
        self.assert_error(500, "DELETE", "/api/jobs/%d/assignments/%d" % (job_id, user_id), cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/time-entries", self.time_fields(job_id), cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/time-entries/%d" % manager_entry,
                          self.time_fields(job_id, minutes=60), cookie=self.manager_cookie)
        self.assert_error(500, "DELETE", "/api/time-entries/%d" % manager_entry, cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/users", {
            "name": "Da annullare", "email": "rollback@punto.local", "password": roles.WORKER_PASSWORD,
            "role": "worker"}, cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/users/%d" % user_id, {
            "name": "Da annullare", "role": "manager", "active": "1"}, cookie=self.manager_cookie)
        self.assertEqual(self.jobs(), before_jobs)
        self.assertEqual(self.day(self.manager_cookie)["total_minutes"], 45)
        self.assertEqual(len(self.users()), 2)
        self.assertEqual(next(user for user in self.users() if user["id"] == user_id)["role"], "worker")
        status, data, _ = self.api("GET", "/api/jobs/%d/assignments" % job_id, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(data, before_assignments)
        self.assertEqual(self.events("limit=100"), before_events)

    def test_schema_migration_maps_legacy_owners_without_assigning_everyone(self):
        job_id = self.create_job()
        entry_id = self.create_time(self.manager_cookie, job_id, minutes=75)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            # Recreate the deployed pre-roles schema, retaining business records.
            rows = db.execute("SELECT id,job_id,owner,date,minutes,note FROM time_entries").fetchall()
            for (table,) in db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
                if table not in ("jobs", "sqlite_sequence"):
                    db.execute('DROP TABLE "' + table.replace('"', '""') + '"')
            db.execute("CREATE TABLE time_entries (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "job_id INTEGER NOT NULL REFERENCES jobs(id),owner TEXT NOT NULL,date TEXT NOT NULL,"
                       "minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),note TEXT NOT NULL DEFAULT '')")
            db.executemany("INSERT INTO time_entries(id,job_id,owner,date,minutes,note) VALUES(?,?,?,?,?,?)", rows)
            db.execute("UPDATE time_entries SET owner=? WHERE id=?", ("MaNaGeR@PuNtO.LoCaL", entry_id))
            legacy_id = db.execute("INSERT INTO time_entries(job_id,owner,date,minutes,note) VALUES(?,?,?,?,?)",
                                   (job_id, "FuTuRe@punto.local", "2026-10-10", 35, "Storico non ancora associato")).lastrowid
            orphan_id = db.execute("INSERT INTO time_entries(job_id,owner,date,minutes,note) VALUES(?,?,?,?,?)",
                                   (job_id, "orphan@punto.local", "2026-10-10", 20, "Da conservare")).lastrowid
            db.execute("PRAGMA user_version=0")
            db.commit()
        self.start_server(email=roles.MANAGER_EMAIL.upper())
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        manager_id = self.users()[0]["id"]
        self.assertEqual(self.day(self.manager_cookie)["total_minutes"], 75)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT owner,user_id FROM time_entries WHERE id=?", (entry_id,)).fetchone(),
                             ("MaNaGeR@PuNtO.LoCaL", manager_id))
            self.assertEqual(db.execute("SELECT user_id FROM time_entries WHERE id=?", (legacy_id,)).fetchone(), (None,))
            self.assertEqual(db.execute("SELECT owner,minutes,note,user_id FROM time_entries WHERE id=?", (orphan_id,)).fetchone(),
                             ("orphan@punto.local", 20, "Da conservare", None))
        future_id, future_cookie = self.worker(email="future@punto.local")
        self.assertEqual(self.jobs(future_cookie), [])
        self.assertEqual(self.day(future_cookie)["total_minutes"], 35)
        self.assertEqual(self.day(future_cookie)["entries"][0]["id"], legacy_id)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT owner,user_id FROM time_entries WHERE id=?", (legacy_id,)).fetchone(),
                             ("FuTuRe@punto.local", future_id))
        before_events = self.events("limit=100")
        self.assertEqual(sum(event["action"] == "system.migration" for event in before_events), 1)
        self.stop_server()
        self.start_server(password="Replacement-only!2026")
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.events("limit=100"), before_events)
        self.assertEqual(self.day(self.login("future@punto.local", roles.WORKER_PASSWORD))["total_minutes"], 35)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM time_entries").fetchone(), (3,))

    def test_upgrade_preserves_existing_accounts_and_claims_only_unowned_history(self):
        manager_id = self.users()[0]["id"]
        user_id, worker = self.worker(name="Utente già presente")
        disabled_id = self.create_user(email="disabled@punto.local", name="Disabilitato")
        status, data, _ = self.edit_user(disabled_id, name="Disabilitato", active="0")
        self.assertEqual(status, 200, data)
        job_id = self.create_job()
        self.assign(job_id, user_id)
        entry_id = self.create_time(worker, job_id, minutes=55)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            # The previous release already had accounts, but owned timesheets by email.
            rows = db.execute("SELECT id,job_id,owner,date,minutes,note FROM time_entries").fetchall()
            for (table,) in db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
                if table not in ("jobs", "users", "sqlite_sequence"):
                    db.execute('DROP TABLE "' + table.replace('"', '""') + '"')
            db.execute("CREATE TABLE time_entries (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "job_id INTEGER NOT NULL REFERENCES jobs(id),owner TEXT NOT NULL,date TEXT NOT NULL,"
                       "minutes INTEGER NOT NULL CHECK(minutes BETWEEN 1 AND 1440),note TEXT NOT NULL DEFAULT '')")
            db.executemany("INSERT INTO time_entries(id,job_id,owner,date,minutes,note) VALUES(?,?,?,?,?,?)", rows)
            db.execute("UPDATE time_entries SET owner=? WHERE id=?", (roles.WORKER_EMAIL.upper(), entry_id))
            db.execute("PRAGMA user_version=0")
            db.commit()
        self.start_server(email="different-bootstrap@punto.local", password="Replacement-only!2026")
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        by_id = {user["id"]: user for user in self.users()}
        self.assertEqual(set(by_id), {manager_id, user_id, disabled_id})
        self.assertEqual(by_id[user_id]["name"], "Utente già presente")
        self.assertEqual(by_id[user_id]["role"], "worker")
        self.assertIs(by_id[disabled_id]["active"], False)
        self.assertEqual(self.jobs(worker), [])
        self.assertEqual(self.day(worker)["total_minutes"], 55)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT user_id,owner FROM time_entries WHERE id=?", (entry_id,)).fetchone(),
                             (user_id, roles.WORKER_EMAIL.upper()))
            # A non-NULL user_id is authoritative even when an old email snapshot matches a new user.
            db.execute("UPDATE time_entries SET owner='later@punto.local' WHERE id=?", (entry_id,))
            db.commit()
        _, later = self.worker(email="later@punto.local")
        self.assertEqual(self.day(later)["entries"], [])
        self.assertEqual(self.day(worker)["total_minutes"], 55)

    def test_future_schema_version_is_rejected_without_bootstrap_or_database_changes(self):
        self.stop_server()
        future_path = self.db_path.with_name("future-schema.db")
        with closing(sqlite3.connect(future_path)) as db:
            db.execute("CREATE TABLE sentinel (id INTEGER PRIMARY KEY,value TEXT NOT NULL)")
            db.execute("INSERT INTO sentinel VALUES(1,'future data must survive')")
            db.execute("PRAGMA user_version=99")
            db.commit()
        before_bytes = future_path.read_bytes()
        env = dict(os.environ, PORT=str(self.port), PUBLIC_DIR=str(self.public),
                   PUNTO_EMAIL="future-bootstrap@punto.local", PUNTO_PASSWORD=roles.MANAGER_PASSWORD,
                   DB_PATH=str(future_path))
        result = subprocess.run([str(self.binary)], cwd=roles.ROOT, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
        self.assertNotEqual(result.returncode, 0, result.stdout.decode("utf-8", "replace"))
        self.assertNotIn(roles.MANAGER_PASSWORD.encode(), result.stdout)
        self.assertEqual(future_path.read_bytes(), before_bytes)
        with closing(sqlite3.connect(future_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (99,))
            self.assertEqual(db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall(), [("sentinel",)])
            self.assertEqual(db.execute("SELECT * FROM sentinel").fetchall(), [(1, "future data must survive")])


if __name__ == "__main__":
    unittest.main()
