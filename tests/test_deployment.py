"""Native HTTP deployment checks, independent of Docker and third-party packages."""

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
EMAIL = "deployment@punto.local"
PASSWORD = "Deployment-test!2026"


class DeploymentIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / "build" / "punto"
        if not cls.binary.is_file():
            raise RuntimeError("Missing build/punto: run make before the tests")
        cls.fixture = tempfile.TemporaryDirectory(prefix="punto-deployment-tests-")
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.public = Path(cls.fixture.name) / "public"
        cls.public.mkdir()
        (cls.public / "index.html").write_text("<html>deployment fixture</html>",
                                               encoding="utf-8")
        with socket.socket() as internal, socket.socket() as published:
            internal.bind(("127.0.0.1", 0))
            published.bind(("127.0.0.1", 0))
            cls.port = internal.getsockname()[1]
            cls.public_port = published.getsockname()[1]
        cls.env = dict(os.environ, PORT=str(cls.port), PUBLIC_PORT=str(cls.public_port),
                       BIND_HOST="127.0.0.1", PUBLIC_DIR=str(cls.public),
                       DB_PATH=str(Path(cls.fixture.name) / "deployment.db"),
                       PUNTO_EMAIL=EMAIL, PUNTO_PASSWORD=PASSWORD)
        cls.log = tempfile.TemporaryFile(mode="w+b")
        cls.addClassCleanup(cls.log.close)
        cls.server = subprocess.Popen([str(cls.binary)], cwd=ROOT, env=cls.env,
                                      stdout=cls.log, stderr=subprocess.STDOUT)
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                cls.log.seek(0)
                raise RuntimeError("Server exited during startup: " +
                                   cls.log.read().decode("utf-8", "replace"))
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.025)
        raise RuntimeError("Server did not listen within five seconds")

    @classmethod
    def stop_server(cls):
        if cls.server.poll() is None:
            cls.server.terminate()
            try:
                cls.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait(timeout=3)

    def request(self, method, path, *, host=None, origin=None, cookie=None,
                fields=None, headers=None):
        request_headers = dict(headers or {})
        if host is not None:
            request_headers["Host"] = host
        if origin is not None:
            request_headers["Origin"] = origin
        if cookie is not None:
            request_headers["Cookie"] = cookie
        body = None
        if fields is not None:
            request_headers["Content-Type"] = "application/x-www-form-urlencoded"
            body = urlencode(fields)
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()
        self.assertNotIn(PASSWORD.encode(), result[2], "A response leaked a password")
        return result

    def login(self, host):
        status, headers, body = self.request("POST", "/api/login", host=host,
            origin="http://" + host, fields={"email": EMAIL, "password": PASSWORD})
        self.assertEqual(status, 200, body)
        self.assertIs(json.loads(body).get("ok"), True)
        return headers["Set-Cookie"].split(";", 1)[0]

    def test_health_endpoint_is_public_and_head_matches_get(self):
        status, headers, body = self.request("GET", "/healthz")
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body), {"ok": True, "database": "sqlite"})
        self.assertIn("application/json", headers.get("Content-Type", ""))
        self.assertNotIn("Set-Cookie", headers)
        status, head_headers, head_body = self.request("HEAD", "/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(head_body, b"")
        self.assertEqual(head_headers.get("Content-Length"), str(len(body)))
        self.assertEqual(head_headers.get("Content-Type"), headers.get("Content-Type"))
        self.assertNotIn("Set-Cookie", head_headers)
        status, _, body = self.request("POST", "/healthz", fields={})
        self.assertEqual(status, 405, body)
        self.assertIs(json.loads(body).get("ok"), False)

    def test_internal_and_public_authorities_allow_matching_origins_only(self):
        hosts = ["127.0.0.1:%d" % self.port, "localhost:%d" % self.port,
                 "127.0.0.1:%d" % self.public_port, "localhost:%d" % self.public_port]
        for index, host in enumerate(hosts):
            with self.subTest(accepted_host=host):
                cookie = self.login(host)
                status, _, body = self.request("POST", "/api/jobs", host=host,
                    origin="http://" + host, cookie=cookie,
                    fields={"code": "DEPLOY-HOST-%d" % index,
                            "name": "Published port check", "status": "active"})
                self.assertEqual(status, 201, body)
                self.assertIs(json.loads(body).get("ok"), True)
        unused = next(port for port in (1024, 1025, 1026)
                      if port not in (self.port, self.public_port))
        for host in ("outside.example:%d" % self.public_port,
                     "127.0.0.1:%d" % unused, "localhost:%d" % unused,
                     "0.0.0.0:%d" % self.port):
            with self.subTest(rejected_host=host):
                status, _, body = self.request("GET", "/healthz", host=host)
                self.assertEqual(status, 403, body)

    def test_cross_origin_mutations_cannot_write_or_end_a_session(self):
        host = "127.0.0.1:%d" % self.public_port
        cookie = self.login(host)
        status, _, body = self.request("GET", "/api/jobs", host=host, cookie=cookie)
        self.assertEqual(status, 200, body)
        before = json.loads(body)["jobs"]
        origins = ("https://" + host, "http://outside.example:%d" % self.public_port,
                   "http://127.0.0.1:%d" % self.port)
        for origin in origins:
            for path, fields in (("/api/jobs", {"code": "DEPLOY-FORBIDDEN",
                                                "name": "Must not exist", "status": "active"}),
                                 ("/api/logout", {}),
                                 ("/api/login", {"email": EMAIL, "password": PASSWORD})):
                with self.subTest(origin=origin, endpoint=path):
                    status, _, body = self.request("POST", path, host=host,
                        origin=origin, cookie=cookie, fields=fields)
                    self.assertEqual(status, 403, body)
        status, _, body = self.request("POST", "/api/jobs", host=host,
            origin="http://" + host, cookie=cookie,
            fields={"code": "DEPLOY-FORBIDDEN", "name": "Must not exist", "status": "active"},
            headers={"Sec-Fetch-Site": "cross-site"})
        self.assertEqual(status, 403, body)
        status, _, body = self.request("GET", "/api/jobs", host=host, cookie=cookie)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)["jobs"], before)
        status, _, body = self.request("GET", "/api/session", host=host, cookie=cookie)
        self.assertEqual(status, 200, body)
        self.assertIs(json.loads(body).get("authenticated"), True)

    def test_invalid_network_configuration_exits_before_creating_database(self):
        invalid = [("BIND_HOST", "localhost"), ("BIND_HOST", "::1"),
                   ("BIND_HOST", "999.1.1.1"), ("PUBLIC_PORT", ""),
                   ("PUBLIC_PORT", "0"), ("PUBLIC_PORT", "65536"),
                   ("PUBLIC_PORT", "8082x")]
        for key, value in invalid:
            with self.subTest(variable=key, value=value):
                with tempfile.TemporaryDirectory(prefix="punto-invalid-deployment-") as directory:
                    env = dict(self.env, DB_PATH="data/punto.db")
                    env[key] = value
                    process = subprocess.run([str(self.binary)], cwd=directory, env=env,
                                             capture_output=True, timeout=3)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn(key.encode(), process.stderr)
                    self.assertFalse((Path(directory) / "data").exists(),
                                     "Rejected config must not create the data directory")


if __name__ == "__main__":
    unittest.main()
