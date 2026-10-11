"""Explicit group ownership, safe migration retries and least-privilege reader."""
import copy
import importlib.util
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("group_configuration", ROOT / "scripts/configure_keycloak_groups.py")
GROUPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GROUPS)


class FakeAdmin:
    def __init__(self):
        self.groups = []
        self.members = {}
        self.users = {"user-a": {"id": "user-a", "enabled": True},
                      "user-b": {"id": "user-b", "enabled": False}}
        self.clients = {"management-id": {"id": "management-id", "clientId": "realm-management"}}
        self.assignments = {}
        self.commands = []
        self.before_membership = None
        self.fail_role_once = False

    @property
    def mutations(self):
        return [command for command in self.commands if command[0][0] != "get"]

    def command(self, arguments, *, payload=None):
        self.commands.append((list(arguments), copy.deepcopy(payload)))
        method, path = arguments[:2]
        parts = path.split("/")
        parameters = {arguments[i + 1].split("=", 1)[0]: arguments[i + 1].split("=", 1)[1]
                      for i in range(len(arguments)) if arguments[i] == "-q"}
        value = None
        if method == "get" and path == "groups/count":
            value = {"count": len(self.groups)}
        elif method == "get" and (path == "groups" or parts[0] == "groups"):
            if path == "groups":
                rows = [group for group in self.groups if group.get("parentId") is None]
            elif parts[2] == "children":
                rows = [group for group in self.groups if group.get("parentId") == parts[1]]
            elif parts[2] == "members":
                rows = [self.users[subject] for subject in self.members.get(parts[1], [])]
            else:
                raise AssertionError(path)
            first, maximum = int(parameters.get("first", 0)), int(parameters.get("max", 100))
            value = rows[first:first + maximum]
        elif method == "create" and (path == "groups" or parts[0] == "groups"):
            gid = "created-group-" + str(len(self.groups) + 1)
            self.groups.append({"id": gid, "name": payload["name"], "parentId": parts[1] if len(parts) > 1 else None})
            return gid.encode()
        elif path == "clients":
            if method == "get":
                value = [client for client in self.clients.values() if client["clientId"] == parameters["clientId"]]
            elif method == "create":
                self.clients["sync-client-id"] = dict(copy.deepcopy(payload), id="sync-client-id")
                self.clients["sync-client-id"]["defaultClientScopes"] = ["service_account", "roles"]
                self.users["service-sub"] = {"id": "service-sub", "enabled": True}
                return b"sync-client-id"
        elif parts[0] == "clients" and len(parts) == 2:
            value = self.clients[parts[1]]
        elif parts[0] == "clients" and parts[2] == "client-secret":
            value = {"value": self.clients[parts[1]]["secret"]}
        elif parts[0] == "clients" and parts[2] == "service-account-user":
            value = {"id": "service-sub"}
        elif parts[0] == "clients" and parts[2] == "roles":
            value = {"id": "role-" + parts[3], "name": parts[3]}
        elif (parts[0] == "clients" and parts[2] == "scope-mappings") or (parts[0] == "users" and len(parts) > 2 and parts[2] == "role-mappings"):
            if method == "get":
                value = self.assignments.get(path, [])
            elif method == "create":
                if self.fail_role_once:
                    self.fail_role_once = False
                    raise GROUPS.AdministrationError("fixture role failure")
                self.assignments.setdefault(path, []).extend(copy.deepcopy(payload))
        elif parts[0] == "users" and len(parts) == 2:
            value = self.users.get(parts[1], {})
        elif parts[0] == "users" and parts[2] == "groups" and method == "update":
            if self.before_membership:
                self.before_membership(parts[3])
            self.members.setdefault(parts[3], []).append(parts[1])
        else:
            raise AssertionError("Unexpected command: " + str(arguments))
        return json.dumps(value).encode() if value is not None else b""


class GroupConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="group-migration-")
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.admin = FakeAdmin()
        self.users = {"a@example.test": {"subject": "user-a", "apps": {"punto": {"id": 10}}},
                      "b@example.test": {"subject": "user-b", "apps": {"punto": {"id": 20}}}}
        # Deliberately child first; migration must preserve the explicit parent.
        self.legacy = {"groups": [{"id": 9, "name": "Support", "parent_id": 3, "members": [20]},
                                  {"id": 3, "name": "Operations", "parent_id": None, "members": [10]}]}

    def migrate(self):
        return GROUPS.migrate(self.admin, self.data, self.legacy, self.users)

    def test_explicit_ids_hierarchy_members_and_private_secret_are_preserved(self):
        def ownership_written_before_membership(subject):
            manifest = GROUPS.config.read_json(self.data / "managed-groups-migration.json")
            self.assertTrue(any(value["subject"] == subject for value in manifest.values()))

        self.admin.before_membership = ownership_written_before_membership
        result = self.migrate()
        self.assertEqual((result["groups"], result["created"], result["memberships_added"]), (2, 2, 2))
        bindings = GROUPS.config.read_json(self.data / "bindings/groups.json")
        self.assertEqual(bindings, {"created-group-1": 3, "created-group-2": 9})
        self.assertEqual(self.admin.groups[1]["parentId"], "created-group-1")
        self.assertEqual(self.admin.members, {"created-group-1": ["user-a"], "created-group-2": ["user-b"]})
        secret = self.data / "secrets/groups-client-secret"
        self.assertEqual(stat.S_IMODE(secret.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((self.data / "bindings/groups.json").stat().st_mode), 0o600)
        self.assertNotIn(secret.read_text().strip(), json.dumps(result))
        client = self.admin.clients["sync-client-id"]
        self.assertTrue(client["serviceAccountsEnabled"])
        for flag in ("standardFlowEnabled", "implicitFlowEnabled", "directAccessGrantsEnabled", "fullScopeAllowed"):
            self.assertFalse(client[flag])
        self.assertEqual({role["name"] for role in self.admin.assignments["users/service-sub/role-mappings/clients/management-id"]},
                         {"query-groups", "view-users"})
        self.assertTrue(all(command[0][1] != "users/user-a" for command in self.admin.mutations))

    def test_retry_keeps_remote_rename_and_does_not_restore_removed_membership(self):
        self.migrate()
        self.admin.groups[0]["name"] = "Renamed centrally"
        self.admin.members["created-group-1"] = []
        before_secret = (self.data / "secrets/groups-client-secret").read_bytes()
        self.admin.commands.clear()
        result = self.migrate()
        self.assertEqual((result["created"], result["memberships_added"]), (0, 0))
        self.assertEqual(self.admin.groups[0]["name"], "Renamed centrally")
        self.assertEqual(self.admin.members["created-group-1"], [])
        self.assertEqual(self.admin.mutations, [])
        self.assertEqual((self.data / "secrets/groups-client-secret").read_bytes(), before_secret)

    def test_preexisting_group_name_collision_never_adopted(self):
        self.admin.groups.append({"id": "foreign-group", "name": "Operations", "parentId": None})
        with self.assertRaisesRegex(GROUPS.AdministrationError, "omonimo"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])
        self.assertFalse((self.data / "bindings/groups.json").exists())

    def test_preexisting_sync_client_without_manifest_never_adopted(self):
        self.admin.clients["foreign-client"] = {"id": "foreign-client", "clientId": GROUPS.CLIENT_ID}
        with self.assertRaisesRegex(GROUPS.AdministrationError, "proprietà esplicita"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])

    def test_known_group_missing_is_not_recreated(self):
        self.migrate()
        self.admin.groups.pop()
        self.admin.commands.clear()
        with self.assertRaisesRegex(GROUPS.AdministrationError, "registrato assente"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])

    def test_missing_subject_or_unknown_local_member_fails_before_mutation(self):
        self.legacy["groups"][0]["members"] = [30]
        with self.assertRaisesRegex(GROUPS.AdministrationError, "senza subject"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])
        self.legacy["groups"][0]["members"] = [20]
        self.admin.users.pop("user-b")
        with self.assertRaisesRegex(GROUPS.AdministrationError, "assente"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])

    def test_partial_service_client_provisioning_reuses_registered_identity(self):
        self.admin.fail_role_once = True
        with self.assertRaisesRegex(GROUPS.AdministrationError, "fixture role"):
            self.migrate()
        manifest = GROUPS.config.read_json(self.data / "groups-sync-client.json")
        self.assertEqual(manifest["id"], "sync-client-id")
        self.admin.commands.clear()
        self.migrate()
        self.assertFalse(any(command[0][:2] == ["create", "clients"] for command in self.admin.commands))

    def test_secret_mismatch_or_extra_permissions_are_not_overwritten(self):
        self.migrate()
        self.admin.clients["sync-client-id"]["secret"] = "b" * 64
        self.admin.commands.clear()
        with self.assertRaisesRegex(GROUPS.AdministrationError, "Segreto remoto"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])
        self.admin.clients["sync-client-id"]["secret"] = (self.data / "secrets/groups-client-secret").read_text().strip()
        self.admin.assignments["users/service-sub/role-mappings/clients/management-id"].append({"id": "extra", "name": "realm-admin"})
        self.admin.commands.clear()
        with self.assertRaisesRegex(GROUPS.AdministrationError, "permessi non previsti"):
            self.migrate()
        self.assertEqual(self.admin.mutations, [])

    def test_legacy_graph_names_and_member_limits(self):
        invalid = [
            {"groups": [{"id": 1, "name": "A", "parent_id": 1, "members": []}]},
            {"groups": [{"id": 1, "name": "A", "parent_id": 2, "members": []}]},
            {"groups": [{"id": 1, "name": "é" * 121, "parent_id": None, "members": []}]},
            {"groups": [{"id": 1, "name": "A", "parent_id": None, "members": [10, 10]}]},
            {"groups": [{"id": 1, "name": "A", "parent_id": None, "members": list(range(1, 202))}]},
            {"groups": [{"id": i + 1, "name": "A", "parent_id": i or None, "members": []} for i in range(17)]},
            {"groups": [{"id": i + 1, "name": "A", "parent_id": None, "members": []} for i in range(2)]},
        ]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(GROUPS.AdministrationError):
                GROUPS.validate_legacy(value)

    def test_empty_directory_initializes_reader_and_empty_binding(self):
        self.legacy = {"groups": []}
        result = self.migrate()
        self.assertEqual(result["groups"], 0)
        self.assertEqual(GROUPS.config.read_json(self.data / "bindings/groups.json"), {})
        self.assertEqual(self.admin.groups, [])

    def test_unicode_legacy_name_uses_character_limit(self):
        value = {"groups": [{"id": 1, "name": "é" * 61, "parent_id": None, "members": []}]}
        self.assertEqual(GROUPS.validate_legacy(value)[0]["name"], "é" * 61)
        value["groups"][0]["name"] = "é" * 121
        with self.assertRaisesRegex(GROUPS.AdministrationError, "120 caratteri"):
            GROUPS.validate_legacy(value)


if __name__ == "__main__":
    unittest.main()
