"""Ticket SSO error pages and anonymous, explicit provider logout over HTTP."""

import http.client
import json
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

import test_tickets as ticket_http
import test_tickets_oidc as legacy


class TicketSSOErrorTests(unittest.TestCase):
    start = ticket_http.TicketApplicationTests.start
    stop = ticket_http.TicketApplicationTests.stop
    request = ticket_http.TicketApplicationTests.request
    api = ticket_http.TicketApplicationTests.api
    login = ticket_http.TicketApplicationTests.login
    enable_sso = legacy.TicketOIDCTests.enable_sso

    @classmethod
    def setUpClass(cls):
        ticket_http.TicketApplicationTests.setUpClass()
        cls.binary = ticket_http.TicketApplicationTests.binary

    def setUp(self):
        ticket_http.TicketApplicationTests.setUp(self)
        self.enable_sso()

    def raw_request(self, method, target, cookie=None, body=None, headers=None):
        request_headers = dict(headers or {})
        if cookie:
            request_headers["Cookie"] = cookie
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)
        try:
            connection.request(method, target, body=body, headers=request_headers)
            response = connection.getresponse()
            payload = response.read()
            self.assertNotIn(ticket_http.PASSWORD.encode(), payload)
            self.assertNotIn(self.provider.secret.encode(), payload)
            return response.status, response.getheaders(), payload
        finally:
            connection.close()

    def begin_sso(self, cookie=None, **options):
        status, headers, _ = self.raw_request("GET", "/auth/login", cookie)
        self.assertEqual(status, 303)
        headers = dict(headers)
        query = self.provider.register(headers["Location"], **options)
        return query, headers["Set-Cookie"].split(";", 1)[0]

    def sso(self, **options):
        query, cookie = self.begin_sso(**options)
        return self.raw_request("GET", "/auth/callback?" + query, cookie)

    def session_cookie(self, headers):
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        return next(value.split(";", 1)[0] for value in cookies
                    if value.startswith("tickets_session="))

    def authenticated(self, cookie):
        status, _, _ = self.raw_request("GET", "/api/session", cookie)
        self.assertIn(status, (200, 401))
        return status == 200

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

    def assert_clear_cookies(self, headers, transaction_name=None, secure=False):
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        application = [value for value in cookies if value.startswith("tickets_session=")]
        transaction = [value for value in cookies if value.startswith("punto_oidc_")]
        self.assertEqual(len(application), 1)
        self.assertEqual(len(transaction), 1)
        for value in (application[0], transaction[0]):
            self.assertIn("Max-Age=0", value)
            self.assertIn("HttpOnly", value)
            self.assertEqual(value.split(";", 1)[0].split("=", 1)[1], "")
            if secure:
                self.assertIn("; Secure", value)
        self.assertIn("Path=/;", application[0])
        self.assertIn("SameSite=Strict", application[0])
        self.assertIn("Path=/auth/;", transaction[0])
        self.assertIn("SameSite=Lax", transaction[0])
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
            "client_id": ["punto-tickets"],
            "post_logout_redirect_uri": [(origin or self.origin) + "/"],
        })
        self.assertEqual(destination.fragment, "")
        self.assertNotIn(b"id_token", body)
        self.assertNotIn(b"attacker.example.test", body)

    def test_invalid_callback_is_html_without_reflecting_callback_credentials(self):
        values = {"code": "private-authorization-code", "state": "private-state".ljust(43, "x"),
                  "error_description": "<script>callback-injection-marker</script>"}
        status, headers, body = self.raw_request("GET", "/auth/callback?" + urlencode(values))
        self.assert_error_page(status, headers, body, 401)
        for value in values.values():
            self.assertNotIn(value.encode(), body + str(headers).encode())
        cookies = [value for name, value in headers if name.lower() == "set-cookie"]
        self.assertEqual(len(cookies), 1)
        self.assertTrue(cookies[0].startswith("punto_oidc_"))
        self.assertIn("Max-Age=0", cookies[0])

    def test_invalid_signature_uses_the_html_error_page(self):
        status, headers, body = self.sso(bad_signature=True)
        self.assert_error_page(status, headers, body, 401)

    def test_unlinked_account_error_preserves_the_existing_ticket_session(self):
        current = self.login()
        cookie = current["cookie"]
        query, transaction = self.begin_sso(cookie, claims={"sub": "unlinked-subject"})
        status, headers, body = self.raw_request("GET", "/auth/callback?" + query,
                                               cookie + "; " + transaction)
        self.assert_error_page(status, headers, body, 403)
        self.assertFalse(any(value.startswith("tickets_session=") for name, value in headers
                             if name.lower() == "set-cookie"))
        self.assertTrue(self.authenticated(cookie))
        self.assertNotIn(parse_qs(query)["code"][0].encode(), body)

    def test_error_page_reloads_with_get_and_head(self):
        status, headers, body = self.raw_request("GET", "/auth/error")
        self.assert_error_page(status, headers, body, 200)
        status, head_headers, head_body = self.raw_request("HEAD", "/auth/error")
        self.assertEqual(status, 200)
        self.assertEqual(head_body, b"")
        self.assertEqual(dict(head_headers)["Content-Length"], str(len(body)))
        self.assertTrue(dict(head_headers)["Content-Type"].startswith("text/html;"))
        self.assert_private_response(head_headers)
        self.assertEqual(self.raw_request("POST", "/auth/error", headers={"Origin": self.origin})[0], 405)

    def test_error_page_accepts_only_one_of_the_six_language_values(self):
        for language in ("it", "en", "zh", "es", "fr", "de"):
            for method in ("GET", "HEAD"):
                with self.subTest(language=language, method=method):
                    status, headers, body = self.raw_request(method, "/auth/error?lang=" + language)
                    self.assertEqual(status, 200)
                    self.assertTrue(dict(headers)["Content-Type"].startswith("text/html;"))
                    self.assert_private_response(headers)
                    if method == "HEAD":
                        self.assertEqual(body, b"")
                    else:
                        self.assertIn(b'id="sso-error-logout"', body)
        for query in ("code=private-code", "next=https://attacker.example.test", "lang=xx", "lang=",
                      "lang=en&code=private-code", "lang=en&next=/app", "lang=en&lang=fr", "lang=en&lang=en"):
            for method in ("GET", "HEAD"):
                with self.subTest(query=query, method=method):
                    status, headers, body = self.raw_request(method, "/auth/error?" + query)
                    self.assertEqual(status, 400)
                    self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                         for name, _ in headers))
                    self.assertNotIn(b"private-code", body)
                    self.assertNotIn(b"attacker.example.test", body)
                    if method == "HEAD":
                        self.assertEqual(body, b"")

    def test_anonymous_logout_needs_no_csrf_and_returns_the_fixed_provider_url(self):
        status, headers, body = self.raw_request("POST", "/auth/logout", headers={"Origin": self.origin})
        self.assertEqual(status, 200)
        self.assertTrue(dict(headers)["Content-Type"].startswith("application/json;"))
        self.assertNotIn("Location", dict(headers))
        self.assert_logout_url(body)
        self.assert_clear_cookies(headers)
        self.assert_private_response(headers)

    def test_logout_revokes_only_the_current_ticket_session(self):
        status, headers, _ = self.sso()
        self.assertEqual(status, 303)
        first = self.session_cookie(headers)
        status, headers, _ = self.sso()
        self.assertEqual(status, 303)
        second = self.session_cookie(headers)
        _, transaction = self.begin_sso(first)
        status, headers, body = self.raw_request("POST", "/auth/logout", first + "; " + transaction,
                                               headers={"Origin": self.origin})
        self.assertEqual(status, 200)
        self.assert_clear_cookies(headers, transaction.split("=", 1)[0])
        self.assert_logout_url(body)
        self.assertFalse(self.authenticated(first))
        self.assertTrue(self.authenticated(second))

    def test_rejected_logout_requests_do_not_revoke_the_ticket_session(self):
        cookie = self.login()["cookie"]
        attempts = [
            ("GET", "/auth/logout", {}, None, 405),
            ("HEAD", "/auth/logout", {}, None, 405),
            ("PUT", "/auth/logout", {"Origin": self.origin}, None, 405),
            ("POST", "/auth/logout", {}, None, 403),
            ("POST", "/auth/logout", {"Origin": "https://attacker.example.test"}, None, 403),
            ("POST", "/auth/logout?next=https://attacker.example.test", {"Origin": self.origin}, None, 400),
            ("POST", "/auth/logout", {"Origin": self.origin}, "{}", 400),
            ("POST", "/auth/logout", {"Origin": self.origin}, " ", 400),
            ("POST", "/auth/logout", {"Origin": self.origin}, b"\0", 400),
        ]
        for method, target, headers, body, expected in attempts:
            with self.subTest(method=method, target=target, origin=headers.get("Origin"), body=body):
                status, response_headers, _ = self.raw_request(method, target, cookie, body, headers)
                self.assertEqual(status, expected)
                self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                     for name, _ in response_headers))
                self.assertTrue(self.authenticated(cookie))

    def test_public_origin_and_forwarded_headers_cannot_choose_the_return_url(self):
        self.stop()
        canonical = "https://ticket.example.test"
        self.environment.update(OIDC_REDIRECT_URI=canonical + "/auth/callback",
                                TICKETS_PUBLIC_ORIGIN=canonical)
        self.start()
        status, headers, body = self.raw_request("POST", "/auth/logout", headers={
            "Host": "ticket.example.test", "Origin": canonical,
            "Forwarded": 'host="attacker.example.test";proto=https',
            "X-Forwarded-Host": "attacker.example.test",
        })
        self.assertEqual(status, 200)
        self.assert_logout_url(body, canonical)
        self.assert_clear_cookies(headers, secure=True)

    def test_local_mode_has_no_provider_logout_and_keeps_the_session(self):
        self.stop()
        self.environment["AUTH_MODE"] = "local"
        self.start()
        cookie = self.login()["cookie"]
        for method, target in (("GET", "/auth/error"), ("POST", "/auth/logout")):
            with self.subTest(method=method, target=target):
                status, headers, _ = self.raw_request(method, target, cookie,
                                                       headers={"Origin": self.origin})
                self.assertEqual(status, 404)
                self.assertFalse(any(name.lower() in ("set-cookie", "location")
                                     for name, _ in headers))
                self.assertTrue(self.authenticated(cookie))


if __name__ == "__main__":
    unittest.main()
