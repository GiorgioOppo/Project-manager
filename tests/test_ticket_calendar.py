"""Internal agenda and durable Google mirror, with isolated Ticket/Punto stores."""
from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import json
import sqlite3
import subprocess
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

import test_tickets as tickets
from test_ticket_integration import FakePunto, PREFIX


@contextmanager
def database(path):
    connection = sqlite3.connect(path)
    try:
        with connection:
            yield connection
    finally:
        connection.close()


START = 1791802800


class CalendarPunto(FakePunto):
    """Source queue persists immutable operations independently of Ticket responses."""
    def __init__(self):
        super().__init__()
        self.calendar_id = "collaboratore@example.test"
        self.connection_id = "a" * 64
        self.after_status = None
        self.calendar_connected = True
        self.changes = {}
        self.events = {}
        self.posts = []
        self.lose_ack = False
        self.fail_post = None
        self.change_state = "applied"
        self.before_reply = None
        fixture = self
        parent = self.server.RequestHandlerClass

        class Handler(parent):
            def authorize_calendar(self):
                authorization = self.headers.get("Authorization", "")
                grant = authorization.removeprefix("Bearer ")
                scope = fixture.grants.get(grant) if authorization.startswith("Bearer ") else None
                if scope is None:
                    self.reply(401, {"ok": False, "error": "Grant scaduto"})
                    return None
                if not scope.get("calendar_access", False):
                    self.reply(403, {"ok": False, "error": "Calendario non autorizzato"})
                    return None
                return scope

            def do_GET(self):
                parsed = urlsplit(self.path)
                resource = parsed.path.removeprefix(PREFIX)
                if not resource.startswith("calendar/"):
                    return super().do_GET()
                scope = self.authorize_calendar()
                if scope is None:
                    return
                query = parse_qs(parsed.query)
                fixture.requests.append({"resource": resource, "query": query})
                if resource in fixture.failures:
                    return self.reply(*fixture.failures[resource])
                if resource == "calendar/status":
                    if fixture.before_reply:
                        callback, fixture.before_reply = fixture.before_reply, None
                        callback()
                    status = {"connected": fixture.calendar_connected, "calendar_id": fixture.calendar_id,
                              "connection_id": fixture.connection_id if fixture.calendar_connected else None,
                              "reauth_required": False, "oauth_configured": True}
                    if fixture.after_status:
                        callback, fixture.after_status = fixture.after_status, None
                        callback()
                    return self.reply(200, status)
                if resource == "calendar/change":
                    row = fixture.changes.get(query.get("idempotency_key", [""])[0])
                    return self.reply(200, row) if row else self.reply(404, {"ok": False, "error": "Operazione non trovata"})
                if resource == "calendar/event":
                    row = fixture.events.get(query.get("event_id", [""])[0])
                    return self.reply(200, {"event": row, "calendar_id": fixture.calendar_id}) if row else self.reply(404, {"ok": False, "error": "Evento non trovato"})
                return self.reply(404, {"ok": False, "error": "Risorsa inesistente"})

            def do_POST(self):
                scope = self.authorize_calendar()
                if scope is None:
                    return
                resource = urlsplit(self.path).path.removeprefix(PREFIX)
                raw = self.rfile.read(int(self.headers["Content-Length"])).decode()
                form = {key: value[0] for key, value in parse_qs(raw, keep_blank_values=True).items()}
                fixture.posts.append((resource, deepcopy(form), raw))
                if fixture.fail_post:
                    return self.reply(fixture.fail_post, {"ok": False, "error": "Errore sintetico"})
                if form.get("expected_connection_id") != fixture.connection_id or form.get("expected_calendar_id") != fixture.calendar_id:
                    return self.reply(409, {"ok": False, "error": "Target calendario cambiato"})
                key = form["idempotency_key"]
                if key in fixture.changes:
                    return self.reply(200, fixture.changes[key])
                operation = resource.removeprefix("calendar/")
                event_id = hashlib.sha256(f"{scope['user']['id']}:{key}".encode()).hexdigest() if operation == "create" else form["event_id"]
                if operation in ("update", "delete") and (event_id not in fixture.events or form["etag"] != fixture.events[event_id]["etag"]):
                    return self.reply(409, {"ok": False, "error": "ETag cambiato"})
                change = {"id": len(fixture.changes) + 1, "idempotency_key": key,
                          "event_id": event_id, "state": fixture.change_state, "error": "",
                          "attempts": 1, "calendar_id": fixture.calendar_id}
                fixture.changes[key] = change
                if fixture.change_state == "applied":
                    if operation == "delete":
                        fixture.events.pop(event_id, None)
                    else:
                        event = json.loads(form["event"])
                        event.update({"id": event_id, "etag": f'"revision-{len(fixture.changes)}"',
                                      "htmlLink": "https://calendar.google.com/calendar/event?eid=" + event_id})
                        fixture.events[event_id] = event
                if fixture.lose_ack:
                    fixture.lose_ack = False
                    return self.reply(503, {"ok": False, "error": "Risposta persa dopo l'applicazione"})
                return self.reply(202 if fixture.change_state == "queued" else 200, change)

        self.server.RequestHandlerClass = Handler


class TicketCalendarTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    create_ticket = tickets.TicketApplicationTests.create_ticket
    update_ticket = tickets.TicketApplicationTests.update_ticket
    create_user = tickets.TicketApplicationTests.create_user
    stop = tickets.TicketApplicationTests.stop

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make", "-C", str(tickets.APP)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.binary = tickets.APP / "build" / "punto-tickets"

    def setUp(self):
        self.punto = CalendarPunto()
        self.addCleanup(self.punto.close)
        tickets.TicketApplicationTests.setUp(self)

    def start(self):
        self.environment["TICKETS_PUNTO_ORIGIN"] = self.punto.url
        tickets.TicketApplicationTests.start(self)

    def connect(self, session, *, calendar=True, owner=700, job_ids=(21,)):
        pending, _ = self.api("POST", "/api/integrations/punto/start", {"calendar": True} if calendar else {}, session)
        self.assertEqual(pending["url"].endswith("&calendar=1"), calendar)
        grant = self.punto.issue(pending["state"], job_ids=job_ids)
        self.punto.grants[grant]["calendar_access"] = calendar
        self.punto.grants[grant]["user"]["id"] = owner
        connected, _ = self.api("POST", "/api/integrations/punto/connect", {"state": pending["state"], "grant": grant}, session)
        self.assertEqual(connected["calendar_access"], calendar)
        self.assertEqual(connected["user_id"], owner)
        return grant

    def scheduled(self, session, **fields):
        return self.create_ticket(session, scheduled_start=START, scheduled_end=START + 3600,
                                  scheduled_timezone="Europe/Rome", **fields)

    def google(self, session, ticket, action=None, expected=200):
        return self.api("POST" if action else "GET", f"/api/tickets/{ticket['id']}/calendar",
                        {"action": action, "revision": ticket["revision"]} if action else None,
                        session, expected=expected)[0]

    def mirror(self, ticket):
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            db.row_factory = sqlite3.Row
            return dict(db.execute("SELECT * FROM ticket_calendar WHERE ticket_id=?", (ticket["id"],)).fetchone())

    def test_schedule_is_atomic_revisioned_and_strictly_validated(self):
        session = self.login()
        ticket = self.scheduled(session)
        self.assertEqual((ticket["scheduled_start"], ticket["scheduled_end"], ticket["scheduled_timezone"]), (START, START + 3600, "Europe/Rome"))
        invalid = [dict(scheduled_start=START), dict(scheduled_start=START, scheduled_end=None, scheduled_timezone="UTC"),
                   dict(scheduled_start=0, scheduled_end=1, scheduled_timezone="UTC"),
                   dict(scheduled_start=START, scheduled_end=START, scheduled_timezone="UTC"),
                   dict(scheduled_start=START, scheduled_end=START + 366 * 86400 + 1, scheduled_timezone="UTC"),
                   dict(scheduled_start=START + .5, scheduled_end=START + 1, scheduled_timezone="UTC"),
                   dict(scheduled_start=START, scheduled_end=START + 1, scheduled_timezone="../etc/passwd"),
                   dict(scheduled_start=START, scheduled_end=START + 1, scheduled_timezone="Europe/Invalid"),
                   dict(scheduled_start=7258118398, scheduled_end=7258118400, scheduled_timezone="UTC")]
        for fields in invalid:
            with self.subTest(fields=fields):
                self.api("PUT", f"/api/tickets/{ticket['id']}", {"revision": ticket["revision"], **fields}, session, expected=400)
        latest, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=session)
        self.assertEqual(latest["ticket"]["revision"], ticket["revision"])
        cleared = self.update_ticket(session, ticket, scheduled_start=None, scheduled_end=None, scheduled_timezone=None)
        self.assertIsNone(cleared["scheduled_start"])
        self.assertEqual(cleared["revision"], ticket["revision"] + 1)
        detail, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=session)
        self.assertIn("programmazione", detail["history"][0]["detail"])

    def test_agenda_overlap_boundaries_filters_and_pagination(self):
        session = self.login()
        self.connect(session)
        first = self.scheduled(session, title="Primo", job_id=21)
        second = self.scheduled(session, title="Secondo", priority="urgent")
        self.create_ticket(session, title="Non programmato")
        query = urlencode({"from": START + 3600, "to": START + 7200})
        empty, _ = self.api("GET", "/api/tickets/agenda?" + query, session=session)
        self.assertEqual(empty["total"], 0)
        query = urlencode({"from": START + 1, "to": START + 3600, "limit": 1})
        page, _ = self.api("GET", "/api/tickets/agenda?" + query, session=session)
        self.assertEqual(page["total"], 2)
        self.assertEqual(page["items"][0]["id"], first["id"])
        next_page, _ = self.api("GET", "/api/tickets/agenda?" + query + "&offset=1", session=session)
        self.assertEqual(next_page["items"][0]["id"], second["id"])
        filtered, _ = self.api("GET", "/api/tickets/agenda?" + query + "&priority=urgent", session=session)
        self.assertEqual(filtered["total"], 1)
        for suffix in ("", "from=1", "from=1&to=1", f"from=1&to={93*86400+2}",
                       "from=1&to=2&from=1", "from=1&to=2&unknown=x", "from=1&to=2&&limit=1", "from=1&to=2&limit=101"):
            with self.subTest(suffix=suffix):
                self.api("GET", "/api/tickets/agenda?" + suffix, session=session, expected=400)

    def test_live_commessa_acl_hides_agenda_and_calendar(self):
        session = self.login()
        grant = self.connect(session)
        linked = self.scheduled(session, job_id=21)
        self.scheduled(session, title="Agenda interna autonoma")
        self.punto.grants[grant]["job_ids"] = []
        agenda, _ = self.api("GET", f"/api/tickets/agenda?from={START}&to={START+7200}", session=session)
        self.assertEqual(agenda["total"], 1)
        self.google(session, linked, expected=404)
        self.google(session, linked, "sync", expected=404)
        self.assertFalse(self.punto.posts)

    def test_calendar_capability_cannot_silently_upgrade_readonly_grant(self):
        session = self.login()
        self.connect(session, calendar=False)
        ticket = self.scheduled(session)
        status = self.google(session, ticket)
        self.assertFalse(status["calendar_access"])
        self.assertFalse(status["can_sync"])
        self.google(session, ticket, "sync", expected=409)
        pending, _ = self.api("POST", "/api/integrations/punto/start", {}, session)
        grant = self.punto.issue(pending["state"])
        self.punto.grants[grant]["calendar_access"] = True
        self.api("POST", "/api/integrations/punto/connect", {"state": pending["state"], "grant": grant}, session, expected=403)

    def test_explicit_sync_update_remove_and_no_duplicate_for_unchanged_ticket(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session, job_id=21)
        status = self.google(session, ticket, "sync")
        self.assertEqual(status["state"], "applied")
        self.assertTrue(status["event_link"].startswith("https://calendar.google.com/"))
        self.assertFalse(status["remote_diff"])
        event_id = status["event_id"]
        self.assertEqual(self.punto.posts[0][1]["job_id"], "21")
        self.google(session, ticket, "sync")
        self.assertEqual(len(self.punto.posts), 1)
        ticket = self.update_ticket(session, ticket, title="Programmazione aggiornata")
        self.assertTrue(self.google(session, ticket)["remote_diff"])
        self.assertEqual(len(self.punto.posts), 1)
        status = self.google(session, ticket, "sync")
        self.assertEqual(status["event_id"], event_id)
        self.assertFalse(status["remote_diff"])
        self.assertEqual(self.punto.posts[-1][0], "calendar/update")
        removed = self.google(session, ticket, "remove")
        self.assertEqual(removed["state"], "removed")
        self.assertIsNone(removed["event_link"])
        self.assertIsNone(removed["remote_event"])
        detail, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=session)
        self.assertEqual(detail["ticket"]["scheduled_start"], START)
        self.assertEqual(len(self.punto.events), 0)
        self.google(session, ticket, "remove")
        self.assertEqual(len(self.punto.posts), 3)

    def test_lost_ack_is_reconciled_by_exact_key_without_second_create(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.punto.lose_ack = True
        status = self.google(session, ticket, "sync", expected=202)
        self.assertEqual(status["state"], "unknown")
        saved = self.mirror(ticket)
        self.assertEqual(saved["event_id"], next(iter(self.punto.events)))
        self.assertIn(saved["key"], self.punto.changes)
        status = self.google(session, ticket)
        self.assertEqual(status["state"], "applied")
        self.google(session, ticket, "sync")
        self.assertEqual(len(self.punto.posts), 1)
        self.assertEqual(len(self.punto.events), 1)
        self.assertEqual(self.mirror(ticket)["payload"], saved["payload"])

    def test_unknown_intent_survives_restart_and_missing_ledger_never_recreates(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.punto.fail_post = 503
        self.google(session, ticket, "sync", expected=202)
        saved = self.mirror(ticket)
        self.punto.fail_post = None
        self.punto.connection_id = "b" * 64
        self.stop()
        self.start()
        session = self.login()
        self.connect(session)
        status = self.google(session, ticket, expected=202)
        self.assertEqual(status["state"], "stale")
        self.assertFalse(status["can_retry"])
        self.google(session, ticket, "sync", expected=409)
        self.assertEqual(len(self.punto.posts), 1)
        self.assertEqual(self.mirror(ticket)["key"], saved["key"])
        self.assertEqual(self.mirror(ticket)["payload"], saved["payload"])
        self.assertEqual(self.punto.events, {})

    def test_queued_operation_preserves_same_intent_until_source_applies(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.punto.change_state = "queued"
        first = self.google(session, ticket, "sync", expected=202)
        self.assertEqual(first["state"], "queued")
        saved = self.mirror(ticket)
        ticket = self.update_ticket(session, ticket, title="Modifica mentre Google lavora")
        second = self.google(session, ticket, "sync", expected=202)
        self.assertEqual(second["state"], "queued")
        self.assertEqual(len(self.punto.posts), 1)
        self.assertEqual(self.mirror(ticket)["payload"], saved["payload"])
        self.google(session, ticket, "remove", expected=409)

    def test_other_owner_or_disconnected_session_cannot_read_private_remote_data(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        status = self.google(session, ticket, "sync")
        self.punto.events[status["event_id"]]["location"] = "INDIRIZZO PRIVATO"
        self.punto.events[status["event_id"]]["description"] = "APPUNTAMENTO RISERVATO"
        self.assertIn("INDIRIZZO PRIVATO", json.dumps(self.google(session, ticket)))
        other = self.login()
        self.connect(other, owner=701)
        status = self.google(other, ticket)
        self.assertTrue(status["owner_mismatch"])
        for key in ("calendar_id", "event_id", "event_link", "remote_event"):
            self.assertIsNone(status[key], key)
        self.assertFalse(status["remote_diff"])
        self.assertNotIn("INDIRIZZO PRIVATO", json.dumps(status))
        self.assertNotIn("APPUNTAMENTO RISERVATO", json.dumps(status))
        self.google(other, ticket, "sync", expected=409)
        self.api("POST", "/api/integrations/punto/disconnect", {}, session)
        status = self.google(session, ticket)
        self.assertIsNone(status["remote_event"])
        self.assertIsNone(status["event_link"])
        self.assertIsNone(status["error"])

    def test_calendar_selection_change_does_not_reassociate_or_create(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.google(session, ticket, "sync")
        self.punto.calendar_id = "another@example.test"
        status = self.google(session, ticket)
        self.assertEqual(status["state"], "stale")
        self.assertIsNone(status["remote_event"])
        self.google(session, ticket, "sync", expected=409)
        self.assertEqual(len(self.punto.posts), 1)

    def test_provider_validation_and_etag_conflict_are_terminal_not_unknown(self):
        session = self.login()
        self.connect(session)
        for code, state in ((400, "failed"), (422, "failed"), (409, "conflict")):
            with self.subTest(code=code):
                ticket = self.scheduled(session)
                self.punto.fail_post = code
                status = self.google(session, ticket, "sync")
                # A definitive rejection must not become an endlessly pending intent.
                self.assertFalse(status["pending"])
                self.assertEqual(status["state"], state)
                self.assertTrue(self.mirror(ticket)["payload"])
        self.punto.fail_post = None

    def test_equivalent_google_offset_and_fraction_are_not_false_differences(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        status = self.google(session, ticket, "sync")
        event = self.punto.events[status["event_id"]]
        for key in ("start", "end"):
            value = datetime.fromisoformat(event[key]["dateTime"].replace("Z", "+00:00"))
            event[key]["dateTime"] = value.astimezone(timezone(timedelta(hours=2))).isoformat(timespec="milliseconds")
        refreshed = self.google(session, ticket, "refresh")
        self.assertFalse(refreshed["remote_diff"])
        self.assertEqual(len(self.punto.posts), 1)

    def test_ticket_revision_is_checked_before_intent_and_again_under_write_lock(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        stale = {**ticket, "revision": ticket["revision"] + 1}
        self.google(session, stale, "sync", expected=409)
        self.assertFalse(self.punto.posts)
        def concurrent_edit():
            with database(self.environment["TICKETS_DB_PATH"]) as db:
                db.execute("UPDATE tickets SET revision=revision+1,title='Cambio concorrente' WHERE id=?", (ticket["id"],))
        self.punto.before_reply = concurrent_edit
        self.google(session, ticket, "sync", expected=409)
        self.assertFalse(self.punto.posts)
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM ticket_calendar").fetchone()[0], 0)

    def test_competing_calendar_intent_is_not_overwritten(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        def concurrent_intent():
            with database(self.environment["TICKETS_DB_PATH"]) as db:
                db.execute("INSERT INTO ticket_calendar(ticket_id,owner_id,calendar_id,state,operation,key,payload,revision) VALUES(?,700,?,'unknown','create','competing-key','immutable-payload',?)",
                           (ticket["id"], self.punto.calendar_id, ticket["revision"]))
        self.punto.before_reply = concurrent_intent
        self.google(session, ticket, "sync", expected=409)
        self.assertFalse(self.punto.posts)
        self.assertEqual(self.mirror(ticket)["key"], "competing-key")
        self.assertEqual(self.mirror(ticket)["payload"], "immutable-payload")

    def test_missing_ledger_finds_existing_event_without_automatic_replay(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.punto.lose_ack = True
        self.google(session, ticket, "sync", expected=202)
        event_id = next(iter(self.punto.events))
        self.punto.changes.clear()
        self.punto.connection_id = "b" * 64
        status = self.google(session, ticket, expected=202)
        self.assertEqual(status["state"], "stale")
        self.assertEqual(status["event_id"], event_id)
        self.assertEqual(len(self.punto.posts), 1)
        status = self.google(session, ticket, "refresh")
        self.assertEqual(status["state"], "conflict")
        self.google(session, ticket, "sync")
        self.assertEqual(self.punto.posts[-1][0], "calendar/update")
        self.assertEqual(len(self.punto.events), 1)
        self.assertEqual(next(iter(self.punto.events)), event_id)

    def test_same_lifecycle_outage_retries_exact_immutable_intent_once(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        self.punto.fail_post = 503
        self.google(session, ticket, "sync", expected=202)
        saved = self.mirror(ticket)
        self.punto.fail_post = None
        self.stop()
        self.start()
        session = self.login()
        self.connect(session)
        status = self.google(session, ticket, expected=202)
        self.assertEqual(status["state"], "unknown")
        self.assertTrue(status["can_retry"])
        self.assertEqual(status["retry_action"], "sync")
        self.assertEqual(len(self.punto.posts), 1)
        status = self.google(session, ticket, "sync")
        self.assertEqual(status["state"], "applied")
        self.assertEqual(len(self.punto.posts), 2)
        self.assertEqual(self.punto.posts[0][2], self.punto.posts[1][2])
        self.assertEqual(self.mirror(ticket)["key"], saved["key"])
        self.assertEqual(len(self.punto.events), 1)

    def test_target_change_between_status_and_post_does_not_enqueue(self):
        session = self.login()
        self.connect(session)
        for field, value in (("connection_id", "b" * 64), ("calendar_id", "new@example.test")):
            with self.subTest(field=field):
                ticket = self.scheduled(session)
                self.punto.after_status = lambda field=field, value=value: setattr(self.punto, field, value)
                status = self.google(session, ticket, "sync")
                self.assertEqual(status["state"], "conflict")
                self.assertEqual(self.punto.events, {})
                self.assertEqual(self.punto.changes, {})
                self.punto.connection_id = "a" * 64
                self.punto.calendar_id = "collaboratore@example.test"

    def test_google_cancelled_event_never_causes_automatic_recreation(self):
        session = self.login()
        self.connect(session)
        ticket = self.scheduled(session)
        status = self.google(session, ticket, "sync")
        self.punto.events[status["event_id"]]["status"] = "cancelled"
        status = self.google(session, ticket, "refresh")
        self.assertEqual(status["state"], "missing")
        self.assertFalse(status["can_sync"])
        self.assertIsNone(status["remote_event"])
        self.google(session, ticket, "sync", expected=409)
        self.assertEqual(len(self.punto.posts), 1)

    def test_long_multilingual_summary_respects_provider_utf8_byte_limit(self):
        session = self.login()
        self.connect(session)
        self.punto.jobs[21]["code"] = "😀" * 200
        ticket = self.scheduled(session, job_id=21, title="😀" * 160)
        self.google(session, ticket, "sync")
        summary = json.loads(self.punto.posts[-1][1]["event"])["summary"]
        self.assertLessEqual(len(summary.encode("utf-8")), 1000)
        self.assertTrue(summary.startswith("Ticket #"))
        self.assertTrue(summary.endswith("…"))
        self.assertFalse(self.google(session, ticket)["remote_diff"])

    def test_schema_two_migration_preserves_existing_ticket_and_nullable_schedule(self):
        session = self.login()
        self.connect(session)
        ticket = self.create_ticket(session, job_id=21, asset_id=101)
        self.stop()
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("DROP INDEX tickets_schedule_idx")
            db.execute("DROP TABLE ticket_calendar")
            for column in ("scheduled_start", "scheduled_end", "scheduled_timezone"):
                db.execute("ALTER TABLE tickets DROP COLUMN " + column)
            db.execute("PRAGMA user_version=2")
        self.start()
        session = self.login()
        self.connect(session)
        detail, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=session)
        migrated = detail["ticket"]
        self.assertEqual(migrated["title"], ticket["title"])
        self.assertEqual(migrated["job_id"], 21)
        self.assertEqual(migrated["asset_id"], 101)
        self.assertEqual(len(detail["history"]), 1)
        for key in ("scheduled_start", "scheduled_end", "scheduled_timezone"):
            self.assertIsNone(migrated[key])
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 3)


if __name__ == "__main__":
    unittest.main()
