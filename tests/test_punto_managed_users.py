"""Keycloak user projection over real HTTP and signed provider tokens."""

import json
from contextlib import closing, contextmanager
from pathlib import Path
import sqlite3
import unittest

import test_punto_oidc as legacy
from oidc_fixture import OIDCProvider


class PuntoManagedUserTests(unittest.TestCase):
    setUp = legacy.PuntoOIDCTests.setUp
    stop = legacy.PuntoOIDCTests.stop
    request = legacy.PuntoOIDCTests.request
    sso = legacy.PuntoOIDCTests.sso
    session_cookie = legacy.PuntoOIDCTests.session_cookie

    @classmethod
    def setUpClass(cls):
        cls.provider = OIDCProvider()
        cls.addClassCleanup(cls.provider.close)

    def start(self):
        self.environment.update(AUTH_USER_MANAGEMENT="keycloak",
                                AUTH_LOCAL_ADMIN_EMAIL=legacy.EMAIL,
                                OIDC_ROLE_CLAIM="app_roles", OIDC_ADMIN_ROLE="manager",
                                OIDC_USER_ROLE="worker",
                                OIDC_USERS_ADMIN_URL="https://auth.example.test/admin/punto/console/")
        legacy.PuntoOIDCTests.start(self)

    def managed_sso(self, subject=None, email=legacy.EMAIL, name="Utente aziendale",
                    role="manager", **claims):
        payload = {"email": email, "name": name, "app_roles": [role]}
        if subject is not None:
            payload["sub"] = subject
        payload.update(claims)
        return self.sso(claims=payload)

    def bindings(self, mapping):
        self.stop()
        Path(self.environment["OIDC_BINDINGS_FILE"]).write_text(json.dumps(mapping))
        self.start()

    def local_cookie(self, email=legacy.EMAIL):
        status, headers, _ = self.request("POST", "/api/login",
                                         fields={"email": email, "password": legacy.PASSWORD})
        self.assertEqual(status, 200)
        return dict(headers)["Set-Cookie"].split(";", 1)[0]

    @contextmanager
    def db(self):
        with closing(sqlite3.connect(self.database)) as db, db:
            yield db

    def test_new_subject_provisions_once_and_persists_across_restart(self):
        status, headers, _ = self.managed_sso(subject="new-subject", email="new@example.test", role="worker")
        self.assertEqual(status, 303)
        cookie = self.session_cookie(headers)
        account = json.loads(self.request("GET", "/api/session", cookie)[2])
        self.assertEqual((account["email"], account["name"], account["role"]),
                         ("new@example.test", "Utente aziendale", "worker"))
        with self.db() as db:
            user_id = db.execute("SELECT id FROM users WHERE email='new@example.test'").fetchone()[0]
            self.assertEqual(db.execute("SELECT issuer,subject,user_id FROM oidc_identities").fetchone(),
                             (self.provider.issuer, "new-subject", user_id))
            self.assertEqual(db.execute("SELECT count(*) FROM users").fetchone()[0], 2)
        self.stop()
        self.start()
        self.assertEqual(self.managed_sso(subject="new-subject", email="new@example.test", role="worker")[0], 303)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT id FROM users WHERE email='new@example.test'").fetchone()[0], user_id)
            self.assertEqual(db.execute("SELECT count(*) FROM users").fetchone()[0], 2)
            self.assertEqual(db.execute("SELECT count(*) FROM audit_events WHERE action='users.oidc_link'").fetchone()[0], 1)

    def test_explicit_first_binding_preserves_id_history_and_password(self):
        with self.db() as db:
            original = db.execute("SELECT id,password_hash,salt FROM users").fetchone()
            db.execute("INSERT INTO jobs(code,name,status) VALUES('OIDC','Storico','active')")
            db.execute("INSERT INTO time_entries(job_id,owner,user_id,date,minutes,note) VALUES(1,?,?,?,30,'Prima di SSO')",
                       (legacy.EMAIL, original[0], "2026-10-11"))
        self.assertEqual(self.managed_sso(email="renamed@example.test", name="Nome aggiornato")[0], 303)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT id,password_hash,salt FROM users").fetchone(), original)
            self.assertEqual(db.execute("SELECT user_id,owner,note FROM time_entries").fetchone(),
                             (original[0], legacy.EMAIL, "Prima di SSO"))
            self.assertEqual(db.execute("SELECT email,name FROM users").fetchone(),
                             ("renamed@example.test", "Nome aggiornato"))
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_email_collision_never_links_or_mutates_existing_user(self):
        self.assertEqual(self.managed_sso(subject="unknown-subject", email=legacy.EMAIL)[0], 403)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM oidc_identities").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT name,role FROM users").fetchone(), ("Gestore", "manager"))
            self.assertEqual(db.execute("SELECT count(*) FROM audit_events WHERE action LIKE 'users.oidc_%'").fetchone()[0], 0)

    def test_second_subject_cannot_claim_account_already_bound(self):
        self.assertEqual(self.managed_sso()[0], 303)
        self.bindings({self.provider.subject: legacy.EMAIL, "another-subject": legacy.EMAIL})
        self.assertEqual(self.managed_sso(subject="another-subject", email="other@example.test")[0], 403)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT subject FROM oidc_identities").fetchall(), [(self.provider.subject,)])
            self.assertEqual(db.execute("SELECT count(*) FROM users").fetchone()[0], 1)

    def test_known_subject_wins_over_changed_binding_and_syncs_roles(self):
        status, headers, _ = self.managed_sso()
        self.assertEqual(status, 303)
        old_cookie = self.session_cookie(headers)
        with self.db() as db:
            original_id = db.execute("SELECT id FROM users").fetchone()[0]
        # A later login updates the projection and existing sessions' privileges.
        self.assertEqual(self.managed_sso(name="Operatore", role="worker")[0], 303)
        old_session = json.loads(self.request("GET", "/api/session", old_cookie)[2])
        self.assertEqual(old_session["role"], "worker")
        self.assertFalse(old_session["permissions"]["manage_jobs"])
        self.bindings({self.provider.subject: "missing@example.test"})
        self.assertEqual(self.managed_sso(email="changed@example.test", name="Gestore nuovo", role="manager")[0], 303)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT id,email,name,role FROM users").fetchone(),
                             (original_id, "changed@example.test", "Gestore nuovo", "manager"))
            self.assertEqual(db.execute("SELECT count(*) FROM audit_events WHERE action='users.oidc_sync'").fetchone()[0], 2)

    def test_known_subject_email_conflict_rolls_back_entire_profile(self):
        self.assertEqual(self.managed_sso()[0], 303)
        self.assertEqual(self.managed_sso(subject="second", email="second@example.test", role="worker")[0], 303)
        self.assertEqual(self.managed_sso(email="second@example.test", name="Collisione", role="worker")[0], 403)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT name,role FROM users WHERE email=?", (legacy.EMAIL,)).fetchone(),
                             ("Utente aziendale", "manager"))
            self.assertEqual(db.execute("SELECT count(*) FROM oidc_identities").fetchone()[0], 2)

    def test_provider_authentication_reactivates_known_projection(self):
        self.assertEqual(self.managed_sso()[0], 303)
        with self.db() as db:
            db.execute("UPDATE users SET active=0")
        self.assertEqual(self.managed_sso()[0], 303)
        with self.db() as db:
            self.assertEqual(db.execute("SELECT active FROM users").fetchone()[0], 1)

    def test_user_management_is_external_but_user_directory_remains(self):
        cookie = self.local_cookie()
        config = json.loads(self.request("GET", "/api/auth/config")[2])
        self.assertEqual(config, {"sso": True, "local": True, "managed_users": True,
                                  "users_admin_url": self.environment["OIDC_USERS_ADMIN_URL"]})
        session = json.loads(self.request("GET", "/api/session", cookie)[2])
        self.assertTrue(session["permissions"]["manage_jobs"])
        self.assertFalse(session["permissions"]["manage_users"])
        self.assertEqual(self.request("GET", "/api/users", cookie)[0], 200)
        for target, fields in (("/api/users", {"name": "Locale", "email": "local@example.test",
                                              "password": legacy.PASSWORD, "role": "worker"}),
                               ("/api/users/1", {"name": "Modifica", "role": "worker", "active": "0"})):
            with self.subTest(target=target):
                status, _, body = self.request("POST", target, cookie, fields)
                self.assertEqual(status, 403)
                self.assertIn("Keycloak", json.loads(body)["error"])
        with self.db() as db:
            self.assertEqual(db.execute("SELECT name,role,active FROM users").fetchall(), [("Gestore", "manager", 1)])

    def test_only_configured_emergency_account_accepts_local_password(self):
        with self.db() as db:
            db.execute("INSERT INTO users(email,name,role,active,password_hash,salt) "
                       "SELECT 'other@example.test','Altro','manager',1,password_hash,salt FROM users LIMIT 1")
        self.local_cookie()
        self.assertEqual(self.request("POST", "/api/login", fields={"email": "other@example.test",
                                                                    "password": legacy.PASSWORD})[0], 403)


if __name__ == "__main__":
    unittest.main()
