"""C mail client and durable alert outbox against an isolated HTTPS API.

No SMTP server or external destinations are used. Each test owns its TLS
listener, credentials, database, and synthetic recipient addresses.
"""

from contextlib import closing
from datetime import datetime
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs

import test_alerts as alerts
import test_roles as roles


MAIL_ENV = ("MAIL_API_URL", "MAIL_API_CA_FILE", "MAIL_API_TOKEN_FILE", "MAIL_API_NAMESPACE")
TOKEN = "7d" * 32
CLIENT_HARNESS = r'''
#include "mail_client.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (!mail_client_open()) { puts("invalid_configuration"); return 0; }
    printf("enabled=%d\n", mail_client_enabled());
    if (argc > 1 && strcmp(argv[1], "send") == 0) {
        mail_client_refresh(1800000000);
        printf("smtp_configured=%d\n", mail_client_smtp_configured());
        MailDelivery result = mail_client_send("test:outbox:123", "recipient@example.test",
            "Verifica + caffè & stato", "Riga uno\nRiga due: 8h = 480 minuti.");
        const char *states[] = {"sent", "unconfigured", "unknown", "failed", "retry"};
        printf("delivery=%s\n", states[result]);
    }
    mail_client_close();
    return 0;
}
'''


def create_certificate(directory, name, hostname="localhost"):
    openssl = shutil.which("openssl")
    if openssl is None:
        raise unittest.SkipTest("OpenSSL is required for isolated TLS fixtures")
    certificate = Path(directory) / (name + ".crt")
    key = Path(directory) / (name + ".key")
    san = "DNS:" + hostname
    if hostname == "localhost":
        san += ",IP:127.0.0.1"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=" + hostname, "-addext", "subjectAltName=" + san,
                    "-keyout", str(key), "-out", str(certificate)],
                   check=True, capture_output=True, text=True)
    key.chmod(0o600)
    certificate.chmod(0o644)
    return certificate, key


class FakeMailAPI:
    """Capture the service contract without sending a single message."""

    def __init__(self, certificate, key):
        self.smtp_configured = True
        self.status_code = 200
        self.status_body = None
        self.delivery_code = 200
        self.delivery_body = {"status": "sent", "replayed": False}
        self.delivery_delay = 0
        self.requests = []
        self.replies = []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, status, value, headers=None):
                payload = value if isinstance(value, bytes) else json.dumps(value).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                for name, content in (headers or {}).items():
                    self.send_header(name, content)
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass  # The timeout test intentionally closes the TLS client.

            def do_GET(self):
                fixture.requests.append({"method": "GET", "path": self.path,
                                         "authorization": self.headers.get("Authorization")})
                value = fixture.status_body if fixture.status_body is not None else {
                    "smtp_configured": fixture.smtp_configured, "transport": "smtp_tls", "idempotency": True}
                self.reply(fixture.status_code, value, {"Location": "/unexpected-redirect"})

            def do_POST(self):
                payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                request = {"method": "POST", "path": self.path,
                           "authorization": self.headers.get("Authorization"),
                           "content_type": self.headers.get("Content-Type"),
                           "fields": parse_qs(payload.decode("ascii"), keep_blank_values=True)}
                fixture.requests.append(request)
                if fixture.delivery_delay:
                    time.sleep(fixture.delivery_delay)
                if fixture.replies:
                    status, value = fixture.replies.pop(0)
                else:
                    status, value = fixture.delivery_code, fixture.delivery_body
                self.reply(status, value)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(certificate), str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = "https://127.0.0.1:%d" % self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def posts(self):
        return [request for request in self.requests if request["method"] == "POST"]


class MailClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="punto-mail-client-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.certificate, cls.key = create_certificate(cls.build.name, "api")
        cls.other_certificate, _ = create_certificate(cls.build.name, "other-ca")
        cls.wrong_hostname_certificate, cls.wrong_hostname_key = create_certificate(
            cls.build.name, "wrong-name", "wrong-api.invalid")
        source = Path(cls.build.name) / "client-check.c"
        source.write_text(CLIENT_HARNESS, encoding="utf-8")
        cls.binary = Path(cls.build.name) / "client-check"
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if compiler is None:
            raise unittest.SkipTest("C compiler unavailable")
        subprocess.run([compiler, "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
                        "-Wpedantic", "-Werror", "-I", str(roles.ROOT / "src"),
                        str(roles.ROOT / "src/mail_client.c"), str(source), "-lcurl", "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="punto-mail-client-case-")
        self.addCleanup(self.files.cleanup)
        self.token_file = Path(self.files.name) / "token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.api = FakeMailAPI(self.certificate, self.key)
        self.addCleanup(self.api.close)
        self.environment = {name: value for name, value in os.environ.items() if name not in MAIL_ENV}
        self.environment.update(MAIL_API_URL=self.api.url, MAIL_API_TOKEN_FILE=str(self.token_file),
                                MAIL_API_CA_FILE=str(self.certificate), MAIL_API_NAMESPACE="client-test-v1")

    def invoke(self, *, send=True, changes=None, absent=()):
        environment = dict(self.environment, **(changes or {}))
        for name in absent:
            environment.pop(name, None)
        result = subprocess.run([str(self.binary)] + (["send"] if send else []), env=environment,
                                check=True, capture_output=True, text=True, timeout=8)
        self.assertNotIn(TOKEN, result.stdout + result.stderr, "The service token leaked into process output")
        return result.stdout.strip()

    def test_disabled_configuration_is_valid_and_partial_configuration_fails_closed(self):
        self.assertEqual(self.invoke(send=False, absent=MAIL_ENV), "enabled=0")
        for name in MAIL_ENV:
            with self.subTest(missing=name):
                self.assertEqual(self.invoke(send=False, absent=(name,)), "invalid_configuration")
        self.assertEqual(self.api.requests, [])

    def test_untrusted_urls_and_malformed_tokens_fail_before_requests(self):
        for url in ("http://127.0.0.1:8090", "https://attacker.example:8090",
                    self.api.url + "/path", self.api.url + "?x=1", "https://user@localhost:8090"):
            with self.subTest(url=url):
                self.assertEqual(self.invoke(changes={"MAIL_API_URL": url}), "invalid_configuration")
        for token in ("too-short", "A" * 129, "A" * 31 + "\r\nInjected: yes", "A" * 32 + "\x00tail"):
            with self.subTest(token_length=len(token)):
                self.token_file.write_text(token, encoding="ascii")
                self.assertEqual(self.invoke(), "invalid_configuration")
        self.assertEqual(self.api.requests, [])

    def test_writable_secret_file_is_rejected(self):
        self.token_file.chmod(0o666)
        self.assertEqual(self.invoke(), "invalid_configuration")
        self.assertEqual(self.api.requests, [])

    def test_authenticated_tls_request_encodes_content_and_bypasses_external_proxy(self):
        result = self.invoke(changes={"HTTPS_PROXY": "http://127.0.0.1:1",
                                      "https_proxy": "http://127.0.0.1:1", "NO_PROXY": ""})
        self.assertIn("smtp_configured=1", result)
        self.assertIn("delivery=sent", result)
        self.assertEqual([row["path"] for row in self.api.requests], ["/v1/status", "/v1/emails"])
        for request in self.api.requests:
            self.assertEqual(request["authorization"], "Bearer " + TOKEN)
        request = self.api.posts()[0]
        self.assertEqual(request["content_type"], "application/x-www-form-urlencoded")
        self.assertEqual(request["fields"], {
            "idempotency_key": ["test:outbox:123"], "to": ["recipient@example.test"],
            "subject": ["Verifica + caffè & stato"], "body": ["Riga uno\nRiga due: 8h = 480 minuti."]})

    def test_smtp_unconfigured_does_not_submit_an_email(self):
        self.api.smtp_configured = False
        self.assertIn("delivery=unconfigured", self.invoke())
        self.assertEqual(self.api.posts(), [])

    def test_api_failures_malformed_status_and_redirects_cannot_enable_delivery(self):
        for status, body in ((401, {"error": "unauthorized"}), (503, {"error": "unavailable"}),
                             (302, {"smtp_configured": True}), (200, b"not-json"),
                             (200, {"smtp_configured": "true"})):
            with self.subTest(status=status, body=body):
                self.api.requests.clear()
                self.api.status_code, self.api.status_body = status, body
                self.assertIn("delivery=unconfigured", self.invoke())
                self.assertEqual([row["path"] for row in self.api.requests], ["/v1/status"])

    def test_untrusted_ca_and_certificate_hostname_prevent_delivery(self):
        self.assertIn("delivery=unconfigured", self.invoke(changes={"MAIL_API_CA_FILE": str(self.other_certificate)}))
        self.assertEqual(self.api.requests, [])
        self.api.close()
        self.api = FakeMailAPI(self.wrong_hostname_certificate, self.wrong_hostname_key)
        self.addCleanup(self.api.close)
        self.assertIn("delivery=unconfigured", self.invoke(changes={
            "MAIL_API_URL": self.api.url, "MAIL_API_CA_FILE": str(self.wrong_hostname_certificate)}))
        self.assertEqual(self.api.requests, [])

    def test_delivery_responses_distinguish_sent_pending_unknown_failed_and_retry(self):
        cases = ((200, {"status": "sent", "replayed": True}, "sent"),
                 (503, {"error": "smtp_not_configured", "status": "pending_configuration"}, "unconfigured"),
                 (409, {"error": "delivery_unknown", "status": "unknown"}, "unknown"),
                 (502, {"error": "smtp_delivery_failed", "status": "failed"}, "failed"),
                 (409, {"error": "idempotency_conflict"}, "failed"),
                 (401, {"error": "unauthorized"}, "retry"),
                 (503, {"error": "temporarily_unavailable"}, "retry"),
                 (200, b"{" + b"x" * 9000, "retry"))
        for status, body, expected in cases:
            with self.subTest(status=status, expected=expected):
                self.api.delivery_code, self.api.delivery_body = status, body
                self.assertIn("delivery=" + expected, self.invoke())

    def test_timeout_is_bounded_and_keeps_the_same_idempotency_key_for_retry(self):
        self.api.delivery_delay = 3.4
        started = time.monotonic()
        self.assertIn("delivery=retry", self.invoke())
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(len(self.api.posts()), 1)
        self.assertEqual(self.api.posts()[0]["fields"]["idempotency_key"], ["test:outbox:123"])


class MailOutboxIntegrationTests(unittest.TestCase):
    # Reuse fixture methods without inheriting and rerunning the alert suite.
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    time_fields = roles.RoleIntegrationTests.time_fields
    rule_fields = alerts.AlertIntegrationTests.rule_fields
    create_rule = alerts.AlertIntegrationTests.create_rule
    run_rule = alerts.AlertIntegrationTests.run_rule
    share_team = alerts.AlertIntegrationTests.share_team
    worker = alerts.AlertIntegrationTests.worker
    record_invalid_attempt = alerts.AlertIntegrationTests.record_invalid_attempt

    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="punto-mail-outbox-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.certificate, cls.key = create_certificate(cls.build.name, "api")
        cls.native_check = alerts.build_native_check(cls.build.name)

    def setUp(self):
        self.mail_api = FakeMailAPI(self.certificate, self.key)
        self.mail_api.smtp_configured = False
        self.addCleanup(self.mail_api.close)
        self.token_file = Path(self.build.name) / "outbox-token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.mail_environment = {
            "MAIL_API_URL": self.mail_api.url, "MAIL_API_TOKEN_FILE": str(self.token_file),
            "MAIL_API_CA_FILE": str(self.certificate), "MAIL_API_NAMESPACE": "outbox-test-v1"}
        with patch.dict(os.environ, self.mail_environment):
            roles.RoleIntegrationTests.setUp(self)
        # Time alerts may email only an active account authorized for the subject.
        user_id, worker = self.worker(email="recipient@example.test")
        self.rule_id = self.create_rule(recipient_user_ids=str(user_id), recipient_emails="recipient@example.test")
        self.record_invalid_attempt(worker)
        self.assertEqual(self.run_rule(self.rule_id)["emails_queued"], 1)
        self.stop_server()

    def tick(self, local_time="2035-03-24T09:00:00"):
        epoch = int(datetime.fromisoformat(local_time).replace(tzinfo=alerts.ROME).timestamp())
        environment = dict(os.environ, **self.mail_environment)
        result = subprocess.run([str(self.native_check), "tick", str(self.db_path), str(epoch)],
                                env=environment, check=True, capture_output=True, text=True, timeout=10)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)

    def outbox(self):
        with closing(sqlite3.connect(self.db_path)) as db:
            return db.execute("SELECT id,email,status FROM alert_email_outbox ORDER BY id").fetchall()

    def test_unconfigured_service_preserves_pending_outbox_without_submitting(self):
        self.tick()
        self.assertEqual([(email, status) for _, email, status in self.outbox()],
                         [("recipient@example.test", "pending_configuration")])
        self.assertEqual(self.mail_api.posts(), [])

    def test_success_is_durable_and_repeated_tick_does_not_resend(self):
        self.mail_api.smtp_configured = True
        self.tick()
        self.assertEqual(self.outbox()[0][2], "sent")
        self.tick("2035-03-25T09:00:00")
        self.assertEqual(len(self.mail_api.posts()), 1)
        self.assertEqual(self.mail_api.posts()[0]["fields"]["idempotency_key"],
                         ["outbox-test-v1:outbox:" + str(self.outbox()[0][0])])

    def test_temporary_failure_retries_persisted_item_with_identical_payload_and_key(self):
        self.mail_api.smtp_configured = True
        self.mail_api.replies = [(503, {"error": "temporarily_unavailable"}),
                                (200, {"status": "sent", "replayed": False})]
        self.tick()
        self.assertEqual(self.outbox()[0][2], "retrying")
        self.tick("2035-03-25T09:00:00")
        self.assertEqual(self.outbox()[0][2], "sent")
        self.assertEqual(len(self.mail_api.posts()), 2)
        self.assertEqual(self.mail_api.posts()[0]["fields"], self.mail_api.posts()[1]["fields"])

    def test_ambiguous_and_definite_delivery_failures_are_terminal(self):
        self.mail_api.smtp_configured = True
        for status, error, expected in ((409, "delivery_unknown", "delivery_unknown"),
                                         (502, "smtp_delivery_failed", "delivery_failed")):
            with self.subTest(error=error):
                with closing(sqlite3.connect(self.db_path)) as db:
                    db.execute("UPDATE alert_email_outbox SET status='pending_configuration'")
                    db.commit()
                self.mail_api.delivery_code = status
                self.mail_api.delivery_body = {"error": error}
                before = len(self.mail_api.posts())
                self.tick()
                self.assertEqual(self.outbox()[0][2], expected)
                self.tick("2035-03-25T09:00:00")
                self.assertEqual(len(self.mail_api.posts()), before + 1)

    def test_service_losing_smtp_configuration_keeps_existing_item_pending(self):
        self.mail_api.smtp_configured = True
        self.mail_api.delivery_code = 503
        self.mail_api.delivery_body = {"error": "smtp_not_configured", "status": "pending_configuration"}
        self.tick()
        self.assertEqual(self.outbox()[0][2], "pending_configuration")
        self.assertEqual(len(self.mail_api.posts()), 1)

    def test_revoked_email_recipient_and_disabled_rule_owner_are_not_delivered(self):
        self.mail_api.smtp_configured = True
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DELETE FROM alert_rule_emails WHERE rule_id=?", (self.rule_id,))
            db.commit()
        self.tick()
        self.assertEqual(self.mail_api.posts(), [])
        self.assertEqual(self.outbox()[0][2], "pending_configuration")
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("INSERT INTO alert_rule_emails(rule_id,email) VALUES(?,?)",
                       (self.rule_id, "recipient@example.test"))
            db.execute("UPDATE users SET active=0 WHERE id=(SELECT owner_id FROM alert_rules WHERE id=?)",
                       (self.rule_id,))
            db.commit()
        self.tick()
        self.assertEqual(self.mail_api.posts(), [])
        self.assertEqual(self.outbox()[0][2], "pending_configuration")

    def test_delivery_audit_failure_recovers_unknown_without_resending(self):
        self.mail_api.smtp_configured = True
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER reject_email_audit BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='alert_emails.delivery' BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            db.commit()
        self.tick()
        self.assertEqual(self.outbox()[0][2], "pending_configuration")
        self.assertEqual(len(self.mail_api.posts()), 1)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='alert_emails.delivery'").fetchone(), (0,))
            self.assertEqual(db.execute("SELECT state FROM ii_email_controls").fetchone(), ("sending",))
            db.execute("DROP TRIGGER reject_email_audit")
            db.commit()
        # SMTP may already have accepted the message while the local audit
        # transaction rolled back. Restarting preserves the uncertainty and
        # requires provider review instead of submitting another delivery.
        self.mail_api.delivery_body = {"status": "sent", "replayed": True}
        self.tick("2035-03-25T09:00:00")
        self.assertEqual(self.outbox()[0][2], "delivery_unknown")
        self.assertEqual(len(self.mail_api.posts()), 1)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='alert_emails.delivery'").fetchone(), (0,))
            self.assertEqual(db.execute("SELECT state FROM ii_email_controls").fetchone(), ("normal",))

    def test_schema_six_upgrade_preserves_outbox_ids_sequence_business_records_and_audit(self):
        with closing(sqlite3.connect(self.db_path)) as db:
            # Rebuild only the old outbox shape. All other genuine records and
            # existing primary keys are kept from the HTTP fixture.
            ddl = db.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name='alert_email_outbox'").fetchone()[0]
            ddl = re.sub(r"CHECK\s*\(status\s+IN\s*\([^)]*\)\)",
                         "CHECK(status='pending_configuration')", ddl, flags=re.IGNORECASE)
            old_rows = db.execute("SELECT * FROM alert_email_outbox ORDER BY id").fetchall()
            db.execute("DROP TABLE alert_email_outbox")
            db.execute(ddl)
            db.executemany("INSERT INTO alert_email_outbox VALUES(" + ",".join("?" for _ in old_rows[0]) + ")", old_rows)
            # Preserve a high-water ID beyond current rows; a migration may
            # never reuse an idempotency key from a formerly deleted item.
            db.execute("UPDATE sqlite_sequence SET seq=77 WHERE name='alert_email_outbox'")
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' "
                                 "AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (7,8,9,10,12,13,14)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=6")
            db.commit()
            # Compare the columns present in schema 6; schemas 9/10 add alert thresholds.
            projections = {"alert_rules": "id,owner_id,name,type,run_time,enabled,created_at,updated_at,"
                                          "last_run_at,last_run_on,last_event_id"}
            old = {table: db.execute('SELECT ' + projections.get(table, '*') + ' FROM "' + table + '" ORDER BY rowid').fetchall()
                   for table in ("users", "jobs", "time_entries", "alert_rules", "alert_rule_emails",
                                 "alert_email_outbox", "alert_notifications")}
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
            with self.assertRaises(sqlite3.IntegrityError):
                db.execute("UPDATE alert_email_outbox SET status='sent'")
        self.tick()
        with closing(sqlite3.connect(self.db_path)) as db:
            # The native scheduler intentionally does not perform migrations.
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (6,))
        with patch.dict(os.environ, self.mail_environment):
            self.start_server()
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (14,))
            for table, rows in old.items():
                self.assertEqual(db.execute('SELECT ' + projections.get(table, '*') + ' FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            self.assertEqual(db.execute("SELECT seq FROM sqlite_sequence WHERE name='alert_email_outbox'").fetchone(), (77,))
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE NOT(action='system.migration' "
                                        "AND entity_id IN (7,8,9,10,12,13,14)) ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' "
                                        "AND entity_id=7").fetchone(), (1,))
            db.execute("UPDATE alert_email_outbox SET status='retrying'")
            db.commit()
            final_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        with patch.dict(os.environ, self.mail_environment):
            self.start_server()
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT * FROM audit_events ORDER BY id").fetchall(), final_audit)
            self.assertEqual(self.outbox()[0][2], "retrying")


if __name__ == "__main__":
    unittest.main()
