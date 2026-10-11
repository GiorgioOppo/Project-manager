"""Opt-in real Keycloak startup/import test in a disposable Docker project."""

import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get("PUNTO_KEYCLOAK_DOCKER_TESTS") == "1", "isolated Keycloak Docker test opt-in")
class KeycloakRuntimeTests(unittest.TestCase):
    def test_production_start_imports_both_clients_with_tls_and_private_database(self):
        docker = shutil.which("docker")
        if not docker:
            self.skipTest("Docker required")
        spec = importlib.util.spec_from_file_location("keycloak_prepare", ROOT / "scripts/configure_keycloak.py")
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        with tempfile.TemporaryDirectory(prefix="punto-keycloak-smoke-") as directory:
            data = Path(directory) / "keycloak"
            prepare.initialize(data, "https://auth.example.test/realms/punto",
                               "https://punto.example.test", "https://tickets.example.test")
            source = (ROOT / "compose.keycloak.yaml").read_text()
            # Exercise production services without reusing application data.
            source = source.split("\n  app:\n")[0] + "\nvolumes:\n" + source.split("\nvolumes:\n", 1)[1]
            source = source.split("\nnetworks:\n")[0]
            source = re.sub(r"^\s+(?:ipv4_address|interface_name):.*\n", "", source, flags=re.MULTILINE)
            source = source.replace("./auth/keycloak", str(ROOT / "auth/keycloak")).replace("./data/keycloak", str(data))
            source += "\nnetworks:\n  private_app:\n    internal: true\n  auth_api:\n    internal: true\n  auth_database:\n    internal: true\n"
            compose_file = Path(directory) / "compose.yaml"
            compose_file.write_text(source)
            compose = [docker, "compose", "-p", "punto-keycloak-qa-" + uuid4().hex[:10],
                       "--env-file", str(data / "compose.env"), "-f", str(compose_file)]
            try:
                started = subprocess.run(compose + ["up", "-d", "--wait", "--wait-timeout", "180"],
                                         capture_output=True, text=True, timeout=210)
                if started.returncode:
                    logs = subprocess.run(compose + ["logs", "--no-color", "keycloak"],
                                          capture_output=True, text=True, timeout=10)
                    self.fail("Disposable Keycloak failed to become healthy: " + logs.stdout[-6000:] + started.stderr[-2000:])
                # Passwords are read by the official CLI inside the disposable
                # server. Captured client details are never logged.
                certificate = data / "tls/ca.crt"
                subprocess.run(compose + ["cp", str(certificate), "keycloak:/tmp/punto-ca.crt"],
                               check=True, capture_output=True, text=True, timeout=10)
                install = "keytool -importcert -noprompt -alias punto-ca -file /tmp/punto-ca.crt -keystore /tmp/punto-trust.jks -storepass changeit >/dev/null 2>&1"
                subprocess.run(compose + ["exec", "-T", "keycloak", "/bin/bash", "-c", install],
                               check=True, capture_output=True, text=True, timeout=15)
                script = '''set -e
/opt/keycloak/bin/kcadm.sh config credentials --server https://keycloak:8443 --realm master --user "$KC_BOOTSTRAP_ADMIN_USERNAME" --password "$(cat /run/punto-auth/bootstrap-password)" --truststore /tmp/punto-trust.jks --trustpass changeit >/dev/null
/opt/keycloak/bin/kcadm.sh get clients -r punto --fields id,clientId --truststore /tmp/punto-trust.jks --trustpass changeit
'''
                checked = subprocess.run(compose + ["exec", "-T", "keycloak", "/bin/bash", "-c", script],
                                         capture_output=True, text=True, timeout=30)
                self.assertEqual(checked.returncode, 0, checked.stderr)
                clients = {item["clientId"]: item for item in json.loads(checked.stdout)}

                def admin(*arguments, document=None):
                    result = subprocess.run(compose + ["exec", "-T", "keycloak", "/opt/keycloak/bin/kcadm.sh",
                        *arguments, "-r", "punto", "--truststore", "/tmp/punto-trust.jks", "--trustpass", "changeit"],
                        input=json.dumps(document) if document is not None else None,
                        capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    return result.stdout

                # This identity exists only in the disposable project. Evaluating
                # its ID token exercises the imported role scope mappings and
                # protocol mappers without enabling a password grant on clients.
                user_id = admin("create", "users", "-i", "-f", "-", document={
                    "username": "runtime-probe", "email": "runtime-only@example.test", "emailVerified": True,
                    "firstName": "Runtime", "lastName": "Probe", "enabled": True}).strip()
                self.assertRegex(user_id, r"^[0-9a-f-]{36}$")
                for name, origin in (("punto", "https://punto.example.test"), ("punto-tickets", "https://tickets.example.test")):
                    detail = subprocess.run(compose + ["exec", "-T", "keycloak", "/opt/keycloak/bin/kcadm.sh",
                        "get", "clients/" + clients[name]["id"], "-r", "punto",
                        "--truststore", "/tmp/punto-trust.jks", "--trustpass", "changeit"],
                        capture_output=True, text=True, timeout=15)
                    self.assertEqual(detail.returncode, 0, detail.stderr)
                    client = json.loads(detail.stdout)
                    self.assertFalse(client["publicClient"])
                    self.assertTrue(client["standardFlowEnabled"])
                    self.assertFalse(client["directAccessGrantsEnabled"])
                    self.assertFalse(client["fullScopeAllowed"])
                    self.assertEqual(client["redirectUris"], [origin + "/auth/callback"])
                    self.assertEqual(client["attributes"].get("pkce.code.challenge.method"), "S256", client["attributes"])
                    mapper = next(item for item in client["protocolMappers"] if item["name"] == "punto-managed-app-roles")
                    self.assertEqual(mapper["protocolMapper"], "oidc-usermodel-client-role-mapper")
                    self.assertEqual(mapper["config"]["usermodel.clientRoleMapping.clientId"], name)
                    self.assertEqual(mapper["config"]["claim.name"], "app_roles")
                    self.assertEqual(mapper["config"]["multivalued"], "true")
                    self.assertEqual(mapper["config"]["id.token.claim"], "true")
                    client_id = clients[name]["id"]
                    expected_roles = {"manager", "worker"} if name == "punto" else {"admin", "agent"}
                    roles = json.loads(admin("get", "clients/" + client_id + "/roles"))
                    self.assertEqual({role["name"] for role in roles}, expected_roles)
                    scoped_roles = json.loads(admin("get", "clients/" + client_id + "/scope-mappings/clients/" + client_id))
                    self.assertEqual({role["name"] for role in scoped_roles}, expected_roles)
                    scopes = json.loads(admin("get", "clients/" + client_id + "/default-client-scopes"))
                    self.assertTrue({"profile", "email"}.issubset({scope["name"] for scope in scopes}))
                    admin("create", "users/" + user_id + "/role-mappings/clients/" + client_id, "-f", "-", document=roles)
                for name in ("punto", "punto-tickets"):
                    token = json.loads(admin("get", "clients/" + clients[name]["id"] +
                        "/evaluate-scopes/generate-example-id-token", "-q", "userId=" + user_id,
                        "-q", "scope=openid profile email"))
                    self.assertEqual(token["email"], "runtime-only@example.test")
                    self.assertEqual(token["name"], "Runtime Probe")
                    # The realm deliberately uses the email address as username.
                    self.assertEqual(token["preferred_username"], "runtime-only@example.test")
                    self.assertIsInstance(token["app_roles"], list)
                    expected_roles = {"manager", "worker"} if name == "punto" else {"admin", "agent"}
                    self.assertEqual(set(token["app_roles"]), expected_roles)
            finally:
                removed = subprocess.run(compose + ["down", "-v", "--remove-orphans"],
                                         capture_output=True, text=True, timeout=60)
                self.assertEqual(removed.returncode, 0, "Disposable project cleanup failed: " + removed.stderr)
