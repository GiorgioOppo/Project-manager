"""Personal schedules, calendar overrides and actionable work notifications."""

from contextlib import closing
from datetime import datetime, timedelta
import sqlite3
import unittest
from zoneinfo import ZoneInfo

import test_alerts as alerts
import test_reports as reports
import test_roles as roles


class WorkSchedulesIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = reports.ReportsIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    assign_job = roles.RoleIntegrationTests.assign_job
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    assert_error = roles.RoleIntegrationTests.assert_error
    rule_fields = alerts.AlertIntegrationTests.rule_fields
    create_rule = alerts.AlertIntegrationTests.create_rule
    run_rule = alerts.AlertIntegrationTests.run_rule
    notifications = alerts.AlertIntegrationTests.notifications

    def today(self, offset=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=offset)).isoformat()

    def post(self, path, fields, cookie=None, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def seed(self):
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        group = self.post("/api/groups", {"name": "Squadra turni"})["id"]
        for member in (self.users()[0]["id"], self.worker_id):
            self.post(f"/api/groups/{group}/members", {"user_id": member})
        self.job_id = self.post("/api/jobs", {"code": "SCHED-1", "name": "Lavoro pianificato", "status": "active"})["id"]
        self.assign_job(self.job_id, self.worker_id)

    def schedule(self, **changes):
        return {"user_id": self.worker_id, "starts_on": "2026-01-01", "mon": 240, "tue": 240,
                "wed": 240, "thu": 240, "fri": 240, "sat": 0, "sun": 0, **changes}

    def db(self, sql, values=()):
        with closing(sqlite3.connect(self.db_path)) as connection:
            result = connection.execute(sql, values).fetchall()
            connection.commit()
            return result

    def test_personal_schedule_history_scope_and_upsert(self):
        self.seed()
        self.assert_error(403, "POST", "/api/work-schedules", self.schedule(), cookie=self.worker_cookie)
        first = self.post("/api/work-schedules", self.schedule())["id"]
        second = self.post("/api/work-schedules", self.schedule(starts_on="2026-10-01", fri=120))["id"]
        self.assertNotEqual(first, second)
        self.post("/api/work-schedules", self.schedule(starts_on="2026-10-01", fri=180), expected=200)
        own = self.get("/api/work-schedules", self.worker_cookie)["schedules"]
        self.assertEqual([row["id"] for row in own], [second, first])
        self.assertEqual([row["fri"] for row in own], [180, 240])
        other = self.create_user(email="schedule-other@example.test")
        self.post("/api/work-schedules", self.schedule(user_id=other))
        self.assertEqual(len(self.get("/api/work-schedules")["schedules"]), 3)
        self.assertEqual(len(self.get("/api/work-schedules?user_id=%d" % self.worker_id)["schedules"]), 2)
        self.assert_error(403, "GET", "/api/work-schedules?user_id=%d" % other, cookie=self.worker_cookie)

    def test_exceptions_holidays_upsert_delete_and_validation(self):
        self.seed()
        exception = {"user_id": self.worker_id, "date": self.today(), "expected_minutes": 0, "reason": "Ferie"}
        identifier = self.post("/api/work-exceptions", exception)["id"]
        self.post("/api/work-exceptions", {**exception, "expected_minutes": 120}, expected=200)
        rows = self.get("/api/work-exceptions", self.worker_cookie)["exceptions"]
        self.assertEqual((len(rows), rows[0]["expected_minutes"]), (1, 120))
        self.assert_error(403, "POST", "/api/work-exceptions/%d/delete" % identifier, {}, cookie=self.worker_cookie)
        self.post("/api/work-exceptions/%d/delete" % identifier, {}, expected=200)
        self.assertEqual(self.get("/api/work-exceptions")["exceptions"], [])
        self.post("/api/business-holidays", {"date": self.today(), "name": "Festività aziendale"})
        self.post("/api/business-holidays", {"date": self.today(), "name": "Chiusura"}, expected=200)
        self.assertEqual(self.get("/api/business-holidays", self.worker_cookie)["holidays"][0]["name"], "Chiusura")
        self.post("/api/business-holidays/%s/delete" % self.today(), {}, expected=200)
        for changes in ({"mon": -1}, {"tue": 1441}, {"starts_on": "2026-02-30"}, {"date": self.today()}):
            self.assert_error(400, "POST", "/api/work-schedules", self.schedule(**changes), cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/work-exceptions", {**exception, "expected_minutes": -1}, cookie=self.manager_cookie)
        self.assert_error(403, "POST", "/api/business-holidays", {"date": self.today(), "name": "x"}, cookie=self.worker_cookie)

    def check_missing_day(self, day):
        rule = self.create_rule(type="missing_hours", recipient_user_ids=str(self.worker_id))
        self.db("UPDATE alert_subjects SET starts_on=?,last_checked_on=NULL WHERE rule_id=?", (day, rule))
        self.run_rule(rule)
        return [row for row in self.notifications(self.worker_cookie)["notifications"] if row["work_date"] == day and row["rule_id"] == rule]

    def test_expected_minutes_priority_exception_holiday_schedule_default(self):
        self.seed()
        day = datetime.now(ZoneInfo("Europe/Rome")).date() - timedelta(days=4)
        while day.weekday() >= 5:
            day -= timedelta(days=1)
        day = day.isoformat()
        self.create_time(self.worker_cookie, self.job_id, date=day, minutes=120)
        # Default is 480, then effective personal target is 120.
        self.assertEqual(len(self.check_missing_day(day)), 1)
        self.post("/api/work-schedules", self.schedule(starts_on="0001-01-01", mon=120, tue=120, wed=120, thu=120, fri=120))
        self.assertEqual(self.check_missing_day(day), [])
        self.post("/api/business-holidays", {"date": day, "name": "Chiusura aziendale"})
        self.assertEqual(self.check_missing_day(day), [])
        # Explicit work during a holiday wins over the holiday itself.
        exception = {"user_id": self.worker_id, "date": day, "expected_minutes": 180, "reason": "Turno confermato"}
        self.post("/api/work-exceptions", exception)
        rows = self.check_missing_day(day)
        self.assertEqual(len(rows), 1)
        self.assertIn("180 previsti", rows[0]["message"])
        self.post("/api/work-exceptions", {**exception, "expected_minutes": 0, "reason": "Assenza"}, expected=200)
        self.assertEqual(self.check_missing_day(day), [])

    def test_weekend_shift_is_checked_after_same_48_hour_grace(self):
        self.seed()
        day = datetime.now(ZoneInfo("Europe/Rome")).date() - timedelta(days=4)
        while day.weekday() != 5:
            day -= timedelta(days=1)
        day = day.isoformat()
        self.create_time(self.worker_cookie, self.job_id, date=day, minutes=60)
        self.assertEqual(self.check_missing_day(day), [])
        self.post("/api/work-schedules", self.schedule(starts_on="0001-01-01", mon=0, tue=0, wed=0, thu=0, fri=0, sat=90))
        rows = self.check_missing_day(day)
        self.assertEqual(len(rows), 1)
        self.assertIn("90 previsti", rows[0]["message"])

    def test_notifications_assignment_due_dedup_read_and_revocation(self):
        self.seed()
        task = self.post("/api/tasks", {"job_id": self.job_id, "title": "Preparare la consegna",
            "assigned_user_id": self.worker_id, "due_date": self.today()})["id"]
        feed = self.get("/api/work-notifications", self.worker_cookie)
        self.assertEqual({row["type"] for row in feed["notifications"]}, {"assigned", "task_due"})
        self.assertEqual(feed["unread_count"], 2)
        self.assertEqual(self.get("/api/work-notifications", self.worker_cookie)["unread_count"], 2)
        self.assertEqual(self.get("/api/work-notifications")["notifications"], [])
        notification = next(row for row in feed["notifications"] if row["type"] == "assigned")
        self.assertEqual(notification["task_id"], task)
        self.assert_error(404, "POST", "/api/work-notifications/%d/read" % notification["id"], {}, cookie=self.manager_cookie)
        self.post("/api/work-notifications/%d/read" % notification["id"], {}, self.worker_cookie, 200)
        self.assertEqual(self.get("/api/work-notifications", self.worker_cookie)["unread_count"], 1)
        status, data, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (self.job_id, self.worker_id), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.get("/api/work-notifications", self.worker_cookie), {"ok": True, "notifications": [], "unread_count": 0})
        self.assert_error(404, "POST", "/api/work-notifications/%d/read" % notification["id"], {}, cookie=self.worker_cookie)

    def test_changing_deadline_hides_the_previous_due_notification(self):
        self.seed()
        task = self.post("/api/tasks", {"job_id": self.job_id, "title": "Scadenza variabile",
            "assigned_user_id": self.worker_id, "due_date": self.today()})["id"]
        self.assertEqual(self.get("/api/work-notifications", self.worker_cookie)["unread_count"], 2)
        self.post("/api/tasks/%d" % task, {"revision": 1, "due_date": self.today(20)}, expected=200)
        feed = self.get("/api/work-notifications", self.worker_cookie)
        self.assertEqual([row["type"] for row in feed["notifications"]], ["assigned"])
        self.assertEqual(feed["unread_count"], 1)

    def test_due_notifications_for_blockers_close_and_reassignment(self):
        self.seed()
        blocker = self.post("/api/blockers", {"job_id": self.job_id, "title": "Attendere dati",
            "owner_id": self.worker_id, "due_date": self.today(-1)})["id"]
        feed = self.get("/api/work-notifications", self.worker_cookie)
        self.assertEqual([row["type"] for row in feed["notifications"]], ["blocker_due"])
        self.post("/api/blockers/%d" % blocker, {"revision": 1, "status": "resolved"}, expected=200)
        self.assertEqual(self.get("/api/work-notifications", self.worker_cookie)["unread_count"], 0)
        task = self.post("/api/tasks", {"job_id": self.job_id, "title": "Riassegnabile", "assigned_user_id": self.worker_id})["id"]
        other = self.create_user(email="new-assignee@example.test")
        other_cookie = self.login("new-assignee@example.test", roles.WORKER_PASSWORD)
        self.assign_job(self.job_id, other)
        self.post("/api/tasks/%d" % task, {"revision": 1, "assigned_user_id": other}, expected=200)
        self.assertEqual([row["task_id"] for row in self.get("/api/work-notifications", other_cookie)["notifications"]], [task])


if __name__ == "__main__":
    unittest.main()
