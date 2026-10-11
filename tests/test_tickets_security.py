"""Security regressions for account isolation and the native HTTP transport."""

from contextlib import ExitStack
import http.client
from pathlib import Path
import socket
import time
import unittest

import test_tickets as ticket_http


class TicketSecurityTests(unittest.TestCase):
    setUp = ticket_http.TicketApplicationTests.setUp
    start = ticket_http.TicketApplicationTests.start
    stop = ticket_http.TicketApplicationTests.stop
    request = ticket_http.TicketApplicationTests.request
    api = ticket_http.TicketApplicationTests.api
    login = ticket_http.TicketApplicationTests.login
    create_user = ticket_http.TicketApplicationTests.create_user

    @classmethod
    def setUpClass(cls):
        ticket_http.TicketApplicationTests.setUpClass()
        cls.binary = ticket_http.TicketApplicationTests.binary

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=3)

    def assert_service_available(self):
        started = time.monotonic()
        self.api("GET", "/healthz")
        self.login()
        self.assertLess(time.monotonic() - started, 2.5)

    def test_failed_account_is_throttled_without_blocking_other_accounts(self):
        admin = self.login()
        agent = self.create_user(admin)
        # The same normalized account shares its failure quota across casing.
        for attempt in range(20):
            email = agent["email"].upper() if attempt % 2 else agent["email"]
            self.api("POST", "/api/login", {"email": email, "password": "incorrect"},
                     expected=401)
        self.api("POST", "/api/login", {"email": agent["email"], "password": "incorrect"},
                 expected=429)
        # Forwarded addresses cannot turn an account quota into a global lock.
        self.api("POST", "/api/login", {"email": "admin@example.test",
                 "password": "incorrect"}, expected=401,
                 headers={"X-Forwarded-For": "198.51.100.20"})
        self.login()
        # Another account's success cannot clear the attacked account's quota.
        self.api("POST", "/api/login", {"email": agent["email"],
                 "password": ticket_http.PASSWORD}, expected=429)
        # The failure table is intentionally process-local, like session state.
        self.stop()
        self.start()
        self.login(agent["email"])
        self.api("POST", "/api/login", {"email": agent["email"], "password": "incorrect"},
                 expected=401)

    def test_partial_header_body_and_idle_connections_leave_service_available(self):
        with ExitStack() as sockets:
            header = sockets.enter_context(self.connect())
            header.sendall(b"GET /healthz HTTP/1.1\r\nX-Unfinished: ")
            body = sockets.enter_context(self.connect())
            body.sendall((f"POST /api/login HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
                          "Content-Type: application/json\r\nContent-Length: 100\r\n\r\n{").encode())
            sockets.enter_context(self.connect())
            self.assert_service_available()

    def test_pending_connections_are_bounded_without_locking_out_new_requests(self):
        with ExitStack() as sockets:
            for _ in range(20):
                connection = sockets.enter_context(self.connect())
                connection.sendall(b"GET /healthz HTTP/1.1\r\nX-Unfinished: ")
            self.assert_service_available()

    def test_dripping_headers_and_body_cannot_extend_absolute_deadline(self):
        with ExitStack() as sockets:
            header = sockets.enter_context(self.connect())
            header.sendall(b"GET /healthz HTTP/1.1\r\nX-Unfinished: ")
            body = sockets.enter_context(self.connect())
            body.sendall((f"POST /api/login HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
                          "Content-Type: application/json\r\nContent-Length: 65536\r\n\r\n{").encode())
            idle = sockets.enter_context(self.connect())
            started = time.monotonic()
            self.assert_service_available()
            while time.monotonic() - started < 10.5:
                for connection in (header, body):
                    try:
                        connection.sendall(b" ")
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                time.sleep(0.2)
            for connection in (header, body, idle):
                connection.settimeout(1.5)
                response = b""
                while True:
                    try:
                        part = connection.recv(8192)
                    except ConnectionResetError:
                        break
                    if not part:
                        break
                    response += part
                self.assertTrue(response.startswith(b"HTTP/1.1 408 "), response[:200])
            self.assertLess(time.monotonic() - started, 12.5)
            self.assert_service_available()

    def test_slow_response_reader_leaves_service_available(self):
        self.stop()
        public = Path(self.temporary.name) / "public"
        public.mkdir()
        (public / "large.dat").write_bytes(b"x" * (5 * 1024 * 1024))
        self.environment["TICKETS_PUBLIC_DIR"] = str(public)
        self.start()
        with ExitStack() as sockets:
            responses = []
            connections = []
            for _ in range(9):
                connection = sockets.enter_context(self.connect())
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                connection.sendall((f"GET /large.dat HTTP/1.1\r\n"
                                    f"Host: 127.0.0.1:{self.port}\r\n\r\n").encode())
                response = sockets.enter_context(http.client.HTTPResponse(connection))
                response.begin()
                self.assertEqual(response.status, 200)
                connections.append(connection)
                responses.append(response)
            # Completed responses must survive the peer admission quota, and
            # blocked output sockets must leave health checks and login usable.
            self.assert_service_available()
            connections[0].setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
            self.assertEqual(len(responses[0].read()), 5 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
