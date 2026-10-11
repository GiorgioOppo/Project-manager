"""Browser-facing SSO failures and explicit, origin-protected account logout."""

import json
import sqlite3
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

import test_punto_oidc as legacy
from oidc_fixture import OIDCProvider


class PuntoSSOErrorTests(unittest.TestCase):
    setUp = legacy.PuntoOIDCTests.setUp
    start = legacy.PuntoOIDCTests.start
    stop = legacy.PuntoOIDCTests.stop
    request = legacy.PuntoOIDCTests.request
    sso = legacy.PuntoOIDCTests.sso
    session_cookie = legacy.PuntoOIDCTests.session_cookie

    @classmethod
    def setUpClass(cls):
        cls.provider = OIDCProvider()
        cls.addClassCleanup(cls.provider.close)

    def local_cookie(self):
        status, headers, _ = self.request("POST", "/api/login", fields={
            "email": legacy.EMAIL, "password": legacy.PASSWORD,
        })
        self.assertEqual(status, 200)
        return dict(headers)["Set-Cookie"].split(";", 1)[0]

    def authenticated(self, cookie):
        status, _, body = self.request("GET", "/api/session", cookie)
        self.assertEqual(status, 200)
        return json.loads(body)["authenticated"]

    def begin_sso(self, cookie=None, **options):
        status, headers, _ = self.request("GET", "/auth/login", cookie)
        self.assertEqual(status, 303)
        headers = dict(headers)
        transaction = headers["Set-Cookie"].split(";", 1)[0]
        return self.provider.register(headers["Location"], **options), transaction

    def assert_private_response(self, headers):
        headers = {name.lower(): value for name, value in headers}
        self.assertEqual(headers["cache-control"], "no-store")
        self.assertEqual(headers["referrer-policy"], "no-referrer")
        self.assertEqual(headers["x-content-type-options"], "nosniff")
        self.assertIn("frame-ancestors 'none'", headers["content-security-policy"])

    def assert_error_page(self, status, headers, body, expected):
        self.assertEqual(status, expected)
        self.assertTrue(dict(headers)["Content-Type"].startswith("text/html;"))
        self.assertIn(b"<!doctype html>", body.lower())
        self.assertIn(b'id="sso-error-logout"', body)
        self.assertIn("Accesso aziendale non riuscito".encode(), body)
        self.assertNotIn(b'"ok":false', body)
        self.assert_private_response(headers)

    def assert_clear_cookies(self, headers, transaction_name=None):
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        app = [value for value in cookies if value.startswith("punto_session=")]
        transaction = [value for value in cookies if value.startswith("punto_oidc_")]
        self.assertEqual(len(app), 1)
        self.assertEqual(len(transaction), 1)
        for value in (app[0], transaction[0]):
            self.assertIn("Max-Age=0", value)
            self.assertIn("HttpOnly", value)
            self.assertIn("SameSite=Lax", value)
            self.assertEqual(value.split(";", 1)[0].split("=", 1)[1], "")
        self.assertIn("Path=/;", app[0])
        self.assertIn("Path=/auth/;", transaction[0])
        if transaction_name:
            self.assertTrue(transaction[0].startswith(transaction_name + "="))

    def assert_logout_url(self, body, origin=None):
        result = json.loads(body)
        self.assertEqual(set(result), {"ok", "logout_url"})
        self.assertIs(result["ok"], True)
        destination = urlsplit(result["logout_url"])
        provider = urlsplit(self.provider.issuer)
        self.assertEqual((destination.scheme, destination.netloc),
                         (provider.scheme, provider.netloc))
        self.assertEqual(destination.path, provider.path + "/protocol/openid-connect/logout")
        self.assertEqual(parse_qs(destination.query), {
            "client_id": ["punto"], "post_logout_redirect_uri": [(origin or self.origin) + "/"],
        })
        self.assertEqual(destination.fragment, "")
        self.assertNotIn(b"id_token", body)
        self.assertNotIn(b"attacker.example.test", body)

    def test_invalid_callback_is_html_and_never_reflects_callback_credentials(self):
        values = {"code": "private-authorization-code", "state": "private-transaction-state".ljust(43, "x"),
                  "error_description": "<script>callback-injection-marker</script>"}
        status, headers, body = self.request("GET", "/auth/callback?" + urlencode(values))
        self.assert_error_page(status, headers, body, 401)
        response = body + str(headers).encode()
        for value in values.values():
            self.assertNotIn(value.encode(), response)
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        self.assertEqual(len(cookies), 1)
        self.assertTrue(cookies[0].startswith("punto_oidc_"))
        self.assertIn("Max-Age=0", cookies[0])

    def test_invalid_provider_signature_uses_same_error_page(self):
        status, headers, body = self.sso(bad_signature=True)
        self.assert_error_page(status, headers, body, 401)

    def test_unlinked_account_error_preserves_an_existing_application_session(self):
        cookie = self.local_cookie()
        query, transaction = self.begin_sso(cookie, claims={"sub": "unlinked-subject"})
        status, headers, body = self.request("GET", "/auth/callback?" + query,
                                           cookie + "; " + transaction)
        self.assert_error_page(status, headers, body, 403)
        self.assertFalse(any(value.startswith("punto_session=") for name, value in headers
                             if name.lower() == "set-cookie"))
        self.assertTrue(self.authenticated(cookie))
        self.assertNotIn(parse_qs(query)["code"][0].encode(), body)

    def test_database_unavailability_keeps_503_and_shows_the_error_page(self):
        query, transaction = self.begin_sso()
        with sqlite3.connect(self.database) as db:
            db.execute("ALTER TABLE users RENAME TO temporarily_unavailable_users")
        try:
            status, headers, body = self.request("GET", "/auth/callback?" + query, transaction)
            self.assert_error_page(status, headers, body, 503)
        finally:
            with sqlite3.connect(self.database) as db:
                db.execute("ALTER TABLE temporarily_unavailable_users RENAME TO users")

    def test_error_page_can_be_reloaded_without_repeating_the_callback(self):
        status, headers, body = self.request("GET", "/auth/error")
        self.assert_error_page(status, headers, body, 200)
        status, head_headers, head_body = self.request("HEAD", "/auth/error")
        self.assertEqual(status, 200)
        self.assertEqual(head_body, b"")
        self.assertEqual(dict(head_headers)["Content-Length"], str(len(body)))
        self.assertTrue(dict(head_headers)["Content-Type"].startswith("text/html;"))
        self.assert_private_response(head_headers)
        self.assertEqual(self.request("GET", "/auth/error?code=private-code")[0], 400)
        self.assertEqual(self.request("POST", "/auth/error", fields={})[0], 405)

    def test_error_page_accepts_only_one_supported_language_parameter(self):
        for language in ("it", "en", "zh", "es", "fr", "de"):
            for method in ("GET", "HEAD"):
                with self.subTest(language=language, method=method):
                    status, headers, body = self.request(method, "/auth/error?lang=" + language)
                    self.assertEqual(status, 200)
                    self.assertTrue(dict(headers)["Content-Type"].startswith("text/html;"))
                    self.assert_private_response(headers)
                    if method == "HEAD":
                        self.assertEqual(body, b"")
                    else:
                        self.assertIn(b'id="sso-error-logout"', body)

        invalid = ("code=private-code", "next=https://attacker.example.test", "lang=xx",
                   "lang=", "lang=en&code=private-code", "lang=en&next=/app",
                   "lang=en&lang=fr", "lang=en&lang=en")
        for query in invalid:
            for method in ("GET", "HEAD"):
                with self.subTest(query=query, method=method):
                    status, headers, body = self.request(method, "/auth/error?" + query)
                    self.assertEqual(status, 400)
                    self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                         for name, _ in headers))
                    self.assertNotIn(b"private-code", body)
                    self.assertNotIn(b"attacker.example.test", body)
                    if method == "HEAD":
                        self.assertEqual(body, b"")

    def test_unauthenticated_logout_returns_a_fixed_provider_destination(self):
        status, headers, body = self.request("POST", "/auth/logout", fields={})
        self.assertEqual(status, 200)
        self.assertTrue(dict(headers)["Content-Type"].startswith("application/json;"))
        self.assertNotIn("Location", dict(headers))
        self.assert_logout_url(body)
        self.assert_clear_cookies(headers)
        self.assert_private_response(headers)

    def test_logout_revokes_only_the_current_application_session(self):
        status, headers, _ = self.sso()
        self.assertEqual(status, 303)
        first = self.session_cookie(headers)
        status, headers, _ = self.sso()
        self.assertEqual(status, 303)
        second = self.session_cookie(headers)
        _, transaction = self.begin_sso(first)
        status, headers, body = self.request("POST", "/auth/logout",
                                           first + "; " + transaction, fields={})
        self.assertEqual(status, 200)
        self.assert_clear_cookies(headers, transaction.split("=", 1)[0])
        self.assert_logout_url(body)
        self.assertFalse(self.authenticated(first))
        self.assertTrue(self.authenticated(second))

    def test_method_origin_and_query_rejections_do_not_revoke_a_session(self):
        cookie = self.local_cookie()
        attempts = [
            ("GET", "/auth/logout", {}, 405),
            ("HEAD", "/auth/logout", {}, 405),
            ("PUT", "/auth/logout", {"Origin": self.origin}, 405),
            ("POST", "/auth/logout", {}, 403),
            ("POST", "/auth/logout", {"Origin": "https://attacker.example.test"}, 403),
            ("POST", "/auth/logout", {"Origin": self.origin, "Sec-Fetch-Site": "cross-site"}, 403),
            ("POST", "/auth/logout?next=https://attacker.example.test",
             {"Origin": self.origin}, 400),
        ]
        for method, target, headers, expected in attempts:
            with self.subTest(method=method, target=target, origin=headers.get("Origin")):
                status, response_headers, _ = self.request(method, target, cookie,
                                                           extra_headers=headers)
                self.assertEqual(status, expected)
                self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                     for name, _ in response_headers))
                self.assertTrue(self.authenticated(cookie))

    def test_public_alias_and_forwarded_headers_cannot_select_logout_destination(self):
        self.stop()
        canonical = "https://punto.example.test"
        alias = "https://projects.example.test"
        self.environment.update(OIDC_REDIRECT_URI=canonical + "/auth/callback",
                                PUBLIC_ORIGINS=canonical + "," + alias)
        self.start()
        status, headers, body = self.request("POST", "/auth/logout", extra_headers={
            "Host": "projects.example.test", "Origin": alias,
            "Forwarded": 'host="attacker.example.test";proto=https',
            "X-Forwarded-Host": "attacker.example.test",
        })
        self.assertEqual(status, 200)
        self.assert_logout_url(body, canonical)
        self.assert_clear_cookies(headers)
        self.assertTrue(all("; Secure" in value for name, value in headers
                            if name.lower() == "set-cookie"))

    def test_local_mode_has_no_provider_logout_or_error_route(self):
        self.stop()
        self.environment["AUTH_MODE"] = "local"
        self.start()
        cookie = self.local_cookie()
        for method, target in (("GET", "/auth/error"), ("POST", "/auth/logout")):
            with self.subTest(method=method, target=target):
                status, headers, _ = self.request(method, target, cookie,
                                                   extra_headers={"Origin": self.origin})
                self.assertEqual(status, 404)
                self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                     for name, _ in headers))
                self.assertTrue(self.authenticated(cookie))


if __name__ == "__main__":
    unittest.main()
