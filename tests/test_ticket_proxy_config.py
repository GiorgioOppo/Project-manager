"""Ticket proxy provisioning tests: no Docker, external DNS or credentials."""

from contextlib import redirect_stdout
from copy import deepcopy
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from configure_ticket_proxy import ConfigurationError, configure, https_redirect, ticket_origins


class FakeClient:
    def __init__(self, hosts=(), setup=True, online=True):
        self.hosts = deepcopy(list(hosts))
        self.setup = setup
        self.online = online
        self.token = None
        self.calls = []

    def wait_ready(self):
        return {"status": "OK", "setup": self.setup}

    def request(self, method, path, payload=None):
        self.calls.append((method, path, deepcopy(payload)))
        if path == "/api/tokens":
            return {"token": "test-token-never-print"}
        if method == "GET" and path == "/api/nginx/proxy-hosts":
            return deepcopy(self.hosts)
        if method == "POST" and path == "/api/nginx/proxy-hosts":
            host = dict(deepcopy(payload), id=len(self.hosts) + 1, meta={"nginx_online": self.online})
            self.hosts.append(host)
            return deepcopy(host)
        if method == "GET" and path.startswith("/api/nginx/proxy-hosts/"):
            return deepcopy(next(host for host in self.hosts if host["id"] == int(path.rsplit("/", 1)[1])))
        raise AssertionError(f"Unexpected API call: {method} {path}")

    def mutations(self):
        return [call for call in self.calls if call[0] != "GET" and call[1] != "/api/tokens"]


class TicketProxyConfigTests(unittest.TestCase):
    def setUp(self):
        self.origins = ticket_origins("http://ticket.localhost", "https://ticket.tuaazienda.it")

    def configure(self, client):
        output = io.StringIO()
        with redirect_stdout(output):
            configure(client, "test-admin@punto.local", "test-password-never-print", self.origins)
        self.assertNotIn("test-password-never-print", output.getvalue())
        self.assertNotIn("test-token-never-print", output.getvalue())
        return output.getvalue()

    def test_origins_are_normalized_and_default_ports_accepted(self):
        self.assertEqual(ticket_origins(" http://TICKET.localhost:80 ", "https://ticket.example:443"),
                         [{"hostname": "ticket.localhost", "scheme": "http"},
                          {"hostname": "ticket.example", "scheme": "https"}])

    def test_invalid_and_ambiguous_origins_are_rejected(self):
        for origin in ("", "ticket.localhost", "ftp://ticket.localhost", "https://",
                       "http://user:password@ticket.localhost", "http://ticket.localhost/",
                       "https://ticket.localhost/path", "https://ticket.localhost?",
                       "https://ticket.localhost#", "https://ticket.localhost:8083",
                       "https://ticket.localhost:0", "https://ticket.localhost:65536",
                       "https://ticket.localhost:not-a-port", "https://*.example.com",
                       "https://tïcket.example", "https://ticket..example", "https://-ticket.example",
                       "https://ticket-.example", "https://ticket.example.", "https://ticket_example",
                       "http://[::1]", "http://ticket.localhost\n", "http://tick\tet.localhost",
                       "https://" + "a" * 64 + ".example", "http://a.example,http://b.example",
                       "http://ticket.localhost;return 200;"):
            with self.subTest(origin=origin), self.assertRaises(ConfigurationError):
                ticket_origins(origin)
        for additional in ("http://ticket.localhost", "https://ticket.localhost",
                           "http://TICKET.localhost:80", "https://ticket.example,",
                           "https://a.example,https://b.example,https://c.example,https://d.example"):
            with self.subTest(additional=additional), self.assertRaises(ConfigurationError):
                ticket_origins("http://ticket.localhost", additional)

    def test_creates_local_http_and_public_redirect_without_claiming_https_ready(self):
        client = FakeClient()
        output = self.configure(client)
        self.assertEqual(len(client.mutations()), 2)
        local, public = client.hosts
        for host in client.hosts:
            self.assertEqual((host["forward_scheme"], host["forward_host"], host["forward_port"]),
                             ("http", "tickets", 8083))
            self.assertEqual(host["certificate_id"], 0)
            self.assertIs(host["ssl_forced"], False)
        self.assertEqual(local["advanced_config"], "")
        self.assertEqual(public["advanced_config"],
                         "if ($scheme = http) { return 308 https://ticket.tuaazienda.it$request_uri; }")
        self.assertIn("HTTPS in attesa del certificato", output)
        self.assertNotIn("HTTPS in attesa del certificato per ticket.localhost", output)

    def test_repeat_run_does_not_change_existing_hosts(self):
        client = FakeClient()
        self.configure(client)
        original = deepcopy(client.hosts)
        client.calls.clear()
        self.configure(client)
        self.assertEqual(client.mutations(), [])
        self.assertEqual(client.hosts, original)

    def test_existing_https_certificate_and_forced_ssl_are_preserved(self):
        for advanced in ("", https_redirect("ticket.tuaazienda.it")):
            with self.subTest(advanced=advanced):
                client = FakeClient()
                self.configure(client)
                client.hosts[1].update(certificate_id=17, ssl_forced=True, advanced_config=advanced)
                original = deepcopy(client.hosts)
                client.calls.clear()
                output = self.configure(client)
                self.assertEqual(client.mutations(), [])
                self.assertEqual(client.hosts, original)
                self.assertNotIn("HTTPS in attesa", output)

    def test_https_primary_with_local_certificate_and_alias_is_preserved(self):
        self.origins = ticket_origins("https://ticket.localhost", "https://ticket.tuaazienda.it")
        client = FakeClient()
        self.configure(client)
        client.hosts[0].update(certificate_id=5, ssl_forced=True, advanced_config="", forward_host="ticket-app")
        original = deepcopy(client.hosts[0])
        client.hosts.pop()
        client.calls.clear()
        self.configure(client)
        self.assertEqual(client.hosts[0], original)
        self.assertEqual(len(client.mutations()), 1)
        self.assertEqual(client.hosts[1]["domain_names"], ["ticket.tuaazienda.it"])

    def test_conflict_in_later_origin_prevents_all_host_mutations(self):
        client = FakeClient()
        self.configure(client)
        public = client.hosts[1]
        public.update(forward_host="some-other-app")
        client = FakeClient([public])
        with self.assertRaisesRegex(ConfigurationError, "incompatibile"):
            self.configure(client)
        self.assertEqual(client.mutations(), [])

    def test_mismatched_or_offline_existing_hosts_are_not_modified(self):
        base = FakeClient()
        self.configure(base)
        mutations = ({"forward_port": 8080}, {"forward_scheme": "https"}, {"enabled": False},
                     {"locations": [{"path": "/api"}]}, {"access_list_id": 9},
                     {"advanced_config": "return 200;"}, {"meta": {"nginx_online": False}},
                     {"certificate_id": 0, "advanced_config": ""},
                     {"certificate_id": 4, "ssl_forced": False})
        for changes in mutations:
            with self.subTest(changes=changes):
                host = dict(deepcopy(base.hosts[1]), **changes)
                client = FakeClient([host])
                with self.assertRaises(ConfigurationError):
                    self.configure(client)
                self.assertEqual(client.mutations(), [])

    def test_duplicate_existing_domain_is_conflict(self):
        client = FakeClient()
        self.configure(client)
        client.hosts.append(deepcopy(client.hosts[1]))
        client.calls.clear()
        with self.assertRaises(ConfigurationError):
            self.configure(client)
        self.assertEqual(client.mutations(), [])

    def test_unrelated_hosts_are_preserved(self):
        unrelated = {"id": 99, "domain_names": ["projects.example"], "forward_host": "app"}
        client = FakeClient([unrelated])
        self.configure(client)
        self.assertEqual(client.hosts[0], unrelated)
        self.assertEqual(len(client.mutations()), 2)

    def test_uninitialized_manager_does_not_create_admin_or_proxy_hosts(self):
        client = FakeClient(setup=False)
        with self.assertRaises(ConfigurationError):
            self.configure(client)
        self.assertEqual(client.calls, [])

    def test_inactive_created_host_stops_before_creating_next_host(self):
        client = FakeClient(online=False)
        with self.assertRaisesRegex(ConfigurationError, "non verificato attivo"):
            self.configure(client)
        self.assertEqual(len(client.mutations()), 1)


if __name__ == "__main__":
    unittest.main()
