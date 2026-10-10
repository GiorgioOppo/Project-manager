"""Integration tests for the C HTTP server, using only the Python standard library.

Build with ``make`` first, then run ``python3 -m unittest discover -s tests -v``.
The suite starts its own loopback server with temporary static assets.
"""

import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
EMAIL = "integration@punto.local"
PASSWORD = "Test-only! 2026 +&="


class ServerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        binary = ROOT / "build" / "punto"
        if not binary.is_file():
            raise RuntimeError("Missing build/punto: run make before the tests")

        cls.fixture = tempfile.TemporaryDirectory(prefix="punto-tests-")
        public = Path(cls.fixture.name) / "public"
        public.mkdir()
        cls.login_marker = "<html><body>fixture-login</body></html>"
        cls.app_marker = "<html><body>fixture-private-app</body></html>"
        for name in ("index.html", "login.html"):
            (public / name).write_text(cls.login_marker, encoding="utf-8")
        (public / "app.html").write_text(cls.app_marker, encoding="utf-8")
        (public / "styles.css").write_text("body { color: #123456; }", encoding="utf-8")
        (public / "login.js").write_text("/* fixture login script */", encoding="utf-8")
        (public / "app.js").write_text("/* fixture app script */", encoding="utf-8")
        (Path(cls.fixture.name) / "secret.txt").write_text("fixture-secret", encoding="utf-8")

        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            cls.port = reserved.getsockname()[1]
        cls.origin = "http://127.0.0.1:%s" % cls.port
        env = dict(os.environ, PORT=str(cls.port), PUBLIC_DIR=str(public),
                   PUNTO_EMAIL=EMAIL, PUNTO_PASSWORD=PASSWORD,
                   DB_PATH=str(Path(cls.fixture.name) / "auth-tests.db"))
        cls.log = tempfile.TemporaryFile(mode="w+b")
        cls.server = subprocess.Popen([str(binary)], cwd=ROOT, env=env,
                                      stdout=cls.log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if cls.server.poll() is not None:
                    cls.log.seek(0)
                    raise RuntimeError("Server exited during startup: " +
                                       cls.log.read().decode("utf-8", "replace"))
                try:
                    with socket.create_connection(("127.0.0.1", cls.port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.025)
            else:
                raise RuntimeError("Server did not listen within five seconds")
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "server") and cls.server.poll() is None:
            cls.server.terminate()
            try:
                cls.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait(timeout=3)
        if hasattr(cls, "log"):
            cls.log.close()
        if hasattr(cls, "fixture"):
            cls.fixture.cleanup()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            payload = response.read()
            result = response.status, dict(response.getheaders()), payload
        finally:
            connection.close()
        self.assertNotIn(PASSWORD.encode(), payload, "A response leaked the password")
        return result

    def api(self, method, path, body=None, headers=None):
        status, response_headers, payload = self.request(method, path, body, headers)
        self.assertIn("application/json", response_headers.get("Content-Type", ""))
        return status, response_headers, json.loads(payload)

    def login(self, remember=False, headers=None):
        fields = {"email": EMAIL, "password": PASSWORD}
        if remember:
            fields["remember"] = "on"
        request_headers = {"Content-Type": "application/x-www-form-urlencoded",
                           "Origin": self.origin}
        request_headers.update(headers or {})
        return self.api("POST", "/api/login", urlencode(fields), request_headers)

    def authenticated_cookie(self, remember=False):
        status, headers, data = self.login(remember)
        self.assertEqual(status, 200)
        self.assertIs(data.get("ok"), True)
        set_cookie = headers.get("Set-Cookie", "")
        self.assertTrue(set_cookie.startswith("punto_session="))
        self.assertIn("httponly", set_cookie.lower())
        self.assertIn("samesite=lax", set_cookie.lower())
        self.assertIn("path=/", set_cookie.lower())
        cookie = set_cookie.split(";", 1)[0]
        self.assertGreater(len(cookie.split("=", 1)[1]), 20)
        return cookie, set_cookie

    def test_public_page_and_assets(self):
        status, headers, body = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(body.decode(), self.login_marker)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        for path, content_type in (("/styles.css", "text/css"),
                                   ("/login.js", "javascript"),
                                   ("/app.js", "javascript")):
            with self.subTest(path=path):
                status, headers, body = self.request("GET", path)
                self.assertEqual(status, 200)
                self.assertIn(content_type, headers.get("Content-Type", ""))
                self.assertTrue(body)

    def test_login_session_logout_lifecycle(self):
        status, _, data = self.api("GET", "/api/session")
        self.assertEqual(status, 200)
        self.assertIs(data.get("authenticated"), False)
        cookie, _ = self.authenticated_cookie()
        status, _, data = self.api("GET", "/api/session", headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertIs(data.get("authenticated"), True)
        self.assertEqual(data.get("email"), EMAIL)
        status, _, body = self.request("GET", "/app", headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertEqual(body.decode(), self.app_marker)
        status, headers, data = self.api("POST", "/api/logout", headers={
            "Cookie": cookie, "Origin": self.origin})
        self.assertEqual(status, 200)
        self.assertIs(data.get("ok"), True)
        self.assertIn("max-age=0", headers.get("Set-Cookie", "").lower())
        status, _, data = self.api("GET", "/api/session", headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertIs(data.get("authenticated"), False)

    def test_invalid_credentials(self):
        status, headers, data = self.api("POST", "/api/login", urlencode({
            "email": EMAIL, "password": "incorrect-password"}), {
            "Content-Type": "application/x-www-form-urlencoded", "Origin": self.origin})
        self.assertEqual(status, 401)
        self.assertIs(data.get("ok"), False)
        self.assertTrue(data.get("error"))
        self.assertNotIn("Set-Cookie", headers)

    def test_malformed_form_is_rejected(self):
        for body in ("email=%ZZ&password=bad", "email=test", "password=test", "",
                     "email=first&email=second&password=test",
                     "email=test&password=first&password=second"):
            with self.subTest(body=body):
                status, _, data = self.api("POST", "/api/login", body, {
                    "Content-Type": "application/x-www-form-urlencoded", "Origin": self.origin})
                self.assertEqual(status, 400)
                self.assertIs(data.get("ok"), False)

    def test_unsupported_login_content_type(self):
        status, headers, data = self.api("POST", "/api/login", json.dumps({
            "email": EMAIL, "password": PASSWORD}), {
            "Content-Type": "application/json", "Origin": self.origin})
        self.assertEqual(status, 415)
        self.assertIs(data.get("ok"), False)
        self.assertNotIn("Set-Cookie", headers)

    def test_protected_route_rejects_missing_and_forged_session(self):
        for headers in ({}, {"Cookie": "punto_session=forged-session"}):
            with self.subTest(headers=headers):
                status, response_headers, body = self.request("GET", "/app", headers=headers)
                self.assertEqual(status, 303)
                self.assertEqual(response_headers.get("Location"), "/")
                self.assertNotIn(b"fixture-private-app", body)
        status, _, body = self.request("GET", "/app.html")
        self.assertIn(status, (303, 404))
        self.assertNotIn(b"fixture-private-app", body)

    def test_remember_me_uses_a_persistent_cookie(self):
        _, regular = self.authenticated_cookie()
        _, remembered = self.authenticated_cookie(remember=True)
        self.assertNotIn("max-age=", regular.lower())
        self.assertNotIn("expires=", regular.lower())
        self.assertIn("max-age=2592000", remembered.lower())

    def test_hostile_origin_cannot_login_or_logout(self):
        status, headers, data = self.login(headers={"Origin": "https://attacker.example"})
        self.assertEqual(status, 403)
        self.assertIs(data.get("ok"), False)
        self.assertNotIn("Set-Cookie", headers)
        cookie, _ = self.authenticated_cookie()
        status, _, _ = self.api("POST", "/api/logout", headers={
            "Cookie": cookie, "Origin": "https://attacker.example"})
        self.assertEqual(status, 403)
        status, _, data = self.api("GET", "/api/session", headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertIs(data.get("authenticated"), True)

    def test_security_headers(self):
        for path in ("/", "/api/session"):
            with self.subTest(path=path):
                status, headers, _ = self.request("GET", path)
                self.assertEqual(status, 200)
                self.assertEqual(headers.get("X-Content-Type-Options"), "nosniff")
                self.assertIn("no-store", headers.get("Cache-Control", ""))
                self.assertTrue(headers.get("Content-Security-Policy"))

    def test_unknown_routes_and_methods(self):
        for method, path, expected in (("GET", "/does-not-exist", 404),
                                       ("POST", "/", 405),
                                       ("GET", "/api/login", 405),
                                       ("POST", "/api/session", 405)):
            with self.subTest(method=method, path=path):
                status, _, _ = self.request(method, path)
                self.assertEqual(status, expected)

    def test_traversal_paths_are_rejected(self):
        for path in ("/../secret.txt", "/%2e%2e/secret.txt", "/styles.css/../../secret.txt",
                     "/%2e%2e%2fsecret.txt"):
            with self.subTest(path=path):
                status, _, body = self.request("GET", path)
                self.assertIn(status, (400, 403, 404))
                self.assertNotIn(b"fixture-secret", body)

    def test_oversized_body_is_rejected(self):
        status, headers, body = self.request("POST", "/api/login", "x" * 8193, {
            "Content-Type": "application/x-www-form-urlencoded", "Origin": self.origin})
        self.assertIn(status, (400, 413))
        self.assertNotIn("Set-Cookie", headers)

    def test_incomplete_content_length_does_not_authenticate(self):
        encoded = urlencode({"email": EMAIL, "password": PASSWORD}).encode()
        request = ("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                   "Content-Type: application/x-www-form-urlencoded\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n" %
                   (self.port, len(encoded) + 10)).encode() + encoded
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
            connection.settimeout(3)
            connection.sendall(request)
            connection.shutdown(socket.SHUT_WR)
            response = bytearray()
            while True:
                block = connection.recv(4096)
                if not block:
                    break
                response.extend(block)
        self.assertNotIn(b"200 OK", response)
        self.assertNotIn(b"Set-Cookie: punto_session=", response)
        self.assertNotIn(PASSWORD.encode(), response)
        status, _, data = self.api("GET", "/api/session")
        self.assertEqual(status, 200)
        self.assertIs(data.get("authenticated"), False)


if __name__ == "__main__":
    unittest.main()
