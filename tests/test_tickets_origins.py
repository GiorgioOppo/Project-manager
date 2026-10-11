"""Security boundaries for multiple public Ticket origins behind one proxy."""

from pathlib import Path
import subprocess
import unittest

from oidc_fixture import OIDCProvider
import test_tickets as ticket_http


HTTPS_ORIGIN = "https://ticket.tuaazienda.it"
HTTP_ORIGIN = "http://ticket.localhost"


class TicketOriginTests(unittest.TestCase):
    start = ticket_http.TicketApplicationTests.start
    stop = ticket_http.TicketApplicationTests.stop
    request = ticket_http.TicketApplicationTests.request
    api = ticket_http.TicketApplicationTests.api

    @classmethod
    def setUpClass(cls):
        ticket_http.TicketApplicationTests.setUpClass()
        cls.binary = ticket_http.TicketApplicationTests.binary

    def setUp(self):
        ticket_http.TicketApplicationTests.setUp(self)
        self.stop()
        self.environment.update({
            "TICKETS_PUBLIC_ORIGIN": HTTPS_ORIGIN,
            "TICKETS_ADDITIONAL_ORIGINS": HTTP_ORIGIN,
        })
        self.start()

    def origin_headers(self, origin, **extra):
        return {"Host": origin.split("://", 1)[1], "Origin": origin, **extra}

    def origin_login(self, origin, **extra):
        body, headers = self.api("POST", "/api/login", {
            "email": "admin@example.test", "password": ticket_http.PASSWORD,
        }, headers=self.origin_headers(origin, **extra))
        cookie = headers["set-cookie"]
        attributes = {part.strip().lower() for part in cookie.split(";")[1:]}
        self.assertEqual("secure" in attributes, origin.startswith("https://"))
        self.assertIn("httponly", attributes)
        self.assertIn("samesite=strict", attributes)
        self.assertFalse(any(part.startswith("domain=") for part in attributes))
        return {**body, "cookie": cookie.split(";", 1)[0]}

    def test_both_origins_can_authenticate_mutate_and_logout(self):
        for origin in (HTTPS_ORIGIN, HTTP_ORIGIN):
            with self.subTest(origin=origin):
                headers = self.origin_headers(origin)
                status, page, _ = self.request("GET", "/", headers=headers)
                self.assertEqual(status, 200)
                self.assertIn("<html", page.lower())
                session = self.origin_login(origin)
                self.api("GET", "/api/session", session=session, headers=headers)
                self.api("POST", "/api/tickets", {"title": "Origin workflow"},
                         session, expected=201, headers=headers)
                _body, logout_headers = self.api("POST", "/api/logout", {}, session,
                                                 headers=headers)
                attributes = {part.strip().lower() for part in logout_headers["set-cookie"].split(";")[1:]}
                self.assertIn("max-age=0", attributes)
                self.assertEqual("secure" in attributes, origin.startswith("https://"))
                self.api("GET", "/api/session", session=session, expected=401, headers=headers)

    def test_origin_must_match_the_requested_host_even_when_both_are_allowed(self):
        for origin, other in ((HTTPS_ORIGIN, HTTP_ORIGIN), (HTTP_ORIGIN, HTTPS_ORIGIN)):
            with self.subTest(origin=origin):
                session = self.origin_login(origin)
                wrong_headers = {**self.origin_headers(origin), "Origin": other}
                self.api("POST", "/api/login", {"email": "admin@example.test",
                         "password": ticket_http.PASSWORD}, expected=403, headers=wrong_headers)
                self.api("POST", "/api/tickets", {"title": "Rejected cross-origin mutation"},
                         session, expected=403, headers=wrong_headers)
                self.api("POST", "/api/tickets", {"title": "CSRF still required"},
                         session, expected=403,
                         headers={**self.origin_headers(origin), "X-CSRF-Token": None})
        status, _body, _headers = self.request("GET", "/", headers={"Host": "unlisted.localhost"})
        self.assertEqual(status, 400)
        # The local health-check exception does not allow application or login access.
        self.api("GET", "/healthz")
        self.assertEqual(self.request("GET", "/")[0], 400)

    def test_session_cannot_be_replayed_on_another_configured_host(self):
        for origin, other in ((HTTPS_ORIGIN, HTTP_ORIGIN), (HTTP_ORIGIN, HTTPS_ORIGIN)):
            with self.subTest(origin=origin):
                session = self.origin_login(origin)
                self.api("GET", "/api/session", session=session, expected=401,
                         headers={"Host": other.split("://", 1)[1]})
                self.api("POST", "/api/tickets", {"title": "Replayed cookie"}, session,
                         expected=401, headers=self.origin_headers(other))
                self.api("GET", "/api/session", session=session,
                         headers=self.origin_headers(origin))

    def test_https_local_primary_and_https_company_share_data_with_separate_sessions(self):
        local = "https://ticket.localhost"
        self.stop()
        self.environment.update({"TICKETS_PUBLIC_ORIGIN": local,
                                 "TICKETS_ADDITIONAL_ORIGINS": HTTPS_ORIGIN})
        self.start()
        local_session = self.origin_login(local)
        company_session = self.origin_login(HTTPS_ORIGIN)
        created, _ = self.api("POST", "/api/tickets", {"title": "Shared ticket store"},
                              local_session, expected=201, headers=self.origin_headers(local))
        detail, _ = self.api("GET", f"/api/tickets/{created['ticket']['id']}",
                             session=company_session, headers=self.origin_headers(HTTPS_ORIGIN))
        self.assertEqual(detail["ticket"]["title"], "Shared ticket store")
        self.api("GET", "/api/session", session=local_session, expected=401,
                 headers=self.origin_headers(HTTPS_ORIGIN))
        self.api("GET", "/api/session", session=company_session, expected=401,
                 headers=self.origin_headers(local))

    def test_forwarded_headers_cannot_change_origin_or_cookie_security(self):
        for origin, claimed in ((HTTPS_ORIGIN, HTTP_ORIGIN), (HTTP_ORIGIN, HTTPS_ORIGIN)):
            with self.subTest(origin=origin):
                scheme, host = claimed.split("://", 1)
                extra = {"X-Forwarded-Proto": scheme, "X-Forwarded-Host": host,
                         "Forwarded": f'proto={scheme};host="{host}"'}
                session = self.origin_login(origin, **extra)
                self.api("GET", "/api/session", session=session,
                         headers=self.origin_headers(origin, **extra))
                self.api("POST", "/api/logout", {}, session, expected=403,
                         headers={**self.origin_headers(origin, **extra), "Origin": claimed})

    def test_invalid_origin_configuration_fails_before_database_creation(self):
        invalid = (
            ("", HTTP_ORIGIN),
            (HTTPS_ORIGIN, ","),
            (HTTPS_ORIGIN, HTTP_ORIGIN + ","),
            (HTTPS_ORIGIN, "," + HTTP_ORIGIN),
            (HTTPS_ORIGIN, HTTP_ORIGIN + ",," + "https://other.example.test"),
            (HTTPS_ORIGIN, " " + HTTP_ORIGIN),
            (HTTPS_ORIGIN, "http://*.localhost"),
            (HTTPS_ORIGIN, "http://user:password@ticket.localhost"),
            (HTTPS_ORIGIN, "http://ticket.localhost/path"),
            (HTTPS_ORIGIN, "http://ticket.localhost?key=value"),
            (HTTPS_ORIGIN, "http://ticket.localhost#fragment"),
            (HTTPS_ORIGIN, "http://ticket.localhost:0"),
            (HTTPS_ORIGIN, "http://ticket.localhost:65536"),
            (HTTPS_ORIGIN, "http://ticket.localhost\r\nHost:evil.test"),
            (HTTPS_ORIGIN, "ftp://ticket.localhost"),
            (HTTPS_ORIGIN, HTTPS_ORIGIN),
            (HTTPS_ORIGIN, "http://ticket.tuaazienda.it"),
            (HTTPS_ORIGIN, "http://TICKET.TUAAZIENDA.IT"),
            (HTTPS_ORIGIN, HTTP_ORIGIN + "," + HTTP_ORIGIN),
            (HTTPS_ORIGIN, ",".join(f"https://origin{i}.example.test" for i in range(4))),
            (HTTPS_ORIGIN + "/", HTTP_ORIGIN),
        )
        for index, (primary, additional) in enumerate(invalid):
            with self.subTest(primary=primary, additional=additional):
                database = Path(self.temporary.name) / f"invalid-origin-{index}.sqlite"
                environment = {**self.environment, "TICKETS_PUBLIC_ORIGIN": primary,
                               "TICKETS_ADDITIONAL_ORIGINS": additional,
                               "TICKETS_DB_PATH": str(database)}
                result = subprocess.run([str(self.binary)], cwd=ticket_http.APP,
                                        env=environment, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(database.exists())

    def test_unconfigured_origin_defaults_remain_loopback_only(self):
        self.stop()
        self.environment.pop("TICKETS_PUBLIC_ORIGIN")
        self.environment.pop("TICKETS_ADDITIONAL_ORIGINS")
        self.start()
        session = self.origin_login(self.origin)
        self.api("GET", "/api/session", session=session)
        self.assertEqual(self.request("GET", "/", headers={"Host": "ticket.localhost"})[0], 400)
        self.api("POST", "/api/tickets", {"title": "Other loopback origin"}, session,
                 expected=403, headers={"Origin": f"http://localhost:{self.port}"})

    def test_oidc_transactions_and_sessions_are_bound_to_redirect_origin(self):
        self.stop()
        provider = OIDCProvider()
        self.addCleanup(provider.close)
        self.environment.update(provider.configure(
            Path(self.temporary.name) / "oidc", HTTPS_ORIGIN + "/auth/callback",
            client_id="punto-tickets", bindings={provider.subject: "admin@example.test"},
        ))
        self.start()
        for origin, enabled in ((HTTPS_ORIGIN, True), (HTTP_ORIGIN, False)):
            config, _ = self.api("GET", "/api/auth/config", headers=self.origin_headers(origin))
            self.assertEqual(config, {"sso": enabled, "local": True})
        status, _body, headers = self.request("GET", "/auth/login",
                                             headers=self.origin_headers(HTTP_ORIGIN))
        self.assertEqual(status, 403)
        # The SSO error page expires the transaction cookie without issuing a session.
        self.assertIn("Max-Age=0", headers.get("set-cookie", ""))
        self.assertNotIn("tickets_session=", headers["set-cookie"])
        status, _body, headers = self.request("GET", "/auth/login",
                                             headers=self.origin_headers(HTTPS_ORIGIN))
        self.assertEqual(status, 303)
        self.assertIn("; Secure", headers["set-cookie"])
        transaction_cookie = headers["set-cookie"].split(";", 1)[0]
        query = provider.register(headers["location"])
        callback = "/auth/callback?" + query
        wrong_headers = self.origin_headers(HTTP_ORIGIN, Cookie=transaction_cookie)
        self.assertEqual(self.request("GET", callback, headers=wrong_headers)[0], 403)
        self.assertEqual(provider.token_requests, [])
        status, _body, headers = self.request("GET", callback, headers=self.origin_headers(
            HTTPS_ORIGIN, Cookie=transaction_cookie))
        self.assertEqual(status, 303)
        self.assertIn("; Secure", headers["set-cookie"])
        session_cookie = headers["set-cookie"].split(";", 1)[0]
        self.api("GET", "/api/session", headers=self.origin_headers(HTTPS_ORIGIN, Cookie=session_cookie))
        self.api("GET", "/api/session", expected=401,
                 headers=self.origin_headers(HTTP_ORIGIN, Cookie=session_cookie))

    def test_unlisted_oidc_redirect_origin_fails_before_database_creation(self):
        provider = OIDCProvider()
        self.addCleanup(provider.close)
        database = Path(self.temporary.name) / "unlisted-oidc.sqlite"
        environment = {**self.environment, **provider.configure(
            Path(self.temporary.name) / "oidc-unlisted", "https://unlisted.example.test/auth/callback",
            client_id="punto-tickets", bindings={provider.subject: "admin@example.test"},
        ), "TICKETS_DB_PATH": str(database)}
        result = subprocess.run([str(self.binary)], cwd=ticket_http.APP,
                                env=environment, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(database.exists())


if __name__ == "__main__":
    unittest.main()
