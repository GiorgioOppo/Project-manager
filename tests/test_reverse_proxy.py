"""HTTP integration checks for the explicit reverse-proxy origin allowlist.

These use the native SQLite binary and temporary data. No proxy or DNS is
required: the TCP connection is loopback while Host models the proxy request.
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
EMAIL = "proxy-tests@punto.local"
PASSWORD = "Proxy-test-only! 2026"
ORIGINS = (
    "http://projects.test,https://secure.projects.test,"
    "https://secure-port.projects.test:8443,http://plain-port.projects.test:8085,"
    "https://default.projects.test:443,http://dual.projects.test,https://dual.projects.test,"
    " http://198.51.100.7:80 "
)


class ReverseProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / "build" / "punto"
        if not cls.binary.is_file():
            raise RuntimeError("Missing build/punto: run make before the tests")
        cls.fixture = tempfile.TemporaryDirectory(prefix="punto-proxy-tests-")
        cls.directory = Path(cls.fixture.name)
        cls.public = cls.directory / "public"
        cls.public.mkdir()
        (cls.public / "index.html").write_text("proxy-login-fixture", encoding="utf-8")
        (cls.public / "app.html").write_text("proxy-app-fixture", encoding="utf-8")
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            cls.port = reserved.getsockname()[1]
        cls.local_host = "127.0.0.1:%d" % cls.port
        cls.env = dict(os.environ, PORT=str(cls.port),
                       PUBLIC_DIR=str(cls.public), PUBLIC_ORIGINS=ORIGINS,
                       PUNTO_EMAIL=EMAIL, PUNTO_PASSWORD=PASSWORD,
                       DB_PATH=str(cls.directory / "proxy-tests.db"))
        cls.env.pop("PUBLIC_PORT", None)
        cls.log = tempfile.TemporaryFile(mode="w+b")
        cls.server = subprocess.Popen([str(cls.binary)], cwd=ROOT, env=cls.env,
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

    def request(self, method, path, host=None, origin=None, body=None, headers=None):
        request_headers = {"Host": host or self.local_host}
        if origin is not None:
            request_headers["Origin"] = origin
        request_headers.update(headers or {})
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            payload = response.read()
            result = response.status, dict(response.getheaders()), payload
        finally:
            connection.close()
        self.assertNotIn(PASSWORD.encode(), payload)
        return result

    def login(self, host, origin=None, headers=None):
        request_headers = {"Content-Type": "application/x-www-form-urlencoded"}
        request_headers.update(headers or {})
        return self.request("POST", "/api/login", host, origin,
                            urlencode({"email": EMAIL, "password": PASSWORD}),
                            request_headers)

    def test_configured_http_and_https_origins_can_login(self):
        for host, origin, secure in (
            ("projects.test", "http://projects.test", False),
            ("plain-port.projects.test:8085", "http://plain-port.projects.test:8085", False),
            ("secure.projects.test", "https://secure.projects.test", True),
            ("secure-port.projects.test:8443", "https://secure-port.projects.test:8443", True),
            # Nginx Proxy Manager's $host header omits the incoming public port.
            ("secure-port.projects.test", "https://secure-port.projects.test:8443", True),
            ("plain-port.projects.test", "http://plain-port.projects.test:8085", False),
            ("default.projects.test", "https://default.projects.test", True),
            ("default.projects.test:443", "https://default.projects.test:443", True),
            ("198.51.100.7", "http://198.51.100.7", False),
        ):
            with self.subTest(origin=origin):
                status, headers, payload = self.login(host, origin)
                self.assertEqual(status, 200)
                self.assertIs(json.loads(payload)["ok"], True)
                attributes = [part.strip().lower() for part in headers["Set-Cookie"].split(";")]
                self.assertIn("httponly", attributes)
                self.assertIn("samesite=lax", attributes)
                self.assertEqual("secure" in attributes, secure)

    def test_https_session_and_logout_cookie_are_secure_without_origin(self):
        host = "secure.projects.test"
        status, headers, _ = self.login(host)
        self.assertEqual(status, 200)
        self.assertIn("; secure", headers["Set-Cookie"].lower())
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        status, _, payload = self.request("GET", "/api/session", host,
                                          headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertIs(json.loads(payload)["authenticated"], True)
        status, headers, _ = self.request("POST", "/api/logout", host,
                                          "https://secure.projects.test",
                                          headers={"Cookie": cookie})
        self.assertEqual(status, 200)
        self.assertIn("max-age=0", headers["Set-Cookie"].lower())
        self.assertIn("; secure", headers["Set-Cookie"].lower())

    def test_configured_origin_must_match_host_scheme_and_port(self):
        cases = (
            ("projects.test", "https://projects.test"),
            ("secure.projects.test", "http://secure.projects.test"),
            ("projects.test", "https://secure.projects.test"),
            ("secure-port.projects.test:8443", "https://secure-port.projects.test"),
            ("secure-port.projects.test:8443", "https://secure-port.projects.test:443"),
            ("secure.projects.test", "https://evil.test"),
            ("secure.projects.test", "https://secure.projects.test.evil.test"),
        )
        for host, origin in cases:
            with self.subTest(host=host, origin=origin):
                status, headers, _ = self.login(host, origin)
                self.assertEqual(status, 403)
                self.assertNotIn("Set-Cookie", headers)

    def test_unlisted_hosts_are_rejected_even_with_forwarded_headers(self):
        for host in ("evil.test", "projects.test.evil.test", "projects.test:8085",
                     "secure-port.projects.test:9443", "secure.projects.test:8443"):
            with self.subTest(host=host):
                status, headers, _ = self.login(host, "https://secure.projects.test", {
                    "X-Forwarded-Host": "secure.projects.test",
                    "X-Forwarded-Proto": "https",
                })
                self.assertEqual(status, 403)
                self.assertNotIn("Set-Cookie", headers)

    def test_forwarded_headers_do_not_replace_host_or_origin(self):
        status, headers, _ = self.login(self.local_host, "https://secure.projects.test", {
            "X-Forwarded-Host": "secure.projects.test", "X-Forwarded-Proto": "https",
        })
        self.assertEqual(status, 403)
        self.assertNotIn("Set-Cookie", headers)
        status, headers, _ = self.login(self.local_host, "http://" + self.local_host, {
            "X-Forwarded-Host": "secure.projects.test", "X-Forwarded-Proto": "https",
        })
        self.assertEqual(status, 200)
        self.assertNotIn("; secure", headers["Set-Cookie"].lower())
        status, headers, _ = self.login("secure.projects.test", "https://secure.projects.test", {
            "X-Forwarded-Host": "evil.test", "X-Forwarded-Proto": "http",
        })
        self.assertEqual(status, 200)
        self.assertIn("; secure", headers["Set-Cookie"].lower())

    def test_cross_site_fetch_still_rejected_on_configured_origin(self):
        status, headers, _ = self.login("secure.projects.test", "https://secure.projects.test", {
            "Sec-Fetch-Site": "cross-site",
        })
        self.assertEqual(status, 403)
        self.assertNotIn("Set-Cookie", headers)

    def test_dual_scheme_host_sets_cookie_security_from_accepted_origin(self):
        for origin, secure in (("http://dual.projects.test", False),
                               ("https://dual.projects.test", True), (None, True)):
            with self.subTest(origin=origin):
                status, headers, _ = self.login("dual.projects.test", origin)
                self.assertEqual(status, 200)
                self.assertEqual("; secure" in headers["Set-Cookie"].lower(), secure)

    def test_invalid_public_origin_configuration_fails_before_database_write(self):
        invalid = (
            "ftp://projects.test", "projects.test", "https://", "https://*.projects.test",
            "https://user:password@projects.test", "https://projects.test/path",
            "https://projects.test?query=1", "https://projects.test#fragment",
            "https://projects.test:0", "https://projects.test:65536",
            "https://projects.test:not-a-port", "https://projects.test,https://evil.test/path",
            "https://projects.test/", "https://pro jects.test", "https://projects.test,",
            "https://projects.test,,http://projects.test",
            "https://projects.test,https://projects.test:443",
        )
        for index, origin in enumerate(invalid):
            with self.subTest(origin=origin):
                database = self.directory / ("invalid-proxy-%d.db" % index)
                env = dict(self.env, PUBLIC_ORIGINS=origin, DB_PATH=str(database))
                process = subprocess.Popen([str(self.binary)], cwd=ROOT, env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                try:
                    output, _ = process.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=2)
                    self.fail("Invalid PUBLIC_ORIGINS did not stop startup: " + origin)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn(b"PUBLIC_ORIGINS", output)
                self.assertFalse(database.exists(), "Invalid configuration created a database")
                self.assertFalse(Path(str(database) + "-wal").exists())
                self.assertNotIn(PASSWORD.encode(), output)


if __name__ == "__main__":
    unittest.main()
