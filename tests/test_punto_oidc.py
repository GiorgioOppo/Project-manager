"""Real HTTP integration of Keycloak-style OIDC with existing Punto accounts."""

import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

from oidc_fixture import OIDCProvider

ROOT = Path(__file__).resolve().parents[1]
EMAIL = "oidc-local@example.test"
PASSWORD = "OIDC-local-test!2026"


class PuntoOIDCTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.provider = OIDCProvider()
        cls.addClassCleanup(cls.provider.close)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="punto-oidc-app-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.origin = f"http://127.0.0.1:{self.port}"
        self.database = self.directory / "punto.db"
        self.environment = dict(os.environ, PORT=str(self.port), BIND_HOST="127.0.0.1",
                                PUBLIC_DIR=str(ROOT / "public"), DB_PATH=str(self.database),
                                PUNTO_EMAIL=EMAIL, PUNTO_PASSWORD=PASSWORD)
        self.environment.update(self.provider.configure(self.directory, self.origin + "/auth/callback"))
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.addCleanup(self.log.close)
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.process = subprocess.Popen([str(ROOT / "build/punto")], cwd=ROOT,
                                        env=self.environment, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                break
            try:
                if self.request("GET", "/healthz")[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(.025)
        self.log.seek(0)
        self.fail("Punto OIDC startup failed: " + self.log.read().decode(errors="replace"))

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)

    def request(self, method, path, cookie=None, fields=None, extra_headers=None):
        headers = {}
        if cookie:
            headers["Cookie"] = cookie
        body = None
        if fields is not None:
            headers.update({"Origin": self.origin, "Content-Type": "application/x-www-form-urlencoded"})
            body = urlencode(fields)
        if extra_headers:
            headers.update(extra_headers)
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            self.assertNotIn(PASSWORD.encode(), payload)
            self.assertNotIn(self.provider.secret.encode(), payload)
            return response.status, response.getheaders(), payload
        finally:
            connection.close()

    def sso(self, **options):
        status, headers, _ = self.request("GET", "/auth/login")
        self.assertEqual(status, 303)
        headers = dict(headers)
        transaction_cookie = headers["Set-Cookie"].split(";", 1)[0]
        query = self.provider.register(headers["Location"], **options)
        return self.request("GET", "/auth/callback?" + query, transaction_cookie)

    def session_cookie(self, headers):
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        self.assertEqual(len(cookies), 2)
        self.assertTrue(any("Max-Age=0" in cookie for cookie in cookies))
        return next(cookie.split(";", 1)[0] for cookie in cookies if cookie.startswith("punto_session="))

    def test_sso_preserves_local_account_and_roles_and_revocation(self):
        status, headers, _ = self.sso()
        self.assertEqual(status, 303)
        self.assertEqual(dict(headers)["Location"], "/app")
        cookie = self.session_cookie(headers)
        status, _, body = self.request("GET", "/api/session", cookie)
        account = json.loads(body)
        self.assertEqual(status, 200)
        self.assertTrue(account["authenticated"])
        self.assertEqual(account["email"], EMAIL)
        self.assertEqual(account["role"], "manager")
        with sqlite3.connect(self.database) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM users").fetchone()[0], 1)
            db.execute("UPDATE users SET role='worker'")
        self.assertEqual(json.loads(self.request("GET", "/api/session", cookie)[2])["role"], "worker")
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE users SET active=0")
        self.assertFalse(json.loads(self.request("GET", "/api/session", cookie)[2])["authenticated"])

    def test_unbound_subject_and_inactive_account_are_denied(self):
        status, headers, _ = self.sso(claims={"sub": "unbound", "email": EMAIL, "email_verified": True})
        self.assertEqual(status, 403)
        self.assertFalse(any(value.startswith("punto_session=") for name, value in headers if name == "Set-Cookie"))
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE users SET active=0")
        self.assertEqual(self.sso()[0], 403)

    def test_alias_reaches_canonical_origin_before_starting_sso(self):
        hostile = "https://attacker.example.test/steal"
        query = urlencode({"redirect_uri": hostile, "next": hostile, "state": "untrusted-state"})
        status, headers, body = self.request("GET", "/auth/login?" + query, extra_headers={
            "Host": f"localhost:{self.port}",
            "Forwarded": 'host="attacker.example.test";proto=https',
            "X-Forwarded-Host": "attacker.example.test",
        })
        self.assertEqual(status, 303)
        self.assertEqual(dict(headers)["Location"], self.origin + "/auth/login")
        self.assertFalse(any(name.lower() == "set-cookie" for name, _ in headers))
        self.assertNotIn(hostile.encode(), body)
        self.assertNotIn("attacker.example.test", str(headers))
        self.assertNotIn("untrusted-state", str(headers))

        # The next request is made on the configured callback origin, so the
        # transaction cookie and the returning callback share the same host.
        status, headers, _ = self.request("GET", urlsplit(dict(headers)["Location"]).path)
        self.assertEqual(status, 303)
        headers = dict(headers)
        authorize = parse_qs(urlsplit(headers["Location"]).query)
        self.assertEqual(authorize["redirect_uri"], [self.origin + "/auth/callback"])
        query = self.provider.register(headers["Location"])
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        status, headers, _ = self.request("GET", "/auth/callback?" + query, cookie)
        self.assertEqual(status, 303)
        session = self.session_cookie(headers)
        self.assertTrue(json.loads(self.request("GET", "/api/session", session)[2])["authenticated"])

    def test_https_public_aliases_redirect_to_configured_sso_origin(self):
        self.stop()
        canonical = "https://punto.example.test"
        self.environment.update({
            "OIDC_REDIRECT_URI": canonical + "/auth/callback",
            "PUBLIC_ORIGINS": canonical + ",https://projects.example.test,http://localhost",
        })
        self.start()
        for host in ("projects.example.test", "projects.example.test:443", "localhost"):
            with self.subTest(host=host):
                status, headers, _ = self.request("GET", "/auth/login", extra_headers={"Host": host})
                self.assertEqual(status, 303)
                self.assertEqual(dict(headers)["Location"], canonical + "/auth/login")
                self.assertFalse(any(name.lower() == "set-cookie" for name, _ in headers))

        # As behind NPM, the application receives HTTP with the canonical Host;
        # PUBLIC_ORIGINS supplies the configured public HTTPS origin.
        for host in ("punto.example.test", "punto.example.test:443"):
            with self.subTest(host=host):
                status, headers, _ = self.request("GET", "/auth/login", extra_headers={"Host": host})
                self.assertEqual(status, 303)
                headers = dict(headers)
                self.assertTrue(headers["Location"].startswith(self.provider.issuer + "/protocol/"))
                self.assertEqual(parse_qs(urlsplit(headers["Location"]).query)["redirect_uri"],
                                 [canonical + "/auth/callback"])
                self.assertIn("; Secure", headers["Set-Cookie"])

    def test_unknown_host_is_rejected_before_sso_redirect(self):
        status, headers, _ = self.request("GET", "/auth/login", extra_headers={
            "Host": "attacker.example.test",
            "X-Forwarded-Host": f"127.0.0.1:{self.port}",
        })
        self.assertEqual(status, 403)
        self.assertFalse(any(name.lower() in ("location", "set-cookie") for name, _ in headers))

    def test_oidc_mode_blocks_local_password_login(self):
        self.stop()
        self.environment["AUTH_MODE"] = "oidc"
        self.start()
        config = json.loads(self.request("GET", "/api/auth/config")[2])
        self.assertEqual(config, {"sso": True, "local": False})
        self.assertEqual(self.request("POST", "/api/login", fields={"email": EMAIL, "password": PASSWORD})[0], 403)
        self.assertEqual(self.sso()[0], 303)

    def test_hybrid_and_local_modes_keep_password_login(self):
        self.assertEqual(json.loads(self.request("GET", "/api/auth/config")[2]), {"sso": True, "local": True})
        self.assertEqual(self.request("POST", "/api/login", fields={"email": EMAIL, "password": PASSWORD})[0], 200)
        self.stop()
        self.environment["AUTH_MODE"] = "local"
        self.start()
        self.assertEqual(json.loads(self.request("GET", "/api/auth/config")[2]), {"sso": False, "local": True})
        self.assertEqual(self.request("GET", "/auth/login")[0], 404)

    def test_invalid_oidc_configuration_fails_before_database_creation(self):
        self.stop()
        missing = self.directory / "must-not-exist.db"
        for mode in ("invalid", "oidc", "hybrid"):
            with self.subTest(mode=mode):
                environment = dict(self.environment, AUTH_MODE=mode, DB_PATH=str(missing), OIDC_CLIENT_ID="")
                process = subprocess.run([str(ROOT / "build/punto")], cwd=ROOT, env=environment,
                                         capture_output=True, timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertFalse(missing.exists())

    def test_unlisted_https_callback_origin_fails_before_database_creation(self):
        self.stop()
        missing = self.directory / "unlisted-callback.db"
        environment = dict(self.environment, DB_PATH=str(missing),
                           OIDC_REDIRECT_URI="https://punto.example.test/auth/callback",
                           PUBLIC_ORIGINS="https://projects.example.test,http://localhost")
        process = subprocess.run([str(ROOT / "build/punto")], cwd=ROOT, env=environment,
                                 capture_output=True, timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b"OIDC_REDIRECT_URI", process.stderr)
        self.assertFalse(missing.exists())
