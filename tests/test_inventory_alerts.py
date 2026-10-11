"""Inventory reminders over verified TLS, with atomic delivery and deduplication."""

from contextlib import closing
from datetime import datetime, timedelta
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

import test_alerts as alerts
from test_calendar_client import FakeCalendarAPI
from test_mail_client import create_certificate
import test_roles as roles


TOKEN = "inventory-alerts-" + "d" * 64


class PagedPrivateAPI(FakeCalendarAPI):
    """Queue source pages without changing the shared TLS fixture."""

    def __init__(self, certificate, key):
        self.replies = []
        self.reply_hook = None
        super().__init__(certificate, key)

    @property
    def reply_value(self):
        if self.reply_hook:
            hook, self.reply_hook = self.reply_hook, None
            hook()
        if self.replies:
            self.reply_status, value = self.replies.pop(0)
            return value
        return self._reply_value

    @reply_value.setter
    def reply_value(self, value):
        self._reply_value = value


class InventoryAlertTests(unittest.TestCase):
    start_server = alerts.AlertIntegrationTests.start_server
    stop_server = alerts.AlertIntegrationTests.stop_server
    api = alerts.AlertIntegrationTests.api
    login = alerts.AlertIntegrationTests.login
    assert_error = alerts.AlertIntegrationTests.assert_error
    create_user = alerts.AlertIntegrationTests.create_user
    users = alerts.AlertIntegrationTests.users
    edit_user = alerts.AlertIntegrationTests.edit_user
    rule_fields = alerts.AlertIntegrationTests.rule_fields
    create_rule = alerts.AlertIntegrationTests.create_rule
    update_rule = alerts.AlertIntegrationTests.update_rule
    run_rule = alerts.AlertIntegrationTests.run_rule
    rules = alerts.AlertIntegrationTests.rules
    notifications = alerts.AlertIntegrationTests.notifications
    worker = alerts.AlertIntegrationTests.worker

    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-inventory-alerts-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "inventory-alerts")
        cls.native_check = alerts.build_native_check(cls.certificates.name)

    def setUp(self):
        self.service = PagedPrivateAPI(self.cert, self.key)
        self.addCleanup(self.service.close)
        self.service.reply_value = {"ok": True, "items": [], "next_after_id": None}
        token = Path(self.certificates.name) / "inventory-token"
        token.write_text(TOKEN + "\n", encoding="ascii")
        token.chmod(0o600)
        self.environment = {"INVENTORY_API_URL": self.service.url,
                            "INVENTORY_API_CA_FILE": str(self.cert),
                            "INVENTORY_API_TOKEN_FILE": str(token)}
        with patch.dict(os.environ, self.environment):
            alerts.AlertIntegrationTests.setUp(self)
        self.manager_id = next(user["id"] for user in self.users() if user["email"] == roles.MANAGER_EMAIL)
        self.today = datetime.now(alerts.ROME).date()

    def due(self, asset_id=17, kind="warranty_until", remaining=10, **changes):
        value = {"asset_id": asset_id, "asset_tag": "IT-%d" % asset_id,
                 "name": 'Portatile "Qualità" ✓', "asset_type": "it_device",
                 "due_kind": kind, "due_on": (self.today + timedelta(days=remaining)).isoformat(),
                 "days_remaining": remaining}
        if kind in ("expires_on", "renewal_on"):
            value["asset_type"] = "license"
        elif kind == "maintenance_due":
            value["asset_type"] = "physical"
        value.update(changes)
        return value

    def source(self, *items):
        self.service.reply_value = {"ok": True, "items": sorted(items, key=lambda item: item["asset_id"], reverse=True),
                                    "next_after_id": None}

    def inventory_rule(self, **changes):
        return self.create_rule(type="inventory_due", **changes)

    def operational_due(self, asset_id=17, kind="return", remaining=-1, source_id=71, **changes):
        return self.due(asset_id, kind + "_due", remaining, source_id=source_id, **changes)

    def state(self, rule_id):
        with closing(sqlite3.connect(self.db_path)) as db:
            return (db.execute("SELECT last_run_at,last_run_on FROM alert_rules WHERE id=?", (rule_id,)).fetchone(),
                    db.execute("SELECT COUNT(*) FROM alert_notifications").fetchone()[0],
                    db.execute("SELECT COUNT(*) FROM alert_email_outbox").fetchone()[0])

    def test_rule_threshold_defaults_and_validation(self):
        rule = self.inventory_rule()
        self.assertEqual(self.rules()["rules"][0]["days_before"], 30)
        self.update_rule(rule, type="inventory_due", days_before="365")
        self.assertEqual(self.rules()["rules"][0]["days_before"], 365)
        for value in ("", "0", "366", "-1", "1.5", "30x"):
            self.assert_error(400, "POST", "/api/alert-rules", self.rule_fields(type="inventory_due", days_before=value),
                              cookie=self.manager_cookie)

    def test_operational_rule_types_and_verification_interval_validation(self):
        for rule_type in ("inventory_return", "inventory_verification"):
            rule = self.create_rule(type=rule_type)
            saved = next(item for item in self.rules()["rules"] if item["id"] == rule)
            self.assertEqual((saved["type"], saved["verification_days"], saved["days_before"]), (rule_type, 90, 30))
            self.update_rule(rule, type=rule_type, verification_days="365")
            self.assertEqual(next(item for item in self.rules()["rules"] if item["id"] == rule)["verification_days"], 365)
            for value in ("", "0", "366", "-1", "1.5", "90x"):
                self.assert_error(400, "POST", "/api/alert-rules", self.rule_fields(type=rule_type, verification_days=value),
                                  cookie=self.manager_cookie)

    def test_return_notifications_are_shared_and_each_checkout_has_its_own_deduplication(self):
        worker_id, worker = self.worker()
        rule = self.create_rule(type="inventory_return", recipient_user_ids="%d,%d" % (self.manager_id, worker_id),
                                recipient_emails="assets@example.test")
        self.source(self.operational_due())
        result = self.run_rule(rule)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (2, 1))
        for cookie in (self.manager_cookie, worker):
            notification = self.notifications(cookie)["notifications"][0]
            self.assertEqual((notification["event_kind"], notification["due_kind"], notification["source_id"]),
                             ("inventory_return", "return_due", 71))
            self.assertEqual(notification["asset_id"], 17)
            self.assertIn("in ritardo da 1 giorni", notification["message"])
            self.assertNotIn("days_before", notification)
        self.assertEqual(self.run_rule(rule)["notifications_created"], 0)
        self.source(self.operational_due(source_id=72))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 2)
        self.assertEqual(self.state(rule)[1:], (4, 2))
        request = urlsplit(self.service.requests[-1]["path"])
        self.assertEqual(request.path, "/v1/operational-due")
        self.assertEqual(parse_qs(request.query)["kind"], ["return"])

    def test_verification_notifications_include_never_verified_and_interval_boundary(self):
        rule = self.create_rule(type="inventory_verification", recipient_emails="assets@example.test")
        self.source(self.operational_due(17, "verification", -100, 0), self.operational_due(18, "verification", 0, 41))
        result = self.run_rule(rule)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (2, 2))
        notifications = {item["asset_id"]: item for item in self.notifications()["notifications"]}
        self.assertIn("non è mai stato verificato", notifications[17]["message"])
        self.assertIn("raggiunto oggi", notifications[18]["message"])
        for item in notifications.values():
            self.assertEqual((item["event_kind"], item["due_kind"], item["verification_days"]),
                             ("inventory_verification", "verification_due", 90))
        query = parse_qs(urlsplit(self.service.requests[-1]["path"]).query)
        self.assertEqual((query["kind"], query["verification_days"]), (["verification"], ["90"]))

    def test_verification_deduplication_tracks_new_verbal_and_changed_interval(self):
        rule = self.create_rule(type="inventory_verification")
        self.source(self.operational_due(kind="verification", source_id=41))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.assertEqual(self.run_rule(rule)["notifications_created"], 0)
        self.source(self.operational_due(kind="verification", source_id=42))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.update_rule(rule, type="inventory_verification", verification_days="91")
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.assertEqual(self.run_rule(rule)["notifications_created"], 0)

    def test_operational_source_failure_after_first_page_is_atomic(self):
        for kind in ("return", "verification"):
            with self.subTest(kind=kind):
                rule = self.create_rule(type="inventory_" + kind, recipient_emails="assets@example.test")
                before = self.state(rule)
                self.service.replies = [
                    (200, {"ok": True, "items": [self.operational_due(200, kind)], "next_after_id": 200}),
                    (503, {"ok": False, "error": "Temporary source failure"}),
                ]
                self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
                self.assertEqual(self.state(rule), before)
                self.service.replies = [
                    (200, {"ok": True, "items": [self.operational_due(200, kind)], "next_after_id": 200}),
                    (200, {"ok": True, "items": [self.operational_due(199, kind)], "next_after_id": None}),
                ]
                self.assertEqual(self.run_rule(rule)["notifications_created"], 2)

    def test_operational_source_rejects_invalid_dates_records_types_and_future_deadlines(self):
        for kind in ("return", "verification"):
            rule = self.create_rule(type="inventory_" + kind)
            before = self.state(rule)
            for changes in ({"source_id": -1}, {"source_id": "71"}, {"days_remaining": 0},
                            {"due_on": "2026-02-30"}, {"asset_type": "license"}, {"due_kind": "warranty_until"},
                            {"remaining": 1 if kind == "verification" else 0}):
                with self.subTest(kind=kind, changes=changes):
                    self.source(self.operational_due(kind=kind, **changes))
                    self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
                    self.assertEqual(self.state(rule), before)
        self.source(self.operational_due(source_id=0))
        rule = self.create_rule(type="inventory_return")
        self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)

    def test_changed_verification_policy_during_fetch_does_not_consume_the_run(self):
        rule = self.create_rule(type="inventory_verification", recipient_emails="assets@example.test")
        self.source(self.operational_due(kind="verification"))

        def change_policy():
            with closing(sqlite3.connect(self.db_path)) as database:
                database.execute("UPDATE alert_rules SET verification_days=91 WHERE id=?", (rule,))
                database.commit()

        self.service.reply_hook = change_policy
        self.assert_error(409, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(self.state(rule)[1:], (0, 0))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)

    def test_changed_inventory_rule_type_during_fetch_creates_no_notifications(self):
        rule = self.create_rule(type="inventory_return", recipient_emails="assets@example.test")
        self.source(self.operational_due())
        before = self.state(rule)

        def change_type():
            with closing(sqlite3.connect(self.db_path)) as database:
                database.execute("UPDATE alert_rules SET type='invalid_job' WHERE id=?", (rule,))
                database.commit()

        self.service.reply_hook = change_type
        self.assert_error(409, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(self.state(rule), before)

    def test_operational_delivery_sql_failure_rolls_back_all_channels(self):
        for kind in ("return", "verification"):
            with self.subTest(kind=kind):
                rule = self.create_rule(type="inventory_" + kind, recipient_emails="assets@example.test")
                self.source(self.operational_due(17, kind), self.operational_due(18, kind))
                before = self.state(rule)
                with closing(sqlite3.connect(self.db_path)) as database:
                    database.execute("CREATE TRIGGER reject_operational_alert BEFORE INSERT ON alert_notifications "
                                     "WHEN NEW.event_key LIKE 'inventory_" + kind + ":17:%' "
                                     "BEGIN SELECT RAISE(ABORT,'fixture failure'); END")
                    database.commit()
                self.assert_error(500, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
                self.assertEqual(self.state(rule), before)
                with closing(sqlite3.connect(self.db_path)) as database:
                    database.execute("DROP TRIGGER reject_operational_alert")
                    database.commit()
                self.assertEqual(self.run_rule(rule)["notifications_created"], 2)

    def test_four_due_kinds_include_overdue_and_all_selected_active_users(self):
        worker_id, worker = self.worker()
        rule = self.inventory_rule(recipient_user_ids="%d,%d" % (self.manager_id, worker_id),
                                   recipient_emails="assets@example.test")
        self.source(self.due(17, remaining=-20), self.due(18, "maintenance_due", 0),
                    self.due(19, "expires_on", 30), self.due(20, "renewal_on", 1))
        result = self.run_rule(rule)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (8, 4))
        for cookie in (self.manager_cookie, worker):
            data = self.notifications(cookie)
            self.assertEqual((len(data["notifications"]), data["unread_count"]), (4, 4))
            self.assertEqual({item["due_kind"] for item in data["notifications"]},
                             {"warranty_until", "maintenance_due", "expires_on", "renewal_on"})
            for item in data["notifications"]:
                self.assertEqual(item["event_kind"], "inventory_due")
                self.assertIsNone(item["work_date"])
                self.assertIsNone(item["recorded_minutes"])
                self.assertEqual(item["days_before"], 30)
            first = data["notifications"][0]
            status, _, _ = self.api("POST", "/api/notifications/%d/read" % first["id"], {}, cookie=cookie)
            self.assertEqual(status, 200)
        request = self.service.requests[-1]
        self.assertEqual(request["authorization"], "Bearer " + TOKEN)
        query = parse_qs(urlsplit(request["path"]).query)
        self.assertEqual(query.get("today"), [self.today.isoformat()])
        self.assertEqual(query.get("days_before"), ["30"])

    def test_repeat_is_deduplicated_but_date_or_threshold_change_notifies_again(self):
        rule = self.inventory_rule(recipient_emails="assets@example.test")
        self.source(self.due())
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.assertEqual(self.run_rule(rule)["notifications_created"], 0)
        self.assertEqual(self.state(rule)[1:], (1, 1))
        self.source(self.due(remaining=11))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.update_rule(rule, type="inventory_due", days_before="31", recipient_emails="assets@example.test")
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        self.assertEqual(self.run_rule(rule)["notifications_created"], 0)
        self.assertEqual(self.state(rule)[1:], (3, 3))

    def test_valid_utf8_asset_names_use_character_limits(self):
        rule = self.inventory_rule()
        self.source(self.due(asset_tag="字" * 64, name="資" * 160))
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)
        message = self.notifications()["notifications"][0]["message"]
        self.assertIn("字" * 64, message)
        self.assertIn("資" * 160, message)

    def test_source_pagination_keeps_all_deadlines_of_each_asset(self):
        rule = self.inventory_rule(recipient_emails="assets@example.test")
        self.service.replies = [
            (200, {"ok": True, "items": [self.due(200, asset_type="license"), self.due(200, "expires_on")],
                   "next_after_id": 200}),
            (200, {"ok": True, "items": [self.due(199, "maintenance_due")], "next_after_id": None}),
        ]
        result = self.run_rule(rule)
        self.assertEqual((result["notifications_created"], result["emails_queued"]), (3, 3))
        self.assertEqual(len(self.service.requests), 2)
        self.assertEqual(parse_qs(urlsplit(self.service.requests[-1]["path"]).query)["after_id"], ["200"])
        self.assertEqual({(item["asset_id"], item["due_kind"]) for item in self.notifications()["notifications"]},
                         {(200, "warranty_until"), (200, "expires_on"), (199, "maintenance_due")})

    def test_failed_later_source_page_never_delivers_a_partial_list(self):
        rule = self.inventory_rule(recipient_emails="assets@example.test")
        before = self.state(rule)
        self.service.replies = [
            (200, {"ok": True, "items": [self.due(200)], "next_after_id": 200}),
            (503, {"ok": False, "error": "Temporary source failure"}),
        ]
        self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(len(self.service.requests), 2)
        self.assertEqual(self.state(rule), before)

    def test_source_cursor_must_equal_last_asset_to_prevent_skipped_pages(self):
        for rule_type in ("inventory_due", "inventory_return", "inventory_verification"):
            with self.subTest(rule_type=rule_type):
                rule = self.create_rule(type=rule_type, recipient_emails="assets@example.test")
                before = self.state(rule)
                if rule_type == "inventory_due":
                    first, second = self.due(200), self.due(99)
                else:
                    kind = rule_type.removeprefix("inventory_")
                    first, second = self.operational_due(200, kind), self.operational_due(99, kind)
                requests_before = len(self.service.requests)
                self.service.replies = [
                    (200, {"ok": True, "items": [first], "next_after_id": 100}),
                    (200, {"ok": True, "items": [second], "next_after_id": None}),
                ]
                self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
                self.assertEqual(self.state(rule), before)
                self.assertEqual(len(self.service.requests), requests_before + 1,
                                 "A corrupt cursor must be rejected before fetching its next page.")
                self.service.replies.clear()

    def test_unavailable_and_invalid_source_do_not_consume_run_or_deliver(self):
        rule = self.inventory_rule(recipient_emails="assets@example.test")
        before = self.state(rule)
        self.service.reply_status = 503
        self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(self.state(rule), before)
        self.service.reply_status = 200
        for invalid in (self.due(days_remaining="10"), self.due(due_on="2026-02-30"),
                        self.due(due_kind="unknown"), self.due(asset_id=0),
                        self.due(days_remaining=9), self.due(remaining=31)):
            self.source(self.due(18), invalid)
            status, data, _ = self.api("POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
            self.assertGreaterEqual(status, 500, data)
            self.assertEqual(self.state(rule), before)
        self.source(self.due())
        self.assertEqual(self.run_rule(rule)["notifications_created"], 1)

    def test_delivery_uses_current_recipient_activity_and_survives_demotion(self):
        worker_id, worker = self.worker()
        other_id = self.create_user(email="asset-recipient@example.test", role="manager")
        other = self.login("asset-recipient@example.test", roles.WORKER_PASSWORD)
        rule = self.inventory_rule(recipient_user_ids="%d,%d,%d" % (self.manager_id, worker_id, other_id))
        self.edit_user(worker_id, active="0")
        self.edit_user(other_id, role="worker")
        self.source(self.due())
        self.assertEqual(self.run_rule(rule)["notifications_created"], 2)
        self.assertEqual(len(self.notifications(other)["notifications"]), 1)
        self.assert_error(401, "GET", "/api/notifications", cookie=worker)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT recipient_id,subject_user_id FROM alert_notifications ORDER BY recipient_id").fetchall(),
                             [(self.manager_id, self.manager_id), (other_id, other_id)])

    def test_sql_failure_rolls_back_all_notifications_email_and_run_metadata(self):
        rule = self.inventory_rule(recipient_emails="assets@example.test")
        self.source(self.due(17), self.due(18))
        before = self.state(rule)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER reject_inventory_alert BEFORE INSERT ON alert_notifications "
                       "WHEN NEW.event_key LIKE 'inventory_due:17:%' BEGIN SELECT RAISE(ABORT,'fixture failure'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(self.state(rule), before)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TRIGGER reject_inventory_alert")
            db.commit()
        self.assertEqual(self.run_rule(rule)["notifications_created"], 2)

    def test_disabled_inventory_configuration_fails_without_consuming_run(self):
        rule = self.inventory_rule()
        before = self.state(rule)
        self.stop_server()
        with patch.dict(os.environ, {key: "" for key in self.environment}):
            self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assert_error(503, "POST", "/api/alert-rules/%d/run" % rule, {}, cookie=self.manager_cookie)
        self.assertEqual(self.state(rule), before)

    def test_scheduler_retries_failed_source_and_deduplicates_across_days(self):
        rule = self.inventory_rule()
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("UPDATE alert_rules SET enabled=1,run_time='08:00',last_run_on=NULL WHERE id=?", (rule,))
            db.commit()
        before = self.state(rule)

        def tick(day):
            epoch = int(datetime.fromisoformat(day.isoformat() + "T08:00:00").replace(tzinfo=alerts.ROME).timestamp())
            with patch.dict(os.environ, self.environment):
                subprocess.run([str(self.native_check), "tick", str(self.db_path), str(epoch)],
                               check=True, capture_output=True, text=True)

        self.service.reply_status = 503
        tick(self.today)
        self.assertEqual(self.state(rule), before)
        self.service.reply_status = 200
        self.source(self.due())
        tick(self.today)
        self.assertEqual(self.state(rule)[0][1], self.today.isoformat())
        self.assertEqual(self.state(rule)[1:], (1, 0))
        requests = len(self.service.requests)
        tick(self.today)
        self.assertEqual(len(self.service.requests), requests)
        self.source(self.due(days_remaining=9))
        tick(self.today + timedelta(days=1))
        self.assertEqual(self.state(rule)[0][1], (self.today + timedelta(days=1)).isoformat())
        self.assertEqual(self.state(rule)[1:], (1, 0))


if __name__ == "__main__":
    unittest.main()
