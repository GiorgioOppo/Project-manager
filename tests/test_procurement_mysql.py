"""Opt-in contracts/GridFS integration test against a running MySQL deployment.

Set PUNTO_PROCUREMENT_TEST_URL to an HTTP(S) origin and
PUNTO_MYSQL_TEST_EMAIL / PUNTO_MYSQL_TEST_PASSWORD to an existing manager.
Credentials fall back to PUNTO_EMAIL / PUNTO_PASSWORD; this test never reads
.env files. Run ``python3 tests/test_procurement_mysql.py -v``.

Creates and retains uniquely named codex-test-* supplier/contract/job records,
two valid PDFs (including a multi-chunk document), assignments and audit. The
new worker is disabled in a finally block. Existing records are never changed.
Optionally set PUNTO_PROCUREMENT_TEST_MANIFEST to save non-secret fixture IDs
and document digests for a later persistence check. No Docker or direct SQL.
"""

from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import unittest
from urllib.parse import quote, urlencode, urlsplit


TEST_URL = os.environ.get("PUNTO_PROCUREMENT_TEST_URL", "").strip()


def pdf_fixture(padding_bytes=0):
    """Generate a one-page PDF with correct byte offsets, xref and stream length."""
    output = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\x00 binary fixture\n")
    comment = b"% codex-test harmless document padding " + b"x" * 180 + b"\n"
    while padding_bytes > 0:
        output.extend(comment)
        padding_bytes -= len(comment)
    stream = b"BT /F1 12 Tf 72 720 Td (Contract integration fixture) Tj ET\n"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    offsets = [0]
    for number, content in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(str(number).encode("ascii") + b" 0 obj\n" + content + b"\nendobj\n")
    xref = len(output)
    output.extend(b"xref\n0 6\n0000000000 65535 f \n")
    for offset in offsets[1:]:
        output.extend(("%010d 00000 n \n" % offset).encode("ascii"))
    output.extend(b"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + str(xref).encode("ascii") + b"\n%%EOF\n")
    return bytes(output)


@unittest.skipUnless(TEST_URL, "Set PUNTO_PROCUREMENT_TEST_URL to test a running MySQL/GridFS deployment")
class ProcurementMySQLIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.url = urlsplit(TEST_URL)
        if (self.url.scheme not in ("http", "https") or not self.url.hostname or
                self.url.username or self.url.password or self.url.query or self.url.fragment or
                self.url.path not in ("", "/")):
            self.fail("PUNTO_PROCUREMENT_TEST_URL must be an HTTP(S) origin without credentials")
        email = os.environ.get("PUNTO_MYSQL_TEST_EMAIL") or os.environ.get("PUNTO_EMAIL")
        password = os.environ.get("PUNTO_MYSQL_TEST_PASSWORD") or os.environ.get("PUNTO_PASSWORD")
        if not email or not password:
            self.fail("Set PUNTO_MYSQL_TEST_EMAIL/PUNTO_MYSQL_TEST_PASSWORD or PUNTO_EMAIL/PUNTO_PASSWORD")
        self.origin = "%s://%s" % (self.url.scheme, self.url.netloc)
        self.secret_values = [password]
        self.prefix = "codex-test-%s-%s" % (
            datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S"), secrets.token_hex(3))
        self.manifest = {"fixture_prefix": self.prefix, "documents": []}
        health = self.json_request("GET", "/healthz")
        self.assertIs(health.get("ok"), True)
        self.assertEqual(health.get("database"), "mysql", "This test requires a real MySQL backend")
        self.manager = self.login(email, password)
        session = self.json_request("GET", "/api/session", cookie=self.manager)
        self.assertEqual(session.get("role"), "manager")

    def raw_request(self, method, path, body=None, *, cookie=None, headers=None, expected=200):
        request_headers = {"Accept": "application/json"}
        if cookie:
            request_headers["Cookie"] = cookie
        if method in ("POST", "DELETE"):
            request_headers["Origin"] = self.origin
        request_headers.update(headers or {})
        connection_type = http.client.HTTPSConnection if self.url.scheme == "https" else http.client.HTTPConnection
        connection = connection_type(self.url.hostname, self.url.port, timeout=20)
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            payload = response.read()
            status, result_headers = response.status, dict(response.getheaders())
        finally:
            connection.close()
        self.assertTrue(all(secret.encode("utf-8") not in payload for secret in self.secret_values),
                        "A response exposed a credential")
        # Do not emit complete response payloads, which could contain customer data.
        self.assertEqual(status, expected, {"method": method, "path": path, "status": status})
        return payload, result_headers

    def json_request(self, method, path, fields=None, *, cookie=None, expected=200):
        body, headers = None, {}
        if fields is not None:
            body = urlencode(fields)
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        payload, result_headers = self.raw_request(method, path, body, cookie=cookie,
                                                   headers=headers, expected=expected)
        self.assertIn("application/json", result_headers.get("Content-Type", ""))
        try:
            data = json.loads(payload)
        except (UnicodeError, ValueError):
            self.fail("The API did not return valid JSON")
        if expected >= 400:
            self.assertIs(data.get("ok"), False)
            self.assertTrue(data.get("error"))
        if path == "/api/login" and expected == 200:
            self.assertIn("Set-Cookie", result_headers)
            return result_headers["Set-Cookie"].split(";", 1)[0]
        return data

    def login(self, email, password):
        return self.json_request("POST", "/api/login", {"email": email, "password": password})

    def upload(self, contract_id, content, filename, cookie, expected=201):
        path = "/api/contracts/%d/documents?filename=%s" % (contract_id, quote(filename, safe=""))
        payload, _ = self.raw_request("POST", path, content, cookie=cookie,
                                      headers={"Content-Type": "application/pdf"}, expected=expected)
        self.assertTrue(payload.startswith(b"{"))
        return json.loads(payload)

    def save_manifest(self):
        destination = os.environ.get("PUNTO_PROCUREMENT_TEST_MANIFEST")
        if not destination:
            return
        path = Path(destination)
        # Restrict even fixture metadata, and never include manager/worker credentials.
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            os.fchmod(output.fileno(), 0o600)
            json.dump(self.manifest, output, indent=2, ensure_ascii=False)
            output.write("\n")

    def test_mysql_suppliers_contracts_gridfs_pdf_authorization_and_audit(self):
        worker_id = None
        worker_name = self.prefix + " Operatore test"
        try:
            supplier_id = self.json_request("POST", "/api/suppliers", {
                "code": self.prefix + "-S", "name": self.prefix + " Fornitore qualità ✓",
                "email": self.prefix + "@example.test", "notes": "Fixture di integrazione conservata"},
                cookie=self.manager, expected=201)["id"]
            self.manifest["supplier_id"] = supplier_id
            contract_id = self.json_request("POST", "/api/contracts", {
                "code": self.prefix + "-C", "title": self.prefix + " Contratto test", "supplier_id": supplier_id,
                "status": "active", "value_cents": 0, "starts_on": "2026-01-01", "ends_on": "2030-12-31"},
                cookie=self.manager, expected=201)["id"]
            self.manifest["contract_id"] = contract_id
            # Preserve optional values correctly through the MySQL numeric bindings.
            self.json_request("POST", "/api/contracts/%d" % contract_id,
                              {"notes": "Versioni PDF di prova"}, cookie=self.manager)
            contracts = self.json_request("GET", "/api/contracts", cookie=self.manager)["contracts"]
            contract = next(row for row in contracts if row["id"] == contract_id)
            self.assertEqual(contract["value_cents"], 0)
            self.assertEqual(contract["supplier_id"], supplier_id)
            self.assertEqual(contract["documents_count"], 0)
            job_id = self.json_request("POST", "/api/jobs", {
                "code": self.prefix + "-J", "name": self.prefix + " Commessa test", "status": "active",
                "contract_id": contract_id}, cookie=self.manager, expected=201)["id"]
            self.manifest["job_id"] = job_id
            jobs = self.json_request("GET", "/api/jobs", cookie=self.manager)["jobs"]
            job = next(row for row in jobs if row["id"] == job_id)
            self.assertEqual(job["contract_id"], contract_id)
            self.assertEqual(job["contract_code"], contract["code"])
            self.assertEqual(job["contract_title"], contract["title"])
            self.assertEqual(job["supplier_name"], contract["supplier_name"])

            worker_email = self.prefix + "@punto.local"
            worker_password = secrets.token_urlsafe(24)
            self.secret_values.append(worker_password)
            worker_id = self.json_request("POST", "/api/users", {
                "name": worker_name, "email": worker_email, "password": worker_password, "role": "worker"},
                cookie=self.manager, expected=201)["id"]
            self.manifest["worker_id"] = worker_id
            worker = self.login(worker_email, worker_password)
            self.json_request("POST", "/api/jobs/%d/assignments" % job_id,
                              {"user_id": worker_id}, cookie=self.manager, expected=201)
            worker_jobs = self.json_request("GET", "/api/jobs", cookie=worker)["jobs"]
            self.assertEqual([row["id"] for row in worker_jobs], [job_id])
            self.assertEqual(worker_jobs[0]["contract_id"], contract_id)

            contents = [pdf_fixture(), pdf_fixture(1250 * 1024)]
            self.assertGreater(len(contents[1]), 1024 * 1024)
            self.assertIn(b"\x00", contents[0])
            for index, content in enumerate(contents, 1):
                filename = "Contratto qualità versione %d.pdf" % index
                document_id = self.upload(contract_id, content, filename, self.manager)["id"]
                self.manifest["documents"].append({"id": document_id, "file_name": filename,
                                                  "byte_size": len(content),
                                                  "sha256": hashlib.sha256(content).hexdigest()})
                payload, headers = self.raw_request("GET", "/api/contract-documents/%d/download" % document_id,
                                                    cookie=self.manager)
                self.assertEqual(payload, content, "Downloaded PDF differs from uploaded bytes")
                self.assertEqual(headers.get("Content-Type"), "application/pdf")
                self.assertEqual(headers.get("X-Content-Type-Options"), "nosniff")
                self.assertIn("attachment;", headers.get("Content-Disposition", ""))
                self.assertIn("filename*=UTF-8''", headers["Content-Disposition"])
            documents = self.json_request("GET", "/api/contracts/%d/documents" % contract_id,
                                          cookie=self.manager)["documents"]
            self.assertEqual(len(documents), 2)
            for expected in self.manifest["documents"]:
                saved = next(row for row in documents if row["id"] == expected["id"])
                for key in ("file_name", "byte_size", "sha256"):
                    self.assertEqual(saved[key], expected[key])
                self.assertEqual(saved["contract_id"], contract_id)
                self.assertNotIn("storage_key", saved)
            contracts = self.json_request("GET", "/api/contracts", cookie=self.manager)["contracts"]
            self.assertEqual(next(row for row in contracts if row["id"] == contract_id)["documents_count"], 2)

            document_id = self.manifest["documents"][0]["id"]
            for method, path, fields in (
                    ("GET", "/api/suppliers", None),
                    ("POST", "/api/suppliers", {"code": self.prefix + "-D", "name": "Forbidden fixture"}),
                    ("POST", "/api/suppliers/%d" % supplier_id, {"name": "Forbidden fixture"}),
                    ("GET", "/api/contracts", None),
                    ("POST", "/api/contracts", {"code": self.prefix + "-D", "title": "Forbidden fixture", "supplier_id": supplier_id}),
                    ("POST", "/api/contracts/%d" % contract_id, {"title": "Forbidden fixture"}),
                    ("GET", "/api/contracts/%d/documents" % contract_id, None),
                    ("GET", "/api/contract-documents/%d/download" % document_id, None)):
                self.json_request(method, path, fields, cookie=worker, expected=403)
                self.json_request(method, path, fields, expected=401)
            self.upload(contract_id, contents[0], "forbidden.pdf", worker, expected=403)
            self.upload(contract_id, contents[0], "anonymous.pdf", None, expected=401)
            self.assertEqual(len(self.json_request("GET", "/api/contracts/%d/documents" % contract_id,
                                                  cookie=self.manager)["documents"]), 2)

            for entity, expected_id in (("supplier", supplier_id), ("contract", contract_id),
                                       ("contract_document", document_id)):
                events = self.json_request("GET", "/api/audit?" + urlencode({"entity_type": entity, "limit": 100}),
                                           cookie=self.manager)["events"]
                self.assertTrue(all(event["entity_type"] == entity for event in events))
                self.assertTrue(any(event["entity_id"] == expected_id for event in events))
            self.manifest["completed"] = True
        finally:
            try:
                if worker_id is not None:
                    self.json_request("POST", "/api/users/%d" % worker_id, {
                        "name": worker_name, "role": "worker", "active": "0"}, cookie=self.manager)
                    self.manifest["worker_disabled"] = True
            finally:
                self.save_manifest()


if __name__ == "__main__":
    unittest.main()
