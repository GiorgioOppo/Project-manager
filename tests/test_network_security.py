"""Read-only, opt-in checks against the running segmented Docker deployment.

Run: PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
The tests inspect containers, open short-lived connections and execute SELECTs.
They do not stop containers, alter networks or create application fixtures.
"""

import http.client
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    from manage_cluster import Cluster, NODES, sql_literal
finally:
    sys.path.pop(0)


@unittest.skipUnless(os.environ.get("PUNTO_NETWORK_TEST") == "1",
                     "Set PUNTO_NETWORK_TEST=1 to inspect the running Docker deployment")
class NetworkSecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        context = os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else "")
        cls.cluster = Cluster(Path(os.environ.get("PUNTO_CLUSTER_ENV_FILE", ROOT / ".env.docker")), context)
        cls.docker_command = cls.cluster.command[:cls.cluster.command.index("compose")]
        ids = cls.cluster.run(("ps", "-q"), timeout=20).stdout.decode().split()
        if not ids:
            raise RuntimeError("No running Compose containers were found")
        containers = cls.inspect("container", *ids)
        cls.containers = {
            item["Config"]["Labels"]["com.docker.compose.service"]: item for item in containers
        }
        required = {"app", "nginx-proxy-manager", "db-proxy", "documents", *NODES}
        if required - cls.containers.keys():
            raise RuntimeError("Required Compose services are not running")
        network_ids = {network["NetworkID"] for item in containers
                       for network in item["NetworkSettings"]["Networks"].values()}
        cls.networks = {}
        for network in cls.inspect("network", *sorted(network_ids)):
            logical_name = (network.get("Labels") or {}).get("com.docker.compose.network")
            if logical_name:
                cls.networks[logical_name] = network
        if {"public", "private_app", "backend_app"} - cls.networks.keys():
            raise RuntimeError("The three requested Compose networks are not present")
        cls.http_port = int(cls.cluster.values.get("NPM_HTTP_PORT", "80"))

    @classmethod
    def inspect(cls, kind, *identifiers):
        result = subprocess.run(cls.docker_command + [kind, "inspect", *identifiers],
                                env=cls.cluster.environment, cwd=ROOT, capture_output=True, timeout=20)
        if result.returncode:
            # Docker inspection can include environment secrets. Never echo its payload.
            raise RuntimeError("Docker inspection failed")
        return json.loads(result.stdout)

    def address(self, service, network):
        actual_name = self.networks[network]["Name"]
        value = self.containers[service]["NetworkSettings"]["Networks"][actual_name]["IPAddress"]
        self.assertTrue(value, service + " has no IPv4 address on " + network)
        return value

    def probe(self, service, destination, port):
        """Return TCP reachability; distinguish connection denial from missing tools."""
        if service == "nginx-proxy-manager":
            arguments = ("node", "-e",
                "const net=require('net');let done=false;"
                "function end(code){if(!done){done=true;socket.destroy();process.exit(code);}}"
                "const socket=net.createConnection({host:process.argv[1],port:Number(process.argv[2])});"
                "socket.setTimeout(2500);socket.on('connect',()=>end(0));"
                "socket.on('timeout',()=>end(17));socket.on('error',()=>end(17));",
                destination, str(port))
        elif service == "db-proxy":
            arguments = ("python3", "-c",
                "import socket,sys\n"
                "try:\n s=socket.create_connection((sys.argv[1],int(sys.argv[2])),2.5);s.close()\n"
                "except OSError:\n sys.exit(17)\n", destination, str(port))
        else:
            # Both the Debian app runtime and the MySQL image contain bash/coreutils.
            # Probe TCP directly so an HTTP/TLS mismatch cannot look like denial.
            arguments = ("timeout", "4", "bash", "-c",
                         'exec 3<>"/dev/tcp/$1/$2"', "probe", destination, str(port))
        result = self.cluster.run(("exec", "-T", service, *arguments), check=False, timeout=10)
        self.assertNotIn(result.returncode, (126, 127), "Probe tools unavailable in " + service)
        if service in ("app", "documents", *NODES):
            self.assertIn(result.returncode, (0, 1, 124), "Unexpected TCP probe failure in " + service)
        else:
            self.assertIn(result.returncode, (0, 17), "Unexpected TCP probe failure in " + service)
        return result.returncode == 0

    def test_01_exact_memberships_internal_networks_and_published_ports(self):
        expected = {
            "nginx-proxy-manager": {"public", "private_app"},
            "app": {"private_app", "backend_app"},
            "db-proxy": {"backend_app"},
            "documents": {"backend_app"},
            **{node: {"backend_app"} for node in NODES},
        }
        for service, logical_networks in expected.items():
            actual = set(self.containers[service]["NetworkSettings"]["Networks"])
            wanted = {self.networks[name]["Name"] for name in logical_networks}
            self.assertEqual(actual, wanted, service + " has unexpected network attachments")
            interfaces = self.cluster.run(("exec", "-T", service, "ls", "-1", "/sys/class/net"), timeout=10)
            # Docker Desktop's kernel can expose inactive tunnel templates here;
            # validate the configured Docker links by name and actual MAC.
            self.assertTrue(logical_networks | {"lo"} <= set(interfaces.stdout.decode().split()),
                            service + " is missing a configured interface name")
            for logical in logical_networks:
                address = self.cluster.run(("exec", "-T", service, "cat",
                                            "/sys/class/net/" + logical + "/address"), timeout=10)
                expected_mac = self.containers[service]["NetworkSettings"]["Networks"][self.networks[logical]["Name"]]["MacAddress"]
                self.assertEqual(address.stdout.decode().strip(), expected_mac,
                                 service + " has a mismatched Docker interface: " + logical)
            self.assertEqual(self.containers[service]["State"]["Status"], "running", service)
            health = self.containers[service]["State"].get("Health")
            if health:
                self.assertEqual(health["Status"], "healthy", service)
        self.assertFalse(self.networks["public"]["Internal"])
        self.assertTrue(self.networks["private_app"]["Internal"])
        self.assertTrue(self.networks["backend_app"]["Internal"])
        for name in ("private_app", "backend_app"):
            options = self.networks[name].get("Options") or {}
            self.assertEqual(options.get("com.docker.network.bridge.gateway_mode_ipv4"), "isolated", name)

        for service in ("app", "db-proxy", "documents", *NODES):
            bindings = self.containers[service]["HostConfig"].get("PortBindings") or {}
            self.assertFalse(any(bindings.values()), service + " publishes a host port")
        npm_ports = self.containers["nginx-proxy-manager"]["HostConfig"].get("PortBindings") or {}
        self.assertEqual(set(npm_ports), {"80/tcp", "443/tcp", "81/tcp"})
        expected_ports = {"80/tcp": self.http_port,
                          "443/tcp": int(self.cluster.values.get("NPM_HTTPS_PORT", "443")),
                          "81/tcp": int(self.cluster.values.get("NPM_ADMIN_PORT", "81"))}
        for container_port, host_port in expected_ports.items():
            self.assertTrue(npm_ports[container_port])
            for binding in npm_ports[container_port]:
                self.assertEqual(int(binding["HostPort"]), host_port)
                if container_port == "81/tcp":
                    self.assertEqual(binding["HostIp"], "127.0.0.1", "NPM administration is publicly exposed")

    def test_02_allowed_proxy_path_and_denied_cross_network_paths(self):
        app_private = self.address("app", "private_app")
        app_backend = self.address("app", "backend_app")
        self.assertTrue(self.probe("nginx-proxy-manager", app_private, 8080),
                        "NPM cannot reach the app on private_app")
        self.assertFalse(self.probe("db-proxy", app_backend, 8080),
                         "The app HTTP listener is reachable from backend_app")
        for node in NODES:
            self.assertFalse(self.probe("nginx-proxy-manager", self.address(node, "backend_app"), 3306),
                             "NPM can reach a database directly: " + node)
            self.assertTrue(self.probe("db-proxy", self.address(node, "backend_app"), 3306),
                            "The DB proxy cannot reach " + node)
        document_address = self.address("documents", "backend_app")
        self.assertFalse(self.probe("nginx-proxy-manager", document_address, 27017),
                         "NPM can reach MongoDB directly")
        self.assertTrue(self.probe("app", document_address, 27017),
                        "The app cannot reach MongoDB")

        connection = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=10)
        try:
            connection.request("GET", "/healthz", headers={"Host": "localhost"})
            response = connection.getresponse()
            response.read()
            self.assertEqual(response.status, 200, "Application health through NPM failed")
        finally:
            connection.close()

    def test_03_external_egress_is_only_available_to_nginx(self):
        destination = os.environ.get("PUNTO_NETWORK_TEST_EGRESS_IP", "1.1.1.1")
        self.assertTrue(self.probe("nginx-proxy-manager", destination, 443),
                        "The external TCP control endpoint is unavailable; egress denial is inconclusive")
        for service in ("app", "db-proxy", "documents", *NODES):
            self.assertFalse(self.probe(service, destination, 443),
                             service + " has unexpected public egress")

    def test_04_database_requires_tls_and_observed_app_sessions_use_tls(self):
        app_user = sql_literal(self.cluster.values.get("MYSQL_USER", "punto"))
        sessions = 0
        for node in NODES:
            self.assertEqual(self.cluster.sql(node, "SELECT @@require_secure_transport;"), "1", node)
            plaintext = self.cluster.run(("exec", "-T", node, "sh", "-c",
                'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --protocol=TCP --host=127.0.0.1 '
                # Let caching_sha2_password finish its RSA exchange so the test
                # observes the server's transport policy, not client error 2061.
                '--user=root --ssl-mode=DISABLED --get-server-public-key --batch --skip-column-names'),
                data=b"SELECT 1;", check=False, timeout=10)
            self.assertNotEqual(plaintext.returncode, 0, "MySQL accepted an unencrypted TCP connection on " + node)
            self.assertIsNotNone(re.search(rb"ERROR 3159\b", plaintext.stderr),
                                 "MySQL did not reject plaintext specifically for insecure transport on " + node)
            row = self.cluster.sql(node,
                "SELECT COUNT(*),COALESCE(SUM(COALESCE(s.VARIABLE_VALUE,'')<>''),0) "
                "FROM performance_schema.threads t LEFT JOIN performance_schema.status_by_thread s "
                "ON s.THREAD_ID=t.THREAD_ID AND s.VARIABLE_NAME='Ssl_cipher' "
                "WHERE t.TYPE='FOREGROUND' AND t.PROCESSLIST_USER=" + app_user + ";")
            total, encrypted = map(int, row.split("\t"))
            self.assertEqual(total, encrypted, "An observed application-account session is unencrypted on " + node)
            sessions += total
        self.assertGreater(sessions, 0, "No application-account sessions were observed; TLS verification is inconclusive")

    def test_05_containers_cannot_forward_packets_or_change_network_policy(self):
        for service, container in self.containers.items():
            config = container["HostConfig"]
            drops = {value.removeprefix("CAP_") for value in (config.get("CapDrop") or [])}
            additions = {value.removeprefix("CAP_") for value in (config.get("CapAdd") or [])}
            self.assertTrue("ALL" in drops or {"NET_RAW", "NET_ADMIN"} <= drops,
                            service + " retains network administration or raw socket capabilities")
            self.assertFalse({"ALL", "NET_ADMIN", "SYS_ADMIN"} & additions,
                             service + " adds a privileged administration capability")
            self.assertEqual((config.get("Sysctls") or {}).get("net.ipv4.ip_forward"), "0", service)
            self.assertEqual((config.get("Sysctls") or {}).get("net.ipv6.conf.all.forwarding"), "0", service)
            self.assertFalse(config.get("Privileged"), service + " runs privileged")
        app_environment = dict(value.partition("=")[::2] for value in self.containers["app"]["Config"]["Env"])
        self.assertEqual(app_environment.get("BIND_HOST"), self.address("app", "private_app"),
                         "The application listener is not bound to private_app")

    def test_06_app_cannot_reach_nginx_administration_but_host_loopback_can(self):
        npm_private = self.address("nginx-proxy-manager", "private_app")
        for port in (81, 3000):
            self.assertFalse(self.probe("app", npm_private, port),
                             "The app can reach NPM administration port " + str(port))
        admin_port = int(self.cluster.values.get("NPM_ADMIN_PORT", "81"))
        connection = http.client.HTTPConnection("127.0.0.1", admin_port, timeout=10)
        try:
            connection.request("GET", "/", headers={"Host": "localhost"})
            response = connection.getresponse()
            response.read()
            self.assertEqual(response.status, 200, "NPM administration is unavailable through host loopback")
        finally:
            connection.close()

    def test_07_mongodb_requires_tls_and_authenticates_scoped_application_user(self):
        result = self.cluster.run(("exec", "-T", "documents", "mongosh", "--quiet",
            "--host", "documents", "--tls", "--tlsCAFile", "/etc/mongo/tls/ca.crt",
            "--eval", "const d=db.getSiblingDB(process.env.DOC_DB_NAME);"
            "d.auth(process.env.DOC_DB_USER,process.env.DOC_DB_PASSWORD);"
            "d.getCollectionNames();quit(0);"), check=False, timeout=15)
        self.assertEqual(result.returncode, 0, "MongoDB TLS and application authentication failed")
        plaintext = self.cluster.run(("exec", "-T", "documents", "mongosh", "--quiet",
            "mongodb://documents:27017/?serverSelectionTimeoutMS=2000&tls=false",
            "--eval", "db.runCommand({ping:1});"), check=False, timeout=10)
        self.assertNotEqual(plaintext.returncode, 0, "MongoDB accepted plaintext transport")


if __name__ == "__main__":
    unittest.main()
