"""Scoped report aggregates over real HTTP, using only temporary SQLite data."""

from datetime import datetime
import json
import os
from pathlib import Path
import unittest
from urllib.parse import urlencode
from zoneinfo import ZoneInfo

import test_roles as roles


class ReportsIntegrationTests(unittest.TestCase):
    # Reuse the existing isolated HTTP fixture without running its tests twice.
    def setUp(self):
        roles.RoleIntegrationTests.setUp(self)
        manager_id = self.users()[0]["id"]
        status, data, _ = self.api("POST", f"/api/users/{manager_id}/access-groups",
                                   {"groups": json.dumps(["sales", "finance"])}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    assign_job = roles.RoleIntegrationTests.assign_job
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time

    def start_server(self, email=roles.MANAGER_EMAIL, password=roles.MANAGER_PASSWORD):
        settings = {"TZ": "Europe/Rome",
                    "DOC_FILES_DIR": str(Path(self.fixture.name).resolve() / "documents")}
        previous = {key: os.environ.get(key) for key in settings}
        os.environ.update(settings)
        try:
            roles.RoleIntegrationTests.start_server(self, email, password)
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def create_record(self, resource, fields):
        status, data, _ = self.api("POST", "/api/" + resource, fields,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        self.assertIs(data.get("ok"), True)
        return data["id"]

    def report(self, view="date", *, cookie=None, start="2026-10-09",
               end="2026-10-10", **filters):
        query = {"view": view, "from": start, "to": end}
        query.update(filters)
        status, data, headers = self.api("GET", "/api/reports?" + urlencode(query),
                                         cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        self.assertEqual((data["view"], data["from"], data["to"]),
                         (view, start, end))
        self.assertRegex(data["today"], r"^\d{4}-\d{2}-\d{2}$")
        self.assertIn("no-store", headers.get("Cache-Control", ""))
        for key in ("minutes", "entry_count", "job_count", "user_count"):
            self.assertIs(type(data["summary"][key]), int)
            self.assertGreaterEqual(data["summary"][key], 0)
        self.assertIsInstance(data["rows"], list)
        self.assertIsInstance(data["options"]["jobs"], list)
        self.assertIsInstance(data["options"]["users"], list)
        return data

    def seed_reports(self):
        """Seven in-range entries, two outside, shared and missing contract values."""
        self.manager_id = self.users()[0]["id"]
        self.worker_id = self.create_user(name='Tecnico "Qualità" ✓')
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.other_id = self.create_user(email="second-worker@example.test", name="Secondo tecnico")
        self.other_cookie = self.login("second-worker@example.test", roles.WORKER_PASSWORD)
        self.idle_id = self.create_user(email="no-hours@example.test", name="Senza registrazioni")
        group = self.create_record("groups", {"name": "Squadra report"})
        for user_id in (self.manager_id, self.worker_id, self.other_id, self.idle_id):
            self.create_record(f"groups/{group}/members", {"user_id": user_id})
        supplier = self.create_record("suppliers", {
            "code": "REPORT-SUP", "name": 'Fornitore "Servizi" ✓'})
        self.contracts = {}
        for code, amount in (("KNOWN", 123456), ("UNKNOWN", None),
                             ("ZERO", 0), ("UNUSED", 99999)):
            fields = {"code": "REPORT-" + code, "title": "Contratto " + code,
                      "supplier_id": supplier, "status": "active",
                      "starts_on": "2026-01-01", "ends_on": "2030-12-31"}
            if amount is not None:
                fields["value_cents"] = amount
            self.contracts[code] = self.create_record("contracts", fields)
        self.jobs = {}
        for code, contract in (("A", "KNOWN"), ("B", "KNOWN"),
                               ("C", "UNKNOWN"), ("D", None),
                               ("E", "ZERO"), ("IDLE", "UNUSED")):
            fields = {"code": "REPORT-" + code, "name": "Commessa " + code,
                      "client": "Cliente " + code, "status": "active"}
            if contract:
                fields["contract_id"] = self.contracts[contract]
            self.jobs[code] = self.create_record("jobs", fields)
        for code in ("A", "C", "D"):
            self.assign_job(self.jobs[code], self.worker_id)
        for code in ("B", "E"):
            self.assign_job(self.jobs[code], self.other_id)
        entries = ((self.manager_cookie, "A", "2026-10-08", 45),
                   (self.manager_cookie, "A", "2026-10-09", 60),
                   (self.manager_cookie, "B", "2026-10-10", 90),
                   (self.manager_cookie, "C", "2026-10-11", 40),
                   (self.worker_cookie, "A", "2026-10-09", 30),
                   (self.worker_cookie, "C", "2026-10-10", 45),
                   (self.worker_cookie, "D", "2026-10-10", 15),
                   (self.other_cookie, "B", "2026-10-09", 120),
                   (self.other_cookie, "E", "2026-10-10", 20))
        for cookie, code, date, minutes in entries:
            self.create_time(cookie, self.jobs[code], date=date, minutes=minutes)
        # Inactive users retain their historical entries and must remain filterable.
        status, data, _ = self.api("POST", "/api/users/%d" % self.other_id,
                                   {"name": "Secondo tecnico", "role": "worker", "active": "0"},
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)

    def assert_summary(self, data, minutes, entries, jobs, users):
        expected = {"minutes": minutes, "entry_count": entries,
                    "job_count": jobs, "user_count": users}
        self.assertEqual({key: data["summary"][key] for key in expected}, expected)

    def test_date_aggregation_is_inclusive_and_counts_distinct_jobs_and_users(self):
        self.seed_reports()
        report = self.report()
        self.assertEqual(report["scope"], "team")
        self.assert_summary(report, 380, 7, 5, 3)
        self.assertEqual(report["rows"], [
            {"date": "2026-10-09", "minutes": 210, "entry_count": 3,
             "job_count": 2, "user_count": 3},
            {"date": "2026-10-10", "minutes": 170, "entry_count": 4,
             "job_count": 4, "user_count": 3}])
        day = self.report(start="2026-10-09", end="2026-10-09")
        self.assert_summary(day, 210, 3, 2, 3)
        self.assertEqual(day["rows"], report["rows"][:1])

    def test_job_aggregation_and_combined_user_filter(self):
        self.seed_reports()
        report = self.report("job")
        self.assert_summary(report, 380, 7, 5, 3)
        rows = {row["job_id"]: row for row in report["rows"]}
        expected = {"A": (90, 2, 2), "B": (210, 2, 2),
                    "C": (45, 1, 1), "D": (15, 1, 1), "E": (20, 1, 1)}
        self.assertEqual(set(rows), {self.jobs[code] for code in expected})
        for code, totals in expected.items():
            row = rows[self.jobs[code]]
            self.assertEqual((row["minutes"], row["entry_count"], row["user_count"]), totals)
            self.assertEqual((row["code"], row["name"], row["client"], row["status"]),
                             ("REPORT-" + code, "Commessa " + code, "Cliente " + code, "active"))
        filtered = self.report("job", job_id=self.jobs["A"], user_id=self.worker_id)
        self.assert_summary(filtered, 30, 1, 1, 1)
        self.assertEqual([row["job_id"] for row in filtered["rows"]], [self.jobs["A"]])

    def test_user_aggregation_includes_inactive_historical_identity(self):
        self.seed_reports()
        report = self.report("user")
        self.assert_summary(report, 380, 7, 5, 3)
        rows = {row["user_id"]: row for row in report["rows"]}
        self.assertEqual(set(rows), {self.manager_id, self.worker_id, self.other_id})
        for user_id, totals in ((self.manager_id, (150, 2, 2)),
                                (self.worker_id, (90, 3, 3)),
                                (self.other_id, (140, 2, 2))):
            row = rows[user_id]
            self.assertEqual((row["minutes"], row["entry_count"], row["job_count"]), totals)
        self.assertEqual(rows[self.worker_id]["name"], 'Tecnico "Qualità" ✓')
        self.assertEqual(rows[self.worker_id]["email"], roles.WORKER_EMAIL)
        self.assertFalse(rows[self.other_id]["active"])
        filtered = self.report("user", user_id=self.other_id)
        self.assert_summary(filtered, 140, 2, 2, 1)
        self.assertEqual(filtered["rows"], [rows[self.other_id]])

    def test_costs_deduplicate_contracts_distinguish_unknown_and_zero_and_keep_cents(self):
        self.seed_reports()
        report = self.report("cost")
        self.assert_summary(report, 380, 7, 5, 3)
        self.assertEqual({key: report["summary"][key] for key in (
            "contract_count", "known_value_cents", "unpriced_contract_count",
            "uncontracted_minutes")}, {"contract_count": 3, "known_value_cents": 123456,
                                      "unpriced_contract_count": 1, "uncontracted_minutes": 15})
        self.assertIs(type(report["summary"]["known_value_cents"]), int)
        rows = {row["contract_id"]: row for row in report["rows"]}
        self.assertEqual(set(rows), {self.contracts[key] for key in ("KNOWN", "UNKNOWN", "ZERO")})
        known = rows[self.contracts["KNOWN"]]
        self.assertEqual((known["minutes"], known["job_count"], known["value_cents"]), (300, 2, 123456))
        self.assertIs(type(known["value_cents"]), int)
        self.assertEqual((known["code"], known["title"], known["supplier_name"], known["currency"]),
                         ("REPORT-KNOWN", "Contratto KNOWN", 'Fornitore "Servizi" ✓', "EUR"))
        self.assertEqual((known["status"], known["starts_on"], known["ends_on"]),
                         ("active", "2026-01-01", "2030-12-31"))
        self.assertIsNone(rows[self.contracts["UNKNOWN"]]["value_cents"])
        self.assertEqual(rows[self.contracts["ZERO"]]["value_cents"], 0)
        self.assertEqual(rows[self.contracts["ZERO"]]["minutes"], 20)
        # Selecting one linked job restricts hours, but the contract value is whole.
        self.assertEqual(report["scope"], "financial")
        self.assertEqual(report["options"]["users"], [])
        filtered = self.report("cost", job_id=self.jobs["A"])
        self.assert_summary(filtered, 90, 2, 1, 2)
        self.assertEqual(len(filtered["rows"]), 1)
        self.assertEqual(filtered["rows"][0]["value_cents"], 123456)
        self.assertEqual(filtered["summary"]["known_value_cents"], 123456)

    def test_options_are_complete_for_manager_and_independent_of_filters_and_period(self):
        self.seed_reports()
        baseline = self.report("job")["options"]
        self.assertEqual({row["id"] for row in baseline["jobs"]}, set(self.jobs.values()))
        self.assertEqual({row["id"] for row in baseline["users"]},
                         {self.manager_id, self.worker_id, self.other_id, self.idle_id})
        inactive = next(row for row in baseline["users"] if row["id"] == self.other_id)
        self.assertFalse(inactive["active"])
        filtered = self.report("job", start="2026-12-01", end="2026-12-02",
                               job_id=self.jobs["A"], user_id=self.worker_id)
        self.assert_summary(filtered, 0, 0, 0, 0)
        self.assertEqual(filtered["rows"], [])
        self.assertEqual(filtered["options"], baseline)

    def test_worker_only_sees_own_history_after_assignment_revocation(self):
        self.seed_reports()
        status, data, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" %
                                   (self.jobs["A"], self.worker_id), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        for view in ("date", "job", "user"):
            with self.subTest(view=view):
                report = self.report(view, cookie=self.worker_cookie)
                self.assertEqual(report["scope"], "personal")
                self.assert_summary(report, 90, 3, 3, 1)
                self.assertEqual({row["id"] for row in report["options"]["jobs"]},
                                 {self.jobs[code] for code in ("A", "C", "D")})
                self.assertEqual([row["id"] for row in report["options"]["users"]], [self.worker_id])
                self.assertNotIn("known_value_cents", report["summary"])
                self.assertNotIn("contract_count", report["summary"])
                self.assertNotIn("second-worker@example.test", str(report))
                self.assertNotIn("REPORT-B", str(report))
                self.assertNotIn("REPORT-IDLE", str(report))
        own_job = self.report("job", cookie=self.worker_cookie, job_id=self.jobs["A"])
        self.assert_summary(own_job, 30, 1, 1, 1)
        self.assertEqual(own_job["rows"][0]["job_id"], self.jobs["A"])
        # A guessed private job ID only narrows the actor's own data to an empty set.
        private = self.report("job", cookie=self.worker_cookie, job_id=self.jobs["B"])
        self.assert_summary(private, 0, 0, 0, 0)
        self.assertEqual(private["rows"], [])
        self.assertNotIn("REPORT-B", str(private))
        self.assert_summary(self.report("job"), 380, 7, 5, 3)

    def test_worker_cannot_read_costs_or_filter_other_users(self):
        self.seed_reports()
        query = {"view": "cost", "from": "2026-10-09", "to": "2026-10-10"}
        self.assert_error(403, "GET", "/api/reports?" + urlencode(query), cookie=self.worker_cookie)
        for view in ("date", "job", "user"):
            query.update(view=view, user_id=self.manager_id)
            with self.subTest(view=view):
                self.assert_error(403, "GET", "/api/reports?" + urlencode(query), cookie=self.worker_cookie)
        own = self.report("user", cookie=self.worker_cookie, user_id=self.worker_id)
        self.assert_summary(own, 90, 3, 3, 1)

    def test_existing_session_immediately_loses_team_scope_after_manager_demotion(self):
        self.seed_reports()
        backup = self.create_user(email="remaining-manager@example.test", name="Altro gestore", role="manager")
        roles.RoleIntegrationTests.make_space_admin(self, backup)
        name = next(user["name"] for user in self.users() if user["id"] == self.manager_id)
        status, data, _ = self.api("POST", "/api/users/%d" % self.manager_id,
                                   {"name": name, "role": "worker", "active": "1"},
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        # Keep the original cookie: authorization must be re-evaluated server-side.
        report = self.report("job")
        self.assertEqual(report["scope"], "personal")
        self.assert_summary(report, 150, 2, 2, 1)
        self.assertEqual({row["job_id"] for row in report["rows"]},
                         {self.jobs["A"], self.jobs["B"]})
        self.assertEqual([row["id"] for row in report["options"]["users"]], [self.manager_id])
        # Finance is an explicit independent group; demotion revokes only team hours.
        self.assertEqual(self.report("cost")["scope"], "financial")

    def test_empty_reports_have_zero_totals_not_invented_rows_or_contracts(self):
        for view in ("date", "job", "user", "cost"):
            with self.subTest(view=view):
                report = self.report(view)
                self.assert_summary(report, 0, 0, 0, 0)
                self.assertEqual(report["rows"], [])
                if view == "cost":
                    for key in ("contract_count", "known_value_cents", "unpriced_contract_count", "uncontracted_minutes"):
                        self.assertEqual(report["summary"][key], 0)
        # Today is generated by the server, using its configured business timezone.
        self.assertEqual(report["today"], datetime.now(ZoneInfo("Europe/Rome")).date().isoformat())

    def test_authentication_method_and_strict_query_validation(self):
        base = "view=date&from=2026-10-09&to=2026-10-10"
        self.assert_error(401, "GET", "/api/reports?" + base)
        self.assert_error(405, "POST", "/api/reports?" + base, {}, cookie=self.manager_cookie)
        invalid = ("", "view=date", "from=2026-10-09&to=2026-10-10",
                   "view=date&from=2026-10-09", "view=date&to=2026-10-10",
                   base + "&unknown=value", base + "&view=job", base + "&from=2026-10-09",
                   base + "&to=2026-10-10", base + "&job_id=1&job_id=2",
                   base + "&user_id=1&user_id=2", base.replace("view=date", "view=unknown"),
                   base.replace("view=date", "view="), base + "&job_id=%FF")
        for query in invalid:
            with self.subTest(query=query):
                self.assert_error(400, "GET", "/api/reports?" + query, cookie=self.manager_cookie)
        for key in ("job_id", "user_id"):
            for value in ("", "0", "-1", "1.5", "abc", "9223372036854775808"):
                with self.subTest(key=key, value=value):
                    self.assert_error(400, "GET", "/api/reports?" + base + "&" + urlencode({key: value}),
                                      cookie=self.manager_cookie)

    def test_dates_and_maximum_366_day_inclusive_range(self):
        invalid = (("2026-02-29", "2026-03-01"), ("2026-10-10", "2026-10-09"),
                   ("2026-13-01", "2026-13-02"), ("2026-04-31", "2026-05-01"),
                   ("2026-1-01", "2026-01-02"), ("2026-01-01", "2027-01-02"),
                   ("2024-01-01", "2025-01-01"))
        for start, end in invalid:
            with self.subTest(start=start, end=end):
                query = urlencode({"view": "date", "from": start, "to": end})
                self.assert_error(400, "GET", "/api/reports?" + query, cookie=self.manager_cookie)
        for start, end in (("2024-02-29", "2024-02-29"),
                           ("2024-01-01", "2024-12-31"), ("2026-01-01", "2027-01-01")):
            with self.subTest(accepted_start=start, accepted_end=end):
                self.assert_summary(self.report(start=start, end=end), 0, 0, 0, 0)


if __name__ == "__main__":
    unittest.main()
