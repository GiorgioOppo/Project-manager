"""Local HTTPS certificates and NPM API preparation without live deployment."""

import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("configure_local_sso_proxy", ROOT / "scripts" / "configure_local_sso_proxy.py")
PROXY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROXY)


class FakeNpm:
    def __init__(self):
        self.hosts = []
        self.certificates = []
        self.mutations = []
        self.verifications = []

    def request(self, method, path, payload=None):
        if method == "GET" and path == "/api/nginx/proxy-hosts":
            return self.hosts
        if method == "GET" and path == "/api/nginx/certificates":
            return self.certificates
        if method == "GET" and path.startswith("/api/nginx/proxy-hosts/"):
            return next(host for host in self.hosts if host["id"] == int(path.rsplit("/", 1)[1]))
        if method == "POST" and path == "/api/nginx/certificates":
            certificate = dict(payload, id=7, expires_on="0000-00-00 00:00:00")
            self.certificates.append(certificate)
            self.mutations.append((method, path))
            return certificate
        if method == "POST" and path == "/api/nginx/proxy-hosts":
            host = dict(payload, id=len(self.hosts) + 1, meta={"nginx_online": True})
            self.hosts.append(host)
            self.mutations.append((method, path))
            return host
        raise AssertionError((method, path))

    def upload_certificate(self, certificate_id, certificate, key, ca):
        self.certificates[0]["expires_on"] = "2027-10-11 00:00:00"
        self.certificates[0]["domain_names"] = ["Punto"]
        self.mutations.append(("upload", certificate_id))
        return {"id": certificate_id}

    def verify_public_certificate(self, certificate_id, expected_certificate):
        self.verifications.append((certificate_id, str(expected_certificate)))


class LocalSsoProxyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-local-sso-proxy-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name)

    def test_hosts_use_exact_local_domains_upstreams_and_verified_keycloak_tls(self):
        client = FakeNpm()
        result = PROXY.configure_hosts(client, self.data)
        self.assertEqual(result["certificate_id"], 7)
        self.assertEqual({host["domain_names"][0] for host in client.hosts}, set(PROXY.DOMAINS))
        auth = next(host for host in client.hosts if host["domain_names"] == ["auth.localhost"])
        self.assertEqual(auth["forward_scheme"], "https")
        self.assertEqual(auth["forward_host"], "keycloak")
        self.assertEqual(auth["forward_port"], 8443)
        self.assertIn("proxy_ssl_verify on;", auth["advanced_config"])
        self.assertIn("/etc/punto/keycloak-ca.crt", auth["advanced_config"])
        for host in client.hosts:
            self.assertTrue(host["ssl_forced"])
            self.assertFalse(host["caching_enabled"])

    def test_second_run_reuses_certificate_and_hosts_without_mutations(self):
        client = FakeNpm()
        PROXY.configure_hosts(client, self.data)
        mutations = list(client.mutations)
        result = PROXY.configure_hosts(client, self.data)
        self.assertEqual(client.mutations, mutations)
        self.assertFalse(any(host["created"] for host in result["hosts"]))
        self.assertEqual(client.certificates[0]["domain_names"], ["Punto"])
        self.assertEqual(len(client.verifications), 2)

    def test_existing_display_metadata_cannot_override_a_public_certificate_mismatch(self):
        client = FakeNpm()
        PROXY.configure_hosts(client, self.data)
        mutations = list(client.mutations)
        def reject(*_):
            raise PROXY.ConfigurationError("Mismatched public certificate")
        client.verify_public_certificate = reject
        with self.assertRaises(PROXY.ConfigurationError):
            PROXY.configure_hosts(client, self.data)
        self.assertEqual(client.mutations, mutations)

    def test_incompatible_existing_host_is_not_overwritten_or_partially_changed(self):
        client = FakeNpm()
        client.hosts = [dict(PROXY.host_payload("punto.localhost", 2), id=1, forward_host="different-app")]
        with self.assertRaises(PROXY.ConfigurationError):
            PROXY.configure_hosts(client, self.data)
        self.assertEqual(client.mutations, [])

    def test_existing_host_with_another_certificate_is_not_overwritten(self):
        client = FakeNpm()
        client.hosts = [dict(PROXY.host_payload("punto.localhost", 2), id=1)]
        with self.assertRaises(PROXY.ConfigurationError):
            PROXY.configure_hosts(client, self.data)
        self.assertEqual(client.mutations, [])

    def test_sqlite_backup_is_consistent_private_and_preserves_first_snapshot(self):
        source, destination = self.data / "npm.sqlite", self.data / "before.sqlite"
        with sqlite3.connect(source) as connection:
            connection.execute("CREATE TABLE proxy_host (id INTEGER, domain TEXT)")
            connection.execute("INSERT INTO proxy_host VALUES (1, 'existing.localhost')")
        PROXY.backup_database(source, destination)
        self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
        with sqlite3.connect(destination) as connection:
            self.assertEqual(connection.execute("SELECT domain FROM proxy_host").fetchone(), ("existing.localhost",))
        with sqlite3.connect(source) as connection:
            connection.execute("INSERT INTO proxy_host VALUES (2, 'new.localhost')")
        PROXY.backup_database(source, destination)
        with sqlite3.connect(destination) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM proxy_host").fetchone(), (1,))

    def test_administrator_token_is_captured_without_password_reset_or_value_in_arguments(self):
        token = b"header.payload.signature"
        result = subprocess.CompletedProcess([], 0, stdout=b"SQLite database ready\nPUNTO_NPM_TOKEN:" + token + b"\n", stderr=b"")
        with patch.object(PROXY.subprocess, "run", return_value=result) as run:
            actual = PROXY.administrator_token("projectmanament-proxy-web-1", 1)
        self.assertEqual(actual, token.decode())
        args, kwargs = run.call_args
        self.assertNotIn(token.decode(), " ".join(args[0]))
        script = kwargs["input"].decode()
        self.assertIn("roles.includes('admin')", script)
        self.assertNotIn(".patch(", script)
        self.assertNotIn(".update(", script)
        self.assertIn("expiresIn: '5m'", script)
        self.assertIn("await Token().create(", script)
        self.assertIn("result.token", script)
        self.assertNotIn("new Token()", script)


@unittest.skipUnless(shutil.which("openssl"), "OpenSSL required")
class LocalWebTlsTests(unittest.TestCase):
    def test_generated_leaf_covers_exactly_three_local_hosts_and_is_reused(self):
        with tempfile.TemporaryDirectory(prefix="punto-local-web-tls-tests-") as temporary:
            data = Path(temporary)
            tls = data / "tls"
            tls.mkdir(mode=0o700)
            PROXY.configuration.openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                                        "-nodes", "-days", "3650", "-sha256", "-keyout", tls / "ca.key",
                                        "-out", tls / "ca.crt", "-subj", "/CN=Test CA",
                                        "-addext", "basicConstraints=critical,CA:TRUE",
                                        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
            cert, key = PROXY.prepare_web_tls(data)
            before = (cert.read_bytes(), key.read_bytes())
            self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)
            PROXY.prepare_web_tls(data)
            self.assertEqual(before, (cert.read_bytes(), key.read_bytes()))
            key.unlink()
            with self.assertRaises(PROXY.configuration.ConfigurationError):
                PROXY.prepare_web_tls(data)
            self.assertEqual(cert.read_bytes(), before[0])


if __name__ == "__main__":
    unittest.main()
