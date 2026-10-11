"""Operator correspondence and confirmed work logs; HTTPS fixtures never send email."""
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path
import json
import sqlite3
import tempfile
import time
import unittest
from urllib.parse import parse_qs, urlsplit

import test_tickets as tickets
import test_ticket_raw as raw
from test_ticket_integration import FakePunto, PREFIX
from test_mail_client import FakeMailAPI, create_certificate, TOKEN, MAIL_ENV


class TimePunto(FakePunto):
    def __init__(self):
        super().__init__()
        self.entries = {}
        self.posts = []
        self.lose_ack = False
        self.fail_before = False
        self.bad_lookup = False
        fixture = self
        parent = self.server.RequestHandlerClass

        class Handler(parent):
            def authorize_time(self):
                authorization = self.headers.get("Authorization", "")
                scope = fixture.grants.get(authorization.removeprefix("Bearer ")) if authorization.startswith("Bearer ") else None
                if scope is None or scope["expires_at"] <= int(time.time()):
                    self.reply(401, {"error": "Grant scaduto"})
                    return None
                if not scope.get("time_access"):
                    self.reply(403, {"error": "Ore non autorizzate"})
                    return None
                return scope

            def do_GET(self):
                resource = urlsplit(self.path).path.removeprefix(PREFIX)
                if not resource.startswith("time/"):
                    return super().do_GET()
                scope = self.authorize_time()
                if scope is None:
                    return
                query = parse_qs(urlsplit(self.path).query)
                fixture.requests.append({"resource": resource, "query": query})
                if resource in fixture.failures:
                    return self.reply(*fixture.failures[resource])
                if resource == "time/status":
                    return self.reply(200, {"enabled": True, "user_id": scope["user"]["id"], "actor_name": scope["user"]["name"]})
                job = int(query.get("job_id", ["0"])[0])
                if not scope["all_jobs"] and job not in scope["job_ids"]:
                    return self.reply(403, {"error": "Commessa non autorizzata"})
                entry = fixture.entries.get((scope["user"]["id"], query.get("request_key", [""])[0]))
                if entry and fixture.bad_lookup:
                    entry = {**entry, "job_id": 999}
                return self.reply(200, {"items": [entry] if entry else [], "ids": [entry["id"]] if entry else [], "found": entry is not None})

            def do_POST(self):
                scope = self.authorize_time()
                if scope is None:
                    return
                resource = urlsplit(self.path).path.removeprefix(PREFIX)
                form = {key: values[0] for key, values in parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode(), keep_blank_values=True).items()}
                fixture.posts.append((resource, form))
                if fixture.fail_before:
                    return self.reply(503, {"error": "Indisponibile prima della scrittura"})
                entry, = json.loads(form["entries"])
                if not scope["all_jobs"] and entry["job_id"] not in scope["job_ids"]:
                    return self.reply(403, {"error": "Commessa non autorizzata"})
                key = (scope["user"]["id"], form["request_key"])
                if key not in fixture.entries:
                    fixture.entries[key] = {"id": len(fixture.entries) + 1, **entry}
                if fixture.lose_ack:
                    fixture.lose_ack = False
                    return self.reply(503, {"error": "Risposta persa dopo registrazione"})
                return self.reply(200, {"ids": [fixture.entries[key]["id"]]})

        self.server.RequestHandlerClass = Handler


class TicketCommunicationsTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    create_ticket = tickets.TicketApplicationTests.create_ticket
    update_ticket = tickets.TicketApplicationTests.update_ticket
    create_user = tickets.TicketApplicationTests.create_user
    stop = tickets.TicketApplicationTests.stop
    mailbox = raw.TicketRawTests.mailbox
    authorize = raw.TicketRawTests.authorize
    seed = raw.TicketRawTests.seed
    workflow = raw.TicketRawTests.workflow
    claim = raw.TicketRawTests.claim
    convert = raw.TicketRawTests.convert

    @classmethod
    def setUpClass(cls):
        tickets.TicketApplicationTests.setUpClass()
        cls.binary = tickets.APP / "build/punto-tickets"
        cls.files = tempfile.TemporaryDirectory(prefix="ticket-communications-cert-")
        cls.addClassCleanup(cls.files.cleanup)
        cls.cert, cls.key = create_certificate(cls.files.name, "mail")
        cls.token = Path(cls.files.name) / "token"
        cls.token.write_text(TOKEN + "\n")
        cls.token.chmod(0o600)

    def setUp(self):
        self.punto = TimePunto()
        self.addCleanup(self.punto.close)
        self.mail = FakeMailAPI(self.cert, self.key)
        self.addCleanup(self.mail.close)
        self.mail_enabled = True
        self.mail_namespace = "synthetic-ticket-correspondence-v1"
        tickets.TicketApplicationTests.setUp(self)
        self.admin = self.login()
        self.box = self.mailbox()
        self.sender = self.authorize()

    def start(self):
        self.environment["TICKETS_PUNTO_ORIGIN"] = self.punto.url
        for name in MAIL_ENV:
            self.environment.pop(name, None)
        if self.mail_enabled:
            self.environment.update(MAIL_API_URL=self.mail.url, MAIL_API_TOKEN_FILE=str(self.token),
                                    MAIL_API_CA_FILE=str(self.cert), MAIL_API_NAMESPACE=self.mail_namespace)
        tickets.TicketApplicationTests.start(self)

    def restart(self):
        self.stop()
        self.start()
        self.admin = self.login()

    def connect(self, *, time_access=True, owner=700, jobs=(21,), session=None):
        session = session or self.admin
        pending = self.api("POST", "/api/integrations/punto/start", {"time": time_access}, session)[0]
        grant = self.punto.issue(pending["state"], job_ids=jobs)
        self.punto.grants[grant].update(time_access=time_access, calendar_access=False)
        self.punto.grants[grant]["user"]["id"] = owner
        status = self.api("POST", "/api/integrations/punto/connect", {"state": pending["state"], "grant": grant}, session)[0]
        self.assertEqual(status["time_access"], time_access)
        return grant

    def draft(self, ticket, expected=201, **fields):
        result = self.api("POST", f"/api/tickets/{ticket['id']}/messages", {"to_email": "client@example.test", "subject": "Risposta caffè", "body": "Risposta cliente confermata.", **fields}, self.admin, expected=expected)[0]
        return result.get("message", result)

    def send(self, message, expected=200):
        return self.api("POST", f"/api/tickets/{message['ticket_id']}/messages/{message['id']}/send", {"revision": message["revision"]}, self.admin, expected=expected)[0]

    def conversation(self, ticket, expected=200):
        return self.api("GET", f"/api/tickets/{ticket['id']}/conversation", session=self.admin, expected=expected)[0]

    def log_time(self, ticket, expected=201, session=None, **fields):
        payload = {"idempotency_key": "a" * 64, "date": "2026-10-11", "minutes": 45, "description": "Intervento completato", "confirmed": True, **fields}
        return self.api("POST", f"/api/tickets/{ticket['id']}/time", payload, session or self.admin, expected=expected)[0]

    def test_unconfigured_email_keeps_revisioned_drafts_separate_from_internal_notes(self):
        self.mail_enabled = False
        self.restart()
        ticket = self.create_ticket(self.admin, requester="client@example.test")
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nota interna riservata"}, self.admin, expected=201)
        draft = self.draft(ticket)
        listed = self.conversation(ticket)
        self.assertEqual(listed["email"], {"configured": False, "smtp_configured": False})
        self.assertEqual(listed["recipient"], {"email": "client@example.test", "source": "requester"})
        self.assertEqual(len(listed["items"]), 1)
        self.assertNotIn("Nota interna riservata", json.dumps(listed))
        self.send(draft, expected=503)
        self.assertEqual(self.mail.posts(), [])
        saved = self.api("PUT", f"/api/tickets/{ticket['id']}/messages/{draft['id']}", {"revision": 1, "subject": "Aggiornata", "body": "Testo nuovo", "to_email": "other@example.test"}, self.admin)[0]["message"]
        self.assertEqual(saved["revision"], 2)
        self.api("PUT", f"/api/tickets/{ticket['id']}/messages/{draft['id']}", {"revision": 1, "subject": "Stale", "body": "Testo", "to_email": "client@example.test"}, self.admin, expected=409)
        self.restart()
        self.assertEqual(self.conversation(ticket)["items"][0]["body"], "Testo nuovo")
        self.mail_enabled = True
        self.restart()
        self.assertEqual(self.send(saved)["message"]["state"], "sent")

    def test_incoming_raw_and_customer_reply_preserve_thread_and_actual_send(self):
        incoming = self.claim(self.seed())
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_messages SET message_id='incoming@example.test',references_json=? WHERE id=?", (json.dumps(["parent@example.test"]), incoming["id"]))
        ticket = self.convert(incoming)["ticket"]
        conversation = self.conversation(ticket)
        self.assertEqual(conversation["items"][0]["direction"], "incoming")
        self.assertEqual(conversation["recipient"]["email"], "sender@example.test")
        message = self.draft(ticket, to_email="sender@example.test")
        self.assertEqual(message["in_reply_to"], "<incoming@example.test>")
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as db:
            self.assertIsNone(db.execute("SELECT first_response_at FROM ticket_operations WHERE ticket_id=?", (ticket["id"],)).fetchone()[0])
        sent = self.send(message)["message"]
        self.assertEqual(sent["state"], "sent")
        self.assertGreater(sent["sent_at"], 0)
        self.assertEqual(len(self.mail.posts()), 1)
        fields = self.mail.posts()[0]["fields"]
        self.assertEqual(fields["in_reply_to"], ["<incoming@example.test>"])
        self.assertEqual(fields["references"], ["<parent@example.test> <incoming@example.test>"])
        self.assertEqual(fields["message_id"], [message["message_id"]])
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as db:
            self.assertEqual(db.execute("SELECT first_response_at FROM ticket_operations WHERE ticket_id=?", (ticket["id"],)).fetchone()[0], sent["sent_at"])
        self.send(sent, expected=409)
        self.assertEqual(len(self.mail.posts()), 1)

    def test_unknown_delivery_never_retries_and_namespace_change_blocks_old_draft(self):
        ticket = self.create_ticket(self.admin)
        draft = self.draft(ticket)
        self.mail.delivery_code = 503
        self.mail.delivery_body = {"error": "delivery_unknown"}
        unknown = self.send(draft)["message"]
        self.assertEqual(unknown["state"], "unknown")
        self.restart()
        self.send(unknown, expected=409)
        self.assertEqual(len(self.mail.posts()), 1)
        draft = self.draft(ticket)
        self.mail_namespace += "-new"
        self.restart()
        self.send(draft, expected=409)
        self.assertEqual(len(self.mail.posts()), 1)
        draft = self.draft(ticket)
        self.mail = FakeMailAPI(self.cert, self.key)
        self.addCleanup(self.mail.close)
        self.restart()
        self.send(draft, expected=409)
        self.assertEqual(self.mail.posts(), [])

    def test_raw_source_revocation_hides_correspondence_and_blocks_drafts_and_send(self):
        grant = self.connect()
        for binding in ("mailbox", "sender"):
            with self.subTest(binding=binding):
                self.punto.grants[grant]["job_ids"] = [21]
                box = self.mailbox(name="Corrispondenza " + binding)
                sender = self.authorize(mailbox_id=box["id"])
                incoming = self.claim(self.seed(mailbox=box))
                ticket = self.convert(incoming)["ticket"]
                sent = self.draft(ticket, body="Testo riservato derivato dalla sorgente " + binding)
                self.send(sent)
                pending = self.draft(ticket, body="Bozza da conservare senza invio")
                self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nota locale condivisa"}, self.admin, expected=201)
                target = box if binding == "mailbox" else sender
                resource = "mailboxes" if binding == "mailbox" else "senders"
                self.api("PUT", f"/api/raw/{resource}/{target['id']}", {"revision": target["revision"], "job_id": 21}, self.admin)
                self.punto.grants[grant]["job_ids"] = [22]
                delivered = len(self.mail.posts())
                self.conversation(ticket, expected=404)
                self.draft(ticket, expected=404)
                self.api("PUT", f"/api/tickets/{ticket['id']}/messages/{pending['id']}", {"revision": pending["revision"], "to_email": "client@example.test", "subject": "Vietato", "body": "Vietato"}, self.admin, expected=404)
                self.send(pending, expected=404)
                self.assertEqual(len(self.mail.posts()), delivered)
                local = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]
                self.assertEqual(local["ticket"]["id"], ticket["id"])
                self.assertEqual(local["comments"][0]["body"], "Nota locale condivisa")
                self.assertEqual(local["raw_sources"], [])
                self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Nuova nota locale"}, self.admin, expected=201)
                independent = self.create_ticket(self.admin, title="Ticket indipendente senza sorgenti RAW")
                self.assertEqual(self.conversation(independent)["items"], [])
                self.draft(independent)
                self.punto.grants[grant]["job_ids"] = [21]
                self.assertEqual(len(self.conversation(ticket)["items"]), 3)
                self.assertEqual(self.send(pending)["message"]["state"], "sent")

    def test_known_failed_edit_creates_new_delivery_identity_and_unknown_is_immutable(self):
        ticket = self.create_ticket(self.admin)
        draft = self.draft(ticket)
        self.mail.delivery_code = 502
        self.mail.delivery_body = {"error": "smtp_delivery_failed"}
        failed = self.send(draft)["message"]
        self.assertEqual(failed["state"], "failed")
        edited = self.api("PUT", f"/api/tickets/{ticket['id']}/messages/{draft['id']}", {"revision": failed["revision"], "subject": "Nuova verifica", "body": "Correzione confermata", "to_email": "client@example.test"}, self.admin)[0]["message"]
        self.assertNotEqual(edited["message_id"], draft["message_id"])
        self.mail.delivery_code = 200
        self.mail.delivery_body = {"status": "sent"}
        self.send(edited)
        self.assertNotEqual(self.mail.posts()[0]["fields"]["idempotency_key"], self.mail.posts()[1]["fields"]["idempotency_key"])
        self.api("PUT", f"/api/tickets/{ticket['id']}/messages/{draft['id']}", {"revision": edited["revision"] + 2, "subject": "Vietato", "body": "Testo", "to_email": "client@example.test"}, self.admin, expected=409)

    def test_raw_source_revocation_during_status_check_blocks_send_atomically(self):
        incoming = self.claim(self.seed())
        ticket = self.convert(incoming)["ticket"]
        draft = self.draft(ticket)
        handler_type = self.mail.server.RequestHandlerClass
        original_reply = handler_type.reply
        revoked = []

        def reply_after_revocation(handler, status, value, headers=None):
            if urlsplit(handler.path).path == "/v1/status" and not revoked:
                with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
                    db.execute("UPDATE raw_mailboxes SET job_id=21 WHERE id=?", (self.box["id"],))
                revoked.append(True)
            return original_reply(handler, status, value, headers)

        handler_type.reply = reply_after_revocation
        self.send(draft, expected=404)
        self.assertEqual(revoked, [True])
        self.assertEqual(self.mail.posts(), [])
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT state,revision FROM ticket_messages WHERE id=?", (draft["id"],)).fetchone(), ("draft", 1))

    def test_multibyte_outgoing_limits_and_malformed_thread_ids_match_mail_addon(self):
        ticket = self.create_ticket(self.admin)
        for fields in ({"subject": "è" * 126}, {"body": "界" * 2731}, {"subject": "Oggetto\tcon tab"}, {"body": "Testo\x01non valido"}):
            self.api("POST", f"/api/tickets/{ticket['id']}/messages", {"to_email": "client@example.test", "subject": "Oggetto", "body": "Testo", **fields}, self.admin, expected=400)
        incoming = self.claim(self.seed())
        with raw.database(self.environment["TICKETS_DB_PATH"]) as db:
            refs = ["bad@@example.test", 'bad"quote@example.test'] + [str(index) + "@x.test" for index in range(50)]
            db.execute("UPDATE raw_messages SET message_id='bad@@example.test',references_json=? WHERE id=?", (json.dumps(refs), incoming["id"]))
        ticket = self.convert(incoming)["ticket"]
        draft = self.draft(ticket)
        self.assertEqual(draft["in_reply_to"], "")
        self.assertLessEqual(len(draft["references"].split()), 32)
        self.assertNotIn("bad", draft["references"])
        self.assertEqual(self.send(draft)["message"]["state"], "sent")

    def test_recipient_and_header_injection_rejected_and_csrf_required(self):
        ticket = self.create_ticket(self.admin)
        for fields in ({"to_email": "a@example.test,b@example.test"}, {"to_email": "x@example.test\r\nBcc: secret@example.test"}, {"subject": "Oggetto\r\nBcc: secret@example.test"}, {"body": " "}):
            self.api("POST", f"/api/tickets/{ticket['id']}/messages", {"to_email": "x@example.test", "subject": "Oggetto", "body": "Testo", **fields}, self.admin, expected=400)
        status, _, _ = self.request("POST", f"/api/tickets/{ticket['id']}/messages", {"to_email": "x@example.test", "subject": "Oggetto", "body": "Testo"}, {**self.admin, "csrf_token": ""})
        self.assertEqual(status, 403)
        self.assertEqual(self.mail.posts(), [])

    def test_response_templates_are_editable_revisioned_and_reusable(self):
        template = self.api("POST", "/api/response-templates", {"title": "Presa in carico", "subject": "Richiesta ricevuta", "body": "Stiamo verificando la richiesta."}, self.admin, expected=201)[0]["template"]
        edited = self.api("PUT", f"/api/response-templates/{template['id']}", {"revision": 1, "title": "Aggiornamento", "subject": "Novità", "body": "Intervento in corso", "active": False}, self.admin)[0]["template"]
        self.assertEqual(edited["revision"], 2)
        self.assertFalse(edited["active"])
        self.api("PUT", f"/api/response-templates/{template['id']}", {"revision": 1, "title": "Stale", "subject": "Stale", "body": "Stale"}, self.admin, expected=409)
        self.assertEqual(self.api("GET", "/api/response-templates", session=self.admin)[0]["items"], [edited])

    def test_solutions_require_resolved_ticket_and_follow_live_commessa_acl(self):
        grant = self.connect()
        ticket = self.create_ticket(self.admin, job_id=21)
        path = f"/api/tickets/{ticket['id']}/solution"
        content = {"title": "Ripristino", "body": "Procedura riusabile"}
        self.api("POST", path, content, self.admin, expected=409)
        self.update_ticket(self.admin, ticket, status="resolved")
        solution = self.api("POST", path, content, self.admin, expected=201)[0]["solution"]
        self.assertEqual(self.api("GET", "/api/solutions?search=riusabile", session=self.admin)[0]["items"], [solution])
        self.punto.grants[grant]["job_ids"] = []
        self.assertEqual(self.api("GET", "/api/solutions", session=self.admin)[0]["items"], [])
        self.conversation(ticket, expected=404)

    def test_agenda_context_is_only_a_rome_timezone_proposal(self):
        self.connect()
        start = int(datetime(2026, 3, 28, 23, 30, tzinfo=timezone.utc).timestamp())
        ticket = self.create_ticket(self.admin, job_id=21, scheduled_start=start, scheduled_end=start + 5400, scheduled_timezone="Europe/Rome")
        result = self.api("GET", f"/api/tickets/{ticket['id']}/time", session=self.admin)[0]
        self.assertEqual(result["items"], [])
        self.assertEqual((result["suggestion"]["date"], result["suggestion"]["minutes"], result["suggestion"]["source"]), ("2026-03-29", 90, "agenda_proposal"))
        self.assertTrue(result["suggestion"]["requires_confirmation"])
        self.assertEqual(self.punto.posts, [])
        self.log_time(ticket, expected=400, confirmed=False)
        self.assertEqual(self.punto.posts, [])

    def test_time_requires_explicit_capability_job_and_confirmation(self):
        self.connect(time_access=False)
        ticket = self.create_ticket(self.admin, job_id=21)
        self.log_time(ticket, expected=403)
        self.connect(time_access=True)
        local = self.create_ticket(self.admin)
        self.log_time(local, expected=409)
        for fields in ({"minutes": 0}, {"minutes": 1441}, {"minutes": True}, {"date": "2026-02-29"}, {"idempotency_key": "bad"}, {"job_id": 22}):
            self.log_time(ticket, expected=400, **fields)
        self.assertEqual(self.punto.posts, [])

    def test_confirmed_time_is_real_idempotent_and_payload_bound(self):
        self.connect()
        ticket = self.create_ticket(self.admin, job_id=21)
        saved = self.log_time(ticket)["entry"]
        self.assertEqual((saved["state"], saved["job_id"], saved["minutes"], saved["punto_entry_id"]), ("applied", 21, 45, 1))
        self.assertEqual(len(self.punto.posts), 1)
        self.assertEqual(self.log_time(ticket, expected=200)["entry"]["id"], saved["id"])
        self.log_time(ticket, expected=409, minutes=46)
        self.assertEqual(len(self.punto.posts), 1)
        self.assertEqual(json.loads(self.punto.posts[0][1]["entries"]), [{"job_id": 21, "date": "2026-10-11", "minutes": 45, "note": "Intervento completato"}])

    def test_time_lost_ack_recovers_exact_key_and_account_switch_never_duplicates(self):
        self.connect()
        ticket = self.create_ticket(self.admin, job_id=21)
        self.punto.lose_ack = True
        self.log_time(ticket, expected=503)
        self.restart()
        self.connect()
        saved = self.log_time(ticket)["entry"]
        self.assertEqual(saved["state"], "applied")
        self.assertEqual(len(self.punto.posts), 1)
        self.connect(owner=701)
        self.log_time(ticket, expected=409)
        self.assertEqual(len(self.punto.entries), 1)

    def test_time_history_is_personal_live_and_bound_to_source_and_job(self):
        self.connect(owner=700, jobs=(21, 22))
        ticket = self.create_ticket(self.admin, job_id=21)
        self.log_time(ticket)
        self.create_user(self.admin, email="other@example.test")
        other = self.login("other@example.test")
        grant = self.connect(owner=701, jobs=(21, 22), session=other)
        path = f"/api/tickets/{ticket['id']}/time"
        self.assertEqual(self.api("GET", path, session=other)[0]["items"], [])
        self.log_time(ticket, session=other, idempotency_key="b" * 64)
        self.assertEqual([item["owner_user_id"] for item in self.api("GET", path, session=other)[0]["items"]], [701])
        self.assertEqual([item["owner_user_id"] for item in self.api("GET", path, session=self.admin)[0]["items"]], [700])
        self.punto.grants[grant]["time_access"] = False
        self.assertEqual(self.api("GET", path, session=other)[0]["items"], [])
        self.punto.grants[grant]["time_access"] = True
        self.connect(owner=702, jobs=(21, 22), session=other)
        self.assertEqual(self.api("GET", path, session=other)[0]["items"], [])
        grant = self.connect(owner=701, jobs=(21, 22), session=other)
        ticket = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]
        ticket = self.update_ticket(self.admin, ticket, job_id=22)
        self.punto.grants[grant]["job_ids"] = [22]
        self.assertEqual(self.api("GET", path, session=other)[0]["items"], [])
        self.punto.grants[grant]["job_ids"] = [21, 22]
        self.assertEqual(len(self.api("GET", path, session=other)[0]["items"]), 1)
        self.api("POST", "/api/integrations/punto/disconnect", {}, other)
        self.api("GET", path, session=other, expected=404)
        self.punto = TimePunto()
        self.addCleanup(self.punto.close)
        self.restart()
        self.connect(owner=700, jobs=(21, 22))
        self.assertEqual(self.api("GET", path, session=self.admin)[0]["items"], [])

    def test_time_recovery_key_is_only_exposed_to_original_actor_with_live_access(self):
        grant = self.connect(owner=700, jobs=(21, 22))
        ticket = self.create_ticket(self.admin, job_id=21)
        self.punto.lose_ack = True
        self.punto.bad_lookup = True
        self.log_time(ticket, expected=503)
        path = f"/api/tickets/{ticket['id']}/time"
        row, = self.api("GET", path, session=self.admin)[0]["items"]
        self.assertTrue(row["can_retry"])
        self.assertEqual(row["idempotency_key"], "a" * 64)
        self.create_user(self.admin, email="other@example.test")
        other = self.login("other@example.test")
        self.connect(owner=700, jobs=(21, 22), session=other)
        other_row, = self.api("GET", path, session=other)[0]["items"]
        self.assertFalse(other_row["can_retry"])
        self.assertNotIn("idempotency_key", other_row)
        self.log_time(ticket, session=other, expected=409)
        self.assertEqual(len(self.punto.posts), 1)
        self.punto.grants[grant]["time_access"] = False
        self.assertEqual(self.api("GET", path, session=self.admin)[0]["items"], [])
        self.punto.grants[grant]["time_access"] = True
        ticket = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]["ticket"]
        ticket = self.update_ticket(self.admin, ticket, job_id=22)
        moved, = self.api("GET", path, session=self.admin)[0]["items"]
        self.assertFalse(moved["can_retry"])
        self.assertNotIn("idempotency_key", moved)
        ticket = self.update_ticket(self.admin, ticket, job_id=21)
        self.restart()
        self.connect(owner=700, jobs=(21, 22))
        restored, = self.api("GET", path, session=self.admin)[0]["items"]
        self.assertEqual(restored["idempotency_key"], "a" * 64)
        self.punto.bad_lookup = False
        self.log_time(ticket)
        applied, = self.api("GET", path, session=self.admin)[0]["items"]
        self.assertFalse(applied["can_retry"])
        self.assertNotIn("idempotency_key", applied)
        self.assertEqual((len(self.punto.posts), len(self.punto.entries)), (1, 1))

    def test_time_outage_before_write_retries_same_key_and_bad_lookup_fails_closed(self):
        self.connect()
        ticket = self.create_ticket(self.admin, job_id=21)
        self.punto.fail_before = True
        self.log_time(ticket, expected=503)
        self.assertEqual(self.punto.entries, {})
        self.punto.fail_before = False
        self.log_time(ticket)
        self.assertEqual(len(self.punto.entries), 1)
        self.punto.bad_lookup = True
        self.log_time(ticket, expected=503, idempotency_key="b" * 64)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as db:
            self.assertEqual(db.execute("SELECT state FROM ticket_time_entries ORDER BY id DESC LIMIT 1").fetchone()[0], "unknown")

    def test_time_origin_change_and_live_scope_outage_cannot_replay_to_another_source(self):
        self.connect()
        ticket = self.create_ticket(self.admin, job_id=21)
        self.log_time(ticket)
        self.punto.failures["scope"] = (503, {"error": "Sorgente indisponibile"})
        self.log_time(ticket, expected=503)
        self.assertEqual(len(self.punto.posts), 1)
        self.punto = TimePunto()
        self.addCleanup(self.punto.close)
        self.restart()
        self.connect()
        self.log_time(ticket, expected=409)
        self.assertEqual(self.punto.posts, [])


if __name__ == "__main__":
    unittest.main()
