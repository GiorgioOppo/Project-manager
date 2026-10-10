"""Regression checks for standard proxy ports and unprivileged app listeners."""

from contextlib import contextmanager
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PublicPortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = ROOT / "build" / "sqlite" / "punto"
        if not cls.binary.is_file():
            raise RuntimeError("Missing native SQLite binary: run make DB_BACKEND=sqlite")

    def configuration(self, directory, public_port, listen_port=None):
        if listen_port is None:
            with socket.socket() as reserved:
                reserved.bind(("127.0.0.1", 0))
                listen_port = reserved.getsockname()[1]
        database = Path(directory) / "public-port.db"
        env = dict(os.environ, PORT=str(listen_port), PUBLIC_PORT=str(public_port),
                   BIND_HOST="127.0.0.1", PUBLIC_ORIGINS="", PUBLIC_DIR=str(ROOT / "public"),
                   PUNTO_EMAIL="public-port@punto.local", PUNTO_PASSWORD="Fixture-only!2026",
                   DB_PATH=str(database))
        return env, database, listen_port

    @contextmanager
    def running_server(self, public_port):
        with tempfile.TemporaryDirectory(prefix="punto-public-port-") as directory:
            env, _, port = self.configuration(directory, public_port)
            self.assertGreaterEqual(port, 1024)
            with tempfile.TemporaryFile() as log:
                process = subprocess.Popen([str(self.binary)], cwd=ROOT, env=env,
                                           stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            self.fail("Server rejected valid PUBLIC_PORT=" + str(public_port))
                        try:
                            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                                break
                        except OSError:
                            time.sleep(0.025)
                    else:
                        self.fail("Server did not listen on its unprivileged internal port")
                    yield port
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)

    def assert_standard_public_port_accepted(self, public_port):
        with self.running_server(public_port) as port:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                connection.request("GET", "/healthz", headers={"Host": "localhost:" + str(public_port)})
                response = connection.getresponse()
                payload = json.loads(response.read())
                self.assertEqual(response.status, 200)
                self.assertTrue(payload.get("ok"))
            finally:
                connection.close()

    def test_public_http_port_80_is_accepted_with_unprivileged_listener(self):
        self.assert_standard_public_port_accepted(80)

    def test_public_https_port_443_is_accepted_with_unprivileged_listener(self):
        self.assert_standard_public_port_accepted(443)

    def assert_configuration_rejected(self, public_port, diagnostic, listen_port=None):
        with tempfile.TemporaryDirectory(prefix="punto-invalid-port-") as directory:
            env, database, _ = self.configuration(directory, public_port, listen_port)
            result = subprocess.run([str(self.binary)], cwd=ROOT, env=env, capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(diagnostic, result.stderr)
            self.assertFalse(database.exists(), "Invalid port configuration created application data")

    def test_public_port_zero_is_rejected_before_database_creation(self):
        self.assert_configuration_rejected(0, b"PUBLIC_PORT")

    def test_internal_listener_still_rejects_privileged_ports(self):
        self.assert_configuration_rejected(80, b"PORT deve", listen_port=80)


if __name__ == "__main__":
    unittest.main()
