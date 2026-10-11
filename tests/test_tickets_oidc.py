"""Ticket OIDC integration with a local provider and real RSA signatures."""

from contextlib import closing
import sqlite3
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from oidc_fixture import OIDCProvider
import test_tickets as ticket_http

PASSWORD = ticket_http.PASSWORD


class TicketOIDCTests(unittest.TestCase):
    # Reuse the isolated HTTP harness without inheriting the local-login tests.
    setUp = ticket_http.TicketApplicationTests.setUp
    start = ticket_http.TicketApplicationTests.start
    stop = ticket_http.TicketApplicationTests.stop
    request = ticket_http.TicketApplicationTests.request
    api = ticket_http.TicketApplicationTests.api
    login = ticket_http.TicketApplicationTests.login
    create_user = ticket_http.TicketApplicationTests.create_user
    create_ticket = ticket_http.TicketApplicationTests.create_ticket

    @classmethod
    def setUpClass(cls):
        ticket_http.TicketApplicationTests.setUpClass()
        cls.binary = ticket_http.TicketApplicationTests.binary

    def enable_sso(self, email="admin@example.test", mode="hybrid"):
        self.stop()
        self.provider = OIDCProvider()
        self.addCleanup(self.provider.close)
        self.environment = {key: value for key, value in self.environment.items()
                            if not key.startswith("OIDC_")}
        self.environment.update(self.provider.configure(
            Path(self.temporary.name) / "oidc", self.origin + "/auth/callback",
            client_id="punto-tickets", bindings={self.provider.subject: email}, mode=mode,
        ))
        self.start()

    def authorization(self, **registration):
        status, _body, headers = self.request("GET", "/auth/login")
        self.assertEqual(status, 303)
        self.assertIn("httponly", headers["set-cookie"].lower())
        self.assertIn("samesite=lax", headers["set-cookie"].lower())
        self.assertIn("path=/auth/", headers["set-cookie"].lower())
        parameters = parse_qs(urlsplit(headers["location"]).query)
        self.assertEqual(parameters["client_id"], ["punto-tickets"])
        self.assertEqual(parameters["code_challenge_method"], ["S256"])
        query = self.provider.register(headers["location"], **registration)
        return query, headers["set-cookie"].split(";", 1)[0]

    def callback(self, query, cookie):
        return self.request("GET", "/auth/callback?" + query,
                            headers={"Cookie": cookie})

    def test_verified_subject_keeps_local_identity_role_and_csrf(self):
        admin = self.login()
        agent = self.create_user(admin)
        self.enable_sso(email=agent["email"])
        config, _headers = self.api("GET", "/api/auth/config")
        self.assertEqual(config, {"sso": True, "local": True})
        query, transaction_cookie = self.authorization(extra_token={"expires_in": 90})
        status, body, headers = self.callback(query, transaction_cookie)
        self.assertEqual(status, 303, body)
        self.assertEqual(headers["location"], "/")
        self.assertEqual(body, "")
        cookie = headers["set-cookie"]
        self.assertTrue(cookie.startswith("tickets_session="))
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Strict", cookie)
        self.assertIn("Max-Age=90", cookie)
        session, _headers = self.api("GET", "/api/session",
                                     headers={"Cookie": cookie.split(";", 1)[0]})
        self.assertEqual(session["user"]["id"], agent["id"])
        self.assertEqual(session["user"]["email"], agent["email"])
        self.assertEqual(session["user"]["role"], "agent")
        self.assertTrue(session["csrf_token"])
        session["cookie"] = cookie.split(";", 1)[0]
        ticket = self.create_ticket(session, title="Ticket da account aziendale")
        self.assertEqual(ticket["created_by"], agent["id"])
        self.api("POST", "/api/users", {"name": "No", "email": "no@example.test", "password": PASSWORD},
                 session, expected=403)
        self.api("POST", "/api/logout", {}, session)
        self.api("GET", "/api/session", session=session, expected=401)
        # The same provider callback cannot mint another application session.
        self.assertEqual(self.callback(query, transaction_cookie)[0], 401)
        self.assertEqual(len(self.provider.token_requests), 1)

    def test_oidc_only_blocks_password_login_and_allows_sso(self):
        self.enable_sso(mode="oidc")
        config, _headers = self.api("GET", "/api/auth/config")
        self.assertEqual(config, {"sso": True, "local": False})
        self.api("POST", "/api/login", {"email": "admin@example.test", "password": PASSWORD},
                 expected=403)
        query, cookie = self.authorization()
        self.assertEqual(self.callback(query, cookie)[0], 303)

    def test_deactivated_local_account_cannot_use_sso(self):
        admin = self.login()
        agent = self.create_user(admin)
        self.api("PUT", f"/api/users/{agent['id']}", {"active": False}, admin)
        self.enable_sso(email=agent["email"])
        query, cookie = self.authorization()
        status, body, headers = self.callback(query, cookie)
        self.assertEqual(status, 403, body)
        self.assertIn("Max-Age=0", headers["set-cookie"])
        self.api("GET", "/api/session", expected=401)

    def test_unknown_subject_cannot_claim_existing_local_email(self):
        self.enable_sso()
        query, cookie = self.authorization(claims={"sub": "unbound-subject",
                                                  "email": "admin@example.test"})
        status, body, headers = self.callback(query, cookie)
        self.assertEqual(status, 403, body)
        self.assertIn("Max-Age=0", headers["set-cookie"])
        self.api("GET", "/api/session", expected=401)

    def test_binding_does_not_create_a_missing_local_account(self):
        self.enable_sso(email="missing@example.test")
        query, cookie = self.authorization()
        self.assertEqual(self.callback(query, cookie)[0], 403)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 1)

    def test_invalid_signature_does_not_issue_application_cookie(self):
        self.enable_sso()
        query, cookie = self.authorization(bad_signature=True)
        status, body, headers = self.callback(query, cookie)
        self.assertEqual(status, 401, body)
        self.assertIn("Max-Age=0", headers["set-cookie"])
        self.assertNotIn("tickets_session=", headers["set-cookie"])
        self.api("GET", "/api/session", expected=401)


if __name__ == "__main__":
    unittest.main()
