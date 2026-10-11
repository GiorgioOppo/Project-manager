"""Authenticated app bridge against a private TLS calendar API fixture."""

import http.client
import http.server
import json
import os
from pathlib import Path
import ssl
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlencode, urlsplit

from test_mail_client import create_certificate
import test_roles as roles

CALENDAR_ENV = ("CALENDAR_API_URL", "CALENDAR_API_CA_FILE", "CALENDAR_API_TOKEN_FILE")
TOKEN = "calendar-client-" + "c" * 64


class FakeCalendarAPI:
    def __init__(self, certificate, key):
        self.requests = []
        self.reply_status = 200
        self.reply_value = {"configured": True, "oauth_configured": True, "connected": False, "provider": "google"}
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                fixture.requests.append({"method": self.command, "path": self.path,
                                         "authorization": self.headers.get("Authorization"),
                                         "fields": parse_qs(body.decode("ascii"), keep_blank_values=True)})
                value = fixture.reply_value
                payload = value if isinstance(value, bytes) else json.dumps(value).encode("utf-8")
                self.send_response(fixture.reply_status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Location", "https://calendar-redirection-denied.invalid/")
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (ConnectionError, ssl.SSLError):
                    pass

            do_GET = handle_request
            do_POST = handle_request

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


class CalendarBridgeTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    assert_error = roles.RoleIntegrationTests.assert_error
    users = roles.RoleIntegrationTests.users

    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-calendar-bridge-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "calendar-bridge")
        cls.other_ca, _ = create_certificate(cls.certificates.name, "other-ca")
        cls.wrong_cert, cls.wrong_key = create_certificate(cls.certificates.name, "wrong-name", "wrong-api.invalid")

    def setUp(self):
        self.service = FakeCalendarAPI(self.cert, self.key)
        self.addCleanup(self.service.close)
        self.token_file = Path(self.certificates.name) / "api-token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.environment = {"CALENDAR_API_URL": self.service.url, "CALENDAR_API_CA_FILE": str(self.cert),
                            "CALENDAR_API_TOKEN_FILE": str(self.token_file)}
        with patch.dict(os.environ, self.environment):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.manager_id = next(user["id"] for user in self.users() if user["email"] == roles.MANAGER_EMAIL)

    def test_status_is_authenticated_and_each_session_uses_its_own_service_user(self):
        self.assert_error(401, "GET", "/api/calendar/status")
        self.assertEqual(self.service.requests, [])
        for cookie, uid in ((self.manager_cookie, self.manager_id), (self.worker_cookie, self.worker_id)):
            with self.subTest(uid=uid):
                status, value, _ = self.api("GET", "/api/calendar/status", cookie=cookie)
                self.assertEqual(status, 200, value)
                self.assertEqual(self.service.requests[-1]["path"], "/v1/users/%d/status" % uid)
                self.assertEqual(self.service.requests[-1]["authorization"], "Bearer " + TOKEN)

    def test_identity_and_internal_oauth_binding_cannot_be_overridden(self):
        for method, route, fields in (
                ("GET", "/api/calendar/status?user_id=%d" % self.manager_id, None),
                ("POST", "/api/calendar/sync", {"user_id": self.manager_id}),
                ("POST", "/api/calendar/oauth/start", {"session_binding": "f" * 64}),
                ("POST", "/api/calendar/selection", {"calendar_id": "primary", "provider_url": "https://attacker.invalid"})):
            with self.subTest(method=method, route=route):
                self.assert_error(400, method, route, fields, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_mutations_require_same_origin_and_forward_only_allowed_form_fields(self):
        for path, fields in (("/api/calendar/oauth/start", {}),
                             ("/api/calendar/selection", {"calendar_id": "primary@example.test"}),
                             ("/api/calendar/sync", {}), ("/api/calendar/disconnect", {})):
            with self.subTest(path=path):
                self.assert_error(403, "POST", path, fields, cookie=self.worker_cookie,
                                  headers={"Origin": "https://hostile.invalid"})
        self.assertEqual(self.service.requests, [])
        status, value, _ = self.api("POST", "/api/calendar/selection", {"calendar_id": "primary@example.test"},
                                    cookie=self.worker_cookie)
        self.assertEqual(status, 200, value)
        request = self.service.requests[-1]
        self.assertEqual(request["path"], "/v1/users/%d/selection" % self.worker_id)
        self.assertEqual(request["fields"], {"calendar_id": ["primary@example.test"]})

    def test_oauth_start_receives_server_derived_session_binding(self):
        self.service.reply_value = {"authorization_url": "https://accounts.google.com/o/oauth2/v2/auth?state=fixture"}
        for cookie in (self.manager_cookie, self.worker_cookie):
            status, value, _ = self.api("POST", "/api/calendar/oauth/start", {}, cookie=cookie)
            self.assertEqual(status, 200, value)
            request = self.service.requests[-1]
            binding = request["fields"].get("session_binding", [])
            self.assertEqual(len(binding), 1)
            self.assertRegex(binding[0], r"\A[0-9a-f]{64}\Z")
            self.assertNotIn(cookie, json.dumps(request))
        self.assertNotEqual(self.service.requests[0]["fields"], self.service.requests[1]["fields"])

    def test_calendar_mutations_preserve_idempotency_etag_and_json_payload(self):
        payload = json.dumps({"summary": "Qualità & caffè", "start": {"date": "2030-01-02"}, "end": {"date": "2030-01-03"}})
        fields = {"idempotency_key": "client-change-1", "event_id": "abc", "etag": '"rev-2"', "event": payload}
        self.service.reply_status = 202
        self.service.reply_value = {"queued": True}
        status, value, _ = self.api("POST", "/api/calendar/events/update", fields, cookie=self.worker_cookie)
        self.assertEqual(status, 202, value)
        request = self.service.requests[-1]
        self.assertEqual(request["path"], "/v1/users/%d/events/update" % self.worker_id)
        self.assertEqual(request["fields"], {name: [content] for name, content in fields.items()})

    def test_exact_event_and_change_lookups_keep_authenticated_identity(self):
        for route, query, value in (
                ("event", {"event_id": "remote_event-42"},
                 {"event": {"id": "remote_event-42", "etag": '"revision-3"'}, "calendar_id": "primary@example.test"}),
                ("change", {"idempotency_key": "ticket:42:create-1"},
                 {"id": 8, "idempotency_key": "ticket:42:create-1", "event_id": "remote_event-42",
                  "state": "applied", "error": "", "attempts": 1, "calendar_id": "primary@example.test"})):
            with self.subTest(route=route):
                self.service.reply_value = value
                status, body, _headers = self.api("GET", "/api/calendar/" + route + "?" + urlencode(query), cookie=self.worker_cookie)
                self.assertEqual(status, 200, body)
                self.assertEqual(body, value)
                sent = urlsplit(self.service.requests[-1]["path"])
                self.assertEqual(sent.path, f"/v1/users/{self.worker_id}/{route}")
                self.assertEqual(parse_qs(sent.query), {key: [item] for key, item in query.items()})
        before = len(self.service.requests)
        for route in ("event", "event?event_id=", "event?event_id=one&event_id=two",
                      "event?event_id=one&user_id=1", "change", "change?idempotency_key=short",
                      "change?idempotency_key=valid-key&%69dempotency_key=other-key",
                      "change?idempotency_key=valid-key&user_id=1"):
            self.assert_error(400, "GET", "/api/calendar/" + route, cookie=self.worker_cookie)
        self.assertEqual(len(self.service.requests), before)
        self.service.reply_status = 404
        self.service.reply_value = {"ok": False, "error": "event_not_found"}
        self.assert_error(404, "GET", "/api/calendar/event?event_id=missing", cookie=self.worker_cookie)

    def test_provider_errors_and_invalid_json_are_bounded_and_redirects_are_not_followed(self):
        for status, body in ((409, {"error": "conflict"}), (503, {"error": "calendar_not_configured"}),
                             (200, b"not-json"), (302, {"configured": True}), (200, b"[" + b"x" * 300000)):
            with self.subTest(status=status):
                self.service.reply_status, self.service.reply_value = status, body
                response, value, _ = self.api("GET", "/api/calendar/status", cookie=self.worker_cookie)
                if status in (409, 503):
                    self.assertEqual(response, status, value)
                else:
                    self.assertEqual(response, 503, value)
        self.assertEqual(len(self.service.requests), 5)

    def test_private_api_bypasses_external_proxy_environment(self):
        self.stop_server()
        environment = dict(self.environment, HTTPS_PROXY="http://127.0.0.1:1", https_proxy="http://127.0.0.1:1",
                           HTTP_PROXY="http://127.0.0.1:1", http_proxy="http://127.0.0.1:1", NO_PROXY="", no_proxy="")
        with patch.dict(os.environ, environment):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, value, _ = self.api("GET", "/api/calendar/status", cookie=cookie)
        self.assertEqual(status, 200, value)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/users/%d/status" % self.worker_id)

    def test_untrusted_internal_ca_fails_closed_without_http_request(self):
        self.stop_server()
        self.service.requests.clear()
        with patch.dict(os.environ, dict(self.environment, CALENDAR_API_CA_FILE=str(self.other_ca))):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assert_error(503, "GET", "/api/calendar/status", cookie=cookie)
        self.assertEqual(self.service.requests, [])

    def test_change_history_and_calendar_pages_keep_the_authenticated_identity(self):
        self.service.reply_value = {"items": [{"state": "conflict", "error": "etag_conflict"}]}
        for cookie, uid in ((self.worker_cookie, self.worker_id), (self.manager_cookie, self.manager_id)):
            status, value, _ = self.api("GET", "/api/calendar/changes", cookie=cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(self.service.requests[-1]["path"], "/v1/users/%d/changes" % uid)
        before = len(self.service.requests)
        self.assert_error(400, "GET", "/api/calendar/changes?user_id=%d" % self.manager_id, cookie=self.worker_cookie)
        self.assertEqual(len(self.service.requests), before)
        query = {"page_token": "page+cursor&next=2"}
        status, value, _ = self.api("GET", "/api/calendar/calendars?" + urlencode(query), cookie=self.worker_cookie)
        self.assertEqual(status, 200, value)
        sent = urlsplit(self.service.requests[-1]["path"])
        self.assertEqual(sent.path, "/v1/users/%d/calendars" % self.worker_id)
        self.assertEqual(parse_qs(sent.query), {"page_token": ["page+cursor&next=2"]})
        before = len(self.service.requests)
        for query in ({"page_token": "x" * 2049}, {"user_id": str(self.manager_id)}):
            self.assert_error(400, "GET", "/api/calendar/calendars?" + urlencode(query), cookie=self.worker_cookie)
        self.assertEqual(len(self.service.requests), before)

    def test_event_pagination_and_date_filters_are_canonical_and_bounded(self):
        self.service.reply_value = {"items": [], "has_more": False}
        query = {"from": "2030-01-02", "to": "2030-01-31", "limit": "100", "after_id": "event+id&x"}
        status, value, _ = self.api("GET", "/api/calendar/events?" + urlencode(query), cookie=self.worker_cookie)
        self.assertEqual(status, 200, value)
        sent = urlsplit(self.service.requests[-1]["path"])
        self.assertEqual(sent.path, "/v1/users/%d/events" % self.worker_id)
        self.assertEqual(parse_qs(sent.query), {key: [value] for key, value in query.items()})
        before = len(self.service.requests)
        for query in ({"limit": "0"}, {"limit": "101"}, {"from": "2030-02-29"},
                      {"to": "2030-01-32"}, {"after_id": "x" * 1025}):
            with self.subTest(query=query):
                self.assert_error(400, "GET", "/api/calendar/events?" + urlencode(query), cookie=self.worker_cookie)
        self.assertEqual(len(self.service.requests), before)

    def test_oauth_callback_is_session_bound_and_redirects_without_echoing_code(self):
        query = urlencode({"state": "fixture-state-123456", "code": "private-google-code", "scope": "calendar.events", "authuser": "0"})

        def callback(cookie=None, query=query):
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
            try:
                connection.request("GET", "/api/calendar/oauth/callback?" + query,
                                   headers={"Cookie": cookie} if cookie else {})
                response = connection.getresponse()
                return response.status, response.read(), dict(response.getheaders())
            finally:
                connection.close()

        self.assertEqual(callback()[2].get("Location"), "/app?calendar=error")
        self.assertEqual(self.service.requests, [])
        self.service.reply_value = {"ok": True}
        status, body, headers = callback(self.worker_cookie)
        self.assertEqual(status, 303)
        self.assertEqual(headers.get("Location"), "/app?calendar=connected")
        self.assertEqual(body, b"")
        request = self.service.requests[-1]
        self.assertEqual(request["path"], "/v1/users/%d/oauth/callback" % self.worker_id)
        self.assertEqual(request["fields"]["code"], ["private-google-code"])
        self.assertEqual(request["fields"]["state"], ["fixture-state-123456"])
        self.assertRegex(request["fields"]["session_binding"][0], r"\A[0-9a-f]{64}\Z")
        self.assertNotIn("scope", request["fields"])
        self.assertNotIn("authuser", request["fields"])
        for value in ({"ok": False}, {"connected": True}, {"ok": True, "access_token": "private-token"}):
            self.service.reply_value = value
            status, body, headers = callback(self.worker_cookie)
            self.assertEqual(status, 303)
            self.assertEqual(headers.get("Location"), "/app?calendar=error")
            self.assertEqual(body, b"")
        before = len(self.service.requests)
        self.assertEqual(callback(self.worker_cookie, "code=private-google-code")[2].get("Location"), "/app?calendar=error")
        self.assertEqual(len(self.service.requests), before)

    def test_nested_credentials_duplicate_fields_and_wrong_hostname_are_rejected(self):
        for value in ({"nested": [{"access_token": "private-access-token"}]},
                      {"items": [{"deep": {"Client_Secret": "private-client-secret"}}]}):
            self.service.reply_value = value
            self.assert_error(503, "GET", "/api/calendar/status", cookie=self.worker_cookie)
        before = len(self.service.requests)
        for body in ("calendar_id=one&calendar_id=two", "calendar_id=one&user%5fid=1",
                     "calendar_id=one&%63alendar_id=two"):
            self.assert_error(400, "POST", "/api/calendar/selection", cookie=self.worker_cookie,
                              body=body, headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(len(self.service.requests), before)
        self.stop_server()
        self.service.close()
        self.service = FakeCalendarAPI(self.wrong_cert, self.wrong_key)
        self.addCleanup(self.service.close)
        environment = dict(self.environment, CALENDAR_API_URL=self.service.url,
                           CALENDAR_API_CA_FILE=str(self.wrong_cert))
        with patch.dict(os.environ, environment):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assert_error(503, "GET", "/api/calendar/status", cookie=cookie)
        self.assertEqual(self.service.requests, [])

    def test_native_without_addon_configuration_has_explicit_disabled_status(self):
        self.stop_server()
        clean = {name: value for name, value in os.environ.items() if name not in CALENDAR_ENV}
        with patch.dict(os.environ, clean, clear=True):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, value, _ = self.api("GET", "/api/calendar/status", cookie=cookie)
        self.assertEqual(status, 200, value)
        self.assertIs(value["enabled"], False)
        self.assertIs(value["oauth_configured"], False)
        self.assertIs(value["connected"], False)
        self.assertEqual(value["provider"], "google")
        self.assert_error(503, "POST", "/api/calendar/oauth/start", {}, cookie=cookie)


class CalendarClientConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="punto-calendar-client-config-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        cls.cert, _ = create_certificate(cls.path, "client-config")
        cls.source = cls.path / "configuration.c"
        cls.source.write_text('#include "calendar_client.h"\n#include <stdio.h>\n'
                              'int main(void){if(!calendar_client_open()){puts("invalid");return 0;} '
                              'printf("enabled=%d\\n",calendar_client_enabled());calendar_client_close();return 0;}\n')
        cls.binary = cls.path / "configuration"
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "json-c"], check=True,
                               capture_output=True, text=True).stdout.split()
        subprocess.run([shutil.which("cc"), "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        "-I", str(roles.ROOT / "src"), str(roles.ROOT / "src/calendar_client.c"), str(cls.source),
                        "-lcurl", *flags, "-o", str(cls.binary)], check=True, capture_output=True, text=True)

    def setUp(self):
        self.token = self.path / "token"
        self.token.write_text(TOKEN + "\n", encoding="ascii")
        self.token.chmod(0o600)
        self.environment = {name: value for name, value in os.environ.items() if name not in CALENDAR_ENV}
        self.environment.update(CALENDAR_API_URL="https://127.0.0.1:8091", CALENDAR_API_CA_FILE=str(self.cert),
                                CALENDAR_API_TOKEN_FILE=str(self.token))

    def invoke(self, absent=(), **changes):
        env = dict(self.environment, **changes)
        for name in absent:
            env.pop(name, None)
        result = subprocess.run([str(self.binary)], env=env, check=True, capture_output=True, text=True, timeout=3)
        self.assertNotIn(TOKEN, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_absent_configuration_is_disabled_and_every_partial_configuration_fails_closed(self):
        self.assertEqual(self.invoke(absent=CALENDAR_ENV), "enabled=0")
        for name in CALENDAR_ENV:
            with self.subTest(name=name):
                self.assertEqual(self.invoke(absent=(name,)), "invalid")

    def test_untrusted_urls_and_unsafe_secret_files_fail_before_network_io(self):
        for url in ("http://127.0.0.1:8091", "https://attacker.invalid:8091", "https://user@localhost:8091",
                    "https://localhost:8091/path", "https://localhost:8091?override=x", "https://localhost:0"):
            with self.subTest(url=url):
                self.assertEqual(self.invoke(CALENDAR_API_URL=url), "invalid")
        self.token.chmod(0o666)
        self.assertEqual(self.invoke(), "invalid")
        self.token.chmod(0o600)
        for value in ("short", "a" * 129, "a" * 32 + "\r\nInjected: yes", "a" * 32 + "\0hidden"):
            self.token.write_text(value, encoding="ascii")
            self.assertEqual(self.invoke(), "invalid")


if __name__ == "__main__":
    unittest.main()
