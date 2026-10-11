"""Security audit regressions using disposable accounts and loopback servers."""

import http.client
import json
import os
import select
import socket
import time
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

import test_roles as roles


class SecurityRegressionTests(unittest.TestCase):
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    stop_server = roles.RoleIntegrationTests.stop_server
    start_server = roles.RoleIntegrationTests.start_server

    def setUp(self):
        environment = {name: value for name, value in os.environ.items()
                       if not name.startswith(("OIDC_", "AUTH_", "TICKETS_", "INVENTORY_",
                                               "CALENDAR_", "MAIL_", "IDENTITY_SYNC_"))}
        environment.update(AUTH_MODE="local", AUTH_USER_MANAGEMENT="local",
                           BIND_HOST="127.0.0.1", TICKETS_APP_ORIGIN="http://127.0.0.1:19837")
        with patch.dict(os.environ, environment, clear=True):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()

    def session(self, cookie):
        status, data, _ = self.api("GET", "/api/session", cookie=cookie)
        self.assertEqual(status, 200, data)
        return data

    def grant(self, cookie, number):
        state = format(number, "064x")
        status, data, _ = self.api("POST", "/api/integrations/tickets/grant",
                                 {"state": state}, cookie=cookie)
        self.assertEqual(status, 200, data)
        fragment = parse_qs(urlsplit(data["url"]).fragment)
        self.assertEqual(fragment["state"], [state])
        return fragment["punto_grant"][0]

    def scope(self, grant, expected=200):
        status, data, _ = self.api("GET", "/api/integrations/tickets/scope",
                                 headers={"Authorization": "Bearer " + grant})
        self.assertEqual(status, expected, data)
        return data

    def health(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=1)
        try:
            connection.request("GET", "/healthz")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertTrue(json.loads(response.read())["ok"])
        finally:
            connection.close()

    def test_login_failure_limits_are_scoped_to_the_normalized_identity(self):
        for _ in range(8):
            status, _, _ = self.api("POST", "/api/login", {
                "email": "unknown@punto.local", "password": "incorrect-password"})
            self.assertEqual(status, 401)
        status, _, _ = self.api("POST", "/api/login", {
            "email": "unknown@punto.local", "password": "incorrect-password"})
        self.assertEqual(status, 429)
        self.assertTrue(self.session(self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD))["authenticated"])

        for index in range(8):
            email = roles.WORKER_EMAIL.upper() if index % 2 else " " + roles.WORKER_EMAIL + " "
            status, _, _ = self.api("POST", "/api/login", {
                "email": email, "password": "incorrect-password"})
            self.assertEqual(status, 401)
        status, _, _ = self.api("POST", "/api/login", {
            "email": roles.WORKER_EMAIL, "password": "incorrect-password"})
        self.assertEqual(status, 429)
        status, _, _ = self.api("POST", "/api/login", {
            "email": roles.WORKER_EMAIL, "password": roles.WORKER_PASSWORD})
        self.assertEqual(status, 429, "The throttled identity is rejected before password hashing")
        self.assertTrue(self.session(self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD))["authenticated"])

    def test_worker_login_flood_replaces_only_own_oldest_sessions(self):
        cookies = []
        for _ in range(65):
            status, data, headers = self.api("POST", "/api/login", {
                "email": roles.WORKER_EMAIL, "password": roles.WORKER_PASSWORD, "remember": "on"})
            self.assertEqual(status, 200, data)
            cookies.append(headers["Set-Cookie"].split(";", 1)[0])
        self.assertTrue(self.session(self.manager_cookie)["authenticated"])
        self.assertFalse(self.session(cookies[0])["authenticated"])
        self.assertFalse(self.session(cookies[-5])["authenticated"])
        for cookie in cookies[-4:]:
            self.assertTrue(self.session(cookie)["authenticated"])

    def test_rejected_attempts_do_not_extend_the_login_failure_window(self):
        started = time.monotonic()
        for _ in range(8):
            status, _, _ = self.api("POST", "/api/login", {
                "email": roles.WORKER_EMAIL, "password": "incorrect-password"})
            self.assertEqual(status, 401)
        time.sleep(max(0, started + 30 - time.monotonic()))
        status, _, _ = self.api("POST", "/api/login", {
            "email": roles.WORKER_EMAIL, "password": roles.WORKER_PASSWORD})
        self.assertEqual(status, 429)
        self.assertTrue(self.session(self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD))["authenticated"])
        time.sleep(max(0, started + 61 - time.monotonic()))
        self.assertTrue(self.session(self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD))["authenticated"])
        status, _, _ = self.api("POST", "/api/login", {
            "email": roles.WORKER_EMAIL, "password": "incorrect-password"})
        self.assertEqual(status, 401)

    def test_grant_flood_and_logout_cannot_starve_other_users(self):
        manager_grant = self.grant(self.manager_cookie, 1000)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        grants = [self.grant(worker, number) for number in range(129)]
        self.scope(manager_grant)
        self.scope(grants[0], expected=401)
        self.scope(grants[-3], expected=401)
        self.scope(grants[-2])
        self.scope(grants[-1])
        status, _, _ = self.api("POST", "/api/logout", {}, cookie=worker)
        self.assertEqual(status, 200)
        self.scope(grants[-1], expected=401)
        self.scope(manager_grant)
        self.scope(self.grant(self.manager_cookie, 1001))

    def test_session_rotation_revokes_only_its_dependent_grants(self):
        manager_grant = self.grant(self.manager_cookie, 1000)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        old_grant = self.grant(worker, 1)
        for _ in range(4):
            worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.scope(old_grant, expected=401)
        self.scope(manager_grant)
        self.scope(self.grant(worker, 2))

    def test_idle_header_and_body_connections_do_not_block_health_or_login(self):
        sockets = [socket.create_connection(("127.0.0.1", self.port), timeout=1)
                   for _ in range(3)]
        try:
            sockets[1].sendall(b"GET /healthz HTTP/1.1\r\nX-Slow: ")
            sockets[2].sendall(("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                                "Content-Length: 100\r\n\r\nx" % self.port).encode())
            self.health()
            self.assertTrue(self.session(self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD))["authenticated"])
            self.health()
        finally:
            for connection in sockets:
                connection.close()

    def test_route_body_limits_are_enforced_before_waiting_for_the_body(self):
        for route, limit in (("/api/login", 8192), ("/api/jobs", 32768),
                             ("/api/tasks", 65536)):
            with self.subTest(route=route):
                with socket.create_connection(("127.0.0.1", self.port), timeout=1) as connection:
                    connection.sendall(("POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                                        "Content-Length: %d\r\n\r\n" %
                                        (route, self.port, limit + 1)).encode())
                    self.assertIn(b"413 Payload Too Large", connection.recv(4096))
        self.health()

    def test_drip_feed_cannot_extend_the_absolute_request_deadline(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=1) as slow:
            slow.sendall(b"GET /healthz HTTP/1.1\r\nX-Slow: ")
            started = time.monotonic()
            response = bytearray()
            while time.monotonic() - started < 7:
                self.health()
                try:
                    slow.sendall(b"x")
                except (BrokenPipeError, ConnectionResetError):
                    break
                readable, _, _ = select.select([slow], [], [], 0.4)
                if readable:
                    response.extend(slow.recv(4096))
                    break
            self.assertIn(b"408 Request Timeout", response)
            self.assertLess(time.monotonic() - started, 6.5)
        self.health()

    def test_idle_connection_quota_preserves_service_for_new_requests(self):
        sockets = []
        try:
            for _ in range(70):
                sockets.append(socket.create_connection(("127.0.0.1", self.port), timeout=2))
            self.health()
            self.assertTrue(self.session(self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD))["authenticated"])
        finally:
            for connection in sockets:
                connection.close()


if __name__ == "__main__":
    unittest.main()
