"""User-scoped alert rules, delivery and scheduler boundaries on temporary data."""

from contextlib import closing
from datetime import datetime, timedelta
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import unittest
from urllib.parse import urlencode
from zoneinfo import ZoneInfo

import test_reports as reports
import test_roles as roles


ROME = ZoneInfo("Europe/Rome")
ALERT_TABLES = ("alert_rules", "alert_rule_users", "alert_rule_emails", "alert_subjects",
                "alert_events", "alert_notifications", "alert_email_outbox")


def build_native_check(directory):
    compiler = shutil.which(os.environ.get("CC", "cc"))
    if compiler is None:
        raise unittest.SkipTest("C compiler unavailable")
    binary = Path(directory) / "alerts-check"
    subprocess.run([compiler, "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
                    "-Wpedantic", "-Werror", "-I", str(roles.ROOT / "src"),
                    str(roles.ROOT / "src/alerts.c"), str(roles.ROOT / "src/storage.c"),
                    str(roles.ROOT / "src/workspace.c"), str(roles.ROOT / "src/organization.c"),
                    str(roles.ROOT / "src/mail_client.c"),
                    str(roles.ROOT / "tests/alerts_native_check.c"), "-lsqlite3", "-lcurl", "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


class AlertIntegrationTests(unittest.TestCase):
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
    edit_user = roles.RoleIntegrationTests.edit_user

    @classmethod
    def setUpClass(cls):
        cls.native_build = tempfile.TemporaryDirectory(prefix="punto-alerts-http-check-")
        cls.addClassCleanup(cls.native_build.cleanup)
        cls.native_check = build_native_check(cls.native_build.name)

    def tick_at(self, local_time):
        epoch = int(datetime.fromisoformat(local_time).replace(tzinfo=ROME).timestamp())
        subprocess.run([str(self.native_check), "tick", str(self.db_path), str(epoch)],
                       check=True, capture_output=True, text=True)

    def restart_and_login(self):
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)

    def rule_fields(self, **changes):
        # A disabled rule is safe to prepare without racing the real scheduler.
        fields = {"name": "Commessa inesistente", "type": "invalid_job", "run_time": "23:59",
                  "enabled": "0", "recipient_user_ids": str(self.users()[0]["id"]),
                  "recipient_emails": ""}
        fields.update(changes)
        return fields

    def create_rule(self, fields=None, *, cookie=None, **changes):
        status, data, _ = self.api("POST", "/api/alert-rules",
                                   fields if fields is not None else self.rule_fields(**changes),
                                   cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        return data["id"]

    def update_rule(self, rule_id, fields=None, *, cookie=None, **changes):
        status, data, _ = self.api("POST", "/api/alert-rules/%d" % rule_id,
                                   fields if fields is not None else self.rule_fields(**changes),
                                   cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)

    def rules(self, cookie=None):
        status, data, _ = self.api("GET", "/api/alert-rules", cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data["rules"], list)
        self.assertIsInstance(data["users"], list)
        self.assertEqual(data["timezone"], "Europe/Rome")
        self.assertEqual((data["daily_target_minutes"], data["grace_hours"]), (480, 48))
        self.assertIs(data["smtp_configured"], False)
        return data

    def run_rule(self, rule_id, cookie=None):
        status, data, _ = self.api("POST", "/api/alert-rules/%d/run" % rule_id,
                                   {}, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        self.assertIs(type(data["notifications_created"]), int)
        self.assertIs(type(data["emails_queued"]), int)
        return data

    def notifications(self, cookie=None):
        status, data, _ = self.api("GET", "/api/notifications", cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsInstance(data["notifications"], list)
        self.assertIs(type(data["unread_count"]), int)
        return data

    def worker(self, email=roles.WORKER_EMAIL):
        user_id = self.create_user(email=email)
        return user_id, self.login(email, roles.WORKER_PASSWORD)

    def record_invalid_attempt(self, cookie, job_id=999999):
        expected = 404 if cookie == self.manager_cookie else 403
        self.assert_error(expected, "POST", "/api/time-entries",
                          self.time_fields(job_id, date=datetime.now(ROME).date().isoformat()),
                          cookie=cookie)

    def invalid_rule(self, recipients, emails=""):
        return self.create_rule(recipient_user_ids=recipients, recipient_emails=emails)

    def test_rule_metadata_and_owner_isolation_are_server_enforced(self):
        other_id = self.create_user(email="other-manager@punto.local", role="manager")
        other = self.login("other-manager@punto.local", roles.WORKER_PASSWORD)
        manager_id = self.users()[0]["id"]
        rule_id = self.create_rule(name='Verifica "tecnica" ✓',
                                   recipient_user_ids="%d,%d" % (manager_id, other_id),
                                   recipient_emails="alerts@example.test")
        metadata = self.rules()
        self.assertEqual([row["id"] for row in metadata["rules"]], [rule_id])
        rule = metadata["rules"][0]
        self.assertEqual((rule["name"], rule["type"], rule["run_time"]),
                         ('Verifica "tecnica" ✓', "invalid_job", "23:59"))
        self.assertIs(rule["enabled"], False)
        self.assertEqual(set(rule["recipient_user_ids"]), {manager_id, other_id})
        self.assertEqual(rule["recipient_emails"], ["alerts@example.test"])
        self.assertIsNone(rule["last_run_at"])
        self.assertIn(rule["last_run_on"], (None, datetime.now(ROME).date().isoformat()))
        self.assertIsNone(rule["next_run_at"])
        self.assertEqual(self.rules(other)["rules"], [])
        for path in ("/api/alert-rules/%d" % rule_id, "/api/alert-rules/%d/run" % rule_id):
            self.assert_error(404, "POST", path,
                              self.rule_fields() if not path.endswith("/run") else {},
                              cookie=other)

    def test_workers_cannot_manage_rules_and_anonymous_users_cannot_read_notifications(self):
        _, worker = self.worker()
        rule_id = self.create_rule()
        self.assert_error(403, "GET", "/api/alert-rules", cookie=worker)
        self.assert_error(403, "POST", "/api/alert-rules", self.rule_fields(), cookie=worker)
        self.assert_error(403, "POST", "/api/alert-rules/%d" % rule_id, self.rule_fields(), cookie=worker)
        self.assert_error(403, "POST", "/api/alert-rules/%d/run" % rule_id, {}, cookie=worker)
        self.assert_error(403, "POST", "/api/alert-rules/999999/run", {}, cookie=worker)
        self.assert_error(401, "GET", "/api/alert-rules")
        self.assert_error(401, "GET", "/api/notifications")
        self.assertEqual(self.notifications(worker), {"notifications": [], "unread_count": 0})

    def test_rule_validation_rejects_unknown_duplicates_invalid_times_and_recipients(self):
        disabled_id = self.create_user(email="disabled@punto.local")
        status, data, _ = self.edit_user(disabled_id, active="0")
        self.assertEqual(status, 200, data)
        valid = self.rule_fields()
        invalid = ({"name": ""}, {"name": "N" * 121}, {"type": "unknown"},
                   {"enabled": "2"}, {"enabled": "true"}, {"run_time": "24:00"},
                   {"run_time": "12:60"}, {"run_time": "8:00"}, {"run_time": "08:00:00"},
                   {"recipient_user_ids": "-1"}, {"recipient_user_ids": "1.5"},
                   {"recipient_user_ids": "999999"}, {"recipient_user_ids": str(disabled_id)},
                   {"recipient_user_ids": "1,1"},
                   {"recipient_user_ids": "", "recipient_emails": ""},
                   {"recipient_emails": "invalid-address"}, {"recipient_emails": "mail@example.test\r\nBcc:x@y.test"},
                   {"recipient_emails": "same@example.test,SAME@example.test"},
                   {"recipient_emails": "mail@exämple.test"}, {"unexpected": "field"})
        for changes in invalid:
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/alert-rules", dict(valid, **changes),
                                  cookie=self.manager_cookie)
        for missing in ("name", "type", "run_time", "enabled"):
            fields = dict(valid)
            del fields[missing]
            with self.subTest(missing=missing):
                self.assert_error(400, "POST", "/api/alert-rules", fields, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/alert-rules", cookie=self.manager_cookie,
                          body=urlencode(valid) + "&type=missing_hours",
                          headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.rules()["rules"], [])

    def test_rule_recipients_are_normalized_and_can_be_email_only(self):
        manager_id = self.users()[0]["id"]
        worker_id, _ = self.worker()
        rule_id = self.create_rule(recipient_user_ids=" %d, %d " % (manager_id, worker_id),
                                   recipient_emails=" First@example.test\nsecond@example.test ")
        rule = self.rules()["rules"][0]
        self.assertEqual(set(rule["recipient_user_ids"]), {manager_id, worker_id})
        self.assertEqual(len(rule["recipient_user_ids"]), 2)
        self.assertEqual(set(rule["recipient_emails"]), {"first@example.test", "second@example.test"})
        self.assertEqual(len(rule["recipient_emails"]), 2)
        self.update_rule(rule_id, recipient_user_ids="", recipient_emails="email-only@example.test")
        rule = self.rules()["rules"][0]
        self.assertEqual(rule["recipient_user_ids"], [])
        self.assertEqual(rule["recipient_emails"], ["email-only@example.test"])

    def test_disabled_rule_can_run_manually_and_invalid_attempts_do_not_create_hours(self):
        user_id, worker = self.worker()
        rule_id = self.create_rule(recipient_user_ids=str(user_id))
        self.record_invalid_attempt(worker)
        result = self.run_rule(rule_id)
        self.assertEqual(result["notifications_created"], 1)
        self.assertEqual(len(self.notifications(worker)["notifications"]), 1)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM time_entries").fetchone(), (0,))

    def test_invalid_job_alerts_scope_subjects_deduplicate_and_mark_read_privately(self):
        manager_id = self.users()[0]["id"]
        first_id, first = self.worker()
        second_id, second = self.worker("second@punto.local")
        uninvolved_id, uninvolved = self.worker("uninvolved@punto.local")
        rule_id = self.invalid_rule("%d,%d,%d,%d" %
                                    (manager_id, first_id, second_id, uninvolved_id))
        self.record_invalid_attempt(first)
        self.record_invalid_attempt(second)
        result = self.run_rule(rule_id)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (4, 0))
        manager_notes = self.notifications()["notifications"]
        self.assertEqual({row["subject_user_id"] for row in manager_notes}, {first_id, second_id})
        self.assertEqual(self.notifications()["unread_count"], 2)
        first_notes = self.notifications(first)["notifications"]
        second_notes = self.notifications(second)["notifications"]
        self.assertEqual([row["subject_user_id"] for row in first_notes], [first_id])
        self.assertEqual([row["subject_user_id"] for row in second_notes], [second_id])
        self.assertEqual(self.notifications(uninvolved)["notifications"], [])
        self.assertNotIn("second@punto.local", str(first_notes))
        self.assertEqual((self.run_rule(rule_id)["notifications_created"],
                          self.run_rule(rule_id)["emails_queued"]), (0, 0))
        self.assert_error(404, "POST", "/api/notifications/%d/read" % first_notes[0]["id"], {}, cookie=second)
        status, data, _ = self.api("POST", "/api/notifications/%d/read" % first_notes[0]["id"], {}, cookie=first)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.notifications(first)["unread_count"], 0)
        read_at = self.notifications(first)["notifications"][0]["read_at"]
        self.assertIsInstance(read_at, str)
        status, data, _ = self.api("POST", "/api/notifications/%d/read" % first_notes[0]["id"], {}, cookie=first)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.notifications(first)["notifications"][0]["read_at"], read_at)
        self.assertEqual(self.notifications(second)["unread_count"], 1)

    def test_only_inexistent_or_deleted_jobs_create_events_and_repeated_attempts_are_deduplicated(self):
        user_id, worker = self.worker()
        private_job = self.create_record("jobs", {"code": "PRIVATE-ALERT", "name": "Commessa riservata",
                                                 "status": "active"})
        deleted_job = self.create_record("jobs", {"code": "DELETED-ALERT", "name": "Commessa eliminata",
                                                 "status": "active"})
        status, data, _ = self.api("DELETE", "/api/jobs/%d" % deleted_job, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        # The creation cursor excludes attempts which happened before this rule existed.
        self.record_invalid_attempt(worker, job_id=999998)
        rule_id = self.create_rule(recipient_user_ids=str(user_id))
        self.assertEqual(self.run_rule(rule_id)["notifications_created"], 0)
        self.record_invalid_attempt(worker, job_id=private_job)
        self.assert_error(400, "POST", "/api/time-entries",
                          self.time_fields(999999, minutes=0), cookie=worker)
        self.record_invalid_attempt(worker, job_id=deleted_job)
        self.record_invalid_attempt(worker, job_id=deleted_job)
        self.assertEqual(self.run_rule(rule_id)["notifications_created"], 1)
        self.assertEqual(len(self.notifications(worker)["notifications"]), 1)
        self.record_invalid_attempt(worker, job_id=deleted_job)
        self.assertEqual(self.run_rule(rule_id)["notifications_created"], 0)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual({row[0] for row in db.execute("SELECT job_id FROM alert_events")},
                             {999998, deleted_job})
            self.assertEqual(db.execute("SELECT COUNT(*) FROM time_entries").fetchone(), (0,))

    def test_email_delivery_stays_pending_without_smtp_and_disabled_recipients_are_skipped(self):
        manager_id = self.users()[0]["id"]
        subject_id, subject = self.worker()
        disabled_id = self.create_user(email="disabled-recipient@punto.local", role="manager")
        rule_id = self.create_rule(recipient_user_ids="%d,%d" % (manager_id, disabled_id),
                                   recipient_emails="first@example.test,second@example.test")
        status, data, _ = self.edit_user(disabled_id, name="Disabilitato", role="manager", active="0")
        self.assertEqual(status, 200, data)
        self.record_invalid_attempt(subject)
        result = self.run_rule(rule_id)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (1, 2))
        with closing(sqlite3.connect(self.db_path)) as db:
            rows = db.execute("SELECT email,subject_user_id,status FROM alert_email_outbox ORDER BY email").fetchall()
            self.assertEqual(rows, [("first@example.test", subject_id, "pending_configuration"),
                                    ("second@example.test", subject_id, "pending_configuration")])
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications WHERE recipient_id=?",
                                        (disabled_id,)).fetchone(), (0,))
        again = self.run_rule(rule_id)
        self.assertEqual((again["notifications_created"], again["emails_queued"]), (0, 0))

    def test_rule_creation_audit_failure_rolls_back_recipients_and_rule(self):
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER reject_alert_create BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='alert_rules.create' BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            db.commit()
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.assert_error(500, "POST", "/api/alert-rules",
                          self.rule_fields(recipient_emails="pending@example.test"), cookie=self.manager_cookie)
        with closing(sqlite3.connect(self.db_path)) as db:
            for table in ALERT_TABLES:
                self.assertEqual(db.execute('SELECT COUNT(*) FROM "' + table + '"').fetchone(), (0,), table)
            self.assertEqual(db.execute("SELECT * FROM audit_events ORDER BY id").fetchall(), old_audit)

    def test_run_audit_failure_rolls_back_notifications_outbox_cursor_and_schedule(self):
        user_id, worker = self.worker()
        rule_id = self.create_rule(recipient_user_ids=str(user_id), recipient_emails="pending@example.test")
        self.record_invalid_attempt(worker)
        with closing(sqlite3.connect(self.db_path)) as db:
            before = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                      for table in ALERT_TABLES + ("audit_events",)}
            db.execute("CREATE TRIGGER reject_alert_run BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='alert_rules.run' BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/alert-rules/%d/run" % rule_id, {}, cookie=self.manager_cookie)
        with closing(sqlite3.connect(self.db_path)) as db:
            for table, rows in before.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            db.execute("DROP TRIGGER reject_alert_run")
            db.commit()
        result = self.run_rule(rule_id)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (1, 1))

    def test_missing_hours_uses_480_minutes_weekdays_and_48_real_hours_after_day_end(self):
        manager_id = self.users()[0]["id"]
        short_id, short = self.worker()
        complete_id, complete = self.worker("complete@punto.local")
        common = self.create_record("jobs", {"code": "COMMON-HOURS", "name": "Attività comuni",
                                            "status": "active", "is_common": "1"})
        today = datetime.now(ROME).date()
        # Find a recent weekday whose midnight has certainly passed the grace.
        workday = today - timedelta(days=4)
        while workday.weekday() >= 5:
            workday -= timedelta(days=1)
        self.create_time(short, common, date=workday.isoformat(), minutes=479)
        self.create_time(complete, common, date=workday.isoformat(), minutes=480)
        rule_id = self.create_rule(type="missing_hours", recipient_user_ids="%d,%d,%d" %
                                   (manager_id, short_id, complete_id))
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("UPDATE alert_subjects SET starts_on=?,last_checked_on=NULL WHERE rule_id=?",
                       (workday.isoformat(), rule_id))
            db.commit()
        future_id = self.create_user(email="newly-monitored@punto.local")
        self.run_rule(rule_id)
        short_notes = self.notifications(short)["notifications"]
        complete_notes = self.notifications(complete)["notifications"]
        short_day = [row for row in short_notes if row["work_date"] == workday.isoformat()]
        self.assertEqual(len(short_day), 1)
        self.assertEqual((short_day[0]["subject_user_id"], short_day[0]["recorded_minutes"]), (short_id, 479))
        self.assertEqual([row for row in complete_notes if row["work_date"] == workday.isoformat()], [])
        for row in self.notifications()["notifications"]:
            self.assertNotEqual(row["subject_user_id"], future_id)
            day = datetime.fromisoformat(row["work_date"]).date()
            self.assertLess(day.weekday(), 5)
            midnight = datetime.combine(day + timedelta(days=1), datetime.min.time(), ROME)
            self.assertGreaterEqual(datetime.now(ROME).timestamp() - midnight.timestamp(), 48 * 3600)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT starts_on FROM alert_subjects WHERE rule_id=? AND user_id=?",
                                        (rule_id, future_id)).fetchone(), (today.isoformat(),))
        self.assertEqual(self.run_rule(rule_id)["notifications_created"], 0)

    def test_demoted_manager_immediately_loses_other_subject_notifications_in_existing_session(self):
        manager = self.users()[0]
        self.create_user(email="remaining-manager@punto.local", role="manager")
        subject_id, subject = self.worker()
        rule_id = self.create_rule(recipient_user_ids=str(manager["id"]))
        self.record_invalid_attempt(subject)
        self.run_rule(rule_id)
        note = self.notifications()["notifications"][0]
        self.assertEqual(note["subject_user_id"], subject_id)
        status, data, _ = self.edit_user(manager["id"], name=manager["name"], role="worker", active="1")
        self.assertEqual(status, 200, data)
        self.assertEqual(self.notifications()["notifications"], [])
        self.assertEqual(self.notifications()["unread_count"], 0)
        self.assert_error(403, "GET", "/api/alert-rules", cookie=self.manager_cookie)
        self.assert_error(404, "POST", "/api/notifications/%d/read" % note["id"], {}, cookie=self.manager_cookie)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("UPDATE alert_rules SET enabled=1,run_time='00:00',last_run_on=NULL WHERE id=?", (rule_id,))
            db.execute("INSERT INTO alert_events(user_id,job_id,work_date,minutes,created_at) VALUES(?,?,?,?,?)",
                       (subject_id, 999998, "2035-03-24", 60, "2035-03-24T08:01:00Z"))
            db.commit()
        self.tick_at("2035-03-25T09:00:00")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (1,))
            self.assertIsNone(db.execute("SELECT last_run_on FROM alert_rules WHERE id=?", (rule_id,)).fetchone()[0])

    def test_scheduler_runs_once_per_rome_date_persists_across_restarts_and_respects_disabled_rules(self):
        user_id, worker = self.worker()
        rule_id = self.create_rule(recipient_user_ids=str(user_id))
        self.record_invalid_attempt(worker)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("UPDATE alert_rules SET enabled=1,run_time='08:00' WHERE id=?", (rule_id,))
            db.commit()
        self.tick_at("2035-03-24T07:59:59")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (0,))
        self.tick_at("2035-03-24T08:00:00")
        self.tick_at("2035-03-24T09:00:00")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT last_run_on FROM alert_rules WHERE id=?", (rule_id,)).fetchone(),
                             ("2035-03-24",))
            db.execute("INSERT INTO alert_events(user_id,job_id,work_date,minutes,created_at) VALUES(?,?,?,?,?)",
                       (user_id, 999998, "2035-03-24", 60, "2035-03-24T08:01:00Z"))
            db.commit()
        self.tick_at("2035-03-24T18:00:00")
        self.tick_at("2035-03-25T07:59:59")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (1,))
        self.tick_at("2035-03-25T08:00:00")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (2,))
            db.execute("UPDATE alert_rules SET enabled=0 WHERE id=?", (rule_id,))
            db.execute("INSERT INTO alert_events(user_id,job_id,work_date,minutes,created_at) VALUES(?,?,?,?,?)",
                       (user_id, 999997, "2035-03-25", 60, "2035-03-25T08:01:00Z"))
            db.commit()
        self.tick_at("2035-03-26T09:00:00")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (2,))
            self.assertEqual(db.execute("SELECT last_run_on FROM alert_rules WHERE id=?", (rule_id,)).fetchone(),
                             ("2035-03-25",))

    def test_new_rule_with_elapsed_daily_time_waits_until_tomorrow(self):
        user_id, worker = self.worker()
        today = datetime.now(ROME).date()
        rule_id = self.create_rule(recipient_user_ids=str(user_id), run_time="00:00", enabled="1")
        self.record_invalid_attempt(worker)
        self.assertEqual(self.notifications(worker)["notifications"], [])
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT last_run_on,last_run_at FROM alert_rules WHERE id=?",
                                        (rule_id,)).fetchone(), (today.isoformat(), None))
        self.stop_server()
        self.tick_at(today.isoformat() + "T23:59:59")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (0,))
        self.tick_at((today + timedelta(days=1)).isoformat() + "T00:00:00")
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone(), (1,))

    def test_schema_five_upgrade_preserves_business_and_audit_once(self):
        user_id, worker = self.worker()
        common = self.create_record("jobs", {"code": "MIGRATE-ALERT", "name": "Attività comuni",
                                            "status": "active", "is_common": "1"})
        self.create_time(worker, common, minutes=35)
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            for table in reversed(ALERT_TABLES):
                db.execute('DROP TABLE "' + table + '"')
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' "
                                 "AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (6,7)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=5")
            db.commit()
            old = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                   for table in ("jobs", "users", "time_entries", "job_assignments", "labor_rates")}
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.restart_and_login()
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (7,))
            for table, rows in old.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            for table in ALERT_TABLES:
                self.assertEqual(db.execute('SELECT COUNT(*) FROM "' + table + '"').fetchone(), (0,), table)
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE NOT(action='system.migration' "
                                        "AND entity_id IN (6,7)) ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' "
                                        "AND entity_id=6").fetchone(), (1,))
            final_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.stop_server()
        self.restart_and_login()
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT * FROM audit_events ORDER BY id").fetchall(), final_audit)


class AlertNativeClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native_build = tempfile.TemporaryDirectory(prefix="punto-alerts-clock-check-")
        cls.addClassCleanup(cls.native_build.cleanup)
        cls.native_check = build_native_check(cls.native_build.name)

    def check(self, scenario):
        subprocess.run([str(self.native_check), scenario], check=True, capture_output=True, text=True)

    def test_exact_48_hours_weekdays_and_invalid_calendar_dates(self):
        self.check("ordinary")

    def test_spring_change_requires_48_real_hours_and_ends_at_0100_monday(self):
        self.check("spring")

    def test_autumn_change_requires_48_real_hours_and_ends_at_2300_sunday(self):
        self.check("autumn")

    def test_next_run_respects_local_daily_time_and_dst_offsets(self):
        self.check("next_run")


if __name__ == "__main__":
    unittest.main()
