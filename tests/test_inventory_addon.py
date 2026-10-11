"""Asset inventory HTTPS contract against isolated TLS and SQLite fixtures."""

from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import time
import unittest
import warnings
from urllib.parse import urlencode

from test_mail_client import create_certificate

ROOT = Path(__file__).resolve().parents[1]
TOKEN = "inventory-test-" + "a" * 64


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class InventoryAddonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc") or not shutil.which("openssl"):
            raise unittest.SkipTest("C compiler and OpenSSL are required")
        subprocess.run(["make", "-C", str(ROOT / "inventory-addon")], check=True,
                       capture_output=True, text=True)
        cls.certificates = tempfile.TemporaryDirectory(prefix="punto-inventory-certs-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "inventory")
        cls.other_ca, _ = create_certificate(cls.certificates.name, "other-ca")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="punto-inventory-addon-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.token_file = self.path / "api-token"
        self.token_file.write_text(TOKEN + "\n", encoding="ascii")
        self.token_file.chmod(0o600)
        self.db = self.path / "inventory.sqlite3"
        self.context = ssl.create_default_context(cafile=str(self.cert))
        self.process = None
        self.addCleanup(self.stop)

    def environment(self, **changes):
        self.port = free_port()
        env = {name: value for name, value in os.environ.items() if not name.startswith("INVENTORY_")}
        env.update(INVENTORY_BIND="127.0.0.1", INVENTORY_ALLOWED_CLIENT="127.0.0.1",
                   INVENTORY_PORT=str(self.port), INVENTORY_TLS_CERT_FILE=str(self.cert),
                   INVENTORY_TLS_KEY_FILE=str(self.key), INVENTORY_API_TOKEN_FILE=str(self.token_file),
                   INVENTORY_DB_PATH=str(self.db))
        env.update(changes)
        return env

    def stop(self):
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                self.fail("Inventory service failed to stop gracefully")
            finally:
                self.log.close()
                self.process = None

    def start(self, **changes):
        self.stop()
        env = self.environment(**changes)
        self.log = open(self.path / "service.log", "ab")
        self.process = subprocess.Popen([str(ROOT / "inventory-addon/build/punto-inventory")], env=env,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail("Inventory service exited: " + (self.path / "service.log").read_text())
            try:
                if self.request("GET", "/healthz", authenticated=False)[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                time.sleep(0.025)
        self.fail("Inventory service did not become healthy")

    def request(self, method, path, fields=None, *, authenticated=True, token=TOKEN, raw=None,
                content_type="application/x-www-form-urlencoded", context=None):
        headers = {"Authorization": "Bearer " + token} if authenticated else {}
        body = raw
        if fields is not None:
            body = urlencode(fields)
        if body is not None:
            headers["Content-Type"] = content_type
        connection = http.client.HTTPSConnection("127.0.0.1", self.port,
                                                context=context or self.context, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            status = response.status
            self.assertEqual(response.getheader("Cache-Control"), "no-store")
            self.assertEqual(response.getheader("X-Content-Type-Options"), "nosniff")
            self.assertLessEqual(len(payload), 128 * 1024)
        finally:
            connection.close()
        self.assertNotIn(TOKEN.encode(), payload)
        value = json.loads(payload)
        self.assertIsInstance(value.get("ok"), bool)
        if status >= 400:
            self.assertFalse(value["ok"])
            self.assertIsInstance(value.get("error"), str)
            self.assertTrue(value["error"])
        return status, value

    def create(self, tag="PC-001", name="Portatile caffè", **fields):
        data = {"asset_tag": tag, "name": name, "actor_id": 7}
        data.update(fields)
        status, value = self.request("POST", "/v1/assets", data)
        self.assertEqual(status, 201, value)
        return value["asset"]

    def update(self, asset, **fields):
        data = {"asset_tag": asset["asset_tag"], "name": asset["name"],
                "actor_id": 8, "version": asset["version"]}
        data.update(fields)
        return self.request("POST", "/v1/assets/%d" % asset["id"], data)

    def test_health_auth_source_and_tls(self):
        self.start()
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)
        self.assertEqual(self.request("GET", "/v1/status", authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", "/v1/status", token=TOKEN[:-1] + "b")[0], 401)
        status, value = self.request("GET", "/v1/status")
        self.assertEqual(status, 200)
        self.assertTrue(value["configured"])
        self.assertEqual(value["summary"], {"total": 0, "available": 0, "assigned": 0,
                                             "maintenance": 0, "retired": 0, "lost": 0})
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.request("GET", "/healthz", context=ssl.create_default_context(cafile=str(self.other_ca)))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            legacy = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            legacy.check_hostname = False
            legacy.verify_mode = ssl.CERT_NONE
            legacy.minimum_version = ssl.TLSVersion.TLSv1
            legacy.maximum_version = ssl.TLSVersion.TLSv1_1
            legacy.set_ciphers("ALL:@SECLEVEL=0")
        with self.assertRaises(ssl.SSLError):
            self.request("GET", "/healthz", context=legacy)
        self.start(INVENTORY_ALLOWED_CLIENT="127.0.0.2")
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)
        self.assertEqual(self.request("GET", "/v1/status")[0], 403)

    def test_create_update_history_and_restart_persistence(self):
        self.start()
        asset = self.create(serial_number="SER-1", manufacturer="Esempio", model="X1",
                            category="computer", location="Roma", notes="Prima riga\nSeconda riga\tè",
                            purchase_date="2024-02-29", warranty_until="2027-02-28")
        self.assertEqual(asset["version"], 1)
        self.assertEqual(asset["actor_id"], 7)
        self.assertEqual(asset["status"], "available")
        self.assertEqual(asset["created_at"], asset["updated_at"])
        self.assertRegex(asset["created_at"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        status, value = self.request("GET", "/v1/assets/%d" % asset["id"])
        self.assertEqual((status, value["asset"]), (200, asset))
        status, value = self.update(asset, status="assigned", assignee="Mario Rossi", name="Nuovo nome")
        self.assertEqual(status, 200, value)
        updated = value["asset"]
        self.assertEqual(updated["version"], 2)
        self.assertEqual(updated["actor_id"], 8)
        self.assertEqual(updated["category"], "hardware")
        for key in ("serial_number", "manufacturer", "model", "location", "notes", "purchase_date", "warranty_until"):
            self.assertEqual(updated[key], "")
        status, value = self.request("GET", "/v1/assets/%d/history?limit=1" % asset["id"])
        self.assertEqual(status, 200, value)
        newest = value["items"][0]
        self.assertEqual((newest["action"], newest["version"], newest["actor_id"]), ("updated", 2, 8))
        self.assertEqual(newest["snapshot"], updated)
        cursor = value["next_after_id"]
        self.assertEqual(cursor, newest["id"])
        status, value = self.request("GET", "/v1/assets/%d/history?after_id=%d" % (asset["id"], cursor))
        self.assertEqual(status, 200, value)
        self.assertIsNone(value["next_after_id"])
        self.assertEqual(len(value["items"]), 1)
        self.assertEqual(value["items"][0]["snapshot"], asset)
        self.assertEqual(value["items"][0]["action"], "created")
        self.start()
        self.assertEqual(self.request("GET", "/v1/assets/%d" % asset["id"])[1]["asset"], updated)
        self.assertEqual(len(self.request("GET", "/v1/assets/%d/history" % asset["id"])[1]["items"]), 2)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
        self.assertEqual(self.db.stat().st_mode & 0o077, 0)

    def test_conflicts_do_not_write_history(self):
        self.start()
        asset = self.create()
        status, value = self.request("POST", "/v1/assets", {"asset_tag": "pc-001", "name": "Duplicato", "actor_id": 1})
        self.assertEqual(status, 409, value)
        other = self.create(tag="PC-002")
        self.assertEqual(self.update(other, asset_tag="pC-001")[0], 409)
        status, value = self.update(asset, status="retired")
        self.assertEqual(status, 200, value)
        self.assertEqual(self.update(asset, name="Modifica obsoleta")[0], 409)
        current = self.request("GET", "/v1/assets/%d" % asset["id"])[1]["asset"]
        self.assertEqual(current["status"], "retired")
        self.assertEqual(current["version"], 2)
        history = self.request("GET", "/v1/assets/%d/history" % asset["id"])[1]["items"]
        self.assertEqual(len(history), 2)
        self.assertEqual(len(self.request("GET", "/v1/assets/%d/history" % other["id"])[1]["items"]), 1)
        self.assertEqual(self.request("DELETE", "/v1/assets/%d" % asset["id"])[0], 405)
        self.assertEqual(self.request("POST", "/v1/assets/99999", {"asset_tag": "NO", "name": "Missing", "version": 1, "actor_id": 1})[0], 404)

    def test_history_failure_rolls_back_whole_mutation(self):
        self.start()
        asset = self.create()
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_history BEFORE INSERT ON asset_history BEGIN SELECT RAISE(FAIL, 'fixture failure'); END")
            database.commit()
        self.assertEqual(self.update(asset, name="Non deve essere salvato")[0], 500)
        self.assertEqual(self.request("GET", "/v1/assets/%d" % asset["id"])[1]["asset"], asset)
        self.assertEqual(self.request("POST", "/v1/assets", {"asset_tag": "ROLLBACK", "name": "Non salvato", "actor_id": 1})[0], 500)
        self.assertEqual(self.request("GET", "/v1/status")[1]["summary"]["total"], 1)
        self.assertEqual(len(self.request("GET", "/v1/assets/%d/history" % asset["id"])[1]["items"]), 1)

    def test_search_status_summary_and_cursors(self):
        self.start()
        assets = [self.create(tag="TEST-%d" % i, name="Laptop" if i % 2 == 0 else "Telefono",
                              status=status, assignee="Persona" if status == "assigned" else "",
                              serial_number="S-%d" % i, location="Milano" if i == 2 else "Roma")
                  for i, status in enumerate(("available", "assigned", "maintenance", "retired", "lost"))]
        status, value = self.request("GET", "/v1/assets?limit=2")
        self.assertEqual(status, 200)
        self.assertEqual([row["id"] for row in value["items"]], [assets[4]["id"], assets[3]["id"]])
        self.assertEqual(value["summary"], {"total": 5, "available": 1, "assigned": 1, "maintenance": 1, "retired": 1, "lost": 1})
        seen = list(value["items"])
        cursor = value["next_after_id"]
        while cursor:
            status, value = self.request("GET", "/v1/assets?limit=2&after_id=%d" % cursor)
            self.assertEqual(status, 200)
            seen.extend(value["items"])
            cursor = value["next_after_id"]
        self.assertEqual([row["id"] for row in seen], [row["id"] for row in reversed(assets)])
        for query, expected in (("q=laptop", [4, 2, 0]), ("q=S-3", [3]), ("q=milano", [2]), ("status=assigned", [1]), ("q=Telefono&status=retired", [3])):
            status, value = self.request("GET", "/v1/assets?" + query)
            self.assertEqual(status, 200)
            self.assertEqual([row["id"] for row in value["items"]], [assets[i]["id"] for i in expected])
            self.assertEqual(value["summary"]["total"], 5)
        self.assertEqual(self.request("GET", "/v1/assets?q=%25")[1]["items"], [])

    def test_response_byte_budget_retains_usable_cursor(self):
        self.start()
        assets = [self.create(tag="BIG-%03d" % i, notes="\\\n" * 2000) for i in range(35)]
        seen, cursor, pages = [], None, 0
        while True:
            query = "?limit=100" + ("&after_id=%d" % cursor if cursor else "")
            status, value = self.request("GET", "/v1/assets" + query)
            self.assertEqual(status, 200, value)
            self.assertTrue(value["items"])
            seen.extend(row["id"] for row in value["items"])
            pages += 1
            self.assertLess(pages, 10)
            cursor = value["next_after_id"]
            if cursor is None:
                break
            self.assertEqual(cursor, value["items"][-1]["id"])
        self.assertGreater(pages, 1)
        self.assertEqual(seen, [row["id"] for row in reversed(assets)])
        current = assets[0]
        for i in range(20):
            status, value = self.update(current, notes="\\\n" * 2000)
            self.assertEqual(status, 200)
            current = value["asset"]
        versions, cursor = [], None
        while True:
            query = "?limit=100" + ("&after_id=%d" % cursor if cursor else "")
            status, value = self.request("GET", "/v1/assets/%d/history%s" % (current["id"], query))
            self.assertEqual(status, 200)
            versions.extend(row["version"] for row in value["items"])
            cursor = value["next_after_id"]
            if cursor is None:
                break
        self.assertEqual(versions, list(range(21, 0, -1)))

    def test_strict_form_fields_dates_and_utf8(self):
        self.start()
        base = {"asset_tag": "VALID", "name": "Bene", "actor_id": 1}
        invalid = [{"asset_tag": ""}, {"name": "  "}, {"asset_tag": "x" * 65}, {"name": "è" * 81},
                   {"category": "c" * 81}, {"serial_number": "s" * 121}, {"manufacturer": "m" * 121},
                   {"model": "m" * 121}, {"location": "l" * 161}, {"assignee": "a" * 161},
                   {"notes": "n" * 4001}, {"status": "deleted"}, {"status": "assigned"},
                   {"status": "assigned", "assignee": "   "}, {"actor_id": 0}, {"actor_id": "-1"},
                   {"actor_id": "9223372036854775808"}, {"unknown": "field"}, {"version": "1"},
                   {"purchase_date": "2023-02-29"}, {"purchase_date": "2024-04-31"},
                   {"purchase_date": "0000-01-01"}, {"warranty_until": "2024-13-01"},
                   {"purchase_date": "2024-10-02", "warranty_until": "2024-10-01"},
                   {"name": "riga\nnuova"}, {"notes": "controllo\x01"}]
        for changes in invalid:
            with self.subTest(changes=changes):
                fields = dict(base, **changes)
                self.assertEqual(self.request("POST", "/v1/assets", fields)[0], 400)
        for raw in ("asset_tag=A&name=B&actor_id=1&name=C", "asset_tag=A&name=B&actor_id=1&n%61me=C",
                    "asset_tag=A&name=B&actor_id=1&", "asset_tag=%00&name=B&actor_id=1",
                    "asset_tag=A&name=%FF&actor_id=1", "asset_tag=A&name=%C0%AF&actor_id=1",
                    "asset_tag=A&name=%ED%A0%80&actor_id=1", "asset_tag=A&name=%F4%90%80%80&actor_id=1",
                    "asset_tag=A&name=%&actor_id=1", "asset_tag=A&name=B&actor_id=1&bad"):
            with self.subTest(raw=raw):
                self.assertEqual(self.request("POST", "/v1/assets", raw=raw)[0], 400)
        self.assertEqual(self.request("POST", "/v1/assets", raw="{}", content_type="application/json")[0], 415)
        self.assertEqual(self.request("GET", "/v1/status")[1]["summary"]["total"], 0)
        asset = self.create(tag="UNICODE", notes="Prima riga\r\nCaffè ☕", purchase_date="2000-02-29")
        self.assertEqual(asset["notes"], "Prima riga\r\nCaffè ☕")
        for version in ("", "0", "1.0", "-1"):
            self.assertEqual(self.update(asset, version=version)[0], 400)

    def test_strict_queries_and_routes(self):
        self.start()
        asset = self.create()
        for query in ("limit=0", "limit=101", "limit=", "limit=2&limit=3", "after_id=0", "after_id=-1",
                      "after_id=9223372036854775808", "status=", "status=unknown", "unknown=1", "q=%00", "q=%FF", "q=%0A", "q=" + "a" * 201):
            with self.subTest(query=query):
                self.assertEqual(self.request("GET", "/v1/assets?" + query)[0], 400)
        for query in ("q=hello", "status=available", "limit=0", "after_id=0", "limit=1&limit=2"):
            self.assertEqual(self.request("GET", "/v1/assets/%d/history?%s" % (asset["id"], query))[0], 400)
        for path in ("/v1/assets/0", "/v1/assets/-1", "/v1/assets/1abc", "/v1/assets/9223372036854775808"):
            self.assertEqual(self.request("GET", path)[0], 400)
        self.assertEqual(self.request("GET", "/v1/assets/99999")[0], 404)
        self.assertEqual(self.request("GET", "/v1/assets/99999/history")[0], 404)
        self.assertEqual(self.request("GET", "/v1/assets/%d/unknown" % asset["id"])[0], 404)
        self.assertEqual(self.request("GET", "/v1/assets/%d?limit=1" % asset["id"])[0], 400)
        self.assertEqual(self.request("POST", "/v1/assets?limit=1", {"name": "B", "asset_tag": "A", "actor_id": 1})[0], 400)
        self.assertEqual(self.request("POST", "/v1/assets/%d/history" % asset["id"], {})[0], 405)

    def raw_request(self, payload):
        with socket.create_connection(("127.0.0.1", self.port), timeout=3) as sock:
            with self.context.wrap_socket(sock, server_hostname="localhost") as tls:
                tls.sendall(payload)
                response = b""
                while True:
                    part = tls.recv(65536)
                    if not part:
                        break
                    response += part
        head, body = response.split(b"\r\n\r\n", 1)
        status = int(head.split()[1])
        self.assertFalse(json.loads(body)["ok"])
        return status

    def test_http_framing_rejects_ambiguous_headers(self):
        self.start()
        prefix = b"POST /v1/assets HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + TOKEN.encode() + b"\r\nContent-Type: application/x-www-form-urlencoded\r\n"
        for extra in (b"Content-Length: 0\r\nContent-Length: 0\r\n", b"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
                      b"Content-Length: 0\r\nExpect: 100-continue\r\n", b"Content-Length: 0\r\nHost: duplicate\r\n",
                      b"Content-Length: 0\r\nAuthorization: Bearer duplicate\r\n", b"Content-Length: 0\r\nContent-Type: text/plain\r\n",
                      b"Content-Length: -1\r\n", b"Content-Length: 0x0\r\n", b"Content-Length: 32769\r\n", b"Fold: a\r\n continued\r\n"):
            with self.subTest(extra=extra):
                self.assertEqual(self.raw_request(prefix + extra + b"\r\n"), 400)
        self.assertEqual(self.request("GET", "/v1/status")[1]["summary"]["total"], 0)

    def test_maximum_token_allows_standard_line_endings(self):
        maximum = "b" * 128
        for ending in ("", "\n", "\r\n"):
            with self.subTest(ending=ending):
                self.token_file.write_bytes((maximum + ending).encode("ascii"))
                self.start()
                self.assertEqual(self.request("GET", "/v1/status", token=maximum)[0], 200)
                self.stop()
        self.token_file.write_text(maximum + "b", encoding="ascii")
        invalid = subprocess.run([str(ROOT / "inventory-addon/build/punto-inventory")],
                                 env=self.environment(), capture_output=True, timeout=5)
        self.assertNotEqual(invalid.returncode, 0)

    def test_invalid_secret_and_newer_schema_fail_closed(self):
        self.token_file.chmod(0o666)
        invalid = subprocess.run([str(ROOT / "inventory-addon/build/punto-inventory")],
                                 env=self.environment(), capture_output=True, timeout=5)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertNotIn(TOKEN.encode(), invalid.stderr)
        self.token_file.chmod(0o600)
        link = self.path / "linked-token"
        link.symlink_to(self.token_file)
        invalid = subprocess.run([str(ROOT / "inventory-addon/build/punto-inventory")],
                                 env=self.environment(INVENTORY_API_TOKEN_FILE=str(link)), capture_output=True, timeout=5)
        self.assertNotEqual(invalid.returncode, 0)
        invalid = subprocess.run([str(ROOT / "inventory-addon/build/punto-inventory")],
                                 env=self.environment(INVENTORY_API_TOKEN_FILE=""), capture_output=True, timeout=5)
        self.assertNotEqual(invalid.returncode, 0)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("PRAGMA user_version=6")
        invalid = subprocess.run([str(ROOT / "inventory-addon/build/punto-inventory")],
                                 env=self.environment(), capture_output=True, timeout=5)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn(b"Inventory store unavailable", invalid.stderr)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 6)


if __name__ == "__main__":
    unittest.main()
