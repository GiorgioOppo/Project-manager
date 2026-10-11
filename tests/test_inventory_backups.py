"""Live SQLite backups, durable scheduling, retention and safe recovery."""

from contextlib import closing
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import unittest

import test_inventory_addon as fixtures

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "inventory_backup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    sqlite3 *database = NULL;
    if (argc != 2 || sqlite3_open(argv[1], &database) != SQLITE_OK) return 2;
    inventory_backup_open(database);
    char command[2048];
    while (fgets(command, sizeof(command), stdin)) {
        if (!strncmp(command, "tick ", 5)) inventory_backup_tick((time_t)strtoll(command + 5, NULL, 10));
        else if (!strncmp(command, "exec ", 5)) {
            if (sqlite3_exec(database, command + 5, NULL, NULL, NULL) != SQLITE_OK) return 3;
        } else if (!strcmp(command, "reopen\n")) inventory_backup_open(database);
        else if (!strcmp(command, "stop\n")) break;
        json_object *status = inventory_backup_status();
        puts(json_object_to_json_string_ext(status, JSON_C_TO_STRING_PLAIN));
        fflush(stdout);
        json_object_put(status);
    }
    inventory_backup_close();
    sqlite3_close(database);
    return 0;
}
'''


class InventoryBackupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc") or not shutil.which("pkg-config"):
            raise unittest.SkipTest("C compiler and SQLite/json-c dependencies are required")
        cls.compilation = tempfile.TemporaryDirectory(prefix="punto-backup-build-")
        cls.addClassCleanup(cls.compilation.cleanup)
        source = Path(cls.compilation.name) / "backup_harness.c"
        source.write_text(HARNESS)
        cls.binary = source.with_suffix("")
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "sqlite3", "json-c"], text=True).split()
        subprocess.run(["cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        "-I", str(ROOT / "inventory-addon/src"), str(source),
                        str(ROOT / "inventory-addon/src/inventory_backup.c"), "-o", str(cls.binary)] + flags,
                       check=True, capture_output=True, text=True)

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="punto-inventory-backup-")
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)
        self.source = self.path / "inventory.sqlite3"
        self.backups = self.path / "backups"
        self.now = int(time.time())
        self.process = None
        self.addCleanup(self.stop)
        with closing(sqlite3.connect(self.source)) as database:
            database.executescript("CREATE TABLE records(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO records VALUES(1, 'prima'); PRAGMA user_version=2;")

    def start(self, **changes):
        self.stop()
        environment = {key: value for key, value in os.environ.items() if not key.startswith("INVENTORY_BACKUP_")}
        environment.update(INVENTORY_BACKUP_DIR=str(self.backups), INVENTORY_BACKUP_INTERVAL_HOURS="1", INVENTORY_BACKUP_KEEP="2")
        environment.update(changes)
        self.log = open(self.path / "backup.log", "ab")
        self.process = subprocess.Popen([str(self.binary), str(self.source)], env=environment,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True)
        return self.command("status")

    def stop(self):
        if self.process is not None:
            self.process.communicate("stop\n", timeout=8)
            self.assertEqual(self.process.returncode, 0)
            self.log.close()
            self.process = None

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        output = self.process.stdout.readline()
        self.assertTrue(output, (self.path / "backup.log").read_text())
        return json.loads(output)

    def tick(self, delta=0):
        return self.command("tick %d" % (self.now + delta))

    def copies(self):
        return sorted(self.backups.glob("inventory-*.sqlite3"))

    def test_initial_copy_contains_committed_wal_and_is_private_standalone_sqlite(self):
        with closing(sqlite3.connect(self.source)) as writer:
            writer.execute("PRAGMA journal_mode=WAL")
            writer.execute("INSERT INTO records VALUES(2, 'dal WAL')")
            writer.commit()
            self.assertTrue(Path(str(self.source) + "-wal").exists())
            initial = self.start()
            self.assertTrue(initial["enabled"])
            self.assertIsNone(initial["last_success"])
            status = self.tick()
            self.assertIsNone(status["last_error"])
            backup = self.backups / status["latest_file"]
            self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o600)
            self.assertNotIn(str(self.path), json.dumps(status))
            with closing(sqlite3.connect(backup)) as database:
                self.assertEqual(database.execute("SELECT name FROM records ORDER BY id").fetchall(), [("prima",), ("dal WAL",)])
                self.assertEqual(database.execute("PRAGMA integrity_check").fetchall(), [("ok",)])
                self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 2)
                self.assertEqual(database.execute("PRAGMA journal_mode").fetchone()[0], "delete")
            self.assertEqual(list(self.backups.glob("*.tmp")), [])

    def test_restart_recovers_verified_success_and_does_not_duplicate_until_due(self):
        self.start()
        original = self.tick()
        self.command("exec INSERT INTO records VALUES(2, 'successiva')")
        self.assertEqual(self.tick(3599)["latest_file"], original["latest_file"])
        self.assertEqual(len(self.copies()), 1)
        restored = self.start()
        self.assertEqual(restored["last_success"], original["last_success"])
        self.assertEqual(restored["latest_file"], original["latest_file"])
        self.tick(1)
        self.assertEqual(len(self.copies()), 1)
        latest = self.tick(3600)
        self.assertNotEqual(latest["latest_file"], original["latest_file"])
        self.assertEqual(len(self.copies()), 2)
        with closing(sqlite3.connect(self.backups / latest["latest_file"])) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM records").fetchone()[0], 2)
        (self.backups / ".inventory-backup-status.json").unlink()
        recovered = self.start()
        self.assertEqual(recovered["latest_file"], latest["latest_file"])

    def test_retention_removes_only_verified_owned_regular_snapshot_files(self):
        self.start()
        self.tick()
        unrelated = self.backups / "manual.sqlite3"
        shutil.copy(self.source, unrelated)
        corrupt = self.backups / ("inventory-%020d-00000001.sqlite3" % (self.now - 100))
        corrupt.write_bytes(b"not a SQLite archive")
        linked = self.backups / ("inventory-%020d-00000002.sqlite3" % (self.now - 90))
        linked.symlink_to(self.source)
        external = self.source.read_bytes()
        self.tick(3600)
        self.tick(7200)
        verified = [path for path in self.copies() if path != corrupt and path != linked]
        self.assertEqual(len(verified), 2)
        self.assertTrue(unrelated.exists())
        self.assertTrue(corrupt.exists())
        self.assertTrue(linked.is_symlink())
        self.assertEqual(self.source.read_bytes(), external)

    def test_corrupt_latest_copy_is_ignored_during_restart_recovery(self):
        self.start()
        status = self.tick()
        corrupt = self.backups / ("inventory-%020d-ffffffff.sqlite3" % (self.now + 3600))
        corrupt.write_bytes(b"corrupt")
        recovered = self.start()
        self.assertEqual(recovered["latest_file"], status["latest_file"])
        self.tick(1)
        self.assertEqual(len(self.copies()), 2)  # One valid, one ignored corrupt file.

    def test_disabled_or_invalid_configuration_keeps_live_database_available(self):
        status = self.start(INVENTORY_BACKUP_DIR="")
        self.assertFalse(status["enabled"])
        self.assertIsNone(self.tick()["last_attempt"])
        self.assertFalse(self.backups.exists())
        for changes in ({"INVENTORY_BACKUP_INTERVAL_HOURS": "0"}, {"INVENTORY_BACKUP_INTERVAL_HOURS": "169"},
                        {"INVENTORY_BACKUP_KEEP": "366"}, {"INVENTORY_BACKUP_KEEP": "-1"},
                        {"INVENTORY_BACKUP_DIR": "relative/path"}):
            with self.subTest(changes=changes):
                status = self.start(**changes)
                self.assertTrue(status["enabled"])
                self.assertIsNotNone(status["last_error"])
                attempted = self.tick()
                self.assertIsNone(attempted["last_success"])
                self.assertEqual(self.tick(1)["last_attempt"], attempted["last_attempt"])
                self.command("exec UPDATE records SET name='ancora disponibile' WHERE id=1")

    def test_symlink_or_shared_writable_backup_directory_is_rejected(self):
        target = self.path / "target"
        target.mkdir()
        self.backups.symlink_to(target, target_is_directory=True)
        self.assertIsNotNone(self.start()["last_error"])
        self.assertIsNone(self.tick()["last_success"])
        self.assertEqual(list(target.iterdir()), [])
        self.stop()
        self.backups.unlink()
        self.backups.mkdir(mode=0o777)
        self.backups.chmod(0o777)
        self.assertIsNotNone(self.start()["last_error"])
        self.assertIsNone(self.tick()["last_success"])

    def test_busy_copy_is_bounded_retries_later_and_preserves_previous_success(self):
        self.start()
        first = self.tick()
        with closing(sqlite3.connect(self.source)) as writer:
            writer.execute("BEGIN EXCLUSIVE")
            writer.execute("INSERT INTO records VALUES(2, 'non ancora confermata')")
            started = time.monotonic()
            failed = self.tick(3600)
            self.assertLess(time.monotonic() - started, 7)
            self.assertIsNotNone(failed["last_error"])
            self.assertEqual(failed["latest_file"], first["latest_file"])
            self.assertEqual(len(self.copies()), 1)
            self.assertEqual(self.tick(3601)["last_attempt"], failed["last_attempt"])
            writer.commit()
        successful = self.tick(3900)
        self.assertIsNone(successful["last_error"])
        self.assertNotEqual(successful["latest_file"], first["latest_file"])

    def test_failure_metadata_survives_restart_without_retrying_in_a_hot_loop(self):
        self.start()
        previous = self.tick(-3600)
        with closing(sqlite3.connect(self.source)) as writer:
            writer.execute("BEGIN EXCLUSIVE")
            writer.execute("UPDATE records SET name='bloccato'")
            failed = self.tick()
            self.assertIsNotNone(failed["last_error"])
            recovered = self.start()
            self.assertEqual(recovered["last_error"], failed["last_error"])
            self.assertEqual(recovered["latest_file"], previous["latest_file"])
            self.assertEqual(self.tick(1)["last_attempt"], failed["last_attempt"])
            writer.commit()
        self.assertIsNone(self.tick(300)["last_error"])

    def run_script(self, *arguments):
        return subprocess.run([sys.executable, str(ROOT / "scripts/inventory_backup.py"), *map(str, arguments)],
                              capture_output=True, text=True, timeout=35)

    def test_recovery_script_verifies_and_restores_to_new_target_without_overwrite(self):
        self.start()
        status = self.tick()
        backup = self.backups / status["latest_file"]
        checked = self.run_script("verify", backup)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertIn("schema 2", checked.stdout)
        restored = self.path / "restored.sqlite3"
        result = self.run_script("restore", backup, restored)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE(restored.stat().st_mode), 0o600)
        with closing(sqlite3.connect(restored)) as database:
            self.assertEqual(database.execute("SELECT name FROM records").fetchall(), [("prima",)])
        unchanged = restored.read_bytes()
        self.assertEqual(self.run_script("restore", backup, restored).returncode, 1)
        self.assertEqual(restored.read_bytes(), unchanged)
        target_link = self.path / "existing-link.sqlite3"
        target_link.symlink_to(self.source)
        self.assertEqual(self.run_script("restore", backup, target_link).returncode, 1)
        source_link = self.path / "source-link.sqlite3"
        source_link.symlink_to(backup)
        self.assertEqual(self.run_script("verify", source_link).returncode, 1)
        invalid = self.path / "invalid.sqlite3"
        invalid.write_bytes(b"broken")
        absent = self.path / "must-not-exist.sqlite3"
        self.assertEqual(self.run_script("restore", invalid, absent).returncode, 1)
        self.assertFalse(absent.exists())
        self.assertEqual(list(self.path.glob(".inventory-restore-*")), [])

    def test_manual_copy_script_reads_committed_wal_without_stopping_source(self):
        with closing(sqlite3.connect(self.source)) as writer:
            writer.execute("PRAGMA journal_mode=WAL")
            writer.execute("INSERT INTO records VALUES(2, 'online')")
            writer.commit()
            copied = self.path / "manual.sqlite3"
            result = self.run_script("create", self.source, copied)
            self.assertEqual(result.returncode, 0, result.stderr)
            with closing(sqlite3.connect(copied)) as database:
                self.assertEqual(database.execute("SELECT count(*) FROM records").fetchone()[0], 2)


class InventoryBackupServiceTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create

    def test_service_status_exposes_startup_backup_and_recovers_schedule_on_restart(self):
        backups = self.path / "backups"
        self.start(INVENTORY_BACKUP_DIR=str(backups))
        status, value = self.request("GET", "/v1/status")
        self.assertEqual(status, 200, value)
        original = value["backup"]
        self.assertTrue(original["enabled"])
        self.assertEqual((original["interval_hours"], original["keep"]), (24, 14))
        self.assertIsNone(original["last_error"])
        self.assertIsNotNone(original["last_success"])
        self.assertIsNotNone(original["next_due"])
        copied = backups / original["latest_file"]
        self.assertTrue(copied.is_file())
        with closing(sqlite3.connect(copied)) as database:
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchall(), [("ok",)])
            self.assertEqual(database.execute("SELECT count(*) FROM assets").fetchone(), (0,))
        self.create()
        self.start(INVENTORY_BACKUP_DIR=str(backups))
        restarted = self.request("GET", "/v1/status")[1]
        self.assertEqual(restarted["backup"]["latest_file"], original["latest_file"])
        self.assertEqual(restarted["backup"]["last_success"], original["last_success"])
        self.assertEqual(restarted["summary"]["total"], 1)
        self.assertEqual(len(list(backups.glob("inventory-*.sqlite3"))), 1)

    def test_backup_failure_keeps_inventory_healthy_and_allows_writes(self):
        unavailable = self.path / "not-a-directory"
        unavailable.write_text("occupied")
        self.start(INVENTORY_BACKUP_DIR=str(unavailable))
        backup = self.request("GET", "/v1/status")[1]["backup"]
        self.assertTrue(backup["enabled"])
        self.assertIsNotNone(backup["last_error"])
        self.assertIsNone(backup["last_success"])
        self.assertIsNotNone(backup["next_due"])
        self.assertEqual(self.create()["asset_tag"], "PC-001")
        self.assertEqual(self.request("GET", "/healthz", authenticated=False)[0], 200)

if __name__ == "__main__":
    unittest.main()
