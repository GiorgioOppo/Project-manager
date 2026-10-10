"""Supplier/contract workflows and authorization, using isolated native fixtures."""

from contextlib import closing
import hashlib
import http.client
import json
import os
from pathlib import Path
import sqlite3
import unittest
from urllib.parse import quote

import test_roles as roles


class ProcurementIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    def start_server(self, email=roles.MANAGER_EMAIL, password=roles.MANAGER_PASSWORD):
        previous = os.environ.get("DOC_FILES_DIR")
        os.environ["DOC_FILES_DIR"] = str(Path(self.fixture.name).resolve() / "documents")
        try:
            roles.RoleIntegrationTests.start_server(self, email, password)
        finally:
            if previous is None:
                os.environ.pop("DOC_FILES_DIR", None)
            else:
                os.environ["DOC_FILES_DIR"] = previous

    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    create_job = roles.RoleIntegrationTests.create_job
    assign_job = roles.RoleIntegrationTests.assign_job

    def supplier(self, **changes):
        fields = {"code": "SUP-001", "name": 'Fornitore "Qualità" ✓',
                  "vat_number": "IT12345678901", "email": "fornitore@example.test",
                  "phone": "+39 070 12345", "address": "Via Roma 1", "notes": "Prima riga\nSeconda riga"}
        fields.update(changes)
        status, data, _ = self.api("POST", "/api/suppliers", fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        return data["id"]

    def contract(self, supplier_id=None, **changes):
        if supplier_id is None:
            supplier_id = self.supplier()
        fields = {"code": "CON-001", "title": "Assistenza annuale", "supplier_id": supplier_id}
        fields.update(changes)
        status, data, _ = self.api("POST", "/api/contracts", fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        return data["id"]

    def records(self, name):
        status, data, _ = self.api("GET", "/api/" + name, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data[name]

    def raw(self, method, path, body=None, *, cookie=None, headers=None):
        request_headers = {}
        if cookie is not None:
            request_headers["Cookie"] = cookie
        if method == "POST":
            request_headers["Origin"] = self.origin
        request_headers.update(headers or {})
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def upload(self, contract_id, content, filename="contratto.pdf", *, cookie=None, headers=None):
        upload_headers = {"Content-Type": "application/pdf"}
        upload_headers.update(headers or {})
        return self.raw("POST", "/api/contracts/%d/documents?filename=%s" %
                        (contract_id, quote(filename, safe="")), content,
                        cookie=self.manager_cookie if cookie is None else cookie, headers=upload_headers)

    def documents(self, contract_id):
        status, data, _ = self.api("GET", "/api/contracts/%d/documents" % contract_id,
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data["documents"]

    def test_supplier_contract_defaults_edit_preserves_fields_and_restart(self):
        supplier_id = self.supplier()
        contract_id = self.contract(supplier_id, value_cents=123456, status="active",
                                    starts_on="2024-02-29", ends_on="2027-12-31", notes="Dettagli riservati")
        status, data, _ = self.api("POST", "/api/suppliers/%d" % supplier_id,
                                   {"phone": "+39 070 99999"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        supplier = self.records("suppliers")[0]
        self.assertEqual(supplier["name"], 'Fornitore "Qualità" ✓')
        self.assertEqual(supplier["phone"], "+39 070 99999")
        self.assertEqual(supplier["active"], 1)
        self.assertEqual(supplier["notes"], "Prima riga\nSeconda riga")
        status, data, _ = self.api("POST", "/api/contracts/%d" % contract_id,
                                   {"title": "Titolo aggiornato"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        contract = self.records("contracts")[0]
        self.assertEqual(contract["value_cents"], 123456)
        self.assertEqual(contract["currency"], "EUR")
        self.assertEqual(contract["supplier_name"], supplier["name"])
        self.assertEqual(contract["title"], "Titolo aggiornato")
        self.assertEqual(contract["starts_on"], "2024-02-29")
        self.assertEqual(contract["notes"], "Dettagli riservati")
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.records("contracts"), [contract])
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (2,))

    def test_duplicate_codes_case_insensitive_and_invalid_form_fields(self):
        supplier_id = self.supplier()
        self.contract(supplier_id)
        self.assert_error(409, "POST", "/api/suppliers", {"code": "sup-001", "name": "Duplicato"}, cookie=self.manager_cookie)
        self.assert_error(409, "POST", "/api/contracts", {"code": "con-001", "title": "Duplicato", "supplier_id": supplier_id}, cookie=self.manager_cookie)
        for fields in ({"code": "", "name": "Nome"}, {"code": "X", "name": "N" * 121},
                       {"code": "X", "name": "Nome", "email": "not-an-email"},
                       {"code": "X", "name": "Nome", "active": "2"},
                       {"code": "X", "name": "Nome", "unexpected": "field"}):
            with self.subTest(fields=fields):
                self.assert_error(400, "POST", "/api/suppliers", fields, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/suppliers", cookie=self.manager_cookie,
                          body="code=X&code=Y&name=Name", headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assert_error(400, "POST", "/api/suppliers", cookie=self.manager_cookie,
                          body="code=X&name=%FF", headers={"Content-Type": "application/x-www-form-urlencoded"})

    def test_contract_date_status_amount_currency_and_supplier_validation(self):
        supplier_id = self.supplier()
        base = {"code": "INVALID", "title": "Contratto", "supplier_id": supplier_id}
        for change in ({"starts_on": "2023-02-29"}, {"ends_on": "2026-13-01"},
                       {"starts_on": "2027-01-01", "ends_on": "2026-12-31"},
                       {"status": "unknown"}, {"value_cents": "-1"}, {"value_cents": "1.20"},
                       {"value_cents": "1000000000000"}, {"value_cents": "9223372036854775808"},
                       {"currency": "USD"}, {"supplier_id": "0"}, {"supplier_id": "999999"}):
            with self.subTest(change=change):
                self.assert_error(400, "POST", "/api/contracts", dict(base, **change), cookie=self.manager_cookie)
        contract_id = self.contract(supplier_id, value_cents=0)
        row = self.records("contracts")[0]
        self.assertEqual(row["value_cents"], 0)
        self.assertEqual(row["status"], "draft")
        self.assertIsNone(row["starts_on"])
        status, data, _ = self.api("POST", "/api/contracts/%d" % contract_id,
                                   {"value_cents": "", "starts_on": "", "ends_on": ""}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsNone(self.records("contracts")[0]["value_cents"])

    def test_inactive_supplier_blocks_new_links_preserves_existing_contracts(self):
        supplier_id = self.supplier()
        contract_id = self.contract(supplier_id)
        status, data, _ = self.api("POST", "/api/suppliers/%d" % supplier_id,
                                   {"active": "0"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assert_error(400, "POST", "/api/contracts", {
            "code": "CON-002", "title": "Nuovo", "supplier_id": supplier_id}, cookie=self.manager_cookie)
        status, data, _ = self.api("POST", "/api/contracts/%d" % contract_id,
                                   {"notes": "Aggiornamento storico"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.records("contracts")[0]["supplier_id"], supplier_id)
        self.assert_error(405, "DELETE", "/api/suppliers/%d" % supplier_id, cookie=self.manager_cookie)
        self.assert_error(405, "DELETE", "/api/contracts/%d" % contract_id, cookie=self.manager_cookie)

    def test_job_contract_links_optional_preserved_and_closed_contract_rejected(self):
        old_job = self.create_job()
        supplier_id = self.supplier()
        contract_id = self.contract(supplier_id)
        job_fields = {"code": "JOB-LINK", "name": "Commessa collegata", "status": "active", "contract_id": contract_id}
        status, data, _ = self.api("POST", "/api/jobs", job_fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        job_id = data["id"]
        jobs = {row["id"]: row for row in self.records("jobs")}
        self.assertIsNone(jobs[old_job]["contract_id"])
        self.assertEqual(jobs[job_id]["contract_code"], "CON-001")
        self.assertEqual(jobs[job_id]["contract_title"], "Assistenza annuale")
        self.assertEqual(jobs[job_id]["supplier_name"], 'Fornitore "Qualità" ✓')
        status, data, _ = self.api("POST", "/api/contracts/%d" % contract_id,
                                   {"status": "closed"}, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        preserved = {key: value for key, value in job_fields.items() if key != "contract_id"}
        status, data, _ = self.api("POST", "/api/jobs/%d" % job_id, preserved, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual({row["id"]: row for row in self.records("jobs")}[job_id]["contract_id"], contract_id)
        self.assert_error(400, "POST", "/api/jobs", dict(job_fields, code="JOB-NEW"), cookie=self.manager_cookie)
        status, data, _ = self.api("POST", "/api/jobs/%d" % job_id, dict(preserved, contract_id=""), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIsNone({row["id"]: row for row in self.records("jobs")}[job_id]["contract_id"])

    def test_worker_and_unauthenticated_requests_cannot_access_catalog_or_pdfs(self):
        contract_id = self.contract()
        user_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        job_fields = {"code": "SHARED", "name": "Commessa", "status": "active", "contract_id": contract_id}
        status, data, _ = self.api("POST", "/api/jobs", job_fields, cookie=self.manager_cookie)
        self.assertEqual(status, 201, data)
        self.assign_job(data["id"], user_id)
        status, data, _ = self.api("GET", "/api/jobs", cookie=worker)
        self.assertEqual(status, 200, data)
        self.assertEqual(data["jobs"][0]["contract_id"], contract_id)
        status, payload, _ = self.upload(contract_id, b"%PDF-1.7\nprivate")
        self.assertEqual(status, 201, payload)
        document_id = json.loads(payload)["id"]
        for method, path, fields in (
            ("GET", "/api/suppliers", None), ("POST", "/api/suppliers", {"code": "X", "name": "X"}),
            ("POST", "/api/suppliers/1", {"name": "X"}), ("GET", "/api/contracts", None),
            ("POST", "/api/contracts", {"code": "X", "title": "X", "supplier_id": 1}),
            ("POST", "/api/contracts/1", {"title": "X"}),
            ("GET", "/api/contracts/%d/documents" % contract_id, None),
            ("GET", "/api/contract-documents/%d/download" % document_id, None)):
            with self.subTest(method=method, path=path):
                self.assert_error(401, method, path, fields)
                self.assert_error(403, method, path, fields, cookie=worker)
        path = "/api/contracts/%d/documents?filename=private.pdf" % contract_id
        for cookie, expected in ((None, 401), (worker, 403)):
            status, payload, _ = self.raw("POST", path, b"%PDF-1.7\nprivate", cookie=cookie,
                                          headers={"Content-Type": "application/pdf"})
            self.assertEqual(status, expected, payload)

    def test_pdf_binary_roundtrip_sha256_appendonly_history_and_restart(self):
        contract_id = self.contract()
        contents = [b"%PDF-1.7\n\x00\xffbinary\x00data\r\n%%EOF", b"%PDF-1.4\nsecond version\x00\n%%EOF"]
        self.assertEqual(self.records("contracts")[0]["documents_count"], 0)
        document_ids = []
        for index, content in enumerate(contents):
            status, payload, _ = self.upload(contract_id, content, filename="Contratto qualità %d.pdf" % index)
            self.assertEqual(status, 201, payload)
            document_ids.append(json.loads(payload)["id"])
        self.assertEqual(self.records("contracts")[0]["documents_count"], len(contents))
        documents = self.documents(contract_id)
        self.assertEqual([row["id"] for row in documents], list(reversed(document_ids)))
        for document_id, content in zip(document_ids, contents):
            metadata = next(row for row in documents if row["id"] == document_id)
            self.assertEqual(metadata["byte_size"], len(content))
            self.assertEqual(metadata["sha256"], hashlib.sha256(content).hexdigest())
            self.assertNotIn("storage_key", metadata)
            status, payload, headers = self.raw("GET", "/api/contract-documents/%d/download" % document_id,
                                                cookie=self.manager_cookie)
            self.assertEqual(status, 200, payload)
            self.assertEqual(payload, content)
            self.assertEqual(headers["Content-Type"], "application/pdf")
            self.assertIn("attachment;", headers["Content-Disposition"])
            self.assertIn("filename*=UTF-8''", headers["Content-Disposition"])
            self.assertEqual(headers.get("X-Content-Type-Options"), "nosniff")
        with closing(sqlite3.connect(self.db_path)) as db:
            for sql in ("UPDATE contract_documents SET file_name='changed.pdf'", "DELETE FROM contract_documents"):
                with self.assertRaises(sqlite3.IntegrityError):
                    db.execute(sql)
                db.rollback()
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.documents(contract_id), documents)
        status, payload, _ = self.raw("GET", "/api/contract-documents/%d/download" % document_ids[0], cookie=self.manager_cookie)
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload, contents[0])

    def test_pdf_validation_size_content_type_origin_and_filename_paths(self):
        contract_id = self.contract()
        for content, filename in ((b"not-pdf", "file.pdf"), (b"%PDF", "file.pdf"),
                                  (b"%PDF-1.7\n", "../outside.pdf"), (b"%PDF-1.7\n", "a\\b.pdf"),
                                  (b"%PDF-1.7\n", "a\r\nb.pdf"), (b"%PDF-1.7\n", "x" * 181),
                                  (b"%PDF-1.7\n", ".hidden.pdf"), (b"%PDF-1.7\n", 'a"b.pdf'),
                                  (b"%PDF-1.7\n", "contract.txt"), (b"%PDF-1.7\n", "a")):
            with self.subTest(filename=filename, content=content):
                status, payload, _ = self.upload(contract_id, content, filename)
                self.assertEqual(status, 400, payload)
        status, payload, _ = self.upload(contract_id, b"%PDF-1.7\n", headers={"Content-Type": "text/plain"})
        self.assertEqual(status, 415, payload)
        status, payload, _ = self.upload(contract_id, b"%PDF-1.7\n", headers={"Origin": "https://outside.example"})
        self.assertEqual(status, 403, payload)
        status, payload, _ = self.raw("POST", "/api/contracts/%d/documents?filename=large.pdf" % contract_id,
                                     cookie=self.manager_cookie, headers={"Content-Type": "application/pdf",
                                                                         "Content-Length": str(10 * 1024 * 1024 + 1)})
        self.assertEqual(status, 413, payload)
        self.assertEqual(self.documents(contract_id), [])
        status, payload, _ = self.upload(999999, b"%PDF-1.7\n")
        self.assertEqual(status, 404, payload)
        self.assert_error(404, "GET", "/api/contract-documents/999999/download", cookie=self.manager_cookie)

    def test_native_file_tamper_fails_hash_verification_and_does_not_change_metadata(self):
        contract_id = self.contract()
        content = b"%PDF-1.7\noriginal-content"
        status, payload, _ = self.upload(contract_id, content)
        self.assertEqual(status, 201, payload)
        document_id = json.loads(payload)["id"]
        before = self.documents(contract_id)
        with closing(sqlite3.connect(self.db_path)) as db:
            key = db.execute("SELECT storage_key FROM contract_documents WHERE id=?", (document_id,)).fetchone()[0]
        path = Path(self.fixture.name) / "documents" / key
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        path.write_bytes(b"%PDF-1.7\ntampered-content")
        status, payload, _ = self.raw("GET", "/api/contract-documents/%d/download" % document_id,
                                     cookie=self.manager_cookie)
        self.assertEqual(status, 503, payload)
        self.assertEqual(self.documents(contract_id), before)
        path.write_bytes(content)
        status, payload, _ = self.raw("GET", "/api/contract-documents/%d/download" % document_id,
                                     cookie=self.manager_cookie)
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload, content)

    def test_audit_is_atomic_and_contains_supplier_contract_document_events(self):
        contract_id = self.contract()
        status, payload, _ = self.upload(contract_id, b"%PDF-1.7\naudit")
        self.assertEqual(status, 201, payload)
        for entity in ("supplier", "contract", "contract_document"):
            status, data, _ = self.api("GET", "/api/audit?entity_type=" + entity, cookie=self.manager_cookie)
            self.assertEqual(status, 200, data)
            self.assertTrue(data["events"])
            self.assertTrue(all(event["entity_type"] == entity for event in data["events"]))
        with closing(sqlite3.connect(self.db_path)) as db:
            rows = db.execute("SELECT action,entity_type,after_json FROM audit_events WHERE action IN "
                              "('suppliers.create','contracts.create','contract_documents.create') ORDER BY id").fetchall()
            self.assertEqual([row[0] for row in rows], ["suppliers.create", "contracts.create", "contract_documents.create"])
            for _, _, after in rows:
                self.assertIsInstance(json.loads(after), dict)
            db.execute("CREATE TRIGGER deny_procurement_audit BEFORE INSERT ON audit_events "
                       "WHEN NEW.entity_type='supplier' BEGIN SELECT RAISE(ABORT,'test rollback'); END")
            db.commit()
        self.assert_error(500, "POST", "/api/suppliers", {"code": "ROLLBACK", "name": "Non salvato"}, cookie=self.manager_cookie)
        self.assertEqual([row["code"] for row in self.records("suppliers")], ["SUP-001"])


if __name__ == "__main__":
    unittest.main()
