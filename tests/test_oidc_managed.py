"""Managed OIDC identities: signed app roles, explicit migration and local fallback."""

from pathlib import Path
import time
import unittest
from urllib.parse import parse_qs, urlsplit

import test_oidc as protocol


class OIDCManagedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        protocol.OIDCProtocolTests.setUpClass.__func__(cls)

    def setUp(self):
        protocol.OIDCProtocolTests.setUp(self)
        self.env.update(AUTH_USER_MANAGEMENT="keycloak", AUTH_LOCAL_ADMIN_EMAIL="Admin@Punto.Local",
                        OIDC_ADMIN_ROLE="manager", OIDC_USER_ROLE="worker")

    driver = protocol.OIDCProtocolTests.driver
    start = protocol.OIDCProtocolTests.start

    def identity(self, driver, claims=None, **changes):
        location, cookie = self.start(driver)
        values = {"email": "Person@Example.Test", "name": "Persona Cognome", "app_roles": ["worker"]}
        values.update(claims or {})
        query = self.provider.register(location, claims=values, **changes)
        return driver.command("identity", query=query, cookie=cookie)

    def assert_denied(self, result, status):
        self.assertEqual(result["status"], status, result)
        for field in ("issuer", "subject", "email", "name", "role", "bound_email"):
            self.assertEqual(result[field], "", result)
        self.assertEqual(result["lifetime"], 0, result)

    def test_managed_mode_requires_oidc_and_explicit_distinct_roles(self):
        cases = [{"AUTH_MODE": "local"}, {"AUTH_MODE": ""}, {"AUTH_USER_MANAGEMENT": "automatic"},
                 {"OIDC_ADMIN_ROLE": ""}, {"OIDC_USER_ROLE": ""}, {"OIDC_USER_ROLE": "manager"},
                 {"OIDC_ADMIN_ROLE": "a" * 16}, {"OIDC_USER_ROLE": "bad role"},
                 {"OIDC_ROLE_CLAIM": "bad claim"}, {"AUTH_LOCAL_ADMIN_EMAIL": ""},
                 {"AUTH_LOCAL_ADMIN_EMAIL": "not-an-email"}]
        for changes in cases:
            with self.subTest(changes=changes), self.driver(changes) as driver:
                self.assertFalse(driver.initial["initialized"])
                self.assertFalse(driver.initial["enabled"])

    def test_only_explicit_local_administrator_can_use_hybrid_fallback(self):
        with self.driver() as driver:
            for email, allowed in (("ADMIN@PUNTO.LOCAL", True), ("worker@punto.local", False),
                                   ("admin@punto.local.attacker", False), ("", False), (None, False)):
                value = driver.command("management", email=email)
                self.assertTrue(value["managed"])
                self.assertEqual(value["allowed"], allowed)
                self.assertEqual(value["admin_url"], "")
        with self.driver({"AUTH_MODE": "oidc", "AUTH_LOCAL_ADMIN_EMAIL": ""}) as driver:
            self.assertTrue(driver.initial["initialized"])
            self.assertFalse(driver.command("management", email="admin@punto.local")["allowed"])

    def test_legacy_local_management_retains_local_fallback(self):
        with self.driver({"AUTH_USER_MANAGEMENT": "local"}) as driver:
            value = driver.command("management", email="other@punto.local")
            self.assertFalse(value["managed"])
            self.assertTrue(value["allowed"])
            self.assert_denied(driver.command("identity"), 404)

    def test_administration_link_requires_valid_https_url(self):
        console = "https://auth.localhost/admin/punto/console/"
        with self.driver({"OIDC_USERS_ADMIN_URL": console}) as driver:
            self.assertTrue(driver.initial["initialized"])
            self.assertEqual(driver.command("management")["admin_url"], console)
        for url in ("http://localhost/admin", "https://user:pass@auth.localhost/admin", "https://auth.localhost/#x",
                    "https://auth.localhost/?next=evil", "HTTPS://auth.localhost/admin", "https://auth.localhost:0/admin",
                    "https://auth.localhost/admin\r\nLocation: evil"):
            with self.subTest(url=url), self.driver({"OIDC_USERS_ADMIN_URL": url}) as driver:
                self.assertFalse(driver.initial["initialized"])

    def test_unbound_subject_yields_signed_identity_without_email_autolink(self):
        with self.driver() as driver:
            value = self.identity(driver, {"sub": "new-keycloak-subject", "email": "EXISTING@PUNTO.LOCAL"})
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["issuer"], self.provider.issuer)
            self.assertEqual(value["subject"], "new-keycloak-subject")
            self.assertEqual(value["email"], "existing@punto.local")
            self.assertEqual(value["name"], "Persona Cognome")
            self.assertEqual(value["role"], "worker")
            self.assertEqual(value["bound_email"], "")
            self.assertGreater(value["lifetime"], 0)
            self.assertLessEqual(value["lifetime"], 300)

    def test_existing_explicit_binding_is_returned_separately_from_signed_email(self):
        with self.driver() as driver:
            value = self.identity(driver)
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["bound_email"], "existing@punto.local")
            self.assertEqual(value["email"], "person@example.test")

    def test_mapping_is_optional_but_configured_invalid_mapping_fails_closed(self):
        env = dict(self.env)
        del env["OIDC_BINDINGS_FILE"]
        with self.driver(environment=env) as driver:
            self.assertTrue(driver.initial["initialized"])
            value = self.identity(driver)
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["bound_email"], "")
        with self.driver({"OIDC_BINDINGS_FILE": "/nonexistent/bindings.json"}) as driver:
            self.assertFalse(driver.initial["initialized"])
        Path(self.env["OIDC_BINDINGS_FILE"]).write_text('{"subject":"invalid"}')
        with self.driver() as driver:
            self.assertFalse(driver.initial["initialized"])

    def test_admin_role_has_priority_and_custom_role_claim_is_supported(self):
        with self.driver() as driver:
            for roles in (["worker", "manager"], ["manager", "worker"], ["irrelevant", "manager"]):
                value = self.identity(driver, {"app_roles": roles})
                self.assertEqual(value["status"], 200, value)
                self.assertEqual(value["role"], "manager")
        with self.driver({"OIDC_ROLE_CLAIM": "ticket_roles", "OIDC_ADMIN_ROLE": "admin", "OIDC_USER_ROLE": "agent"}) as driver:
            value = self.identity(driver, {"ticket_roles": ["agent"]})
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["role"], "agent")

    def test_missing_malformed_or_unassigned_role_denies_access(self):
        invalid = [None, "manager", {"role": "manager"}, [], ["unrelated"], ["Manager"],
                   ["manager", None], ["manager", ""], ["manager", "x" * 129], ["worker"] * 129]
        with self.driver() as driver:
            for roles in invalid:
                with self.subTest(roles=roles):
                    self.assert_denied(self.identity(driver, {"app_roles": roles}), 403)

    def test_email_and_name_are_bounded_signed_claims(self):
        invalid = [{"email": None}, {"email": "invalid"}, {"email": "a@b\u0000.example"},
                   {"email": "a" * 65 + "@example.test"}, {"email": "é@example.test"},
                   {"name": None}, {"name": ""}, {"name": " "}, {"name": "x" * 121},
                   {"name": "Persona\u0000 Cognome"}, {"name": "Persona\nCognome"},
                   {"sub": "x" * 256}]
        with self.driver() as driver:
            for claims in invalid:
                with self.subTest(claims=claims):
                    self.assert_denied(self.identity(driver, claims), 401)

    def test_preferred_username_fallback_and_unverified_email_do_not_link_accounts(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location, claims={"preferred_username": "Nuovo Utente",
                "email": "Existing@Punto.Local", "email_verified": False, "app_roles": ["worker"], "sub": "unbound"})
            value = driver.command("identity", query=query, cookie=cookie)
            self.assertEqual(value["status"], 200, value)
            self.assertEqual(value["name"], "Nuovo Utente")
            self.assertEqual(value["bound_email"], "")

    def test_managed_authorization_requests_profile_and_email_scope(self):
        with self.driver() as driver:
            location, _ = self.start(driver)
            self.assertEqual(parse_qs(urlsplit(location).query)["scope"], ["openid profile email"])

    def test_managed_identity_preserves_signature_issuer_audience_and_nonce_validation(self):
        with self.driver() as driver:
            self.assert_denied(self.identity(driver, bad_signature=True), 401)
            for claims in ({"iss": "https://wrong.example/realms/punto"}, {"aud": "other-app"},
                           {"nonce": "x" * 43}, {"exp": int(time.time()) - 1}):
                with self.subTest(claims=claims):
                    self.assert_denied(self.identity(driver, claims), 401)
            self.assert_denied(self.identity(driver, header={"alg": "HS256"}), 401)

    def test_managed_callback_requires_browser_binding_and_consumes_transaction(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location, claims={"name": "Persona Cognome", "app_roles": ["worker"]})
            self.assert_denied(driver.command("identity", query=query, cookie=""), 401)
            self.assertEqual(driver.command("identity", query=query, cookie=cookie)["status"], 200)
            self.assert_denied(driver.command("identity", query=query, cookie=cookie), 401)

    def test_legacy_callback_still_requires_explicit_binding_in_managed_configuration(self):
        with self.driver() as driver:
            location, cookie = self.start(driver)
            query = self.provider.register(location, claims={"sub": "unbound", "email": "existing@punto.local",
                "name": "Persona Cognome", "app_roles": ["manager"]})
            value = driver.command("callback", query=query, cookie=cookie)
            self.assertEqual(value["status"], 403, value)
            self.assertEqual(value["email"], "")


if __name__ == "__main__":
    unittest.main()
