"""Ticket/Punto linking with synthetic grants, isolated stores, and loopback fixtures."""

import http.server
import json
import os
from pathlib import Path
import secrets
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlencode, urlsplit

import test_roles as roles
import test_tickets as tickets
from test_mail_client import create_certificate


PREFIX = "/api/integrations/tickets/"


class FakePunto:
    """A read-only source whose live permissions and catalog can change independently."""

    def __init__(self, certificate=None, key=None):
        self.requests = []
        self.grants = {}
        self.failures = {}
        self.jobs = {
            21: {"id": 21, "code": "JOB-CAFFE", "name": "Commessa caffè", "status": "active"},
            22: {"id": 22, "code": "JOB-PRIVATE", "name": "Commessa riservata", "status": "paused"},
        }
        self.assets = {
            101: {"id": 101, "asset_tag": "IT-CAFFE", "name": "Portatile qualità", "status": "available", "asset_type": "it_device"},
            102: {"id": 102, "asset_tag": "NET-02", "name": "Switch ufficio", "status": "maintenance", "asset_type": "network_device"},
        }
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def reply(self, status, value, headers=None):
                body = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                for name, content in (headers or {}).items():
                    self.send_header(name, content)
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (ConnectionError, ssl.SSLError):
                    pass  # Bounded-response checks deliberately close the connection.

            def do_GET(self):
                parsed = urlsplit(self.path)
                resource = parsed.path.removeprefix(PREFIX)
                authorization = self.headers.get("Authorization", "")
                query = parse_qs(parsed.query, keep_blank_values=True)
                fixture.requests.append({"resource": resource, "path": self.path,
                                         "authorization": authorization, "query": query,
                                         "cookie": self.headers.get("Cookie")})
                scope = fixture.grants.get(authorization.removeprefix("Bearer ")) if authorization.startswith("Bearer ") else None
                if not parsed.path.startswith(PREFIX) or scope is None:
                    return self.reply(401, {"ok": False, "error": "Grant non valido"})
                if resource in fixture.failures:
                    return self.reply(*fixture.failures[resource])
                if scope["expires_at"] <= int(time.time()):
                    return self.reply(401, {"ok": False, "error": "Grant scaduto"})
                if resource == "scope":
                    return self.reply(200, scope)
                if resource == "jobs":
                    if not set(query) <= {"search", "limit", "offset"} or any(len(value) != 1 for value in query.values()):
                        return self.reply(400, {"ok": False, "error": "Parametri non validi"})
                    search = query.get("search", [""])[0].casefold()
                    rows = [row for row in fixture.jobs.values() if (scope["all_jobs"] or row["id"] in scope["job_ids"])
                            and (search in row["code"].casefold() or search in row["name"].casefold())]
                    rows.sort(key=lambda row: (row["code"], row["id"]))
                    try:
                        limit, offset = int(query.get("limit", ["30"])[0]), int(query.get("offset", ["0"])[0])
                    except ValueError:
                        return self.reply(400, {"ok": False, "error": "Paginazione non valida"})
                    if not 1 <= limit <= 100 or offset < 0:
                        return self.reply(400, {"ok": False, "error": "Paginazione non valida"})
                    return self.reply(200, {"items": rows[offset:offset + limit], "total": len(rows)})
                if resource == "assets":
                    if not set(query) <= {"q", "limit", "after_id"} or any(len(value) != 1 for value in query.values()):
                        return self.reply(400, {"ok": False, "error": "Parametri non validi"})
                    try:
                        limit, after = int(query.get("limit", ["30"])[0]), int(query.get("after_id", ["9223372036854775807"])[0])
                    except ValueError:
                        return self.reply(400, {"ok": False, "error": "Paginazione non valida"})
                    if not 1 <= limit <= 100 or after < 1:
                        return self.reply(400, {"ok": False, "error": "Paginazione non valida"})
                    search = query.get("q", [""])[0].casefold()
                    rows = [row for row in fixture.assets.values() if row["id"] < after
                            and (search in row["asset_tag"].casefold() or search in row["name"].casefold())]
                    rows.sort(key=lambda row: row["id"], reverse=True)
                    selected = rows[:limit]
                    return self.reply(200, {"items": selected, "next_after_id": selected[-1]["id"] if len(rows) > limit else None})
                if resource == "validate":
                    result = {"job": None, "asset": None}
                    if "job_id" in query:
                        identifier = int(query["job_id"][0])
                        if identifier not in fixture.jobs:
                            return self.reply(404, {"ok": False, "error": "Commessa inesistente"})
                        if not scope["all_jobs"] and identifier not in scope["job_ids"]:
                            return self.reply(403, {"ok": False, "error": "Commessa non autorizzata"})
                        result["job"] = {key: fixture.jobs[identifier][key] for key in ("id", "code", "name")}
                    if "asset_id" in query:
                        identifier = int(query["asset_id"][0])
                        if identifier not in fixture.assets:
                            return self.reply(404, {"ok": False, "error": "Asset inesistente"})
                        result["asset"] = {key: fixture.assets[identifier][key] for key in ("id", "asset_tag", "name")}
                    return self.reply(200, result)
                return self.reply(404, {"ok": False, "error": "Risorsa inesistente"})

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        if certificate is not None:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(certificate), str(key))
            self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.url = f"{'https' if certificate else 'http'}://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def issue(self, state, *, all_jobs=False, job_ids=(21,)):
        grant = secrets.token_hex(32)
        self.grants[grant] = {"state": state, "user": {"id": 700, "name": "Collaboratore Punto"},
                              "all_jobs": all_jobs, "job_ids": list(job_ids), "expires_at": int(time.time()) + 3600}
        return grant

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class TicketPuntoIntegrationTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    create_ticket = tickets.TicketApplicationTests.create_ticket
    update_ticket = tickets.TicketApplicationTests.update_ticket
    create_user = tickets.TicketApplicationTests.create_user
    stop = tickets.TicketApplicationTests.stop

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make", "-C", str(tickets.APP)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.binary = tickets.APP / "build" / "punto-tickets"

    def setUp(self):
        self.punto = FakePunto()
        self.addCleanup(self.punto.close)
        self.punto_enabled = True
        tickets.TicketApplicationTests.setUp(self)

    def start(self):
        for name in tuple(self.environment):
            if name.startswith("TICKETS_PUNTO_"):
                self.environment.pop(name)
        if self.punto_enabled:
            self.environment["TICKETS_PUNTO_ORIGIN"] = self.punto.url
        tickets.TicketApplicationTests.start(self)

    def connect(self, session, **scope):
        pending, _headers = self.api("POST", "/api/integrations/punto/start", {}, session)
        self.assertRegex(pending["state"], r"\A[0-9a-f]{64}\Z")
        self.assertEqual(pending["url"], self.punto.url + "/#ticket-connect?state=" + pending["state"])
        grant = self.punto.issue(pending["state"], **scope)
        connected, _headers = self.api("POST", "/api/integrations/punto/connect",
                                       {"state": pending["state"], "grant": grant}, session)
        self.assertIs(connected["connected"], True)
        self.assertEqual(connected["user_name"], "Collaboratore Punto")
        self.assertNotIn(grant, json.dumps(connected))
        return grant

    def test_connect_requires_csrf_and_pending_state_belongs_to_exact_ticket_session(self):
        first = self.login()
        second = self.login()
        self.api("POST", "/api/integrations/punto/start", {}, first, expected=403,
                 headers={"X-CSRF-Token": None})
        one, _headers = self.api("POST", "/api/integrations/punto/start", {}, first)
        two, _headers = self.api("POST", "/api/integrations/punto/start", {}, second)
        self.assertNotEqual(one["state"], two["state"])
        grant = self.punto.issue(one["state"])
        self.api("POST", "/api/integrations/punto/connect", {"state": one["state"], "grant": grant}, second, expected=400)
        self.api("POST", "/api/integrations/punto/connect", {"state": two["state"], "grant": grant}, second, expected=403)
        self.punto.grants[grant]["user"]["name"] = "è" * 120
        body, _headers = self.api("POST", "/api/integrations/punto/connect", {"state": one["state"], "grant": grant}, first)
        self.assertIs(body["connected"], True)
        self.assertEqual(body["user_name"], "è" * 120)
        status, _headers = self.api("GET", "/api/integrations/punto/status", session=second)
        self.assertIs(status["connected"], False)
        self.api("POST", "/api/integrations/punto/disconnect", {}, first)
        self.api("POST", "/api/integrations/punto/connect", {"state": one["state"], "grant": grant}, first, expected=400)

    def test_disabled_integration_keeps_ordinary_tickets_available(self):
        self.stop()
        self.punto_enabled = False
        self.start()
        admin = self.login()
        status, _headers = self.api("GET", "/api/integrations/punto/status", session=admin)
        self.assertIs(status["configured"], False)
        self.assertIs(status["connected"], False)
        self.api("POST", "/api/integrations/punto/start", {}, admin, expected=503)
        self.create_ticket(admin, title="App autonoma")
        self.api("POST", "/api/tickets", {"title": "Collegamento rifiutato", "job_id": 21}, admin, expected=403)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 1)

    def test_catalog_and_validation_return_canonical_read_only_records(self):
        admin = self.login()
        self.connect(admin)
        jobs, _headers = self.api("GET", "/api/integrations/punto/jobs?" + urlencode({"search": "caffè", "limit": 1, "offset": 0}), session=admin)
        self.assertEqual(jobs, {"items": [self.punto.jobs[21]], "total": 1})
        assets, _headers = self.api("GET", "/api/integrations/punto/assets?limit=1", session=admin)
        self.assertEqual([row["id"] for row in assets["items"]], [102])
        self.assertEqual(assets["next_after_id"], 102)
        next_page, _headers = self.api("GET", "/api/integrations/punto/assets?limit=1&after_id=102", session=admin)
        self.assertEqual([row["id"] for row in next_page["items"]], [101])
        value, _headers = self.api("GET", "/api/integrations/punto/validate?job_id=21&asset_id=101", session=admin)
        self.assertEqual(value, {"job": {key: self.punto.jobs[21][key] for key in ("id", "code", "name")},
                                 "asset": {key: self.punto.assets[101][key] for key in ("id", "asset_tag", "name")}})
        for path in ("jobs?limit=0", "jobs?limit=1&limit=2", "jobs?user_id=999", "assets?actor_id=999",
                     "validate?job_id=0", "validate?job_id=21&job_id=22", "validate?provider_url=https://foreign.invalid"):
            with self.subTest(path=path):
                self.api("GET", "/api/integrations/punto/" + path, session=admin, expected=400)
        self.assertTrue(all(row["authorization"].startswith("Bearer ") for row in self.punto.requests))

    def test_links_use_server_snapshots_support_partial_updates_and_audit_changes(self):
        admin = self.login()
        self.connect(admin, all_jobs=True)
        ticket = self.create_ticket(admin, title="Ticket collegato", job_id=21, asset_id=101)
        self.assertEqual((ticket["job_id"], ticket["job_code"], ticket["job_name"]), (21, "JOB-CAFFE", "Commessa caffè"))
        self.assertEqual((ticket["asset_id"], ticket["asset_tag"], ticket["asset_name"]), (101, "IT-CAFFE", "Portatile qualità"))
        original = ticket.copy()
        self.punto.jobs[21]["name"] = "Nome aggiornato nella sorgente"
        self.punto.assets[101]["name"] = "Bene rinominato nella sorgente"
        ticket = self.update_ticket(admin, ticket, title="Titolo aggiornato", job_id=21, asset_id=101)
        self.assertEqual(ticket["job_name"], original["job_name"])
        self.assertEqual(ticket["asset_name"], original["asset_name"])
        ticket = self.update_ticket(admin, ticket, job_id=22, asset_id=102)
        self.assertEqual((ticket["job_code"], ticket["asset_tag"]), ("JOB-PRIVATE", "NET-02"))
        detail, _headers = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)
        text = " ".join(row["detail"] for row in detail["history"])
        self.assertIn("commessa", text.lower())
        self.assertIn("asset", text.lower())
        ticket = self.update_ticket(admin, ticket, job_id=None, asset_id=None)
        for key in ("job_id", "job_code", "job_name", "asset_id", "asset_tag", "asset_name"):
            self.assertIsNone(ticket[key], key)

    def test_reference_validation_rejects_forgery_unknown_and_forbidden_ids_without_writes(self):
        admin = self.login()
        self.connect(admin)
        for fields in ({"job_id": 22}, {"job_id": 999}, {"asset_id": 999}):
            with self.subTest(fields=fields):
                self.api("POST", "/api/tickets", {"title": "Rifiutato", **fields}, admin, expected=404)
        for fields in ({"job_id": "21"}, {"job_id": True}, {"job_id": 0}, {"asset_id": -1},
                       {"asset_id": "101"}, {"job_id": 21, "job_name": "Nome inventato"},
                       {"asset_id": 101, "asset_tag": "Etichetta inventata"}):
            with self.subTest(fields=fields):
                self.api("POST", "/api/tickets", {"title": "Rifiutato", **fields}, admin, expected=400)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 0)

    def test_malformed_canonical_references_never_create_or_change_tickets(self):
        admin = self.login()
        self.connect(admin, all_jobs=True)
        existing = self.create_ticket(admin, title="Archivio da preservare")
        for value in ({"job": {"id": 22, "code": "WRONG", "name": "Altro"}, "asset": None},
                      {"job": {"id": 21, "code": "JOB-CAFFE"}, "asset": None},
                      {"job": {"id": True, "code": "JOB-CAFFE", "name": "Commessa"}, "asset": None},
                      {"job": None, "asset": {"id": 102, "asset_tag": "WRONG", "name": "Altro"}}):
            with self.subTest(value=value):
                fields = {"asset_id": 101} if value["asset"] is not None else {"job_id": 21}
                self.punto.failures["validate"] = (200, value)
                self.api("POST", "/api/tickets", {"title": "Non salvato", **fields}, admin, expected=503)
                self.api("PUT", f"/api/tickets/{existing['id']}",
                         {"revision": existing["revision"], **fields}, admin, expected=503)
        detail, _headers = self.api("GET", f"/api/tickets/{existing['id']}", session=admin)
        self.assertEqual(detail["ticket"], existing)
        self.assertEqual(len(detail["history"]), 1)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 1)

    def test_live_job_scope_filters_list_detail_mutations_and_every_counter(self):
        admin = self.login()
        grant = self.connect(admin)
        unlinked = self.create_ticket(admin, title="Visibile senza collegamento")
        linked = self.create_ticket(admin, title="Commessa consentita", job_id=21, assignee_id=admin["user"]["id"])
        user = self.create_user(admin)
        operator = self.login(user["email"])
        self.connect(operator, all_jobs=True)
        hidden = self.create_ticket(operator, title="Private hidden phrase", job_id=22)
        hidden = self.update_ticket(operator, hidden, status="resolved")
        body, _headers = self.api("GET", "/api/tickets?today=2032-02-29", session=admin)
        self.assertEqual({row["id"] for row in body["items"]}, {unlinked["id"], linked["id"]})
        self.assertEqual(body["total"], 2)
        self.assertEqual(body["stats"], {"open": 2, "in_progress": 0, "waiting": 0, "resolved": 0, "closed": 0})
        self.assertEqual(body["queues"]["active"], 2)
        self.assertEqual(body["queues"]["mine"], 1)
        filtered, _headers = self.api("GET", "/api/tickets?search=Private+hidden+phrase", session=admin)
        self.assertEqual(filtered["total"], 0)
        self.punto.grants[grant]["job_ids"] = []
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual([row["id"] for row in body["items"]], [unlinked["id"]])
        self.assertEqual(body["stats"]["open"], 1)
        self.assertEqual(body["queues"]["active"], 1)
        for method, payload, suffix in (("GET", None, ""), ("PUT", {"revision": linked["revision"], "title": "Rifiutato"}, ""),
                                         ("POST", {"body": "Rifiutato"}, "/comments")):
            with self.subTest(method=method):
                self.api(method, f"/api/tickets/{linked['id']}{suffix}", payload, admin, expected=404)
        self.api("POST", "/api/integrations/punto/disconnect", {}, operator)
        disconnected, _headers = self.api("GET", "/api/tickets", session=operator)
        self.assertEqual([row["id"] for row in disconnected["items"]], [unlinked["id"]])

    def test_asset_only_tickets_require_connection_but_not_a_job_grant(self):
        admin = self.login()
        self.connect(admin, job_ids=())
        linked = self.create_ticket(admin, title="Ticket sul bene", asset_id=101)
        self.assertIsNone(linked["job_id"])
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin)
        self.api("POST", "/api/integrations/punto/disconnect", {}, admin)
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin, expected=404)
        self.connect(admin, job_ids=())
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin)

    def test_catalog_outage_preserves_old_links_and_does_not_block_unlinked_tickets(self):
        admin = self.login()
        self.connect(admin, all_jobs=True)
        linked = self.create_ticket(admin, job_id=21, asset_id=101)
        unlinked = self.create_ticket(admin, title="Lavoro autonomo")
        self.punto.failures["validate"] = (503, {"ok": False, "error": "Catalogo non disponibile"})
        linked = self.update_ticket(admin, linked, title="Riferimenti storici conservati", job_id=21, asset_id=101)
        self.api("PUT", f"/api/tickets/{linked['id']}", {"revision": linked["revision"], "job_id": 22}, admin, expected=503)
        current, _headers = self.api("GET", f"/api/tickets/{linked['id']}", session=admin)
        self.assertEqual(current["ticket"], linked)
        self.punto.failures["scope"] = (503, {"ok": False, "error": "Sorgente non disponibile"})
        self.api("GET", "/api/tickets", session=admin, expected=503)
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin, expected=503)
        self.api("POST", f"/api/tickets/{linked['id']}/comments", {"body": "Non salvato"}, admin, expected=503)
        self.api("GET", f"/api/tickets/{unlinked['id']}", session=admin)
        unlinked = self.update_ticket(admin, unlinked, title="Modifica locale ancora disponibile")
        self.api("POST", f"/api/tickets/{unlinked['id']}/comments", {"body": "Commento locale"}, admin, expected=201)
        self.assertEqual(unlinked["title"], "Modifica locale ancora disponibile")

    def test_revoked_grant_loses_link_visibility_without_leaking_snapshots(self):
        admin = self.login()
        grant = self.connect(admin)
        linked = self.create_ticket(admin, title="Documento riservato", job_id=21)
        self.create_ticket(admin, title="Pubblico nel team")
        self.punto.grants.pop(grant)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 1)
        self.assertNotIn("Documento riservato", json.dumps(body))
        self.assertNotIn("JOB-CAFFE", json.dumps(body))
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin, expected=404)
        status, _headers = self.api("GET", "/api/integrations/punto/status", session=admin)
        self.assertIs(status["connected"], False)

    def test_expired_source_grant_clears_connection_and_live_access(self):
        admin = self.login()
        grant = self.connect(admin)
        linked = self.create_ticket(admin, title="Riservato alla commessa", job_id=21)
        local = self.create_ticket(admin, title="Lavoro locale")
        self.punto.grants[grant]["expires_at"] = int(time.time()) - 1
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual([item["id"] for item in body["items"]], [local["id"]])
        self.api("GET", f"/api/tickets/{linked['id']}", session=admin, expected=404)
        status, _headers = self.api("GET", "/api/integrations/punto/status", session=admin)
        self.assertIs(status["connected"], False)

    def test_link_filters_search_and_restart_preserve_canonical_snapshots(self):
        admin = self.login()
        self.connect(admin)
        linked = self.create_ticket(admin, title="Assistenza", job_id=21, asset_id=101)
        self.create_ticket(admin, title="Generico")
        with sqlite3.connect(self.environment["TICKETS_DB_PATH"]) as database:
            self.assertEqual(database.execute("SELECT name FROM sqlite_master WHERE name IN ('sessions','punto_connections','punto_visible_jobs','punto_access')").fetchall(), [],
                             "Sessions, source grants, and live permissions must remain in memory")
        for query in ("job_id=21", "asset_id=101", "search=JOB-CAFFE", "search=IT-CAFFE", "search=" + urlencode({"v": "Portatile qualità"})[2:]):
            with self.subTest(query=query):
                body, _headers = self.api("GET", "/api/tickets?" + query, session=admin)
                self.assertEqual([row["id"] for row in body["items"]], [linked["id"]])
        self.stop()
        self.start()
        admin = self.login()
        disconnected, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(disconnected["total"], 1)
        self.punto.jobs[21]["name"] = "Commessa rinominata"
        self.connect(admin)
        detail, _headers = self.api("GET", f"/api/tickets/{linked['id']}", session=admin)
        self.assertEqual(detail["ticket"], linked)


class TicketConnectorSecurityTests(unittest.TestCase):
    request = tickets.TicketApplicationTests.request
    api = tickets.TicketApplicationTests.api
    login = tickets.TicketApplicationTests.login
    stop = tickets.TicketApplicationTests.stop
    connect = TicketPuntoIntegrationTests.connect

    @classmethod
    def setUpClass(cls):
        tickets.TicketApplicationTests.setUpClass.__func__(cls)
        cls.certificates = tempfile.TemporaryDirectory(prefix="ticket-punto-tls-")
        cls.addClassCleanup(cls.certificates.cleanup)
        cls.cert, cls.key = create_certificate(cls.certificates.name, "punto-source")
        cls.other_ca, _key = create_certificate(cls.certificates.name, "punto-untrusted")
        cls.wrong_cert, cls.wrong_key = create_certificate(cls.certificates.name, "punto-wrong-host", "wrong-source.invalid")

    def setUp(self):
        self.punto = FakePunto(self.cert, self.key)
        self.addCleanup(self.punto.close)
        self.punto.url = f"https://localhost:{self.punto.server.server_port}"
        self.connector_environment = {"TICKETS_PUNTO_ORIGIN": self.punto.url,
                                      "TICKETS_PUNTO_CA_FILE": str(self.cert),
                                      "TICKETS_PUNTO_RESOLVE_IP": "127.0.0.1"}
        tickets.TicketApplicationTests.setUp(self)

    def start(self):
        for name in tuple(self.environment):
            if name.startswith("TICKETS_PUNTO_"):
                self.environment.pop(name)
        self.environment.update(self.connector_environment)
        tickets.TicketApplicationTests.start(self)

    def test_connector_configuration_rejects_unsafe_origins_before_database_creation(self):
        self.stop()
        cases = ({"TICKETS_PUNTO_ORIGIN": "http://punto.example.test"},
                 {"TICKETS_PUNTO_ORIGIN": "https://punto.example.test/path"},
                 {"TICKETS_PUNTO_ORIGIN": "https://user:password@punto.example.test"},
                 {"TICKETS_PUNTO_ORIGIN": "https://punto.example.test?redirect=other"},
                 {"TICKETS_PUNTO_ORIGIN": "https://punto.example.test/#fragment"},
                 {"TICKETS_PUNTO_ORIGIN": "http://127.0.0.1:0"},
                 {"TICKETS_PUNTO_CA_FILE": str(self.cert)},
                 {"TICKETS_PUNTO_RESOLVE_IP": "127.0.0.1"},
                 {"TICKETS_PUNTO_ORIGIN": "http://localhost", "TICKETS_PUNTO_RESOLVE_IP": "192.0.2.1"})
        for index, config in enumerate(cases):
            with self.subTest(config=config):
                database = Path(self.temporary.name) / f"invalid-origin-{index}.sqlite"
                environment = {key: value for key, value in self.environment.items()
                               if not key.startswith("TICKETS_PUNTO_")}
                environment.update(config, TICKETS_DB_PATH=str(database))
                result = subprocess.run([str(self.binary)], cwd=tickets.APP, env=environment,
                                        capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(database.exists(), "Invalid connector configuration must fail before opening storage")

    def test_verified_tls_succeeds_with_resolve_ip_and_ignores_proxy_environment(self):
        self.stop()
        self.environment.update({"HTTPS_PROXY": "http://127.0.0.1:1", "https_proxy": "http://127.0.0.1:1",
                                 "ALL_PROXY": "http://127.0.0.1:1", "NO_PROXY": "", "no_proxy": ""})
        self.start()
        admin = self.login()
        grant = self.connect(admin)
        body, _headers = self.api("GET", "/api/integrations/punto/jobs", session=admin)
        self.assertEqual(body["total"], 1)
        self.assertTrue(self.punto.requests)
        self.assertTrue(all(item["authorization"] == "Bearer " + grant for item in self.punto.requests))
        self.assertTrue(all(item["cookie"] is None for item in self.punto.requests))

    def test_untrusted_ca_and_wrong_hostname_fail_before_sending_grants(self):
        wrong = FakePunto(self.wrong_cert, self.wrong_key)
        self.addCleanup(wrong.close)
        wrong.url = f"https://localhost:{wrong.server.server_port}"
        for source, ca in ((self.punto, self.other_ca), (wrong, self.wrong_cert)):
            with self.subTest(origin=source.url, ca=ca.name):
                self.stop()
                self.connector_environment.update(TICKETS_PUNTO_ORIGIN=source.url, TICKETS_PUNTO_CA_FILE=str(ca))
                self.start()
                admin = self.login()
                pending, _headers = self.api("POST", "/api/integrations/punto/start", {}, admin)
                grant = source.issue(pending["state"])
                self.api("POST", "/api/integrations/punto/connect",
                         {"state": pending["state"], "grant": grant}, admin, expected=503)
                self.assertEqual(source.requests, [], "TLS must be verified before the bearer grant is sent")

    def test_redirects_and_bounded_or_malformed_json_fail_closed(self):
        admin = self.login()
        self.connect(admin)
        target = FakePunto(self.cert, self.key)
        self.addCleanup(target.close)
        self.punto.failures["scope"] = (302, {"ok": True}, {"Location": target.url + PREFIX + "scope"})
        self.api("GET", "/api/tickets", session=admin, expected=503)
        self.assertEqual(target.requests, [], "Redirects must never forward the source grant")
        for body in (b"not-json", b'{"valid":true} trailing', b'{"state":"\xff"}', b'{"state":"a\x00b"}',
                     b'{"state":"' + b"x" * (512 * 1024) + b'"}'):
            with self.subTest(length=len(body), prefix=body[:24]):
                self.punto.failures["scope"] = (200, body)
                self.api("GET", "/api/tickets", session=admin, expected=503)
        self.punto.failures.pop("scope")
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 0)


class MainTicketBridgeTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    assign_job = roles.RoleIntegrationTests.assign_job
    edit_user = roles.RoleIntegrationTests.edit_user
    assert_error = roles.RoleIntegrationTests.assert_error

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make"], cwd=roles.ROOT, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def setUp(self):
        self.ticket_origin = "http://127.0.0.1:19837"
        environment = {name: value for name, value in os.environ.items()
                       if not name.startswith(("TICKETS_", "INVENTORY_", "CALENDAR_", "MAIL_"))}
        environment["AUTH_MODE"] = "local"
        environment["TICKETS_APP_ORIGIN"] = self.ticket_origin
        with patch.dict(os.environ, environment, clear=True):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)

    def create_job(self, code, *, common=False):
        status, value, _headers = self.api("POST", "/api/jobs", {
            "code": code, "name": "Commessa " + code, "status": "active", "is_common": "1" if common else "0",
        }, cookie=self.manager_cookie)
        self.assertEqual(status, 201, value)
        return value["id"]

    def grant(self, cookie, state="a" * 64):
        status, value, _headers = self.api("POST", PREFIX + "grant", {"state": state}, cookie=cookie)
        self.assertEqual(status, 200, value)
        self.assertTrue(value["url"].startswith(self.ticket_origin + "/#punto_grant="))
        fragment = parse_qs(urlsplit(value["url"]).fragment)
        self.assertEqual(fragment["state"], [state])
        token = fragment["punto_grant"][0]
        self.assertRegex(token, r"\A[0-9a-f]{64}\Z")
        self.assertNotIn(cookie, value["url"])
        return token

    def read(self, resource, grant, expected=200):
        status, value, _headers = self.api("GET", PREFIX + resource, headers={"Authorization": "Bearer " + grant})
        self.assertEqual(status, expected, value)
        return value

    def test_grants_require_live_session_origin_and_strict_state(self):
        self.assert_error(401, "POST", PREFIX + "grant", {"state": "a" * 64})
        self.assert_error(403, "POST", PREFIX + "grant", {"state": "a" * 64}, cookie=self.worker_cookie,
                          headers={"Origin": "https://foreign.invalid"})
        for fields in ({"state": "A" * 64}, {"state": "a" * 63}, {"state": "g" * 64},
                       {"state": "a" * 64, "redirect_uri": "https://foreign.invalid"}, {"state": "a" * 64, "user_id": "1"}):
            with self.subTest(fields=fields):
                self.assert_error(400, "POST", PREFIX + "grant", fields, cookie=self.worker_cookie)
        grant = self.grant(self.worker_cookie)
        scope = self.read("scope", grant)
        self.assertEqual(scope["state"], "a" * 64)
        self.assertEqual(scope["user"]["id"], self.worker_id)
        self.assertIs(scope["all_jobs"], False)
        self.assertGreater(scope["expires_at"], int(time.time()))
        self.assert_error(401, "GET", PREFIX + "scope", cookie=self.worker_cookie)
        self.read("scope", "f" * 64, expected=401)

    def test_live_job_assignments_control_scope_catalog_and_validation(self):
        granted = self.create_job("ALLOWED")
        denied = self.create_job("DENIED")
        common = self.create_job("COMMON", common=True)
        self.assign_job(granted, self.worker_id)
        grant = self.grant(self.worker_cookie)
        scope = self.read("scope", grant)
        self.assertEqual(set(scope["job_ids"]), {granted, common})
        catalog = self.read("jobs?limit=1&offset=0", grant)
        second = self.read("jobs?limit=1&offset=1", grant)
        self.assertEqual(catalog["total"], 2)
        self.assertEqual({catalog["items"][0]["id"], second["items"][0]["id"]}, {granted, common})
        self.assertEqual(self.read(f"validate?job_id={granted}", grant)["job"]["id"], granted)
        self.read(f"validate?job_id={denied}", grant, expected=403)
        self.read("validate?job_id=999999", grant, expected=404)
        status, value, _headers = self.api("DELETE", f"/api/jobs/{granted}/assignments/{self.worker_id}", cookie=self.manager_cookie)
        self.assertEqual(status, 200, value)
        self.assertEqual(self.read("scope", grant)["job_ids"], [common])
        self.read(f"validate?job_id={granted}", grant, expected=403)
        manager_grant = self.grant(self.manager_cookie, state="b" * 64)
        self.assertIs(self.read("scope", manager_grant)["all_jobs"], True)
        self.assertEqual(self.read("jobs", manager_grant)["total"], 3)

    def test_grants_are_bound_to_original_punto_session_and_live_account(self):
        grant = self.grant(self.worker_cookie)
        other_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.read("scope", grant)
        status, value, _headers = self.api("POST", "/api/logout", {}, cookie=self.worker_cookie)
        self.assertEqual(status, 200, value)
        self.read("scope", grant, expected=401)
        replacement = self.grant(other_cookie, state="c" * 64)
        self.edit_user(self.worker_id, active="0")
        self.read("scope", replacement, expected=401)

    def test_bridge_rejects_identity_injection_and_inventory_outage_does_not_hide_jobs(self):
        self.create_job("AVAILABLE")
        grant = self.grant(self.manager_cookie)
        for resource in ("scope?user_id=1", "jobs?actor_id=1", "jobs?limit=1&%6cimit=2", "jobs?limit=0",
                         "assets?provider_url=https://foreign.invalid", "validate?job_id=1&job_id=2"):
            with self.subTest(resource=resource):
                self.read(resource, grant, expected=400)
        self.read("assets", grant, expected=503)
        self.read("validate?asset_id=1", grant, expected=503)
        self.assertEqual(self.read("jobs", grant)["total"], 1)


if __name__ == "__main__":
    unittest.main()
