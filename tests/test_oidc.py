"""OIDC protocol/security tests against actual RSA signatures and local HTTP/TLS.

The module's public API is exercised in a persistent C process. Every account
binding and credential is disposable; no real identity service is contacted.
"""

from contextlib import contextmanager
import base64
import copy
import json
import os
from pathlib import Path
import select
import shlex
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

from oidc_fixture import OIDCProvider
from test_mail_client import create_certificate


ROOT = Path(__file__).resolve().parents[1]


class Driver:
    def __init__(self, binary, environment):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("OIDC_") and key not in
               ("AUTH_MODE", "AUTH_USER_MANAGEMENT", "AUTH_LOCAL_ADMIN_EMAIL")}
        env.update(environment)
        self.process = subprocess.Popen([str(binary)], env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, bufsize=1)
        self.initial = self.read()

    def read(self):
        readable, _, _ = select.select([self.process.stdout], [], [], 10)
        if not readable:
            raise AssertionError("OIDC driver timed out")
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError("OIDC driver exited: " + self.process.stderr.read())
        return json.loads(line)

    def command(self, op, **kwargs):
        self.process.stdin.write(json.dumps(dict(op=op, **kwargs)) + "\n")
        self.process.stdin.flush()
        return self.read()

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        diagnostics = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        expected = 0 if self.initial["initialized"] else 1
        if self.process.returncode != expected or diagnostics:
            raise AssertionError(f"OIDC driver exited {self.process.returncode}: {diagnostics}")


class OIDCProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="punto-oidc-driver-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "oidc-driver"
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "openssl", "libcurl", "json-c"],
                               capture_output=True, text=True, check=True).stdout
        subprocess.run([os.environ.get("CC", "cc"), "-D_POSIX_C_SOURCE=200809L", "-std=c11",
                        "-Wall", "-Wextra", "-Wpedantic", "-Werror", str(ROOT / "tests/oidc_driver.c"),
                        str(ROOT / "auth/oidc.c"), "-o", str(cls.binary), *shlex.split(flags)],
                       capture_output=True, text=True, check=True)
        cls.provider = OIDCProvider()
        cls.addClassCleanup(cls.provider.close)
        cls.original_jwk = copy.deepcopy(cls.provider.jwk)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="punto-oidc-protocol-")
        self.addCleanup(self.directory.cleanup)
        self.env = self.provider.configure(self.directory.name, "http://127.0.0.1:8080/auth/callback",
                                           bindings={self.provider.subject: "Existing@Punto.Local"})
        self.provider.jwk = copy.deepcopy(self.original_jwk)

    @contextmanager
    def driver(self, overrides=None, environment=None):
        env = dict(self.env if environment is None else environment)
        env.update(overrides or {})
        driver = Driver(self.binary, env)
        try:
            yield driver
        finally:
            driver.close()

    def start(self, driver):
        value = driver.command("start")
        self.assertEqual(value["status"], 200, value)
        location = value["location"]
        cookie = value["cookie"].removeprefix("Set-Cookie: ").split(";", 1)[0]
        return location, cookie

    def callback(self, driver, location=None, cookie=None, **changes):
        if location is None:
            location, cookie = self.start(driver)
        query = self.provider.register(location, **changes)
        return driver.command("callback", query=query, cookie=cookie)

    def test_default_local_mode_and_explicit_authentication_modes(self):
        with self.driver(environment={}) as driver:
            self.assertEqual(driver.initial, {"initialized": True, "enabled": False, "local": True})
            self.assertEqual(driver.command("start")["status"], 404)
        for mode, local in (("hybrid", True), ("oidc", False)):
            with self.subTest(mode=mode), self.driver({"AUTH_MODE": mode}) as driver:
                self.assertEqual(driver.initial, {"initialized": True, "enabled": True, "local": local})
        with self.driver(environment={"AUTH_MODE": "local", "OIDC_ISSUER_URL": "bad://config"}) as driver:
            self.assertTrue(driver.initial["initialized"])

    def test_incomplete_or_unknown_mode_fails_closed(self):
        for env in ({"AUTH_MODE": "OIDC"}, {"AUTH_MODE": "hybrid"}, {"AUTH_MODE": "oidc"}):
            with self.subTest(env=env), self.driver(environment=env) as driver:
                self.assertFalse(driver.initial["initialized"])
                self.assertFalse(driver.initial["enabled"])
                self.assertFalse(driver.initial["local"])
        for missing in ("OIDC_ISSUER_URL", "OIDC_CLIENT_ID", "OIDC_CLIENT_SECRET_FILE",
                        "OIDC_BINDINGS_FILE", "OIDC_REDIRECT_URI"):
            env = dict(self.env)
            del env[missing]
            with self.subTest(missing=missing), self.driver(environment=env) as driver:
                self.assertFalse(driver.initial["initialized"])

    def test_http_requires_explicit_opt_in_and_only_loopback_or_keycloak(self):
        invalid = [
            {"OIDC_ALLOW_HTTP": "0"}, {"OIDC_ALLOW_HTTP": "yes"},
            {"OIDC_ISSUER_URL": "http://external.example/realms/punto"},
            {"OIDC_BACKCHANNEL_URL": "http://172.20.0.10/realms/punto"},
            {"OIDC_REDIRECT_URI": "http://external.example/auth/callback"},
            {"OIDC_ISSUER_URL": "http://localhost.attacker.test/realms/punto"},
        ]
        for overrides in invalid:
            with self.subTest(overrides=overrides), self.driver(overrides) as driver:
                self.assertFalse(driver.initial["initialized"])
        with self.driver({"OIDC_BACKCHANNEL_URL": "http://keycloak:8080/realms/punto"}) as driver:
            self.assertTrue(driver.initial["initialized"])

    def test_urls_cannot_add_credentials_paths_fragments_or_alternate_realms(self):
        cases = [("OIDC_ISSUER_URL", self.provider.issuer + "/"),
                 ("OIDC_ISSUER_URL", self.provider.issuer + "?next=evil"),
                 ("OIDC_ISSUER_URL", "https://user:secret@auth.example/realms/punto"),
                 ("OIDC_ISSUER_URL", "https://auth.example/realms/../punto"),
                 ("OIDC_REDIRECT_URI", "https://app.example/auth/callback#fragment"),
                 ("OIDC_REDIRECT_URI", "https://app.example/auth/callback/extra"),
                 ("OIDC_REDIRECT_URI", "HTTPS://app.example/auth/callback"),
                 ("OIDC_REDIRECT_URI", "https://app.example/%61uth/callback"),
                 ("OIDC_BACKCHANNEL_URL", "https://keycloak:8443/realms/other")]
        for name, value in cases:
            with self.subTest(name=name, value=value), self.driver({name: value}) as driver:
                self.assertFalse(driver.initial["initialized"])

    def test_bindings_and_secret_files_are_strict_and_bounded(self):
        mapping = Path(self.env["OIDC_BINDINGS_FILE"])
        invalid = [b'[]', b'{"subject":"not-an-email"}', b'{"":"valid@example.test"}',
                   b'{"subject":null}', b'{"subject":"one@example.test","subject":"two@example.test"}',
                   b'{"subject":"one@example.test","subje\\u0063t":"two@example.test"}',
                   b'{"subject":"one@example.test\\u0000"}', b'{"subject":"bad\xff@example.test"}',
                   b'{} {}', b'{"subject":"one@example.test",}', b'{"subject":"one@example.test"}\x00']
        for contents in invalid:
            mapping.write_bytes(contents)
            with self.subTest(contents=contents), self.driver() as driver:
                self.assertFalse(driver.initial["initialized"])
        mapping.write_text(json.dumps({self.provider.subject: "valid@example.test"}))
        mapping.chmod(0o666)
        with self.driver() as driver:
            self.assertFalse(driver.initial["initialized"])
        mapping.chmod(0o600)
        link = Path(self.directory.name) / "mapping-link.json"
        link.symlink_to(mapping)
        with self.driver({"OIDC_BINDINGS_FILE": str(link)}) as driver:
            self.assertFalse(driver.initial["initialized"])
        secret = Path(self.env["OIDC_CLIENT_SECRET_FILE"])
        secret.write_text("credential\nextra\n")
        with self.driver() as driver:
            self.assertFalse(driver.initial["initialized"])

    def test_start_binds_nonce_pkce_browser_and_separate_client_cookie(self):
        with self.driver({"OIDC_REDIRECT_URI": "https://app.example/auth/callback"}) as driver:
            first = driver.command("start")
            second = driver.command("start")
            self.assertEqual(first["status"], 200)
            params = parse_qs(urlsplit(first["location"]).query)
            self.assertEqual(params["response_type"], ["code"])
            self.assertEqual(params["scope"], ["openid"])
            self.assertEqual(params["code_challenge_method"], ["S256"])
            for field in ("state", "nonce", "code_challenge"):
                self.assertRegex(params[field][0], r"\A[A-Za-z0-9_-]{43}\Z")
            self.assertNotEqual(params["state"], parse_qs(urlsplit(second["location"]).query)["state"])
            self.assertIn("Path=/auth/; HttpOnly; SameSite=Lax; Max-Age=300; Secure\r\n", first["cookie"])
            self.assertNotIn(self.provider.secret, first["location"])
            cookie_name = first["cookie"].split("=", 1)[0]
        for change in ({"OIDC_CLIENT_ID": "another-app"},
                       {"OIDC_REDIRECT_URI": "https://another.example/auth/callback"}):
            with self.driver(change) as driver:
                self.assertNotEqual(driver.command("start")["cookie"].split("=", 1)[0], cookie_name)

    def test_roundtrip_verifies_real_signature_pkce_and_uses_only_explicit_binding(self):
        with self.driver({"HTTP_PROXY": "http://127.0.0.1:1", "http_proxy": "http://127.0.0.1:1",
                          "ALL_PROXY": "http://127.0.0.1:1"}) as driver:
            value = self.callback(driver)
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["email"], "existing@punto.local")
            self.assertTrue(297 <= value["lifetime"] <= 300)
            self.assertIn("Path=/auth/; HttpOnly; SameSite=Lax; Max-Age=0", value["clear_cookie"])
            sent = self.provider.token_requests[-1]
            self.assertEqual(sent["client_id"], ["punto"])
            self.assertEqual(sent["client_secret"], [self.provider.secret])
            self.assertRegex(sent["code_verifier"][0], r"\A[A-Za-z0-9_-]{43}\Z")

    def test_unknown_subject_is_denied_even_with_matching_email_or_administrator_claim(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location, claims={"sub": "unbound", "email": "existing@punto.local",
                                                            "realm_access": {"roles": ["manager", "admin"]}})
            value = driver.command("callback", query=query, cookie=cookie)
            self.assertEqual(value["status"], 403)
            self.assertEqual((value["email"], value["lifetime"]), ("", 0))
            requests = len(self.provider.token_requests)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests)

    def test_callback_requires_matching_browser_and_is_single_use(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location)
            requests = len(self.provider.token_requests)
            for bad_cookie in ("", cookie[:-1] + ("A" if cookie[-1] != "A" else "B"),
                               cookie + "; " + cookie):
                value = driver.command("callback", query=query, cookie=bad_cookie)
                self.assertEqual(value["status"], 401)
                self.assertEqual(value["email"], "")
            self.assertEqual(len(self.provider.token_requests), requests)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)
            new_query = self.provider.register(location)
            self.assertEqual(driver.command("callback", query=new_query, cookie=cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests + 1)

    def test_encrypted_cookie_rejects_tampering_and_cross_browser_state_before_exchange(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            _, other_cookie = self.start(driver)
            query = self.provider.register(location)
            name, value = cookie.split("=", 1)
            sealed = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
            params = parse_qs(urlsplit(location).query)
            for field in ("state", "nonce"):
                self.assertNotIn(params[field][0].encode(), sealed)
            requests = len(self.provider.token_requests)
            for index in (0, 11, 12, 32, len(sealed) - 17, len(sealed) - 1):
                changed = bytearray(sealed)
                changed[index] ^= 1
                tampered = name + "=" + base64.urlsafe_b64encode(changed).rstrip(b"=").decode()
                with self.subTest(index=index):
                    self.assertEqual(driver.command("callback", query=query, cookie=tampered)["status"], 401)
            self.assertEqual(driver.command("callback", query=query, cookie=other_cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)

    def test_cookie_is_invalid_after_process_restart(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
        query = self.provider.register(location)
        requests = len(self.provider.token_requests)
        with self.driver() as driver:
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)
        self.assertEqual(len(self.provider.token_requests), requests)

    def test_expired_state_is_denied_without_contacting_identity_service(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location)
            requests = len(self.provider.token_requests)
            driver.command("advance", seconds=301)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests)

    def test_duplicate_malformed_and_unexpected_callback_fields_fail_before_exchange(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location)
            requests = len(self.provider.token_requests)
            invalid = [query + "&state=" + "A" * 43, query + "&code=another", query + "&iss=x",
                       query + "&redirect_uri=https://evil.invalid", query + "&error=denied",
                       query + "&error_description=%00", query + "&bad=%QQ", query + "&"]
            for bad_query in invalid:
                with self.subTest(query=bad_query):
                    self.assertEqual(driver.command("callback", query=bad_query, cookie=cookie)["status"], 400)
            self.assertEqual(len(self.provider.token_requests), requests)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)

    def test_provider_error_or_wrong_response_issuer_is_denied_without_exchange(self):
        with self.driver() as driver:
            for error in (False, True):
                location, cookie = self.start(driver)
                params = parse_qs(urlsplit(location).query)
                fields = {"state": params["state"][0]}
                fields.update({"error": "access_denied"} if error else {"code": "code", "iss": "https://other.invalid/realms/punto"})
                query = urlencode(fields)
                self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)
                self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 401)

    def test_invalid_signed_claims_are_rejected(self):
        now = int(time.time())
        cases = [{"iss": "https://other.example/realms/punto"}, {"aud": "another-client"},
                 {"azp": "another-client"}, {"nonce": "A" * 43}, {"sub": ""},
                 {"exp": now - 1}, {"iat": now + 120}, {"iat": now - 3600},
                 {"exp": str(now + 300)}, {"exp": float(now + 300)}, {"iat": True},
                 {"nbf": now + 120}, {"nbf": "soon"},
                 {"aud": ["punto", "another-client"], "azp": None}]
        with self.driver() as driver:
            for claims in cases:
                with self.subTest(claims=claims):
                    value = self.callback(driver, claims=claims)
                    self.assertEqual(value["status"], 401, value)
                    self.assertEqual((value["email"], value["lifetime"]), ("", 0))
            value = self.callback(driver, claims={"aud": ["another-client", "punto"], "azp": "punto"})
            self.assertEqual(value["status"], 200, value)

    def test_signature_algorithm_key_and_critical_header_are_verified(self):
        cases = [{"header": {"alg": "none"}}, {"header": {"alg": "HS256"}},
                 {"header": {"kid": "unknown"}}, {"header": {"crit": ["custom"]}},
                 {"bad_signature": True}]
        with self.driver() as driver:
            for changes in cases:
                with self.subTest(changes=changes):
                    self.assertEqual(self.callback(driver, **changes)["status"], 401)
            before = self.provider.jwks_requests
            self.assertEqual(self.callback(driver, header={"jku": "http://127.0.0.1:1/untrusted",
                                                           "x5u": "http://127.0.0.1:1/untrusted"})["status"], 200)
            self.assertEqual(self.provider.jwks_requests, before + 1)

    def test_unusable_jwks_keys_fail_closed(self):
        with self.driver() as driver:
            for fields in ({"alg": "HS256"}, {"use": "enc"}, {"kty": "oct"}, {"e": "AA"},
                           {"n": "AQAB"}, {"n": self.original_jwk["n"] + "="},
                           {"key_ops": ["sign"]}):
                self.provider.jwk = dict(self.original_jwk, **fields)
                with self.subTest(fields=fields):
                    self.assertEqual(self.callback(driver)["status"], 401)

    def test_session_lifetime_uses_earliest_provider_limit_and_eight_hour_cap(self):
        with self.driver() as driver:
            for expiry, expires_in, expected in ((300, 120, 120), (90, 300, 90), (100000, 100000, 28800)):
                with self.subTest(expiry=expiry, expires_in=expires_in):
                    value = self.callback(driver, claims={"exp": int(time.time()) + expiry},
                                          extra_token={"expires_in": expires_in})
                    self.assertEqual(value["status"], 200, value)
                    self.assertTrue(expected - 2 <= value["lifetime"] <= expected, value)
            for extra in ({"expires_in": 0}, {"expires_in": "300"}, {"token_type": "Basic"}):
                with self.subTest(extra=extra):
                    self.assertEqual(self.callback(driver, extra_token=extra)["status"], 503)

    def test_anonymous_start_flood_does_not_block_or_evict_existing_logins(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location)
            for _ in range(512):
                self.assertEqual(driver.command("start")["status"], 200)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)
            self.assertEqual(self.callback(driver)["status"], 200)

    def test_anonymous_error_callbacks_do_not_consume_shared_replay_capacity(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location)
            requests = len(self.provider.token_requests)
            for _ in range(128):
                abandoned, abandoned_cookie = self.start(driver)
                state = parse_qs(urlsplit(abandoned).query)["state"][0]
                self.assertEqual(driver.command("callback", query=urlencode(dict(state=state, error="access_denied")),
                                                cookie=abandoned_cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)

    def test_authenticated_replay_quota_is_per_subject_and_records_expire(self):
        mapping = Path(self.env["OIDC_BINDINGS_FILE"])
        mapping.write_text(json.dumps({self.provider.subject: "existing@punto.local",
                                       "other-subject": "other@punto.local"}))
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location, claims={"sub": "other-subject"})
            first_location, first_cookie = self.start(driver)
            first_query = self.provider.register(first_location)
            self.assertEqual(driver.command("callback", query=first_query, cookie=first_cookie)["status"], 200)
            for _ in range(63):
                self.assertEqual(self.callback(driver)["status"], 200)
            self.assertEqual(self.callback(driver)["status"], 429)
            self.assertEqual(driver.command("start")["status"], 200)
            self.assertEqual(driver.command("callback", query=query, cookie=cookie)["status"], 200)
            requests = len(self.provider.token_requests)
            self.assertEqual(driver.command("callback", query=first_query, cookie=first_cookie)["status"], 401)
            self.assertEqual(len(self.provider.token_requests), requests)
            driver.command("advance", seconds=301)
            self.assertEqual(self.callback(driver, claims={"iat": int(time.time()) + 301,
                                                         "exp": int(time.time()) + 601})["status"], 200)

    def test_backchannel_does_not_follow_redirects_and_bounds_remote_json(self):
        base = self.provider.server.RequestHandlerClass

        class Redirect(base):
            def do_POST(self):
                self.send_response(302)
                self.send_header("Location", self.path.replace("token", "certs"))
                self.send_header("Content-Length", "0")
                self.end_headers()

        self.provider.server.RequestHandlerClass = Redirect
        try:
            before = self.provider.jwks_requests
            with self.driver() as driver:
                self.assertEqual(self.callback(driver)["status"], 503)
            self.assertEqual(self.provider.jwks_requests, before)
        finally:
            self.provider.server.RequestHandlerClass = base
        self.provider.jwk = dict(self.original_jwk, x5c=["A" * (256 * 1024)])
        with self.driver() as driver:
            self.assertEqual(self.callback(driver)["status"], 503)

    def test_tls_requires_a_trusted_ca_and_matching_server_name(self):
        with OIDCProvider() as provider:
            certificate, key = create_certificate(self.directory.name, "oidc-api")
            provider.server.shutdown()
            provider.thread.join(timeout=3)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(certificate), str(key))
            provider.server.socket = context.wrap_socket(provider.server.socket, server_side=True)
            provider.issuer = provider.issuer.replace("http://", "https://")
            provider.thread = threading.Thread(target=provider.server.serve_forever, daemon=True)
            provider.thread.start()
            env = provider.configure(Path(self.directory.name) / "tls", "https://app.example/auth/callback")
            env.pop("OIDC_ALLOW_HTTP")
            for trusted in (False, True):
                environment = dict(env)
                if trusted:
                    environment["OIDC_CA_FILE"] = str(certificate)
                with self.subTest(trusted=trusted), self.driver(environment=environment) as driver:
                    self.assertTrue(driver.initial["initialized"])
                    location, cookie = self.start(driver)
                    query = provider.register(location)
                    value = driver.command("callback", query=query, cookie=cookie)
                    self.assertEqual(value["status"], 200 if trusted else 503, value)
                    self.assertIn("; Secure\r\n", value["clear_cookie"])
            wrong_cert, wrong_key = create_certificate(self.directory.name, "wrong-api", "wrong.example")
            context.load_cert_chain(str(wrong_cert), str(wrong_key))
            with self.driver(environment=dict(env, OIDC_CA_FILE=str(wrong_cert))) as driver:
                location, cookie = self.start(driver)
                value = driver.command("callback", query=provider.register(location), cookie=cookie)
                self.assertEqual(value["status"], 503, value)


if __name__ == "__main__":
    unittest.main()
