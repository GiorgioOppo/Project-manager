"""Keycloak owns Ticket accounts; local IDs and history remain stable."""

from contextlib import closing
import sqlite3
import unittest
from pathlib import Path

from oidc_fixture import OIDCProvider
import test_tickets as ticket_http
import test_tickets_oidc as oidc_http


class TicketManagedUsersTests(unittest.TestCase):
    setUp = ticket_http.TicketApplicationTests.setUp
    start = ticket_http.TicketApplicationTests.start
    stop = ticket_http.TicketApplicationTests.stop
    request = ticket_http.TicketApplicationTests.request
    api = ticket_http.TicketApplicationTests.api
    login = ticket_http.TicketApplicationTests.login
    create_user = ticket_http.TicketApplicationTests.create_user
    create_ticket = ticket_http.TicketApplicationTests.create_ticket
    authorization = oidc_http.TicketOIDCTests.authorization
    callback = oidc_http.TicketOIDCTests.callback

    @classmethod
    def setUpClass(cls):
        ticket_http.TicketApplicationTests.setUpClass()
        cls.binary = ticket_http.TicketApplicationTests.binary

    def enable_managed(self, bindings=None):
        self.stop()
        self.provider = OIDCProvider()
        self.addCleanup(self.provider.close)
        self.environment = {key: value for key, value in self.environment.items()
                            if not key.startswith("OIDC_")}
        self.environment.update(self.provider.configure(
            Path(self.temporary.name) / "managed", self.origin + "/auth/callback",
            client_id="punto-tickets", bindings=bindings or {}, mode="hybrid",
        ))
        self.environment.update({"AUTH_USER_MANAGEMENT": "keycloak",
                                 "AUTH_LOCAL_ADMIN_EMAIL": "admin@example.test",
                                 "OIDC_ROLE_CLAIM": "app_roles",
                                 "OIDC_ADMIN_ROLE": "admin", "OIDC_USER_ROLE": "agent",
                                 "OIDC_USERS_ADMIN_URL": "https://auth.example.test/admin/punto/console/"})
        self.start()

    def sso(self, email="new@example.test", name="Utente aziendale", role="agent", **claims):
        identity = {"email": email, "name": name, "app_roles": [role], **claims}
        query, cookie = self.authorization(claims=identity)
        status, body, headers = self.callback(query, cookie)
        if status != 303:
            return status, body, headers
        session, _ = self.api("GET", "/api/session",
                               headers={"Cookie": headers["set-cookie"].split(";", 1)[0]})
        session["cookie"] = headers["set-cookie"].split(";", 1)[0]
        return status, session, headers

    def test_local_account_crud_is_denied_and_only_emergency_login_remains(self):
        admin = self.login()
        agent = self.create_user(admin)
        self.enable_managed()
        config, _ = self.api("GET", "/api/auth/config")
        self.assertTrue(config["sso"])
        self.assertTrue(config["local"])
        self.assertTrue(config["managed_users"])
        self.assertEqual(config["users_admin_url"], self.environment["OIDC_USERS_ADMIN_URL"])
        admin = self.login()
        users, _ = self.api("GET", "/api/users", session=admin)
        self.assertEqual(len(users["items"]), 2)
        self.api("POST", "/api/users", {"name": "Denied", "email": "denied@example.test",
                 "password": ticket_http.PASSWORD, "role": "agent"}, admin, expected=403)
        for patch in ({"name": "Denied"}, {"role": "admin"}, {"active": False},
                      {"password": ticket_http.PASSWORD}):
            with self.subTest(patch=patch):
                self.api("PUT", f"/api/users/{agent['id']}", patch, admin, expected=403)
        self.api("POST", "/api/login", {"email": agent["email"], "password": ticket_http.PASSWORD}, expected=403)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 2)
            self.assertEqual(database.execute("SELECT name,role,active FROM users WHERE id=?", (agent["id"],)).fetchone(),
                             (agent["name"], "agent", 1))

    def test_switching_to_managed_invalidates_existing_local_sessions(self):
        admin = self.login()
        agent = self.create_user(admin)
        local_agent = self.login(agent["email"])
        ticket = self.create_ticket(local_agent, title="Prima della gestione centrale")
        self.api("GET", "/api/session", session=local_agent)
        # Sessions are connection-local TEMP data; accounts and work are durable.
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertIsNone(database.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='sessions'").fetchone())
        self.enable_managed({OIDCProvider.subject: agent["email"]})
        self.api("GET", "/api/session", session=local_agent, expected=401)
        self.api("GET", "/api/session", session=admin, expected=401)
        self.api("POST", "/api/login", {"email": agent["email"], "password": ticket_http.PASSWORD}, expected=403)
        emergency = self.login()
        self.assertEqual(emergency["user"]["id"], admin["user"]["id"])
        status, central, _ = self.sso(email=agent["email"])
        self.assertEqual(status, 303, central)
        self.assertEqual(central["user"]["id"], agent["id"])
        detail, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=central)
        self.assertEqual(detail["ticket"]["created_by"], agent["id"])

    def test_unbound_subject_is_provisioned_with_central_role_and_no_local_password(self):
        self.enable_managed()
        status, session, _ = self.sso()
        self.assertEqual(status, 303, session)
        self.assertEqual(session["user"]["email"], "new@example.test")
        self.assertEqual(session["user"]["role"], "agent")
        self.assertEqual(session["user"]["name"], "Utente aziendale")
        self.api("POST", "/api/login", {"email": "new@example.test", "password": ticket_http.PASSWORD}, expected=403)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT user_id FROM oidc_identities WHERE issuer=? AND subject=?",
                             (self.provider.issuer, self.provider.subject)).fetchone()[0], session["user"]["id"])

    def test_explicit_migration_preserves_ids_history_and_persistent_subject_link(self):
        admin = self.login()
        agent = self.create_user(admin)
        agent_session = self.login(agent["email"])
        ticket = self.create_ticket(agent_session, title="Storico da conservare")
        self.enable_managed({OIDCProvider.subject: agent["email"]})
        status, first, _ = self.sso(email="renamed@example.test", name="Nome centrale", role="admin")
        self.assertEqual(status, 303, first)
        self.assertEqual(first["user"]["id"], agent["id"])
        self.assertEqual(first["user"]["role"], "admin")
        self.stop(); self.start()
        status, second, _ = self.sso(email="another@example.test", name="Nuovo nome", role="agent")
        self.assertEqual(status, 303, second)
        self.assertEqual(second["user"]["id"], agent["id"])
        self.assertEqual(second["user"]["role"], "agent")
        detail, _ = self.api("GET", f"/api/tickets/{ticket['id']}", session=second)
        self.assertEqual(detail["ticket"]["created_by"], agent["id"])
        self.assertEqual(detail["history"][0]["actor_name"], agent["name"])
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT actor_id FROM history WHERE ticket_id=?", (ticket["id"],)).fetchone()[0], agent["id"])
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 2)
            self.assertEqual(database.execute("SELECT count(*) FROM oidc_identities").fetchone()[0], 1)

    def test_signed_email_cannot_take_over_existing_account_without_binding(self):
        self.enable_managed()
        status, body, _ = self.sso(email="admin@example.test", role="admin")
        self.assertEqual(status, 403, body)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM oidc_identities").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 1)

    def test_existing_subject_email_collision_rolls_back_profile_and_role(self):
        admin = self.login()
        self.create_user(admin, email="occupied@example.test")
        self.enable_managed()
        status, first, _ = self.sso(email="new@example.test", role="agent")
        self.assertEqual(status, 303, first)
        status, body, _ = self.sso(email="occupied@example.test", role="admin")
        self.assertEqual(status, 403, body)
        current, _ = self.api("GET", "/api/session", session=first)
        self.assertEqual(current["user"]["email"], "new@example.test")
        self.assertEqual(current["user"]["role"], "agent")

    def test_explicit_binding_cannot_relink_account_to_another_subject(self):
        self.enable_managed({OIDCProvider.subject: "admin@example.test",
                             "other-subject": "admin@example.test"})
        status, first, _ = self.sso(email="admin@example.test", role="admin")
        self.assertEqual(status, 303, first)
        status, body, _ = self.sso(sub="other-subject", email="another@example.test", role="admin")
        self.assertEqual(status, 403, body)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 1)
            self.assertEqual(database.execute("SELECT subject FROM oidc_identities").fetchone()[0], self.provider.subject)

    def test_unassigned_central_role_cannot_create_local_mirror(self):
        self.enable_managed()
        for roles in ([], ["unrelated"], None):
            with self.subTest(roles=roles):
                status, body, _ = self.sso(app_roles=roles)
                self.assertEqual(status, 403, body)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM users").fetchone()[0], 1)

    def test_role_change_revokes_previous_session_and_reactivates_local_mirror(self):
        self.enable_managed()
        status, first, _ = self.sso(role="admin")
        self.assertEqual(status, 303, first)
        with closing(sqlite3.connect(self.environment["TICKETS_DB_PATH"])) as database:
            database.execute("UPDATE users SET active=0 WHERE id=?", (first["user"]["id"],))
            database.commit()
        status, second, _ = self.sso(role="agent")
        self.assertEqual(status, 303, second)
        self.assertEqual(second["user"]["id"], first["user"]["id"])
        self.assertTrue(second["user"]["active"])
        self.assertEqual(second["user"]["role"], "agent")
        self.api("GET", "/api/session", session=first, expected=401)


if __name__ == "__main__":
    unittest.main()
