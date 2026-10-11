"""Offline preparation, privilege boundaries and optional Keycloak deployment config."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("configure_keycloak", ROOT / "scripts" / "configure_keycloak.py")
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)
ISSUER = "https://auth.example.test/realms/punto"
PUNTO_ORIGIN = "https://punto.example.test"
TICKETS_ORIGIN = "https://tickets.example.test"


class KeycloakConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("openssl"):
            raise unittest.SkipTest("OpenSSL required for TLS preparation")
        cls.fixture = tempfile.TemporaryDirectory(prefix="punto-keycloak-fixture-")
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.prepared = Path(cls.fixture.name) / "keycloak"
        CONFIG.initialize(cls.prepared, ISSUER, PUNTO_ORIGIN, TICKETS_ORIGIN)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-keycloak-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name) / "keycloak"
        shutil.copytree(self.prepared, self.data)

    def initialize(self, **kwargs):
        return CONFIG.initialize(self.data, kwargs.get("issuer", ISSUER),
                                 kwargs.get("punto_origin", PUNTO_ORIGIN),
                                 kwargs.get("tickets_origin", TICKETS_ORIGIN))

    def test_secrets_and_private_material_are_owner_only(self):
        for path in self.data.rglob("*"):
            if path.is_dir():
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700, str(path))
            elif path.suffix != ".crt":
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600, str(path))
        values = [(self.data / "secrets" / name).read_text().strip()
                  for name in ("database-password", "bootstrap-password", "punto-client-secret", "tickets-client-secret")]
        self.assertEqual(len(set(values)), 4)
        environment = (self.data / "compose.env").read_text()
        for value in values:
            self.assertNotIn(value, environment)

    def test_second_init_preserves_secrets_tls_and_operator_environment(self):
        environment = self.data / "compose.env"
        environment.write_text(environment.read_text().replace("KEYCLOAK_AUTH_MODE=hybrid", "KEYCLOAK_AUTH_MODE=oidc"))
        before = {str(path.relative_to(self.data)): (path.read_bytes(), path.stat().st_mtime_ns)
                  for path in self.data.rglob("*") if path.is_file()}
        self.initialize()
        after = {str(path.relative_to(self.data)): (path.read_bytes(), path.stat().st_mtime_ns)
                 for path in self.data.rglob("*") if path.is_file()}
        self.assertEqual(before, after)

    def test_realm_has_separate_confidential_pkce_clients_without_users(self):
        realm = json.loads((self.data / "realm" / "punto-realm.json").read_text())
        self.assertEqual(realm["realm"], "punto")
        self.assertEqual(realm["users"], [])
        self.assertFalse(realm["registrationAllowed"])
        self.assertFalse(realm["duplicateEmailsAllowed"])
        self.assertTrue(realm["loginWithEmailAllowed"])
        self.assertEqual(realm["sslRequired"], "all")
        clients = {client["clientId"]: client for client in realm["clients"]}
        self.assertEqual(set(clients), {"punto", "punto-tickets"})
        for app, identifier, origin in (("punto", "punto", PUNTO_ORIGIN), ("tickets", "punto-tickets", TICKETS_ORIGIN)):
            client = clients[identifier]
            self.assertTrue(client["enabled"])
            self.assertFalse(client["publicClient"])
            self.assertEqual(client["clientAuthenticatorType"], "client-secret")
            self.assertTrue(client["standardFlowEnabled"])
            self.assertFalse(client["implicitFlowEnabled"])
            self.assertFalse(client["directAccessGrantsEnabled"])
            self.assertFalse(client["serviceAccountsEnabled"])
            self.assertEqual(client["attributes"]["pkce.code.challenge.method"], "S256")
            self.assertEqual(client["attributes"]["id.token.signed.response.alg"], "RS256")
            self.assertEqual(client["redirectUris"], [origin + "/auth/callback"])
            self.assertEqual(client["webOrigins"], [origin])
            self.assertEqual(client["secret"], (self.data / "secrets" / f"{app}-client-secret").read_text().strip())
        self.assertNotEqual(clients["punto"]["secret"], clients["punto-tickets"]["secret"])

    def test_missing_tickets_origin_disables_that_client_and_has_no_wildcard(self):
        configuration = dict(issuer=ISSUER, punto_origin=PUNTO_ORIGIN, tickets_origin=None)
        realm = CONFIG.make_realm(configuration, {"punto": "a", "tickets": "b"})
        tickets = next(client for client in realm["clients"] if client["clientId"] == "punto-tickets")
        self.assertFalse(tickets["enabled"])
        self.assertEqual(tickets["redirectUris"], [])
        self.assertEqual(tickets["webOrigins"], [])

    def test_invalid_urls_are_rejected_before_creating_material(self):
        for value in ("http://auth.example.test/realms/punto", "https://user:pass@auth.example.test/realms/punto",
                      ISSUER + "/", ISSUER + "?x=1", ISSUER + "#anchor", ISSUER + "?", ISSUER + "#",
                      "https://*.example.test/realms/punto", "https://auth.example.test/realms/another",
                      "https://auth.example.test:0/realms/punto", "https://auth.example.test:65536/realms/punto",
                      "https://auth.example.test\n/realms/punto", "https://auth.example.test%2f@evil.test/realms/punto"):
            data = Path(self.temporary.name) / "invalid"
            with self.subTest(value=value), self.assertRaises(CONFIG.ConfigurationError):
                CONFIG.initialize(data, value, PUNTO_ORIGIN)
            self.assertFalse(data.exists())
        for origin in (PUNTO_ORIGIN + "/", PUNTO_ORIGIN + "/app", "https://punto.example.test?",
                       "http://punto.example.test", "https://punto.example.test\\evil"):
            with self.subTest(origin=origin), self.assertRaises(CONFIG.ConfigurationError):
                CONFIG.validate_url(origin)

    def test_urls_canonicalize_https_default_port_and_reject_origin_collisions(self):
        self.assertEqual(CONFIG.validate_url("https://PUNTO.example.test:443"), PUNTO_ORIGIN)
        self.assertEqual(CONFIG.validate_url("https://[::1]:8443"), "https://[::1]:8443")
        with self.assertRaises(CONFIG.ConfigurationError):
            self.initialize(tickets_origin=PUNTO_ORIGIN + ":443")

    def test_reinitializing_with_different_urls_leaves_files_intact(self):
        before = (self.data / "compose.env").read_bytes()
        with self.assertRaises(CONFIG.ConfigurationError):
            self.initialize(punto_origin="https://another.example.test")
        self.assertEqual((self.data / "compose.env").read_bytes(), before)

    def test_bindings_are_explicit_per_app_and_reassignment_requires_removal(self):
        for app in CONFIG.APPS:
            self.assertEqual(json.loads((self.data / "bindings" / f"{app}.json").read_text()), {})
        with CONFIG.locked(self.data):
            path = CONFIG.bind_account(self.data, "punto", "subject-001", "Admin@punto.local")
        self.assertEqual(json.loads(path.read_text()), {"subject-001": "admin@punto.local"})
        self.assertEqual(json.loads((self.data / "bindings" / "tickets.json").read_text()), {})
        with self.assertRaises(CONFIG.ConfigurationError):
            CONFIG.bind_account(self.data, "punto", "subject-001", "different@punto.local")
        CONFIG.bind_account(self.data, "punto", "subject-001", None, remove=True)
        self.assertEqual(json.loads(path.read_text()), {})
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_invalid_bindings_and_existing_symlinks_are_rejected(self):
        for subject, email in (("", "a@test"), ("bad\nsubject", "a@test"), ("sub", "no-email"),
                               ("sub", "a@b\nINJECT=1"), ("sub", "a@b\x7f"), ("sub", "a..b@test"),
                               ("sub", "a@bad_domain"), ("sub", "a@test..example"), ("sub", "a@-test")):
            with self.subTest(subject=subject, email=email), self.assertRaises(CONFIG.ConfigurationError):
                CONFIG.bind_account(self.data, "punto", subject, email)
        path = self.data / "bindings" / "punto.json"
        original = Path(self.temporary.name) / "keep.json"
        original.write_text("{}\n")
        path.unlink()
        path.symlink_to(original)
        with self.assertRaises(CONFIG.ConfigurationError):
            CONFIG.bind_account(self.data, "punto", "subject", "a@test")
        self.assertEqual(original.read_text(), "{}\n")

    def test_duplicate_subjects_and_oversized_maps_are_rejected(self):
        path = self.data / "bindings" / "punto.json"
        path.write_text('{"subject": "first@example.test", "subject": "second@example.test"}')
        with self.assertRaises(CONFIG.ConfigurationError):
            CONFIG.load_bindings(path)
        path.write_text(json.dumps({f"subject-{index}": "first@example.test" for index in range(1025)}))
        with self.assertRaises(CONFIG.ConfigurationError):
            CONFIG.load_bindings(path)

    def test_incomplete_tls_bundle_is_not_silently_rotated(self):
        before = (self.data / "tls" / "ca.key").read_bytes()
        (self.data / "tls" / "keycloak.key").unlink()
        with self.assertRaises(CONFIG.ConfigurationError):
            self.initialize()
        self.assertEqual((self.data / "tls" / "ca.key").read_bytes(), before)
        self.assertFalse((self.data / "tls" / "keycloak.key").exists())

    def test_cli_prints_paths_without_secret_values(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts" / "configure_keycloak.py"),
                                 "--data-dir", str(self.data), "init", "--issuer", ISSUER,
                                 "--punto-origin", PUNTO_ORIGIN, "--tickets-origin", TICKETS_ORIGIN],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.data / "secrets" / "bootstrap-password"), result.stdout)
        for path in (self.data / "secrets").iterdir():
            self.assertNotIn(path.read_text().strip(), result.stdout + result.stderr)


class KeycloakComposeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("docker"):
            raise unittest.SkipTest("Docker CLI needed for offline Compose validation")
        check = subprocess.run(["docker", "compose", "version"], capture_output=True, timeout=15)
        if check.returncode:
            raise unittest.SkipTest("Docker Compose unavailable")

    def config(self, tickets=False):
        env = dict(os.environ,
                   MYSQL_PASSWORD="test-mysql-only", MYSQL_ROOT_PASSWORD="test-root-only",
                   CLUSTER_MONITOR_PASSWORD="test-monitor-only", DOC_DB_ROOT_PASSWORD="test-doc-root-only",
                   DOC_DB_PASSWORD="test-doc-only", PUNTO_EMAIL="test@example.test", PUNTO_PASSWORD="test-password-only",
                   KEYCLOAK_PUBLIC_ORIGIN="https://auth.example.test", KEYCLOAK_ISSUER_URL=ISSUER,
                   KEYCLOAK_PUNTO_REDIRECT_URI=PUNTO_ORIGIN + "/auth/callback",
                   KEYCLOAK_TICKETS_REDIRECT_URI=TICKETS_ORIGIN + "/auth/callback",
                   TICKETS_EMAIL="tickets@example.test", TICKETS_PASSWORD="test-tickets-only",
                   TICKETS_PUBLIC_ORIGIN=TICKETS_ORIGIN, KEYCLOAK_AUTH_MODE="hybrid", KEYCLOAK_TICKETS_AUTH_MODE="hybrid")
        command = ["docker", "compose", "--env-file", "/dev/null", "-f", "compose.yaml"]
        if tickets:
            command += ["-f", "compose.tickets.yaml"]
        command += ["-f", "compose.keycloak.yaml"]
        if tickets:
            command += ["-f", "compose.tickets-keycloak.yaml"]
        result = subprocess.run(command + ["config", "--format", "json"], cwd=ROOT, env=env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_optional_overlay_keeps_keycloak_and_database_private(self):
        config = self.config()
        services = config["services"]
        self.assertNotIn("tickets", services)
        self.assertEqual(services["keycloak"]["image"], "quay.io/keycloak/keycloak:26.8.0")
        self.assertEqual(services["keycloak"]["command"], ["start", "--import-realm"])
        self.assertEqual(services["keycloak-db"]["image"], "postgres:17.11-bookworm")
        for name in ("keycloak", "keycloak-db", "auth-config-init"):
            self.assertFalse(services[name].get("ports"), name)
        self.assertEqual(set(services["keycloak-db"]["networks"]), {"auth_database"})
        self.assertNotIn("auth_database", services["app"]["networks"])
        for name in ("auth_api", "auth_database"):
            self.assertTrue(config["networks"][name]["internal"])
        self.assertEqual(services["app"]["environment"]["BIND_HOST"], "172.30.20.10")
        self.assertEqual(services["keycloak"]["environment"]["KC_HTTP_MANAGEMENT_HOST"], "127.0.0.1")
        self.assertEqual(services["keycloak"]["networks"]["private_app"]["ipv4_address"], "172.30.20.12")
        self.assertNotIn("KC_DB_PASSWORD", services["keycloak"]["environment"])

    def test_ticket_overlay_uses_distinct_credentials_and_shared_issuer(self):
        services = self.config(tickets=True)["services"]
        punto, tickets = (services[name]["environment"] for name in ("app", "tickets"))
        self.assertEqual(punto["OIDC_CLIENT_ID"], "punto")
        self.assertEqual(tickets["OIDC_CLIENT_ID"], "punto-tickets")
        self.assertEqual(punto["OIDC_ISSUER_URL"], tickets["OIDC_ISSUER_URL"])
        self.assertEqual(tickets["OIDC_BACKCHANNEL_URL"], "https://keycloak:8443/realms/punto")
        self.assertEqual(tickets["OIDC_REDIRECT_URI"], TICKETS_ORIGIN + "/auth/callback")
        volumes = {name: {volume["source"] for volume in services[name]["volumes"]}
                   for name in ("app", "tickets")}
        self.assertIn("punto_auth_runtime", volumes["app"])
        self.assertNotIn("tickets_auth_runtime", volumes["app"])
        self.assertIn("tickets_auth_runtime", volumes["tickets"])
        self.assertNotIn("punto_auth_runtime", volumes["tickets"])


if __name__ == "__main__":
    unittest.main()
