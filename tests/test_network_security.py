"""Read-only, opt-in checks against the running segmented Docker deployment.

Run: PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
The tests inspect containers, open short-lived connections and execute SELECTs.
They do not stop containers, alter networks or create application fixtures.
The HTTPS allow test uses the first configured exact host in Squid's allowlist,
or PUNTO_NETWORK_TEST_ALLOWED_HOST. An empty allowlist skips only that test.
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
        required = {"app", "mail-addon", "calendar-addon", "nginx-proxy-manager", "squid", "db-proxy", "documents", *NODES}
        if required - cls.containers.keys():
            raise RuntimeError("Required Compose services are not running")
        network_ids = {network["NetworkID"] for item in containers
                       for network in item["NetworkSettings"]["Networks"].values()}
        cls.networks = {}
        for network in cls.inspect("network", *sorted(network_ids)):
            logical_name = (network.get("Labels") or {}).get("com.docker.compose.network")
            if logical_name:
                cls.networks[logical_name] = network
        if {"public", "private_app", "backend_app", "app_egress", "mail_api", "calendar_api", "proxy_wan"} - cls.networks.keys():
            raise RuntimeError("The requested Compose networks are not present")
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
            # Debian app/Squid runtimes and the database images contain bash/coreutils.
            # Probe TCP directly so an HTTP/TLS mismatch cannot look like denial.
            arguments = ("timeout", "4", "bash", "-c",
                         'exec 3<>"/dev/tcp/$1/$2"', "probe", destination, str(port))
        result = self.cluster.run(("exec", "-T", service, *arguments), check=False, timeout=10)
        self.assertNotIn(result.returncode, (126, 127), "Probe tools unavailable in " + service)
        if service in ("app", "mail-addon", "calendar-addon", "squid", "documents", *NODES):
            self.assertIn(result.returncode, (0, 1, 124), "Unexpected TCP probe failure in " + service)
        else:
            self.assertIn(result.returncode, (0, 17), "Unexpected TCP probe failure in " + service)
        return result.returncode == 0

    def proxy_request(self, url, *, service="app"):
        """Use an explicit proxy and inspect both upstream and CONNECT status.

        HTTP errors must come from the policy, not a DNS/network failure. Do not
        use --insecure: the allow-path test must also complete upstream TLS.
        """
        proxy = "http://squid:3128" if service in ("app", "mail-addon", "calendar-addon") else "http://" + self.address("squid", "app_egress") + ":3128"
        result = self.cluster.run(("exec", "-T", service, "curl", "--silent", "--show-error",
            "--connect-timeout", "4", "--max-time", "12", "--proxy", proxy,
            "--noproxy", "", "--output", "/dev/null", "--write-out", "%{http_code}\t%{http_connect}",
            url), check=False, timeout=18)
        self.assertNotIn(result.returncode, (126, 127), "curl unavailable in " + service)
        status = re.fullmatch(rb"(\d{3})\t(\d{3})", result.stdout.strip())
        self.assertIsNotNone(status, "Proxy probe produced no HTTP/CONNECT status")
        return result.returncode, int(status[1]), int(status[2])

    def allowed_proxy_host(self):
        host = os.environ.get("PUNTO_NETWORK_TEST_ALLOWED_HOST", "").strip()
        if not host:
            lines = (ROOT / "docker" / "squid" / "allowed-domains.txt").read_text(encoding="ascii").splitlines()
            hosts = [line.partition("#")[0].strip() for line in lines if line.partition("#")[0].strip()]
            if not hosts:
                self.skipTest("Squid's allowlist is empty; all outbound destinations are intentionally denied")
            host = hosts[0]
        self.assertRegex(host, r"\A[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?\Z",
                         "The allowed probe must be an exact DNS host")
        return host

    def test_01_exact_memberships_internal_networks_and_published_ports(self):
        expected = {
            "nginx-proxy-manager": {"public", "private_app"},
            "app": {"private_app", "backend_app", "app_egress", "mail_api", "calendar_api"},
            "mail-addon": {"mail_api", "app_egress"},
            "calendar-addon": {"calendar_api", "app_egress"},
            "squid": {"app_egress", "proxy_wan"},
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
        self.assertTrue(self.networks["app_egress"]["Internal"])
        self.assertTrue(self.networks["mail_api"]["Internal"])
        self.assertTrue(self.networks["calendar_api"]["Internal"])
        self.assertFalse(self.networks["proxy_wan"]["Internal"])
        for name in ("private_app", "backend_app", "app_egress", "mail_api", "calendar_api"):
            options = self.networks[name].get("Options") or {}
            self.assertEqual(options.get("com.docker.network.bridge.gateway_mode_ipv4"), "isolated", name)

        for service in ("app", "mail-addon", "calendar-addon", "squid", "db-proxy", "documents", *NODES):
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

    def test_03_direct_external_egress_is_only_available_to_nginx_and_squid(self):
        destination = os.environ.get("PUNTO_NETWORK_TEST_EGRESS_IP", "1.1.1.1")
        for service in ("nginx-proxy-manager", "squid"):
            self.assertTrue(self.probe(service, destination, 443),
                            "The external TCP control endpoint is unavailable to " + service +
                            "; egress denial is inconclusive")
        for service in ("app", "mail-addon", "calendar-addon", "db-proxy", "documents", *NODES):
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

    def test_08_app_uses_private_proxy_interface_without_exposing_its_listener(self):
        environment = dict(value.partition("=")[::2] for value in self.containers["app"]["Config"]["Env"])
        for key in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
            self.assertEqual(environment.get(key), "http://squid:3128", key)
        self.assertEqual(environment.get("NO_PROXY"), environment.get("no_proxy"))
        bypass = set((environment.get("NO_PROXY") or "").split(","))
        self.assertTrue({"localhost", "127.0.0.1", "::1", "app", "db-proxy", "db", "db2", "db3", "documents",
                         self.address("app", "private_app"), "mail-addon", "mail-api", "calendar-addon", "calendar-api"} <= bypass,
                        "Local health/database paths are missing from NO_PROXY")
        self.assertNotIn("*", bypass, "NO_PROXY disables all proxy use")
        squid_private = self.address("squid", "app_egress")
        self.assertTrue(self.probe("app", squid_private, 3128), "The app cannot reach Squid")
        self.assertFalse(self.probe("squid", self.address("app", "app_egress"), 8080),
                         "Squid can reach the app HTTP listener on app_egress")
        self.assertFalse(self.probe("squid", self.address("app", "private_app"), 8080),
                         "Squid can reach private_app directly")
        for node in NODES:
            self.assertFalse(self.probe("squid", self.address(node, "backend_app"), 3306),
                             "Squid can reach a database directly: " + node)
        self.assertFalse(self.probe("squid", self.address("documents", "backend_app"), 27017),
                         "Squid can reach MongoDB directly")
        for port in (81, 3000):
            self.assertFalse(self.probe("squid", self.address("nginx-proxy-manager", "private_app"), port),
                             "Squid can reach NPM administration port " + str(port))

    def test_09_proxy_rejects_plaintext_http_and_unapproved_hosts(self):
        code, status, connect = self.proxy_request("http://accounts.google.com/")
        self.assertEqual((code, status, connect), (0, 403, 0),
                         "Squid must explicitly reject plaintext HTTP")
        destinations = (
            "https://punto-egress-denied.invalid/",
            "https://accounts.google.com.punto-egress-denied.invalid/",
            "https://unapproved.accounts.google.com/",
            "https://0x7f000001/",
            "https://2130706433/",
            "https://[::1]/",
            "https://[::ffff:127.0.0.1]/",
            "https://[2606:4700:4700::1111]/",
        )
        for url in destinations:
            with self.subTest(url=url):
                code, status, connect = self.proxy_request(url)
                self.assertNotEqual(code, 0)
                self.assertEqual(connect, 403,
                                 "An unapproved HTTPS destination was not rejected by Squid")

    def test_10_proxy_rejects_ip_literals_private_targets_and_unsafe_connect_ports(self):
        destinations = (
            "https://1.1.1.1/",
            "https://127.0.0.1/",
            "https://169.254.169.254/",
            "https://" + self.address("db", "backend_app") + "/",
            "https://db/",
            "https://accounts.google.com:8443/",
        )
        for url in destinations:
            with self.subTest(url=url):
                code, status, connect = self.proxy_request(url)
                self.assertNotEqual(code, 0)
                self.assertEqual(connect, 403,
                                 "An unsafe target was not explicitly rejected by Squid")

    def test_11_approved_https_completes_tls_and_rejects_non_app_proxy_clients(self):
        host = self.allowed_proxy_host()
        code, status, connect = self.proxy_request("https://" + host + "/")
        self.assertEqual(connect, 200, "Squid denied the configured allowed HTTPS host")
        self.assertEqual(code, 0, "Allowed CONNECT failed to complete upstream TLS/HTTP")
        self.assertGreaterEqual(status, 200)
        self.assertLess(status, 600)
        # Squid is attached to the same private proxy network but is not an
        # authorized application source. Exercise its client ACL with a host
        # known to pass the domain policy, rather than a universally denied URL.
        code, status, connect = self.proxy_request("https://" + host + "/", service="squid")
        self.assertNotEqual(code, 0)
        self.assertEqual(connect, 403, "Squid permits an unauthorized source to use the proxy")

    def test_12_app_resolves_internal_docker_names_without_external_dns(self):
        internal_names = {
            "squid": "app_egress",
            "mail-api": "mail_api",
            "calendar-api": "calendar_api",
            "nginx-proxy-manager": "private_app",
            "db-proxy": "backend_app",
            "documents": "backend_app",
            **{node: "backend_app" for node in NODES},
        }
        for name, network in internal_names.items():
            with self.subTest(name=name):
                result = self.cluster.run(("exec", "-T", "app", "timeout", "8", "getent", "ahostsv4", name),
                                          check=False, timeout=12)
                self.assertEqual(result.returncode, 0, "Docker name resolution failed for " + name)
                addresses = {line.split()[0].decode("ascii") for line in result.stdout.splitlines() if line.split()}
                self.assertEqual(addresses, {self.address({"mail-api": "mail-addon", "calendar-api": "calendar-addon"}.get(name, name), network)},
                                 "Docker DNS returned an unexpected network address for " + name)
        # An internal-only Docker container retains service discovery while its
        # embedded resolver has no external forwarding path. Check behavior,
        # rather than mandating a particular /etc/resolv.conf or dns setting.
        result = self.cluster.run(("exec", "-T", "app", "timeout", "8", "getent", "ahostsv4",
                                   "accounts.google.com"), check=False, timeout=12)
        self.assertEqual(result.returncode, 2,
                         "External DNS resolution is available or the denial check was inconclusive")
        self.assertFalse(result.stdout.strip(), "The app resolved an external address directly")

    def test_13_mail_api_requires_tls_and_service_authentication(self):
        # Read the credential inside the container. Neither Docker inspect nor
        # command output may expose its value to the test runner.
        environment = dict(value.partition("=")[::2]
                           for value in self.containers["app"]["Config"]["Env"])
        self.assertEqual(environment.get("MAIL_API_URL"), "https://mail-api:8090")
        self.assertTrue(environment.get("MAIL_API_TOKEN_FILE"))
        self.assertTrue(environment.get("MAIL_API_CA_FILE"))
        for path, expected_status in (("/healthz", 200), ("/v1/status", 401)):
            result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
                'curl --silent --show-error --noproxy "*" --connect-timeout 3 --max-time 5 '
                '--cacert "$MAIL_API_CA_FILE" --output /dev/null --write-out "%{http_code}" '
                '"$MAIL_API_URL$1"', "mail-api-check", path), check=False, timeout=10)
            self.assertEqual(result.returncode, 0, "The mail API TLS handshake failed")
            self.assertEqual(result.stdout.decode("ascii").strip(), str(expected_status), path)
        result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
            'token=$(cat "$MAIL_API_TOKEN_FILE") || exit 1; '
            'curl --silent --show-error --noproxy "*" --connect-timeout 3 --max-time 5 '
            '--cacert "$MAIL_API_CA_FILE" --header "Authorization: Bearer $token" '
            '--write-out "\\n%{http_code}" "$MAIL_API_URL/v1/status"'), check=False, timeout=10)
        self.assertEqual(result.returncode, 0, "Authenticated mail API TLS request failed")
        payload, status = result.stdout.rsplit(b"\n", 1)
        self.assertEqual(status.strip(), b"200", "The app's service credential was rejected")
        self.assertIs(json.loads(payload).get("smtp_configured"), False,
                      "This deployment has not configured a real SMTP provider")
        # Trusting the CA alone must not bypass certificate hostname validation.
        result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
            'curl --silent --noproxy "*" --connect-timeout 3 --max-time 5 '
            '--cacert "$MAIL_API_CA_FILE" --resolve "$1:8090:$2" '
            '--output /dev/null "https://$1:8090/healthz"', "mail-api-hostname-check",
            "wrong-mail-api.invalid", self.address("mail-addon", "mail_api")),
            check=False, timeout=10)
        self.assertEqual(result.returncode, 60, "The client accepted a mail API certificate for another hostname")

    def test_14_mail_addon_has_no_database_or_public_listener_access(self):
        api_address = self.address("mail-addon", "mail_api")
        self.assertTrue(self.probe("app", api_address, 8090), "The app cannot reach the mail API")
        self.assertFalse(self.probe("app", self.address("mail-addon", "app_egress"), 8090),
                         "The mail API is listening on the proxy interface")
        for service in ("nginx-proxy-manager", "db-proxy", "squid"):
            self.assertFalse(self.probe(service, api_address, 8090),
                             service + " can reach the isolated mail API")
        for node in NODES:
            self.assertFalse(self.probe("mail-addon", self.address(node, "backend_app"), 3306),
                             "The mail add-on can reach a database directly: " + node)
        self.assertFalse(self.probe("mail-addon", self.address("documents", "backend_app"), 27017),
                         "The mail add-on can reach MongoDB")
        self.assertFalse(self.probe("mail-addon", self.address("app", "mail_api"), 8080),
                         "The app listener is exposed on mail_api")
        container = self.containers["mail-addon"]
        self.assertTrue(container["HostConfig"]["ReadonlyRootfs"], "The mail add-on filesystem is writable")
        self.assertNotIn(container["Config"].get("User"), (None, "", "0", "root", "0:0"),
                         "The mail add-on runs as root")

    def test_15_mail_addon_proxy_scope_does_not_inherit_calendar_access(self):
        self.assertTrue(self.probe("mail-addon", self.address("squid", "app_egress"), 3128),
                        "The mail add-on cannot reach Squid")
        for url in ("https://accounts.google.com/", "https://oauth2.googleapis.com/",
                    "https://www.googleapis.com/", "https://smtp-denied.invalid:465/",
                    "https://smtp-denied.invalid:587/", "https://1.1.1.1:465/"):
            with self.subTest(url=url):
                code, _, connect = self.proxy_request(url, service="mail-addon")
                self.assertNotEqual(code, 0)
                self.assertEqual(connect, 403, "The mail add-on bypassed its SMTP-only proxy policy")
        for port in (465, 587):
            code, _, connect = self.proxy_request("https://accounts.google.com:%d/" % port)
            self.assertNotEqual(code, 0)
            self.assertEqual(connect, 403, "The app inherited the mail add-on's SMTP ports")

    def test_16_mail_addon_resolves_only_internal_docker_names(self):
        result = self.cluster.run(("exec", "-T", "mail-addon", "timeout", "8", "getent", "ahostsv4", "squid"),
                                  check=False, timeout=12)
        self.assertEqual(result.returncode, 0, "The mail add-on cannot resolve Squid internally")
        addresses = {line.split()[0].decode("ascii") for line in result.stdout.splitlines() if line.split()}
        self.assertEqual(addresses, {self.address("squid", "app_egress")})
        result = self.cluster.run(("exec", "-T", "mail-addon", "timeout", "8", "getent", "ahostsv4",
                                   "accounts.google.com"), check=False, timeout=12)
        self.assertEqual(result.returncode, 2,
                         "The mail add-on has external DNS or denial verification was inconclusive")
        self.assertFalse(result.stdout.strip(), "The mail add-on resolved an external address directly")


    def test_17_calendar_api_requires_tls_service_authentication_and_keeps_google_disabled(self):
        environment = dict(value.partition("=")[::2]
                           for value in self.containers["app"]["Config"]["Env"])
        self.assertEqual(environment.get("CALENDAR_API_URL"), "https://calendar-api:8091")
        self.assertTrue(environment.get("CALENDAR_API_TOKEN_FILE"))
        self.assertTrue(environment.get("CALENDAR_API_CA_FILE"))
        for path, expected_status in (("/healthz", 200), ("/v1/status", 401)):
            result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
                'curl --silent --show-error --noproxy "*" --connect-timeout 3 --max-time 5 '
                '--cacert "$CALENDAR_API_CA_FILE" --output /dev/null --write-out "%{http_code}" '
                '"$CALENDAR_API_URL$1"', "calendar-api-check", path), check=False, timeout=10)
            self.assertEqual(result.returncode, 0, "The calendar API TLS handshake failed")
            self.assertEqual(result.stdout.decode("ascii").strip(), str(expected_status), path)
        result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
            'token=$(cat "$CALENDAR_API_TOKEN_FILE") || exit 1; '
            'curl --silent --show-error --noproxy "*" --connect-timeout 3 --max-time 5 '
            '--cacert "$CALENDAR_API_CA_FILE" --header "Authorization: Bearer $token" '
            '--write-out "\\n%{http_code}" "$CALENDAR_API_URL/v1/status"'), check=False, timeout=10)
        self.assertEqual(result.returncode, 0, "Authenticated calendar API TLS request failed")
        payload, status = result.stdout.rsplit(b"\n", 1)
        self.assertEqual(status.strip(), b"200", "The app's calendar credential was rejected")
        self.assertIs(json.loads(payload).get("oauth_configured"), False,
                      "This deployment has not configured Google OAuth credentials")
        result = self.cluster.run(("exec", "-T", "app", "sh", "-c",
            'curl --silent --noproxy "*" --connect-timeout 3 --max-time 5 '
            '--cacert "$CALENDAR_API_CA_FILE" --resolve "$1:8091:$2" '
            '--output /dev/null "https://$1:8091/healthz"', "calendar-api-hostname-check",
            "wrong-calendar-api.invalid", self.address("calendar-addon", "calendar_api")),
            check=False, timeout=10)
        self.assertEqual(result.returncode, 60, "The calendar API accepted a certificate for another hostname")

    def test_18_calendar_addon_has_no_database_public_or_mail_api_access(self):
        api_address = self.address("calendar-addon", "calendar_api")
        self.assertEqual(api_address, "172.30.70.2")
        self.assertEqual(self.address("calendar-addon", "app_egress"), "172.30.40.12")
        self.assertEqual(self.address("app", "calendar_api"), "172.30.70.10")
        self.assertTrue(self.probe("app", api_address, 8091), "The app cannot reach the calendar API")
        self.assertFalse(self.probe("app", self.address("calendar-addon", "app_egress"), 8091),
                         "The calendar API listens on its outbound interface")
        for service in ("nginx-proxy-manager", "db-proxy", "squid", "mail-addon"):
            self.assertFalse(self.probe(service, api_address, 8091),
                             service + " can reach the isolated calendar API")
        for node in NODES:
            self.assertFalse(self.probe("calendar-addon", self.address(node, "backend_app"), 3306),
                             "The calendar add-on can reach a database: " + node)
        self.assertFalse(self.probe("calendar-addon", self.address("documents", "backend_app"), 27017),
                         "The calendar add-on can reach MongoDB")
        self.assertFalse(self.probe("calendar-addon", self.address("mail-addon", "mail_api"), 8090),
                         "The calendar add-on can reach the mail API")
        self.assertFalse(self.probe("calendar-addon", self.address("app", "calendar_api"), 8080),
                         "The app listener is exposed on calendar_api")
        container = self.containers["calendar-addon"]
        self.assertTrue(container["HostConfig"]["ReadonlyRootfs"], "The calendar filesystem is writable")
        self.assertEqual(container["Config"].get("User"), "10004:10004")
        environment = dict(value.partition("=")[::2] for value in container["Config"]["Env"])
        self.assertEqual(environment.get("CALENDAR_BIND"), api_address)
        self.assertEqual(environment.get("CALENDAR_ALLOWED_CLIENT"), self.address("app", "calendar_api"))
        self.assertNotIn(environment.get("CALENDAR_TEST_MODE"), ("1", "true"),
                         "Test-only provider overrides are enabled in the deployment")
        self.assertNotIn("GOOGLE_CLIENT_SECRET", environment, "Google secret is stored in environment metadata")
        mounted = {item["Destination"] for item in container.get("Mounts", [])}
        self.assertTrue({"/run/secrets/calendar_api_token", "/run/secrets/calendar_token_key",
                         "/run/secrets/google_client_secret"} <= mounted)
        self.assertNotIn("/run/secrets/mail_api_token", mounted, "The calendar add-on received the mail API credential")
        app_mounts = {item["Destination"] for item in self.containers["app"].get("Mounts", [])}
        self.assertNotIn("/run/secrets/calendar_token_key", app_mounts, "The app received the calendar encryption key")
        self.assertNotIn("/run/secrets/google_client_secret", app_mounts, "The app received the Google OAuth client secret")

    def test_19_calendar_proxy_allows_only_exact_google_https_hosts(self):
        self.assertTrue(self.probe("calendar-addon", self.address("squid", "app_egress"), 3128),
                        "The calendar add-on cannot reach Squid")
        code, status, connect = self.proxy_request("https://" + self.allowed_proxy_host() + "/", service="calendar-addon")
        self.assertEqual(connect, 200, "Squid denied the calendar add-on's approved Google destination")
        self.assertEqual(code, 0, "The calendar add-on did not verify upstream TLS")
        self.assertGreaterEqual(status, 200)
        self.assertLess(status, 600)
        for url in ("https://smtp-denied.invalid:465/", "https://smtp-denied.invalid:587/",
                    "https://accounts.google.com:587/", "https://accounts.google.com:8443/",
                    "https://unapproved.accounts.google.com/", "https://accounts.google.com.attacker.invalid/",
                    "https://1.1.1.1/", "https://169.254.169.254/"):
            with self.subTest(url=url):
                code, _, connect = self.proxy_request(url, service="calendar-addon")
                self.assertNotEqual(code, 0)
                self.assertEqual(connect, 403, "The calendar add-on bypassed its Google-only proxy policy")
        code, status, connect = self.proxy_request("http://accounts.google.com/", service="calendar-addon")
        self.assertEqual((code, status, connect), (0, 403, 0), "Plaintext outbound HTTP was permitted")

    def test_20_calendar_addon_resolves_only_internal_docker_names(self):
        result = self.cluster.run(("exec", "-T", "calendar-addon", "timeout", "8", "getent", "ahostsv4", "squid"),
                                  check=False, timeout=12)
        self.assertEqual(result.returncode, 0, "The calendar add-on cannot resolve Squid internally")
        addresses = {line.split()[0].decode("ascii") for line in result.stdout.splitlines() if line.split()}
        self.assertEqual(addresses, {self.address("squid", "app_egress")})
        result = self.cluster.run(("exec", "-T", "calendar-addon", "timeout", "8", "getent", "ahostsv4",
                                   "accounts.google.com"), check=False, timeout=12)
        self.assertEqual(result.returncode, 2,
                         "The calendar add-on has external DNS or denial verification was inconclusive")
        self.assertFalse(result.stdout.strip(), "The calendar add-on resolved an external address directly")


if __name__ == "__main__":
    unittest.main()
