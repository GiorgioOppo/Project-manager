"""Explicit identity provisioning: no email auto-link or credential disclosure."""

import importlib.util
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("manage_keycloak", ROOT / "scripts" / "manage_keycloak.py")
MANAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MANAGE)


class FakeAdmin:
    def __init__(self, existing=None):
        self.existing = existing
        self.created = []
        self.entered = 0

    def __enter__(self):
        self.entered += 1
        return self

    def __exit__(self, *_):
        pass

    def find_user(self, username):
        return self.existing

    def create_user(self, payload):
        self.created.append(payload)
        self.existing = dict(payload, id="subject-001")
        return "subject-001"


class KeycloakAdminTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-keycloak-admin-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name)
        (self.data / "config.json").write_text("{}")
        (self.data / "config.json").chmod(0o600)
        (self.data / "bindings").mkdir(mode=0o700)
        for app in ("punto", "tickets"):
            (self.data / "bindings" / f"{app}.json").write_text("{}\n")
            (self.data / "bindings" / f"{app}.json").chmod(0o600)
        self.password = self.data / "user-password"
        self.password.write_text("New-SSO-Password!2026\n")
        self.password.chmod(0o600)
        self.admin = FakeAdmin()

    def create(self, **kwargs):
        return MANAGE.create_user(kwargs.get("admin", self.admin), self.data, "shared-user", "id@example.test",
                                  "Mario Rossi", self.password,
                                  kwargs.get("bindings", [("punto", "punto@example.test"), ("tickets", "ticket@example.test")]),
                                  subject=kwargs.get("subject"), temporary_password=kwargs.get("temporary_password", False))

    def test_new_user_gets_returned_subject_and_only_explicit_local_bindings(self):
        result = self.create(temporary_password=True)
        self.assertTrue(result["created"])
        self.assertEqual(result["subject"], "subject-001")
        self.assertEqual(self.admin.created[0]["credentials"], [{"type": "password", "value": "New-SSO-Password!2026", "temporary": True}])
        self.assertEqual(self.admin.created[0]["firstName"], "Mario")
        self.assertEqual(self.admin.created[0]["lastName"], "Rossi")
        self.assertEqual(json.loads((self.data / "bindings" / "punto.json").read_text()), {"subject-001": "punto@example.test"})
        self.assertEqual(json.loads((self.data / "bindings" / "tickets.json").read_text()), {"subject-001": "ticket@example.test"})
        manifest = self.data / "provisioned-users.json"
        self.assertEqual(stat.S_IMODE(manifest.stat().st_mode), 0o600)
        self.assertNotIn("New-SSO-Password", manifest.read_text())
        self.assertNotIn("New-SSO-Password", json.dumps(result))
        for path in Path(result["binding_backup"]).iterdir():
            self.assertEqual(json.loads(path.read_text()), {})
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_manifest_retry_does_not_change_password_or_create_another_identity(self):
        self.create()
        self.password.write_text("Another-SSO-Password!2026\n")
        result = self.create()
        self.assertFalse(result["created"])
        self.assertEqual(len(self.admin.created), 1)
        self.assertEqual(result["bindings"], [])
        self.assertEqual(self.admin.created[0]["credentials"][0]["value"], "New-SSO-Password!2026")

    def test_matching_email_does_not_link_a_preexisting_user_implicitly(self):
        self.admin.existing = {"id": "subject-001", "username": "shared-user", "email": "id@example.test", "enabled": True}
        with self.assertRaises(MANAGE.AdministrationError):
            self.create()
        self.assertEqual(self.admin.created, [])
        self.assertEqual((self.data / "bindings" / "punto.json").read_text(), "{}\n")
        self.assertFalse((self.data / "provisioned-users.json").exists())

    def test_preexisting_subject_must_match_and_is_never_overwritten(self):
        self.admin.existing = {"id": "subject-001", "username": "shared-user", "email": "id@example.test", "enabled": True}
        with self.assertRaises(MANAGE.AdministrationError):
            self.create(subject="different-subject")
        result = self.create(subject="subject-001")
        self.assertFalse(result["created"])
        self.assertEqual(self.admin.created, [])
        self.assertNotIn("credentials", self.admin.existing)

    def test_known_subject_missing_remotely_is_not_recreated(self):
        with self.assertRaises(MANAGE.AdministrationError):
            self.create(subject="subject-001")
        self.assertEqual(self.admin.created, [])

    def test_password_file_must_be_private_and_not_a_symlink(self):
        self.password.chmod(0o644)
        with self.assertRaises(MANAGE.AdministrationError):
            self.create()
        self.assertEqual(self.admin.entered, 0)
        self.password.chmod(0o600)
        other = self.data / "other-password"
        self.password.replace(other)
        self.password.symlink_to(other)
        with self.assertRaises(MANAGE.configuration.ConfigurationError):
            self.create()
        self.assertEqual(self.admin.entered, 0)

    def test_invalid_bindings_are_rejected_before_remote_admin_login(self):
        for bindings in ([], [("punto", "invalid-email")], [("other", "a@example.test")],
                         [("punto", "a@example.test"), ("punto", "b@example.test")]):
            with self.subTest(bindings=bindings), self.assertRaises((MANAGE.AdministrationError, MANAGE.configuration.ConfigurationError)):
                self.create(bindings=bindings)
        self.assertEqual(self.admin.entered, 0)

    def test_missing_last_name_is_rejected_before_remote_admin_login(self):
        for name in ("Gestore", "Gestore ", " Gestore", "   "):
            with self.subTest(name=name), self.assertRaises(MANAGE.AdministrationError):
                MANAGE.create_user(self.admin, self.data, "shared-user", "id@example.test", name, self.password,
                                   [("punto", "punto@example.test")])
        self.assertEqual(self.admin.entered, 0)

    def test_kcadm_payload_uses_stdin_and_credentials_never_enter_arguments(self):
        ca = self.data / "ca.crt"
        ca.write_text("public-test-ca")
        admin = MANAGE.KeycloakAdmin("projectmanament-keycloak-1", ca)
        admin.work = "/tmp/punto-keycloak-admin.abcd1234"
        captured = []
        def run(arguments, *, data=None, timeout=45):
            captured.append((arguments, data))
            return b"subject-001\n"
        admin.run = run
        self.assertEqual(admin.create_user({"credentials": [{"value": "Protected-Password!2026"}]}), "subject-001")
        arguments, data = captured[0]
        self.assertNotIn("Protected-Password!2026", " ".join(arguments))
        self.assertIn(b"Protected-Password!2026", data)
        self.assertIn("-i", arguments)
        self.assertIn("-", arguments)


if __name__ == "__main__":
    unittest.main()
