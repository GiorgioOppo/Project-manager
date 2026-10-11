"""Commessa closure/archive preserve work and enforce revisions over real HTTP."""

from contextlib import closing
import sqlite3
import unittest

import test_roles as roles


class JobLifecycleIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    create_job = roles.RoleIntegrationTests.create_job
    assign_job = roles.RoleIntegrationTests.assign_job
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    day = roles.RoleIntegrationTests.day

    def closure(self, job_id, cookie=None):
        status, data, _ = self.api("GET", "/api/jobs/%d/closure" % job_id,
                                   cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def lifecycle(self, job_id, action, revision=None, reason="Verificato dal gestore"):
        if revision is None:
            revision = self.closure(job_id)["lifecycle_revision"]
        status, data, _ = self.api("POST", "/api/jobs/%d/lifecycle" % job_id,
                                   {"action": action, "revision": revision, "reason": reason, "checklist_confirmed": "1"},
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data["ok"], True)
        return data

    def test_close_archive_reopen_preserve_personal_hours_and_board(self):
        worker_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, data, _ = self.api("GET", "/api/users", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        manager_id = next(user["id"] for user in data["users"] if user["role"] == "manager")
        status, data, _ = self.api("POST", "/api/groups", {"name": "Squadra chiusura"}, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        group = data["id"]
        for user in (manager_id, worker_id):
            status, data, _ = self.api("POST", "/api/groups/%d/members" % group,
                {"user_id": user}, cookie=self.manager_cookie)
            self.assertEqual(status, 201, data)
        job_id = self.create_job()
        self.assign_job(job_id, worker_id)
        entry_id = self.create_time(worker, job_id, minutes=75, note="Da conservare")
        self.create_time(self.manager_cookie, job_id, minutes=25)
        for endpoint, title in (("tasks", "Attività aperta"), ("blockers", "Blocco aperto")):
            status, data, _ = self.api("POST", "/api/" + endpoint,
                                       {"job_id": job_id, "title": title}, cookie=self.manager_cookie)
            self.assertEqual(status, 201, data)
        initial = self.closure(job_id)
        self.assertEqual((initial["open_tasks"], initial["open_blockers"], initial["time_entries"],
                          initial["total_minutes"], initial["users_with_hours"]), (1, 1, 2, 100, 2))
        self.assertEqual(initial["lifecycle_revision"], 1)
        closed = self.lifecycle(job_id, "close", reason="  Fine rendicontazione  ")
        self.assertTrue(closed["time_closed"])
        self.assertFalse(closed["archived"])
        self.assertEqual(closed["reason"], "Fine rendicontazione")
        for cookie in (worker, self.manager_cookie):
            self.assert_error(409, "POST", "/api/time-entries", self.time_fields(job_id), cookie=cookie)
        self.assertEqual(self.day(worker)["entries"][0]["id"], entry_id)
        archived = self.lifecycle(job_id, "archive")
        self.assertTrue(archived["time_closed"])
        self.assertTrue(archived["archived"])
        self.assertEqual(archived["total_minutes"], 100)
        status, board, _ = self.api("GET", "/api/work-board?job_id=%d" % job_id, cookie=worker)
        self.assertEqual(status, 200, board)
        self.assertEqual(len(board["tasks"]), 1)
        self.assertEqual(len(board["blockers"]), 1)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assertTrue(self.closure(job_id)["archived"])
        self.assertEqual(self.day(worker)["total_minutes"], 75)
        reopened = self.lifecycle(job_id, "reopen")
        self.assertFalse(reopened["time_closed"])
        self.assertFalse(reopened["archived"])
        self.assertEqual(reopened["lifecycle_revision"], 4)
        self.create_time(worker, job_id, minutes=15)
        status, data, _ = self.api("GET", "/api/audit?limit=100", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        actions = [event["action"] for event in data["events"]]
        for action in ("jobs.close", "jobs.archive", "jobs.reopen"):
            self.assertEqual(actions.count(action), 1)

    def test_api_requires_manager_origin_reason_and_current_revision(self):
        worker_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        job_id = self.create_job()
        self.assign_job(job_id, worker_id)
        path = "/api/jobs/%d/lifecycle" % job_id
        fields = {"action": "close", "revision": 1, "reason": "Fine lavoro"}
        self.assert_error(401, "GET", "/api/jobs/%d/closure" % job_id)
        self.assert_error(401, "POST", path, fields)
        self.assert_error(403, "GET", "/api/jobs/%d/closure" % job_id, cookie=worker)
        self.assert_error(403, "POST", path, fields, cookie=worker)
        self.assert_error(403, "POST", path, fields, cookie=self.manager_cookie,
                          headers={"Origin": "https://attacker.example"})
        for changes in ({"reason": "  "}, {"reason": "x" * 1001}, {"revision": 0},
                        {"revision": "1x"}, {"action": "delete"}, {"unexpected": "yes"}):
            self.assert_error(400, "POST", path, dict(fields, **changes), cookie=self.manager_cookie)
        self.assert_error(404, "GET", "/api/jobs/999999/closure", cookie=self.manager_cookie)
        self.assert_error(400, "GET", "/api/jobs/x/closure", cookie=self.manager_cookie)
        self.assert_error(400, "GET", "/api/jobs/%d/closure?all=1" % job_id, cookie=self.manager_cookie)
        self.lifecycle(job_id, "archive", revision=1)
        self.assert_error(409, "POST", path, fields, cookie=self.manager_cookie)
        self.assert_error(409, "POST", path, dict(fields, revision=2), cookie=self.manager_cookie)
        self.assertEqual(self.closure(job_id)["lifecycle_revision"], 2)

    def test_audit_failure_rolls_back_flags_and_revision(self):
        job_id = self.create_job()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER reject_lifecycle_audit BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='jobs.close' BEGIN SELECT RAISE(ABORT,'test audit failure'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/jobs/%d/lifecycle" % job_id,
                          {"revision": 1, "action": "close", "reason": "Fine"}, cookie=self.manager_cookie)
        state = self.closure(job_id)
        self.assertEqual(state["lifecycle_revision"], 1)
        self.assertFalse(state["time_closed"])

    def test_schema10_upgrade_preserves_hours_and_defaults(self):
        job_id = self.create_job()
        entry_id = self.create_time(self.manager_cookie, job_id, minutes=42)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            for table in ("work_notifications", "time_corrections", "time_entry_batches", "job_templates",
                          "business_holidays", "user_work_exceptions", "user_work_schedules"):
                db.execute("DROP TABLE " + table)
            db.execute("DROP INDEX time_entries_task")
            for table, column in (("time_entries", "task_id"), ("time_entries", "revision"),
                                  ("jobs", "time_closed"), ("jobs", "archived"), ("jobs", "lifecycle_revision")):
                db.execute("ALTER TABLE " + table + " DROP COLUMN " + column)
            db.execute("PRAGMA user_version=10")
            db.commit()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        state = self.closure(job_id)
        self.assertEqual((state["lifecycle_revision"], state["total_minutes"]), (1, 42))
        self.assertFalse(state["time_closed"])
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 14)
            self.assertEqual(db.execute("SELECT id,revision,task_id FROM time_entries").fetchone(), (entry_id, 1, None))


if __name__ == "__main__":
    unittest.main()
