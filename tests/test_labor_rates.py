"""Effective hourly rates, authorized labor estimates and schema-four upgrades."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import sqlite3
import unittest
from urllib.parse import urlencode

import test_reports as reports
import test_roles as roles


class LaborRateIntegrationTests(unittest.TestCase):
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
    assert_summary = reports.ReportsIntegrationTests.assert_summary
    seed_reports = reports.ReportsIntegrationTests.seed_reports

    def rate_fields(self, user_id, **changes):
        fields = {"user_id": user_id, "hourly_cents": 2500, "currency": "EUR",
                  "starts_on": "2026-10-09", "ends_on": "2026-10-10"}
        fields.update(changes)
        return fields

    def create_rate(self, user_id, **changes):
        return self.create_record("labor-rates", self.rate_fields(user_id, **changes))

    def rates(self):
        status, data, _ = self.api("GET", "/api/labor-rates", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        self.assertIsInstance(data["rates"], list)
        return data["rates"]

    def events(self):
        status, data, _ = self.api("GET", "/api/audit?entity_type=labor_rate&limit=100",
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data["events"]

    def labor_job(self, code="L1"):
        return self.create_record("jobs", {"code": code, "name": "Commessa " + code,
                                           "status": "active"})

    def seed_labor(self):
        self.labor_users = {}
        self.labor_cookies = {}
        for code in ("A", "B", "ZERO", "MISSING"):
            email = "labor-%s@example.test" % code.lower()
            self.labor_users[code] = self.create_user(email=email, name="Tecnico " + code)
            self.labor_cookies[code] = self.login(email, roles.WORKER_PASSWORD)
        self.labor_jobs = {code: self.labor_job(code) for code in ("L1", "L2")}
        for user_id in self.labor_users.values():
            for job_id in self.labor_jobs.values():
                self.assign_job(job_id, user_id)
        self.early_rate = self.create_rate(self.labor_users["A"], hourly_cents=100,
                                           starts_on="2026-10-09", ends_on="2026-10-09")
        self.create_rate(self.labor_users["A"], hourly_cents=200,
                         starts_on="2026-10-10", ends_on="2026-10-10")
        self.create_rate(self.labor_users["B"], hourly_cents=30, ends_on="")
        self.create_rate(self.labor_users["ZERO"], hourly_cents=0, ends_on="")
        entries = (("A", "L1", "2026-10-09", 1),
                   ("A", "L1", "2026-10-09", 1),
                   ("A", "L1", "2026-10-10", 3),
                   ("A", "L1", "2026-10-11", 5),
                   ("A", "L2", "2026-10-10", 1),
                   ("B", "L1", "2026-10-09", 1),
                   ("ZERO", "L2", "2026-10-10", 60),
                   ("MISSING", "L2", "2026-10-10", 2))
        for user, job, date, minutes in entries:
            self.create_time(self.labor_cookies[user], self.labor_jobs[job],
                              date=date, minutes=minutes)

    def labor_report(self, **filters):
        return self.report("labor_cost", start="2026-10-09", end="2026-10-11", **filters)

    def test_rate_lifecycle_accepts_zero_inactive_users_and_persists_with_audit(self):
        user_id = self.create_user(name='Tecnico "Qualità" ✓')
        status, data, _ = self.api("POST", "/api/users/%d" % user_id,
                                   {"name": 'Tecnico "Qualità" ✓', "role": "worker", "active": "0"},
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        rate_id = self.create_rate(user_id, hourly_cents=0, ends_on="")
        row = self.rates()[0]
        self.assertEqual(row, {"id": rate_id, "user_id": user_id,
                              "user_name": 'Tecnico "Qualità" ✓', "user_email": roles.WORKER_EMAIL,
                              "hourly_cents": 0, "currency": "EUR", "starts_on": "2026-10-09", "ends_on": ""})
        self.assertIs(type(row["hourly_cents"]), int)
        status, data, _ = self.api("POST", "/api/labor-rates/%d" % rate_id,
                                   self.rate_fields(user_id, hourly_cents=99999999, ends_on="2030-12-31"),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        updated = self.rates()[0]
        self.assertEqual((updated["hourly_cents"], updated["ends_on"]), (99999999, "2030-12-31"))
        events = self.events()
        self.assertEqual(len(events), 2)
        created = next(event for event in events if event["action"] == "labor_rates.create")
        changed = next(event for event in events if event["action"] == "labor_rates.update")
        self.assertEqual(created["entity_id"], rate_id)
        self.assertIsNone(created["before"])
        self.assertEqual(created["after"], row)
        self.assertEqual(changed["before"], row)
        self.assertEqual(changed["after"], updated)
        self.assertTrue(all(event["actor_id"] == self.users()[0]["id"] for event in events))
        self.assert_error(405, "DELETE", "/api/labor-rates/%d" % rate_id, cookie=self.manager_cookie)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.rates(), [updated])
        self.assertEqual(self.events(), events)

    def test_rate_period_overlap_is_inclusive_per_user_and_updates_exclude_self(self):
        user_id = self.create_user()
        other_id = self.create_user(email="other-rate@example.test")
        first = self.create_rate(user_id, starts_on="2026-01-01", ends_on="2026-01-31")
        for start, end in (("2026-01-31", "2026-02-28"), ("2026-01-15", "2026-01-20"),
                           ("2025-12-31", "2026-01-01"), ("2025-12-31", "2026-12-31")):
            with self.subTest(start=start, end=end):
                self.assert_error(409, "POST", "/api/labor-rates",
                                  self.rate_fields(user_id, starts_on=start, ends_on=end),
                                  cookie=self.manager_cookie)
        self.create_rate(other_id, starts_on="2026-01-01", ends_on="2026-01-31")
        self.create_rate(user_id, starts_on="2026-02-01", ends_on="2026-02-28")
        self.create_rate(user_id, starts_on="2026-03-01", ends_on="")
        self.assert_error(409, "POST", "/api/labor-rates",
                          self.rate_fields(user_id, starts_on="2027-01-01", ends_on=""),
                          cookie=self.manager_cookie)
        original = self.rate_fields(user_id, starts_on="2026-01-01", ends_on="2026-01-31")
        status, data, _ = self.api("POST", "/api/labor-rates/%d" % first, original,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assert_error(409, "POST", "/api/labor-rates/%d" % first,
                          dict(original, ends_on="2026-02-01"), cookie=self.manager_cookie)
        self.assertEqual(len(self.rates()), 4)

    def test_rate_form_is_strict_and_user_id_cannot_be_changed(self):
        user_id = self.create_user()
        other_id = self.create_user(email="other-rate@example.test")
        base = self.rate_fields(user_id)
        changes = ({"user_id": "0"}, {"user_id": "-1"},
                   {"user_id": "1.5"}, {"hourly_cents": "-1"}, {"hourly_cents": "1.25"},
                   {"hourly_cents": "100000000"}, {"hourly_cents": "9223372036854775808"},
                   {"hourly_cents": ""}, {"currency": "USD"}, {"starts_on": ""},
                   {"starts_on": "2026-02-29"}, {"ends_on": "2026-13-01"},
                   {"starts_on": "2026-10-11", "ends_on": "2026-10-10"}, {"unknown": "value"})
        for change in changes:
            with self.subTest(change=change):
                self.assert_error(400, "POST", "/api/labor-rates", dict(base, **change),
                                  cookie=self.manager_cookie)
        for required in ("user_id", "hourly_cents", "currency", "starts_on"):
            fields = dict(base)
            del fields[required]
            with self.subTest(missing=required):
                self.assert_error(400, "POST", "/api/labor-rates", fields, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/labor-rates", cookie=self.manager_cookie,
                          body=urlencode(base) + "&hourly_cents=3000",
                          headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assert_error(404, "POST", "/api/labor-rates", dict(base, user_id=999999),
                          cookie=self.manager_cookie)
        self.assertEqual(self.rates(), [])
        rate_id = self.create_rate(user_id)
        self.assert_error(400, "POST", "/api/labor-rates/%d" % rate_id,
                          self.rate_fields(other_id), cookie=self.manager_cookie)
        self.assertEqual(self.rates()[0]["user_id"], user_id)
        self.assertEqual(len(self.events()), 1, "Rejected writes must not generate successful audit events")

    def test_unprofiled_workers_cannot_read_rates_and_labor_reports(self):
        user_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        rate_id = self.create_rate(user_id)
        query = urlencode({"view": "labor_cost", "from": "2026-10-09", "to": "2026-10-10"})
        for method, path, fields in (("GET", "/api/labor-rates", None),
                                     ("POST", "/api/labor-rates", self.rate_fields(user_id)),
                                     ("POST", "/api/labor-rates/%d" % rate_id, self.rate_fields(user_id)),
                                     ("GET", "/api/reports?" + query, None)):
            with self.subTest(method=method, path=path):
                self.assert_error(401, method, path, fields)
                self.assert_error(403, method, path, fields, cookie=worker)

    def test_labor_estimate_uses_work_date_and_rounds_half_up_for_each_entry(self):
        self.seed_labor()
        report = self.labor_report()
        self.assert_summary(report, 74, 8, 2, 4)
        self.assertEqual(report["scope"], "financial")
        self.assertEqual({key: report["summary"][key] for key in (
            "estimated_cost_cents", "priced_minutes", "unpriced_minutes")},
            {"estimated_cost_cents": 18, "priced_minutes": 67, "unpriced_minutes": 7})
        self.assertIs(type(report["summary"]["estimated_cost_cents"]), int)
        rows = {row["job_id"]: row for row in report["rows"]}
        expected = (("L1", 11, 5, 6, 5, 15), ("L2", 63, 3, 61, 2, 3))
        self.assertEqual(len(rows), len(expected))
        for job, minutes, entries, priced, unpriced, cost in expected:
            row = rows[self.labor_jobs[job]]
            self.assertEqual((row["minutes"], row["entry_count"], row["priced_minutes"],
                              row["unpriced_minutes"], row["cost_cents"]),
                             (minutes, entries, priced, unpriced, cost))
            self.assertEqual((row["code"], row["job_name"]), (job, "Commessa " + job))
            self.assertFalse({"user_id", "name", "email"}.intersection(row))

    def test_labor_filters_allow_job_and_dates_and_reject_individual_users(self):
        self.seed_labor()
        baseline = self.labor_report()
        filtered = self.labor_report(job_id=self.labor_jobs["L1"])
        self.assert_summary(filtered, 11, 5, 1, 2)
        self.assertEqual(filtered["summary"]["estimated_cost_cents"], 15)
        self.assertEqual(filtered["options"], baseline["options"])
        day = self.report("labor_cost", start="2026-10-09", end="2026-10-09",
                          job_id=self.labor_jobs["L1"])
        self.assert_summary(day, 3, 3, 1, 2)
        self.assertEqual(day["rows"][0]["cost_cents"], 5,
                         "Two separately rounded A entries cost four cents plus one cent for B")
        query = urlencode({"view": "labor_cost", "from": "2026-10-09", "to": "2026-10-11",
                           "user_id": self.labor_users["MISSING"]})
        self.assert_error(403, "GET", "/api/reports?" + query, cookie=self.manager_cookie)
        self.assertEqual(baseline["options"]["users"], [])
        empty = self.report("labor_cost", start="2026-12-01", end="2026-12-01")
        self.assert_summary(empty, 0, 0, 0, 0)
        self.assertEqual(empty["rows"], [])
        self.assertEqual(empty["summary"]["estimated_cost_cents"], 0)

    def test_rate_changes_recompute_estimates_and_do_not_rewrite_time_entries(self):
        self.seed_labor()
        with closing(sqlite3.connect(self.db_path)) as db:
            entries = db.execute("SELECT * FROM time_entries ORDER BY id").fetchall()
        status, data, _ = self.api("POST", "/api/labor-rates/%d" % self.early_rate,
                                   self.rate_fields(self.labor_users["A"], hourly_cents=600,
                                                    starts_on="2026-10-09", ends_on="2026-10-09"),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        report = self.labor_report()
        self.assertEqual(report["summary"]["estimated_cost_cents"], 34)
        changed = next(row for row in report["rows"] if row["job_id"] == self.labor_jobs["L1"])
        self.assertEqual(changed["cost_cents"], 31)
        self.assertEqual((changed["priced_minutes"], changed["unpriced_minutes"]), (6, 5))
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT * FROM time_entries ORDER BY id").fetchall(), entries)

    def test_large_cost_uses_integer_cents_without_32_bit_overflow(self):
        manager_id = self.users()[0]["id"]
        job_id = self.labor_job()
        self.create_rate(manager_id, hourly_cents=99999999, ends_on="")
        self.create_time(self.manager_cookie, job_id, minutes=1440, date="2026-10-09")
        report = self.labor_report()
        self.assertEqual(report["summary"]["estimated_cost_cents"], 2399999976)
        self.assertIs(type(report["rows"][0]["cost_cents"]), int)
        self.assertEqual(report["rows"][0]["cost_cents"], 2399999976)

    def test_audit_failure_rolls_back_rate_creation_and_update(self):
        user_id = self.create_user()
        rate_id = self.create_rate(user_id)
        before_rates, before_events = self.rates(), self.events()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER block_labor_audit BEFORE INSERT ON audit_events "
                       "WHEN NEW.entity_type='labor_rate' BEGIN SELECT RAISE(ABORT,'test audit failure'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/labor-rates", self.rate_fields(
            user_id, starts_on="2026-11-01", ends_on="2026-11-30"), cookie=self.manager_cookie)
        self.assert_error(500, "POST", "/api/labor-rates/%d" % rate_id,
                          self.rate_fields(user_id, hourly_cents=5000), cookie=self.manager_cookie)
        self.assertEqual(self.rates(), before_rates)
        self.assertEqual(self.events(), before_events)

    def test_concurrent_overlapping_rates_have_one_winner(self):
        user_id = self.create_user()
        fields = self.rate_fields(user_id)
        with ThreadPoolExecutor(max_workers=2) as pool:
            calls = [pool.submit(self.api, "POST", "/api/labor-rates", fields,
                                 cookie=self.manager_cookie) for _ in range(2)]
            responses = [call.result() for call in calls]
        self.assertEqual(sorted(status for status, _, _ in responses), [201, 409])
        self.assertEqual(len(self.rates()), 1)
        self.assertEqual(len(self.events()), 1)

    def test_schema_three_upgrade_preserves_all_business_records_and_audit(self):
        self.seed_reports()
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TABLE labor_rates")
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' "
                                 "AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (4,5,6,7,8,9,10,12,13)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=3")
            db.commit()
            tables = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table' "
                                                 "AND name NOT IN ('sqlite_sequence','audit_events')")]
            snapshot = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                        for table in tables}
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (13,))
            for table, rows in snapshot.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE "
                                        "NOT(action='system.migration' AND entity_id IN (4,5,6,7,8,9,10,12,13)) ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE "
                                        "action='system.migration' AND entity_id=4").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE "
                                        "action='system.migration' AND entity_id=5").fetchone(), (1,))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE "
                                        "action='system.migration' AND entity_id=6").fetchone(), (1,))
            final_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.assertEqual(self.rates(), [])
        self.assert_summary(self.report(), 380, 7, 5, 3)
        self.assertEqual(self.report("cost")["summary"]["known_value_cents"], 123456)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT * FROM audit_events ORDER BY id").fetchall(), final_audit)


if __name__ == "__main__":
    unittest.main()
