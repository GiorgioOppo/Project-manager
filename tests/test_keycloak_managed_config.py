"""Offline migration ownership, retry safety and client-role configuration."""

import copy
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
SPEC = importlib.util.spec_from_file_location("configure_keycloak_managed", ROOT / "scripts/configure_keycloak_managed.py")
MANAGED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MANAGED)
ADMIN_EMAIL = "admin@punto.local"
ADMIN_SUBJECT = "verified-admin-subject"


class FakeAdmin:
    def __init__(self):
        self.users = {ADMIN_EMAIL: {"id": ADMIN_SUBJECT, "username": ADMIN_EMAIL,
                                   "email": ADMIN_EMAIL, "enabled": True}}
        self.created = []
        self.commands = []
        self.assignments = {}
        self.clients = {client_id: "client-" + app for app, client_id in MANAGED.config.APPS.items()}
        self.clients["realm-management"] = "client-management"
        self.roles = {"client-" + app: [{"id": app + "-" + role, "name": role} for role in roles]
                      for app, roles in MANAGED.ROLES.items()}
        self.roles["client-management"] = [{"id": "realm-admin-id", "name": "realm-admin"}]
        self.scopes = {cid: [] for cid in self.clients.values()}
        self.mappers = {cid: [] for cid in self.clients.values()}

    def find_user(self, username):
        return copy.deepcopy(self.users.get(username))

    def create_user(self, payload):
        subject = "created-subject-" + str(len(self.created) + 1)
        self.created.append(copy.deepcopy(payload))
        self.users[payload["username"]] = dict(copy.deepcopy(payload), id=subject)
        return subject

    @property
    def mutations(self):
        return [command for command in self.commands if command[0][0] != "get"]

    def command(self, arguments, *, payload=None):
        self.commands.append((list(arguments), copy.deepcopy(payload)))
        method, path = arguments[:2]
        parts = path.split("/")
        result = None
        if path == "clients":
            client_id = arguments[arguments.index("-q") + 1].removeprefix("clientId=")
            result = ([{"id": self.clients[client_id], "clientId": client_id}]
                      if client_id in self.clients else [])
        elif parts[0] == "clients" and parts[2] == "roles":
            cid = parts[1]
            if method == "get":
                result = self.roles[cid] if len(parts) == 3 else next(r for r in self.roles[cid] if r["name"] == parts[3])
            elif method == "create":
                self.roles[cid].append(dict(payload, id=cid + "-" + payload["name"]))
        elif parts[0] == "clients" and parts[2] == "scope-mappings":
            if method == "get":
                result = self.scopes[parts[1]]
            elif method == "create":
                self.scopes[parts[1]].extend(payload)
        elif parts[0] == "clients" and parts[2] == "protocol-mappers":
            if method == "get":
                result = self.mappers[parts[1]]
            elif method == "create":
                self.mappers[parts[1]].append(dict(payload, id="mapper-" + parts[1]))
        elif parts[0] == "users" and parts[2] == "role-mappings":
            current = self.assignments.setdefault((parts[1], parts[4]), [])
            if method == "get":
                result = current
            elif method == "create":
                current.extend(payload)
            elif method == "delete":
                removed = {role["id"] for role in payload}
                current[:] = [role for role in current if role["id"] not in removed]
        else:
            raise AssertionError("Unexpected administrative operation: " + path)
        return json.dumps(result).encode() if result is not None else b""


class ManagedMigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="punto-managed-config-")
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        (self.data / "bindings").mkdir(mode=0o700)
        for app in MANAGED.ROLES:
            MANAGED.config.write_new(self.data / "bindings" / (app + ".json"),
                                     MANAGED.config.json_text({ADMIN_SUBJECT: ADMIN_EMAIL}))
        self.admin = FakeAdmin()
        self.legacy = {"punto": [{"id": 1, "email": ADMIN_EMAIL, "name": "Admin Punto", "role": "manager", "active": 1},
                                  {"id": 9, "email": "worker@example.test", "name": "Mario Rossi", "role": "worker", "active": 1}],
                       "tickets": [{"id": 2, "email": ADMIN_EMAIL, "name": "Admin Punto", "role": "admin", "active": True},
                                   {"id": 17, "email": "worker@example.test", "name": "Mario Rossi", "role": "agent", "active": 1}]}
        self.ids = {app: "client-" + app for app in MANAGED.ROLES}
        self.roles = {app: {role["name"]: role for role in self.admin.roles[cid]} for app, cid in self.ids.items()}

    def migrate(self, *, legacy=None):
        with patch.object(MANAGED, "ensure_clients", return_value=(self.ids, self.roles)) as ensure:
            self.ensure = ensure
            return MANAGED.migrate(self.admin, self.data, self.legacy if legacy is None else legacy, ADMIN_SUBJECT)

    def test_new_identity_uses_explicit_bindings_and_preserves_local_ids(self):
        result = self.migrate()
        self.assertEqual((result["identities"], result["created"]), (2, 1))
        created = self.admin.created[0]
        self.assertTrue(created["enabled"])
        self.assertTrue(created["credentials"][0]["temporary"])
        password = created["credentials"][0]["value"]
        self.assertGreaterEqual(len(password), 32)
        manifest = MANAGED.config.read_json(self.data / "managed-users-migration.json")
        worker = manifest["worker@example.test"]
        self.assertEqual(worker["apps"]["punto"]["id"], 9)
        self.assertEqual(worker["apps"]["tickets"]["id"], 17)
        for app in MANAGED.ROLES:
            self.assertEqual(MANAGED.config.load_bindings(self.data / "bindings" / (app + ".json"))[worker["subject"]], "worker@example.test")
        private = self.data / "credentials/migrated-users.json"
        self.assertEqual(stat.S_IMODE(private.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(private.parent.stat().st_mode), 0o700)
        self.assertNotIn(password, json.dumps(result))
        self.assertNotIn(password, (self.data / "managed-users-migration.json").read_text())
        self.assertEqual(self.admin.assignments[(ADMIN_SUBJECT, "client-management")], [{"id": "realm-admin-id", "name": "realm-admin"}])

    def test_retry_does_not_create_identity_or_reset_password(self):
        self.migrate()
        credentials_before = (self.data / "credentials/migrated-users.json").read_bytes()
        self.admin.commands.clear()
        result = self.migrate()
        self.assertEqual(result["created"], 0)
        self.assertEqual(len(self.admin.created), 1)
        self.assertEqual((self.data / "credentials/migrated-users.json").read_bytes(), credentials_before)
        self.assertEqual(self.admin.mutations, [])

    def test_same_email_remote_user_without_explicit_ownership_is_rejected(self):
        self.admin.users["worker@example.test"] = {"id": "foreign-subject", "email": "worker@example.test",
                                                    "username": "worker@example.test", "enabled": True}
        with self.assertRaises(MANAGED.AdministrationError):
            self.migrate()
        self.ensure.assert_not_called()
        self.assertEqual(self.admin.created, [])
        self.assertFalse((self.data / "managed-users-migration.json").exists())

    def test_known_identity_missing_remotely_is_not_recreated(self):
        self.migrate()
        del self.admin.users["worker@example.test"]
        with self.assertRaises(MANAGED.AdministrationError):
            self.migrate()
        self.ensure.assert_not_called()
        self.assertEqual(len(self.admin.created), 1)

    def test_verified_administrator_must_exist_before_any_mutation(self):
        del self.admin.users[ADMIN_EMAIL]
        with self.assertRaises(MANAGED.AdministrationError):
            self.migrate()
        self.ensure.assert_not_called()
        self.assertEqual(self.admin.created, [])
        self.assertEqual(self.admin.mutations, [])

    def test_manifest_subject_cannot_be_replaced_using_another_binding(self):
        MANAGED.config.write_new(self.data / "managed-users-migration.json", MANAGED.config.json_text(
            {"worker@example.test": {"subject": "previous-subject", "apps": {}}}))
        self.admin.users["worker@example.test"] = {"id": "replacement-subject", "email": "worker@example.test",
                                                    "username": "worker@example.test", "enabled": True}
        MANAGED.config.bind_account(self.data, "punto", "replacement-subject", "worker@example.test")
        with self.assertRaises(MANAGED.AdministrationError):
            self.migrate()
        self.ensure.assert_not_called()
        self.assertEqual(MANAGED.config.read_json(self.data / "managed-users-migration.json")["worker@example.test"]["subject"], "previous-subject")

    def test_snapshot_rejects_invalid_ids_roles_and_active_values(self):
        for changed in ({"id": 0}, {"id": -1}, {"id": True}, {"active": "false"}, {"active": 2}, {"role": "admin"}):
            value = copy.deepcopy(self.legacy)
            value["punto"][1].update(changed)
            with self.subTest(changed=changed), self.assertRaises(MANAGED.AdministrationError):
                MANAGED.validate_legacy(value)
        value = copy.deepcopy(self.legacy)
        value["punto"][1]["id"] = 1
        with self.assertRaises(MANAGED.AdministrationError):
            MANAGED.validate_legacy(value)

    def test_inactive_projection_has_no_app_role_and_only_managed_roles_are_removed(self):
        self.migrate()
        subject = MANAGED.config.read_json(self.data / "managed-users-migration.json")["worker@example.test"]["subject"]
        unrelated = {"id": "custom-role", "name": "custom"}
        self.admin.assignments[(subject, "client-punto")].append(unrelated)
        self.legacy["punto"][1]["active"] = False
        self.migrate()
        self.assertEqual(self.admin.assignments[(subject, "client-punto")], [unrelated])
        self.assertEqual(self.admin.assignments[(subject, "client-tickets")], [self.roles["tickets"]["agent"]])


class ManagedClientTests(unittest.TestCase):
    def setUp(self):
        self.admin = FakeAdmin()

    def test_role_scope_and_mapper_creation_is_idempotent(self):
        self.admin.roles["client-punto"].pop()
        ids, roles = MANAGED.ensure_clients(self.admin)
        self.assertEqual(set(ids), {"punto", "tickets"})
        self.assertEqual(set(roles["punto"]), {"manager", "worker"})
        for app, client_id in MANAGED.config.APPS.items():
            cid = ids[app]
            self.assertEqual({r["name"] for r in self.admin.scopes[cid]}, set(MANAGED.ROLES[app]))
            mapper = {key: value for key, value in self.admin.mappers[cid][0].items() if key != "id"}
            self.assertEqual(mapper, MANAGED.config.managed_role_mapper(client_id))
        self.admin.commands.clear()
        MANAGED.ensure_clients(self.admin)
        self.assertEqual(self.admin.mutations, [])

    def test_existing_incompatible_mapper_is_not_overwritten(self):
        for cid in ("client-punto", "client-tickets"):
            self.admin.scopes[cid] = copy.deepcopy(self.admin.roles[cid])
        mapper = MANAGED.config.managed_role_mapper("punto")
        mapper["config"]["claim.name"] = "another-owner-claim"
        self.admin.mappers["client-punto"] = [dict(mapper, id="existing-mapper")]
        with self.assertRaises(MANAGED.AdministrationError):
            MANAGED.ensure_clients(self.admin)
        self.assertEqual(self.admin.mutations, [])
        self.assertEqual(self.admin.mappers["client-punto"][0]["config"]["claim.name"], "another-owner-claim")


if __name__ == "__main__":
    unittest.main()
