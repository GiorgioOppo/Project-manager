"""Weekly, task-linked, atomic and guided user workflows over isolated HTTP."""
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
import json
import unittest
import test_work_board as board


class UserWorkflowTests(unittest.TestCase):
    setUp = board.WorkBoardIntegrationTests.setUp
    start_server = board.WorkBoardIntegrationTests.start_server
    stop_server = board.WorkBoardIntegrationTests.stop_server
    api = board.WorkBoardIntegrationTests.api
    login = board.WorkBoardIntegrationTests.login
    assert_error = board.WorkBoardIntegrationTests.assert_error
    create_user = board.WorkBoardIntegrationTests.create_user
    today = board.WorkBoardIntegrationTests.today
    worker = board.WorkBoardIntegrationTests.worker
    get = board.WorkBoardIntegrationTests.get
    job = board.WorkBoardIntegrationTests.job
    assign = board.WorkBoardIntegrationTests.assign
    task = board.WorkBoardIntegrationTests.task
    blocker = board.WorkBoardIntegrationTests.blocker
    time_fields = board.WorkBoardIntegrationTests.time_fields
    create_time = board.WorkBoardIntegrationTests.create_time

    def post(self, path, fields, expected=201, cookie=None):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), expected < 400)
        return data

    def batch(self, rows, key="batch-test-0001", cookie=None, expected=201):
        return self.post("/api/time-entries/batch", {"request_key": key, "entries": json.dumps(rows)},
                         expected, cookie)

    def row(self, job, **changes):
        return {"job_id": job, "date": self.today(), "minutes": 30, "note": "Lavoro", **changes}

    def wizard(self, **changes):
        fields = {"request_key": "wizard-test-0001", "code": "GUIDED-1", "name": "Avvio guidato",
                  "status": "active", "assignments": "[]", "groups": "[]", "tasks": "[]", **changes}
        return fields

    def test_week_crosses_year_and_keeps_multiple_notes(self):
        job = self.job()
        self.create_time(self.manager_cookie, job, date="2026-12-31", minutes=30, note="Prima nota")
        self.create_time(self.manager_cookie, job, date="2026-12-31", minutes=15, note="Seconda nota")
        self.create_time(self.manager_cookie, job, date="2027-01-03", minutes=10)
        data = self.get("/api/time-entries/week?date=2027-01-01")
        self.assertEqual((data["start"], data["end"]), ("2026-12-28", "2027-01-03"))
        self.assertEqual(data["total_minutes"], 55)
        self.assertEqual(len(data["entries"]), 3)
        self.assertEqual(data["days"][3]["total_minutes"], 45)
        self.assertTrue(all(entry["revision"] == 1 for entry in data["entries"]))

    def test_week_leap_day_boundaries_and_bad_dates(self):
        self.assertEqual(self.get("/api/time-entries/week?date=2028-02-29")["start"], "2028-02-28")
        self.assertEqual(self.get("/api/time-entries/week?date=0001-01-01")["start"], "0001-01-01")
        for date in ("2026-02-29", "9999-12-31", "0000-01-01"):
            self.assert_error(400, "GET", "/api/time-entries/week?date=" + date, cookie=self.manager_cookie)

    def test_batch_replay_is_idempotent_and_payload_key_conflicts(self):
        job = self.job()
        rows = [self.row(job), self.row(job, minutes=45, note="Seconda")]
        first = self.batch(rows)
        repeat = self.batch(rows, expected=200)
        self.assertEqual(first["ids"], repeat["ids"])
        self.assertTrue(repeat["replayed"])
        self.assertEqual(self.get("/api/time-entries?date=" + self.today())["total_minutes"], 75)
        self.batch([self.row(job, minutes=60)], expected=409)

    def test_batch_is_atomic_when_final_day_total_exceeds_limit(self):
        job = self.job()
        self.batch([self.row(job, minutes=1000), self.row(job, minutes=500)], expected=409)
        self.assertEqual(self.get("/api/time-entries?date=" + self.today())["entries"], [])
        self.batch([self.row(job, minutes=1440)])

    def test_batch_validates_all_rows_and_size_without_partial_save(self):
        job = self.job()
        for rows in ([], [self.row(job)] * 101, [self.row(job), self.row(job, date="2026-02-30")],
                     [self.row(job, minutes=0)], [self.row(job, note="x" * 1001)],
                     [self.row(job, minutes=1.5)], [self.row(job, unexpected="field")]):
            self.batch(rows, expected=400)
        self.assertEqual(self.get("/api/time-entries?date=" + self.today())["entries"], [])

    def test_worker_batch_week_and_revocation_are_scoped(self):
        job = self.job()
        user, cookie = self.worker()
        self.assign(job, user)
        self.batch([self.row(job)], cookie=cookie)
        self.assertEqual(self.get("/api/time-entries/week?date=" + self.today(), cookie)["total_minutes"], 30)
        self.assertEqual(self.get("/api/time-entries/week?date=" + self.today())["total_minutes"], 0)
        status, _, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (job, user), cookie=self.manager_cookie)
        self.assertEqual(status, 200)
        self.batch([self.row(job)], key="batch-revoked-0001", cookie=cookie, expected=403)
        self.assertEqual(self.get("/api/time-entries/week?date=" + self.today(), cookie)["total_minutes"], 30)

    def test_full_batch_accepts_maximum_unicode_notes_over_legacy_body_limit(self):
        job = self.job()
        note = "🔧" * 1000
        rows = [self.row(job, minutes=1, note=note) for _ in range(100)]
        self.assertEqual(len(self.batch(rows, key="batch-large-unicode-0001")["ids"]), 100)
        saved = self.get("/api/time-entries/week?date=" + self.today())
        self.assertEqual((len(saved["entries"]), saved["total_minutes"]), (100, 100))
        self.assertTrue(all(entry["note"] == note for entry in saved["entries"]))

    def test_task_link_validates_job_and_is_in_daily_weekly_history(self):
        job, other = self.job("FIRST"), self.job("OTHER")
        task = self.task(job)
        self.batch([self.row(other, task_id=task)], expected=400)
        entry = self.batch([self.row(job, task_id=task)])["ids"][0]
        day = self.get("/api/time-entries?date=" + self.today())["entries"][0]
        self.assertEqual((day["id"], day["task_id"], day["task_title"]), (entry, task, "Attività di prova"))
        self.assertEqual(self.get("/api/time-entries/week?date=" + self.today())["entries"][0]["task_id"], task)
        self.post("/api/tasks/%d/delete" % task, {"revision": 1}, 200)
        self.assertIsNone(self.get("/api/time-entries?date=" + self.today())["entries"][0]["task_id"])

    def test_direct_edit_revision_conflict_and_task_preservation(self):
        job = self.job()
        task = self.task(job)
        entry = self.post("/api/time-entries", self.row(job, task_id=task))["id"]
        self.post("/api/time-entries/%d" % entry, self.row(job, minutes=45, revision=1), 200)
        saved = self.get("/api/time-entries?date=" + self.today())["entries"][0]
        self.assertEqual((saved["revision"], saved["task_id"]), (2, task))
        self.post("/api/time-entries/%d" % entry, self.row(job, minutes=60, revision=1), 409)

    def test_wizard_creates_job_access_and_tasks_and_retry_once(self):
        user, cookie = self.worker()
        fields = self.wizard(assignments=json.dumps([{"user_id": user, "role_id": 2}]),
                             tasks=json.dumps([{"title": "Primo passo", "assigned_user_id": user}]))
        job = self.post("/api/jobs/wizard", fields)["id"]
        self.assertEqual(self.post("/api/jobs/wizard", fields, 200)["id"], job)
        self.assertEqual(self.get("/api/jobs", cookie)["jobs"][0]["id"], job)
        self.assertEqual(self.get("/api/work-board?job_id=%d" % job, cookie)["tasks"][0]["title"], "Primo passo")
        notices = self.get("/api/work-notifications", cookie)["notifications"]
        self.assertEqual(len([n for n in notices if n["type"] == "assigned"]), 1)
        self.assertEqual(notices[0]["message"], "Primo passo")

    def test_wizard_validation_rolls_back_job_and_access(self):
        user, _ = self.worker()
        for fields in (self.wizard(assignments=json.dumps([{"user_id": user, "role_id": 9999}])),
                       self.wizard(tasks=json.dumps([{"title": "Senza accesso", "assigned_user_id": user}]))):
            self.post("/api/jobs/wizard", fields, 400)
            self.assertEqual(self.get("/api/jobs")["jobs"], [])

        self.post("/api/jobs/wizard", self.wizard(contract_id=99999), 403)
        self.assertEqual(self.get("/api/jobs")["jobs"], [])

    def test_templates_are_explicit_and_restricted_to_setup_data(self):
        definition = {"name": "Assistenza", "status": "active", "tasks": [{"title": "Avvio"}]}
        self.post("/api/job-templates", {"name": "Modello", "definition": json.dumps(definition)})
        saved = self.get("/api/job-templates")["templates"][0]
        self.assertEqual(json.loads(saved["definition_json"]), definition)
        self.post("/api/job-templates", {"name": "Modello", "definition": json.dumps(definition)}, 409)
        self.post("/api/job-templates", {"name": "Vietato", "definition": '{"hours":[8]}'}, 400)
        _, cookie = self.worker()
        self.assert_error(403, "GET", "/api/job-templates", cookie=cookie)
        self.assert_error(403, "POST", "/api/jobs/wizard", self.wizard(), cookie=cookie)

    def test_personal_queue_follows_assignment_and_live_permission(self):
        job = self.job()
        user, cookie = self.worker()
        self.assign(job, user)
        task = self.task(job, assigned_user_id=user)
        self.blocker(job, cookie=cookie, title="Serve un dato")
        work = self.get("/api/my-work", cookie)
        self.assertEqual(work["tasks"][0]["id"], task)
        self.assertEqual(work["blockers"][0]["title"], "Serve un dato")
        status, _, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (job, user), cookie=self.manager_cookie)
        self.assertEqual(status, 200)
        self.assertEqual(self.get("/api/my-work", cookie)["tasks"], [])
        self.assertEqual(self.get("/api/my-work", cookie)["blockers"], [])

    def test_advanced_routes_require_authentication_and_origin(self):
        for path in ("/api/my-work", "/api/job-templates", "/api/time-entries/week?date=" + self.today()):
            self.assert_error(401, "GET", path)
        job = self.job()
        self.assert_error(403, "POST", "/api/time-entries/batch", {"request_key": "csrf-check-0001", "entries": json.dumps([self.row(job)])},
                          cookie=self.manager_cookie, headers={"Origin": "https://invalid.example"})
