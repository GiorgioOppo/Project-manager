"""Opt-in driver TLS regression, independent of Docker and real databases.

Requires a C compiler, pkg-config and MariaDB Connector/C development files.
Run: PUNTO_MYSQL_TLS_TEST=1 python3 tests/test_mysql_tls.py -v

Alternatively compile tests/mysql_tls_driver_check.c with src/db_mysql.c using
-D_POSIX_C_SOURCE=200809L -DPUNTO_MYSQL -Isrc -pthread and the cflags/libs from
pkg-config libmariadb. Run the resulting harness in a container with
--network none if desired: its only socket traffic is on its own loopback.
No environment credentials or production CA files are used.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


@unittest.skipUnless(os.environ.get("PUNTO_MYSQL_TLS_TEST") == "1",
                     "Set PUNTO_MYSQL_TLS_TEST=1 for isolated Connector/C TLS tests")
class MySQLTransportSecurityTests(unittest.TestCase):
    def test_driver_rejects_downgrade_before_authentication(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        self.assertTrue(compiler and shutil.which(compiler[0]), "A C compiler is required")
        self.assertTrue(shutil.which("pkg-config"), "pkg-config is required")
        package = subprocess.run(["pkg-config", "--cflags", "--libs", "libmariadb"],
                                 capture_output=True, text=True, timeout=10)
        self.assertEqual(package.returncode, 0, "MariaDB Connector/C development files are required")
        with tempfile.TemporaryDirectory(prefix="punto-mysql-tls-") as directory:
            binary = Path(directory) / "driver-check"
            command = compiler + [
                "-D_POSIX_C_SOURCE=200809L", "-DPUNTO_MYSQL", "-std=c11",
                "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
                "-I" + str(ROOT / "src"), str(ROOT / "src" / "db_mysql.c"),
                str(ROOT / "tests" / "mysql_tls_driver_check.c"),
                *shlex.split(package.stdout), "-o", str(binary),
            ]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(executed.returncode, 0, executed.stderr)
            self.assertIn("8 TLS rejection cases passed", executed.stdout)


if __name__ == "__main__":
    unittest.main()
