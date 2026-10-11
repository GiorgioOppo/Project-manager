"""Published commercial data, confirmed proposals and verified bridges over real HTTP."""
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

import test_roles as roles
import test_procurement as procurement
from test_calendar_client import FakeCalendarAPI
from test_mail_client import create_certificate

PREFIX = "/api/improvements/integrations/"


class IntegrationImprovementsTests(unittest.TestCase):
    start_server = procurement.ProcurementIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    edit_user = roles.RoleIntegrationTests.edit_user
    assert_error = roles.RoleIntegrationTests.assert_error
    assign_job = roles.RoleIntegrationTests.assign_job
    supplier = procurement.ProcurementIntegrationTests.supplier
    contract = procurement.ProcurementIntegrationTests.contract
    raw = procurement.ProcurementIntegrationTests.raw
    upload = procurement.ProcurementIntegrationTests.upload
    create_time = roles.RoleIntegrationTests.create_time
    time_fields = roles.RoleIntegrationTests.time_fields

    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-integrations-tls-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "private-integrations")

    def setUp(self):
        self.service = FakeCalendarAPI(self.cert, self.key)
        self.addCleanup(self.service.close)
        self.service.reply_value = {"ok": True, "items": []}
        token = Path(self.certificates.name) / "token"
        token.write_text("integrations-" + "a" * 64 + "\n", encoding="ascii")
        token.chmod(0o600)
        env = {f"{prefix}_API_{suffix}": value for prefix in ("INVENTORY", "CALENDAR")
               for suffix, value in (("URL", self.service.url), ("CA_FILE", str(self.cert)), ("TOKEN_FILE", str(token)))}
        self.environment = env
        with patch.dict(os.environ, env):
            procurement.ProcurementIntegrationTests.setUp(self)
        self.manager_id = self.get("/api/session")["user_id"]
        self.post(f"/api/users/{self.manager_id}/access-groups", {"groups": '["sales","finance"]'})
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.job_id = self.post("/api/jobs", {"code": "INT-01", "name": "Integrazioni", "status": "active"}, expected=201)["id"]
        self.post(f"/api/jobs/{self.job_id}/assignments", {"user_id": self.worker_id, "role_id": 2}, expected=201)

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def post(self, path, fields, cookie=None, expected=200):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def sql(self, statement, values=()):
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("PRAGMA foreign_keys=ON")
            rows = db.execute(statement, values).fetchall()
            db.commit()
            return rows

    def rate(self, cents=2500):
        return self.post("/api/labor-rates", {"user_id": self.worker_id, "starts_on": "2026-01-01",
                         "ends_on": "", "hourly_cents": cents, "currency": "EUR"}, expected=201)["id"]

    def draft(self, **changes):
        fields = {"source": "ticket", "source_ref": "ticket:42", "entries": json.dumps([
            {"job_id": self.job_id, "date": "2026-10-10", "minutes": 60, "note": "Intervento verificato"}])}
        fields.update(changes)
        return self.post(PREFIX + "time-drafts", fields, self.worker_cookie, expected=201)

    def preview(self, **changes):
        return self.post(PREFIX + "economic-periods", {"from": "2026-10-10", "to": "2026-10-10", "action": "preview", **changes})

    def approve(self, preview, **changes):
        return self.post(PREFIX + "economic-periods", {"from": preview["from"], "to": preview["to"], "action": "approve",
                         "name": "Ottobre approvato", "reason": "Controllo consuntivo", "preview_hash": preview["preview_hash"], **changes}, expected=201)

    def test_published_contract_snapshot_and_job_binding_are_explicit(self):
        contract = self.contract(value_cents=123400, notes="Originale")
        self.post(f"/api/jobs/{self.job_id}", {"contract_id": contract, "code": "INT-01", "name": "Integrazioni", "status": "active"})
        status, data, _ = self.upload(contract, b"%PDF-1.4\nOriginal immutable PDF\n%%EOF\n")
        self.assertEqual(status, 201, data)
        draft = self.post(PREFIX + "contract-revisions", {"contract_id": contract, "reason": "Prima revisione", "title": "Titolo pubblicato"}, expected=201)
        self.assert_error(422, "POST", PREFIX + "job-contract-revision", {"job_id": self.job_id, "revision_id": draft["id"]}, cookie=self.manager_cookie)
        self.post(PREFIX + "contract-revisions/publish", {"id": draft["id"], "revision": 1, "reason": "Approvazione"})
        self.post(PREFIX + "job-contract-revision", {"job_id": self.job_id, "revision_id": draft["id"]})
        self.post(f"/api/contracts/{contract}", {"title": "Titolo corrente cambiato", "value_cents": 999900})
        revision = self.get(PREFIX + f"contract-revisions?contract_id={contract}")["revisions"][0]
        self.assertEqual((revision["status"], revision["snapshot"]["title"], revision["snapshot"]["value_cents"]), ("published", "Titolo pubblicato", 123400))
        self.assertEqual(len(revision["snapshot"]["documents"]), 1)
        linked = self.get(PREFIX + f"job-contract-revision?job_id={self.job_id}")["contract_revision"]
        self.assertEqual(linked["revision_id"], draft["id"])
        self.assert_error(409, "POST", PREFIX + "contract-revisions/publish", {"id": draft["id"], "revision": 2, "reason": "Sovrascrittura"}, cookie=self.manager_cookie)

    def test_sales_finance_permissions_are_live(self):
        contract = self.contract()
        self.post(f"/api/users/{self.manager_id}/access-groups", {"groups": '["finance"]'})
        self.get(PREFIX + f"contract-revisions?contract_id={contract}")
        self.assert_error(403, "POST", PREFIX + "contract-revisions", {"contract_id": contract, "reason": "Non consentito"}, cookie=self.manager_cookie)
        self.post(f"/api/users/{self.manager_id}/access-groups", {"groups": '[]'})
        self.assert_error(403, "GET", PREFIX + f"contract-revisions?contract_id={contract}", cookie=self.manager_cookie)
        self.assert_error(403, "GET", PREFIX + f"budget?job_id={self.job_id}", cookie=self.manager_cookie)
        self.assert_error(403, "GET", PREFIX + "economic-periods", cookie=self.worker_cookie)
        self.assert_error(403, "GET", PREFIX + "budget", cookie=self.worker_cookie)
        self.post(f"/api/users/{self.worker_id}/access-groups", {"groups": '["finance"]'})
        catalog = self.get(PREFIX + "budget", self.worker_cookie)["jobs"]
        self.assertEqual(catalog, [{"id": self.job_id, "code": "INT-01", "name": "Integrazioni"}])

    def test_period_snapshot_survives_rate_changes_and_explicit_correction(self):
        rate = self.rate()
        entry = self.create_time(self.worker_cookie, self.job_id)
        preview = self.preview()
        self.assertEqual((preview["minutes"], preview["cost_cents"]), (60, 2500))
        period = self.approve(preview)
        self.assert_error(409, "POST", "/api/time-entries", self.time_fields(self.job_id), cookie=self.worker_cookie)
        self.assert_error(403, "POST", f"/api/time-entries/{entry}", self.time_fields(self.job_id, minutes=30), cookie=self.worker_cookie)
        self.post(f"/api/labor-rates/{rate}", {"user_id": self.worker_id, "starts_on": "2026-01-01", "ends_on": "", "hourly_cents": 9000, "currency": "EUR"})
        closed = self.get(PREFIX + "economic-periods")["periods"][0]
        self.assertEqual(closed["cost_cents"], 2500)
        self.assert_error(409, "DELETE", f"/api/jobs/{self.job_id}", cookie=self.manager_cookie)
        self.assertEqual(self.get(PREFIX + "economic-periods")["periods"][0]["rows"], closed["rows"])
        self.post(PREFIX + "economic-corrections", {"period_id": period["id"], "job_id": self.job_id, "delta_cents": -200, "reason": "Rettifica approvata separatamente"}, expected=201)
        self.post(PREFIX + "budget", {"job_id": self.job_id, "budget_cents": 10000, "remaining_cents": 3000, "revision": 0, "reason": "Stima a finire"})
        budget = self.get(PREFIX + f"budget?job_id={self.job_id}")
        self.assertEqual((budget["approved_cents"], budget["actual_cents"], budget["forecast_cents"], budget["variance_cents"]), (2500, 2300, 5300, 4700))
        self.assertEqual(self.get(PREFIX + "economic-periods")["periods"][0]["rows"], closed["rows"])
        self.assert_error(409, "POST", PREFIX + "budget", {"job_id": self.job_id, "budget_cents": 10000, "remaining_cents": 2000, "revision": 5, "reason": "Revisione obsoleta"}, cookie=self.manager_cookie)

    def test_period_preview_rejects_stale_and_unpriced_costs_and_rolls_back_audit(self):
        self.create_time(self.worker_cookie, self.job_id)
        preview = self.preview()
        fields = {"from": preview["from"], "to": preview["to"], "name": "Periodo", "reason": "Verifica", "action": "approve", "preview_hash": preview["preview_hash"]}
        self.assert_error(409, "POST", PREFIX + "economic-periods", fields, cookie=self.manager_cookie)
        self.rate()
        self.assert_error(409, "POST", PREFIX + "economic-periods", fields, cookie=self.manager_cookie)
        fields["preview_hash"] = self.preview()["preview_hash"]
        self.sql("CREATE TRIGGER fail_economic_audit BEFORE INSERT ON audit_events WHEN NEW.action='economic_periods.approve' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        self.assert_error(500, "POST", PREFIX + "economic-periods", fields, cookie=self.manager_cookie)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM ii_economic_periods"), [(0,)])
        self.create_time(self.worker_cookie, self.job_id, minutes=30)

    def test_ticket_draft_requires_confirmation_and_replays_without_duplicates(self):
        draft = self.draft()
        self.assertEqual(self.sql("SELECT COUNT(*) FROM time_entries"), [(0,)])
        self.assertEqual(self.get(PREFIX + "time-drafts")["drafts"], [])
        fields = {"id": draft["id"], "revision": draft["revision"]}
        self.assert_error(404, "POST", PREFIX + "time-drafts/confirm", fields, cookie=self.manager_cookie)
        self.post(PREFIX + "time-drafts/confirm", fields, self.worker_cookie, expected=201)
        self.post(PREFIX + "time-drafts/confirm", fields, self.worker_cookie)
        self.assertEqual(self.sql("SELECT minutes,note FROM time_entries"), [(60, "Intervento verificato")])
        self.assertEqual(self.get(PREFIX + "time-drafts", self.worker_cookie)["drafts"][0]["state"], "confirmed")
        self.assert_error(409, "POST", PREFIX + "time-drafts", {"source": "ticket", "source_ref": "ticket:42", "entries": json.dumps(draft["entries"]), **fields}, cookie=self.worker_cookie)

    def test_draft_validates_live_access_and_atomic_daily_limit(self):
        entries = [{"job_id": self.job_id, "date": "2026-10-10", "minutes": 800, "note": ""}] * 2
        draft = self.draft(entries=json.dumps(entries))
        self.assert_error(409, "POST", PREFIX + "time-drafts/confirm", {"id": draft["id"], "revision": 1}, cookie=self.worker_cookie)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM time_entries"), [(0,)])
        self.sql("DELETE FROM job_assignments WHERE job_id=? AND user_id=?", (self.job_id, self.worker_id))
        self.assert_error(403, "POST", PREFIX + "time-drafts/confirm", {"id": draft["id"], "revision": 1}, cookie=self.worker_cookie)
        self.assert_error(422, "POST", PREFIX + "time-drafts", {"source": "ticket", "source_ref": "x", "entries": '[{"job_id":1,"date":"2026-02-30","minutes":1.5,"note":""}]'}, cookie=self.worker_cookie)

    def test_draft_recovers_committed_batch_after_period_closure_without_duplicate(self):
        self.rate()
        draft = self.draft()
        fields = {"id": draft["id"], "revision": draft["revision"]}
        self.sql("CREATE TRIGGER fail_draft_mark BEFORE UPDATE ON ii_time_drafts WHEN NEW.state='confirmed' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        self.post(PREFIX + "time-drafts/confirm", fields, self.worker_cookie, expected=201)
        self.assertEqual(self.sql("SELECT state FROM ii_time_drafts"), [("confirming",)])
        self.sql("DROP TRIGGER fail_draft_mark")
        self.approve(self.preview())
        self.sql("DELETE FROM job_assignments WHERE job_id=? AND user_id=?", (self.job_id, self.worker_id))
        self.post(PREFIX + "time-drafts/confirm", fields, self.worker_cookie)
        self.assertEqual(self.sql("SELECT state FROM ii_time_drafts"), [("confirmed",)])
        self.assertEqual(self.sql("SELECT COUNT(*),SUM(minutes) FROM time_entries"), [(1, 60)])

    def test_calendar_proposal_checks_private_actor_and_splits_midnight(self):
        self.service.reply_value = {"event": {"id": "private-event", "summary": "Intervento serale", "start": {"dateTime": "2026-10-10T23:30:00+02:00"}, "end": {"dateTime": "2026-10-11T01:00:00+02:00"}}}
        draft = self.post(PREFIX + "time-drafts", {"source": "calendar", "source_ref": "private-event", "job_id": self.job_id}, self.worker_cookie, expected=201)
        self.assertEqual([(row["date"], row["minutes"]) for row in draft["entries"]], [("2026-10-10", 30), ("2026-10-11", 60)])
        request = self.service.requests[-1]
        self.assertEqual(urlsplit(request["path"]).path, f"/v1/users/{self.worker_id}/event")
        self.assertEqual(parse_qs(urlsplit(request["path"]).query), {"event_id": ["private-event"]})
        self.assertEqual(self.sql("SELECT COUNT(*) FROM time_entries"), [(0,)])
        self.service.reply_value = {"event": {"id": "all-day", "start": {"date": "2026-10-10"}, "end": {"date": "2026-10-11"}}}
        self.assert_error(422, "POST", PREFIX + "time-drafts", {"source": "calendar", "source_ref": "all-day", "job_id": self.job_id}, cookie=self.worker_cookie)
        self.service.reply_status = 404
        self.service.reply_value = {"error": "event_not_found"}
        self.assert_error(422, "POST", PREFIX + "time-drafts", {"source": "calendar", "source_ref": "someone-elses-event", "job_id": self.job_id}, cookie=self.worker_cookie)

    def seed_email(self, status, event):
        rule = self.post("/api/alert-rules", {"name": "Avviso fixture", "type": "invalid_job", "run_time": "23:59", "enabled": "0", "recipient_user_ids": str(self.manager_id), "recipient_emails": ""}, expected=201)["id"]
        self.sql("INSERT INTO alert_email_outbox(rule_id,email,subject_user_id,event_key,title,message,created_at,status) VALUES(?,? ,?,?,?,?,'2026-10-11T00:00:00Z',?)", (rule, "delivery@example.test", self.manager_id, event, "Titolo", "Corpo", status))
        return self.sql("SELECT MAX(id) FROM alert_email_outbox")[0][0]

    def test_outbox_cancel_unknown_review_and_in_flight_guard(self):
        pending = self.seed_email("pending_configuration", "fixture:pending")
        unknown = self.seed_email("delivery_unknown", "fixture:unknown")
        self.post(PREFIX + "email-outbox/decision", {"id": pending, "revision": 0, "status": "pending_configuration", "decision": "cancel", "reason": "Non più necessario"})
        self.post(PREFIX + "email-outbox/decision", {"id": unknown, "revision": 0, "status": "delivery_unknown", "decision": "review", "reason": "Verifica presso il fornitore", "outcome": "confirmed_sent"})
        items = {item["id"]: item for item in self.get(PREFIX + "email-outbox")["items"]}
        self.assertEqual((items[pending]["status"], items[unknown]["review_outcome"]), ("cancelled", "confirmed_sent"))
        self.assertEqual(self.sql("SELECT status FROM alert_email_outbox WHERE id=?", (unknown,)), [("delivery_unknown",)])
        self.assert_error(409, "POST", PREFIX + "email-outbox/decision", {"id": pending, "revision": 0, "status": "pending_configuration", "decision": "cancel", "reason": "Doppio click"}, cookie=self.manager_cookie)
        sending = self.seed_email("retrying", "fixture:sending")
        self.sql("INSERT INTO ii_email_controls(outbox_id,state,revision) VALUES(?,'sending',1)", (sending,))
        self.assert_error(409, "POST", PREFIX + "email-outbox/decision", {"id": sending, "revision": 1, "status": "retrying", "decision": "cancel", "reason": "Troppo tardi"}, cookie=self.manager_cookie)
        self.assert_error(403, "GET", PREFIX + "email-outbox", cookie=self.worker_cookie)

    def test_inventory_mapping_is_verified_direct_and_removed_for_inactive_account(self):
        self.service.reply_value = {"person": {"id": 7, "display_name": "Persona verificata", "active": True}}
        self.post(PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 0})
        self.service.reply_value = {"ok": True, "items": []}
        self.get("/api/inventory/assets?mine=1", self.worker_cookie)
        query = parse_qs(urlsplit(self.service.requests[-1]["path"]).query)
        self.assertEqual(query, {"assignee_user_id": [str(self.worker_id)], "mapped_person_id": ["7"]})
        self.assert_error(400, "GET", "/api/inventory/assets?mapped_person_id=99", cookie=self.worker_cookie)
        self.assert_error(403, "POST", PREFIX + "inventory-person-accounts", {"person_id": 9, "user_id": self.worker_id, "revision": 0}, cookie=self.worker_cookie)
        self.edit_user(self.worker_id, active="0")
        self.post(PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 1, "remove": "1"})
        self.assertEqual(self.get(PREFIX + "inventory-person-accounts")["mappings"], [])

    def test_mapping_scope_and_upstream_failure_do_not_write(self):
        scoped = self.create_user(email="area@example.test", role="manager")
        cookie = self.login("area@example.test", roles.WORKER_PASSWORD)
        self.assert_error(403, "POST", PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 0}, cookie=cookie)
        self.assertEqual(self.service.requests, [])
        self.service.reply_value = {"person": {"id": 7, "display_name": "Persona inattiva", "active": False}}
        self.assert_error(422, "POST", PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 0}, cookie=self.manager_cookie)
        self.service.reply_value = {"person": {"id": 8, "display_name": "Persona diversa", "active": True}}
        self.assert_error(422, "POST", PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 0}, cookie=self.manager_cookie)
        self.service.reply_status = 503
        self.assert_error(503, "POST", PREFIX + "inventory-person-accounts", {"person_id": 7, "user_id": self.worker_id, "revision": 0}, cookie=self.manager_cookie)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM ii_inventory_person_accounts"), [(0,)])

    def test_strict_request_guards_and_audit_atomicity(self):
        self.assert_error(403, "POST", PREFIX + "time-drafts", {"source": "manual"}, cookie=self.worker_cookie, headers={"Origin": "https://hostile.invalid"})
        self.assert_error(400, "POST", PREFIX + "budget", cookie=self.manager_cookie, body="job_id=1&job_id=2", headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assert_error(400, "POST", PREFIX + "time-drafts", {"source": "manual", "actor_id": self.manager_id}, cookie=self.worker_cookie)
        row = {"job_id": self.job_id, "date": "2026-10-10", "minutes": 1, "note": "x\0y"}
        for entries in (json.dumps([row]), json.dumps([{**row, "note": ""}]) + " trailing"):
            self.assert_error(422, "POST", PREFIX + "time-drafts", {"source": "manual", "entries": entries}, cookie=self.worker_cookie)
        self.sql("CREATE TRIGGER fail_draft_audit BEFORE INSERT ON audit_events WHEN NEW.action='time_drafts.save' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        self.assert_error(500, "POST", PREFIX + "time-drafts", {"source": "manual", "entries": json.dumps([{"job_id": self.job_id, "date": "2026-10-10", "minutes": 1, "note": ""}])}, cookie=self.worker_cookie)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM ii_time_drafts"), [(0,)])


if __name__ == "__main__":
    unittest.main()
