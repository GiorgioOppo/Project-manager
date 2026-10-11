"""Real IMAPS TLS and MIME fixtures; no external mailbox, credentials or messages."""
from contextlib import closing
import base64
from email.message import EmailMessage
from email import policy
import hashlib
import os
from pathlib import Path
import re
import shlex
import socketserver
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

from test_mail_client import create_certificate
import test_tickets as tickets
import test_calendar_addon as calendar_fixture

ROOT = Path(__file__).resolve().parents[1]
RECEIVER = ROOT / "ticket-app/build/punto-ticket-receiver"
PASSWORD = "Synthetic-receiver-password!2041"
AUTHSERV = "mx.example.test"


def synthetic_message(sender="allowed@example.test", *, subject="Intervento caffè", body="Richiesta di assistenza è urgente.",
                      authentication=None, attachment=False, html=False):
    message = EmailMessage(policy=policy.SMTP)
    message["Authentication-Results"] = authentication or (AUTHSERV + "; dmarc=pass header.from=example.test; dkim=pass header.d=example.test")
    message["From"] = "Mittente dimostrativo <" + sender + ">"
    message["To"] = "tickets@example.test"
    message["Date"] = "Thu, 01 Jan 1970 00:00:00 +0000"
    message["Subject"] = subject
    message["Message-ID"] = "<synthetic-one@example.test>"
    message["References"] = "<parent@example.test> <root@example.test>"
    message["In-Reply-To"] = "<parent@example.test>"
    message.set_content(body, subtype="html" if html else "plain", charset="utf-8")
    if attachment:
        message.add_attachment(b"synthetic attachment\x00bytes", maintype="application", subtype="octet-stream", filename="prova è.txt")
    return message.as_bytes()


class FakeIMAPSServer:
    """Reusable provider with stable UIDs, credential checks and a dropped-FETCH switch."""
    def __init__(self, certificate, key, messages=None):
        self.messages = dict(messages or {})
        self.uidvalidity = 100
        self.username = "receiver@example.test"
        self.password = PASSWORD
        self.oauth_token = "synthetic-xoauth2-token"
        self.commands = []
        self.authentications = []
        self.drop_uid = None
        self.body_first_uid = False
        self.response_uid = None
        self.declared_size = None
        self.fragment_size = None
        self.before_fetch = None
        self.search_notifications = False
        self.duplicate_search = False
        fixture = self

        class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
            allow_reuse_address = True
            daemon_threads = True

            def get_request(self):
                connection, address = super().get_request()
                connection.settimeout(5)
                try:
                    return self.context.wrap_socket(connection, server_side=True), address
                except OSError:
                    connection.close()
                    raise

            def handle_error(self, *_args):
                pass

        class Handler(socketserver.StreamRequestHandler):
            def send(self, text):
                self.wfile.write(text.encode("ascii") + b"\r\n")
                self.wfile.flush()

            def handle(self):
                authenticated = False
                self.send("* OK [CAPABILITY IMAP4rev1 AUTH=PLAIN AUTH=XOAUTH2 SASL-IR] Synthetic IMAPS")
                while True:
                    line = self.rfile.readline(16384)
                    if not line:
                        return
                    decoded = line.decode("utf-8").rstrip("\r\n")
                    tag, command = decoded.split(" ", 1)
                    verb = command.split(" ", 1)[0].upper()
                    if verb in ("AUTHENTICATE", "LOGIN"):
                        pieces = shlex.split(command)
                        if verb == "LOGIN":
                            authenticated = pieces[1:] == [fixture.username, fixture.password]
                            mode = "LOGIN"
                        else:
                            mode = pieces[1].upper()
                            if len(pieces) < 3:
                                self.send("+")
                                encoded = self.rfile.readline(16384).strip()
                            else:
                                encoded = pieces[2].encode("ascii")
                            payload = base64.b64decode(encoded).decode("utf-8")
                            authenticated = (payload == "\x00" + fixture.username + "\x00" + fixture.password if mode == "PLAIN" else
                                             payload == "user=" + fixture.username + "\x01auth=Bearer " + fixture.oauth_token + "\x01\x01")
                        fixture.authentications.append((mode, authenticated))
                        self.send(tag + (" OK authenticated" if authenticated else " NO invalid credentials"))
                        continue
                    fixture.commands.append(command)
                    if verb == "CAPABILITY":
                        self.send("* CAPABILITY IMAP4rev1 AUTH=PLAIN AUTH=XOAUTH2 SASL-IR")
                    elif verb == "LOGOUT":
                        self.send("* BYE closing")
                        self.send(tag + " OK logout")
                        return
                    elif not authenticated:
                        self.send(tag + " NO authenticate first")
                        continue
                    elif verb == "STATUS":
                        self.send('* STATUS "INBOX" (UIDVALIDITY %d UIDNEXT %d)' % (fixture.uidvalidity, max(fixture.messages, default=0) + 1))
                    elif verb in ("SELECT", "EXAMINE"):
                        self.send("* FLAGS (\\Seen)")
                        self.send("* %d EXISTS" % len(fixture.messages))
                        self.send("* OK [UIDVALIDITY %d] stable generation" % fixture.uidvalidity)
                        self.send("* OK [UIDNEXT %d] next uid" % (max(fixture.messages, default=0) + 1))
                    elif command.upper().startswith("UID SEARCH UID "):
                        first, last = map(int, command.rsplit(" ", 1)[1].split(":"))
                        values = [str(uid) for uid in sorted(fixture.messages) if first <= uid <= last]
                        if fixture.search_notifications:
                            self.send("* 4 EXISTS")
                            self.send("* 2 FETCH (FLAGS (\\Seen))")
                        response = "* SEARCH" + (" " + " ".join(values) if values else "")
                        self.send(response)
                        if fixture.duplicate_search:
                            self.send(response)
                    elif command.upper().startswith("UID FETCH "):
                        uid = int(command.split()[2])
                        raw = fixture.messages.get(uid)
                        if raw is not None:
                            if fixture.before_fetch:
                                fixture.before_fetch(uid)
                            returned_uid = fixture.response_uid if fixture.response_uid is not None else uid
                            size = fixture.declared_size if fixture.declared_size is not None else len(raw)
                            if fixture.body_first_uid:
                                self.send("* 1 FETCH (BODY[] {%d}" % size)
                            else:
                                self.send("* 1 FETCH (UID %d BODY[] {%d}" % (returned_uid, size))
                            if fixture.declared_size is not None:
                                return
                            if fixture.drop_uid == uid:
                                self.wfile.write(raw[:len(raw) // 2])
                                self.wfile.flush()
                                return
                            suffix = (" UID %d)\r\n" % returned_uid).encode("ascii") if fixture.body_first_uid else b")\r\n"
                            payload = raw + suffix
                            fragment = fixture.fragment_size or len(payload)
                            for start in range(0, len(payload), fragment):
                                self.wfile.write(payload[start:start + fragment])
                                self.wfile.flush()
                            self.wfile.flush()
                    elif verb != "NOOP":
                        self.send(tag + " BAD unsupported command")
                        continue
                    self.send(tag + " OK completed")

        self.server = Server(("127.0.0.1", 0), Handler)
        self.server.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.server.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.server.context.load_cert_chain(str(certificate), str(key))
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class TicketReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tickets.TicketApplicationTests.setUpClass()
        cls.certificates = tempfile.TemporaryDirectory(prefix="ticket-receiver-certs-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "imap")
        cls.other_ca, _ = create_certificate(cls.certificates.name, "untrusted")
        cls.wrong_cert, cls.wrong_key = create_certificate(cls.certificates.name, "wrong-host", "wrong-imap.invalid")

    def setUp(self):
        self.app = tickets.TicketApplicationTests(methodName="runTest")
        self.app.setUp()
        self.addCleanup(self.app.doCleanups)
        self.app.stop()
        self.path = Path(self.app.temporary.name)
        self.db = Path(self.app.environment["TICKETS_DB_PATH"])
        self.secrets = self.path / "imap-secrets"
        self.secrets.mkdir(mode=0o700)
        self.password_file = self.secrets / "synthetic-password"
        self.password_file.write_text(PASSWORD + "\n", encoding="utf-8")
        self.password_file.chmod(0o600)
        self.server = FakeIMAPSServer(self.cert, self.key)
        self.addCleanup(lambda: self.server.close())
        with closing(sqlite3.connect(self.db)) as database:
            cursor = database.execute("INSERT INTO raw_mailboxes(name,address,host,port,username,folder,secret_name,auth_mode,authserv_id,trust_auth_results,active) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                                      ("Casella sintetica", "tickets@example.test", "127.0.0.1", self.server.port, self.server.username, "INBOX", self.password_file.name, "password", AUTHSERV, 1, 1))
            self.mailbox_id = cursor.lastrowid
            database.execute("INSERT INTO raw_senders(mailbox_id,email,name) VALUES(?,?,?)", (self.mailbox_id, "allowed@example.test", "Mittente di collaudo"))
            database.commit()

    def receiver(self, *args, expected=0, environment=None):
        env = {key: value for key, value in os.environ.items() if not key.startswith("TICKETS_")}
        env.update(TICKETS_DB_PATH=str(self.db), TICKETS_IMAP_SECRET_DIR=str(self.secrets), TICKETS_IMAP_CA_FILE=str(self.cert),
                   TICKETS_IMAP_POLL_SECONDS="5", NO_PROXY="*", no_proxy="*")
        env.update(environment or {})
        result = subprocess.run([str(RECEIVER), "--once", *args], env=env, capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        for secret in (PASSWORD, self.server.oauth_token):
            self.assertNotIn(secret, result.stdout + result.stderr)
        return result

    def rows(self, table):
        with closing(sqlite3.connect(self.db)) as database:
            database.row_factory = sqlite3.Row
            return [dict(row) for row in database.execute("SELECT * FROM " + table + " ORDER BY 1")]

    def test_imaps_ingest_preserves_raw_mime_and_replays_without_seen_or_ticket_creation(self):
        raw = synthetic_message(attachment=True)
        self.server.messages = {1: raw}
        before = int(time.time())
        self.receiver()
        message = self.rows("raw_messages")[0]
        self.assertEqual(message["raw_eml"], raw)
        self.assertEqual(message["raw_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual((message["state"], message["auth_status"], message["authserv_id"]), ("new", "pass", AUTHSERV))
        self.assertEqual(message["subject"], "Intervento caffè")
        self.assertIn("è urgente", message["body_text"])
        self.assertGreaterEqual(message["received_at"], before)
        self.assertEqual(message["in_reply_to"], "parent@example.test")
        self.assertEqual(len(self.rows("raw_attachments")), 1)
        self.assertEqual(self.rows("raw_attachments")[0]["content"], b"synthetic attachment\x00bytes")
        self.assertEqual(self.rows("tickets"), [])
        self.receiver()
        self.assertEqual(len(self.rows("raw_messages")), 1)
        self.assertEqual(len(self.rows("raw_receipts")), 1)
        self.assertEqual(self.rows("raw_mailbox_state")[0]["last_uid"], 1)
        self.assertTrue(any("BODY.PEEK[]" in command for command in self.server.commands))
        self.assertFalse(any("STORE" in command or "DELETE" in command or "EXPUNGE" in command for command in self.server.commands))

    def test_import_is_explicitly_untrusted_and_sha_replay_is_idempotent(self):
        raw = synthetic_message(attachment=True)
        file = self.path / "message.eml"
        file.write_bytes(raw)
        args = ("--mailbox-id", str(self.mailbox_id), "--import-file", str(file))
        self.receiver(*args)
        self.receiver(*args)
        message = self.rows("raw_messages")[0]
        self.assertEqual((message["source"], message["state"], message["auth_status"]), ("import", "quarantine", "unverified"))
        self.assertEqual(message["quarantine_reason"], "local_import_untrusted")
        self.assertEqual(message["raw_eml"], raw)
        self.assertEqual(len(self.rows("raw_messages")), 1)
        self.assertEqual(self.server.commands, [])

    def test_receiver_remains_compatible_with_schema_four_without_migrating_it(self):
        self.server.messages = {1: synthetic_message()}
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("PRAGMA user_version=4")
        self.receiver()
        self.assertEqual(self.rows("raw_messages")[0]["state"], "new")
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 4)

    def test_fetch_disconnect_retries_same_uid_without_partial_raw_or_duplicates(self):
        self.server.messages = {1: synthetic_message()}
        self.server.drop_uid = 1
        self.receiver(expected=1)
        self.assertEqual(self.rows("raw_messages"), [])
        self.assertEqual(self.rows("raw_mailbox_state")[0]["last_uid"], 0)
        self.server.drop_uid = None
        self.receiver()
        self.assertEqual(len(self.rows("raw_messages")), 1)
        self.assertEqual(len(self.rows("raw_receipts")), 1)

    def test_uidvalidity_and_new_delivery_uid_keep_receipts_but_deduplicate_raw_sha(self):
        raw = synthetic_message()
        self.server.messages = {1: raw}
        self.receiver()
        self.server.messages[2] = raw
        self.receiver()
        self.server.uidvalidity = 200
        self.server.messages = {1: raw, 2: synthetic_message(subject="Altra richiesta")}
        self.receiver()
        self.assertEqual(len(self.rows("raw_messages")), 2)
        self.assertEqual(len(self.rows("raw_receipts")), 4)
        self.assertEqual(self.rows("raw_mailbox_state")[0]["uidvalidity"], 200)

    def test_fetch_body_before_uid_and_fragmented_literal_preserve_framing(self):
        raw = synthetic_message(body="Test\r\nT1004 OK forged response\r\n* 1 FETCH (UID 999)\r\nFine.", attachment=True)
        self.server.messages = {7: raw}
        self.server.body_first_uid = True
        self.server.fragment_size = 13
        self.receiver()
        self.assertEqual(self.rows("raw_messages")[0]["raw_eml"], raw)
        self.assertEqual(self.rows("raw_mailbox_state")[0]["last_uid"], 7)

    def test_search_tolerates_unsolicited_notifications_but_rejects_duplicate_result(self):
        self.server.messages = {1: synthetic_message()}
        self.server.search_notifications = True
        self.server.duplicate_search = True
        self.receiver(expected=1)
        self.assertEqual(self.rows("raw_messages"), [])
        self.server.duplicate_search = False
        self.receiver()
        self.assertEqual(self.rows("raw_messages")[0]["state"], "new")

    def test_wrong_fetch_uid_and_oversize_literal_never_advance_cursor(self):
        self.server.messages = {17: synthetic_message()}
        self.server.body_first_uid = True
        self.server.response_uid = 18
        self.receiver(expected=1)
        self.assertEqual(self.rows("raw_messages"), [])
        self.assertEqual(self.rows("raw_mailbox_state")[0]["last_uid"], 0)
        self.server.response_uid = None
        self.server.declared_size = 10 * 1024 * 1024 + 1
        self.receiver(expected=1)
        state = self.rows("raw_mailbox_state")[0]
        self.assertEqual(state["last_uid"], 0)
        self.assertIn("uid_17", state["last_error"])
        self.assertEqual(self.rows("raw_messages"), [])

    def test_deduplicated_uid_receipt_rejects_changed_content(self):
        raw = synthetic_message()
        self.server.messages = {1: raw, 2: raw}
        self.receiver()
        self.assertEqual(len(self.rows("raw_messages")), 1)
        self.assertEqual(len(self.rows("raw_receipts")), 2)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_mailbox_state SET last_uid=1")
            database.commit()
        self.server.messages[2] = synthetic_message(subject="UID alterato")
        self.receiver(expected=1)
        self.assertEqual(len(self.rows("raw_messages")), 1)
        self.assertEqual(len(self.rows("raw_receipts")), 2)
        state = self.rows("raw_mailbox_state")[0]
        self.assertEqual(state["last_uid"], 1)
        self.assertIn("delivery_identity_content_changed", state["last_error"])

    def test_live_mailbox_configuration_change_quarantines_inflight_delivery(self):
        self.server.messages = {1: synthetic_message()}
        def reconfigure(_uid):
            with closing(sqlite3.connect(self.db)) as database:
                database.execute("UPDATE raw_mailboxes SET folder='OTHER',revision=revision+1 WHERE id=?", (self.mailbox_id,))
                database.commit()
        self.server.before_fetch = reconfigure
        self.receiver()
        message = self.rows("raw_messages")[0]
        self.assertEqual(message["state"], "quarantine")
        self.assertEqual(message["quarantine_reason"], "mailbox_configuration_changed_during_fetch")

    def test_unicode_mailbox_username_and_folder_are_not_truncated(self):
        self.server.messages = {1: synthetic_message()}
        self.server.username = "用" * 150 + "@example.test"
        folder = "à" * 170 + "\\\" archivio"
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_mailboxes SET username=?,folder=? WHERE id=?", (self.server.username, folder, self.mailbox_id))
            database.commit()
        self.receiver()
        self.assertTrue(self.server.authentications[0][1])
        examined = next(command for command in self.server.commands if command.startswith("EXAMINE "))
        self.assertEqual(shlex.split(examined)[1], folder)
        self.assertEqual(self.rows("raw_messages")[0]["state"], "new")

    def test_spoofed_or_untrusted_auth_and_unauthorized_senders_are_quarantined(self):
        cases = [synthetic_message("other@example.test"),
                 synthetic_message(authentication="attacker.example; dmarc=pass header.from=example.test"),
                 synthetic_message(authentication=AUTHSERV + "; dmarc=fail header.from=example.test"),
                 synthetic_message(authentication=AUTHSERV + "; dmarc=pass header.from=other.test"),
                 synthetic_message().replace(b"From:", b"Authentication-Results: " + AUTHSERV.encode() + b"; dmarc=pass header.from=example.test\r\nFrom:", 1),
                 synthetic_message().replace(b"From:", b"From: attacker@example.test\r\nFrom:", 1)]
        self.server.messages = {index: raw for index, raw in enumerate(cases, 1)}
        self.receiver()
        messages = self.rows("raw_messages")
        self.assertEqual(len(messages), len(cases))
        self.assertTrue(all(item["state"] == "quarantine" for item in messages), messages)
        self.assertTrue(all(item["quarantine_reason"] for item in messages))
        self.assertEqual(self.rows("tickets"), [])

    def test_auth_results_limits_and_quoted_delimiters_cannot_hide_dmarc_failure(self):
        headers = [AUTHSERV + "; dmarc=pass header.from=example.test" + " " * 8800 + "; dmarc=fail header.from=example.test",
                   AUTHSERV + "; dmarc=pass header.from=example.test (" + "x" * 8800,
                   AUTHSERV + '; spf=fail reason="foo\\\"; dmarc=pass header.from=example.test;"; dmarc=fail header.from=example.test',
                   AUTHSERV + '; spf=fail reason=";" dmarc=pass header.from=example.test; dkim=pass header.d=example.test']
        self.server.messages = {index: synthetic_message(authentication=header) for index, header in enumerate(headers, 1)}
        self.receiver()
        messages = self.rows("raw_messages")
        self.assertEqual(len(messages), len(headers))
        self.assertTrue(all(item["state"] == "quarantine" and item["auth_status"] != "pass" for item in messages), messages)
        self.assertTrue(all(item["parse_error"] for item in messages[:2]))

    def test_expired_allowlist_and_conflicting_job_bindings_are_quarantined(self):
        self.server.messages = {1: synthetic_message()}
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_senders SET valid_until=?", (int(time.time()) - 1,))
            database.commit()
        self.receiver()
        self.assertEqual(self.rows("raw_messages")[0]["state"], "quarantine")
        self.server.messages[2] = synthetic_message(subject="Binding incompatibile")
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_senders SET valid_until=NULL,job_id=21")
            database.execute("UPDATE raw_mailboxes SET job_id=22")
            database.commit()
        self.receiver()
        self.assertEqual(self.rows("raw_messages")[1]["quarantine_reason"], "sender_mailbox_job_mismatch")

    def test_unsafe_secret_files_are_rejected_before_authentication(self):
        self.server.messages = {1: synthetic_message()}
        self.password_file.chmod(0o644)
        self.receiver(expected=1)
        self.password_file.chmod(0o600)
        link = self.secrets / "symlink-secret"
        link.symlink_to(self.password_file)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_mailboxes SET secret_name=?", (link.name,))
            database.commit()
        self.receiver(expected=1)
        self.assertEqual(self.server.authentications, [])
        self.assertEqual(self.rows("raw_messages"), [])

    def test_attachment_and_mime_structure_budgets_quarantine_without_losing_raw(self):
        from email.parser import BytesParser
        many = BytesParser(policy=policy.SMTP).parsebytes(synthetic_message())
        for number in range(21):
            many.add_attachment(b"part", maintype="application", subtype="octet-stream", filename="part-%d.bin" % number)
        nested = EmailMessage(policy=policy.SMTP)
        nested.set_content("Contenuto profondo")
        for _ in range(18):
            parent = EmailMessage(policy=policy.SMTP)
            parent.make_mixed()
            parent.attach(nested)
            nested = parent
        deep = BytesParser(policy=policy.SMTP).parsebytes(synthetic_message())
        deep.make_mixed()
        deep.attach(nested)
        cases = [many.as_bytes(), deep.as_bytes()]
        self.server.messages = {index: raw for index, raw in enumerate(cases, 1)}
        self.receiver()
        messages = self.rows("raw_messages")
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]["parse_error"], "attachment_limits_exceeded")
        self.assertEqual(messages[1]["parse_error"], "mime_structure_limits_exceeded")
        self.assertEqual(len(self.rows("raw_attachments")), 20)
        self.assertTrue(all(item["state"] == "quarantine" and item["raw_eml"] == raw for item, raw in zip(messages, cases)))

    def test_html_only_is_text_and_charset_and_body_limits_preserve_original(self):
        html = '<html><head><script>alert(1)</script></head><body><p>Caff&egrave; &amp; assistenza</p><img src="https://external.invalid/track"><a href="javascript:alert(1)">Apri</a></body></html>'
        html_raw = synthetic_message(body=html, html=True)
        large = synthetic_message(subject="Test lungo", body="x" * 70000)
        self.server.messages = {1: html_raw, 2: large}
        self.receiver()
        first, second = self.rows("raw_messages")
        self.assertNotIn("script", first["body_text"])
        self.assertNotIn("external.invalid", first["body_text"])
        self.assertNotIn("javascript:", first["body_text"])
        self.assertIn("assistenza", first["body_text"])
        self.assertLessEqual(len(second["body_text"].encode()), 65536)
        self.assertEqual(second["parse_error"], "body_text_truncated")
        self.assertEqual(second["state"], "quarantine")
        self.assertEqual(second["raw_eml"], large)

    def test_tls_hostname_and_ca_fail_before_authentication_or_ingest(self):
        self.server.messages = {1: synthetic_message()}
        self.receiver(expected=1, environment={"TICKETS_IMAP_CA_FILE": str(self.other_ca)})
        self.assertEqual(self.server.authentications, [])
        self.assertEqual(self.rows("raw_messages"), [])
        self.server.close()
        self.server = FakeIMAPSServer(self.wrong_cert, self.wrong_key, {1: synthetic_message()})
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_mailboxes SET port=? WHERE id=?", (self.server.port, self.mailbox_id))
            database.commit()
        self.receiver(expected=1, environment={"TICKETS_IMAP_CA_FILE": str(self.wrong_cert)})
        self.assertEqual(self.server.authentications, [])
        self.assertEqual(self.rows("raw_messages"), [])

    def test_xoauth2_and_explicit_connect_proxy_ignore_no_proxy(self):
        self.server.messages = {1: synthetic_message()}
        token = self.secrets / "synthetic-oauth-token"
        token.write_text(self.server.oauth_token + "\n")
        token.chmod(0o600)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE raw_mailboxes SET auth_mode='xoauth2',secret_name=? WHERE id=?", (token.name, self.mailbox_id))
            database.commit()
        proxy = calendar_fixture.ThreadedServer(("127.0.0.1", 0), calendar_fixture.ConnectHandler)
        proxy.target_port = self.server.port
        proxy.requests = []
        thread = threading.Thread(target=proxy.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 2)
        self.addCleanup(proxy.server_close)
        self.addCleanup(proxy.shutdown)
        self.receiver(environment={"TICKETS_IMAP_PROXY_URL": "http://127.0.0.1:%d" % proxy.server_address[1]})
        self.assertTrue(proxy.requests)
        self.assertEqual(self.server.authentications, [("XOAUTH2", True)])
        self.assertEqual(self.rows("raw_messages")[0]["state"], "new")


if __name__ == "__main__":
    unittest.main()
