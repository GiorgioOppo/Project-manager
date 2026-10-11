"""Common activities and deliberate permanent job cleanup, over isolated HTTP."""

from contextlib import closing
import json
import sqlite3
import unittest

import test_reports as reports
import test_roles as roles


class JobDefaultsIntegrationTests(unittest.TestCase):
    # Reuse the temporary SQLite/server fixture without inheriting its test cases.
    setUp = reports.ReportsIntegrationTests.setUp
    start_server = reports.ReportsIntegrationTests.start_server
    stop_server = reports.ReportsIntegrationTests.stop_server
    api = reports.ReportsIntegrationTests.api
    login = reports.ReportsIntegrationTests.login
    assert_error = reports.ReportsIntegrationTests.assert_error
    create_user = reports.ReportsIntegrationTests.create_user
    users = reports.ReportsIntegrationTests.users
    assign_job = reports.ReportsIntegrationTests.assign_job
    time_fields = reports.ReportsIntegrationTests.time_fields
    create_time = reports.ReportsIntegrationTests.create_time
    create_record = reports.ReportsIntegrationTests.create_record
    report = reports.ReportsIntegrationTests.report
    day = roles.RoleIntegrationTests.day
    edit_user = roles.RoleIntegrationTests.edit_user

    def job_fields(self, code="DEFAULT-001", **changes):
        fields = {"code": code, "name": "Commessa " + code, "status": "active"}
        fields.update(changes)
        return fields

    def job(self, code="DEFAULT-001", **changes):
        return self.create_record("jobs", self.job_fields(code, **changes))

    def update_job(self, job_id, code="DEFAULT-001", **changes):
        status, data, _ = self.api("POST", "/api/jobs/%d" % job_id,
                                   self.job_fields(code, **changes), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)

    def jobs(self, cookie=None):
        status, data, _ = self.api("GET", "/api/jobs", cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data["jobs"]

    def worker(self, email=roles.WORKER_EMAIL):
        user_id = self.create_user(email=email)
        return user_id, self.login(email, roles.WORKER_PASSWORD)

    def delete_job(self, job_id):
        status, data, _ = self.api("DELETE", "/api/jobs/%d" % job_id,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        return data

    def post(self, path, fields):
        status, data, _ = self.api("POST", path, fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        return data

    def events(self, job_id):
        status, data, _ = self.api("GET", "/api/audit?entity_type=job&limit=100",
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        return [event for event in data["events"] if event["entity_id"] == job_id]

    def test_new_jobs_are_private_by_default_and_noncommon_permissions_are_unchanged(self):
        assigned_id, assigned = self.worker()
        _, unassigned = self.worker("unassigned@punto.local")
        job_id = self.job()
        job = self.jobs()[0]
        self.assertEqual(job["id"], job_id)
        self.assertIs(job["is_common"], False)
        self.assertEqual(self.jobs(assigned), [])
        self.assertEqual(self.jobs(unassigned), [])
        self.assign_job(job_id, assigned_id)
        self.assertEqual([job["id"] for job in self.jobs(assigned)], [job_id])
        self.assertIs(self.jobs(assigned)[0]["can_log_time"], True)
        self.create_time(assigned, job_id, minutes=10)
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(job_id), cookie=unassigned)

    def test_common_job_is_usable_by_current_and_future_users_without_assignments(self):
        _, existing = self.worker()
        common = self.job("COMMON-001", is_common="1")
        _, future = self.worker("future-worker@punto.local")
        for cookie in (existing, future):
            with self.subTest(actor=cookie == existing):
                visible = self.jobs(cookie)
                self.assertEqual([job["id"] for job in visible], [common])
                self.assertIs(visible[0]["is_common"], True)
                self.assertIs(visible[0]["can_log_time"], True)
                self.create_time(cookie, common, minutes=20)
                self.assertEqual(self.day(cookie)["total_minutes"], 20)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM job_assignments WHERE job_id=?",
                                        (common,)).fetchone(), (0,))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM job_group_grants WHERE job_id=?",
                                        (common,)).fetchone(), (0,))
        report = self.report("job")
        # A common job grants the right to work, independently from team-hour visibility.
        self.assertEqual((report["summary"]["minutes"], report["summary"]["user_count"]), (0, 0))
        self.assertEqual(self.report("job", cookie=existing)["summary"]["minutes"], 20)
        self.assertEqual(self.report("job", cookie=future)["summary"]["minutes"], 20)

    def test_revoking_common_flag_reapplies_private_assignment_permissions(self):
        assigned_id, assigned = self.worker()
        _, unassigned = self.worker("unassigned@punto.local")
        common = self.job(is_common="1")
        self.assign_job(common, assigned_id)
        self.create_time(unassigned, common, minutes=15)
        self.update_job(common, is_common="0")
        self.assertEqual(self.jobs(unassigned), [])
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(common), cookie=unassigned)
        self.assertEqual([job["id"] for job in self.jobs(assigned)], [common])
        self.create_time(assigned, common, minutes=30)
        self.assertEqual(self.day(unassigned)["total_minutes"], 15)
        self.assertEqual(self.report("job", cookie=unassigned)["summary"]["minutes"], 15)
        update = next(event for event in self.events(common) if event["action"] == "jobs.update")
        self.assertIs(update["before"]["is_common"], True)
        self.assertIs(update["after"]["is_common"], False)

    def test_common_jobs_do_not_expose_private_jobs_or_bypass_disabled_accounts(self):
        user_id, worker = self.worker()
        common = self.job("COMMON-001", is_common="1")
        private = self.job("PRIVATE-001")
        self.assertEqual([job["id"] for job in self.jobs(worker)], [common])
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(private), cookie=worker)
        status, data, _ = self.edit_user(user_id, active="0")
        self.assertEqual(status, 200, data)
        self.assert_error(401, "GET", "/api/jobs", cookie=worker)
        self.assert_error(401, "POST", "/api/time-entries", self.time_fields(common), cookie=worker)

    def test_common_flag_is_strict_and_omission_preserves_previous_value(self):
        for value in ("", "2", "-1", "true", "false", "01", "1.0"):
            with self.subTest(value=value):
                self.assert_error(400, "POST", "/api/jobs", self.job_fields(is_common=value),
                                  cookie=self.manager_cookie)
        job_id = self.job(is_common="1")
        self.assert_error(400, "POST", "/api/jobs/%d" % job_id,
                          self.job_fields(is_common="2"), cookie=self.manager_cookie)
        self.update_job(job_id, name="Nome aggiornato", status="paused")
        job = self.jobs()[0]
        self.assertEqual((job["name"], job["status"]), ("Nome aggiornato", "paused"))
        self.assertIs(job["is_common"], True)
        self.assert_error(400, "POST", "/api/jobs/%d" % job_id, cookie=self.manager_cookie,
                          body="code=DEFAULT-001&name=Commessa&status=active&is_common=0&is_common=1",
                          headers={"Content-Type": "application/x-www-form-urlencoded"})

    def test_worker_cannot_create_common_jobs_change_flag_or_delete_jobs(self):
        _, worker = self.worker()
        job_id = self.job(is_common="1")
        self.create_time(worker, job_id, minutes=15)
        self.assert_error(403, "POST", "/api/jobs", self.job_fields("FORGED", is_common="1"),
                          cookie=worker)
        self.assert_error(403, "POST", "/api/jobs/%d" % job_id,
                          self.job_fields(is_common="0"), cookie=worker)
        for target in (job_id, 999999):
            self.assert_error(403, "DELETE", "/api/jobs/%d" % target, cookie=worker)
        self.assert_error(401, "DELETE", "/api/jobs/%d" % job_id)
        self.assertIs(self.jobs()[0]["is_common"], True)
        self.assertEqual(self.day(worker)["total_minutes"], 15)

    def test_delete_removes_all_owners_hours_and_job_grants_but_preserves_other_records(self):
        worker_id, worker = self.worker()
        other_id, other_worker = self.worker("other-worker@punto.local")
        supplier = self.create_record("suppliers", {"code": "SUP-KEEP", "name": "Fornitore da conservare"})
        contract = self.create_record("contracts", {"code": "CON-KEEP", "title": "Contratto da conservare",
                                                   "supplier_id": supplier, "status": "active"})
        doomed = self.job("DELETE-001", contract_id=contract)
        survivor = self.job("KEEP-001", contract_id=contract)
        self.assign_job(doomed, worker_id)
        self.assign_job(survivor, worker_id)
        group = self.create_record("groups", {"name": "Gruppo da conservare"})
        self.post("/api/groups/%d/members" % group, {"user_id": other_id})
        for member in (self.users()[0]["id"], worker_id):
            self.post("/api/groups/%d/members" % group, {"user_id": member})
        self.post("/api/jobs/%d/group-grants" % doomed, {"group_id": group, "role_id": 2})
        self.post("/api/jobs/%d/group-grants" % survivor, {"group_id": group, "role_id": 2})
        for cookie, minutes in ((self.manager_cookie, 20), (worker, 35), (other_worker, 45)):
            self.create_time(cookie, doomed, minutes=minutes)
        for cookie, minutes in ((self.manager_cookie, 10), (worker, 15), (other_worker, 25)):
            self.create_time(cookie, survivor, minutes=minutes)
        with closing(sqlite3.connect(self.db_path)) as db:
            preserve = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                        for table in ("users", "suppliers", "contracts", "organization_groups", "group_members", "job_roles")}
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
            surviving_entries = db.execute("SELECT * FROM time_entries WHERE job_id=? ORDER BY id", (survivor,)).fetchall()
            surviving_assignments = db.execute("SELECT * FROM job_assignments WHERE job_id=? ORDER BY user_id", (survivor,)).fetchall()
            surviving_grants = db.execute("SELECT * FROM job_group_grants WHERE job_id=? ORDER BY group_id", (survivor,)).fetchall()
        self.delete_job(doomed)
        with closing(sqlite3.connect(self.db_path)) as db:
            for table in ("jobs", "time_entries", "job_assignments", "job_group_grants"):
                column = "id" if table == "jobs" else "job_id"
                self.assertEqual(db.execute('SELECT COUNT(*) FROM "' + table + '" WHERE ' + column + '=?',
                                            (doomed,)).fetchone(), (0,), table)
            for table, rows in preserve.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            self.assertEqual(db.execute("SELECT * FROM time_entries WHERE job_id=? ORDER BY id", (survivor,)).fetchall(), surviving_entries)
            self.assertEqual(db.execute("SELECT * FROM job_assignments WHERE job_id=? ORDER BY user_id", (survivor,)).fetchall(), surviving_assignments)
            self.assertEqual(db.execute("SELECT * FROM job_group_grants WHERE job_id=? ORDER BY group_id", (survivor,)).fetchall(), surviving_grants)
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE action<>'jobs.delete' ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])
        self.assertEqual([job["id"] for job in self.jobs()], [survivor])
        self.assertEqual([job["id"] for job in self.jobs(worker)], [survivor])
        for cookie, remaining in ((self.manager_cookie, 10), (worker, 15), (other_worker, 25)):
            self.assertEqual(self.day(cookie)["total_minutes"], remaining)
        self.assertEqual(self.report("job")["summary"]["minutes"], 50)
        self.assertEqual([row["job_id"] for row in self.report("job")["rows"]], [survivor])
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(doomed), cookie=worker)
        self.assert_error(404, "POST", "/api/time-entries", self.time_fields(doomed), cookie=self.manager_cookie)
        deleted = next(event for event in self.events(doomed) if event["action"] == "jobs.delete")
        self.assertEqual(deleted["before"]["code"], "DELETE-001")
        self.assertNotIn("deleted_minutes", deleted["before"])
        self.assertNotIn("deleted_time_entries", deleted["before"])
        self.assertEqual({key: deleted["before"][key] for key in (
            "deleted_assignments", "deleted_group_grants")},
            {"deleted_assignments": 1, "deleted_group_grants": 1})
        # The immutable internal record remains complete; the public feed hides unscoped hours.
        with closing(sqlite3.connect(self.db_path)) as db:
            stored = json.loads(db.execute("SELECT before_json FROM audit_events WHERE action='jobs.delete' AND entity_id=?",
                                           (doomed,)).fetchone()[0])
        self.assertEqual((stored["deleted_time_entries"], stored["deleted_minutes"]), (3, 100))
        self.assertIsNone(deleted["after"])

    def test_failed_delete_rolls_back_job_hours_grants_and_audit_together(self):
        user_id, worker = self.worker()
        doomed = self.job()
        self.assign_job(doomed, user_id)
        self.create_time(worker, doomed, minutes=45)
        with closing(sqlite3.connect(self.db_path)) as db:
            tables = ("jobs", "time_entries", "job_assignments", "job_group_grants", "audit_events")
            before = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                      for table in tables}
            db.execute("CREATE TRIGGER reject_job_delete_audit BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='jobs.delete' BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            db.commit()
        self.assert_error(500, "DELETE", "/api/jobs/%d" % doomed, cookie=self.manager_cookie)
        with closing(sqlite3.connect(self.db_path)) as db:
            for table, rows in before.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            db.execute("DROP TRIGGER reject_job_delete_audit")
            db.commit()
        self.assertEqual(self.day(worker)["total_minutes"], 45)
        self.delete_job(doomed)
        self.assertEqual(self.day(worker)["entries"], [])

    def test_unknown_delete_is_404_and_deleted_code_can_be_reused(self):
        job_id = self.job(is_common="1")
        before_events = self.events(job_id)
        self.assert_error(404, "DELETE", "/api/jobs/999999", cookie=self.manager_cookie)
        self.assert_error(400, "DELETE", "/api/jobs/%d" % job_id,
                          {"is_common": "0"}, cookie=self.manager_cookie)
        self.assertEqual(self.events(job_id), before_events)
        self.delete_job(job_id)
        self.assert_error(404, "DELETE", "/api/jobs/%d" % job_id, cookie=self.manager_cookie)
        replacement = self.job(is_common="1")
        self.assertNotEqual(replacement, job_id)
        self.assertEqual([job["id"] for job in self.jobs()], [replacement])

    def test_schema_four_upgrade_preserves_records_and_sets_safe_defaults_once(self):
        user_id, worker = self.worker()
        job_id = self.job()
        self.assign_job(job_id, user_id)
        self.create_time(worker, job_id, minutes=45, note="Ore precedenti alle commesse comuni")
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("ALTER TABLE jobs DROP COLUMN is_common")
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' "
                                 "AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (5,6,7,8,9,10,12,13,14)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=4")
            db.commit()
            old_entries = db.execute("SELECT * FROM time_entries ORDER BY id").fetchall()
            old_users = db.execute("SELECT * FROM users ORDER BY id").fetchall()
            old_assignments = db.execute("SELECT * FROM job_assignments ORDER BY job_id,user_id").fetchall()
            old_jobs = db.execute("SELECT id,code,name,client,description,status,contract_id FROM jobs ORDER BY id").fetchall()
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (14,))
            self.assertEqual(db.execute("SELECT * FROM time_entries ORDER BY id").fetchall(), old_entries)
            self.assertEqual(db.execute("SELECT * FROM users ORDER BY id").fetchall(), old_users)
            self.assertEqual(db.execute("SELECT * FROM job_assignments ORDER BY job_id,user_id").fetchall(), old_assignments)
            self.assertEqual(db.execute("SELECT id,code,name,client,description,status,contract_id FROM jobs ORDER BY id").fetchall(), old_jobs)
            self.assertEqual(db.execute("SELECT is_common FROM jobs WHERE id=?", (job_id,)).fetchone(), (0,))
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE NOT(action='system.migration' "
                                        "AND entity_id IN (5,6,7,8,9,10,12,13,14)) ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' "
                                        "AND entity_id=5").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' "
                                        "AND entity_id=6").fetchone(), (1,))
            final_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.assertEqual(self.day(worker)["total_minutes"], 45)
        self.assertIs(self.jobs(worker)[0]["is_common"], False)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT * FROM audit_events ORDER BY id").fetchall(), final_audit)


if __name__ == "__main__":
    unittest.main()
