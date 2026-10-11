"""Startup diagnostics distinguish unavailable MySQL from incompatible metadata.

The classification cases compile the real schema module against a synthetic Db
adapter. An additional Connector/C case uses only a disposable loopback socket;
no deployment credentials, database, container, or migration are touched.
"""
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest


ROOT = Path(__file__).resolve().parent.parent
MOCK = r'''
#include "mysql_schema.h"
#include "improvements.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Db { int unused; };
struct DbStmt { int metadata; int steps; };
static const char *scenario;
static int opens, writes;
static struct Db database;

int db_open_v2(const char *path, Db **out, int flags, const char *vfs) {
    (void)path; (void)flags; (void)vfs;
    ++opens;
    *out = NULL;
    if (!strcmp(scenario,"connection") || (!strcmp(scenario,"prepare_connection") && opens==2)) {
        fputs("MySQL: connessione non riuscita (codice 2013).\n",stderr);
        return DB_ERROR;
    }
    *out = &database;
    return DB_OK;
}
int db_close(Db *db) { (void)db; return DB_OK; }
int db_prepare_v2(Db *db,const char *sql,int bytes,DbStmt **out,const char **tail) {
    (void)db; (void)bytes; (void)tail;
    *out = NULL;
    if (!strcmp(scenario,"unreadable")) return DB_ERROR;
    *out = calloc(1,sizeof(**out));
    if (!*out) return DB_NOMEM;
    (*out)->metadata = !strcmp(sql,"SELECT id,version FROM schema_metadata");
    return DB_OK;
}
int db_step(DbStmt *st) {
    if (!st->steps++) return DB_ROW;
    if (st->metadata && !strcmp(scenario,"multiple") && st->steps==2) return DB_ROW;
    if (st->metadata && !strcmp(scenario,"metadata_fetch")) return DB_ERROR;
    return DB_DONE;
}
DbInt db_column_int64(DbStmt *st,int column) {
    if (st->metadata && column==1) return !strcmp(scenario,"future") ? 15 : 14;
    return 1;
}
int db_column_type(DbStmt *st,int column) { (void)st; (void)column; return DB_INTEGER; }
const unsigned char *db_column_text(DbStmt *st,int column) {
    (void)st; (void)column; return (const unsigned char *)"";
}
int db_bind_text(DbStmt *st,int index,const char *value,int bytes,DbDestructor destroy) {
    (void)st; (void)index; (void)value; (void)bytes; (void)destroy; return DB_OK;
}
int db_finalize(DbStmt *st) { free(st); return DB_OK; }
int db_exec(Db *db,const char *sql,int (*callback)(void *,int,char **,char **),void *argument,char **error) {
    (void)db; (void)sql; (void)callback; (void)argument; (void)error;
    ++writes;
    return !strcmp(scenario,"partial_ddl") && writes==2 ? DB_ERROR : DB_OK;
}
bool improvements_schema(Db *db) { (void)db; return true; }

int main(int argc,char **argv) {
    if (argc!=2) return 2;
    scenario=argv[1];
    bool okay=mysql_schema_preflight();
    if (okay && (!strcmp(scenario,"partial_ddl") || !strcmp(scenario,"prepare_connection")))
        okay=mysql_schema_prepare();
    printf("%d\n%d\n%s\n",okay,writes,mysql_schema_error());
    return 0;
}
'''
REAL = r'''
#include "mysql_schema.h"
#include "improvements.h"
#include <stdio.h>
bool improvements_schema(Db *db) { (void)db; return true; }
int main(void) {
    if (mysql_schema_preflight()) return 2;
    puts(mysql_schema_error());
    return 0;
}
'''


class MySQLPreflightDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shlex.split(os.environ.get("CC", "cc"))
        if not cls.compiler or not shutil.which(cls.compiler[0]) or not shutil.which("pkg-config"):
            raise unittest.SkipTest("C compiler and pkg-config required")
        flags = subprocess.run(["pkg-config", "--cflags", "json-c"], capture_output=True, text=True, timeout=10)
        if flags.returncode:
            raise unittest.SkipTest("json-c development headers required")
        cls.flags = shlex.split(flags.stdout)
        cls.directory = tempfile.TemporaryDirectory(prefix="punto-mysql-preflight-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        source = cls.path / "mock.c"
        source.write_text(MOCK)
        cls.binary = cls.path / "preflight-check"
        command = cls.compiler + ["-DPUNTO_MYSQL", "-D_POSIX_C_SOURCE=200809L", "-std=c11",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I" + str(ROOT / "src"),
            *cls.flags, str(ROOT / "src/mysql_schema.c"), str(source), "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def check_scenario(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        okay, writes, message = result.stdout.split("\n", 2)
        return bool(int(okay)), int(writes), message.strip()

    def test_connection_failure_does_not_claim_a_newer_database(self):
        okay, writes, message = self.check_scenario("connection")
        self.assertFalse(okay); self.assertEqual(writes, 0)
        self.assertIn("Connessione MySQL non disponibile", message)
        self.assertNotIn("versione più recente", message)

    def test_connection_loss_between_check_and_preparation_stays_connection_error(self):
        okay, writes, message = self.check_scenario("prepare_connection")
        self.assertFalse(okay); self.assertEqual(writes, 0)
        self.assertIn("Connessione MySQL non disponibile", message)

    def test_future_version_and_extra_metadata_rows_reject_without_writes(self):
        for scenario in ("future", "multiple"):
            with self.subTest(scenario=scenario):
                okay, writes, message = self.check_scenario(scenario)
                self.assertFalse(okay); self.assertEqual(writes, 0)
                self.assertIn("versione più recente", message)
                self.assertIn("Nessun aggiornamento eseguito", message)

    def test_unreadable_metadata_is_distinct_from_unsupported_version(self):
        for scenario in ("unreadable", "metadata_fetch"):
            with self.subTest(scenario=scenario):
                okay, writes, message = self.check_scenario(scenario)
                self.assertFalse(okay); self.assertEqual(writes, 0)
                self.assertIn("Impossibile leggere la versione", message)
                self.assertNotIn("versione più recente", message)

    def test_ddl_failure_does_not_promise_that_no_updates_were_applied(self):
        okay, writes, message = self.check_scenario("partial_ddl")
        self.assertFalse(okay); self.assertEqual(writes, 2)
        self.assertIn("alcune modifiche preliminari potrebbero essere già applicate", message)
        self.assertNotIn("Nessun aggiornamento eseguito", message)

    def test_supported_version_clears_the_diagnostic(self):
        okay, writes, message = self.check_scenario("supported")
        self.assertTrue(okay); self.assertEqual(writes, 0); self.assertEqual(message, "")

    def test_real_connector_lost_handshake_reports_connection_only(self):
        package = subprocess.run(["pkg-config", "--cflags", "--libs", "libmariadb"],
                                 capture_output=True, text=True, timeout=10)
        if package.returncode:
            self.skipTest("MariaDB Connector/C development files required for real2013 case")
        source = self.path / "real.c"
        source.write_text(REAL)
        binary = self.path / "real-preflight-check"
        command = self.compiler + ["-DPUNTO_MYSQL", "-D_POSIX_C_SOURCE=200809L", "-std=c11",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I" + str(ROOT / "src"),
            *self.flags, str(ROOT / "src/db_mysql.c"), str(ROOT / "src/mysql_schema.c"), str(source),
            *shlex.split(package.stdout), "-o", str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        # The peer closes before greeting/authentication, as a proxy with no
        # writable backend can do. Only synthetic values enter the subprocess.
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(10)
            def disconnect():
                connection, _ = listener.accept()
                connection.close()
            thread = threading.Thread(target=disconnect, daemon=True); thread.start()
            ca = self.path / "synthetic-ca.pem"; ca.write_text("synthetic test trust file\n")
            environment = {key: value for key, value in os.environ.items() if not key.startswith("DB_")}
            environment.update(DB_HOST="127.0.0.1", DB_PORT=str(listener.getsockname()[1]),
                DB_NAME="synthetic", DB_USER="synthetic", DB_PASSWORD="synthetic-only",
                DB_TLS_MODE="verify_identity", DB_SSL_CA=str(ca))
            executed = subprocess.run([str(binary)], env=environment, capture_output=True, text=True, timeout=15)
            thread.join(10)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertIn("codice 2013", executed.stderr)
        self.assertIn("Connessione MySQL non disponibile", executed.stdout)
        self.assertNotIn("versione più recente", executed.stdout)


if __name__ == "__main__":
    unittest.main()
