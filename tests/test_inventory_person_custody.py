"""Real-person assignments and custody stay separate from login users and identities."""

from contextlib import closing
from datetime import date
import csv
import io
import sqlite3
import subprocess
import unittest

import test_inventory_addon as fixtures
import test_inventory_custody as custody_fixtures
import test_inventory_directory as directory_fixtures


class InventoryPersonCustodyTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update
    get = custody_fixtures.InventoryCustodyTests.get
    history = custody_fixtures.InventoryCustodyTests.history
    act = custody_fixtures.InventoryCustodyTests.act
    place = custody_fixtures.InventoryCustodyTests.place
    person = directory_fixtures.InventoryDirectoryTests.person
    entity_update = directory_fixtures.InventoryDirectoryTests.update_entity

    def assign(self, asset, person=None, **fields):
        status, value = self.act(asset, "context", assignee_person_id=person["id"] if person else "", **fields)
        self.assertEqual(status, 200, value)
        return value["asset"]

    def checkout(self, asset, person, **fields):
        data = dict(person_id=person["id"], checkout_on=date.today().isoformat(), condition_out="good")
        data.update(fields)
        status, value = self.act(asset, "checkout", **data)
        self.assertEqual(status, 201, value)
        return value["asset"], value["custody"]

    def checkin(self, asset, **fields):
        data = dict(returned_on=date.today().isoformat(), condition_in="good")
        data.update(fields)
        status, value = self.act(asset, "checkin", **data)
        self.assertEqual(status, 200, value)
        return value["asset"], value["custody"]

    def test_assignment_and_checkout_without_login_have_independent_snapshots(self):
        self.start()
        person = self.person("EMP-001", "Carla senza login")
        asset = self.create(location="Magazzino")
        assigned = self.assign(asset, person)
        self.assertEqual((assigned["status"], assigned["assignee"]), ("assigned", person["display_name"]))
        self.assertEqual(assigned["context"]["assignee_person_id"], person["id"])
        self.assertEqual(assigned["context"]["assignee_person_name"], person["display_name"])
        self.assertIsNone(assigned["context"]["assignee_user_id"])
        self.assertEqual(assigned["context"]["assignee_user_name"], "")
        checked, record = self.checkout(assigned, person, notes="Consegna alla collaboratrice")
        self.assertEqual((record["person_id"], record["person_name"]), (person["id"], person["display_name"]))
        self.assertIsNone(record["user_id"])
        self.assertEqual(record["user_name"], "")
        self.assertEqual(checked["location"], assigned["location"])
        status, renamed = self.entity_update(False, person, display_name="Carla aggiornata")
        self.assertEqual(status, 200, renamed)
        self.assertEqual(self.get(asset), checked)
        returned, closed = self.checkin(checked, condition_in="fair", notes="Completo")
        self.assertEqual((returned["status"], returned["assignee"]), ("available", ""))
        self.assertIsNone(returned["context"]["assignee_person_id"])
        self.assertEqual(closed["person_name"], "Carla senza login")
        self.assertFalse(closed["active"])
        self.assertEqual(self.history(asset)[1]["snapshot"]["custody"], record)
        self.assertEqual(self.request("GET", "/v1/people/%d/assets" % person["id"])[1]["items"], [])
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_asset_subjects").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_identity_people").fetchone()[0], 0)
        self.start()
        self.assertEqual(self.get(asset), returned)

    def test_new_assignments_reject_inactive_but_existing_assignment_can_move_or_return(self):
        self.start()
        person = self.person()
        site = self.place("site", "Roma")
        location = self.place("location", "Deposito", site)
        assigned = self.assign(self.create(), person)
        checked, _ = self.checkout(assigned, person)
        status, result = self.entity_update(False, person, active=0)
        self.assertEqual(status, 200, result)
        inactive = result["person"]
        fresh = self.create(tag="FRESH")
        for operation, fields in (("context", dict(assignee_person_id=inactive["id"])),
                                  ("checkout", dict(person_id=inactive["id"], checkout_on=date.today().isoformat(), condition_out="good"))):
            self.assertEqual(self.act(fresh, operation, **fields)[0], 400)
        moved = self.assign(checked, inactive, site_id=site["id"], location_id=location["id"])
        self.assertEqual(moved["location"], "Deposito")
        returned, _ = self.checkin(moved)
        self.assertIsNone(returned["context"]["assignee_person_id"])
        # An assignment without an open handover can also be cleared after deactivation.
        status, result = self.entity_update(False, inactive, active=1)
        active_person = result["person"]
        assigned = self.assign(returned, active_person)
        self.assertEqual(self.entity_update(False, active_person, active=0)[0], 200)
        cleared = self.assign(assigned)
        self.assertEqual(cleared["status"], "available")

    def test_recipient_exclusivity_missing_people_and_forged_names_are_rejected(self):
        self.start()
        person = self.person()
        asset = self.create()
        for fields in (dict(assignee_person_id=person["id"], assignee_user_id=person["id"], assignee_user_name="Login"),
                       dict(assignee_person_id=person["id"], assignee_person_name="Forged"),
                       dict(assignee_person_id=person["id"], assignee_user_name="Forged"),
                       dict(assignee_person_id=99999), dict(assignee_person_id=0), dict(assignee_person_id=-1)):
            with self.subTest(fields=fields):
                self.assertEqual(self.act(asset, "context", **fields)[0], 400)
        for fields in (dict(person_id=person["id"], user_id=person["id"], user_name="Login"),
                       dict(person_id=person["id"], user_name="Forged"), dict(person_id=person["id"], person_name="Forged"),
                       dict(person_id=99999), dict(person_id=-1), dict()):
            with self.subTest(fields=fields):
                self.assertEqual(self.act(asset, "checkout", checkout_on=date.today().isoformat(), condition_out="good", **fields)[0], 400)
        self.assertEqual(self.get(asset), asset)

    def test_open_handover_distinguishes_person_from_user_with_same_numeric_id(self):
        self.start()
        person = self.person()
        other = self.person("P-OTHER", "Altra persona")
        asset, record = self.checkout(self.create(), person)
        for fields in (dict(assignee_user_id=person["id"], assignee_user_name="Login stesso ID"),
                       dict(assignee_person_id=other["id"]), dict()):
            self.assertEqual(self.act(asset, "context", **fields)[0], 409)
        returned, _ = self.checkin(asset)
        status, value = self.act(returned, "checkout", user_id=person["id"], user_name="Login legacy", checkout_on=date.today().isoformat(), condition_out="good")
        self.assertEqual(status, 201, value)
        self.assertIsNone(value["custody"]["person_id"])
        self.assertEqual(value["custody"]["user_id"], person["id"])
        self.assertEqual(self.act(value["asset"], "context", assignee_person_id=person["id"])[0], 409)
        self.assertEqual(self.request("GET", "/v1/assets/%d/custody?limit=1" % asset["id"])[1]["next_after_id"], value["custody"]["id"])

    def test_editor_and_csv_cannot_bypass_person_assignment_or_open_handover(self):
        self.start()
        person = self.person()
        assigned = self.assign(self.create(), person)
        self.assertEqual(self.update(assigned, status="available", assignee=assigned["assignee"])[0], 409)
        self.assertEqual(self.update(assigned, status="assigned", assignee="Nome libero")[0], 409)
        self.assertEqual(self.update(assigned, status="assigned", assignee=assigned["assignee"], asset_type="software")[0], 409)
        status, edited = self.update(assigned, status="assigned", assignee=assigned["assignee"], notes="Ammesso")
        self.assertEqual(status, 200, edited)
        checked, _ = self.checkout(edited["asset"], person)
        exported = self.request("GET", "/v1/assets/export")[1]
        records = list(csv.reader(io.StringIO(exported["csv"])))
        records[1][records[0].index("assignee")] = "Cambio da CSV"
        stream = io.StringIO()
        csv.writer(stream).writerows(records)
        status, preview = self.request("POST", "/v1/import/preview", dict(csv=stream.getvalue(), mode="upsert", actor_id=11))
        self.assertEqual(status, 200, preview)
        self.assertFalse(preview["can_commit"])
        self.assertEqual(self.get(checked), checked)

    def test_history_failures_roll_back_person_context_checkout_and_checkin(self):
        self.start()
        person = self.person()
        asset = self.create()
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER person_custody_fail BEFORE INSERT ON asset_history BEGIN SELECT RAISE(ABORT,'forced'); END")
            database.commit()
        for operation, fields in (("context", dict(assignee_person_id=person["id"])),
                                  ("checkout", dict(person_id=person["id"], checkout_on=date.today().isoformat(), condition_out="good"))):
            self.assertEqual(self.act(asset, operation, **fields)[0], 500)
            self.assertEqual(self.get(asset), asset)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_custody").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_context").fetchone()[0], 0)
            database.execute("DROP TRIGGER person_custody_fail")
            database.commit()
        checked, _ = self.checkout(asset, person)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER person_custody_fail BEFORE INSERT ON asset_history BEGIN SELECT RAISE(ABORT,'forced'); END")
            database.commit()
        self.assertEqual(self.act(checked, "checkin", returned_on=date.today().isoformat(), condition_in="good")[0], 500)
        self.assertEqual(self.get(asset), checked)
        self.assertEqual(self.act(asset, "context", assignee_person_id=person["id"])[0], 409)

    def schema_five_fixture(self, obstruct=False):
        snapshot = directory_fixtures.InventoryDirectoryTests.schema_four_fixture(self)
        # Frozen schema-5 directory; the migrated tables are still the original schema-4
        # context/custody/relations. No columns from the implementation under test are used.
        with closing(sqlite3.connect(self.db)) as database:
            database.executescript("""
                CREATE TABLE inventory_people(id INTEGER PRIMARY KEY AUTOINCREMENT,person_code TEXT NOT NULL COLLATE NOCASE UNIQUE,display_name TEXT NOT NULL,email TEXT NOT NULL DEFAULT '',organization TEXT NOT NULL DEFAULT '',job_title TEXT NOT NULL DEFAULT '',notes TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1,version INTEGER NOT NULL DEFAULT 1,actor_id INTEGER NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL);
                CREATE TABLE inventory_identities(id INTEGER PRIMARY KEY AUTOINCREMENT,realm TEXT NOT NULL COLLATE NOCASE,login TEXT NOT NULL COLLATE NOCASE,display_name TEXT NOT NULL DEFAULT '',kind TEXT NOT NULL DEFAULT 'personal',notes TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1,version INTEGER NOT NULL DEFAULT 1,actor_id INTEGER NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,UNIQUE(realm,login));
                CREATE TABLE inventory_identity_people(id INTEGER PRIMARY KEY AUTOINCREMENT,identity_id INTEGER NOT NULL REFERENCES inventory_identities(id),person_id INTEGER NOT NULL REFERENCES inventory_people(id),role TEXT NOT NULL DEFAULT 'user',notes TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1,actor_id INTEGER NOT NULL,created_at TEXT NOT NULL,removed_actor_id INTEGER,removed_at TEXT);
                CREATE UNIQUE INDEX inventory_identity_people_active_pair ON inventory_identity_people(identity_id,person_id) WHERE active=1;
                CREATE TABLE inventory_entity_history(id INTEGER PRIMARY KEY AUTOINCREMENT,entity_type TEXT NOT NULL,entity_id INTEGER NOT NULL,action TEXT NOT NULL,actor_id INTEGER NOT NULL,version INTEGER NOT NULL,created_at TEXT NOT NULL,snapshot TEXT NOT NULL,change TEXT,UNIQUE(entity_type,entity_id,version));
                CREATE TABLE inventory_asset_subjects(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),person_id INTEGER REFERENCES inventory_people(id),identity_id INTEGER REFERENCES inventory_identities(id),role TEXT NOT NULL DEFAULT 'user',notes TEXT NOT NULL DEFAULT '',subject_snapshot TEXT NOT NULL,actor_id INTEGER NOT NULL,created_at TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1,removed_at TEXT,removed_actor_id INTEGER);
                CREATE INDEX custom_custody_notes ON asset_custody(notes_out);
                PRAGMA user_version=5;
            """)
            database.execute("UPDATE sqlite_sequence SET seq=1000 WHERE name='asset_custody'")
            database.execute("INSERT INTO inventory_people(id,person_code,display_name,actor_id,created_at,updated_at) VALUES(88,'P-MIGRATED','Persona precedente',7,'2026-01-02T03:04:05Z','2026-01-02T03:04:05Z')")
            if obstruct:
                database.execute("CREATE TABLE asset_custody_next(obstruction TEXT)")
            database.commit()
        return snapshot

    def test_schema_five_migration_preserves_legacy_custody_ids_sequence_indexes_and_history(self):
        snapshot = self.schema_five_fixture()
        self.start()
        asset = self.get(dict(id=41))
        self.assertEqual((asset["custody"]["id"], asset["custody"]["user_id"], asset["custody"]["user_name"]), (19, 21, "Mario precedente"))
        self.assertIsNone(asset["custody"]["person_id"])
        self.assertIsNone(asset["context"]["assignee_person_id"])
        returned, closed = self.checkin(asset)
        self.assertEqual(closed["id"], 19)
        status, person = self.request("GET", "/v1/people/88")
        self.assertEqual(status, 200, person)
        checked, record = self.checkout(returned, person["person"])
        self.assertGreater(record["id"], 1000)
        self.assertEqual(checked["custody"]["person_id"], 88)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 6)
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], snapshot)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM sqlite_master WHERE name='custom_custody_notes'").fetchone()[0], 1)
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_failed_schema_five_migration_rolls_back_ddl_and_retries(self):
        snapshot = self.schema_five_fixture(obstruct=True)
        result = subprocess.run([str(fixtures.ROOT / "inventory-addon/build/punto-inventory")], env=self.environment(), capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertNotIn("assignee_person_id", [row[1] for row in database.execute("PRAGMA table_info(asset_context)")])
            self.assertEqual(database.execute("SELECT user_id FROM asset_custody WHERE id=19").fetchone()[0], 21)
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], snapshot)
            database.execute("DROP TABLE asset_custody_next")
            database.commit()
        self.start()
        self.assertEqual(self.get(dict(id=41))["custody"]["id"], 19)

    def test_failed_relation_migration_also_rolls_back_completed_custody_rebuild(self):
        snapshot = self.schema_five_fixture()
        with closing(sqlite3.connect(self.db)) as database:
            # This collision is reached only after custody has been rebuilt and
            # all three new relation columns have been added in the same transaction.
            database.execute("CREATE INDEX active_relation_identity ON assets(name)")
            database.commit()
            custody_sql = database.execute("SELECT sql FROM sqlite_master WHERE name='asset_custody'").fetchone()[0]
            relation_index_sql = database.execute("SELECT sql FROM sqlite_master WHERE name='active_relation_assignee'").fetchone()[0]
            custody_row = database.execute("SELECT * FROM asset_custody WHERE id=19").fetchone()
            context_row = database.execute("SELECT * FROM asset_context WHERE asset_id=41").fetchone()
            history_row = database.execute("SELECT * FROM asset_history WHERE id=77").fetchone()
            custody_sequence = database.execute("SELECT seq FROM sqlite_sequence WHERE name='asset_custody'").fetchone()[0]
        result = subprocess.run([str(fixtures.ROOT / "inventory-addon/build/punto-inventory")], env=self.environment(), capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertEqual(database.execute("SELECT sql FROM sqlite_master WHERE name='asset_custody'").fetchone()[0], custody_sql)
            custody_columns = {row[1]: row for row in database.execute("PRAGMA table_info(asset_custody)")}
            self.assertEqual(custody_columns["user_id"][3], 1)  # Legacy NOT NULL was restored.
            self.assertNotIn("person_id", custody_columns)
            self.assertEqual(database.execute("SELECT * FROM asset_custody WHERE id=19").fetchone(), custody_row)
            self.assertEqual(database.execute("SELECT seq FROM sqlite_sequence WHERE name='asset_custody'").fetchone()[0], custody_sequence)
            self.assertEqual(database.execute("SELECT * FROM asset_context WHERE asset_id=41").fetchone(), context_row)
            self.assertNotIn("assignee_person_id", [row[1] for row in database.execute("PRAGMA table_info(asset_context)")])
            self.assertEqual(database.execute("SELECT * FROM asset_history WHERE id=77").fetchone(), history_row)
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], snapshot)
            relation_columns = [row[1] for row in database.execute("PRAGMA table_info(asset_relations)")]
            for column in ("person_id", "identity_id", "subject_snapshot"):
                self.assertNotIn(column, relation_columns)
            self.assertEqual(database.execute("SELECT sql FROM sqlite_master WHERE name='active_relation_assignee'").fetchone()[0], relation_index_sql)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM sqlite_master WHERE name IN('asset_custody_next','asset_custody_person','asset_context_person','active_relation_person')").fetchone()[0], 0)
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            database.execute("DROP INDEX active_relation_identity")
            database.commit()
        self.start()
        self.assertEqual(self.get(dict(id=41))["custody"]["id"], 19)


if __name__ == "__main__":
    unittest.main()
