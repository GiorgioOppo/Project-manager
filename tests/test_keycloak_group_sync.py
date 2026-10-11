"""Stable group identity, complete directory snapshots and verified HTTPS."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("group_sync", ROOT / "auth/keycloak/sync_groups.py")
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)
ISSUER = "https://auth.example.test/realms/punto"


class DirectoryReader:
    def __init__(self, groups=None, members=None):
        self.groups = groups if groups is not None else [
            {"id": "root-id", "name": "Operations", "parentId": None},
            {"id": "child-id", "name": "Support", "parentId": "root-id"}]
        self.members = members if members is not None else {
            "root-id": [{"id": "active-sub", "enabled": True}, {"id": "disabled-sub", "enabled": False}],
            "child-id": [{"id": "child-sub", "enabled": True}]}
        self.calls = []

    def get(self, path, **query):
        self.calls.append((path, query))
        if path == "groups/count":
            return {"count": len(self.groups)}
        if path == "groups":
            rows = [group for group in self.groups if group.get("parentId") is None]
        else:
            _, gid, kind = path.split("/")
            if kind == "children":
                rows = [group for group in self.groups if group.get("parentId") == gid]
            elif kind == "members":
                rows = self.members.get(gid, [])
            else:
                raise AssertionError(path)
        return copy.deepcopy(rows[query.get("first", 0):query.get("first", 0) + query.get("max", 100)])


class GroupSnapshotTests(unittest.TestCase):
    def test_snapshot_uses_stable_ids_explicit_bindings_and_live_enabled_state(self):
        reader = DirectoryReader()
        snapshot = SYNC.snapshot(reader, ISSUER, {"root-id": 17}, {"active-sub": "admin@example.test"}, now=12345)
        self.assertEqual(snapshot["version"], 1)
        self.assertEqual(snapshot["issuer"], ISSUER)
        self.assertEqual(snapshot["generated_at"], 12345)
        groups = {group["id"]: group for group in snapshot["groups"]}
        self.assertEqual(groups["root-id"]["local_id"], 17)
        self.assertEqual(groups["root-id"]["members"], ["active-sub"])
        self.assertIsNone(groups["child-id"]["local_id"])
        self.assertEqual(groups["child-id"]["parent_id"], "root-id")
        self.assertEqual(len([call for call in reader.calls if call[0] == "groups"]), 2)

    def test_rename_does_not_change_binding(self):
        reader = DirectoryReader()
        reader.groups[0]["name"] = "Renamed / Operations"
        snapshot = SYNC.snapshot(reader, ISSUER, {"root-id": 17}, {}, now=1)
        root = next(group for group in snapshot["groups"] if group["id"] == "root-id")
        self.assertEqual((root["local_id"], root["name"]), (17, "Renamed / Operations"))

    def test_unicode_group_name_limit_counts_characters(self):
        reader = DirectoryReader()
        reader.groups[0]["name"] = "é" * 61
        value = SYNC.snapshot(reader, ISSUER, {}, {})
        self.assertEqual(next(group["name"] for group in value["groups"] if group["id"] == "root-id"), "é" * 61)
        reader.groups[0]["name"] = "é" * 120
        self.assertEqual(len(SYNC.snapshot(reader, ISSUER, {}, {})["groups"]), 2)
        reader.groups[0]["name"] = "é" * 121
        with self.assertRaisesRegex(SYNC.SyncError, "invalid-group-name"):
            SYNC.snapshot(reader, ISSUER, {}, {})

    def test_all_roots_children_and_members_are_explicitly_paginated(self):
        groups = [{"id": "root-id", "name": "Root", "parentId": None}]
        groups += [{"id": "child-" + str(i), "name": "Child " + str(i), "parentId": "root-id"} for i in range(105)]
        members = {"root-id": [{"id": "sub-" + str(i), "enabled": True} for i in range(105)]}
        reader = DirectoryReader(groups, members)
        result = SYNC.snapshot(reader, ISSUER, {}, {})
        self.assertEqual(len(result["groups"]), 106)
        self.assertTrue(any(path == "groups/root-id/children" and query["first"] == 100 for path, query in reader.calls))
        self.assertTrue(any(path == "groups/root-id/members" and query["first"] == 100 for path, query in reader.calls))
        roots = [{"id": "root-" + str(i), "name": "Root " + str(i), "parentId": None} for i in range(105)]
        reader = DirectoryReader(roots, {})
        self.assertEqual(len(SYNC.snapshot(reader, ISSUER, {}, {})["groups"]), 105)
        self.assertTrue(any(path == "groups" and query["first"] == 100 for path, query in reader.calls))

    def test_count_and_second_pass_detect_partial_or_changed_directory(self):
        reader = DirectoryReader()
        original = reader.get

        def count_mismatch(path, **query):
            result = original(path, **query)
            return {"count": 3} if path == "groups/count" else result

        reader.get = count_mismatch
        with self.assertRaisesRegex(SYNC.SyncError, "inconsistent-group-count"):
            SYNC.snapshot(reader, ISSUER, {}, {})
        reader = DirectoryReader()
        original = reader.get
        traversals = 0

        def changed_members(path, **query):
            nonlocal traversals
            if path == "groups":
                traversals += 1
            result = original(path, **query)
            if traversals >= 2 and path == "groups/root-id/members":
                return []
            return result

        reader.get = changed_members
        with self.assertRaisesRegex(SYNC.SyncError, "directory-changed"):
            SYNC.snapshot(reader, ISSUER, {}, {})

    def test_limits_duplicate_identity_missing_status_and_invalid_parent_rejected(self):
        cases = []
        reader = DirectoryReader()
        reader.members["root-id"][0].pop("enabled")
        cases.append(reader)
        reader = DirectoryReader()
        reader.members["root-id"] *= 2
        cases.append(reader)
        reader = DirectoryReader()
        reader.groups[0]["name"] = "é" * 121
        cases.append(reader)
        cases.append(DirectoryReader([{"id": "g-" + str(i), "name": "G", "parentId": None} for i in range(201)], {}))
        cases.append(DirectoryReader([{"id": "g-" + str(i), "name": "G", "parentId": "g-" + str(i - 1) if i else None}
                                      for i in range(17)], {}))
        reader = DirectoryReader()
        reader.members["root-id"] = [{"id": "u-" + str(i), "enabled": True} for i in range(201)]
        cases.append(reader)
        for reader in cases:
            with self.subTest(reader=reader), self.assertRaises(SYNC.SyncError):
                SYNC.snapshot(reader, ISSUER, {}, {})

    def test_atomic_snapshot_private_mode_health_and_last_good_preserved_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "groups.json"
            value = SYNC.snapshot(DirectoryReader(), ISSUER, {}, {}, now=1000)
            SYNC.atomic_snapshot(output, value)
            before = output.read_bytes()
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o640)
            self.assertTrue(SYNC.healthy(output, now=1119))
            self.assertFalse(SYNC.healthy(output, now=1120))
            self.assertFalse(SYNC.healthy(output, now=900))
            with patch.object(Path, "replace", side_effect=OSError("fixture failure")):
                with self.assertRaises(SYNC.SyncError):
                    SYNC.atomic_snapshot(output, dict(value, generated_at=1100))
            self.assertEqual(output.read_bytes(), before)
            self.assertEqual(list(Path(directory).glob(".groups-*")), [])

    def test_bindings_refuse_duplicate_local_id_and_noncanonical_email(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bindings.json"
            path.write_text('{"a":1,"b":1}')
            with self.assertRaises(SYNC.SyncError):
                SYNC.load_group_bindings(path)
            path.write_text('{"a":"UPPER@example.test"}')
            with self.assertRaises(SYNC.SyncError):
                SYNC.load_user_bindings(path)
            path.write_text('{"a":1,"a":2}')
            with self.assertRaises(SYNC.SyncError):
                SYNC.load_group_bindings(path)


class HTTPSReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="group-sync-https-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.cert = cls.directory / "server.crt"
        key = cls.directory / "server.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-keyout", str(key), "-out", str(cls.cert), "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                       check=True, capture_output=True, timeout=30)
        cls.secret = cls.directory / "secret"
        cls.secret.write_text("a" * 64)
        cls.requests = []
        cls.status = 200

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def respond(self, value):
                raw = json.dumps(value).encode()
                self.send_response(cls.status)
                if cls.status == 302:
                    self.send_header("Location", "https://example.invalid/steal")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_POST(self):
                body = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
                cls.requests.append((self.path, dict(self.headers), body))
                self.respond({"access_token": "fixture-token", "token_type": "Bearer", "expires_in": 300})

            def do_GET(self):
                cls.requests.append((self.path, dict(self.headers), None))
                self.respond({"count": 0})

        try:
            cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        except PermissionError:
            raise unittest.SkipTest("Local HTTPS fixture requires socket permission")
        cls.addClassCleanup(cls.server.server_close)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.cert, key)
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.server.shutdown)
        cls.backend = "https://127.0.0.1:" + str(cls.server.server_port) + "/realms/punto"

    def setUp(self):
        self.requests.clear()
        type(self).status = 200

    def reader(self):
        return SYNC.KeycloakReader(self.backend, "punto-groups-sync", self.secret, self.cert)

    def test_verified_https_client_credentials_and_cached_bearer_no_ambient_proxy(self):
        with patch.dict(os.environ, {"HTTPS_PROXY": "http://127.0.0.1:1", "NO_PROXY": ""}):
            reader = self.reader()
            self.assertEqual(reader.get("groups/count", top="false"), {"count": 0})
            self.assertEqual(reader.get("groups/count"), {"count": 0})
        self.assertEqual(len(self.requests), 3)
        self.assertEqual(self.requests[0][2]["client_secret"], ["a" * 64])
        self.assertEqual(self.requests[0][2]["grant_type"], ["client_credentials"])
        self.assertEqual(self.requests[1][1]["Authorization"], "Bearer fixture-token")
        self.assertTrue(self.requests[1][0].startswith("/admin/realms/punto/groups/count?"))

    def test_untrusted_certificate_and_redirect_are_rejected(self):
        reader = self.reader()
        reader.opener = SYNC.build_opener(SYNC.ProxyHandler({}), SYNC.HTTPSHandler(context=ssl.create_default_context()), SYNC.NoRedirect())
        with self.assertRaisesRegex(SYNC.SyncError, "https-unavailable"):
            reader.get("groups/count")
        type(self).status = 302
        with self.assertRaisesRegex(SYNC.SyncError, "redirect-rejected"):
            self.reader().get("groups/count")

    def test_permission_failure_contains_no_token_or_response(self):
        reader = self.reader()
        reader.access_token()
        type(self).status = 403
        with self.assertRaisesRegex(SYNC.SyncError, "^http-403$"):
            reader.get("groups/count")


if __name__ == "__main__":
    unittest.main()
