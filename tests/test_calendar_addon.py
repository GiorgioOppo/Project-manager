"""Isolated Google OAuth/Calendar HTTPS and CONNECT fixtures; no Google account is used."""

from contextlib import closing
import base64
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import select
import shlex
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
from urllib.parse import parse_qs, unquote, urlencode, urlsplit

from test_mail_client import create_certificate

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "calendar-test-" + "a" * 64
BINDING = "1a" * 32
OTHER_BINDING = "2b" * 32


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def event(identifier="remote-event", summary="Evento privato caffè", etag='"etag-1"'):
    return {"id": identifier, "etag": etag, "summary": summary,
            "description": "Appuntamento di collaudo riservato",
            "start": {"dateTime": "2030-01-02T09:00:00+01:00"},
            "end": {"dateTime": "2030-01-02T10:00:00+01:00"}}


class ThreadedServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, *_arguments):
        pass  # Deliberately rejected TLS peers are fixtures.


class ConnectHandler(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = self.request.recv(4096)
            if not chunk or len(request) > 8192:
                return
            request += chunk
        try:
            method, authority, _version = request.split(b"\r\n", 1)[0].decode("ascii").split()
            host, port = authority.rsplit(":", 1)
            port = int(port)
        except (UnicodeError, ValueError):
            return
        self.server.requests.append((method, authority))
        if method != "CONNECT" or host not in ("localhost", "127.0.0.1") or port != self.server.target_port:
            self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        with socket.create_connection(("127.0.0.1", port), timeout=5) as remote:
            self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            peers = [self.request, remote]
            while True:
                readable, _, _ = select.select(peers, [], [], 5)
                if not readable:
                    return
                for source in readable:
                    try:
                        chunk = source.recv(32768)
                        if not chunk:
                            return
                        (remote if source is self.request else self.request).sendall(chunk)
                    except (ConnectionError, OSError):
                        return


class FakeGoogle:
    """Small deterministic subset of the real Google REST contract."""

    def __init__(self, certificate, key):
        self.requests = []
        self.events = [event()]
        self.list_responses = []
        self.conflict = False
        self.token_expires = 3600
        self.events_delay = 0
        self.events_started = None
        self.events_release = None
        self.events_replied = None
        self.token_status = 200
        self.token_body = None
        self.mutations = []
        self.drop_mutation_ack = False
        self.tokens = []
        self.primary_calendar_id = "primary@example.test"
        self.additional_calendar_ids = set()
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def reply(self, status, value):
                payload = value if isinstance(value, bytes) else json.dumps(value).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass

            def capture(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                parsed = urlsplit(self.path)
                record = {"method": self.command, "path": unquote(parsed.path),
                          "query": parse_qs(parsed.query), "body": body,
                          "authorization": self.headers.get("Authorization"),
                          "if_match": self.headers.get("If-Match")}
                fixture.requests.append(record)
                return record

            def do_GET(self):
                record = self.capture()
                if record["path"] == "/calendar/v3/users/me/calendarList/primary":
                    return self.reply(200, {"id": fixture.primary_calendar_id, "primary": True, "accessRole": "owner"})
                if record["path"].startswith("/calendar/v3/users/me/calendarList/"):
                    identifier = record["path"].rsplit("/", 1)[1]
                    if identifier not in ("primary@example.test", "readonly@example.test") and identifier not in fixture.additional_calendar_ids:
                        return self.reply(404, {"error": {"code": 404}})
                    return self.reply(200, {"id": identifier, "summary": "Calendario",
                                           "accessRole": "reader" if identifier.startswith("readonly") else "owner"})
                if record["path"] == "/calendar/v3/users/me/calendarList":
                    items = [
                        {"id": "primary@example.test", "summary": "Calendario personale", "accessRole": "owner", "primary": True,
                         "access_token": "private-calendar-item-token"},
                        {"id": "readonly@example.test", "summary": "Lettura", "accessRole": "reader"}]
                    if record["query"].get("pageToken") == ["calendar-page-two"]:
                        return self.reply(200, {"items": items[1:]})
                    maximum = int(record["query"].get("maxResults", ["50"])[0])
                    value = {"items": items[:maximum]}
                    if maximum == 1:
                        value["nextPageToken"] = "calendar-page-two"
                    return self.reply(200, value)
                if record["path"].endswith("/events"):
                    if fixture.events_started is not None:
                        fixture.events_started.set()
                    if fixture.events_release is not None:
                        fixture.events_release.wait(8)
                    if fixture.events_delay:
                        time.sleep(fixture.events_delay)
                    if fixture.list_responses:
                        status, value = fixture.list_responses.pop(0)
                        return self.reply(status, value)
                    self.reply(200, {"items": fixture.events, "nextSyncToken": "sync-complete-1"})
                    if fixture.events_replied is not None:
                        fixture.events_replied.set()
                    return
                if "/events/" in record["path"]:
                    identifier = record["path"].rsplit("/", 1)[1]
                    for value in fixture.events:
                        if value["id"] == identifier:
                            return self.reply(200, value)
                    return self.reply(404, {"error": {"code": 404}})
                self.reply(404, {"error": {"code": 404}})

            def do_POST(self):
                record = self.capture()
                if record["path"] == "/token":
                    fields = parse_qs(record["body"].decode("ascii"))
                    fixture.tokens.append(fields)
                    code = fields.get("code", ["refresh"])[0]
                    body = fixture.token_body if fixture.token_body is not None else {
                        "access_token": "secret-access-" + code,
                        "refresh_token": "secret-refresh-" + code,
                        "expires_in": fixture.token_expires,
                        "token_type": "Bearer", "scope": "https://www.googleapis.com/auth/calendar.events https://www.googleapis.com/auth/calendar.calendarlist.readonly"}
                    return self.reply(fixture.token_status, body)
                if record["path"] == "/revoke":
                    return self.reply(200, {})
                self.mutate(record)

            def do_PATCH(self):
                self.mutate(self.capture())

            def do_PUT(self):
                self.mutate(self.capture())

            def do_DELETE(self):
                record = self.capture()
                fixture.mutations.append(record)
                if fixture.conflict:
                    return self.reply(412, {"error": {"code": 412, "message": "Precondition failed"}})
                identifier = record["path"].rsplit("/", 1)[1]
                fixture.events[:] = [row for row in fixture.events if row["id"] != identifier]
                self.reply(204, b"")

            def mutate(self, record):
                fixture.mutations.append(record)
                if fixture.conflict:
                    return self.reply(412, {"error": {"code": 412, "message": "Precondition failed"}})
                try:
                    value = json.loads(record["body"])
                except (ValueError, UnicodeError):
                    return self.reply(400, {"error": {"code": 400}})
                value.setdefault("id", record["path"].rsplit("/", 1)[1] if self.command != "POST" else "created-event")
                value["etag"] = '"etag-written"'
                fixture.events[:] = [row for row in fixture.events if row["id"] != value["id"]] + [value]
                if fixture.drop_mutation_ack:
                    fixture.drop_mutation_ack = False
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                self.reply(200, value)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(certificate), str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.origin = "https://127.0.0.1:%d" % self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class CalendarAddonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc") or not shutil.which("openssl"):
            raise unittest.SkipTest("C compiler and OpenSSL are required")
        subprocess.run(["make", "-C", str(ROOT / "calendar-addon")], check=True, capture_output=True, text=True)
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-calendar-certs-")
        cls.addClassCleanup(cls.certificates.cleanup)
        # Produce real schema-one AES-GCM ciphertext, using the historical AAD.
        helper = Path(cls.certificates.name) / "legacy_payload.c"
        helper.write_text('#define main calendar_service_main\n#include "' + str(ROOT / "calendar-addon/src/calendar.c") + '"\n#undef main\n' + r'''
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    memset(config.encryption_key, 0xab, sizeof(config.encryption_key));
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 3;
    long size = ftell(file);
    if (size < 29 || size > RESPONSE_LIMIT || fseek(file, 0, SEEK_SET)) return 4;
    void *input = malloc((size_t)size);
    if (!input || fread(input, 1, (size_t)size, file) != (size_t)size) return 5;
    fclose(file);
    char *plain = unseal(1, argv[2], input, (int)size);
    free(input);
    if (!plain) return 6;
    int length = 0;
    char *output = seal(1, argv[3], plain, &length);
    OPENSSL_cleanse(plain, strlen(plain)); free(plain);
    if (!output) return 7;
    file = fopen(argv[1], "wb");
    bool ok = file && fwrite(output, 1, (size_t)length, file) == (size_t)length;
    if (file) fclose(file);
    free(output);
    return ok ? 0 : 8;
}
''', encoding="utf-8")
        cls.legacy_helper = Path(cls.certificates.name) / "legacy-payload"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "json-c", "openssl"], text=True))
        subprocess.run(["cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", str(helper), "-o", str(cls.legacy_helper),
                        *flags, "-lcurl", "-lsqlite3", "-pthread"], check=True, capture_output=True, text=True)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "calendar")
        cls.other_ca, _ = create_certificate(cls.certificates.name, "other-ca")
        cls.wrong_cert, cls.wrong_key = create_certificate(cls.certificates.name, "wrong-name", "wrong-calendar.invalid")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="punto-calendar-addon-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.token_file = self.path / "api-token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.key_file = self.path / "token-key"
        self.key_file.write_text("ab" * 32 + "\n", encoding="ascii")
        self.secret_file = self.path / "google-secret"
        self.secret_file.write_text("synthetic-google-client-secret\n", encoding="ascii")
        for file in (self.token_file, self.key_file, self.secret_file):
            file.chmod(0o600)
        self.db = self.path / "calendar.sqlite3"
        self.process = None
        self.google = FakeGoogle(self.cert, self.key)
        self.proxy = ThreadedServer(("127.0.0.1", 0), ConnectHandler)
        self.proxy.target_port = self.google.server.server_port
        self.proxy.requests = []
        self.proxy_thread = threading.Thread(target=self.proxy.serve_forever, daemon=True)
        self.proxy_thread.start()
        self.context = ssl.create_default_context(cafile=str(self.cert))
        self.addCleanup(self.cleanup_services)

    def cleanup_services(self):
        if self.google.events_release is not None:
            self.google.events_release.set()
        self.stop()
        self.proxy.shutdown()
        self.proxy.server_close()
        self.proxy_thread.join(timeout=2)
        self.google.close()

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

    def start(self, configured=True, **changes):
        self.stop()
        self.port = free_port()
        env = {name: value for name, value in os.environ.items()
               if not name.startswith(("CALENDAR_", "GOOGLE_"))}
        env.update(CALENDAR_BIND="127.0.0.1", CALENDAR_ALLOWED_CLIENT="127.0.0.1", CALENDAR_PORT=str(self.port),
                   CALENDAR_TLS_CERT_FILE=str(self.cert), CALENDAR_TLS_KEY_FILE=str(self.key),
                   CALENDAR_API_TOKEN_FILE=str(self.token_file), CALENDAR_TOKEN_KEY_FILE=str(self.key_file),
                   CALENDAR_DB_PATH=str(self.db), CALENDAR_TEST_MODE="1",
                   CALENDAR_GOOGLE_ORIGIN=self.google.origin, CALENDAR_GOOGLE_CA_FILE=str(self.cert),
                   CALENDAR_PROXY_URL="http://127.0.0.1:%d" % self.proxy.server_address[1],
                   CALENDAR_SYNC_INTERVAL="3600", CALENDAR_HTTP_TIMEOUT="3", NO_PROXY="*", no_proxy="*")
        if configured:
            env.update(GOOGLE_CLIENT_ID="synthetic-client.apps.googleusercontent.com",
                       GOOGLE_CLIENT_SECRET_FILE=str(self.secret_file),
                       GOOGLE_REDIRECT_URI="http://localhost:8080/api/calendar/oauth/callback")
        env.update(changes)
        self.log = open(self.path / "service.log", "ab")
        self.process = subprocess.Popen([str(ROOT / "calendar-addon/build/punto-calendar")], env=env,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail("Calendar service exited during startup: " + (self.path / "service.log").read_text())
            try:
                status, _ = self.request("GET", "/healthz", authenticated=False)
                if status == 200:
                    return
            except (OSError, http.client.HTTPException):
                time.sleep(0.025)
        self.fail("Calendar service did not become healthy")

    def request(self, method, path, fields=None, *, authenticated=True, token=TOKEN):
        headers = {"Authorization": "Bearer " + token} if authenticated else {}
        body = None
        if fields is not None:
            body = urlencode(fields)
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            status = response.status
        finally:
            connection.close()
        value = json.loads(payload)
        for secret in (TOKEN, "synthetic-google-client-secret", "secret-access-", "secret-refresh-"):
            self.assertNotIn(secret.encode("ascii"), payload, "API leaked a credential")
        return status, value

    def user(self, method, operation, fields=None, *, uid=1):
        return self.request(method, "/v1/users/%d/%s" % (uid, operation), fields)

    def authorize(self, uid=1, code="user-one", binding=BINDING):
        status, value = self.user("POST", "oauth/start", {"session_binding": binding}, uid=uid)
        self.assertEqual(status, 200, value)
        authorization = urlsplit(value["authorization_url"])
        parameters = parse_qs(authorization.query)
        self.assertEqual(parameters.get("code_challenge_method"), ["S256"])
        self.assertEqual(parameters.get("access_type"), ["offline"])
        self.assertTrue(parameters.get("state"))
        callback = {"state": parameters["state"][0], "code": code, "session_binding": binding}
        status, value = self.user("POST", "oauth/callback", callback, uid=uid)
        self.assertEqual(status, 200, value)
        token = self.google.tokens[-1]
        challenge = base64.urlsafe_b64encode(hashlib.sha256(token["code_verifier"][0].encode("ascii")).digest()).rstrip(b"=").decode("ascii")
        self.assertEqual(parameters["code_challenge"], [challenge])
        return callback

    def select(self, uid=1):
        status, value = self.user("POST", "selection", {"calendar_id": "primary@example.test"}, uid=uid)
        self.assertEqual(status, 200, value)

    def connect(self):
        self.authorize()
        self.select()

    def synchronize(self, *, expected=200, uid=1):
        status, value = self.user("POST", "sync", {}, uid=uid)
        self.assertEqual(status, 202, value)
        self.assertIs(value["sync_requested"], True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            status, value = self.user("GET", "status", uid=uid)
            self.assertEqual(status, 200, value)
            if not value.get("sync_requested") and not value.get("sync_running"):
                if expected == 200:
                    self.assertEqual(value["last_error"], "", value)
                    self.assertGreater(value["last_sync"], 0, value)
                else:
                    self.assertEqual(expected, 502)
                    self.assertEqual(value["last_error"], "event_sync_failed", value)
                return value
            time.sleep(0.025)
        self.fail("Background calendar synchronization did not finish: " + json.dumps(value))

    def cached_events(self, uid=1):
        status, value = self.user("GET", "events", uid=uid)
        self.assertEqual(status, 200, value)
        return value.get("events", value.get("items", []))

    def test_unconfigured_is_healthy_authenticated_and_never_contacts_google(self):
        self.start(configured=False)
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)
        self.assertEqual(self.request("GET", "/v1/status", authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", "/v1/status", token="wrong-token")[0], 401)
        status, value = self.request("GET", "/v1/status")
        self.assertEqual(status, 200, value)
        self.assertIs(value["oauth_configured"], False)
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 503, value)
        self.assertEqual(self.google.requests, [])
        self.assertEqual(self.proxy.requests, [])

    def test_oauth_pkce_single_use_session_binding_and_per_user_isolation(self):
        self.start()
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 200, value)
        state = parse_qs(urlsplit(value["authorization_url"]).query)["state"][0]
        fields = {"state": state, "code": "user-one", "session_binding": OTHER_BINDING}
        self.assertEqual(self.user("POST", "oauth/callback", fields)[0], 400)
        fields["session_binding"] = BINDING
        self.assertEqual(self.user("POST", "oauth/callback", fields, uid=2)[0], 400)
        self.assertEqual(self.user("POST", "oauth/callback", fields)[0], 200)
        self.assertEqual(self.user("POST", "oauth/callback", fields)[0], 400)
        self.assertEqual(len(self.google.tokens), 1)
        status, value = self.user("GET", "status", uid=2)
        self.assertEqual(status, 200, value)
        self.assertIs(value["connected"], False)
        self.assertEqual(self.user("GET", "events", uid=2)[0], 409)
        self.assertTrue(self.proxy.requests, "NO_PROXY=* bypassed the mandatory CONNECT proxy")
        self.assertTrue(all(method == "CONNECT" for method, _ in self.proxy.requests))

    def test_wrong_encryption_key_fails_startup_without_resetting_accounts(self):
        self.start()
        self.authorize()
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            before = database.execute("SELECT user_id,token_blob FROM accounts").fetchall()
        self.key_file.write_text("cd" * 32 + "\n", encoding="ascii")
        with self.assertRaisesRegex(self.failureException, "Calendar encrypted store unavailable"):
            self.start()
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT user_id,token_blob FROM accounts").fetchall(), before)
        self.assertEqual(len(self.google.tokens), 1)

    def test_expired_oauth_state_is_rejected_without_token_exchange(self):
        self.start()
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 200, value)
        state = parse_qs(urlsplit(value["authorization_url"]).query)["state"][0]
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE oauth_states SET expires=0")
            database.commit()
        self.assertEqual(self.user("POST", "oauth/callback", {
            "state": state, "code": "expired-code", "session_binding": BINDING})[0], 400)
        self.assertEqual(self.google.tokens, [])
        self.assertNotIn(state.encode("ascii"), self.db.read_bytes(), "OAuth state was stored as plaintext")

    def test_refresh_token_is_used_for_expired_access_and_never_exposed(self):
        self.start()
        self.google.token_expires = 1
        self.authorize()
        self.google.token_expires = 3600
        self.select()
        self.synchronize()
        refreshes = [fields for fields in self.google.tokens if fields.get("grant_type") == ["refresh_token"]]
        self.assertTrue(refreshes, "An expired access token was not refreshed")
        self.assertEqual(refreshes[0].get("refresh_token"), ["secret-refresh-user-one"])
        self.assertTrue(self.proxy.requests)
        self.assertEqual(self.cached_events()[0]["id"], "remote-event")

    def test_private_source_acl_does_not_accept_a_valid_token_from_another_client(self):
        self.start(configured=False, CALENDAR_ALLOWED_CLIENT="127.0.0.2")
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)
        self.assertEqual(self.request("GET", "/v1/status")[0], 403)
        self.assertEqual(self.google.requests, [])

    def test_tokens_and_event_contents_are_encrypted_and_survive_restart(self):
        self.start()
        self.connect()
        self.synchronize()
        self.assertEqual([row["id"] for row in self.cached_events()], ["remote-event"])
        self.stop()
        disk = b"".join(file.read_bytes() for file in self.path.glob("calendar.sqlite3*"))
        for plaintext in (b"secret-access-user-one", b"secret-refresh-user-one",
                          "Evento privato caffè".encode(), b"Appuntamento di collaudo riservato"):
            self.assertNotIn(plaintext, disk)
        self.start()
        self.assertIs(self.user("GET", "status")[1]["connected"], True)
        self.assertEqual(self.cached_events()[0]["summary"], "Evento privato caffè")

    def test_full_incremental_pagination_commits_atomically_and_410_recovers(self):
        self.start()
        self.connect()
        self.google.list_responses = [(200, {"items": [event("first")], "nextPageToken": "page-two"}),
                                      (200, {"items": [event("second")], "nextSyncToken": "sync-two"})]
        self.synchronize()
        self.assertEqual({row["id"] for row in self.cached_events()}, {"first", "second"})
        requests = [row for row in self.google.requests if row["method"] == "GET" and row["path"].endswith("/events")]
        self.assertTrue(any(row["query"].get("pageToken") == ["page-two"] for row in requests))
        self.google.list_responses = [(200, {"items": [event("partial")], "nextPageToken": "bad-page"}),
                                      (500, {"error": {"code": 500}})]
        self.synchronize(expected=502)
        self.assertEqual({row["id"] for row in self.cached_events()}, {"first", "second"})
        self.google.list_responses = [(200, {"items": [{"id": "first", "status": "cancelled"}, event("second", "Aggiornato")],
                                            "nextSyncToken": "sync-three"})]
        self.synchronize()
        self.assertEqual({row["id"]: row.get("status", "confirmed") for row in self.cached_events()}, {"first": "cancelled", "second": "confirmed"})
        self.assertEqual(next(row for row in self.cached_events() if row["id"] == "second")["summary"], "Aggiornato")
        requests = [row for row in self.google.requests if row["method"] == "GET" and row["path"].endswith("/events")]
        self.assertTrue(any(row["query"].get("syncToken") == ["sync-two"] for row in requests))
        self.google.list_responses = [(410, {"error": {"code": 410}}),
                                      (200, {"items": [event("replacement")], "nextSyncToken": "sync-four"})]
        self.synchronize()
        self.assertEqual([row["id"] for row in self.cached_events()], ["replacement"])

    def test_outbound_create_update_delete_use_durable_idempotency_and_etags(self):
        self.start()
        self.connect()
        payload = event(summary="Creato dall'app")
        for name in ("id", "etag"):
            payload.pop(name)
        fields = {"idempotency_key": "create-1", "event": json.dumps(payload)}
        status, value = self.user("POST", "events/create", fields)
        self.assertIn(status, (200, 202), value)
        self.synchronize()
        creates = [row for row in self.google.mutations if row["method"] == "POST"]
        self.assertEqual(len(creates), 1)
        identifier = json.loads(creates[0]["body"])["id"]
        # Re-selecting a calendar clears only its cache. Applied operation
        # history must survive so the same key can never create a second event.
        self.select()
        self.assertEqual(self.user("GET", "changes")[1]["items"][0]["state"], "applied")
        self.stop()
        self.start()
        status, value = self.user("POST", "events/create", fields)
        self.assertEqual(status, 200, value)
        self.synchronize()
        self.assertEqual(len([row for row in self.google.mutations if row["method"] == "POST"]), 1)
        changed = dict(fields, event=json.dumps(dict(payload, summary="Payload diverso")))
        self.assertEqual(self.user("POST", "events/create", changed)[0], 409)
        update = {"idempotency_key": "update-1", "event_id": identifier, "etag": '"etag-written"',
                  "event": json.dumps(dict(payload, summary="Modificato dall'app"))}
        self.assertIn(self.user("POST", "events/update", update)[0], (200, 202))
        self.synchronize()
        updates = [row for row in self.google.mutations if row["method"] in ("PUT", "PATCH")]
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0]["if_match"], '"etag-written"')
        self.assertIn(self.user("POST", "events/delete", {
            "idempotency_key": "delete-1", "event_id": identifier, "etag": '"etag-written"'})[0], (200, 202))
        self.synchronize()
        deletes = [row for row in self.google.mutations if row["method"] == "DELETE"]
        self.assertEqual(len(deletes), 1)
        self.assertEqual(deletes[0]["if_match"], '"etag-written"')
        self.assertNotIn(identifier, [row["id"] for row in self.cached_events()])

    def test_exact_cached_event_lookup_is_private_and_does_not_contact_provider(self):
        self.start()
        self.connect()
        self.google.events[0]["extendedProperties"] = {"private": {"hidden": "private-provider-property"}}
        self.synchronize()
        self.authorize(uid=2, code="second-user", binding=OTHER_BINDING)
        self.select(uid=2)
        before = len(self.google.requests)
        status, value = self.user("GET", "event?event_id=remote-event")
        self.assertEqual(status, 200, value)
        self.assertEqual(value["calendar_id"], "primary@example.test")
        self.assertEqual(value["event"]["id"], "remote-event")
        self.assertEqual(value["event"]["etag"], '"etag-1"')
        self.assertNotIn("extendedProperties", value["event"])
        self.assertEqual(self.user("GET", "event?event_id=remote-event", uid=2)[0], 404)
        self.assertEqual(self.user("GET", "event?event_id=missing")[0], 404)
        self.assertEqual(len(self.google.requests), before, "Exact cache reads must not perform provider requests")

    def test_exact_change_lookup_preserves_ids_and_state_across_replay_and_restart(self):
        self.start()
        self.connect()
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        first = {"idempotency_key": "ticket:42:create-first", "event": json.dumps(payload)}
        status, queued = self.user("POST", "events/create", first)
        self.assertEqual(status, 202, queued)
        self.assertEqual(queued["calendar_id"], "primary@example.test")
        self.assertTrue(queued["event_id"])
        self.user("POST", "events/create", {"idempotency_key": "ticket:43:create-second", "event": json.dumps(dict(payload, summary="Secondo"))})
        self.synchronize()
        lookup = "change?" + urlencode({"idempotency_key": first["idempotency_key"]})
        status, exact = self.user("GET", lookup)
        self.assertEqual(status, 200, exact)
        self.assertEqual((exact["id"], exact["event_id"], exact["idempotency_key"]),
                         (queued["id"], queued["event_id"], first["idempotency_key"]))
        self.assertEqual(exact["state"], "applied")
        self.assertEqual(exact["calendar_id"], "primary@example.test")
        self.assertEqual(self.user("GET", lookup, uid=2)[0], 404)
        self.assertEqual(self.user("GET", "change?idempotency_key=missing-key")[0], 404)
        self.stop()
        self.start()
        self.assertEqual(self.user("GET", lookup)[1], exact)
        status, replay = self.user("POST", "events/create", first)
        self.assertEqual(status, 200, replay)
        self.assertEqual(replay["event_id"], queued["event_id"])
        self.assertEqual(replay["calendar_id"], "primary@example.test")
        self.synchronize()
        self.assertEqual(len([item for item in self.google.mutations if item["method"] == "POST"]), 2)

    def test_exact_lookup_queries_reject_identity_duplicates_and_invalid_references(self):
        self.start()
        before = len(self.google.requests)
        for query in ("event", "event?event_id=", "event?event_id=a&event_id=b", "event?event_id=a&user_id=2",
                      "event?event_id=bad%2Fid", "event?event_id=a&", "change", "change?idempotency_key=short",
                      "change?idempotency_key=valid-key&%69dempotency_key=other-key",
                      "change?idempotency_key=valid-key&user_id=2", "change?idempotency_key=bad%2Fkey"):
            with self.subTest(query=query):
                self.assertEqual(self.user("GET", query)[0], 400)
        self.assertEqual(len(self.google.requests), before)

    def test_operation_calendar_identity_survives_calendar_reselection(self):
        self.start()
        self.connect()
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        fields = {"idempotency_key": "ticket:42:original-calendar", "event": json.dumps(payload)}
        queued = self.user("POST", "events/create", fields)[1]
        self.synchronize()
        self.google.additional_calendar_ids.add("alternate@example.test")
        status, selected = self.user("POST", "selection", {"calendar_id": "alternate@example.test"})
        self.assertEqual(status, 200, selected)
        self.assertEqual(self.user("GET", "status")[1]["calendar_id"], "alternate@example.test")
        query = "change?" + urlencode({"idempotency_key": fields["idempotency_key"]})
        exact = self.user("GET", query)[1]
        self.assertEqual(exact["calendar_id"], "primary@example.test")
        self.assertEqual(exact["event_id"], queued["event_id"])
        replay = self.user("POST", "events/create", fields)[1]
        self.assertEqual(replay["calendar_id"], "primary@example.test")
        self.assertEqual(len([item for item in self.google.mutations if item["method"] == "POST"]), 1)

    def test_connection_identity_survives_restart_reauthorization_and_selection_but_changes_after_disconnect(self):
        self.start()
        self.assertIsNone(self.user("GET", "status")[1]["connection_id"])
        self.connect()
        connection = self.user("GET", "status")[1]["connection_id"]
        self.assertRegex(connection, r"\A[0-9a-f]{64}\Z")
        self.authorize()
        self.assertEqual(self.user("GET", "status")[1]["connection_id"], connection)
        self.select()
        self.assertEqual(self.user("GET", "status")[1]["connection_id"], connection)
        self.stop()
        self.start()
        self.assertEqual(self.user("GET", "status")[1]["connection_id"], connection)
        self.assertEqual(self.user("POST", "disconnect", {})[0], 200)
        self.assertIsNone(self.user("GET", "status")[1]["connection_id"])
        self.connect()
        replacement = self.user("GET", "status")[1]["connection_id"]
        self.assertRegex(replacement, r"\A[0-9a-f]{64}\Z")
        self.assertNotEqual(replacement, connection)

    def test_schema_one_ledger_migration_preserves_rows_and_only_backfills_pending_targets(self):
        self.start()
        self.connect()
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        states = ("queued", "unknown", "applied", "conflict", "failed")
        for state in states:
            self.assertEqual(self.user("POST", "events/create", {"idempotency_key": "legacy-key-" + state,
                                                                 "event": json.dumps(dict(payload, summary=state))})[0], 202)
        self.synchronize()
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            for state in states:
                database.execute("UPDATE changes SET state=?,next_attempt=? WHERE idempotency_key=?",
                                 (state, int(time.time()) + 86400, "legacy-key-" + state))
            for state in ("queued", "unknown"):
                key = "legacy-key-" + state
                payload_file = self.path / (state + ".ciphertext")
                payload_file.write_bytes(database.execute("SELECT payload FROM changes WHERE idempotency_key=?", (key,)).fetchone()[0])
                purpose = "change-v2:" + key + ":" + hashlib.sha256(b"primary@example.test").hexdigest()
                subprocess.run([str(self.legacy_helper), str(payload_file), purpose, "change:" + key], check=True)
                database.execute("UPDATE changes SET payload=? WHERE idempotency_key=?", (payload_file.read_bytes(), key))
            # Rebuild the actual schema-one table, rather than resetting only its version.
            database.executescript("""
                CREATE TABLE legacy_changes(id INTEGER PRIMARY KEY AUTOINCREMENT,user_id INTEGER NOT NULL REFERENCES accounts(user_id) ON DELETE CASCADE,idempotency_key TEXT NOT NULL,payload_hash TEXT NOT NULL,operation TEXT NOT NULL,event_id TEXT NOT NULL,etag TEXT NOT NULL,payload BLOB NOT NULL,state TEXT NOT NULL CHECK(state IN ('queued','applied','conflict','failed','unknown')),response BLOB,error TEXT NOT NULL DEFAULT '',attempts INTEGER NOT NULL DEFAULT 0,next_attempt INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL,UNIQUE(user_id,idempotency_key));
                INSERT INTO legacy_changes SELECT id,user_id,idempotency_key,payload_hash,operation,event_id,etag,payload,state,response,error,attempts,next_attempt,created FROM changes;
                DROP TABLE changes;
                ALTER TABLE legacy_changes RENAME TO changes;
                CREATE INDEX changes_ready ON changes(user_id,state,next_attempt,id);
                PRAGMA user_version=1;
            """)
            database.execute("ALTER TABLE accounts DROP COLUMN connection_id")
            original_accounts = database.execute("SELECT * FROM accounts ORDER BY user_id").fetchall()
            original_changes = database.execute("SELECT * FROM changes ORDER BY id").fetchall()
            original_events = database.execute("SELECT * FROM events ORDER BY user_id,event_id").fetchall()
            database.commit()
        connection = None
        for _restart in range(2):
            self.start()
            current_connection = self.user("GET", "status")[1]["connection_id"]
            self.assertRegex(current_connection, r"\A[0-9a-f]{64}\Z")
            if connection is not None:
                self.assertEqual(current_connection, connection)
            connection = current_connection
            for state in states:
                status, value = self.user("GET", "change?idempotency_key=legacy-key-" + state)
                self.assertEqual(status, 200, value)
                self.assertEqual(value["state"], state)
                self.assertEqual(value["calendar_id"], "primary@example.test" if state in ("queued", "unknown") else "")
            self.stop()
            with closing(sqlite3.connect(self.db)) as database:
                self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 2)
                self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])
                self.assertEqual(database.execute("SELECT user_id,token_blob,calendar_id,sync_token,last_sync,last_error,reauth_required,next_poll,sync_running,provider_id,generation FROM accounts ORDER BY user_id").fetchall(), original_accounts)
                migrated = database.execute("SELECT id,user_id,idempotency_key,payload_hash,operation,event_id,etag,payload,state,response,error,attempts,next_attempt,created FROM changes ORDER BY id").fetchall()
                self.assertEqual(migrated, original_changes,
                                 "Schema migration must preserve ciphertext with an unauthenticated legacy calendar")
                self.assertEqual(database.execute("SELECT * FROM events ORDER BY user_id,event_id").fetchall(), original_events)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE changes SET next_attempt=0 WHERE state IN ('queued','unknown')")
            database.commit()
        before = len(self.google.mutations)
        self.start()
        self.synchronize()
        for state in ("queued", "unknown"):
            status, value = self.user("GET", "change?idempotency_key=legacy-key-" + state)
            self.assertEqual(status, 200, value)
            self.assertEqual(value["state"], "failed", value)
            self.assertEqual(value["error"], "queue_integrity_failed", value)
        self.assertEqual(len(self.google.mutations), before,
                         "Legacy changes without an authenticated calendar reached the provider")

    def test_guarded_mutations_reject_changed_connection_or_calendar_before_enqueue_or_replay(self):
        self.start()
        self.connect()
        current = self.user("GET", "status")[1]
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        fields = {"idempotency_key": "guarded-create-key", "event": json.dumps(payload),
                  "expected_connection_id": current["connection_id"], "expected_calendar_id": current["calendar_id"]}
        status, queued = self.user("POST", "events/create", fields)
        self.assertEqual(status, 202, queued)
        self.assertEqual(self.user("POST", "events/create", fields)[0], 200)
        self.synchronize()
        self.google.additional_calendar_ids.add("replacement@example.test")
        self.assertEqual(self.user("POST", "selection", {"calendar_id": "replacement@example.test"})[0], 200)
        self.assertEqual(self.user("GET", "status")[1]["connection_id"], current["connection_id"])
        before = len(self.google.mutations)
        for changes in ({}, {"idempotency_key": "guarded-new-key"}):
            status, value = self.user("POST", "events/create", {**fields, **changes})
            self.assertEqual(status, 409, value)
            self.assertEqual(value["error"], "calendar_target_changed")
        self.assertEqual(self.user("GET", "change?idempotency_key=guarded-new-key")[0], 404)
        self.assertEqual(self.user("POST", "disconnect", {})[0], 200)
        self.connect()
        self.assertNotEqual(self.user("GET", "status")[1]["connection_id"], current["connection_id"])
        self.assertEqual(self.user("POST", "events/create", fields)[0], 409)
        self.assertEqual(self.user("GET", "change?idempotency_key=guarded-create-key")[0], 404)
        for guard, value in (("expected_connection_id", ""), ("expected_connection_id", "z" * 64),
                             ("expected_calendar_id", "")):
            self.assertEqual(self.user("POST", "events/create", {**fields, guard: value})[0], 400)
        self.assertEqual(len(self.google.mutations), before)

    def test_etag_conflict_is_reported_without_overwriting_google(self):
        self.start()
        self.connect()
        self.synchronize()
        self.google.conflict = True
        fields = {"idempotency_key": "conflict-1", "event_id": "remote-event", "etag": '"stale-etag"',
                  "event": json.dumps({name: value for name, value in event().items() if name not in ("id", "etag")})}
        self.assertIn(self.user("POST", "events/update", fields)[0], (200, 202))
        self.synchronize()
        status, value = self.user("GET", "changes")
        self.assertEqual(status, 200, value)
        self.assertIn("conflict", json.dumps(value))
        attempts = len(self.google.mutations)
        self.synchronize()
        self.assertEqual(len(self.google.mutations), attempts, "A terminal ETag conflict was retried")
        self.assertEqual(self.google.events[0]["summary"], "Evento privato caffè")

    def test_tampered_queue_metadata_fails_before_delivery_or_reconciliation(self):
        self.start(CALENDAR_HTTP_TIMEOUT="10")
        self.connect()
        # Hold the sole worker after its initial flush so queued rows can be
        # changed deterministically before any delivery or reconciliation.
        self.google.events_started = threading.Event()
        self.google.events_release = threading.Event()
        self.assertEqual(self.user("POST", "sync", {})[0], 202)
        self.assertTrue(self.google.events_started.wait(8))
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        for state in ("queued", "unknown"):
            for operation in ("create", "update", "delete"):
                for column, replacement in (("operation", "update" if operation == "delete" else "delete"),
                                            ("event_id", "altered-target"), ("etag", '"altered-etag"')):
                    key = "tampered-%s-%s-%s" % (state, operation, column)
                    fields = {"idempotency_key": key}
                    if operation != "create":
                        fields.update(event_id="remote-event", etag='"etag-1"')
                    if operation != "delete":
                        fields["event"] = json.dumps(payload)
                    status, value = self.user("POST", "events/" + operation, fields)
                    self.assertEqual(status, 202, value)
                    with closing(sqlite3.connect(self.db)) as database:
                        database.execute("UPDATE changes SET " + column + "=?,state=? WHERE idempotency_key=?",
                                         (replacement, state, key))
                        database.commit()
            # Reproduce the full audit exploit: rewrite a create into a delete
            # of an existing event while retaining its authentic ciphertext.
            key = "tampered-compound-" + state
            self.assertEqual(self.user("POST", "events/create", {
                "idempotency_key": key, "event": json.dumps(payload)})[0], 202)
            with closing(sqlite3.connect(self.db)) as database:
                database.execute("UPDATE changes SET operation='delete',event_id='remote-event',"
                                 "etag='\"etag-1\"',state=? WHERE idempotency_key=?", (state, key))
                database.commit()
        self.google.events_release.set()
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(len(changes), 20)
        for change in changes:
            with self.subTest(key=change["idempotency_key"]):
                self.assertEqual(change["state"], "failed", change)
                self.assertEqual(change["error"], "queue_integrity_failed", change)
                self.assertEqual(change["attempts"], 0, change)
        self.assertEqual(self.google.mutations, [], "Tampered queue metadata reached Google")
        self.assertFalse(any("/events/" in row["path"] for row in self.google.requests),
                         "Tampered metadata was used for reconciliation")
        self.assertEqual([row["id"] for row in self.google.events], ["remote-event"])

    def test_tampered_queue_calendar_binding_fails_before_delivery_or_reconciliation(self):
        self.start(CALENDAR_HTTP_TIMEOUT="10")
        self.connect()
        self.google.events_started = threading.Event()
        self.google.events_release = threading.Event()
        self.assertEqual(self.user("POST", "sync", {})[0], 202)
        self.assertTrue(self.google.events_started.wait(8))
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        for state in ("queued", "unknown"):
            for operation in ("create", "update", "delete"):
                key = "calendar-tamper-%s-%s" % (state, operation)
                fields = {"idempotency_key": key}
                if operation != "create":
                    fields.update(event_id="remote-event", etag='"etag-1"')
                if operation != "delete":
                    fields["event"] = json.dumps(payload)
                self.assertEqual(self.user("POST", "events/" + operation, fields)[0], 202)
                with closing(sqlite3.connect(self.db)) as database:
                    database.execute("UPDATE changes SET state=? WHERE idempotency_key=?", (state, key))
                    database.commit()
            key = "account-only-tamper-" + state
            self.assertEqual(self.user("POST", "events/delete", {
                "idempotency_key": key, "event_id": "remote-event", "etag": '"etag-1"'})[0], 202)
            with closing(sqlite3.connect(self.db)) as database:
                database.execute("UPDATE changes SET state=? WHERE idempotency_key=?", (state, key))
                database.commit()
        with closing(sqlite3.connect(self.db)) as database:
            # Retarget both plaintext copies: comparing these columns alone
            # cannot authenticate the calendar originally chosen at enqueue.
            database.execute("UPDATE accounts SET calendar_id='altered-calendar@example.test'")
            database.execute("UPDATE changes SET calendar_id='altered-calendar@example.test' "
                             "WHERE idempotency_key LIKE 'calendar-tamper-%'")
            database.commit()
        self.google.events_release.set()
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(len(changes), 8)
        for change in changes:
            with self.subTest(key=change["idempotency_key"]):
                self.assertEqual(change["state"], "failed", change)
                expected_error = ("calendar_target_changed" if change["idempotency_key"].startswith("account-only-")
                                  else "queue_integrity_failed")
                self.assertEqual(change["error"], expected_error, change)
                self.assertEqual(change["attempts"], 0, change)
        self.assertEqual(self.google.mutations, [])
        self.assertFalse(any("/events/" in row["path"] for row in self.google.requests),
                         "An altered calendar was used for reconciliation")
        self.assertEqual([row["id"] for row in self.google.events], ["remote-event"])

    def test_legacy_queue_without_authenticated_calendar_binding_fails_closed(self):
        self.start(CALENDAR_HTTP_TIMEOUT="10")
        self.connect()
        self.google.events_started = threading.Event()
        self.google.events_release = threading.Event()
        self.assertEqual(self.user("POST", "sync", {})[0], 202)
        self.assertTrue(self.google.events_started.wait(8))
        # Genuine v1 AES-256-GCM delete envelopes: the fixture key is ab * 32,
        # nonce 42 * 12, and AAD is punto-calendar:v1:1:change:<key>.
        legacy = {
            "queued": "AUJCQkJCQkJCQkJCQuMSfYaEqs/fCC0mRPiENPt143gunUyWINkQBPyiP2I1bxbTQZQ/dNuPisR9bTKEA3PRGUwOqxcIV+HQIkG5FZlZmR/BIuGSjEgjoivHQLqOreQJjVgb5ZUhHimt3JNz",
            "unknown": "AUJCQkJCQkJCQkJCQup0xckQ53jGIHpfEEXRi7Z143gunUyWINkQBPyiP2I1bxbTQZQ/dNuPisR9bTKEA3PRGUwOqxcIV+HQIkG5FZlZmR/BIuGSjEgjoivHQLqOreQJjVgb5ZUhHimt3JNz",
        }
        for state, encoded in legacy.items():
            key = "legacy-change-" + state
            self.assertEqual(self.user("POST", "events/delete", {
                "idempotency_key": key, "event_id": "remote-event", "etag": '"etag-1"'})[0], 202)
            with closing(sqlite3.connect(self.db)) as database:
                database.execute("UPDATE changes SET payload=?,state=? WHERE idempotency_key=?",
                                 (base64.b64decode(encoded), state, key))
                database.commit()
        self.google.events_release.set()
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(len(changes), 2)
        for change in changes:
            self.assertEqual(change["state"], "failed", change)
            self.assertEqual(change["error"], "queue_integrity_failed", change)
            self.assertEqual(change["attempts"], 0, change)
        self.assertEqual(self.google.mutations, [])
        self.assertFalse(any("/events/" in row["path"] for row in self.google.requests))
        self.assertEqual([row["id"] for row in self.google.events], ["remote-event"])

    def test_authentic_unknown_queue_operations_reconcile_and_deliver(self):
        self.start(CALENDAR_HTTP_TIMEOUT="10")
        self.google.events.append(event("remote-delete"))
        self.connect()
        self.google.events_started = threading.Event()
        self.google.events_release = threading.Event()
        self.assertEqual(self.user("POST", "sync", {})[0], 202)
        self.assertTrue(self.google.events_started.wait(8))
        payload = {name: value for name, value in event().items() if name not in ("id", "etag")}
        identifiers = set()
        for operation, identifier in (("create", ""), ("update", "remote-event"), ("delete", "remote-delete")):
            fields = {"idempotency_key": "authentic-unknown-" + operation}
            if operation != "create":
                fields.update(event_id=identifier, etag='"etag-1"')
            if operation != "delete":
                fields["event"] = json.dumps(payload)
            status, value = self.user("POST", "events/" + operation, fields)
            self.assertEqual(status, 202, value)
            identifiers.add(value["event_id"])
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE changes SET state='unknown'")
            database.commit()
        self.google.events_release.set()
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(len(changes), 3)
        self.assertTrue(all(change["state"] == "applied" for change in changes), changes)
        self.assertEqual([row["method"] for row in self.google.mutations], ["POST", "PATCH", "DELETE"])
        for mutation, identifier in zip(self.google.mutations[1:], ("remote-event", "remote-delete")):
            self.assertTrue(mutation["path"].endswith("/events/" + identifier), mutation)
            self.assertEqual(mutation["if_match"], '"etag-1"')
        reconciled = {row["path"].rsplit("/", 1)[1] for row in self.google.requests
                      if row["method"] == "GET" and "/events/" in row["path"]}
        self.assertEqual(reconciled, identifiers)
        self.assertNotIn("remote-delete", [row["id"] for row in self.google.events])

    def test_readonly_calendar_and_arbitrary_event_fields_are_rejected(self):
        self.start()
        self.authorize()
        self.assertEqual(self.user("POST", "selection", {"calendar_id": "readonly@example.test"})[0], 403)
        self.select()
        duplicate = '{"summary":"one","summary":"two","start":{"date":"2030-01-02"},"end":{"date":"2030-01-03"}}'
        for payload in ("not-json", duplicate,
                        json.dumps(dict(event(), attendees=[{"email": "third-party@example.test"}]))):
            with self.subTest(payload=payload):
                self.assertEqual(self.user("POST", "events/create", {"idempotency_key": "invalid-1", "event": payload})[0], 400)
        self.assertEqual(self.google.mutations, [])

    def test_google_tls_failures_never_exchange_tokens_or_bypass_proxy(self):
        self.start(CALENDAR_GOOGLE_CA_FILE=str(self.other_ca))
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 200, value)
        state = parse_qs(urlsplit(value["authorization_url"]).query)["state"][0]
        status, value = self.user("POST", "oauth/callback", {"state": state, "code": "user-one", "session_binding": BINDING})
        self.assertEqual(status, 502, value)
        self.assertEqual(self.google.tokens, [])
        self.assertTrue(self.proxy.requests)

    def test_google_hostname_is_verified_even_when_its_certificate_is_trusted(self):
        self.google.close()
        self.google = FakeGoogle(self.wrong_cert, self.wrong_key)
        self.proxy.target_port = self.google.server.server_port
        self.start(CALENDAR_GOOGLE_CA_FILE=str(self.wrong_cert))
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 200, value)
        state = parse_qs(urlsplit(value["authorization_url"]).query)["state"][0]
        status, value = self.user("POST", "oauth/callback", {"state": state, "code": "user-one", "session_binding": BINDING})
        self.assertEqual(status, 502, value)
        self.assertEqual(self.google.tokens, [])
        self.assertTrue(self.proxy.requests)

    def test_user_caches_and_disconnect_are_scoped_to_the_account(self):
        self.start()
        self.connect()
        self.google.events = [event(summary="Appuntamento utente uno")]
        self.synchronize()
        self.authorize(uid=2, code="user-two", binding=OTHER_BINDING)
        self.select(uid=2)
        self.google.events = [event(summary="Appuntamento utente due")]
        self.synchronize(uid=2)
        self.assertEqual(self.cached_events(uid=1)[0]["summary"], "Appuntamento utente uno")
        self.assertEqual(self.cached_events(uid=2)[0]["summary"], "Appuntamento utente due")
        status, value = self.user("POST", "disconnect", {}, uid=1)
        self.assertEqual(status, 200, value)
        self.assertIs(value["disconnected"], True)
        self.assertIs(self.user("GET", "status", uid=1)[1]["connected"], False)
        self.assertIs(self.user("GET", "status", uid=2)[1]["connected"], True)
        self.assertEqual(self.cached_events(uid=2)[0]["summary"], "Appuntamento utente due")
        revoked = [row for row in self.google.requests if row["path"] == "/revoke"]
        self.assertEqual(len(revoked), 1)
        self.assertEqual(parse_qs(revoked[0]["body"].decode("ascii"))["token"], ["secret-refresh-user-one"])

    def test_background_sync_keeps_private_status_and_health_responsive(self):
        self.start()
        self.connect()
        self.google.events_delay = 2
        started = time.monotonic()
        status, value = self.user("POST", "sync", {})
        self.assertEqual(status, 202, value)
        self.assertLess(time.monotonic() - started, 1, "POSTsync blocked on Google")
        started = time.monotonic()
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)
        self.assertEqual(self.user("GET", "status")[0], 200)
        self.assertLess(time.monotonic() - started, 1, "Provider I/O blocked the private API")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            value = self.user("GET", "status")[1]
            if not value["sync_requested"] and not value["sync_running"]:
                self.assertEqual(value["last_error"], "")
                break
            time.sleep(0.025)
        else:
            self.fail("Background sync did not finish")
        self.assertEqual(self.cached_events()[0]["id"], "remote-event")

    def test_calendar_pages_are_bounded_and_strip_provider_credentials(self):
        self.start()
        self.authorize()
        status, value = self.user("GET", "calendars?limit=1")
        self.assertEqual(status, 200, value)
        self.assertEqual(len(value["items"]), 1)
        self.assertEqual(value["items"][0]["id"], "primary@example.test")
        self.assertNotIn("access_token", value["items"][0])
        self.assertEqual(value["next_page_token"], "calendar-page-two")
        status, value = self.user("GET", "calendars?" + urlencode({"page_token": value["next_page_token"], "limit": "1"}))
        self.assertEqual(status, 200, value)
        self.assertEqual([row["id"] for row in value["items"]], ["readonly@example.test"])
        self.assertFalse(value.get("next_page_token"))
        requests = [row for row in self.google.requests if row["path"] == "/calendar/v3/users/me/calendarList"]
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[-1]["query"].get("pageToken"), ["calendar-page-two"])
        for query in ("limit=0", "limit=51", "page_token=%00bad", "user_id=2"):
            self.assertEqual(self.user("GET", "calendars?" + query)[0], 400)

    def test_cached_event_pages_and_dates_are_bounded_and_repeatable(self):
        self.start()
        self.connect()
        self.google.events = [event("event-%02d" % index, "Evento %d" % index) for index in range(5)]
        self.synchronize()
        status, value = self.user("GET", "events?" + urlencode({"limit": "2", "from": "2030-01-02", "to": "2030-01-02"}))
        self.assertEqual(status, 200, value)
        self.assertEqual([row["id"] for row in value["items"]], ["event-00", "event-01"])
        self.assertEqual(value["next_after_id"], "event-01")
        status, value = self.user("GET", "events?" + urlencode({"limit": "2", "after_id": "event-01"}))
        self.assertEqual(status, 200, value)
        self.assertEqual([row["id"] for row in value["items"]], ["event-02", "event-03"])
        status, value = self.user("GET", "events?" + urlencode({"limit": "2", "after_id": "event-03"}))
        self.assertEqual(status, 200, value)
        self.assertEqual([row["id"] for row in value["items"]], ["event-04"])
        self.assertIsNone(value["next_after_id"])
        for query in ({"limit": "101"}, {"limit": "0"}, {"from": "2030-01-02"},
                      {"from": "2030-02-30", "to": "2030-03-01"},
                      {"from": "2030-01-03", "to": "2030-01-02"}):
            self.assertEqual(self.user("GET", "events?" + urlencode(query))[0], 400)

    def test_encrypted_tokens_cannot_be_transplanted_to_another_user(self):
        self.start()
        self.authorize()
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("INSERT INTO accounts(user_id,token_blob) SELECT 2,token_blob FROM accounts WHERE user_id=1")
            database.commit()
        before = len(self.google.requests)
        status, value = self.user("GET", "calendars", uid=2)
        self.assertEqual(status, 502, value)
        self.assertEqual(len(self.google.requests), before, "A transplanted user token was used against Google")
        self.assertEqual(self.user("GET", "calendars", uid=1)[0], 200)

    def test_reauthorization_preserves_selected_calendar_cache_and_change_history(self):
        self.start()
        self.connect()
        self.synchronize()
        payload = {name: value for name, value in event(summary="Cambio in coda").items() if name not in ("id", "etag")}
        status, queued = self.user("POST", "events/create", {"idempotency_key": "reauth-change-1", "event": json.dumps(payload)})
        self.assertEqual(status, 202, queued)
        self.authorize(code="reauthorized")
        status, value = self.user("GET", "status")
        self.assertEqual(status, 200, value)
        self.assertEqual(value["calendar_id"], "primary@example.test")
        self.assertEqual(self.cached_events()[0]["id"], "remote-event")
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(changes[0]["idempotency_key"], "reauth-change-1")
        status, replay = self.user("POST", "events/create", {"idempotency_key": "reauth-change-1", "event": json.dumps(payload)})
        self.assertEqual(status, 200, replay)
        self.assertIs(replay["replayed"], True)
        self.google.primary_calendar_id = "other-account@example.test"
        status, value = self.user("POST", "oauth/start", {"session_binding": BINDING})
        self.assertEqual(status, 200, value)
        state = parse_qs(urlsplit(value["authorization_url"]).query)["state"][0]
        status, value = self.user("POST", "oauth/callback", {"state": state, "code": "other-google-account", "session_binding": BINDING})
        self.assertEqual(status, 409, value)
        self.assertEqual(value["error"], "google_account_mismatch")
        self.assertEqual(self.user("GET", "status")[1]["calendar_id"], "primary@example.test")
        self.assertTrue(self.user("GET", "changes")[1]["items"])

    def assert_inflight_pull_cannot_repopulate_reconnected_account(self, provider_id):
        self.start(CALENDAR_HTTP_TIMEOUT="10")
        self.connect()
        self.google.events = [event("old-private-event", "Dato del precedente collegamento")]
        self.google.events_started = threading.Event()
        self.google.events_release = threading.Event()
        self.google.events_replied = threading.Event()
        status, value = self.user("POST", "sync", {})
        self.assertEqual(status, 202, value)
        self.assertTrue(self.google.events_started.wait(8), "The old account pull never started")
        status, value = self.user("POST", "disconnect", {})
        self.assertEqual(status, 200, value)
        self.google.primary_calendar_id = provider_id
        self.authorize(code="reconnected-account")
        self.select()
        self.google.events_release.set()
        self.assertTrue(self.google.events_replied.wait(3), "The old provider response was not released")
        # Check over a short period after the old response so a worker commit
        # cannot slip past a single immediate status read on the API thread.
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            self.assertEqual(self.cached_events(), [], "An old in-flight response populated the new account")
            status, value = self.user("GET", "status")
            self.assertEqual(status, 200, value)
            self.assertEqual(value["last_sync"], 0, "An old worker marked the new account synchronized")
            self.assertEqual(value["last_error"], "", "An old worker changed the new account status")
            time.sleep(0.025)

    def test_inflight_pull_is_discarded_after_switching_google_accounts(self):
        self.assert_inflight_pull_cannot_repopulate_reconnected_account("other-account@example.test")

    def test_inflight_pull_is_discarded_after_reconnecting_the_same_google_account(self):
        self.assert_inflight_pull_cannot_repopulate_reconnected_account("primary@example.test")

    def test_lost_remote_ack_is_reconciled_after_restart_without_resending(self):
        self.start()
        self.connect()
        payload = event(summary="Accettato prima della perdita di connessione")
        for name in ("id", "etag"):
            payload.pop(name)
        self.google.drop_mutation_ack = True
        status, value = self.user("POST", "events/create", {
            "idempotency_key": "unknown-change-1", "event": json.dumps(payload)})
        self.assertEqual(status, 202, value)
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(changes[0]["state"], "unknown")
        self.assertEqual(len(self.google.mutations), 1)
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE changes SET next_attempt=0")
            database.commit()
        self.start()
        self.synchronize()
        changes = self.user("GET", "changes")[1]["items"]
        self.assertEqual(changes[0]["state"], "applied")
        self.assertEqual(len(self.google.mutations), 1, "A remotely accepted change was sent twice")

    def test_framing_and_user_identifiers_fail_closed(self):
        self.start(configured=False)
        for path in ("/v1/users/-1/status", "/v1/users/0/status", "/v1/users/1%2F2/status"):
            with self.subTest(path=path):
                self.assertIn(self.request("GET", path)[0], (400, 404))
        for headers in (b"Content-Length: -1\r\n", b"Transfer-Encoding: chunked\r\n",
                        b"Content-Length: 0\r\nContent-Length: 1\r\n"):
            with self.subTest(headers=headers):
                with socket.create_connection(("127.0.0.1", self.port), timeout=3) as raw:
                    with self.context.wrap_socket(raw, server_hostname="127.0.0.1") as tls:
                        tls.sendall(b"POST /v1/users/1/oauth/start HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " +
                                    TOKEN.encode() + b"\r\n" + headers + b"Connection: close\r\n\r\n")
                        response = tls.recv(8192)
                        self.assertRegex(response.split(b"\r\n", 1)[0], rb"HTTP/1.[01] (400|411|413)")
        self.assertEqual(self.google.requests, [])


if __name__ == "__main__":
    unittest.main()
