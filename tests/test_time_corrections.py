"""Reviewed timesheet corrections over HTTP, with isolated temporary SQLite data."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from datetime import datetime, timedelta
import sqlite3
import unittest
from zoneinfo import ZoneInfo

import test_roles as roles


class TimeCorrectionsIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    assign_job = roles.RoleIntegrationTests.assign_job
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    assert_error = roles.RoleIntegrationTests.assert_error

    def today(self, offset=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=offset)).isoformat()

    def post(self, path, fields, cookie=None, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), True)
        return data

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def seed(self):
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        manager_id = self.get("/api/session")["user_id"]
        self.team_id = self.post("/api/groups", {"name": "Squadra rettifiche"})["id"]
        for user_id in (manager_id, self.worker_id):
            self.post("/api/groups/%d/members" % self.team_id, {"user_id": user_id})
        self.job_id = self.post("/api/jobs", {"code": "CORR-1", "name": "Rettifiche", "status": "active"})["id"]
        self.assign_job(self.job_id, self.worker_id)
        self.entry_id = self.create_time(self.worker_cookie, self.job_id, date=self.today(), minutes=120)
        return self.entry_id

    def entries(self, cookie=None, date=None):
        return self.get("/api/time-entries?date=" + (date or self.today()), cookie or self.worker_cookie)["entries"]

    def fields(self, **changes):
        entry = next(row for row in self.entries() if row["id"] == self.entry_id)
        fields = {"entry_id": self.entry_id, "revision": entry["revision"], "job_id": self.job_id,
                  "date": self.today(), "minutes": 90, "note": "Nota corretta", "reason": "Durata verificata",
                  "delete_requested": 0}
        fields.update(changes)
        return fields

    def request(self, **changes):
        return self.post("/api/time-corrections", self.fields(**changes), self.worker_cookie)["id"]

    def decide(self, request_id, decision="approved", cookie=None, reason=""):
        return self.post("/api/time-corrections/%d/decision" % request_id,
                         {"decision": decision, "reason": reason}, cookie, 200)

    def corrections(self, cookie=None, query=""):
        return self.get("/api/time-corrections" + query, cookie or self.worker_cookie)["requests"]

    def db(self, sql, values=()):
        with closing(sqlite3.connect(self.db_path)) as connection:
            connection.execute("PRAGMA foreign_keys=ON")
            result = connection.execute(sql, values).fetchall()
            connection.commit()
            return result

    def test_worker_requests_manager_reviews_and_totals_revision_audit_change(self):
        self.seed()
        request_id = self.request()
        original = self.entries()[0]
        self.assertEqual((original["minutes"], original["revision"]), (120, 1))
        request = self.corrections()[0]
        self.assertEqual((request["id"], request["status"], request["minutes"]), (request_id, "pending", 90))
        self.assertEqual(request["before"]["minutes"], 120)
        self.assertTrue(request["can_cancel"])
        self.assertFalse(request["can_decide"])
        self.assertTrue(self.corrections(self.manager_cookie)[0]["can_decide"])
        self.decide(request_id)
        corrected = self.entries()[0]
        self.assertEqual((corrected["minutes"], corrected["note"], corrected["revision"]), (90, "Nota corretta", 2))
        self.assertEqual(corrected["owner"], roles.WORKER_EMAIL)
        self.assertEqual(self.corrections()[0]["status"], "approved")
        self.assertEqual(self.corrections(query="?status=pending"), [])
        events = self.db("SELECT action FROM audit_events WHERE entity_type='time_correction' ORDER BY id")
        self.assertEqual([row[0] for row in events], ["time_corrections.create", "time_corrections.decide"])

    def test_role_scope_ownership_and_one_pending_request(self):
        self.seed()
        other_id = self.create_user(email="other-correction@example.test")
        other_cookie = self.login("other-correction@example.test", roles.WORKER_PASSWORD)
        fields = self.fields()
        self.assert_error(404, "POST", "/api/time-corrections", fields, cookie=other_cookie)
        request_id = self.request()
        self.assertEqual(self.corrections(other_cookie), [])
        self.assert_error(409, "POST", "/api/time-corrections", fields, cookie=self.worker_cookie)
        self.assert_error(403, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.worker_cookie)
        self.assert_error(404, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "cancelled"}, cookie=other_cookie)
        self.assert_error(403, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "cancelled"}, cookie=self.manager_cookie)
        self.assertGreater(other_id, 0)
        self.decide(request_id, "cancelled", self.worker_cookie)
        self.assertEqual(self.corrections()[0]["status"], "cancelled")
        self.request()  # Cancelled requests no longer reserve the original entry.

    def test_outside_manager_and_finance_never_receive_snapshots_or_decide(self):
        self.seed()
        request_id = self.request(note="Nota personale riservata", reason="Motivo personale")
        outside_id = self.create_user(email="outside-manager@example.test", role="manager")
        outside = self.login("outside-manager@example.test", roles.WORKER_PASSWORD)
        self.post("/api/users/%d/access-groups" % outside_id, {"groups": '["finance"]'}, expected=200)
        response = self.get("/api/time-corrections", outside)
        self.assertEqual((response["scope"], response["requests"]), ("team", []))
        for target in (request_id, 999999):
            status, data, _ = self.api("POST", "/api/time-corrections/%d/decision" % target,
                                       {"decision": "approved"}, cookie=outside)
            self.assertEqual(status, 404, data)
            self.assertNotIn("snapshot", str(data))
            self.assertNotIn("Nota personale", str(data))
        self.assertEqual(self.corrections()[0]["status"], "pending")
        self.assertEqual(self.entries()[0]["minutes"], 120)

    def test_team_revocation_hides_original_and_proposal_even_after_original_delete(self):
        self.seed()
        request_id = self.request()
        self.assertEqual(len(self.corrections(self.manager_cookie)), 1)
        status, data, _ = self.api("DELETE", "/api/groups/%d/members/%d" % (self.team_id, self.worker_id),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.db("DELETE FROM time_entries WHERE id=?", (self.entry_id,))
        self.assertEqual(self.corrections(self.manager_cookie), [])
        self.assert_error(404, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "rejected", "reason": "Fuori squadra"}, cookie=self.manager_cookie)
        own = self.corrections()[0]
        self.assertIsNone(own["entry_id"])
        self.assertEqual(own["before"]["minutes"], 120)
        self.decide(request_id, "cancelled", self.worker_cookie)

    def test_manager_cannot_approve_or_reject_own_request(self):
        self.seed()
        own_entry = self.create_time(self.manager_cookie, self.job_id, date=self.today())
        fields = self.fields(entry_id=own_entry, revision=1, minutes=30)
        request_id = self.post("/api/time-corrections", fields)["id"]
        for decision in ("approved", "rejected"):
            self.assert_error(403, "POST", "/api/time-corrections/%d/decision" % request_id,
                              {"decision": decision, "reason": "Verificato"}, cookie=self.manager_cookie)
        reviewer_id = self.create_user(email="reviewer@example.test", role="manager")
        self.post("/api/groups/%d/members" % self.team_id, {"user_id": reviewer_id})
        reviewer = self.login("reviewer@example.test", roles.WORKER_PASSWORD)
        self.post("/api/improvements/governance/delegations", {"user_id": reviewer_id, "group_id": self.team_id, "can_approve_corrections": 1, "starts_on": "2026-01-01", "ends_on": "", "reason": "Revisore della squadra"})
        self.decide(request_id, cookie=reviewer)
        self.assertEqual(self.entries(self.manager_cookie)[0]["minutes"], 30)

    def test_rejection_requires_reason_and_never_changes_hours(self):
        self.seed()
        request_id = self.request()
        self.assert_error(400, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "rejected", "reason": "  "}, cookie=self.manager_cookie)
        self.decide(request_id, "rejected", reason="La durata originale è confermata")
        self.assertEqual(self.entries()[0]["minutes"], 120)
        request = self.corrections()[0]
        self.assertEqual((request["status"], request["decision_reason"]),
                         ("rejected", "La durata originale è confermata"))
        self.assert_error(409, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)

    def test_modified_original_conflicts_and_preserves_proposal(self):
        self.seed()
        stale_fields = self.fields()
        self.db("UPDATE time_entries SET minutes=110,revision=revision+1 WHERE id=?", (self.entry_id,))
        self.assert_error(409, "POST", "/api/time-corrections", stale_fields, cookie=self.worker_cookie)
        request_id = self.request()
        self.db("UPDATE time_entries SET note='modifica concorrente',revision=revision+1 WHERE id=?", (self.entry_id,))
        self.assert_error(409, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)
        request = self.corrections()[0]
        self.assertEqual(request["status"], "conflict")
        self.assertEqual(request["before"]["minutes"], 110)
        self.assertEqual(request["minutes"], 90)
        self.assertEqual(self.entries()[0]["note"], "modifica concorrente")

    def test_deleted_original_and_deleted_job_preserve_snapshot_and_conflict(self):
        self.seed()
        request_id = self.request()
        status, data, _ = self.api("DELETE", "/api/jobs/%d" % self.job_id, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        request = self.corrections()[0]
        self.assertIsNone(request["entry_id"])
        self.assertEqual(request["before"]["job_name"], "Rettifiche")
        self.assertEqual(request["original_entry_id"], self.entry_id)
        self.assert_error(409, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)
        self.assertEqual(self.corrections()[0]["status"], "conflict")

    def test_revoked_owner_can_reduce_or_delete_but_cannot_increase_or_move(self):
        self.seed()
        request_id = self.request(minutes=180)
        status, data, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (self.job_id, self.worker_id), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assert_error(403, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)
        self.decide(request_id, "cancelled", self.worker_cookie)
        for changes in ({"minutes": 180}, {"minutes": 120, "note": "Solo nota"}, {"date": self.today(1)}):
            self.assert_error(403, "POST", "/api/time-corrections", self.fields(**changes), cookie=self.worker_cookie)
        reduced = self.request(minutes=60)
        self.decide(reduced)
        self.assertEqual(self.entries()[0]["minutes"], 60)
        deleted = self.request(minutes=60, delete_requested=1)
        self.decide(deleted)
        self.assertEqual(self.entries(), [])
        self.assertEqual(self.corrections()[0]["status"], "approved")
        self.assertIsNone(self.corrections()[0]["entry_id"])

    def test_closed_or_archived_destination_rechecked_at_approval(self):
        for flag in ("time_closed", "archived"):
            with self.subTest(flag=flag):
                if not hasattr(self, "entry_id"):
                    self.seed()
                request_id = self.request(minutes=130)
                self.db("UPDATE jobs SET %s=1 WHERE id=?" % flag, (self.job_id,))
                self.assert_error(403, "POST", "/api/time-corrections/%d/decision" % request_id,
                                  {"decision": "approved"}, cookie=self.manager_cookie)
                self.decide(request_id, "cancelled", self.worker_cookie)
                request_id = self.request(minutes=90 if flag == "time_closed" else 60)
                self.decide(request_id)
                self.db("UPDATE jobs SET %s=0 WHERE id=?" % flag, (self.job_id,))

    def test_daily_total_limit_is_rechecked_without_partial_changes(self):
        self.seed()
        request_id = self.request(minutes=1400)
        self.create_time(self.worker_cookie, self.job_id, date=self.today(), minutes=60)
        self.assert_error(409, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)
        self.assertEqual(sum(row["minutes"] for row in self.entries()), 180)
        self.assertEqual(self.corrections()[0]["status"], "pending")
        self.assertEqual(self.db("SELECT COUNT(*) FROM audit_events WHERE action='time_corrections.decide'")[0][0], 0)

    def test_target_day_record_limit_and_date_grants(self):
        self.seed()
        request_id = self.request(date=self.today(1), minutes=1)
        with closing(sqlite3.connect(self.db_path)) as connection:
            connection.executemany("INSERT INTO time_entries(job_id,user_id,owner,date,minutes,note) VALUES(?,?,?,?,1,'')",
                                   [(self.job_id, self.worker_id, roles.WORKER_EMAIL, self.today(1))] * 1000)
            connection.commit()
        self.assert_error(409, "POST", "/api/time-corrections/%d/decision" % request_id,
                          {"decision": "approved"}, cookie=self.manager_cookie)
        self.decide(request_id, "cancelled", self.worker_cookie)
        self.db("UPDATE job_assignments SET starts_on=? WHERE job_id=? AND user_id=?", (self.today(), self.job_id, self.worker_id))
        self.assert_error(403, "POST", "/api/time-corrections", self.fields(date=self.today(-1)), cookie=self.worker_cookie)

    def test_simultaneous_decisions_apply_once(self):
        self.seed()
        request_id = self.request()
        path = "/api/time-corrections/%d/decision" % request_id
        with ThreadPoolExecutor(max_workers=2) as pool:
            replies = list(pool.map(lambda _: self.api("POST", path, {"decision": "approved"}, cookie=self.manager_cookie), range(2)))
        self.assertEqual(sorted(reply[0] for reply in replies), [200, 409])
        self.assertEqual(self.entries()[0]["revision"], 2)
        self.assertEqual(self.db("SELECT COUNT(*) FROM audit_events WHERE action='time_corrections.decide'")[0][0], 1)

    def test_task_link_is_kept_for_same_job_and_cleared_after_move(self):
        self.seed()
        task_id = self.post("/api/tasks", {"job_id": self.job_id, "title": "Attività collegata"})["id"]
        self.db("UPDATE time_entries SET task_id=? WHERE id=?", (task_id, self.entry_id))
        request_id = self.request()
        self.decide(request_id)
        self.assertEqual(self.entries()[0]["task_id"], task_id)
        destination = self.post("/api/jobs", {"code": "CORR-DEST", "name": "Destinazione", "status": "active"})["id"]
        self.assign_job(destination, self.worker_id)
        request_id = self.request(job_id=destination)
        self.decide(request_id)
        moved = self.entries()[0]
        self.assertEqual(moved["job_id"], destination)
        self.assertIsNone(moved["task_id"])

    def test_validation_and_authentication(self):
        self.seed()
        self.assert_error(401, "GET", "/api/time-corrections")
        for changes in ({"reason": "   "}, {"minutes": 0}, {"minutes": 1441}, {"delete_requested": 2},
                        {"date": "2026-02-30"}, {"unexpected": "value"}, {"revision": 0}):
            self.assert_error(400, "POST", "/api/time-corrections", self.fields(**changes), cookie=self.worker_cookie)
        self.assertEqual(self.get("/api/time-corrections?status=approved", self.worker_cookie)["requests"], [])
        self.assert_error(400, "GET", "/api/time-corrections?status=invalid", cookie=self.worker_cookie)
        self.assert_error(400, "POST", "/api/time-corrections/abc/decision", {"decision": "approved"}, cookie=self.manager_cookie)


if __name__ == "__main__":
    unittest.main()
