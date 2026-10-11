"""Isolated HTTP integration tests for the separate native ticket application."""

from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "ticket-app"
PASSWORD = "Ticket-test-only!2037"
REPLACEMENT_PASSWORD = "Changed-test-only!2038"


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class TicketApplicationTests(unittest.TestCase):
    """Each test owns its database, bootstrap credentials, and loopback listener."""

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(
            ["make", "-C", str(APP)], capture_output=True, text=True, timeout=90
        )
        if result.returncode:
            raise RuntimeError("Ticket application build failed:\n" + result.stdout + result.stderr)
        cls.binary = APP / "build" / "punto-tickets"

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tickets-integration-")
        self.addCleanup(self.temporary.cleanup)
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.addCleanup(self.log.close)
        self.port = free_port()
        self.origin = f"http://127.0.0.1:{self.port}"
        self.environment = os.environ.copy()
        self.environment.update({
            "AUTH_MODE": "local",
            "TICKETS_BIND": "127.0.0.1",
            "TICKETS_PORT": str(self.port),
            "TICKETS_DB_PATH": str(Path(self.temporary.name) / "tickets.sqlite"),
            "TICKETS_PUBLIC_DIR": str(APP / "public"),
            "TICKETS_EMAIL": "admin@example.test",
            "TICKETS_PASSWORD": PASSWORD,
            "TICKETS_NAME": "Amministratore di collaudo",
            "TICKETS_PUBLIC_ORIGIN": self.origin,
        })
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [str(self.binary)], cwd=APP, env=self.environment,
            stdout=self.log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                break
            try:
                status, body, _headers = self.request("GET", "/healthz")
                if status == 200 and body.get("ok") is True:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.025)
        self.log.seek(0)
        self.fail("Ticket application did not become ready:\n" + self.log.read().decode("utf-8", "replace"))

    def stop(self):
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None

    def request(self, method, path, payload=None, session=None, headers=None):
        request_headers = {}
        if payload is not None:
            payload = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            request_headers["Content-Type"] = "application/json"
        if session:
            request_headers["Cookie"] = session["cookie"]
            request_headers["X-CSRF-Token"] = session["csrf_token"]
        if headers:
            for name, value in headers.items():
                if value is None:
                    request_headers.pop(name, None)
                else:
                    request_headers[name] = value
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            connection.request(method, path, body=payload, headers=request_headers)
            response = connection.getresponse()
            raw = response.read()
            response_headers = {key.lower(): value for key, value in response.getheaders()}
            if "application/json" in response_headers.get("content-type", ""):
                body = json.loads(raw)
            else:
                body = raw.decode("utf-8", "replace")
            return response.status, body, response_headers
        finally:
            connection.close()

    def api(self, method, path, payload=None, session=None, expected=200, headers=None):
        status, body, response_headers = self.request(method, path, payload, session, headers)
        self.assertEqual(status, expected, body)
        self.assertIsInstance(body, dict)
        if expected >= 400:
            self.assertIs(body.get("ok"), False, body)
            self.assertIsInstance(body.get("error"), str, body)
            self.assertTrue(body["error"], body)
        return body, response_headers

    def login(self, email="admin@example.test", password=PASSWORD):
        body, headers = self.api("POST", "/api/login", {"email": email, "password": password})
        self.assertIs(body["ok"], True)
        self.assertTrue(body["csrf_token"])
        self.assertIn("httponly", headers["set-cookie"].lower())
        self.assertIn("samesite=", headers["set-cookie"].lower())
        self.assertNotIn("password", body["user"])
        self.assertNotIn("password_hash", body["user"])
        return {**body, "cookie": headers["set-cookie"].split(";", 1)[0]}

    def create_ticket(self, session, **fields):
        body, _headers = self.api(
            "POST", "/api/tickets", {"title": "Stampante ufficio", **fields}, session, expected=201
        )
        return body["ticket"]

    def update_ticket(self, session, ticket, **fields):
        body, _headers = self.api(
            "PUT", f"/api/tickets/{ticket['id']}", {"revision": ticket["revision"], **fields}, session
        )
        return body["ticket"]

    def create_user(self, session, email="agent@example.test", role="agent", name="Operatore prova"):
        body, _headers = self.api("POST", "/api/users", {
            "name": name, "email": email, "password": PASSWORD, "role": role,
        }, session, expected=201)
        return body["user"]

    def set_ticket_activity(self, ticket, timestamp):
        """Pin timestamps in this test's database instead of waiting on the clock."""
        with sqlite3.connect(self.environment["TICKETS_DB_PATH"]) as database:
            database.execute("UPDATE tickets SET updated_at=? WHERE id=?", (timestamp, ticket["id"]))

    def test_health_and_protected_routes(self):
        body, _headers = self.api("GET", "/healthz")
        self.assertIs(body["ok"], True)
        for path in ("/api/session", "/api/tickets", "/api/users", "/api/tickets/1"):
            with self.subTest(path=path):
                self.api("GET", path, expected=401)

    def test_local_auth_configuration_and_sso_routes(self):
        body, _headers = self.api("GET", "/api/auth/config")
        self.assertEqual(body, {"sso": False, "local": True})
        self.api("POST", "/api/auth/config", {}, expected=405)
        for path in ("/auth/login", "/auth/callback?state=forged&code=forged"):
            with self.subTest(path=path):
                status, body, _headers = self.request("GET", path)
                self.assertIn(status, (400, 404, 503), body)
                self.assertIs(body.get("ok"), False)

    def test_invalid_auth_mode_fails_before_creating_database(self):
        database_path = Path(self.temporary.name) / "invalid-auth.sqlite"
        environment = {**self.environment, "AUTH_MODE": "unknown", "TICKETS_DB_PATH": str(database_path)}
        result = subprocess.run([str(self.binary)], cwd=APP, env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(database_path.exists())

    def test_oidc_without_configuration_fails_before_creating_database(self):
        database_path = Path(self.temporary.name) / "missing-oidc.sqlite"
        environment = {key: value for key, value in self.environment.items() if not key.startswith("OIDC_")}
        environment.update({"AUTH_MODE": "oidc", "TICKETS_DB_PATH": str(database_path)})
        result = subprocess.run([str(self.binary)], cwd=APP, env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(database_path.exists())

    def test_login_session_and_logout(self):
        admin = self.login()
        body, _headers = self.api("GET", "/api/session", session=admin)
        self.assertEqual(body["user"], admin["user"])
        self.assertEqual(body["csrf_token"], admin["csrf_token"])
        self.assertEqual(body["user"]["role"], "admin")
        self.api("POST", "/api/logout", {}, admin)
        self.api("GET", "/api/session", session=admin, expected=401)
        self.api("POST", "/api/tickets", {"title": "Unauthorized"}, admin, expected=401)

    def test_invalid_credentials_and_forged_session(self):
        for email, password in (("admin@example.test", "incorrect"), ("missing@example.test", PASSWORD)):
            with self.subTest(email=email):
                self.api("POST", "/api/login", {"email": email, "password": password}, expected=401)
        self.api("POST", "/api/login", {"email": "admin@example.test"}, expected=400)
        admin = self.login()
        forged = {**admin, "cookie": admin["cookie"] + "-tampered"}
        self.api("GET", "/api/session", session=forged, expected=401)

    def test_mutations_require_csrf_and_trusted_origin(self):
        admin = self.login()
        for headers in ({"X-CSRF-Token": None}, {"X-CSRF-Token": "forged"},
                        {"Origin": "https://foreign.example.test"}, {"Origin": "null"}):
            with self.subTest(headers=headers):
                self.api("POST", "/api/tickets", {"title": "Rejected"}, admin, expected=403, headers=headers)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 0)
        self.api("POST", "/api/tickets", {"title": "Origin accepted"}, admin, expected=201,
                 headers={"Origin": self.origin})
        self.api("POST", "/api/login", {"email": "admin@example.test", "password": PASSWORD},
                 expected=403, headers={"Origin": "https://foreign.example.test"})

    def test_foreign_host_is_rejected(self):
        status, _body, _headers = self.request("GET", "/healthz", headers={"Host": "foreign.example.test"})
        self.assertIn(status, (400, 403))
        self.api("GET", "/healthz")

    def test_https_public_origin_supports_reverse_proxy_transport(self):
        self.stop()
        self.environment["TICKETS_PUBLIC_ORIGIN"] = "https://ticket.example.test"
        self.start()
        proxy_headers = {"Host": "ticket.example.test", "Origin": "https://ticket.example.test"}
        body, headers = self.api("POST", "/api/login", {
            "email": "admin@example.test", "password": PASSWORD,
        }, headers=proxy_headers)
        self.assertIs(body["ok"], True)
        cookie_attributes = [item.strip().lower() for item in headers["set-cookie"].split(";")]
        self.assertIn("secure", cookie_attributes)
        self.assertIn("httponly", cookie_attributes)
        session = {**body, "cookie": headers["set-cookie"].split(";", 1)[0]}
        self.api("POST", "/api/tickets", {"title": "Ticket dal proxy HTTPS"}, session,
                 expected=201, headers=proxy_headers)
        self.api("POST", "/api/tickets", {"title": "Origin HTTP rifiutata"}, session,
                 expected=403, headers={"Host": "ticket.example.test", "Origin": "http://ticket.example.test"})

    def test_ticket_workflow_comments_and_audit_history(self):
        admin = self.login()
        ticket = self.create_ticket(admin, title="Accesso contabilità — caffè", description="Errore all'avvio",
                                    requester="Marta Rossi", priority="high", due_date="2032-02-29")
        self.assertEqual(ticket["title"], "Accesso contabilità — caffè")
        self.assertEqual(ticket["status"], "open")
        self.assertEqual(ticket["priority"], "high")
        self.assertEqual(ticket["requester"], "Marta Rossi")
        self.assertEqual(ticket["due_date"], "2032-02-29")
        self.assertIsNone(ticket["assignee_id"])
        self.assertEqual(ticket["created_by"], admin["user"]["id"])
        self.assertEqual(ticket["creator_name"], admin["user"]["name"])
        for field in ("created_at", "updated_at"):
            self.assertRegex(ticket[field], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
            datetime.fromisoformat(ticket[field].replace("Z", "+00:00"))
        initial_revision = ticket["revision"]
        for state in ("in_progress", "waiting", "resolved", "closed", "open"):
            previous_revision = ticket["revision"]
            ticket = self.update_ticket(admin, ticket, status=state, title=ticket["title"])
            self.assertEqual(ticket["status"], state)
            self.assertGreater(ticket["revision"], previous_revision)
        self.assertGreater(ticket["revision"], initial_revision)
        self.set_ticket_activity(ticket, "2000-01-01T00:00:00Z")
        other = self.create_ticket(admin, title="Ultima attività precedente")
        self.set_ticket_activity(other, "2001-01-01T00:00:00Z")
        before_comment, _headers = self.api("GET", "/api/tickets?sort=updated", session=admin)
        self.assertEqual([row["id"] for row in before_comment["items"]], [other["id"], ticket["id"]])
        body, _headers = self.api("POST", f"/api/tickets/{ticket['id']}/comments",
                                  {"body": "Verifica completata, perché l'accesso è ripristinato."}, admin, expected=201)
        self.assertEqual(body["comment"]["body"], "Verifica completata, perché l'accesso è ripristinato.")
        detail, _headers = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)
        self.assertEqual(detail["ticket"]["revision"], ticket["revision"])
        self.assertNotEqual(detail["ticket"]["updated_at"], "2000-01-01T00:00:00Z")
        self.assertGreaterEqual(detail["ticket"]["updated_at"], body["comment"]["created_at"])
        self.assertEqual(detail["ticket"]["created_at"], ticket["created_at"])
        after_comment, _headers = self.api("GET", "/api/tickets?sort=updated", session=admin)
        self.assertEqual([row["id"] for row in after_comment["items"]], [ticket["id"], other["id"]])
        self.assertEqual(len(detail["comments"]), 1)
        self.assertEqual(detail["comments"][0]["id"], body["comment"]["id"])
        self.assertGreaterEqual(len(detail["history"]), 6)
        latest_update = next(entry for entry in detail["history"] if entry["action"] == "updated")
        self.assertIn("stato", latest_update["detail"].lower())
        self.assertNotIn("titolo", latest_update["detail"].lower())

    def test_search_filters_pagination_and_global_counts(self):
        admin = self.login()
        first = self.create_ticket(admin, title="Rete sala nord", description="Ricerca univoca ZXQ", priority="urgent")
        second = self.create_ticket(admin, title="Stampante sala sud", priority="low")
        second = self.update_ticket(admin, second, status="resolved")
        third = self.create_ticket(admin, title="Richiesta badge", requester="Alessia Test")
        third = self.update_ticket(admin, third, status="waiting")
        filtered, _headers = self.api("GET", "/api/tickets?" + urlencode({"search": "univoca ZXQ"}), session=admin)
        self.assertEqual(filtered["total"], 1)
        self.assertEqual([row["id"] for row in filtered["items"]], [first["id"]])
        filtered, _headers = self.api("GET", "/api/tickets?status=resolved&priority=low", session=admin)
        self.assertEqual(filtered["total"], 1)
        self.assertEqual(filtered["items"][0]["id"], second["id"])
        self.assertEqual(filtered["stats"], {"open": 1, "in_progress": 0, "waiting": 1, "resolved": 1, "closed": 0})
        pages = [self.api("GET", f"/api/tickets?limit=1&offset={offset}", session=admin)[0]
                 for offset in range(4)]
        self.assertTrue(all(page["total"] == 3 for page in pages))
        self.assertEqual({page["items"][0]["id"] for page in pages[:3]}, {first["id"], second["id"], third["id"]})
        self.assertEqual(pages[3]["items"], [])

    def test_work_queues_use_assignee_and_keep_global_counts(self):
        admin = self.login()
        agent = self.create_user(admin)
        other = self.create_user(admin, email="other-agent@example.test", name="Altro operatore")
        operator = self.login(agent["email"])
        definitions = (
            ("mine_past", "open", agent["id"], "2032-02-28", admin),
            ("created_by_me", "in_progress", other["id"], "2032-02-27", operator),
            ("unassigned_today", "waiting", None, "2032-02-29", admin),
            ("mine_day_seven", "open", agent["id"], "2032-03-07", admin),
            ("other_day_eight", "open", other["id"], "2032-03-08", admin),
            ("unassigned_no_due", "open", None, None, admin),
            ("resolved_past", "resolved", agent["id"], "2032-02-28", admin),
            ("closed_today", "closed", None, "2032-02-29", admin),
        )
        tickets = {}
        for title, status, assignee_id, due_date, creator in definitions:
            ticket = self.create_ticket(creator, title=title, assignee_id=assignee_id, due_date=due_date)
            tickets[title] = self.update_ticket(admin, ticket, status=status) if status != "open" else ticket
        expected = {
            "all": set(tickets),
            "active": {"mine_past", "created_by_me", "unassigned_today", "mine_day_seven", "other_day_eight", "unassigned_no_due"},
            "mine": {"mine_past", "mine_day_seven"},
            "overdue": {"mine_past", "created_by_me"},
            "due_soon": {"unassigned_today", "mine_day_seven"},
            "unassigned": {"unassigned_today", "unassigned_no_due"},
        }
        counts = {queue: len(names) for queue, names in expected.items() if queue != "all"}
        stats = {"open": 4, "in_progress": 1, "waiting": 1, "resolved": 1, "closed": 1}
        for queue, names in expected.items():
            with self.subTest(queue=queue):
                body, _headers = self.api("GET", f"/api/tickets?queue={queue}&today=2032-02-29", session=operator)
                self.assertEqual({row["id"] for row in body["items"]}, {tickets[name]["id"] for name in names})
                self.assertEqual(body["total"], len(names))
                self.assertEqual(body["queues"], counts)
                self.assertEqual(body["stats"], stats)
                self.assertEqual(body["reference_date"], "2032-02-29")
        body, _headers = self.api("GET", "/api/tickets?queue=mine&today=2032-02-29", session=admin)
        self.assertEqual(body["total"], 0)
        self.assertEqual(body["queues"]["mine"], 0)
        for query in ("queue=active&status=resolved", f"queue=mine&assignee_id={other['id']}"):
            with self.subTest(query=query):
                body, _headers = self.api("GET", f"/api/tickets?{query}&today=2032-02-29", session=operator)
                self.assertEqual(body["items"], [])
                self.assertEqual(body["total"], 0)
                self.assertEqual(body["queues"], counts)
                self.assertEqual(body["stats"], stats)
        body, _headers = self.api("GET", "/api/tickets?search=resolved_past&status=resolved&today=2032-02-29", session=operator)
        self.assertEqual([row["id"] for row in body["items"]], [tickets["resolved_past"]["id"]])
        self.assertEqual(body["queues"], counts)
        self.assertEqual(body["stats"], stats)

    def test_due_soon_clamps_to_last_supported_calendar_date(self):
        admin = self.login()
        past = self.create_ticket(admin, title="Già scaduto", due_date="9999-12-28")
        today = self.create_ticket(admin, title="Scade oggi", due_date="9999-12-29")
        last = self.create_ticket(admin, title="Ultima data", due_date="9999-12-31")
        self.create_ticket(admin, title="Senza scadenza")
        for reference, expected in (("9999-12-29", {today["id"], last["id"]}),
                                    ("9999-12-31", {last["id"]})):
            with self.subTest(reference=reference):
                body, _headers = self.api("GET", f"/api/tickets?queue=due_soon&today={reference}", session=admin)
                self.assertEqual({row["id"] for row in body["items"]}, expected)
                self.assertEqual(body["total"], len(expected))
                self.assertEqual(body["queues"]["due_soon"], len(expected))
                self.assertEqual(body["reference_date"], reference)
        body, _headers = self.api("GET", "/api/tickets?queue=overdue&today=9999-12-29", session=admin)
        self.assertEqual([row["id"] for row in body["items"]], [past["id"]])

    def test_sort_orders_and_pagination_are_deterministic(self):
        admin = self.login()
        definitions = (
            ("low", "low", "2032-02-20", "2030-01-05T00:00:00Z"),
            ("urgent_no_due", "urgent", None, "2030-01-04T00:00:00Z"),
            ("high", "high", "2032-02-18", "2030-01-03T00:00:00Z"),
            ("urgent_first", "urgent", "2032-02-19", "2030-01-02T00:00:00Z"),
            ("urgent_tied", "urgent", "2032-02-19", "2030-01-02T00:00:00Z"),
            ("normal", "normal", "2032-02-18", "2030-01-01T00:00:00Z"),
            ("normal_no_due", "normal", None, "2030-01-06T00:00:00Z"),
        )
        tickets = {}
        for title, priority, due_date, activity in definitions:
            tickets[title] = self.create_ticket(admin, title=title, priority=priority, due_date=due_date)
            self.set_ticket_activity(tickets[title], activity)
        orders = {
            "updated": ("normal_no_due", "low", "urgent_no_due", "high", "urgent_tied", "urgent_first", "normal"),
            "priority": ("urgent_tied", "urgent_first", "urgent_no_due", "high", "normal", "normal_no_due", "low"),
            "due": ("high", "normal", "urgent_tied", "urgent_first", "low", "urgent_no_due", "normal_no_due"),
        }
        for sort, names in orders.items():
            with self.subTest(sort=sort):
                expected = [tickets[name]["id"] for name in names]
                body, _headers = self.api("GET", f"/api/tickets?sort={sort}&today=2032-02-29", session=admin)
                self.assertEqual([row["id"] for row in body["items"]], expected)
                pages = [self.api("GET", f"/api/tickets?sort={sort}&today=2032-02-29&limit=2&offset={offset}",
                                  session=admin)[0] for offset in range(0, 10, 2)]
                self.assertTrue(all(page["total"] == 7 for page in pages))
                self.assertEqual([row["id"] for page in pages for row in page["items"]], expected)
        first, _headers = self.api("GET", "/api/tickets?queue=active&status=open&priority=urgent&sort=due&limit=2&today=2032-02-29", session=admin)
        second, _headers = self.api("GET", "/api/tickets?queue=active&status=open&priority=urgent&sort=due&limit=2&offset=2&today=2032-02-29", session=admin)
        self.assertEqual(first["total"], 3)
        self.assertEqual(second["total"], 3)
        self.assertEqual([row["id"] for row in first["items"] + second["items"]],
                         [tickets[name]["id"] for name in ("urgent_tied", "urgent_first", "urgent_no_due")])

    def test_status_columns_paginate_after_queue_and_search_filters(self):
        admin = self.login()
        agent = self.create_user(admin)
        operator = self.login(agent["email"])
        expected = {self.create_ticket(admin, title=f"Boardfixture {index:02d}", priority="high",
                                       assignee_id=agent["id"])["id"] for index in range(21)}
        waiting = self.create_ticket(admin, title="Boardfixture waiting", priority="high", assignee_id=agent["id"])
        self.update_ticket(admin, waiting, status="waiting")
        resolved = self.create_ticket(admin, title="Boardfixture resolved", priority="high", assignee_id=agent["id"])
        self.update_ticket(admin, resolved, status="resolved")
        self.create_ticket(admin, title="Boardfixture other assignee", priority="high", assignee_id=admin["user"]["id"])
        self.create_ticket(admin, title="Boardfixture low", priority="low", assignee_id=agent["id"])
        self.create_ticket(admin, title="Unrelated search", priority="high", assignee_id=agent["id"])
        pages = [self.api("GET", "/api/tickets?queue=mine&status=open&search=Boardfixture&priority=high"
                         f"&today=2032-02-29&limit=20&offset={offset}", session=operator)[0]
                 for offset in (0, 20, 40)]
        self.assertEqual([len(page["items"]) for page in pages], [20, 1, 0])
        self.assertTrue(all(page["total"] == 21 for page in pages))
        ids = [row["id"] for page in pages for row in page["items"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(set(ids), expected)

    def test_queue_sort_and_reference_date_reject_invalid_or_duplicate_values(self):
        admin = self.login()
        invalid = ("queue=missing", "queue=", "sort=missing", "sort=", "sort=priority%3BDROP+TABLE+tickets",
                   "today=", "today=2031-02-29", "today=2032-2-29", "today=0000-01-01", "today=10000-01-01",
                   "today=2032-02-29junk", "queue=all&queue=active", "sort=due&sort=updated",
                   "today=2032-02-29&today=2032-03-01", "queue=all&%71ueue=all")
        for query in invalid:
            with self.subTest(query=query):
                self.api("GET", f"/api/tickets?{query}", session=admin, expected=400)
        body, _headers = self.api("GET", "/api/tickets?today=2032-02-29", session=admin)
        self.assertEqual(body["reference_date"], "2032-02-29")
        self.assertEqual(body["items"], [])
        before = datetime.now(timezone.utc).date().isoformat()
        default, _headers = self.api("GET", "/api/tickets", session=admin)
        after = datetime.now(timezone.utc).date().isoformat()
        self.assertIn(default["reference_date"], (before, after))

    def test_assignment_requires_an_active_user(self):
        admin = self.login()
        agent = self.create_user(admin)
        ticket = self.create_ticket(admin, assignee_id=agent["id"])
        self.assertEqual(ticket["assignee_id"], agent["id"])
        self.assertEqual(ticket["assignee_name"], agent["name"])
        filtered, _headers = self.api("GET", f"/api/tickets?assignee_id={agent['id']}", session=admin)
        self.assertEqual([row["id"] for row in filtered["items"]], [ticket["id"]])
        retained = self.create_ticket(admin, title="Assegnazione storica", assignee_id=agent["id"])
        ticket = self.update_ticket(admin, ticket, assignee_id=None, due_date=None)
        self.assertIsNone(ticket["assignee_id"])
        self.assertIsNone(ticket["due_date"])
        unassigned, _headers = self.api("GET", "/api/tickets?assignee_id=unassigned", session=admin)
        self.assertEqual([row["id"] for row in unassigned["items"]], [ticket["id"]])
        self.api("PUT", f"/api/users/{agent['id']}", {"active": False}, admin)
        retained = self.update_ticket(admin, retained, status="in_progress")
        retained = self.update_ticket(admin, retained, title="Ticket ancora modificabile", assignee_id=agent["id"])
        self.assertEqual(retained["assignee_id"], agent["id"])
        self.assertEqual(retained["title"], "Ticket ancora modificabile")
        for assignee_id in (agent["id"], 999999):
            with self.subTest(assignee_id=assignee_id):
                self.api("POST", "/api/tickets", {"title": "Invalid assignment", "assignee_id": assignee_id},
                         admin, expected=400)

    def test_ticket_validation_rejects_invalid_data_without_writes(self):
        admin = self.login()
        invalid = [{}, {"title": "   "}, {"title": "a" * 161}, {"title": 42},
                   {"title": "Valid", "description": "a" * 8001},
                   {"title": "Valid", "requester": "a" * 201},
                   {"title": "Valid", "priority": "emergency"},
                   {"title": "Valid", "assignee_id": "1"}]
        invalid.extend({"title": "Invalid date", "due_date": value} for value in
                       ("2031-02-29", "2030-13-01", "2030-04-31", "2030-01-00", "2030-1-01", "2030-01-01junk"))
        for fields in invalid:
            with self.subTest(fields={key: str(value)[:80] for key, value in fields.items()}):
                self.api("POST", "/api/tickets", fields, admin, expected=400)
        body, _headers = self.api("GET", "/api/tickets", session=admin)
        self.assertEqual(body["total"], 0)

    def test_revision_prevents_lost_updates(self):
        admin = self.login()
        original = self.create_ticket(admin)
        current = self.update_ticket(admin, original, title="Correzione salvata")
        self.api("PUT", f"/api/tickets/{original['id']}",
                 {"revision": original["revision"], "title": "Modifica obsoleta"}, admin, expected=409)
        self.api("PUT", f"/api/tickets/{original['id']}", {"title": "Missing revision"}, admin, expected=400)
        self.api("PUT", f"/api/tickets/{original['id']}",
                 {"revision": current["revision"], "status": "invalid"}, admin, expected=400)
        body, _headers = self.api("GET", f"/api/tickets/{original['id']}", session=admin)
        self.assertEqual(body["ticket"], current)

    def test_unicode_title_limit_counts_characters(self):
        admin = self.login()
        ticket = self.create_ticket(admin, title="è" * 160)
        self.assertEqual(ticket["title"], "è" * 160)
        results, _headers = self.api("GET", "/api/tickets?" + urlencode({"search": ticket["title"]}), session=admin)
        self.assertEqual([row["id"] for row in results["items"]], [ticket["id"]])
        self.api("POST", "/api/tickets", {"title": "è" * 161}, admin, expected=400)
        self.api("PUT", f"/api/tickets/{ticket['id']}",
                 {"revision": ticket["revision"], "title": "è" * 161}, admin, expected=400)
        body, _headers = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)
        self.assertEqual(body["ticket"]["title"], "è" * 160)

    def test_comment_validation(self):
        admin = self.login()
        ticket = self.create_ticket(admin)
        for value in ("", " \n\t ", "a" * 4001, 42, None):
            with self.subTest(value=str(value)[:50]):
                self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": value}, admin, expected=400)
        body, _headers = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)
        self.assertEqual(body["comments"], [])
        self.assertEqual(body["ticket"], ticket)

    def test_agents_manage_tickets_but_cannot_manage_accounts(self):
        admin = self.login()
        agent = self.create_user(admin)
        inactive = self.create_user(admin, email="inactive@example.test", name="Operatore disattivato")
        self.api("PUT", f"/api/users/{inactive['id']}", {"active": False}, admin)
        operator = self.login(agent["email"])
        users, _headers = self.api("GET", "/api/users", session=operator)
        self.assertIn(agent["id"], {row["id"] for row in users["items"]})
        self.assertNotIn(inactive["id"], {row["id"] for row in users["items"]})
        all_users, _headers = self.api("GET", "/api/users", session=admin)
        self.assertIn(inactive["id"], {row["id"] for row in all_users["items"]})
        self.api("POST", "/api/users", {"name": "Forbidden", "email": "forbidden@example.test",
                 "password": PASSWORD, "role": "admin"}, operator, expected=403)
        self.api("PUT", f"/api/users/{agent['id']}", {"role": "admin"}, operator, expected=403)
        ticket = self.create_ticket(admin)
        ticket = self.update_ticket(operator, ticket, status="in_progress", assignee_id=agent["id"])
        self.assertEqual(ticket["status"], "in_progress")
        self.create_ticket(operator, title="Creato dall'operatore")
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Presa in carico"}, operator, expected=201)

    def test_last_active_administrator_is_protected(self):
        admin = self.login()
        for payload in ({"active": False}, {"role": "agent"}):
            with self.subTest(payload=payload):
                status, body, _headers = self.request("PUT", f"/api/users/{admin['user']['id']}", payload, admin)
                self.assertIn(status, (400, 409), body)
                self.assertIs(body["ok"], False)
        session, _headers = self.api("GET", "/api/session", session=admin)
        self.assertEqual(session["user"]["role"], "admin")
        self.assertTrue(session["user"]["active"])

    def test_role_change_revokes_existing_sessions(self):
        original_admin = self.login()
        second = self.create_user(original_admin, email="second-admin@example.test", role="admin")
        second_admin = self.login(second["email"])
        body, _headers = self.api("PUT", f"/api/users/{original_admin['user']['id']}",
                                  {"role": "agent"}, second_admin)
        self.assertEqual(body["user"]["role"], "agent")
        self.api("GET", "/api/session", session=original_admin, expected=401)
        operator = self.login()
        self.assertEqual(operator["user"]["role"], "agent")
        self.api("POST", "/api/users", {"name": "Forbidden", "email": "x@example.test",
                 "password": PASSWORD, "role": "admin"}, operator, expected=403)

    def test_password_and_deactivation_revoke_sessions(self):
        admin = self.login()
        user = self.create_user(admin)
        first_session = self.login(user["email"])
        self.api("PUT", f"/api/users/{user['id']}", {"password": REPLACEMENT_PASSWORD}, admin)
        self.api("GET", "/api/session", session=first_session, expected=401)
        self.api("POST", "/api/login", {"email": user["email"], "password": PASSWORD}, expected=401)
        second_session = self.login(user["email"], REPLACEMENT_PASSWORD)
        self.api("PUT", f"/api/users/{user['id']}", {"active": False}, admin)
        self.api("GET", "/api/session", session=second_session, expected=401)
        self.api("POST", "/api/login", {"email": user["email"], "password": REPLACEMENT_PASSWORD}, expected=401)

    def test_tickets_comments_and_changed_credentials_survive_restart(self):
        admin = self.login()
        ticket = self.create_ticket(admin, title="Ticket persistente", due_date="2030-12-31")
        ticket = self.update_ticket(admin, ticket, status="waiting")
        self.api("POST", f"/api/tickets/{ticket['id']}/comments", {"body": "Persistenza verificabile"}, admin, expected=201)
        ticket = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)[0]["ticket"]
        self.api("PUT", f"/api/users/{admin['user']['id']}", {"password": REPLACEMENT_PASSWORD}, admin)
        previous_session = self.login(password=REPLACEMENT_PASSWORD)
        self.stop()
        self.start()
        self.api("GET", "/api/session", session=previous_session, expected=401)
        self.api("POST", "/api/login", {"email": "admin@example.test", "password": PASSWORD}, expected=401)
        admin = self.login(password=REPLACEMENT_PASSWORD)
        detail, _headers = self.api("GET", f"/api/tickets/{ticket['id']}", session=admin)
        self.assertEqual(detail["ticket"], ticket)
        self.assertEqual([row["body"] for row in detail["comments"]], ["Persistenza verificabile"])
        self.assertGreaterEqual(len(detail["history"]), 2)
        users, _headers = self.api("GET", "/api/users", session=admin)
        self.assertEqual(len(users["items"]), 1)

    def test_missing_records_return_not_found(self):
        admin = self.login()
        self.api("GET", "/api/tickets/999999", session=admin, expected=404)
        self.api("PUT", "/api/tickets/999999", {"revision": 1, "title": "Missing"}, admin, expected=404)
        self.api("POST", "/api/tickets/999999/comments", {"body": "Missing"}, admin, expected=404)
        self.api("PUT", "/api/users/999999", {"name": "Missing"}, admin, expected=404)

    def test_unsupported_database_version_is_preserved(self):
        self.stop()
        database_path = Path(self.temporary.name) / "future-version.sqlite"
        with sqlite3.connect(database_path) as database:
            database.execute("PRAGMA user_version=99")
            database.execute("CREATE TABLE sentinel(id INTEGER PRIMARY KEY, value TEXT)")
            database.execute("INSERT INTO sentinel VALUES(1, 'Archivio da preservare')")
            original_schema = database.execute("SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY name").fetchall()
        environment = {**self.environment, "TICKETS_DB_PATH": str(database_path)}
        result = subprocess.run([str(self.binary)], cwd=APP, env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(database_path) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 99)
            self.assertEqual(database.execute("SELECT * FROM sentinel").fetchall(), [(1, "Archivio da preservare")])
            self.assertEqual(database.execute("SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY name").fetchall(),
                             original_schema)

    def test_schema_one_migration_preserves_existing_accounts_tickets_and_history(self):
        self.stop()
        with sqlite3.connect(self.environment["TICKETS_DB_PATH"]) as database:
            account = database.execute("SELECT id,name,email,role,active,password_salt,password_hash,created_at FROM users").fetchone()
        legacy_path = Path(self.temporary.name) / "schema-one.sqlite"
        with sqlite3.connect(legacy_path) as database:
            # The actual prior schema: no new reference columns or current-schema pragma trick.
            database.executescript("""
                CREATE TABLE users(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,email TEXT NOT NULL COLLATE NOCASE UNIQUE,role TEXT NOT NULL CHECK(role IN('admin','agent')),active INTEGER NOT NULL DEFAULT 1 CHECK(active IN(0,1)),password_salt TEXT NOT NULL,password_hash TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));
                CREATE TABLE tickets(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,description TEXT NOT NULL DEFAULT '',requester TEXT NOT NULL DEFAULT '',status TEXT NOT NULL DEFAULT 'open' CHECK(status IN('open','in_progress','waiting','resolved','closed')),priority TEXT NOT NULL DEFAULT 'normal' CHECK(priority IN('low','normal','high','urgent')),assignee_id INTEGER REFERENCES users(id),created_by INTEGER NOT NULL REFERENCES users(id),due_date TEXT,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),revision INTEGER NOT NULL DEFAULT 1);
                CREATE INDEX tickets_updated_idx ON tickets(updated_at DESC,id DESC);
                CREATE INDEX tickets_filter_idx ON tickets(status,priority,assignee_id);
                CREATE TABLE comments(id INTEGER PRIMARY KEY AUTOINCREMENT,ticket_id INTEGER NOT NULL REFERENCES tickets(id),author_id INTEGER NOT NULL REFERENCES users(id),body TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));
                CREATE INDEX comments_ticket_idx ON comments(ticket_id,id);
                CREATE TABLE history(id INTEGER PRIMARY KEY AUTOINCREMENT,ticket_id INTEGER NOT NULL REFERENCES tickets(id),actor_id INTEGER NOT NULL REFERENCES users(id),actor_name TEXT NOT NULL,action TEXT NOT NULL,detail TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));
                CREATE INDEX history_ticket_idx ON history(ticket_id,id);
                PRAGMA user_version=1;
            """)
            database.execute("INSERT INTO users VALUES(?,?,?,?,?,?,?,?)", account)
            original_ticket = (41, "Ticket storico", "Descrizione originale caffè", "Richiedente originale", "waiting", "high",
                               account[0], account[0], "2030-12-31", "2030-01-02T03:04:05Z", "2030-01-03T04:05:06Z", 7)
            database.execute("INSERT INTO tickets VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", original_ticket)
            original_comment = (77, 41, account[0], "Commento storico", "2030-01-03T04:05:06Z")
            database.execute("INSERT INTO comments VALUES(?,?,?,?,?)", original_comment)
            original_history = (88, 41, account[0], account[1], "created", "Ticket creato prima della migrazione.", "2030-01-02T03:04:05Z")
            database.execute("INSERT INTO history VALUES(?,?,?,?,?,?,?)", original_history)
        self.environment["TICKETS_DB_PATH"] = str(legacy_path)
        for _restart in range(2):
            self.start()
            admin = self.login()
            detail, _headers = self.api("GET", "/api/tickets/41", session=admin)
            self.assertEqual(detail["ticket"]["revision"], 7)
            self.assertEqual(detail["ticket"]["description"], original_ticket[2])
            self.assertEqual(detail["comments"][0]["id"], 77)
            self.assertEqual(detail["comments"][0]["body"], original_comment[3])
            self.assertEqual(detail["history"][0]["id"], 88)
            for key in ("job_id", "job_code", "job_name", "asset_id", "asset_tag", "asset_name"):
                self.assertIsNone(detail["ticket"][key], key)
            self.stop()
            with sqlite3.connect(legacy_path) as database:
                self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 3)
                self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])
                self.assertEqual(database.execute("SELECT id,name,email,role,active,password_salt,password_hash,created_at FROM users").fetchone(), account)
                self.assertEqual(database.execute("SELECT id,title,description,requester,status,priority,assignee_id,created_by,due_date,created_at,updated_at,revision FROM tickets WHERE id=41").fetchone(), original_ticket)
                self.assertEqual(database.execute("SELECT * FROM comments WHERE id=77").fetchone(), original_comment)
                self.assertEqual(database.execute("SELECT * FROM history WHERE id=88").fetchone(), original_history)
        self.start()
        admin = self.login()
        created = self.create_ticket(admin, title="Creato dopo la migrazione")
        self.assertGreater(created["id"], 41)
        updated = self.update_ticket(admin, detail["ticket"], title="Modifica da client precedente")
        self.assertEqual(updated["revision"], 8)
        comment, _headers = self.api("POST", "/api/tickets/41/comments", {"body": "Commento dopo la migrazione"}, admin, expected=201)
        self.assertGreater(comment["comment"]["id"], 77)

    def test_ambiguous_http_request_framing_is_rejected(self):
        host = f"Host: 127.0.0.1:{self.port}\r\n".encode("ascii")
        prefixes = (
            b"Content-Length: 0\r\nContent-Length: 1\r\n",
            b"Content-Length: -1\r\n",
            b"Content-Length: 1junk\r\n",
            b"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
            b"Transfer-Encoding: chunked\r\n",
        )
        for framing in prefixes:
            with self.subTest(framing=framing):
                raw = b"POST /api/login HTTP/1.1\r\n" + host + framing + b"Connection: close\r\n\r\n0\r\n\r\n"
                with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
                    connection.sendall(raw)
                    connection.shutdown(socket.SHUT_WR)
                    response = b""
                    while True:
                        part = connection.recv(8192)
                        if not part:
                            break
                        response += part
                self.assertTrue(response.startswith(b"HTTP/1.1 "), response[:200])
                status = int(response.split(b" ", 2)[1])
                self.assertIn(status, (400, 411, 413, 501), response[:300])
        self.api("GET", "/healthz")


if __name__ == "__main__":
    unittest.main()
