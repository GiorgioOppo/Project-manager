"""Operational improvements: real HTTP, isolated SQLite fixtures, live scopes and audit."""
from contextlib import closing
import json
import sqlite3
import unittest
import test_roles as roles
import test_work_board as board

PREFIX = "/api/improvements/operations/"


class OperationsImprovementsTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    assert_error = roles.RoleIntegrationTests.assert_error
    today = board.WorkBoardIntegrationTests.today
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time

    def setUp(self):
        roles.RoleIntegrationTests.setUp(self)
        self.manager_id = self.get("/api/session")["user_id"]
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.group = self.post("/api/groups", {"name": "Squadra operativa"}, expected=201)["id"]
        for user in (self.manager_id, self.worker_id):
            self.post(f"/api/groups/{self.group}/members", {"user_id": user}, expected=201)
        self.job_id = self.post("/api/jobs", {"code": "OPS-01", "name": "Operazioni", "status": "active"}, expected=201)["id"]
        self.post(f"/api/jobs/{self.job_id}/assignments", {"user_id": self.worker_id, "role_id": 2}, expected=201)

    def db(self, sql, values=()):
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("PRAGMA foreign_keys=ON")
            result = db.execute(sql, values).fetchall()
            db.commit()
            return result

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def post(self, path, fields, cookie=None, expected=200):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def absence(self, **changes):
        return {"starts_on": "2026-11-02", "ends_on": "2026-11-04", "expected_minutes": 0,
                "reason": "Ferie concordate", **changes}

    def request_absence(self, **changes):
        return self.post(PREFIX + "absences", self.absence(**changes), self.worker_cookie)["id"]

    def decision(self, request, **changes):
        return {"id": request, "revision": 1, "decision": "approved", "reason": "", **changes}

    def schedule(self, **changes):
        return {"user_id": self.worker_id, "starts_on": "2026-11-01", "mon": 240, "tue": 240,
                "wed": 240, "thu": 240, "fri": 240, "sat": 0, "sun": 0, **changes}

    def task(self, **changes):
        return self.post("/api/tasks", {"job_id": self.job_id, "title": "Attività pianificata",
                         "assigned_user_id": self.worker_id, **changes}, expected=201)["id"]

    def test_absence_approval_applies_complete_interval_with_audit(self):
        self.post("/api/work-exceptions", {"user_id": self.worker_id, "date": "2026-11-03",
                  "expected_minutes": 120, "reason": "Parziale"}, expected=201)
        request = self.request_absence()
        self.assertEqual(self.db("SELECT COUNT(*) FROM user_work_exceptions")[0][0], 1)
        self.post(PREFIX + "absence-decision", self.decision(request))
        self.assertEqual(self.db("SELECT date,expected_minutes FROM user_work_exceptions ORDER BY date"),
                         [("2026-11-02", 0), ("2026-11-03", 0), ("2026-11-04", 0)])
        before = self.db("SELECT before_json FROM audit_events WHERE action='work_exceptions.save' AND before_json IS NOT NULL")
        self.assertTrue(any(json.loads(row[0]).get("expected_minutes") == 120 for row in before))
        row = self.get(PREFIX + "absences?status=approved")["requests"][0]
        self.assertEqual((row["status"], row["revision"]), ("approved", 2))
        self.assertTrue(row["decided_at"].endswith("Z"))
        self.assert_error(409, "POST", PREFIX + "absence-decision", self.decision(request), cookie=self.manager_cookie)

    def test_absence_reject_cancel_overlap_and_no_self_approval(self):
        request = self.request_absence()
        self.assert_error(409, "POST", PREFIX + "absences", self.absence(starts_on="2026-11-04"), cookie=self.worker_cookie)
        self.assert_error(403, "POST", PREFIX + "absence-decision", self.decision(request), cookie=self.worker_cookie)
        self.assert_error(400, "POST", PREFIX + "absence-decision", self.decision(request, decision="rejected"), cookie=self.manager_cookie)
        self.post(PREFIX + "absence-decision", self.decision(request, decision="rejected", reason="Periodo non disponibile"))
        second = self.request_absence()
        self.post(PREFIX + "absence-decision", self.decision(second, decision="cancelled"), self.worker_cookie)
        self.assertEqual(self.db("SELECT COUNT(*) FROM user_work_exceptions")[0][0], 0)

    def test_absence_scope_revocation_and_manager_without_delegation(self):
        request = self.request_absence()
        manager = self.create_user(email="area@example.test", role="manager")
        cookie = self.login("area@example.test", roles.WORKER_PASSWORD)
        self.assertEqual(self.get(PREFIX + "absences", cookie)["requests"], [])
        self.post(f"/api/groups/{self.group}/members", {"user_id": manager}, expected=201)
        queue = self.get(PREFIX + "absences", cookie)["requests"]
        self.assertFalse(queue[0]["can_decide"])
        self.assert_error(403, "POST", PREFIX + "absence-decision", self.decision(request), cookie=cookie)
        self.db("INSERT INTO access_area_delegations(manager_id,group_id,can_manage_users,starts_on,reason,created_by,created_at) VALUES(?,?,1,'2026-01-01','Fixture',?,'2026-10-11T00:00:00Z')", (manager, self.group, self.manager_id))
        self.assertTrue(self.get(PREFIX + "absences", cookie)["requests"][0]["can_decide"])
        self.db("DELETE FROM group_members WHERE user_id=?", (self.worker_id,))
        self.assertEqual(self.get(PREFIX + "absences", cookie)["requests"], [])
        self.assert_error(404, "POST", PREFIX + "absence-decision", self.decision(request), cookie=cookie)

    def test_absence_audit_failure_rolls_back_all_days_and_decision(self):
        request = self.request_absence()
        self.db("CREATE TRIGGER fail_absence_audit BEFORE INSERT ON audit_events WHEN NEW.action='absence_requests.decide' BEGIN SELECT RAISE(ABORT,'fixture failure'); END")
        self.assert_error(500, "POST", PREFIX + "absence-decision", self.decision(request), cookie=self.manager_cookie)
        self.assertEqual(self.db("SELECT COUNT(*) FROM user_work_exceptions")[0][0], 0)
        self.assertEqual(self.db("SELECT status,revision FROM operations_absences"), [("pending", 1)])

    def test_absence_validation_and_pagination(self):
        for changes in ({"ends_on": "2026-10-31"}, {"ends_on": "2028-11-04"}, {"expected_minutes": 1441}, {"reason": "  "}, {"starts_on": "2026-02-30"}, {"user_id": self.manager_id}):
            self.assert_error(400, "POST", PREFIX + "absences", self.absence(**changes), cookie=self.worker_cookie)
        for day in ("2026-11-02", "2026-11-03", "2026-11-04"):
            self.request_absence(starts_on=day, ends_on=day)
        first = self.get(PREFIX + "absences?limit=2")
        self.assertTrue(first["has_more"])
        last = self.get(PREFIX + "absences?limit=2&after_id=" + str(first["next_after_id"]))
        self.assertFalse(last["has_more"])
        self.assertEqual(len(first["requests"]) + len(last["requests"]), 3)

    def test_task_estimate_revision_and_capacity_calendar_precedence(self):
        task = self.task(due_date="2026-11-03")
        self.post("/api/work-schedules", self.schedule(), expected=201)
        self.post("/api/business-holidays", {"date": "2026-11-03", "name": "Chiusura"}, expected=201)
        self.post("/api/work-exceptions", {"user_id": self.worker_id, "date": "2026-11-03", "expected_minutes": 60, "reason": "Turno"}, expected=201)
        self.post(PREFIX + "task-plan", {"task_id": task, "revision": 1, "estimated_minutes": 180})
        self.assert_error(409, "POST", PREFIX + "task-plan", {"task_id": task, "revision": 1, "estimated_minutes": 90}, cookie=self.manager_cookie)
        self.assert_error(404, "POST", PREFIX + "task-plan", {"task_id": task, "revision": 2, "estimated_minutes": 90}, cookie=self.worker_cookie)
        self.create_time(self.worker_cookie, self.job_id, date="2026-11-02", minutes=120)
        data = self.get(PREFIX + "capacity?from=2026-11-02&to=2026-11-08", self.worker_cookie)
        self.assertEqual((data["expected_minutes"], data["planned_minutes"], data["recorded_minutes"], data["available_minutes"]), (1020, 180, 120, 840))
        board_data = self.get("/api/work-board?job_id=" + str(self.job_id))
        self.assertEqual(board_data["tasks"][0]["estimated_minutes"], 180)
        self.assertEqual(board_data["tasks"][0]["revision"], 2)

    def test_capacity_live_scope_and_completed_tasks(self):
        task = self.task()
        self.post(PREFIX + "task-plan", {"task_id": task, "revision": 1, "estimated_minutes": 500})
        self.post(f"/api/tasks/{task}", {"revision": 2, "status": "done"}, expected=200)
        data = self.get(PREFIX + f"capacity?user_id={self.worker_id}&from=2026-11-02&to=2026-11-08")
        self.assertEqual(data["planned_minutes"], 0)
        self.db("DELETE FROM group_members WHERE user_id=?", (self.worker_id,))
        self.assert_error(403, "GET", PREFIX + f"capacity?user_id={self.worker_id}&from=2026-11-02&to=2026-11-08", cookie=self.manager_cookie)
        self.assert_error(400, "GET", PREFIX + "capacity?from=2026-11-02&to=2028-11-02", cookie=self.worker_cookie)

    def test_capacity_drops_revoked_project_estimates_immediately(self):
        task = self.task()
        self.post(PREFIX + "task-plan", {"task_id": task, "revision": 1, "estimated_minutes": 500})
        url = PREFIX + "capacity?from=2026-11-02&to=2026-11-08"
        self.assertEqual(self.get(url, self.worker_cookie)["planned_minutes"], 500)
        self.db("DELETE FROM job_assignments WHERE user_id=? AND job_id=?", (self.worker_id, self.job_id))
        self.assertEqual(self.get(url, self.worker_cookie)["planned_minutes"], 0)
        self.assertEqual(self.get(url + "&user_id=" + str(self.worker_id))["planned_minutes"], 0)

    def test_schedule_revision_conflict_and_complete_before_after(self):
        identifier = self.post("/api/work-schedules", self.schedule(), expected=201)["id"]
        self.assert_error(409, "POST", "/api/work-schedules", self.schedule(fri=120), cookie=self.manager_cookie)
        self.post("/api/work-schedules", self.schedule(revision=1, fri=120))
        self.assert_error(409, "POST", "/api/work-schedules", self.schedule(revision=1, fri=30), cookie=self.manager_cookie)
        rows = self.get("/api/work-schedules?user_id=" + str(self.worker_id))["schedules"]
        self.assertEqual((rows[0]["id"], rows[0]["revision"], rows[0]["fri"]), (identifier, 2, 120))
        before, after = self.db("SELECT before_json,after_json FROM audit_events WHERE action='work_schedules.save' ORDER BY id DESC LIMIT 1")[0]
        self.assertEqual((json.loads(before)["fri"], json.loads(after)["fri"]), (240, 120))
        self.assertEqual((json.loads(before)["revision"], json.loads(after)["revision"]), (1, 2))

    def test_schedule_area_permissions_and_privacy(self):
        outsider = self.create_user(email="outside@example.test")
        self.post("/api/work-schedules", self.schedule(user_id=outsider), expected=201)
        self.assertEqual(self.get("/api/work-schedules")["schedules"], [])
        manager = self.create_user(email="outside-manager@example.test", role="manager")
        cookie = self.login("outside-manager@example.test", roles.WORKER_PASSWORD)
        self.assert_error(403, "POST", "/api/work-schedules", self.schedule(), cookie=cookie)
        self.assertEqual(self.get("/api/work-schedules?user_id=" + str(outsider), cookie)["schedules"], [])

    def test_templates_version_archive_restore_and_stale_conflict(self):
        definition = {"name": "Commessa proposta", "client": "Cliente", "tasks": [{"title": "Fase iniziale"}]}
        template = self.post("/api/job-templates", {"name": "Modello originale", "definition": json.dumps(definition)}, expected=201)["id"]
        fields = {"id": template, "name": "Modello rivisto", "definition": json.dumps({**definition, "client": "Altro cliente"}), "archived": 1, "revision": 1}
        self.post(PREFIX + "templates", fields)
        self.assertEqual(self.get("/api/job-templates")["templates"], [])
        history = self.get(PREFIX + f"templates?id={template}&history=1")["templates"]
        self.assertEqual([item["revision"] for item in history], [2, 1])
        self.assert_error(409, "POST", PREFIX + "templates", fields, cookie=self.manager_cookie)
        self.post(PREFIX + "templates", {**fields, "name": history[1]["name"], "definition": history[1]["definition_json"], "archived": 0, "revision": 2})
        row = self.get("/api/job-templates")["templates"][0]
        self.assertEqual((row["revision"], row["name"]), (3, "Modello originale"))
        self.assertEqual(json.loads(row["definition_json"]), definition)

    def test_template_owner_isolation_duplicate_and_audit_failure(self):
        first = self.post("/api/job-templates", {"name": "Primo", "definition": "{}"}, expected=201)["id"]
        self.post("/api/job-templates", {"name": "Secondo", "definition": "{}"}, expected=201)
        fields = {"id": first, "name": "Secondo", "definition": "{}", "archived": 0, "revision": 1}
        self.assert_error(409, "POST", PREFIX + "templates", fields, cookie=self.manager_cookie)
        worker_rows = self.get(PREFIX + "templates", self.worker_cookie)["templates"]
        self.assertEqual(worker_rows, [])
        self.assert_error(404, "POST", PREFIX + "templates", {**fields, "name": "Terzo"}, cookie=self.worker_cookie)
        self.db("CREATE TRIGGER fail_template_audit BEFORE INSERT ON audit_events WHEN NEW.action='job_templates.update' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        self.assert_error(500, "POST", PREFIX + "templates", {**fields, "name": "Terzo"}, cookie=self.manager_cookie)
        self.assertEqual(self.db("SELECT revision,name FROM job_templates WHERE id=?", (first,)), [(1, "Primo")])
        self.assertEqual(self.db("SELECT COUNT(*) FROM operations_template_versions")[0][0], 0)

    def correction(self):
        entry = self.create_time(self.worker_cookie, self.job_id, date=self.today(), minutes=60)
        return self.post("/api/time-corrections", {"entry_id": entry, "revision": 1, "job_id": self.job_id,
                         "date": self.today(), "minutes": 30, "note": "Rettifica", "reason": "Durata", "delete_requested": 0}, self.worker_cookie, 201)["id"]

    def test_checklist_links_pending_corrections_revision_and_closure_ack(self):
        self.task()
        request = self.correction()
        fields = {"job_id": self.job_id, "kind": "ticket", "resource_id": 42, "label": "Collaudo cliente", "status": "open", "revision": 0}
        self.post(PREFIX + "checklist", fields)
        data = self.get(PREFIX + "checklist?job_id=" + str(self.job_id))
        self.assertEqual((data["open_tasks"], data["pending_corrections"], data["unresolved_count"]), (1, 1, 3))
        self.assert_error(409, "POST", PREFIX + "checklist", fields, cookie=self.manager_cookie)
        self.post(PREFIX + "checklist", {**fields, "status": "done", "revision": 1})
        lifecycle = {"revision": 1, "action": "close", "reason": "Verifica conclusa"}
        self.assert_error(409, "POST", f"/api/jobs/{self.job_id}/lifecycle", lifecycle, cookie=self.manager_cookie)
        self.post(f"/api/jobs/{self.job_id}/lifecycle", {**lifecycle, "checklist_confirmed": 1})
        self.assertEqual(self.db("SELECT time_closed FROM jobs WHERE id=?", (self.job_id,)), [(1,)])
        self.assertEqual(self.db("SELECT status FROM time_corrections WHERE id=?", (request,)), [("pending",)])

    def test_checklist_readiness_and_no_global_manager_access(self):
        data = self.get(PREFIX + "checklist?job_id=" + str(self.job_id))
        self.assertTrue(data["ready"])
        manager = self.create_user(email="unscoped@example.test", role="manager")
        cookie = self.login("unscoped@example.test", roles.WORKER_PASSWORD)
        self.assert_error(403, "GET", PREFIX + "checklist?job_id=" + str(self.job_id), cookie=cookie)
        self.assert_error(403, "GET", f"/api/jobs/{self.job_id}/closure", cookie=cookie)
        self.assert_error(404, "GET", "/api/work-board?job_id=" + str(self.job_id), cookie=cookie)

    def test_corrections_filter_cursor_over_thousand_and_no_snapshot_leak(self):
        request = self.correction()
        columns = "entry_id,original_entry_id,requester_id,entry_revision,original_job_id,original_date,original_minutes,original_note,before_json,job_id,date,minutes,note,reason,delete_requested"
        for _ in range(1001):
            self.db(f"INSERT INTO time_corrections({columns},status) SELECT {columns},'cancelled' FROM time_corrections WHERE id=?", (request,))
        first = self.get("/api/time-corrections?limit=100")
        self.assertEqual(len(first["requests"]), 100)
        self.assertTrue(first["has_more"])
        second = self.get("/api/time-corrections?limit=100&after_id=" + str(first["next_after_id"]))
        self.assertLess(max(row["id"] for row in second["requests"]), min(row["id"] for row in first["requests"]))
        pending = self.get("/api/time-corrections?status=pending&limit=25")
        self.assertEqual([row["id"] for row in pending["requests"]], [request])
        self.db("DELETE FROM group_members WHERE user_id=?", (self.worker_id,))
        self.assertEqual(self.get("/api/time-corrections?status=pending")["requests"], [])
        self.assert_error(403, "GET", "/api/time-corrections?user_id=" + str(self.worker_id), cookie=self.manager_cookie)

    def test_area_wizard_requires_a_group_within_current_delegation(self):
        manager = self.create_user(email="wizard-area@example.test", role="manager")
        cookie = self.login("wizard-area@example.test", roles.WORKER_PASSWORD)
        self.post("/api/improvements/governance/delegations", {"user_id": manager, "group_id": self.group,
                  "can_manage_jobs": 1, "can_manage_groups": 1, "starts_on": "2026-01-01", "reason": "Area fixture"}, expected=201)
        fields = {"code": "AREA-NEW", "name": "Commessa area", "status": "active", "request_key": "area-wizard-request-1", "groups": "[]", "tasks": "[]", "assignments": "[]"}
        self.assert_error(403, "POST", "/api/jobs/wizard", fields, cookie=cookie)
        self.assertEqual(self.db("SELECT COUNT(*) FROM jobs WHERE code='AREA-NEW'")[0][0], 0)
        fields["groups"] = json.dumps([{"group_id": self.group, "role_id": 2, "starts_on": "", "ends_on": ""}])
        identifier = self.post("/api/jobs/wizard", fields, cookie, expected=201)["id"]
        self.assertTrue(any(job["id"] == identifier for job in self.get("/api/jobs", cookie)["jobs"]))
        self.post("/api/tasks", {"job_id": identifier, "title": "Attività area"}, cookie, expected=201)
        self.db("UPDATE access_area_delegations SET active=0 WHERE manager_id=?", (manager,))
        self.assert_error(404, "GET", "/api/work-board?job_id=" + str(identifier), cookie=cookie)

    def test_batch_server_error_identifies_row_and_day_without_partial_save(self):
        self.create_time(self.worker_cookie, self.job_id, date=self.today(), minutes=1400)
        rows = [{"job_id": self.job_id, "date": self.today(), "minutes": minutes, "note": "Batch"} for minutes in (20, 30)]
        status, data, _ = self.api("POST", "/api/time-entries/batch", {"request_key": "batch-error-row-1", "entries": json.dumps(rows)}, cookie=self.worker_cookie)
        self.assertEqual(status, 409, data)
        self.assertEqual((data["row"], data["date"]), (2, self.today()))
        self.assertEqual(self.db("SELECT COUNT(*),SUM(minutes) FROM time_entries"), [(1, 1400)])

    def test_routes_unknown_fields_auth_and_validation(self):
        self.assert_error(401, "GET", PREFIX + "absences", cookie=None)
        self.assert_error(405, "DELETE", PREFIX + "absences", cookie=self.manager_cookie)
        self.assert_error(400, "POST", PREFIX + "absences?x=1", self.absence(), cookie=self.worker_cookie)
        self.assert_error(404, "GET", PREFIX + "missing", cookie=self.manager_cookie)
        self.assert_error(400, "GET", PREFIX + "checklist?job_id=1&unexpected=1", cookie=self.manager_cookie)
        for invalid in ("%0A1", "%2B1", "%FF", "%ED%A0%80", "%F4%90%80%80", "%E2%82", "%C0%AF"):
            with self.subTest(encoded_value=invalid):
                self.assert_error(400, "GET", PREFIX + "checklist?job_id=" + invalid, cookie=self.manager_cookie)
        self.assert_error(400, "GET", "/api/time-corrections?limit=101", cookie=self.worker_cookie)
        self.assert_error(400, "GET", "/api/time-corrections?status=invalid", cookie=self.worker_cookie)
