"""The C mail bridge uses isolated TLS SMTP and CONNECT sinks; no real email is sent."""

from email import policy
from email.parser import BytesParser
import http.client
import json
import os
from pathlib import Path
import select
import shutil
import socket
import socketserver
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
TOKEN = "test-only-" + "a" * 64


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class ThreadedServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, request, client_address):
        # TLS rejection and intentionally dropped delivery acknowledgements are fixtures.
        pass


class SmtpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        connection = self.request
        connection.settimeout(5)
        encrypted = self.server.mode == "smtps"
        if encrypted:
            connection = self.server.context.wrap_socket(connection, server_side=True)
        reader = connection.makefile("rb")

        def reply(text):
            connection.sendall(text.encode("ascii") + b"\r\n")

        reply("220 fixture.invalid ESMTP")
        while True:
            line = reader.readline(10000)
            if not line:
                break
            command = line.split(b" ", 1)[0].strip().upper()
            self.server.commands.append(command)
            if command in (b"EHLO", b"HELO"):
                if not encrypted and self.server.starttls:
                    reply("250-fixture.invalid\r\n250 STARTTLS")
                else:
                    reply("250 fixture.invalid")
            elif command == b"STARTTLS":
                reply("220 Upgrade to TLS")
                reader.close()
                connection = self.server.context.wrap_socket(connection, server_side=True)
                reader = connection.makefile("rb")
                encrypted = True
            elif command == b"MAIL":
                if not encrypted:
                    self.server.plaintext_mail = True
                reply("250 Sender accepted")
            elif command == b"RCPT":
                reply("550 Rejected fixture recipient" if self.server.reject else "250 Recipient accepted")
            elif command == b"DATA":
                reply("354 End with dot")
                chunks = []
                while True:
                    chunk = reader.readline(10000)
                    if chunk in (b".\r\n", b".\n", b""):
                        break
                    chunks.append(chunk[1:] if chunk.startswith(b"..") else chunk)
                self.server.messages.append(b"".join(chunks))
                if self.server.drop_ack:
                    break
                reply("250 Accepted")
            elif command == b"QUIT":
                reply("221 Bye")
                break
            else:
                reply("250 OK")
        reader.close()
        connection.close()


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            part = self.request.recv(4096)
            if not part:
                return
            request += part
            if len(request) > 8192:
                return
        first = request.split(b"\r\n", 1)[0].decode("ascii")
        method, authority, version = first.split()
        self.server.requests.append((method, authority, version))
        host, port = authority.rsplit(":", 1)
        if method != "CONNECT" or host != "localhost" or int(port) != self.server.smtp_port:
            self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        with socket.create_connection(("127.0.0.1", int(port)), timeout=5) as remote:
            self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            peers = [self.request, remote]
            while True:
                readable, _, _ = select.select(peers, [], [], 5)
                if not readable:
                    return
                for source in readable:
                    try:
                        data = source.recv(32768)
                        if not data:
                            return
                        (remote if source is self.request else self.request).sendall(data)
                    except (ConnectionError, OSError):
                        return


class MailAddonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc") or not shutil.which("openssl"):
            raise unittest.SkipTest("C compiler and OpenSSL are needed")
        subprocess.run(["make", "-C", str(ROOT / "addon")], check=True, capture_output=True, text=True)
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-mail-certs-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert_dir = Path(cls.certificates.name)
        cls.ca = cls.cert_dir / "ca.crt"
        cls.ca_key = cls.cert_dir / "ca.key"
        cls.openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                    "-keyout", str(cls.ca_key), "-out", str(cls.ca), "-subj", "/CN=Punto mail test CA",
                    "-addext", "basicConstraints=critical,CA:TRUE",
                    "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        cls.cert, cls.key = cls.issue_certificate("valid", "DNS:localhost,IP:127.0.0.1")
        cls.wrong_cert, cls.wrong_key = cls.issue_certificate("wrong", "DNS:wrong.example.test")

    @classmethod
    def openssl(cls, *arguments):
        subprocess.run(["openssl", *arguments], check=True, capture_output=True)

    @classmethod
    def issue_certificate(cls, name, san):
        key, cert, csr = (cls.cert_dir / (name + suffix) for suffix in (".key", ".crt", ".csr"))
        extensions = cls.cert_dir / (name + ".ext")
        extensions.write_text("subjectAltName=" + san + "\nextendedKeyUsage=serverAuth\n"
                              "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n")
        cls.openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key),
                    "-out", str(csr), "-subj", "/CN=" + name)
        cls.openssl("x509", "-req", "-in", str(csr), "-CA", str(cls.ca), "-CAkey", str(cls.ca_key),
                    "-CAcreateserial", "-out", str(cert), "-days", "2", "-extfile", str(extensions))
        return cert, key

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="punto-mail-addon-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.token_file = self.path / "api-token"
        self.token_file.write_text(TOKEN + "\n")
        self.smtp_password = self.path / "smtp-password"
        self.smtp_password.write_text("")
        self.db = self.path / "delivery.sqlite3"
        self.process = None
        self.servers = []
        self.context = ssl.create_default_context(cafile=str(self.ca))
        self.addCleanup(self.cleanup_services)

    def cleanup_services(self):
        self.stop()
        for server in self.servers:
            server.shutdown()
            server.server_close()

    def stop(self):
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
            self.process = None
            self.log.close()

    def start(self, configured=False, **environment):
        self.stop()
        self.port = free_port()
        env = os.environ.copy()
        for name in list(env):
            if name.startswith("MAIL_"):
                del env[name]
        env.update(MAIL_BIND="127.0.0.1", MAIL_PORT=str(self.port), MAIL_ALLOWED_CLIENT="127.0.0.1",
                   MAIL_API_TOKEN_FILE=str(self.token_file), MAIL_TLS_CERT_FILE=str(self.cert),
                   MAIL_TLS_KEY_FILE=str(self.key), MAIL_LEDGER_PATH=str(self.db),
                   MAIL_SMTP_PASSWORD_FILE=str(self.smtp_password), MAIL_SMTP_TIMEOUT="3")
        if configured:
            env.update(MAIL_SMTP_HOST="localhost", MAIL_SMTP_PORT=str(self.smtp.server_address[1]),
                       MAIL_SMTP_FROM="punto@example.test", MAIL_SMTP_CA_FILE=str(self.ca),
                       MAIL_PROXY_URL="http://127.0.0.1:%d" % self.proxy.server_address[1],
                       MAIL_SMTP_MODE=self.smtp.mode,
                       NO_PROXY="*", no_proxy="*")  # Explicitly prove the proxy cannot be bypassed.
        env.update(environment)
        self.log = open(self.path / "service.log", "ab")
        self.process = subprocess.Popen([str(ROOT / "addon/build/punto-mailer")], env=env,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail("Mail addon did not start: " + (self.path / "service.log").read_text())
            try:
                if self.api("GET", "/healthz", token=None)[0] == 200:
                    return
            except (ConnectionError, OSError):
                time.sleep(0.02)
        self.fail("Mail addon was not ready")

    def sinks(self, mode="starttls", *, wrong_certificate=False, reject=False, drop_ack=False, starttls=True):
        self.smtp = ThreadedServer(("127.0.0.1", 0), SmtpHandler)
        self.smtp.mode, self.smtp.reject, self.smtp.drop_ack, self.smtp.starttls = mode, reject, drop_ack, starttls
        self.smtp.messages, self.smtp.commands, self.smtp.plaintext_mail = [], [], False
        self.smtp.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.smtp.context.load_cert_chain(str(self.wrong_cert if wrong_certificate else self.cert),
                                         str(self.wrong_key if wrong_certificate else self.key))
        self.proxy = ThreadedServer(("127.0.0.1", 0), ProxyHandler)
        self.proxy.smtp_port, self.proxy.requests = self.smtp.server_address[1], []
        for server in (self.smtp, self.proxy):
            threading.Thread(target=server.serve_forever, daemon=True).start()
            self.servers.append(server)

    def fields(self, **changes):
        result = {"idempotency_key": "punto-alerts-v1:outbox:42", "to": "recipient@example.test",
                  "subject": "Ore mancanti: attività di lunedì", "body": "Hai registrato 6 ore.\nNe mancano 2."}
        result.update(changes)
        return result

    def api(self, method="POST", path="/v1/emails", fields=None, *, token=TOKEN, body=None, content_type=None):
        headers = {}
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        if fields is not None:
            body = urlencode(fields)
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        if content_type:
            headers["Content-Type"] = content_type
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=6)
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def raw(self, payload):
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as connection:
            with self.context.wrap_socket(connection, server_hostname="localhost") as tls:
                tls.sendall(payload)
                response = b""
                while True:
                    chunk = tls.recv(32768)
                    if not chunk:
                        break
                    response += chunk
        return int(response.split(b" ", 2)[1])

    def row_count(self):
        with sqlite3.connect(self.db) as db:
            return db.execute("SELECT COUNT(*) FROM deliveries").fetchone()[0]

    def test_disabled_smtp_preserves_requests_for_later_configuration(self):
        self.start()
        status, data = self.api("GET", "/v1/status")
        self.assertEqual(status, 200)
        self.assertIs(data["smtp_configured"], False)
        for _ in range(2):
            status, data = self.api(fields=self.fields())
            self.assertEqual(status, 503)
            self.assertEqual(data["status"], "pending_configuration")
        self.assertEqual(self.row_count(), 0)

    def test_api_requires_bearer_token_but_health_does_not(self):
        self.start()
        self.assertEqual(self.api("GET", "/healthz", token=None)[0], 200)
        for token in (None, "wrong-token"):
            self.assertEqual(self.api("GET", "/v1/status", token=token)[0], 401)
            self.assertEqual(self.api(fields=self.fields(), token=token)[0], 401)
        self.assertEqual(self.row_count(), 0)

    def test_health_only_source_cannot_use_authenticated_api(self):
        self.start(MAIL_ALLOWED_CLIENT="172.30.60.10")
        self.assertEqual(self.api("GET", "/healthz", token=None)[0], 200)
        self.assertEqual(self.api("GET", "/v1/status")[0], 403)

    def test_starttls_delivery_is_tunneled_and_idempotent_after_restart(self):
        self.sinks()
        self.start(configured=True)
        self.assertTrue(self.api("GET", "/v1/status")[1]["smtp_configured"])
        status, data = self.api(fields=self.fields())
        self.assertEqual(status, 200, data)
        self.assertIs(data["replayed"], False)
        self.assertEqual(len(self.smtp.messages), 1)
        self.assertFalse(self.smtp.plaintext_mail)
        self.assertEqual(self.proxy.requests[0][0], "CONNECT")
        self.start(configured=True)
        status, data = self.api(fields=self.fields())
        self.assertEqual(status, 200, data)
        self.assertIs(data["replayed"], True)
        self.assertEqual(len(self.smtp.messages), 1)
        self.assertEqual(len(self.proxy.requests), 1)

    def test_smtps_and_italian_mime_encoding(self):
        self.sinks(mode="smtps")
        self.start(configured=True)
        subject = "Attività e qualità àèìòù " * 6
        body = "Attività, qualità e formazione.\nSeconda riga con àèìòù."
        status, data = self.api(fields=self.fields(subject=subject, body=body))
        self.assertEqual(status, 200, data)
        message = BytesParser(policy=policy.default).parsebytes(self.smtp.messages[0])
        self.assertEqual(str(message["Subject"]), subject)
        self.assertEqual(message.get_content(), body)
        self.assertLessEqual(max(map(len, self.smtp.messages[0].split(b"\r\n"))), 78)

    def test_same_key_different_payload_is_conflict(self):
        self.sinks()
        self.start(configured=True)
        self.assertEqual(self.api(fields=self.fields())[0], 200)
        status, data = self.api(fields=self.fields(body="Different body"))
        self.assertEqual((status, data["error"]), (409, "idempotency_conflict"))
        self.assertEqual(len(self.smtp.messages), 1)

    def test_ambiguous_delivery_is_never_resent(self):
        self.sinks(drop_ack=True)
        self.start(configured=True)
        for replayed in (False, True):
            status, data = self.api(fields=self.fields())
            self.assertEqual((status, data["status"]), (409, "unknown"))
            self.assertIs(data["replayed"], replayed)
        self.assertEqual(len(self.smtp.messages), 1)
        self.assertEqual(len(self.proxy.requests), 1)

    def test_known_smtp_rejection_is_persisted_without_resending(self):
        self.sinks(reject=True)
        self.start(configured=True)
        for replayed in (False, True):
            status, data = self.api(fields=self.fields())
            self.assertEqual((status, data["status"]), (502, "failed"))
            self.assertIs(data["replayed"], replayed)
        self.assertEqual(self.smtp.messages, [])
        self.assertEqual(len(self.proxy.requests), 1)

    def test_crashed_sending_record_recovers_as_unknown(self):
        self.sinks()
        self.start(configured=True)
        self.assertEqual(self.api(fields=self.fields())[0], 200)
        self.stop()
        with sqlite3.connect(self.db) as db:
            db.execute("UPDATE deliveries SET status='sending'")
        self.start(configured=True)
        status, data = self.api(fields=self.fields())
        self.assertEqual((status, data["status"]), (409, "unknown"))
        self.assertEqual(len(self.smtp.messages), 1)

    def test_smtp_hostname_verification_cannot_be_disabled(self):
        self.sinks(wrong_certificate=True)
        self.start(configured=True)
        status, data = self.api(fields=self.fields())
        self.assertEqual((status, data["status"]), (502, "failed"))
        self.assertEqual(self.smtp.messages, [])

    def test_smtp_untrusted_ca_is_rejected(self):
        self.sinks()
        self.start(configured=True, MAIL_SMTP_CA_FILE="")
        self.assertEqual(self.api(fields=self.fields())[0], 502)
        self.assertEqual(self.smtp.messages, [])

    def test_smtp_without_starttls_never_receives_mail(self):
        self.sinks(starttls=False)
        self.start(configured=True)
        self.assertEqual(self.api(fields=self.fields())[0], 502)
        self.assertNotIn(b"MAIL", self.smtp.commands)
        self.assertEqual(self.smtp.messages, [])

    def test_header_injection_duplicate_fields_and_invalid_addresses_rejected(self):
        self.start()
        for fields in (self.fields(subject="Hi\r\nBcc: hidden@example.test"),
                       self.fields(to="user@example.test\r\nRCPT TO:<other@example.test>"),
                       self.fields(to="a@example.test,b@example.test"), self.fields(to="a@-invalid.test"),
                       self.fields(idempotency_key="bad\"key"), self.fields(body="a\x00b"),
                       self.fields(subject=""), self.fields(body="x" * 8193)):
            with self.subTest(fields=fields):
                self.assertEqual(self.api(fields=fields)[0], 400)
        body = urlencode(self.fields()) + "&to=other%40example.test"
        self.assertEqual(self.api(body=body, content_type="application/x-www-form-urlencoded")[0], 400)
        for suffix in ("&unknown=1", "&body=%FF", "&body=%GG"):
            self.assertEqual(self.api(body=urlencode(self.fields()) + suffix,
                                      content_type="application/x-www-form-urlencoded")[0], 400)
        self.assertEqual(self.row_count(), 0)

    def test_http_framing_and_route_limits(self):
        self.start()
        self.assertEqual(self.api("GET", "/v1/emails")[0], 405)
        self.assertEqual(self.api("POST", "/unknown", fields={})[0], 404)
        self.assertEqual(self.api(body="{}", content_type="application/json")[0], 415)
        for headers in (b"Content-Length: 0\r\nContent-Length: 0\r\n",
                        b"Transfer-Encoding: chunked\r\n", b"Content-Length: 32769\r\n",
                        b"Content-Length: -1\r\n", b"Expect: 100-continue\r\n"):
            raw = b"POST /v1/emails HTTP/1.1\r\nHost: localhost\r\n" + headers + b"\r\n"
            self.assertEqual(self.raw(raw), 400)

    def test_api_certificate_is_checked_by_client(self):
        self.start()
        untrusted = ssl.create_default_context()
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=untrusted, timeout=3)
        with self.assertRaises(ssl.SSLCertVerificationError):
            connection.request("GET", "/healthz")
        connection.close()

    def test_ledger_contains_no_mail_content_or_credentials(self):
        self.sinks()
        self.start(configured=True)
        self.assertEqual(self.api(fields=self.fields())[0], 200)
        with sqlite3.connect(self.db) as db:
            columns = [row[1] for row in db.execute("PRAGMA table_info(deliveries)")]
            row = db.execute("SELECT * FROM deliveries").fetchone()
        self.assertEqual(columns, ["idempotency_key", "payload_hash", "status", "created_at", "updated_at"])
        self.assertEqual(len(row[1]), 64)
        self.assertEqual(row[2], "sent")
        self.stop()
        log = (self.path / "service.log").read_text()
        for private in (TOKEN, self.fields()["to"], self.fields()["subject"], self.fields()["body"]):
            self.assertNotIn(private, log)


if __name__ == "__main__":
    unittest.main()
