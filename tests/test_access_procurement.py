"""Live commercial/Finance segregation, including direct procurement-module guards."""

from contextlib import closing
import ctypes
import hashlib
import json
from pathlib import Path
import shlex
import sqlite3
import subprocess
import sys
import tempfile
import unittest

import test_procurement as procurement
import test_roles as roles


class Response(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("body", ctypes.c_void_p)]


class AccessProcurementTests(unittest.TestCase):
    start_server = procurement.ProcurementIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    raw = procurement.ProcurementIntegrationTests.raw
    upload = procurement.ProcurementIntegrationTests.upload

    @classmethod
    def setUpClass(cls):
        cls.native_build = tempfile.TemporaryDirectory(prefix="punto-procurement-access-build-")
        cls.addClassCleanup(cls.native_build.cleanup)
        helper = Path(cls.native_build.name) / "guards.c"
        helper.write_text('#include <stdbool.h>\n#include <stdlib.h>\n'
                          'bool organization_groups_available(void) { return false; }\n'
                          'void test_free(void *value) { free(value); }\n', encoding="utf-8")
        binary = Path(cls.native_build.name) / "guards.so"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl", "json-c"], text=True))
        command = ["cc", "-shared", "-fPIC", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra",
                   "-Wpedantic", "-Werror", "-I" + str(roles.ROOT / "src")]
        command += [str(roles.ROOT / "src" / name) for name in
                    ("procurement.c", "storage.c", "documents.c", "access_control.c")]
        command += [str(helper), "-o", str(binary), "-lsqlite3", *flags]
        if sys.platform == "darwin":
            command += ["-framework", "Security"]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("Procurement native guard build failed: " + result.stderr)
        cls.native = ctypes.CDLL(str(binary))
        cls.native.procurement_open.argtypes = [ctypes.c_char_p]
        cls.native.procurement_open.restype = ctypes.c_bool
        cls.native.procurement_handle.argtypes = [ctypes.c_char_p] * 4 + [ctypes.c_longlong, ctypes.c_char_p,
                                                                       ctypes.POINTER(Response)]
        cls.native.procurement_handle.restype = ctypes.c_bool
        cls.native.procurement_upload.argtypes = [ctypes.c_longlong, ctypes.c_char_p, ctypes.c_void_p,
                                                  ctypes.c_size_t, ctypes.c_longlong, ctypes.c_char_p,
                                                  ctypes.POINTER(Response)]
        cls.native.procurement_download.argtypes = [ctypes.c_longlong, ctypes.c_longlong, ctypes.c_char_p,
                                                    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t),
                                                    ctypes.c_char_p]
        cls.native.procurement_download.restype = ctypes.c_int
        cls.native.test_free.argtypes = [ctypes.c_void_p]

    def setUp(self):
        roles.RoleIntegrationTests.setUp(self)
        self.manager_id = self.session(self.manager_cookie)["user_id"]
        self.sales_id = self.create_user(email="sales@example.test", name="Commerciale")
        self.sales = self.login("sales@example.test", roles.WORKER_PASSWORD)
        self.finance_id = self.create_user(email="finance@example.test", name="Finance")
        self.finance = self.login("finance@example.test", roles.WORKER_PASSWORD)
        self.worker_id = self.create_user(email="operator@example.test")
        self.worker = self.login("operator@example.test", roles.WORKER_PASSWORD)
        self.groups(self.sales_id, ["sales"])
        self.groups(self.finance_id, ["finance"])
        self.addCleanup(self.native.procurement_close)

    def session(self, cookie):
        status, data, _ = self.api("GET", "/api/session", cookie=cookie)
        self.assertEqual(status, 200, data)
        return data

    def groups(self, user_id, groups):
        status, data, _ = self.api("POST", "/api/users/%d/access-groups" % user_id,
                                   {"groups": json.dumps(groups)}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)

    def post(self, path, fields, cookie=None, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.sales)
        self.assertEqual(status, expected, data)
        return data

    def seed(self):
        supplier = self.post("/api/suppliers", {"code": "PRIVATE-SUP", "name": "Fornitore riservato",
                                               "notes": "Dettagli commerciali personali"})["id"]
        contract = self.post("/api/contracts", {"code": "PRIVATE-CON", "title": "Accordo riservato",
                                               "supplier_id": supplier, "value_cents": 123456,
                                               "notes": "Contenuto commerciale segreto"})["id"]
        return supplier, contract

    def seed_document(self):
        supplier, contract = self.seed()
        content = b"%PDF-1.7\nprivate contract fixture\x00\xff\n%%EOF\n"
        status, payload, _ = self.upload(contract, content, cookie=self.sales)
        self.assertEqual(status, 201, payload)
        return supplier, contract, json.loads(payload)["id"], content

    def denied(self, expected, method, path, fields=None, cookie=None):
        status, data, _ = self.api(method, path, fields, cookie=cookie)
        self.assertEqual(status, expected, data)
        self.assertEqual(set(data), {"ok", "error"})
        self.assertFalse(data["ok"])
        self.assertNotIn("PRIVATE", str(data))
        self.assertNotIn("riservato", str(data))
        return data

    def test_manager_and_operator_profiles_never_imply_commercial_access_or_reveal_ids(self):
        _, contract = self.seed()
        for cookie, expected in ((None, 401), (self.manager_cookie, 403), (self.worker, 403)):
            with self.subTest(cookie=cookie):
                for path in ("/api/suppliers", "/api/contracts", "/api/contracts/%d/documents" % contract,
                             "/api/contracts/999999/documents"):
                    self.denied(expected, "GET", path, cookie=cookie)
                self.denied(expected, "POST", "/api/contracts/999999", {"title": "Tentativo"}, cookie=cookie)
        for cookie in (self.sales, self.finance):
            for resource in ("suppliers", "contracts"):
                status, data, _ = self.api("GET", "/api/" + resource, cookie=cookie)
                self.assertEqual(status, 200, data)
                self.assertEqual(len(data[resource]), 1)
                self.assertNotIn("storage_key", str(data))

    def test_finance_is_read_only_for_contracts_suppliers_and_document_upload(self):
        supplier, contract = self.seed()
        with closing(sqlite3.connect(self.db_path)) as db:
            before = db.execute("SELECT COUNT(*) FROM audit_events").fetchone()
        for path, fields in (("/api/suppliers", {"code": "DENIED", "name": "Denied"}),
                             ("/api/suppliers/%d" % supplier, {"notes": "Changed"}),
                             ("/api/contracts", {"code": "DENIED", "title": "Denied", "supplier_id": supplier}),
                             ("/api/contracts/%d" % contract, {"value_cents": 0})):
            self.denied(403, "POST", path, fields, cookie=self.finance)
        for target in (contract, 999999):
            status, payload, _ = self.upload(target, b"%PDF-denied", cookie=self.finance)
            self.assertEqual(status, 403, payload)
            self.assertNotIn(b"storage_key", payload)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events").fetchone(), before)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM contract_documents").fetchone(), (0,))
            self.assertEqual(db.execute("SELECT value_cents FROM contracts WHERE id=?", (contract,)).fetchone(), (123456,))

    def test_sales_can_edit_and_finance_downloads_identical_pdf_without_storage_metadata(self):
        supplier, contract, document, content = self.seed_document()
        self.post("/api/suppliers/%d" % supplier, {"phone": "123456"}, expected=200)
        self.post("/api/contracts/%d" % contract, {"title": "Accordo aggiornato"}, expected=200)
        for cookie in (self.sales, self.finance):
            status, data, _ = self.api("GET", "/api/contracts/%d/documents" % contract, cookie=cookie)
            self.assertEqual(status, 200, data)
            self.assertEqual(data["documents"][0]["sha256"], hashlib.sha256(content).hexdigest())
            self.assertNotIn("storage_key", str(data))
            status, payload, headers = self.raw("GET", "/api/contract-documents/%d/download" % document, cookie=cookie)
            self.assertEqual(status, 200, payload)
            self.assertEqual(payload, content)
            self.assertEqual(headers["Content-Type"], "application/pdf")
        for cookie in (self.manager_cookie, self.worker):
            for target in (document, 999999):
                self.denied(403, "GET", "/api/contract-documents/%d/download" % target, cookie=cookie)

    def test_profile_revocation_applies_to_existing_sessions_and_pdf_downloads_immediately(self):
        _, contract, document, _ = self.seed_document()
        self.groups(self.sales_id, [])
        for path in ("/api/contracts", "/api/contracts/%d/documents" % contract, "/api/contract-documents/%d/download" % document):
            self.denied(403, "GET", path, cookie=self.sales)
        status, _, _ = self.upload(contract, b"%PDF-denied", cookie=self.sales)
        self.assertEqual(status, 403)
        self.groups(self.finance_id, [])
        self.denied(403, "GET", "/api/contract-documents/%d/download" % document, cookie=self.finance)
        self.groups(self.sales_id, ["sales"])
        status, data, _ = self.api("GET", "/api/contracts", cookie=self.sales)
        self.assertEqual(status, 200, data)
        self.groups(self.manager_id, ["finance"])
        status, data, _ = self.api("GET", "/api/contracts", cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.denied(403, "POST", "/api/contracts/%d" % contract, {"notes": "Denied"}, cookie=self.manager_cookie)

    def native_get(self, actor, claimed_role=b"manager"):
        response = Response()
        handled = self.native.procurement_handle(b"GET", b"/api/contracts", b"", b"", actor,
                                                  claimed_role, ctypes.byref(response))
        self.assertTrue(handled)
        data = json.loads(ctypes.string_at(response.body))
        self.native.test_free(response.body)
        return response.status, data

    def test_module_enforces_live_actor_permissions_and_clears_forbidden_download_outputs(self):
        _, contract, document, _ = self.seed_document()
        self.assertTrue(self.native.procurement_open(str(self.db_path).encode()))
        for actor in (self.manager_id, self.worker_id):
            status, data = self.native_get(actor, b"manager")
            self.assertEqual(status, 403, data)
            response = Response()
            self.native.procurement_upload(contract, b"denied.pdf", b"%PDF-denied", 11, actor,
                                            b"manager", ctypes.byref(response))
            self.assertEqual(response.status, 403)
            self.native.test_free(response.body)
            pointer, size, filename = ctypes.c_void_p(1), ctypes.c_size_t(999), ctypes.create_string_buffer(b"private.pdf", 181)
            status = self.native.procurement_download(document, actor, b"manager", ctypes.byref(pointer),
                                                      ctypes.byref(size), filename)
            self.assertEqual((status, pointer.value, size.value, filename.value), (403, None, 0, b""))
        self.assertEqual(self.native_get(self.sales_id, b"worker")[0], 200)
        self.assertEqual(self.native_get(self.finance_id, b"worker")[0], 200)
        self.groups(self.sales_id, [])
        self.assertEqual(self.native_get(self.sales_id, b"manager")[0], 403)
        self.assertEqual(self.native_get(0)[0], 401)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("UPDATE users SET active=0 WHERE id=?", (self.finance_id,))
            db.commit()
        self.assertEqual(self.native_get(self.finance_id, b"manager")[0], 403)
