"""Main-app schema 8→9 preserves alert delivery state and rolls back atomically."""

from contextlib import closing
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

SCHEMA_EIGHT = """
PRAGMA foreign_keys=ON;
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL);
INSERT INTO users VALUES(1,'Gestore','manager@example.test'),(2,'Tecnico','worker@example.test');
CREATE TABLE audit_events(id INTEGER PRIMARY KEY AUTOINCREMENT,
 created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),
 actor_id INTEGER REFERENCES users(id),actor_name TEXT NOT NULL,actor_email TEXT NOT NULL,
 action TEXT NOT NULL,entity_type TEXT NOT NULL,entity_id INTEGER NOT NULL,
 before_json TEXT,after_json TEXT);
INSERT INTO audit_events(actor_id,actor_name,actor_email,action,entity_type,entity_id,after_json)
 VALUES(1,'Gestore','manager@example.test','fixture.created','fixture',1,'{"preserved":true}');
CREATE TABLE alert_rules(id INTEGER PRIMARY KEY AUTOINCREMENT,
 owner_id INTEGER NOT NULL REFERENCES users(id),name TEXT NOT NULL,
 type TEXT NOT NULL CHECK(type IN ('invalid_job','missing_hours')),
 run_time TEXT NOT NULL CHECK(length(run_time)=5),enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),
 created_at TEXT NOT NULL,updated_at TEXT NOT NULL,last_run_at TEXT,last_run_on TEXT,
 last_event_id INTEGER NOT NULL DEFAULT 0 CHECK(last_event_id>=0));
CREATE INDEX alert_rules_owner ON alert_rules(owner_id,id);
INSERT INTO alert_rules VALUES(11,1,'Regola conservata','invalid_job','08:00',0,'2026-10-01T06:00:00Z',
 '2026-10-02T06:00:00Z','2026-10-02T06:00:00Z','2026-10-02',88);
INSERT INTO alert_rules VALUES(34,1,'Ore mancanti','missing_hours','09:15',1,'2026-10-01T07:00:00Z',
 '2026-10-03T07:00:00Z',NULL,NULL,0);
INSERT INTO alert_rules SELECT 900,owner_id,name,type,run_time,enabled,created_at,updated_at,
 last_run_at,last_run_on,last_event_id FROM alert_rules WHERE id=11;
DELETE FROM alert_rules WHERE id=900;
CREATE TABLE alert_rule_users(rule_id INTEGER NOT NULL REFERENCES alert_rules(id),
 user_id INTEGER NOT NULL REFERENCES users(id),PRIMARY KEY(rule_id,user_id));
INSERT INTO alert_rule_users VALUES(11,2);
CREATE TABLE alert_rule_emails(rule_id INTEGER NOT NULL REFERENCES alert_rules(id),
 email TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(rule_id,email));
INSERT INTO alert_rule_emails VALUES(11,'alerts@example.test');
CREATE TABLE alert_subjects(rule_id INTEGER NOT NULL REFERENCES alert_rules(id),
 user_id INTEGER NOT NULL REFERENCES users(id),starts_on TEXT NOT NULL,last_checked_on TEXT,
 PRIMARY KEY(rule_id,user_id));
INSERT INTO alert_subjects VALUES(34,2,'2026-10-01','2026-10-05');
CREATE TABLE alert_notifications(id INTEGER PRIMARY KEY AUTOINCREMENT,
 rule_id INTEGER NOT NULL REFERENCES alert_rules(id),recipient_id INTEGER NOT NULL REFERENCES users(id),
 subject_user_id INTEGER NOT NULL REFERENCES users(id),event_key TEXT NOT NULL,title TEXT NOT NULL,
 message TEXT NOT NULL,work_date TEXT,recorded_minutes INTEGER,created_at TEXT NOT NULL,read_at TEXT,
 UNIQUE(rule_id,recipient_id,event_key));
INSERT INTO alert_notifications VALUES(45,11,1,2,'invalid_job:88','Avviso','Da conservare','2026-10-02',60,
 '2026-10-02T06:00:00Z',NULL);
CREATE TABLE alert_email_outbox(id INTEGER PRIMARY KEY AUTOINCREMENT,
 rule_id INTEGER NOT NULL REFERENCES alert_rules(id),email TEXT NOT NULL COLLATE NOCASE,
 subject_user_id INTEGER NOT NULL REFERENCES users(id),event_key TEXT NOT NULL,title TEXT NOT NULL,
 message TEXT NOT NULL,created_at TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'pending_configuration'
 CHECK(status IN ('pending_configuration','retrying','sent','skipped')),
 attempt_count INTEGER NOT NULL DEFAULT 0,last_attempt_at TEXT,next_attempt_at TEXT,sent_at TEXT,
 last_error_code TEXT,UNIQUE(rule_id,email,event_key));
INSERT INTO alert_email_outbox VALUES(57,11,'alerts@example.test',2,'invalid_job:88','Avviso','Messaggio',
 '2026-10-02T06:00:00Z','retrying',3,'2026-10-02T06:00:00Z','2026-10-02T07:00:00Z',NULL,'connection_failed');
CREATE TABLE preserved_business(id INTEGER PRIMARY KEY,description TEXT);
INSERT INTO preserved_business VALUES(8,'Dati esterni alle regole');
PRAGMA user_version=8;
"""


class InventoryAlertMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise unittest.SkipTest("C compiler is required")
        cls.compilation = tempfile.TemporaryDirectory(prefix="punto-inventory-migration-build-")
        cls.addClassCleanup(cls.compilation.cleanup)
        source = Path(cls.compilation.name) / "migration.c"
        source.write_text('#include "storage.h"\nint main(int argc,char **argv) {\n'
                          'if(argc!=2) return 2; bool okay=storage_open(argv[1]);\n'
                          'if(okay) okay=storage_health(); storage_close(); return okay ? 0 : 1;\n}\n')
        cls.binary = source.with_suffix("")
        subprocess.run(["cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        "-I", str(ROOT / "src"), str(source), str(ROOT / "src/storage.c"), "-lsqlite3", "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="punto-inventory-migration-")
        self.addCleanup(directory.cleanup)
        self.db = Path(directory.name) / "main.sqlite3"
        with closing(sqlite3.connect(self.db)) as database:
            database.executescript(SCHEMA_EIGHT)
        self.original = self.snapshot()

    def snapshot(self):
        with closing(sqlite3.connect(self.db)) as database:
            tables = [row[0] for row in database.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
            return {table: database.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall() for table in tables}

    def migrate(self, expected=0):
        result = subprocess.run([str(self.binary), str(self.db)], capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, expected, result.stderr)

    def test_schema_eight_upgrade_preserves_children_high_water_sequence_and_audit(self):
        self.migrate()
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone(), (9,))
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(database.execute("SELECT seq FROM sqlite_sequence WHERE name='alert_rules'").fetchone(), (900,))
            for table, rows in self.original.items():
                if table in ("sqlite_sequence", "audit_events"):
                    continue
                if table == "alert_rules":
                    self.assertEqual(database.execute("SELECT id,owner_id,name,type,run_time,enabled,created_at,updated_at,"
                                                      "last_run_at,last_run_on,last_event_id FROM alert_rules ORDER BY id").fetchall(), rows)
                    self.assertEqual(database.execute("SELECT days_before FROM alert_rules ORDER BY id").fetchall(), [(30,), (30,)])
                else:
                    self.assertEqual(database.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            self.assertEqual(database.execute("SELECT * FROM audit_events WHERE action!='system.migration' ORDER BY id").fetchall(), self.original["audit_events"])
            self.assertEqual(database.execute("SELECT entity_id,after_json FROM audit_events WHERE action='system.migration'").fetchall(), [(9, '{"schema_version":9}')])
            database.execute("PRAGMA foreign_keys=ON")
            inserted = database.execute("INSERT INTO alert_rules(owner_id,name,type,run_time,created_at,updated_at,days_before) "
                                        "VALUES(1,'Scadenze','inventory_due','10:00','2026-10-11','2026-10-11',365)").lastrowid
            self.assertEqual(inserted, 901)
            for value in (0, 366):
                with self.assertRaises(sqlite3.IntegrityError):
                    database.execute("UPDATE alert_rules SET days_before=? WHERE id=?", (value, inserted))
            with self.assertRaises(sqlite3.IntegrityError):
                database.execute("UPDATE alert_rules SET type='unknown' WHERE id=?", (inserted,))
            database.commit()
        migrated = self.snapshot()
        self.migrate()
        self.assertEqual(self.snapshot(), migrated, "Restart must preserve rules and avoid a duplicate migration audit.")

    def test_audit_failure_rolls_back_rebuild_rows_sequence_and_version(self):
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_migration_audit BEFORE INSERT ON audit_events "
                             "WHEN NEW.action='system.migration' AND NEW.entity_id=9 "
                             "BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            database.commit()
            original_ddl = database.execute("SELECT sql FROM sqlite_master WHERE name='alert_rules'").fetchone()
        self.migrate(expected=1)
        self.assertEqual(self.snapshot(), self.original)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone(), (8,))
            self.assertEqual(database.execute("SELECT sql FROM sqlite_master WHERE name='alert_rules'").fetchone(), original_ddl)
            self.assertEqual(database.execute("SELECT name FROM sqlite_master WHERE name='alert_rules_v9'").fetchall(), [])
            database.execute("DROP TRIGGER reject_migration_audit")
            database.commit()
        self.migrate()

    def test_preexisting_broken_foreign_key_prevents_commit(self):
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("INSERT INTO alert_rule_users VALUES(9999,2)")
            database.commit()
        broken = self.snapshot()
        self.migrate(expected=1)
        self.assertEqual(self.snapshot(), broken)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone(), (8,))


if __name__ == "__main__":
    unittest.main()
