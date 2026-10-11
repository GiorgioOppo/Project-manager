"""Session authorization and verified TLS for the separate inventory service."""

import json
import socket
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlencode, urlsplit

from test_calendar_client import FakeCalendarAPI as PrivateAPI
from test_mail_client import create_certificate
import test_roles as roles

TOKEN = "inventory-client-" + "e" * 64
ENVIRONMENT = ("INVENTORY_API_URL", "INVENTORY_API_CA_FILE", "INVENTORY_API_TOKEN_FILE")


class InventoryBridgeTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    assert_error = roles.RoleIntegrationTests.assert_error
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user

    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-inventory-bridge-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "inventory-bridge")
        cls.other_ca, _ = create_certificate(cls.certificates.name, "inventory-other-ca")
        cls.wrong_cert, cls.wrong_key = create_certificate(cls.certificates.name, "inventory-wrong-name", "wrong-api.invalid")

    def setUp(self):
        self.service = PrivateAPI(self.cert, self.key)
        self.addCleanup(self.service.close)
        self.service.reply_value = {"ok": True, "items": [], "next_after_id": None}
        self.token_file = Path(self.certificates.name) / "api-token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.environment = dict(zip(ENVIRONMENT, (self.service.url, str(self.cert), str(self.token_file))))
        with patch.dict(os.environ, self.environment):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.manager_id = next(user["id"] for user in self.users() if user["email"] == roles.MANAGER_EMAIL)

    def test_reads_require_a_current_account_and_allow_workers(self):
        for path in ("status", "assets", "assets/1", "assets/1/history", "assets/1/relations", "assets/export", "due"):
            self.assert_error(401, "GET", "/api/inventory/" + path)
            for cookie in (self.manager_cookie, self.worker_cookie):
                status, _, _ = self.api("GET", "/api/inventory/" + path, cookie=cookie)
                self.assertEqual(status, 200)
                self.assertEqual(self.service.requests[-1]["path"], "/v1/" + path)
                self.assertEqual(self.service.requests[-1]["authorization"], "Bearer " + TOKEN)

    def test_export_and_due_filters_are_forwarded_without_identity(self):
        query = {"q": "Laptop & caffè", "status": "assigned", "asset_type": "it_device", "limit": "25", "after_id": "123"}
        status, _, _ = self.api("GET", "/api/inventory/assets/export?" + urlencode(query), cookie=self.worker_cookie)
        self.assertEqual(status, 200)
        request = self.service.requests[-1]
        self.assertEqual(urlsplit(request["path"]).path, "/v1/assets/export")
        self.assertEqual(parse_qs(urlsplit(request["path"]).query), {key: [value] for key, value in query.items()})
        self.assertEqual(request["fields"], {})
        due = {"today": "2026-10-11", "days_before": "365", "limit": "100", "after_id": "123"}
        status, _, _ = self.api("GET", "/api/inventory/due?" + urlencode(due), cookie=self.worker_cookie)
        self.assertEqual(status, 200)
        request = self.service.requests[-1]
        self.assertEqual(urlsplit(request["path"]).path, "/v1/due")
        self.assertEqual(parse_qs(urlsplit(request["path"]).query), {key: [value] for key, value in due.items()})
        self.assertEqual(request["fields"], {})

    def test_import_preview_and_commit_receive_only_authenticated_actor(self):
        csv = 'asset_tag,name,notes\r\nIT-01,"Portatile caffè & qualità","Prima riga\nSeconda riga"\r\n'
        self.service.reply_value = {"ok": True, "preview_id": "a" * 32, "items": []}
        for mode in (None, "create_only", "upsert"):
            fields = {"csv": csv}
            if mode:
                fields["mode"] = mode
            status, value, _ = self.api("POST", "/api/inventory/import/preview", fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            request = self.service.requests[-1]
            self.assertEqual((request["method"], request["path"]), ("POST", "/v1/import/preview"))
            self.assertEqual(request["fields"], {key: [value] for key, value in dict(fields, actor_id=str(self.manager_id)).items()})
        self.service.reply_value = {"ok": True, "created": 1, "updated": 0}
        status, value, _ = self.api("POST", "/api/inventory/import/commit", {"preview_id": "a" * 32}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, value)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/import/commit")
        self.assertEqual(self.service.requests[-1]["fields"], {"preview_id": ["a" * 32], "actor_id": [str(self.manager_id)]})

    def test_import_permissions_origin_media_and_method_guards(self):
        for path, fields in (("import/preview", {"csv": "asset_tag,name\nIT-01,Laptop\n"}),
                             ("import/commit", {"preview_id": "a" * 32})):
            route = "/api/inventory/" + path
            self.assert_error(401, "POST", route, fields)
            self.assert_error(403, "POST", route, fields, cookie=self.worker_cookie)
            self.assert_error(403, "POST", route, fields, cookie=self.manager_cookie, headers={"Origin": "https://hostile.invalid"})
            self.assert_error(415, "POST", route, cookie=self.manager_cookie, body="{}", headers={"Content-Type": "application/json"})
            self.assert_error(400, "POST", route + "?mode=upsert", fields, cookie=self.manager_cookie)
            self.assert_error(405, "GET", route, cookie=self.manager_cookie)
            for identity in ("actor_id", "user_id", "owner_id"):
                self.assert_error(400, "POST", route, dict(fields, **{identity: "99"}), cookie=self.manager_cookie)
        for path in ("assets/export", "due"):
            self.assert_error(405, "POST", "/api/inventory/" + path, {}, cookie=self.manager_cookie)
        self.assertEqual(self.service.requests, [])

    def test_import_rejects_unknown_duplicate_malformed_and_out_of_range_fields(self):
        for path, invalid in (
                ("import/preview", ("", "mode=upsert", "csv=", "csv=a&mode=replace", "csv=a&mode=", "csv=a&version=1",
                                    "csv=a&csv=b", "csv=a&%63sv=b", "csv=a&actor%5fid=99", "csv=a&mode=upsert&mode=create_only",
                                    "csv=%00", "csv=%C0%AF", "csv=a&")),
                ("import/commit", ("", "preview_id=", "preview_id=" + "a" * 31, "preview_id=" + "a" * 33,
                                   "preview_id=" + "g" * 32, "preview_id=" + "a" * 32 + "&csv=a",
                                   "preview_id=" + "a" * 32 + "&preview%5fid=" + "b" * 32,
                                   "preview_id=" + "a" * 32 + "&actor_id=99"))):
            for body in invalid:
                with self.subTest(path=path, body=body[:80]):
                    self.assert_error(400, "POST", "/api/inventory/" + path, cookie=self.manager_cookie, body=body,
                                      headers={"Content-Type": "application/x-www-form-urlencoded"})
        for query in ("days_before=0", "days_before=366", "days_before=1.5", "today=2026-1-01", "today=20261011",
                      "today=2026-ab-11", "days_before=30&days%5fbefore=10", "actor_id=1", "asset_type=license", "limit=101", "after_id=0"):
            self.assert_error(400, "GET", "/api/inventory/due?" + query, cookie=self.worker_cookie)
        for query in ("status=unknown", "actor_id=1", "mode=upsert", "limit=0", "asset_type=invalid", "q=a&q=b"):
            self.assert_error(400, "GET", "/api/inventory/assets/export?" + query, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_large_csv_preview_is_bounded_after_decoding_and_only_on_exact_path(self):
        # Percent encoding expands the accepted 256 KiB CSV past the ordinary
        # 32 KiB form limit while remaining within the dedicated 1 MiB limit.
        body = "csv=" + "%61" * (256 * 1024)
        status, value, _ = self.api("POST", "/api/inventory/import/preview", cookie=self.manager_cookie, body=body,
                                  headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(status, 200, value)
        self.assertEqual(self.service.requests[-1]["fields"]["csv"], ["a" * (256 * 1024)])
        count = len(self.service.requests)
        self.assert_error(400, "POST", "/api/inventory/import/preview", {"csv": "a" * (256 * 1024 + 1)}, cookie=self.manager_cookie)
        self.assertEqual(len(self.service.requests), count)

        def declared_status(path, length, method="POST"):
            # Send only the headers: oversized declarations must be refused
            # before the server allocates or waits for the advertised body.
            with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
                request = ("%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %d\r\n"
                           "Content-Type: application/x-www-form-urlencoded\r\nConnection: close\r\n\r\n" %
                           (method, path, self.port, length))
                connection.sendall(request.encode("ascii"))
                reply = connection.recv(4096)
                return int(reply.split(b" ", 2)[1])

        self.assertEqual(declared_status("/api/inventory/import/preview", 1024 * 1024 + 1), 413)
        for path in ("/api/inventory/import/preview?mode=upsert", "/api/inventory/import/preview/",
                     "/api/inventory/import/commit", "/api/inventory/assets", "/api/inventory/import/preview-other"):
            self.assertEqual(declared_status(path, 32769), 413, path)
        self.assertEqual(declared_status("/api/inventory/import/preview", 32769, method="GET"), 413)

    def test_import_conflict_and_expired_preview_errors_are_preserved(self):
        for upstream in (403, 404, 409, 422, 429, 503):
            self.service.reply_status = upstream
            self.service.reply_value = {"ok": False, "error": "Anteprima non applicabile"}
            self.assert_error(upstream, "POST", "/api/inventory/import/commit", {"preview_id": "b" * 32}, cookie=self.manager_cookie)

    def test_upstream_preview_too_large_error_keeps_actionable_message(self):
        self.service.reply_status = 413
        message = "Anteprima troppo grande: dividi il file CSV in più parti."
        self.service.reply_value = {"ok": False, "error": message}
        status, value, _ = self.api("POST", "/api/inventory/import/preview", {"csv": "asset_tag,name\nIT-01,Laptop\n"},
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 413, value)
        self.assertEqual(value, {"ok": False, "error": message})

    def test_create_and_edit_use_the_authenticated_audit_actor(self):
        self.service.reply_status = 201
        status, _, _ = self.api("POST", "/api/inventory/assets", {
            "asset_tag": "IT-01", "name": "Portatile caffè & qualità", "notes": "Prima riga\nSeconda riga"},
            cookie=self.manager_cookie)
        self.assertEqual(status, 201)
        self.assertEqual(self.service.requests[-1]["fields"], {
            "asset_tag": ["IT-01"], "name": ["Portatile caffè & qualità"],
            "notes": ["Prima riga\nSeconda riga"], "actor_id": [str(self.manager_id)]})
        self.service.reply_status = 200
        status, _, _ = self.api("POST", "/api/inventory/assets/17", {
            "asset_tag": "IT-01", "name": "Portatile", "version": "2", "status": "retired"},
            cookie=self.manager_cookie)
        self.assertEqual(status, 200)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/assets/17")
        self.assertEqual(self.service.requests[-1]["fields"]["version"], ["2"])

    def test_writes_require_manager_and_same_origin(self):
        fields = {"asset_tag": "IT-01", "name": "Laptop", "version": "1"}
        for path in ("assets", "assets/1"):
            self.assert_error(403, "POST", "/api/inventory/" + path, fields, cookie=self.worker_cookie)
            self.assert_error(403, "POST", "/api/inventory/" + path, fields, cookie=self.manager_cookie,
                              headers={"Origin": "https://hostile.invalid"})
        self.assertEqual(self.service.requests, [])

    def test_role_demotion_applies_to_existing_session(self):
        other = self.create_user("inventory-manager@example.test", "InventoryManager!2026", role="manager")
        cookie = self.login("inventory-manager@example.test", "InventoryManager!2026")
        self.edit_user(other, role="worker")
        self.assert_error(403, "POST", "/api/inventory/assets", {"asset_tag": "IT-01", "name": "Laptop"}, cookie=cookie)
        self.assertEqual(self.service.requests, [])

    def test_filters_and_history_cursors_are_forwarded_canonically(self):
        query = {"q": "Laptop & caffè", "status": "assigned", "asset_type": "it_device", "limit": "25", "after_id": "123"}
        self.api("GET", "/api/inventory/assets?" + urlencode(query), cookie=self.worker_cookie)
        sent = urlsplit(self.service.requests[-1]["path"])
        self.assertEqual(sent.path, "/v1/assets")
        self.assertEqual(parse_qs(sent.query), {key: [value] for key, value in query.items()})
        self.api("GET", "/api/inventory/assets/1/history?limit=10&after_id=6", cookie=self.worker_cookie)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/assets/1/history?limit=10&after_id=6")

    def test_typed_details_cross_bridge_without_changing_audit_identity(self):
        details = {"publisher": "Qualità & software", "software_version": "2.4", "edition": "Pro"}
        fields = {"asset_tag": "SW-001", "name": "Software aziendale", "asset_type": "software",
                  "details": json.dumps(details, ensure_ascii=False, indent=2)}
        self.service.reply_status = 201
        status, _, _ = self.api("POST", "/api/inventory/assets", fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201)
        self.assertEqual(json.loads(self.service.requests[-1]["fields"]["details"][0]), details)
        self.assertEqual(self.service.requests[-1]["fields"]["asset_type"], ["software"])
        self.assertEqual(self.service.requests[-1]["fields"]["actor_id"], [str(self.manager_id)])
        for invalid in ("[]", "null", '{"publisher":', '{"publisher":"' + "a" * 4096 + '"}'):
            self.assert_error(400, "POST", "/api/inventory/assets", dict(fields, details=invalid), cookie=self.manager_cookie)
        self.assertEqual(len(self.service.requests), 1)

    def test_relation_create_remove_and_pagination_use_only_server_supplied_actor(self):
        fields = {"relation_type": "installed_on", "target_id": "7", "version": "3",
                  "installed_version": "2.4", "notes": "Installazione verificata\nSeconda riga"}
        self.service.reply_status = 201
        status, _, _ = self.api("POST", "/api/inventory/assets/4/relations", fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201)
        sent = self.service.requests[-1]
        self.assertEqual(sent["path"], "/v1/assets/4/relations")
        self.assertEqual(sent["fields"], {key: [value] for key, value in dict(fields, actor_id=str(self.manager_id)).items()})
        self.service.reply_status = 200
        self.api("GET", "/api/inventory/assets/7/relations?limit=10&after_id=21", cookie=self.worker_cookie)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/assets/7/relations?limit=10&after_id=21")
        status, _, _ = self.api("POST", "/api/inventory/assets/4/relations/21/remove", {"version": "4"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200)
        self.assertEqual(self.service.requests[-1]["path"], "/v1/assets/4/relations/21/remove")
        self.assertEqual(self.service.requests[-1]["fields"], {"version": ["4"], "actor_id": [str(self.manager_id)]})

    def test_relations_require_manager_origin_version_and_known_fields(self):
        fields = {"relation_type": "licensed_to", "assignee": "Mario Rossi", "quantity": "2", "version": "3"}
        for path, values in (("assets/4/relations", fields), ("assets/4/relations/21/remove", {"version": "4"})):
            self.assert_error(401, "POST", "/api/inventory/" + path, values)
            self.assert_error(403, "POST", "/api/inventory/" + path, values, cookie=self.worker_cookie)
            self.assert_error(403, "POST", "/api/inventory/" + path, values, cookie=self.manager_cookie,
                              headers={"Origin": "https://hostile.invalid"})
            self.assert_error(400, "POST", "/api/inventory/" + path, dict(values, actor_id="15"), cookie=self.manager_cookie)
        for values in ({"relation_type": "installed_on", "target_id": "3"},
                       {"relation_type": "other", "version": "1"},
                       {"relation_type": "connected_to", "version": "1", "target_id": "0"},
                       dict(fields, quantity="1000001"), dict(fields, source_id="99")):
            self.assert_error(400, "POST", "/api/inventory/assets/4/relations", values, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/inventory/assets/4/relations/21/remove", {}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/inventory/assets/4/relations/21/remove", {"version": "4", "target_id": "5"}, cookie=self.manager_cookie)
        self.assert_error(400, "GET", "/api/inventory/assets/4/relations?q=bad", cookie=self.worker_cookie)
        for route in ("assets/4/relations/0/remove", "assets/4/relations/9223372036854775808/remove",
                      "assets/4/relations/21/other", "assets/4/relations/21/remove/extra"):
            self.assert_error(404, "POST", "/api/inventory/" + route, {"version": "4"}, cookie=self.manager_cookie)
        self.assert_error(405, "GET", "/api/inventory/assets/4/relations/21/remove", cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_relation_rich_history_keeps_bounded_responses_without_member_truncation(self):
        self.service.reply_value = {"ok": True, "items": [{"id": i,
            "snapshot": {"details": {"key" + str(k): "x" for k in range(25)}},
            "change": {"relation": {"field" + str(k): "x" for k in range(25)}}} for i in range(100)]}
        status, value, _ = self.api("GET", "/api/inventory/assets/4/history?limit=100", cookie=self.worker_cookie)
        self.assertEqual(status, 200)
        self.assertEqual(len(value["items"]), 100)

    def test_identity_unknown_duplicate_and_malformed_fields_never_cross_bridge(self):
        invalid = ("asset_tag=A&name=B&actor_id=5", "asset_tag=A&name=B&actor%5fid=5",
                   "asset_tag=A&name=B&user_id=5", "asset_tag=A&name=B&name=C",
                   "asset_tag=A&name=B&%6eame=C", "asset_tag=A&name=B&", "asset_tag=A&name=%00",
                   "asset_tag=A&name=%C0%AF", "asset_tag=A&name=%F4%90%80%80",
                   "asset_tag=A&name=B&provider_url=https%3A%2F%2Fhostile.invalid",
                   "asset_tag=A&name=B&version=1", "asset_tag=A&name=B%0Ahidden", "name=only")
        for body in invalid:
            with self.subTest(body=body):
                self.assert_error(400, "POST", "/api/inventory/assets", cookie=self.manager_cookie,
                                  body=body, headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.service.requests, [])

    def test_queries_reject_hidden_identity_bounds_and_unknown_fields(self):
        for path in ("status?q=a", "assets/1?limit=1", "assets?actor_id=1", "assets?limit=0", "assets?limit=101",
                     "assets?limit=1&%6cimit=2", "assets?status=broken", "assets?after_id=9223372036854775808",
                     "assets/1/history?q=private"):
            with self.subTest(path=path):
                self.assert_error(400, "GET", "/api/inventory/" + path, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_item_routes_methods_body_and_media_type_are_checked(self):
        for path in ("assets/-1", "assets/0", "assets/1/other", "assets/9223372036854775808", "other"):
            self.assert_error(404, "GET", "/api/inventory/" + path, cookie=self.worker_cookie)
        for path in ("status", "assets/1/history"):
            self.assert_error(405, "POST", "/api/inventory/" + path, {}, cookie=self.manager_cookie)
        self.assert_error(405, "DELETE", "/api/inventory/assets/1", cookie=self.manager_cookie)
        self.assert_error(415, "POST", "/api/inventory/assets", cookie=self.manager_cookie, body="{}",
                          headers={"Content-Type": "application/json"})
        self.assert_error(400, "POST", "/api/inventory/assets?actor_id=1", {"asset_tag": "A", "name": "B"}, cookie=self.manager_cookie)
        self.assert_error(400, "GET", "/api/inventory/assets", cookie=self.worker_cookie, body="unexpected")
        self.assert_error(400, "POST", "/api/inventory/assets/1", {"asset_tag": "A", "name": "B"}, cookie=self.manager_cookie)
        self.assertEqual(self.service.requests, [])

    def test_upstream_conflicts_are_preserved_and_bad_responses_are_unavailable(self):
        for status, value in ((409, {"ok": False, "error": "Versione modificata"}),
                              (503, {"ok": False, "error": "Archivio non disponibile"}),
                              (200, b"not-json"), (200, b'{"x":"' + b"a" * 140000 + b'"}'),
                              (200, {"authorization": "private-token"}),
                              (302, {"ok": True}), (401, {"ok": False})):
            self.service.reply_status, self.service.reply_value = status, value
            response, _, _ = self.api("GET", "/api/inventory/assets", cookie=self.worker_cookie)
            self.assertEqual(response, status if status in (409, 503) else 503)

    def test_private_requests_ignore_external_proxy_environment(self):
        self.stop_server()
        with patch.dict(os.environ, dict(self.environment, HTTPS_PROXY="http://127.0.0.1:1", https_proxy="http://127.0.0.1:1", NO_PROXY="", no_proxy="")):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, _, _ = self.api("GET", "/api/inventory/assets", cookie=cookie)
        self.assertEqual(status, 200)

    def test_untrusted_ca_fails_before_private_http_request(self):
        self.stop_server()
        with patch.dict(os.environ, dict(self.environment, INVENTORY_API_CA_FILE=str(self.other_ca))):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assert_error(503, "GET", "/api/inventory/assets", cookie=cookie)
        self.assertEqual(self.service.requests, [])

    def test_wrong_certificate_hostname_fails_before_private_http_request(self):
        wrong = PrivateAPI(self.wrong_cert, self.wrong_key)
        self.addCleanup(wrong.close)
        self.stop_server()
        with patch.dict(os.environ, dict(self.environment, INVENTORY_API_URL=wrong.url,
                                        INVENTORY_API_CA_FILE=str(self.wrong_cert))):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assert_error(503, "GET", "/api/inventory/assets", cookie=cookie)
        self.assertEqual(wrong.requests, [])

    def test_omitted_configuration_keeps_native_app_available(self):
        self.stop_server()
        with patch.dict(os.environ, {key: "" for key in ENVIRONMENT}):
            self.start_server()
        cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        status, value, _ = self.api("GET", "/api/inventory/status", cookie=cookie)
        self.assertEqual(status, 200)
        self.assertIs(value["configured"], False)
        self.assert_error(503, "GET", "/api/inventory/assets", cookie=cookie)


class InventoryConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="punto-inventory-client-config-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.cert, _ = create_certificate(cls.directory.name, "config")
        cls.token = Path(cls.directory.name) / "token"
        cls.token.write_text(TOKEN + "\n", encoding="ascii")
        cls.token.chmod(0o600)
        source = Path(cls.directory.name) / "check.c"
        source.write_text('#include "inventory_client.h"\n#include <stdio.h>\n'
                          'int main(void){if(!inventory_client_open()){puts("invalid");return 0;} '
                          'printf("enabled=%d\\n",inventory_client_enabled());inventory_client_close();return 0;}\n')
        cls.binary = Path(cls.directory.name) / "check"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "json-c"], text=True).split()
        subprocess.run([shutil.which("cc") or "cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(roles.ROOT / "src"), str(source), str(roles.ROOT / "src/inventory_client.c"),
                        "-o", str(cls.binary), "-lcurl", *flags], check=True, capture_output=True)

    def check(self, **values):
        environment = {key: value for key, value in os.environ.items() if key not in ENVIRONMENT}
        environment.update(values)
        return subprocess.check_output([str(self.binary)], env=environment, text=True).strip()

    def test_destination_and_partial_configuration_fail_closed(self):
        self.assertEqual(self.check(), "enabled=0")
        self.assertEqual(self.check(INVENTORY_API_URL="https://inventory-api:8092"), "invalid")
        for url in ("http://inventory-api:8092", "https://example.com", "https://localhost:8092/path",
                    "https://localhost:0", "https://localhost:65536", "https://localhost:8092@hostile.invalid"):
            self.assertEqual(self.check(INVENTORY_API_URL=url, INVENTORY_API_CA_FILE=str(self.cert),
                                        INVENTORY_API_TOKEN_FILE=str(self.token)), "invalid", url)
        self.assertEqual(self.check(INVENTORY_API_URL="https://inventory-api:8092", INVENTORY_API_CA_FILE=str(self.cert),
                                    INVENTORY_API_TOKEN_FILE=str(self.token)), "enabled=1")

    def test_maximum_token_length_with_portable_line_endings(self):
        original = self.token.read_text(encoding="ascii")
        try:
            for suffix in ("", "\n", "\r\n"):
                self.token.write_bytes(("a" * 128 + suffix).encode("ascii"))
                self.assertEqual(self.check(INVENTORY_API_URL="https://inventory-api:8092",
                                            INVENTORY_API_CA_FILE=str(self.cert),
                                            INVENTORY_API_TOKEN_FILE=str(self.token)), "enabled=1")
            self.token.write_text("a" * 129, encoding="ascii")
            self.assertEqual(self.check(INVENTORY_API_URL="https://inventory-api:8092",
                                        INVENTORY_API_CA_FILE=str(self.cert),
                                        INVENTORY_API_TOKEN_FILE=str(self.token)), "invalid")
        finally:
            self.token.write_text(original, encoding="ascii")
