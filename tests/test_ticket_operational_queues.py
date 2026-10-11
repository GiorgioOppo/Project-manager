"""Operational views use isolated stores, synthetic mail, and live Punto ACLs."""
from datetime import datetime, timedelta, timezone
import time
import unittest
from urllib.parse import urlencode
from zoneinfo import ZoneInfo

import test_tickets as tickets
import test_ticket_raw as raw
import test_ticket_operations as operations
from test_mail_client import MAIL_ENV


class TicketOperationalQueueTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    create_ticket = tickets.TicketApplicationTests.create_ticket
    update_ticket = tickets.TicketApplicationTests.update_ticket
    create_user = tickets.TicketApplicationTests.create_user
    stop = tickets.TicketApplicationTests.stop
    connect = raw.TicketRawTests.connect
    mailbox = raw.TicketRawTests.mailbox
    authorize = raw.TicketRawTests.authorize
    seed = raw.TicketRawTests.seed
    workflow = raw.TicketRawTests.workflow
    claim = raw.TicketRawTests.claim
    convert = raw.TicketRawTests.convert

    @classmethod
    def setUpClass(cls):
        operations.TicketOperationsTests.setUpClass.__func__(cls)

    def setUp(self):
        raw.TicketRawTests.setUp(self)

    def start(self):
        # These tests never configure or invoke a real delivery service.
        for name in MAIL_ENV:
            self.environment.pop(name, None)
        raw.TicketRawTests.start(self)

    def listing(self, queue="all", *, session=None, **filters):
        return self.api("GET", "/api/tickets?" + urlencode({"operations_queue": queue, **filters}),
                        session=session or self.admin)[0]

    def ids(self, queue="all", **filters):
        return {item["id"] for item in self.listing(queue, **filters)["items"]}

    def draft(self, ticket):
        return self.api("POST", f"/api/tickets/{ticket['id']}/messages", {
            "to_email": "sender@example.test", "subject": "Risposta da verificare",
            "body": "Bozza sintetica, nessun invio reale.",
        }, self.admin, expected=201)[0]["message"]

    def test_needs_reply_uses_received_mail_and_actual_sent_time_not_drafts_or_notes(self):
        ticket = self.create_ticket(self.admin)
        message = self.draft(ticket)
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nota interna"},
                 self.admin, expected=201)
        self.assertEqual(self.ids("needs_reply"), {ticket["id"]})
        sent_at = int(time.time()) - 600
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            # Historical deliveries can predate the first-response SLA hook.
            db.execute("UPDATE ticket_messages SET state='sent',sent_at=? WHERE id=?", (sent_at, message["id"]))
        self.assertEqual(self.ids("needs_reply"), set())

        incoming = self.claim(self.seed(files=0))
        self.workflow(incoming, "link", ticket_id=ticket["id"], attachment_ids=[])
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_messages SET received_at=? WHERE id=?", (sent_at + 60, incoming["id"]))
        self.assertEqual(self.ids("needs_reply"), {ticket["id"]})
        later_draft = self.draft(ticket)
        for state in ("draft", "failed", "unknown"):
            with self.subTest(state=state):
                with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
                    db.execute("UPDATE ticket_messages SET state=?,sent_at=? WHERE id=?",
                               (state, sent_at + 120, later_draft["id"]))
                self.assertEqual(self.ids("needs_reply"), {ticket["id"]})
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE ticket_messages SET state='sent' WHERE id=?", (later_draft["id"],))
        self.assertEqual(self.ids("needs_reply"), set())
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_messages SET received_at=? WHERE id=?", (sent_at + 121, incoming["id"]))
        self.assertEqual(self.ids("needs_reply"), {ticket["id"]})
        ticket = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]
        self.update_ticket(self.admin, ticket, status="resolved")
        self.assertEqual(self.ids("needs_reply"), set())

    def test_inaccessible_incoming_mail_does_not_change_visible_ticket_reply_queue(self):
        grant = self.connect(self.admin, all_jobs=True)
        ticket = self.create_ticket(self.admin, title="Ticket locale condiviso")
        message = self.draft(ticket)
        sent_at = int(time.time()) - 600
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE ticket_messages SET state='sent',sent_at=? WHERE id=?", (sent_at, message["id"]))
        incoming = self.claim(self.seed(files=0))
        self.workflow(incoming, "link", ticket_id=ticket["id"], attachment_ids=[])
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_messages SET received_at=? WHERE id=?", (sent_at + 60, incoming["id"]))
        self.assertEqual(self.ids("needs_reply"), {ticket["id"]})
        self.api("PUT", f"/api/raw/mailboxes/{self.box['id']}", {
            "revision": self.box["revision"], "job_id": 21,
        }, self.admin)
        self.punto.grants[grant].update(all_jobs=False, job_ids=[22])
        self.assertEqual(self.ids("all"), {ticket["id"]})
        self.assertEqual(self.ids("needs_reply"), set())
        self.api("GET", f"/api/tickets/{ticket['id']}/conversation", session=self.admin, expected=404)
        self.punto.grants[grant]["job_ids"] = [21]
        self.assertEqual(self.ids("needs_reply"), {ticket["id"]})

    def test_private_raw_sources_hide_all_sla_derivatives_without_changing_history(self):
        grant = self.connect(self.admin, all_jobs=True)
        active = self.create_ticket(self.admin, title="Ticket locale attivo", group_name="Helpdesk")
        incoming = self.claim(self.seed(files=0))
        self.workflow(incoming, "link", ticket_id=active["id"], attachment_ids=[])
        converted = self.convert(self.claim(self.seed(files=0)))["ticket"]
        self.assertIsNotNone(converted["sla"])
        completed = self.update_ticket(self.admin, converted, status="resolved")
        midday = int(datetime.now(ZoneInfo("Europe/Rome")).replace(hour=12, minute=0, second=0, microsecond=0).timestamp())
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_messages SET received_at=?", (midday - 3600,))
            for ticket, first, resolved, reopened in ((active, None, None, 3), (completed, midday + 1800, midday + 3600, 4)):
                db.execute("UPDATE ticket_operations SET started_at=?,sla_timezone='Europe/Rome',sla_weekdays=127,"
                           "sla_day_start=0,sla_day_end=1439,response_budget=1,resolution_budget=2,"
                           "first_response_at=?,resolved_at=?,reopened_count=? WHERE ticket_id=?",
                           (midday, first, resolved, reopened, ticket["id"]))
        self.api("PUT", f"/api/raw/mailboxes/{self.box['id']}", {
            "revision": self.box["revision"], "job_id": 21,
        }, self.admin)

        def detail(ticket):
            return self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]

        visible = {ticket["id"]: detail(ticket)["sla"] for ticket in (active, completed)}
        self.assertEqual(visible[active["id"]]["raw_wait_minutes"], 60)
        report = self.api("GET", "/api/reports/operations", session=self.admin)[0]
        self.assertEqual((report["responded_count"], report["resolved_count"]), (1, 1))
        self.assertEqual(report["counts"]["reopened"], 7)
        self.assertEqual(report["counts"]["response_due_today"], 1)
        self.assertEqual(report["counts"]["resolution_due_today"], 1)
        self.assertEqual((report["average_response_minutes"], report["average_resolution_minutes"],
                          report["average_raw_wait_minutes"]), (90.0, 120.0, 60.0))
        self.assertEqual(self.ids("sla_today"), {active["id"]})
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            stored = db.execute("SELECT * FROM ticket_operations ORDER BY ticket_id").fetchall()
            revisions = db.execute("SELECT id,revision FROM tickets ORDER BY id").fetchall()

        self.punto.grants[grant].update(all_jobs=False, job_ids=[22])
        for ticket in (active, completed):
            self.assertIsNone(detail(ticket)["sla"])
        listing = self.listing()
        self.assertEqual(listing["total"], 2)
        self.assertTrue(all(ticket["sla"] is None for ticket in listing["items"]))
        self.assertEqual(self.ids("sla_today"), set())
        report = self.api("GET", "/api/reports/operations", session=self.admin)[0]
        self.assertEqual(report["counts"]["active"], 1)
        self.assertEqual((report["responded_count"], report["resolved_count"]), (0, 0))
        for key in ("response_breached", "resolution_breached", "response_due_today", "resolution_due_today", "reopened"):
            self.assertEqual(report["counts"][key], 0, key)
        for key in ("average_response_minutes", "average_resolution_minutes", "average_raw_wait_minutes"):
            self.assertIsNone(report[key], key)
        self.assertEqual(report["group_workload"][0]["active"], 1)
        self.assertEqual(report["group_workload"][0]["overdue"], 0)
        self.assertEqual(report["workload"][0]["assigned"], 1)
        self.assertEqual(report["workload"][0]["overdue"], 0)
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT * FROM ticket_operations ORDER BY ticket_id").fetchall(), stored)
            self.assertEqual(db.execute("SELECT id,revision FROM tickets ORDER BY id").fetchall(), revisions)

        self.punto.grants[grant]["job_ids"] = [21]
        for ticket in (active, completed):
            self.assertEqual(detail(ticket)["sla"], visible[ticket["id"]])
        restored = self.api("GET", "/api/reports/operations", session=self.admin)[0]
        self.assertEqual(restored["average_raw_wait_minutes"], 60.0)
        self.assertEqual(restored["responded_count"], 1)

    def test_sla_projection_never_inherits_previous_actor_scope_or_changes_local_notes(self):
        self.connect(self.admin, all_jobs=True)
        ticket = self.create_ticket(self.admin, title="Ticket locale con sorgente riservata")
        incoming = self.claim(self.seed(files=0))
        self.workflow(incoming, "link", ticket_id=ticket["id"], attachment_ids=[])
        self.api("PUT", f"/api/raw/mailboxes/{self.box['id']}", {
            "revision": self.box["revision"], "job_id": 21,
        }, self.admin)
        operator = self.login(self.create_user(self.admin)["email"])
        admin_detail = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]
        self.assertIsNotNone(admin_detail["sla"])
        operator_detail = self.api("GET", f"/api/tickets/{ticket['id']}", session=operator)[0]["ticket"]
        self.assertIsNone(operator_detail["sla"])
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nota interna ancora disponibile"},
                 operator, expected=201)
        self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)
        updated = self.update_ticket(operator, operator_detail, title="Modifica locale consentita")
        self.assertIsNone(updated["sla"])
        admin_detail = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]
        self.assertIsNotNone(admin_detail["ticket"]["sla"])
        self.assertEqual(admin_detail["comments"][0]["body"], "Nota interna ancora disponibile")

        self.punto.failures["scope"] = (503, {"ok": False, "error": "Synthetic outage"})
        self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin, expected=503)
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nota durante indisponibilità Punto"},
                 self.admin, expected=201)
        unavailable = self.update_ticket(self.admin, admin_detail["ticket"], title="Aggiornamento locale durante indisponibilità")
        self.assertIsNone(unavailable["sla"])
        self.api("POST", "/api/tickets/bulk/preview", {
            "tickets": [{"id": ticket["id"], "revision": unavailable["revision"]}], "changes": {"category": "Locale"},
        }, self.admin, expected=503)
        del self.punto.failures["scope"]
        recovered = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]
        self.assertIsNotNone(recovered["sla"])
        self.assertEqual(recovered["title"], "Aggiornamento locale durante indisponibilità")

    def test_waiting_follow_up_and_unscheduled_queues_exclude_completed_tickets(self):
        now = int(time.time())
        customer = self.update_ticket(self.admin, self.create_ticket(self.admin), status="waiting",
                                      waiting_reason="customer", follow_up_at=now - 60)
        supplier = self.update_ticket(self.admin, self.create_ticket(self.admin), status="waiting",
                                      waiting_reason="supplier", follow_up_at=now - 60)
        future = self.update_ticket(self.admin, self.create_ticket(self.admin), status="waiting",
                                    waiting_reason="customer", follow_up_at=now + 86400)
        open_ticket = self.create_ticket(self.admin)
        scheduled = self.create_ticket(self.admin, scheduled_start=1800000000, scheduled_end=1800003600,
                                        scheduled_timezone="Europe/Rome")
        completed = []
        for status in ("resolved", "closed"):
            ticket = self.create_ticket(self.admin)
            completed.append(self.update_ticket(self.admin, ticket, status=status))
        self.assertEqual(self.ids("waiting_customer"), {customer["id"], future["id"]})
        self.assertEqual(self.ids("follow_up_due"), {customer["id"], supplier["id"]})
        self.assertEqual(self.ids("unscheduled"), {customer["id"], supplier["id"], future["id"], open_ticket["id"]})
        self.assertNotIn(scheduled["id"], self.ids("unscheduled"))
        self.assertTrue({ticket["id"] for ticket in completed}.issubset(self.ids("all")))

    def test_sla_today_uses_policy_day_and_pending_response_or_active_resolution(self):
        zone = ZoneInfo("Europe/Rome")
        before = datetime.now(timezone.utc)
        local_day = before.astimezone(zone).date()
        # Select a same-local-day deadline whose UTC date differs from "now".
        hour, minute = (0, 30) if before.date() == local_day else (23, 30)
        deadline = datetime.combine(local_day, datetime.min.time(), zone).replace(hour=hour, minute=minute)
        self.assertNotEqual(deadline.astimezone(timezone.utc).date(), before.date())
        start = int((deadline - timedelta(minutes=30)).timestamp())
        response = self.create_ticket(self.admin)
        answered = self.create_ticket(self.admin)
        resolution = self.create_ticket(self.admin)
        completed = self.update_ticket(self.admin, self.create_ticket(self.admin), status="resolved")
        later = self.create_ticket(self.admin)
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            for ticket, response_budget, resolution_budget, first in (
                (response, 30, 2880, None), (answered, 30, 2880, start),
                (resolution, 2880, 30, start), (completed, 30, 30, None), (later, 2880, 2880, None),
            ):
                db.execute("UPDATE ticket_operations SET started_at=?,sla_timezone='Europe/Rome',"
                           "sla_weekdays=127,sla_day_start=0,sla_day_end=1439,response_budget=?,"
                           "resolution_budget=?,first_response_at=? WHERE ticket_id=?",
                           (start, response_budget, resolution_budget, first, ticket["id"]))
        listing = self.listing("sla_today", today="1900-01-01")
        after = datetime.now(timezone.utc)
        expected = {response["id"], resolution["id"]}
        actual = {item["id"] for item in listing["items"]}
        # A midnight between the two observations legitimately moves the queue.
        self.assertIn(actual, [expected, set()] if after.astimezone(zone).date() != local_day else [expected])
        self.assertEqual(listing["reference_date"], "1900-01-01")
        for item in listing["items"]:
            relevant = "response_due_at" if item["id"] == response["id"] else "resolution_due_at"
            self.assertEqual(item["sla"][relevant], int(deadline.timestamp()))

    def test_queue_filters_before_count_paging_and_board_with_live_job_acl(self):
        grant = self.connect(self.admin, all_jobs=True)
        groups = {}
        for status in ("open", "in_progress", "waiting"):
            groups[status] = []
            for index in range(2):
                ticket = self.create_ticket(self.admin, title=f"Queue {status} {index}", priority="high", job_id=21)
                if status != "open":
                    fields = {"waiting_reason": "customer", "follow_up_at": 2000000000} if status == "waiting" else {}
                    ticket = self.update_ticket(self.admin, ticket, status=status, **fields)
                groups[status].append(ticket)
        hidden = self.create_ticket(self.admin, title="Queue segreto", priority="high", job_id=22)
        wrong_priority = self.create_ticket(self.admin, title="Queue bassa", priority="low", job_id=21)
        wrong_search = self.create_ticket(self.admin, title="Titolo diverso", priority="high", job_id=21)
        resolved = self.update_ticket(self.admin, self.create_ticket(self.admin, title="Queue risolto", priority="high", job_id=21), status="resolved")
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE tickets SET updated_at='2026-01-01T12:00:00Z'")
        self.punto.grants[grant].update(all_jobs=False, job_ids=[21])
        filters = {"search": "Queue", "priority": "high", "limit": 1}
        for status, group in groups.items():
            expected_ids = [ticket["id"] for ticket in reversed(group)]
            for offset in range(3):
                page = self.listing("needs_reply", status=status, offset=offset, **filters)
                self.assertEqual(page["total"], 2)
                self.assertEqual([item["id"] for item in page["items"]], expected_ids[offset:offset + 1])
                self.assertEqual(page["stats"], {"open": 4, "in_progress": 2, "waiting": 2, "resolved": 1, "closed": 0})
                self.assertEqual(page["queues"]["active"], 8)
        self.assertEqual(self.listing("needs_reply", **filters)["total"], 6)
        self.assertNotIn(hidden["id"], self.ids("all"))
        self.assertTrue({wrong_priority["id"], wrong_search["id"], resolved["id"]}.issubset(self.ids("all")))
        self.punto.grants[grant]["job_ids"] = [22]
        self.assertEqual(self.ids("needs_reply"), {hidden["id"]})
        self.api("POST", "/api/integrations/punto/disconnect", {}, self.admin)
        self.assertEqual(self.listing("needs_reply")["total"], 0)

    def test_agenda_applies_operational_queue_before_offset_and_acl(self):
        grant = self.connect(self.admin, all_jobs=True)
        start = 1800000000
        waiting = []
        for index in range(3):
            ticket = self.create_ticket(self.admin, title=f"Agenda {index}", job_id=21,
                                        scheduled_start=start + index * 60, scheduled_end=start + 3600,
                                        scheduled_timezone="Europe/Rome")
            waiting.append(self.update_ticket(self.admin, ticket, status="waiting", waiting_reason="customer", follow_up_at=2000000000))
        self.create_ticket(self.admin, title="Agenda senza attesa", job_id=21, scheduled_start=start,
                           scheduled_end=start + 3600, scheduled_timezone="Europe/Rome")
        private = self.create_ticket(self.admin, job_id=22, scheduled_start=start, scheduled_end=start + 3600,
                                     scheduled_timezone="Europe/Rome")
        self.update_ticket(self.admin, private, status="waiting", waiting_reason="customer", follow_up_at=2000000000)
        self.punto.grants[grant].update(all_jobs=False, job_ids=[21])
        for offset in range(4):
            query = urlencode({"from": start, "to": start + 3600, "operations_queue": "waiting_customer", "limit": 1, "offset": offset})
            page = self.api("GET", "/api/tickets/agenda?" + query, session=self.admin)[0]
            self.assertEqual(page["total"], 3)
            self.assertEqual([item["id"] for item in page["items"]], [item["id"] for item in waiting[offset:offset + 1]])
        query = urlencode({"from": start, "to": start + 3600, "operations_queue": "unscheduled"})
        self.assertEqual(self.api("GET", "/api/tickets/agenda?" + query, session=self.admin)[0]["total"], 0)

    def test_saved_view_round_trips_queue_and_invalid_or_duplicate_values_fail(self):
        wanted = self.create_ticket(self.admin, title="Queue salvata", priority="high")
        self.create_ticket(self.admin, title="Queue esclusa", priority="low")
        filters = {"operations_queue": "needs_reply", "priority": "high", "search": "Queue", "sort": "priority"}
        view = self.api("POST", "/api/operations/views", {"name": "Risposte urgenti", "view": "board", "filters": filters},
                        self.admin, expected=201)[0]["view"]
        operator = self.login(self.create_user(self.admin)["email"])
        listed = self.api("GET", "/api/operations/views", session=operator)[0]["items"]
        saved = next(item for item in listed if item["id"] == view["id"])
        self.assertEqual(saved["filters"], filters)
        selected = self.api("GET", "/api/tickets?" + urlencode(saved["filters"]), session=operator)[0]
        self.assertEqual(selected["total"], 1)
        self.assertEqual(selected["items"][0]["id"], wanted["id"])
        changed = self.api("PUT", f"/api/operations/views/{view['id']}", {
            "revision": view["revision"], "view": "operator",
        }, self.admin)[0]["view"]
        self.assertEqual(changed["view"], "operator")
        self.assertEqual(changed["filters"], filters)
        self.stop()
        self.start()
        self.admin = self.login()
        restored = self.api("GET", "/api/operations/views", session=self.admin)[0]["items"]
        saved = next(item for item in restored if item["id"] == view["id"])
        self.assertEqual(saved["view"], "operator")
        self.assertEqual(saved["filters"], filters)
        for value in ("", "unknown", "NEEDS_REPLY"):
            with self.subTest(value=value):
                self.api("POST", "/api/operations/views", {"name": "Non valida", "filters": {"operations_queue": value}},
                         self.admin, expected=400)
        for suffix in ("operations_queue=", "operations_queue=unknown", "operations_queue=all&operations_queue=all"):
            for path in ("/api/tickets?" + suffix, "/api/tickets/agenda?from=1800000000&to=1800003600&" + suffix):
                with self.subTest(path=path):
                    self.api("GET", path, session=self.admin, expected=400)


if __name__ == "__main__":
    unittest.main()
