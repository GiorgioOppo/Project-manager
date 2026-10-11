"""RAW workflow tests use immutable synthetic bytes in isolated SQLite stores."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import sqlite3
import subprocess
import time
import unittest

import test_tickets as tickets
import test_ticket_integration as integration


@contextmanager
def database(path):
    connection = sqlite3.connect(path)
    try:
        with connection:
            yield connection
    finally:
        connection.close()


class TicketRawTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    create_ticket = tickets.TicketApplicationTests.create_ticket
    update_ticket = tickets.TicketApplicationTests.update_ticket
    create_user = tickets.TicketApplicationTests.create_user
    stop = tickets.TicketApplicationTests.stop
    connect = integration.TicketPuntoIntegrationTests.connect

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make", "-C", str(tickets.APP), "build/punto-tickets"], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.binary = tickets.APP / "build" / "punto-tickets"

    def setUp(self):
        self.punto = integration.FakePunto()
        self.addCleanup(self.punto.close)
        tickets.TicketApplicationTests.setUp(self)
        self.admin = self.login()
        self.box = self.mailbox()
        self.sender = self.authorize()

    def start(self):
        self.environment["TICKETS_PUNTO_ORIGIN"] = self.punto.url
        tickets.TicketApplicationTests.start(self)

    def mailbox(self, **fields):
        return self.api("POST", "/api/raw/mailboxes", {"name": "Assistenza", "address": "help@example.test",
                        "host": "imap.example.test", "username": "help@example.test", "secret_name": "help.password",
                        "authserv_id": "mx.example.test", "trust_auth_results": True, "active": True, **fields}, self.admin, expected=201)[0]["mailbox"]

    def authorize(self, **fields):
        return self.api("POST", "/api/raw/senders", {"mailbox_id": self.box["id"], "email": "sender@example.test", "name": "Cliente", **fields}, self.admin, expected=201)[0]["sender"]

    def seed(self, *, mailbox=None, state="new", auth="pass", source="imap", job=None, files=2, sender="sender@example.test"):
        mailbox = mailbox or self.box
        raw = (f"From: {sender}\r\nSubject: Richiesta originale\r\nX-Original: <script>unsafe</script>\r\n\r\nTesto immutabile {time.time_ns()}").encode()
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            cursor = db.execute("INSERT INTO raw_messages(mailbox_id,source,source_key,delivery_identity,raw_sha256,raw_eml,received_at,from_email,from_name,subject,headers_json,body_text,text_preview,auth_status,authserv_id,state,job_id,quarantine_reason) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                (mailbox["id"], source, "a"*64, hashlib.sha256(raw+b"delivery").hexdigest(), hashlib.sha256(raw).hexdigest(), raw, int(time.time()), sender, "Cliente", "Richiesta originale", json.dumps([{"name": "X-Original", "value": "<script>unsafe</script>"}]), "Testo immutabile <b>non HTML</b>", "Anteprima originale", auth, "mx.example.test", state, job, "Origine non verificata" if state == "quarantine" else ""))
            identifier = cursor.lastrowid
            attachments = []
            for index in range(files):
                content = f"file content {index}".encode()
                attachments.append(db.execute("INSERT INTO raw_attachments(raw_message_id,filename,content_type,size,sha256,content) VALUES(?,?,?,?,?,?)",
                                               (identifier, 'report\r\nX-Evil: injected.html' if index == 0 else f"file-{index}.txt", "text/html" if index == 0 else "text/plain", len(content), hashlib.sha256(content).hexdigest(), content)).lastrowid)
        return {"id": identifier, "revision": 1, "files": attachments, "bytes": raw}

    def detail(self, raw, session=None, expected=200):
        return self.api("GET", f"/api/raw/messages/{raw['id']}", session=session or self.admin, expected=expected)[0]

    def workflow(self, raw, action, session=None, expected=200, **extra):
        return self.api("POST", f"/api/raw/messages/{raw['id']}/{action}", {"revision": raw["revision"], **extra}, session or self.admin, expected=expected)[0]

    def claim(self, raw, session=None):
        return {**raw, **self.workflow(raw, "claim", session)["raw"]}

    def convert(self, raw, session=None, expected=201, **fields):
        return self.workflow(raw, "convert", session, expected, ticket={"title": "Ticket da email", **fields}, attachment_ids=raw["files"][:1])

    def test_admin_configuration_is_revisioned_and_has_no_secret_values(self):
        operator = self.create_user(self.admin)
        session = self.login(operator["email"])
        for resource in ("mailboxes", "senders"):
            self.api("GET", "/api/raw/" + resource, session=session, expected=403)
        draft = self.api("POST", "/api/raw/mailboxes", {"name": "Da configurare", "address": "DRAFT@EXAMPLE.TEST"}, self.admin, expected=201)[0]["mailbox"]
        self.assertFalse(draft["active"])
        self.assertEqual(draft["address"], "draft@example.test")
        self.assertEqual(draft["worker"], {"last_polled_at": None, "last_error": ""})
        for fields in ({"active": True}, {"secret_name": "../private"}, {"secret_name": ".hidden"}, {"host": "https://imap.example.test"}, {"host": ".."}, {"port": 0}, {"auth_mode": "plaintext"}, {"username": "x\r\nsecret"}):
            self.api("PUT", f"/api/raw/mailboxes/{draft['id']}", {"revision": draft["revision"], **fields}, self.admin, expected=400)
        changed = self.api("PUT", f"/api/raw/mailboxes/{draft['id']}", {"revision": draft["revision"], "name": "Modificata"}, self.admin)[0]["mailbox"]
        self.assertEqual(changed["revision"], draft["revision"]+1)
        self.api("PUT", f"/api/raw/mailboxes/{draft['id']}", {"revision": draft["revision"], "name": "Conflitto"}, self.admin, expected=409)
        self.api("POST", "/api/raw/senders", {"mailbox_id": self.box["id"], "email": "SENDER@EXAMPLE.TEST"}, self.admin, expected=409)
        saved = self.api("PUT", f"/api/raw/senders/{self.sender['id']}", {"mailbox_id": self.box["id"], "revision": self.sender["revision"], "name": "Nuovo nome"}, self.admin)[0]["sender"]
        self.assertEqual(saved["name"], "Nuovo nome")
        self.api("PUT", f"/api/raw/senders/{saved['id']}", {"revision": saved["revision"], "email": "other@example.test"}, self.admin, expected=400)
        listed = self.api("GET", "/api/raw/mailboxes", session=self.admin)[0]
        self.assertNotIn("password", listed["items"][0])
        self.assertNotIn("secret", listed["items"][0])
        normalized = self.mailbox(name="Provider maiuscolo", authserv_id="MX.EXAMPLE.TEST")
        self.assertEqual(normalized["authserv_id"], "mx.example.test")

    def test_quarantine_visibility_filters_and_statistics_follow_roles(self):
        raw = self.seed()
        quarantine = self.seed(state="quarantine", auth="unverified")
        operator = self.create_user(self.admin)
        session = self.login(operator["email"])
        listed = self.api("GET", "/api/raw/messages?state=all", session=session)[0]
        self.assertEqual([row["id"] for row in listed["items"]], [raw["id"]])
        self.assertEqual(listed["stats"]["quarantine"], 0)
        self.assertEqual(listed["mailboxes"], [{"id": self.box["id"], "name": self.box["name"]}])
        self.detail(quarantine, session, expected=404)
        self.workflow(quarantine, "claim", expected=409)
        self.workflow(quarantine, "convert", expected=403, ticket={"title": "Non consentito"})
        self.assertEqual(self.api("GET", "/api/raw/messages?state=quarantine", session=self.admin)[0]["total"], 1)
        for query in ("state=bogus", "limit=0", "limit=101", "offset=-1", "state=new&state=new", "mailbox_id=0", "search=x&&state=new", "unknown=x"):
            self.api("GET", "/api/raw/messages?" + query, session=self.admin, expected=400)

    def test_claim_release_and_discard_use_optimistic_revision(self):
        raw = self.seed()
        other_user = self.create_user(self.admin)
        other = self.login(other_user["email"])
        claimed = self.claim(raw)
        self.workflow(raw, "claim", other, expected=409)
        self.workflow(claimed, "release", other, expected=409)
        released = {**raw, **self.workflow(claimed, "release")["raw"]}
        self.assertEqual(released["state"], "new")
        self.assertIsNone(released["claimed_by"])
        claimed_other = self.claim(released, other)
        for reason in (None, "", "   "):
            self.workflow(claimed_other, "discard", other, expected=400, **({"reason": reason} if reason is not None else {}))
        self.assertEqual(self.detail(raw)["raw"]["revision"], claimed_other["revision"])
        discarded = self.workflow(claimed_other, "discard", other, reason="Duplicato verificato")["raw"]
        self.assertEqual(discarded["state"], "discarded")
        self.assertIn("Duplicato verificato", json.dumps(self.detail(raw)))
        self.workflow(discarded, "claim", expected=409)

    def test_simultaneous_claims_have_one_winner(self):
        raw = self.seed()
        other_user = self.create_user(self.admin)
        other = self.login(other_user["email"])
        path = f"/api/raw/messages/{raw['id']}/claim"
        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(lambda session: self.request("POST", path, {"revision": 1}, session), (self.admin, other)))
        self.assertEqual(sorted(status for status, _, _ in outcomes), [200, 409])
        self.assertEqual(self.detail(raw)["raw"]["revision"], 2)

    def test_conversion_is_atomic_replay_safe_and_keeps_original_bytes(self):
        raw = self.claim(self.seed())
        converted = self.convert(raw)
        self.assertFalse(converted["replayed"])
        ticket_id = converted["ticket"]["id"]
        replay = self.convert(raw, expected=200, title="Tentativo differente")
        self.assertTrue(replay["replayed"])
        self.assertEqual(replay["ticket"]["id"], ticket_id)
        self.assertEqual(replay["ticket"]["title"], "Ticket da email")
        detail = self.api("GET", f"/api/tickets/{ticket_id}", session=self.admin)[0]
        self.assertEqual([source["id"] for source in detail["raw_sources"]], [raw["id"]])
        self.assertEqual([file["id"] for file in detail["attachments"]], raw["files"][:1])
        self.assertEqual(detail["attachments"][0]["scan_status"], "not_scanned")
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM tickets").fetchone()[0], 1)
            self.assertEqual(bytes(db.execute("SELECT raw_eml FROM raw_messages WHERE id=?", (raw["id"],)).fetchone()[0]), raw["bytes"])
            self.assertEqual(db.execute("SELECT count(*) FROM ticket_raw_links").fetchone()[0], 1)

    def test_concurrent_conversion_replays_one_ticket(self):
        raw = self.claim(self.seed())
        path = f"/api/raw/messages/{raw['id']}/convert"
        body = {"revision": raw["revision"], "ticket": {"title": "Conversione concorrente"}, "attachment_ids": raw["files"]}
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.request("POST", path, body, self.admin), range(2)))
        self.assertEqual(sorted(code for code, _, _ in results), [200, 201])
        self.assertEqual(len({value["ticket"]["id"] for _, value, _ in results}), 1)
        self.assertEqual(sorted(value["replayed"] for _, value, _ in results), [False, True])

    def test_selected_attachments_belong_to_message_and_validation_rolls_back(self):
        raw = self.claim(self.seed())
        other = self.seed()
        for selected in ([other["files"][0]], [raw["files"][0], raw["files"][0]], [0], ["1"], [True]):
            self.workflow(raw, "convert", expected=400, ticket={"title": "Da non creare"}, attachment_ids=selected)
        self.workflow(raw, "convert", expected=400, ticket={"title": ""}, attachment_ids=[])
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM tickets").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT count(*) FROM ticket_raw_links").fetchone()[0], 0)
        self.assertEqual(self.detail(raw)["raw"]["revision"], raw["revision"])

    def test_sender_revocation_expiry_and_provider_policy_are_rechecked(self):
        for mutation in ("sender_active", "sender_expiry", "mailbox_active", "provider_trust", "authserv_changed"):
            with self.subTest(mutation=mutation):
                raw = self.claim(self.seed())
                with database(self.environment["TICKETS_DB_PATH"]) as db:
                    if mutation == "sender_active": db.execute("UPDATE raw_senders SET active=0")
                    elif mutation == "sender_expiry": db.execute("UPDATE raw_senders SET valid_until=?", (int(time.time())-1,))
                    elif mutation == "mailbox_active": db.execute("UPDATE raw_mailboxes SET active=0")
                    elif mutation == "provider_trust": db.execute("UPDATE raw_mailboxes SET trust_auth_results=0")
                    else: db.execute("UPDATE raw_mailboxes SET authserv_id='different.example.test'")
                self.convert(raw, expected=403)
                with database(self.environment["TICKETS_DB_PATH"]) as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM tickets").fetchone()[0], 0)
                    db.execute("UPDATE raw_senders SET active=1,valid_until=NULL")
                    db.execute("UPDATE raw_mailboxes SET active=1,trust_auth_results=1,authserv_id='mx.example.test'")
        raw = self.seed(auth="unverified", source="import")
        self.convert(self.claim(raw), expected=403)

    def test_sender_revocation_during_external_validation_prevents_atomic_conversion(self):
        self.connect(self.admin, all_jobs=True)
        raw = self.claim(self.seed())
        handler_type = self.punto.server.RequestHandlerClass
        original_reply = handler_type.reply
        revoked = []

        def reply_after_revocation(handler, status, value, headers=None):
            if "/validate?" in handler.path and not revoked:
                with database(self.environment["TICKETS_DB_PATH"]) as db:
                    db.execute("UPDATE raw_senders SET active=0 WHERE id=?", (self.sender["id"],))
                revoked.append(True)
            return original_reply(handler, status, value, headers)

        handler_type.reply = reply_after_revocation
        self.convert(raw, expected=403, job_id=21)
        self.assertEqual(revoked, [True])
        self.assertEqual(self.detail(raw)["raw"]["revision"], raw["revision"])
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM tickets").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT count(*) FROM ticket_raw_links").fetchone()[0], 0)

    def test_raw_job_binding_and_linked_ticket_acl_protect_all_raw_surfaces(self):
        grant = self.connect(self.admin, all_jobs=True)
        raw = self.claim(self.seed(job=21))
        self.convert(raw, expected=403, job_id=22)
        created = self.convert(raw, job_id=21)
        private_ticket = created["ticket"]
        unbound = self.claim(self.seed())
        linked = self.workflow(unbound, "link", ticket_id=private_ticket["id"], attachment_ids=unbound["files"][:1])
        self.assertEqual(linked["raw"]["state"], "linked")
        self.workflow(unbound, "link", expected=200, ticket_id=private_ticket["id"], attachment_ids=[])
        another_ticket = self.create_ticket(self.admin)
        self.workflow(unbound, "link", expected=409, ticket_id=another_ticket["id"], attachment_ids=[])
        self.punto.grants[grant]["all_jobs"] = False
        self.punto.grants[grant]["job_ids"] = [22]
        for message in (raw, unbound):
            self.detail(message, expected=404)
            self.api("GET", f"/api/raw/messages/{message['id']}/eml", session=self.admin, expected=404)
            self.api("GET", f"/api/raw/messages/{message['id']}/attachments/{message['files'][0]}", session=self.admin, expected=404)
        listing = self.api("GET", "/api/raw/messages?state=all", session=self.admin)[0]
        self.assertEqual(listing["total"], 0)
        self.assertEqual(listing["stats"]["converted"], 0)
        self.assertEqual(listing["stats"]["linked"], 0)
        self.api("GET", f"/api/tickets/{private_ticket['id']}/attachments/{unbound['files'][0]}", session=self.admin, expected=404)

    def test_mailbox_and_current_sender_bind_conversion_to_same_project(self):
        self.connect(self.admin, all_jobs=True)
        bound_box = self.mailbox(name="Commessa", job_id=21)
        self.authorize(mailbox_id=bound_box["id"], job_id=21)
        raw = self.claim(self.seed(mailbox=bound_box, job=21))
        self.convert(raw, expected=403, job_id=22)
        result = self.convert(raw)
        self.assertEqual(result["ticket"]["job_id"], 21)
        other = self.claim(self.seed())
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            db.execute("UPDATE raw_senders SET job_id=21 WHERE id=?", (self.sender["id"],))
        self.convert(other, expected=403)
        self.convert(other, job_id=21)

    def test_current_mailbox_and_sender_bindings_hide_previously_unbound_messages(self):
        messages = []
        for binding in ("mailbox", "sender"):
            box = self.mailbox(name="Coda " + binding)
            sender = self.authorize(mailbox_id=box["id"])
            raw = self.claim(self.seed(mailbox=box))
            raw["linked_ticket"] = self.convert(raw)["ticket"]["id"]
            self.detail(raw)
            with database(self.environment["TICKETS_DB_PATH"]) as db:
                if binding == "mailbox":
                    db.execute("UPDATE raw_mailboxes SET job_id=21 WHERE id=?", (box["id"],))
                else:
                    db.execute("UPDATE raw_senders SET job_id=21 WHERE id=?", (sender["id"],))
            messages.append(raw)

        def assert_hidden():
            for raw in messages:
                self.detail(raw, expected=404)
                self.api("GET", f"/api/raw/messages/{raw['id']}/eml", session=self.admin, expected=404)
                self.api("GET", f"/api/raw/messages/{raw['id']}/attachments/{raw['files'][0]}", session=self.admin, expected=404)
                ticket = self.api("GET", f"/api/tickets/{raw['linked_ticket']}", session=self.admin)[0]
                self.assertEqual(ticket["raw_sources"], [])
                self.assertEqual(ticket["attachments"], [])
                self.api("GET", f"/api/tickets/{raw['linked_ticket']}/attachments/{raw['files'][0]}", session=self.admin, expected=404)
            listing = self.api("GET", "/api/raw/messages?state=all", session=self.admin)[0]
            self.assertEqual(listing["items"], [])
            self.assertEqual(listing["total"], 0)
            self.assertTrue(all(value == 0 for value in listing["stats"].values()))

        assert_hidden()  # No Punto grant.
        grant = self.connect(self.admin, all_jobs=False, job_ids=[22])
        assert_hidden()
        self.punto.grants[grant]["job_ids"] = [21]
        for raw in messages:
            self.detail(raw)
        self.assertEqual(self.api("GET", "/api/raw/messages?state=all", session=self.admin)[0]["total"], 2)

    def test_unbound_ticket_and_shared_raw_sources_stay_readable_during_punto_outage(self):
        self.connect(self.admin)
        raw = self.claim(self.seed())
        ticket = self.convert(raw)["ticket"]
        self.punto.failures["scope"] = (503, {"ok": False, "error": "Servizio non disponibile"})
        detail = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]
        self.assertEqual(detail["ticket"]["id"], ticket["id"])
        self.assertEqual([source["id"] for source in detail["raw_sources"]], [raw["id"]])
        self.assertEqual([file["id"] for file in detail["attachments"]], raw["files"][:1])
        self.assertEqual(self.detail(raw)["raw"]["body_text"], "Testo immutabile <b>non HTML</b>")
        status, body, headers = self.request("GET", f"/api/raw/messages/{raw['id']}/eml", session=self.admin)
        self.assertEqual(status, 200)
        self.assertEqual(body.encode(), raw["bytes"])
        self.assertEqual(headers["content-type"], "application/octet-stream")
        for path in (f"/api/raw/messages/{raw['id']}/attachments/{raw['files'][0]}",
                     f"/api/tickets/{ticket['id']}/attachments/{raw['files'][0]}"):
            status, body, headers = self.request("GET", path, session=self.admin)
            self.assertEqual(status, 200)
            self.assertEqual(body, "file content 0")
            self.assertEqual(headers["content-type"], "application/octet-stream")

    def test_downloads_are_authenticated_attachments_and_ticket_exposes_only_selected(self):
        raw = self.seed()
        path = f"/api/raw/messages/{raw['id']}"
        self.api("GET", path + "/eml", expected=401)
        self.api("GET", path + f"/attachments/{raw['files'][0]}", expected=401)
        status, content, headers = self.request("GET", path + "/eml", session=self.admin)
        self.assertEqual(status, 200)
        self.assertEqual(content.encode(), raw["bytes"])
        self.assertEqual(headers["content-type"], "application/octet-stream")
        self.assertTrue(headers["content-disposition"].startswith("attachment;"))
        status, _, headers = self.request("GET", path + f"/attachments/{raw['files'][0]}", session=self.admin)
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "application/octet-stream")
        self.assertEqual(headers["x-content-type-options"], "nosniff")
        self.assertNotIn("x-evil", headers)
        self.assertNotIn("\r", headers["content-disposition"])
        ticket = self.convert(self.claim(raw))["ticket"]
        self.api("GET", f"/api/tickets/{ticket['id']}/attachments/{raw['files'][1]}", session=self.admin, expected=404)
        status, _, headers = self.request("GET", f"/api/tickets/{ticket['id']}/attachments/{raw['files'][0]}", session=self.admin)
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "application/octet-stream")

    def test_schema_three_upgrade_preserves_existing_ticket_and_calendar_tables(self):
        ticket = self.create_ticket(self.admin)
        self.stop()
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            for table in ("raw_history", "ticket_raw_attachments", "ticket_raw_links", "raw_receipts", "raw_attachments", "raw_messages", "raw_mailbox_state", "raw_senders", "raw_mailboxes"):
                db.execute("DROP TABLE " + table)
            db.execute("PRAGMA user_version=3")
        self.start()
        self.admin = self.login()
        detail = self.api("GET", f"/api/tickets/{ticket['id']}", session=self.admin)[0]
        self.assertEqual(detail["ticket"]["title"], ticket["title"])
        self.assertEqual(detail["raw_sources"], [])
        with database(self.environment["TICKETS_DB_PATH"]) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertEqual(db.execute("SELECT count(*) FROM ticket_calendar").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
