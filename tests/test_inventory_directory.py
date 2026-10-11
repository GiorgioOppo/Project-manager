"""Distinct inventory people and identities with audited many-to-many associations."""

from contextlib import closing
import json
import sqlite3
import unittest
from urllib.parse import urlencode

import test_inventory_addon as fixtures


class InventoryDirectoryTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create

    def person(self, code="P-001", name="Mario Rossi", **fields):
        data = dict(person_code=code, display_name=name, actor_id=7)
        data.update(fields)
        status, value = self.request("POST", "/v1/people", data)
        self.assertEqual(status, 201, value)
        return value["person"]

    def identity(self, login="mario.rossi", realm="azienda.local", **fields):
        data = dict(realm=realm, login=login, actor_id=7)
        data.update(fields)
        status, value = self.request("POST", "/v1/identities", data)
        self.assertEqual(status, 201, value)
        return value["identity"]

    def detail(self, identity, entity):
        status, value = self.request("GET", "/v1/%s/%d" % ("identities" if identity else "people", entity["id"]))
        self.assertEqual(status, 200, value)
        return value["identity" if identity else "person"]

    def update_entity(self, identity, entity, **fields):
        keys = ("realm", "login", "display_name", "kind", "notes") if identity else (
            "person_code", "display_name", "email", "organization", "job_title", "notes")
        data = {key: entity[key] for key in keys}
        data.update(version=entity["version"], active=int(entity["active"]), actor_id=8)
        data.update(fields)
        return self.request("POST", "/v1/%s/%d" % ("identities" if identity else "people", entity["id"]), data)

    def link(self, identity, person, **fields):
        data = dict(version=identity["version"], person_id=person["id"], actor_id=11)
        data.update(fields)
        status, value = self.request("POST", "/v1/identities/%d/people" % identity["id"], data)
        self.assertEqual(status, 201, value)
        return value

    def history(self, identity, entity):
        status, value = self.request("GET", "/v1/%s/%d/history" % ("identities" if identity else "people", entity["id"]))
        self.assertEqual(status, 200, value)
        return value["items"]

    def schema_four_fixture(self, obstruct=False):
        # Frozen previous schema: no people, identity, membership or asset-subject tables.
        with closing(sqlite3.connect(self.db)) as database:
            database.executescript("""
                CREATE TABLE assets(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE,name TEXT NOT NULL,category TEXT NOT NULL,serial_number TEXT NOT NULL,manufacturer TEXT NOT NULL,model TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('available','assigned','maintenance','retired','lost')),location TEXT NOT NULL,assignee TEXT NOT NULL,purchase_date TEXT NOT NULL,warranty_until TEXT NOT NULL,notes TEXT NOT NULL,actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL CHECK(version>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,asset_type TEXT NOT NULL DEFAULT 'it_device' CHECK(asset_type IN ('it_device','network_device','physical','software','license')),details TEXT NOT NULL DEFAULT '{}');
                CREATE TABLE asset_history(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),action TEXT NOT NULL CHECK(action IN ('created','updated')),actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL,created_at TEXT NOT NULL,snapshot TEXT NOT NULL,change TEXT,UNIQUE(asset_id,version));
                CREATE TABLE asset_relations(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER NOT NULL REFERENCES assets(id),target_id INTEGER REFERENCES assets(id),relation_type TEXT NOT NULL CHECK(relation_type IN ('installed_on','connected_to','licensed_to')),assignee TEXT NOT NULL DEFAULT '' COLLATE NOCASE,quantity INTEGER NOT NULL CHECK(quantity BETWEEN 1 AND 1000000),installed_version TEXT NOT NULL DEFAULT '',source_port TEXT NOT NULL DEFAULT '',target_port TEXT NOT NULL DEFAULT '',notes TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),removed_at TEXT,removed_actor_id INTEGER);
                CREATE UNIQUE INDEX active_relation_target ON asset_relations(source_id,relation_type,target_id) WHERE active=1 AND target_id IS NOT NULL;
                CREATE UNIQUE INDEX active_relation_assignee ON asset_relations(source_id,relation_type,assignee COLLATE NOCASE) WHERE active=1 AND target_id IS NULL;
                CREATE TABLE inventory_import_previews(preview_id TEXT PRIMARY KEY,actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,used INTEGER NOT NULL DEFAULT 0 CHECK(used IN (0,1)),plan TEXT NOT NULL);
                CREATE TABLE inventory_places(id INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT NOT NULL CHECK(kind IN ('site','department','location')),name TEXT NOT NULL,parent_id INTEGER REFERENCES inventory_places(id),active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),version INTEGER NOT NULL DEFAULT 1 CHECK(version>0),actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL);
                CREATE UNIQUE INDEX inventory_places_unique ON inventory_places(kind,name COLLATE NOCASE,COALESCE(parent_id,0));
                CREATE TABLE asset_context(asset_id INTEGER PRIMARY KEY REFERENCES assets(id),assignee_user_id INTEGER CHECK(assignee_user_id>0),assignee_user_name TEXT NOT NULL DEFAULT '',site_id INTEGER REFERENCES inventory_places(id),site_name TEXT NOT NULL DEFAULT '',department_id INTEGER REFERENCES inventory_places(id),department_name TEXT NOT NULL DEFAULT '',location_id INTEGER REFERENCES inventory_places(id),location_name TEXT NOT NULL DEFAULT '');
                CREATE TABLE asset_custody(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),user_id INTEGER NOT NULL CHECK(user_id>0),user_name TEXT NOT NULL,checkout_on TEXT NOT NULL,due_on TEXT NOT NULL DEFAULT '',condition_out TEXT NOT NULL,notes_out TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,site_id INTEGER REFERENCES inventory_places(id),site_name TEXT NOT NULL DEFAULT '',department_id INTEGER REFERENCES inventory_places(id),department_name TEXT NOT NULL DEFAULT '',location_id INTEGER REFERENCES inventory_places(id),location_name TEXT NOT NULL DEFAULT '',active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),returned_on TEXT,condition_in TEXT,notes_in TEXT,returned_actor_id INTEGER,returned_at TEXT);
                CREATE UNIQUE INDEX asset_custody_active ON asset_custody(asset_id) WHERE active=1;
                CREATE TABLE asset_verifications(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),verified_on TEXT NOT NULL,condition TEXT NOT NULL,observed_location_id INTEGER REFERENCES inventory_places(id),observed_location_name TEXT NOT NULL DEFAULT '',expected_location_id INTEGER REFERENCES inventory_places(id),expected_location_name TEXT NOT NULL DEFAULT '',location_matches INTEGER CHECK(location_matches IN (0,1)),notes TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL);
                PRAGMA user_version=4;
            """)
            stamp = "2026-01-02T03:04:05Z"
            asset = (41, "LEGACY", "Computer storico", "hardware", "OLD", "", "", "assigned", "Roma", "Mario precedente", "", "", "Dati precedenti", 7, 1, stamp, stamp, "it_device", "{}")
            database.execute("INSERT INTO assets VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", asset)
            snapshot = json.dumps(dict(id=41, asset_tag="LEGACY", name="Computer storico", version=1, context=None), ensure_ascii=False)
            database.execute("INSERT INTO asset_history(id,asset_id,action,actor_id,version,created_at,snapshot) VALUES(77,41,'created',7,1,?,?)", (stamp, snapshot))
            database.execute("INSERT INTO inventory_places(id,kind,name,actor_id,created_at,updated_at) VALUES(11,'site','Roma',7,?,?)", (stamp, stamp))
            database.execute("INSERT INTO asset_context(asset_id,assignee_user_id,assignee_user_name,site_id,site_name) VALUES(41,21,'Mario precedente',11,'Roma')")
            database.execute("INSERT INTO asset_custody(id,asset_id,user_id,user_name,checkout_on,due_on,condition_out,actor_id,created_at,site_id,site_name) VALUES(19,41,21,'Mario precedente','2026-01-02','2026-12-01','good',7,?,11,'Roma')", (stamp,))
            database.execute("INSERT INTO asset_verifications(id,asset_id,verified_on,condition,actor_id,created_at) VALUES(23,41,'2026-01-02','good',7,?)", (stamp,))
            database.execute("INSERT INTO inventory_import_previews VALUES(?,7,1,2,0,'[]')", ("a" * 32,))
            database.execute("UPDATE sqlite_sequence SET seq=100 WHERE name='assets'")
            if obstruct:
                # Fail after the first two new tables were created, exercising transactional DDL.
                database.execute("CREATE INDEX inventory_identity_people_active_pair ON assets(name)")
            database.commit()
            return snapshot

    def test_multiple_identities_per_person_and_shared_identity_are_independent_many_to_many(self):
        self.start()
        mario = self.person(email="mario@example.test", organization="Acme", job_title="Tecnico")
        anna = self.person("P-002", "Anna Bianchi")
        identity = self.identity()
        shared = self.identity("support", kind="shared")
        # Kind describes the account: even a personal identity can have more than one person.
        first = self.link(identity, mario, role="owner", notes="Responsabile")
        second = self.link(first["identity"], anna)
        third = self.link(shared, mario)
        self.assertEqual(second["identity"]["people_count"], 2)
        self.assertEqual(self.detail(False, mario)["identity_count"], 2)
        self.assertEqual(self.detail(False, anna)["identity_count"], 1)
        self.assertEqual(self.detail(True, shared)["people_count"], 1)
        self.assertEqual(second["identity"]["version"], 3)
        self.assertEqual(third["person"]["version"], 3)
        self.assertEqual(first["link"]["role"], "owner")
        self.assertEqual(second["link"]["role"], "user")
        status, links = self.request("GET", "/v1/people/%d/identities" % mario["id"])
        self.assertEqual(status, 200, links)
        self.assertEqual({item["identity"]["login"] for item in links["items"]}, {"mario.rossi", "support"})
        status, people = self.request("GET", "/v1/identities/%d/people" % identity["id"])
        self.assertEqual({item["person"]["person_code"] for item in people["items"]}, {"P-001", "P-002"})
        self.assertEqual(self.history(True, identity)[0]["change"]["operation"], "identity_person_added")
        self.assertEqual(self.history(False, mario)[0]["change"]["link"]["identity"]["kind"], "shared")
        self.assertEqual(self.history(False, mario)[-1]["snapshot"]["identity_count"], 0)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_people").fetchone()[0], 2)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_identities").fetchone()[0], 2)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_identity_people WHERE active=1").fetchone()[0], 3)
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_uniqueness_rename_versions_and_soft_deactivation_preserve_connections(self):
        self.start()
        person = self.person()
        identity = self.identity(kind="service")
        for data in (dict(person_code="p-001", display_name="Duplicato", actor_id=7),):
            self.assertEqual(self.request("POST", "/v1/people", data)[0], 409)
        self.assertEqual(self.request("POST", "/v1/identities", dict(realm="AZIENDA.LOCAL", login="Mario.Rossi", actor_id=7))[0], 409)
        other_realm = self.identity("mario.rossi", "cloud.example")
        linked = self.link(identity, person)
        self.assertEqual(self.request("POST", "/v1/identities/%d/people" % identity["id"],
                                      dict(version=linked["identity"]["version"], person_id=person["id"], actor_id=7))[0], 409)
        status, changed = self.update_entity(False, linked["person"], display_name="Mario Verdi", active=0)
        self.assertEqual(status, 200, changed)
        self.assertFalse(changed["person"]["active"])
        self.assertEqual(changed["person"]["identity_count"], 1)
        self.assertEqual(self.update_entity(False, linked["person"], display_name="Obsoleto")[0], 409)
        self.assertEqual(self.request("POST", "/v1/identities/%d/people" % other_realm["id"],
                                      dict(version=1, person_id=person["id"], actor_id=7))[0], 409)
        status, updated = self.update_entity(True, linked["identity"], realm="new.example", login="mverdi", active=0)
        self.assertEqual(status, 200, updated)
        current = self.request("GET", "/v1/identities/%d/people" % identity["id"])[1]["items"][0]
        self.assertEqual(current["person"]["display_name"], "Mario Verdi")
        self.assertFalse(current["identity"]["active"])
        # Removal remains possible when either directory endpoint has been deactivated.
        status, removed = self.request("POST", "/v1/identities/%d/people/%d/remove" % (identity["id"], linked["link"]["id"]),
                                       dict(version=updated["identity"]["version"], actor_id=12))
        self.assertEqual(status, 200, removed)
        self.assertFalse(removed["link"]["active"])
        self.assertEqual((removed["identity"]["people_count"], removed["person"]["identity_count"]), (0, 0))
        self.assertEqual(removed["link"]["removed_actor_id"], 12)
        self.assertTrue(removed["link"]["removed_at"])
        self.assertEqual(self.history(True, identity)[-2]["change"]["link"]["person"]["display_name"], "Mario Rossi")
        self.assertEqual(self.request("GET", "/v1/people/%d/identities" % person["id"])[1]["items"], [])

    def test_remove_relink_audit_retains_original_records_and_rejects_wrong_owner(self):
        self.start()
        person = self.person()
        identity = self.identity()
        other = self.identity("other")
        first = self.link(identity, person)
        route = "/v1/identities/%d/people/%d/remove"
        self.assertEqual(self.request("POST", route % (other["id"], first["link"]["id"]), dict(version=1, actor_id=7))[0], 404)
        self.assertEqual(self.request("POST", route % (identity["id"], first["link"]["id"]), dict(version=1, actor_id=7))[0], 409)
        status, removed = self.request("POST", route % (identity["id"], first["link"]["id"]), dict(version=2, actor_id=12))
        self.assertEqual(status, 200, removed)
        self.assertEqual(self.request("POST", route % (identity["id"], first["link"]["id"]), dict(version=3, actor_id=12))[0], 409)
        second = self.link(removed["identity"], person, role="owner")
        self.assertNotEqual(first["link"]["id"], second["link"]["id"])
        self.assertEqual([item["change"]["operation"] for item in self.history(True, identity)],
                         ["identity_person_added", "identity_person_removed", "identity_person_added", "created"])
        self.assertEqual([item["version"] for item in self.history(False, person)], [4, 3, 2, 1])
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT active FROM inventory_identity_people ORDER BY id").fetchall(), [(0,), (1,)])

    def test_create_update_and_association_validation_reject_credentials_and_malformed_fields(self):
        self.start()
        person = self.person()
        identity = self.identity()
        invalid_people = [dict(person_code="", display_name="X"), dict(person_code="P", display_name=" "),
                          dict(person_code="p" * 65, display_name="X"), dict(person_code="P", display_name="é" * 81),
                          dict(person_code="P", display_name="X", email="a" * 255),
                          dict(person_code="P", display_name="X", organization="a" * 161),
                          dict(person_code="P", display_name="X", job_title="a" * 121),
                          dict(person_code="P", display_name="X", notes="a" * 2001),
                          dict(person_code="P", display_name="X", active="true"),
                          dict(person_code="P", display_name="X", version=1),
                          dict(person_code="P", display_name="X", user_id=1)]
        invalid_identities = [dict(realm="", login="x"), dict(realm="r", login=" "), dict(realm="r" * 161, login="x"),
                              dict(realm="r", login="x" * 161), dict(realm="r", login="x", display_name="a" * 161),
                              dict(realm="r", login="x", kind="admin"), dict(realm="r", login="x", password="secret"),
                              dict(realm="r", login="x", token="secret"), dict(realm="r", login="x", notes="a" * 2001)]
        for endpoint, cases in (("people", invalid_people), ("identities", invalid_identities)):
            for fields in cases:
                with self.subTest(endpoint=endpoint, fields=fields):
                    self.assertEqual(self.request("POST", "/v1/" + endpoint, dict(fields, actor_id=7))[0], 400)
        for extra in (dict(actor_id=0), dict(version=0), dict(version="1.0"), dict(person_id=0), dict(role="admin"),
                      dict(notes="a" * 2001), dict(password="secret"), dict(identity_id=identity["id"])):
            data = dict(version=1, person_id=person["id"], actor_id=7)
            data.update(extra)
            with self.subTest(extra=extra):
                self.assertEqual(self.request("POST", "/v1/identities/%d/people" % identity["id"], data)[0], 400)
        self.assertEqual(self.request("POST", "/v1/identities/%d/people" % identity["id"], dict(version=1, person_id=9999, actor_id=7))[0], 404)
        self.assertEqual(self.detail(True, identity)["version"], 1)
        self.assertEqual(self.detail(False, person)["version"], 1)
        self.assertEqual(self.request("POST", "/v1/people", raw="person_code=P&display_name=X&actor_id=7&actor_id=7")[0], 400)
        self.assertEqual(self.request("POST", "/v1/identities", raw="realm=r&login=%FF&actor_id=7")[0], 400)
        self.assertEqual(self.request("POST", "/v1/people", raw="person_code=P&display_name=%00&actor_id=7")[0], 400)

    def test_filtered_lists_paginate_stably_and_validate_filters(self):
        self.start()
        person = self.person(organization="Operations")
        self.person("P-002", "Anna", active=0)
        first = self.identity("alice", "cloud.example", kind="shared")
        second = self.identity("bob", "cloud.example", kind="service")
        self.identity("charlie", active=0)
        self.link(first, person)
        self.link(second, person)
        status, first_page = self.request("GET", "/v1/identities?limit=1")
        self.assertEqual(status, 200, first_page)
        ids = [first_page["items"][0]["id"]]
        while first_page["next_after_id"]:
            query = urlencode(dict(limit=1, after_id=first_page["next_after_id"]))
            first_page = self.request("GET", "/v1/identities?" + query)[1]
            ids.extend(item["id"] for item in first_page["items"])
        self.assertEqual(ids, sorted(ids, reverse=True))
        self.assertEqual(len(set(ids)), 3)
        self.assertEqual([row["login"] for row in self.request("GET", "/v1/identities?kind=shared&q=CLOUD&active=1")[1]["items"]], ["alice"])
        self.assertEqual(len(self.request("GET", "/v1/identities?person_id=%d" % person["id"])[1]["items"]), 2)
        self.assertEqual(len(self.request("GET", "/v1/identities?person_id=9999")[1]["items"]), 0)
        self.assertEqual(len(self.request("GET", "/v1/people?q=Operations&active=1")[1]["items"]), 1)
        self.assertEqual(len(self.request("GET", "/v1/people?active=0")[1]["items"]), 1)
        for endpoint in ("people", "identities"):
            for query in ("q=" + "x" * 201, "q=%01", "active=true", "active=", "limit=0", "limit=101", "after_id=0", "after_id=-1", "limit=1&limit=1", "unknown=1"):
                with self.subTest(endpoint=endpoint, query=query):
                    self.assertEqual(self.request("GET", "/v1/" + endpoint + "?" + query)[0], 400)
        for query in ("kind=admin", "person_id=0", "person_id=", "person_id=-1"):
            self.assertEqual(self.request("GET", "/v1/identities?" + query)[0], 400)
        self.assertEqual(self.request("GET", "/v1/people?kind=shared")[0], 400)
        self.assertEqual(self.request("GET", "/v1/people?person_id=1")[0], 400)

    def test_history_and_links_obey_response_budget_without_skipping_rows(self):
        self.start()
        identity = self.identity(notes="Identità " + "x" * 1989)
        for number in range(30):
            person = self.person("P-%03d" % number, "Persona %d" % number, notes="Persone " + "y" * 1990)
            identity = self.link(identity, person, notes="Relazione " + "z" * 1989)["identity"]
        # Every row includes two directory entries plus notes; budget must stop before 30 rows.
        for suffix, expected in (("people", 30), ("history", 31)):
            path = "/v1/identities/%d/%s" % (identity["id"], suffix)
            cursor = None
            seen = []
            pages = 0
            while True:
                query = dict(limit=100)
                if cursor:
                    query["after_id"] = cursor
                status, page = self.request("GET", path + "?" + urlencode(query))
                self.assertEqual(status, 200, page)
                self.assertTrue(page["items"])
                seen.extend(item["id"] for item in page["items"])
                pages += 1
                cursor = page["next_after_id"]
                if not cursor:
                    break
                self.assertLess(pages, 10)
            self.assertEqual(len(seen), expected)
            self.assertEqual(len(set(seen)), expected)
            self.assertGreater(pages, 1)

    def test_failed_history_insertion_rolls_back_entities_links_and_both_versions(self):
        self.start()
        person = self.person()
        identity = self.identity()
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_person_history BEFORE INSERT ON inventory_entity_history WHEN NEW.entity_type='person' BEGIN SELECT RAISE(ABORT,'simulated failure'); END")
            database.commit()
        self.assertEqual(self.update_entity(False, person, display_name="Annullato")[0], 500)
        self.assertEqual(self.request("POST", "/v1/people", dict(person_code="NEW", display_name="Annullato", actor_id=7))[0], 500)
        self.assertEqual(self.request("POST", "/v1/identities/%d/people" % identity["id"], dict(version=1, person_id=person["id"], actor_id=7))[0], 500)
        self.assertEqual(self.detail(False, person), person)
        self.assertEqual(self.detail(True, identity), identity)
        self.assertEqual(len(self.history(True, identity)), 1)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_identity_people").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_people").fetchone()[0], 1)
            database.execute("DROP TRIGGER reject_person_history")
            database.commit()
        linked = self.link(identity, person)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_person_history BEFORE INSERT ON inventory_entity_history WHEN NEW.entity_type='person' BEGIN SELECT RAISE(ABORT,'simulated failure'); END")
            database.commit()
        status, value = self.request("POST", "/v1/identities/%d/people/%d/remove" % (identity["id"], linked["link"]["id"]), dict(version=2, actor_id=7))
        self.assertEqual(status, 500, value)
        self.assertEqual(self.detail(False, person), linked["person"])
        self.assertEqual(self.detail(True, identity), linked["identity"])
        self.assertEqual(self.request("GET", "/v1/identities/%d/people" % identity["id"])[1]["items"], [linked["link"]])

    def test_schema_four_migration_preserves_assets_custody_snapshots_and_sequence(self):
        snapshot = self.schema_four_fixture()
        self.start()
        status, value = self.request("GET", "/v1/assets/41")
        self.assertEqual(status, 200, value)
        asset = value["asset"]
        self.assertEqual((asset["asset_tag"], asset["version"], asset["assignee"]), ("LEGACY", 1, "Mario precedente"))
        self.assertEqual(asset["context"]["assignee_user_id"], 21)
        self.assertEqual(asset["custody"]["id"], 19)
        self.assertEqual(asset["last_verification"]["id"], 23)
        self.assertEqual(self.request("GET", "/v1/assets/41/history")[1]["items"][0]["snapshot"], json.loads(snapshot))
        self.assertEqual(self.request("GET", "/v1/people")[1]["items"], [])
        self.assertEqual(self.request("GET", "/v1/identities")[1]["items"], [])
        self.assertGreater(self.create(tag="NEW")["id"], 100)
        person, identity = self.person(), self.identity()
        self.link(identity, person)
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 6)
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], snapshot)
            self.assertEqual(database.execute("SELECT preview_id FROM inventory_import_previews").fetchone()[0], "a" * 32)
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_schema_four_failed_migration_is_atomic_and_can_be_retried(self):
        self.schema_four_fixture(obstruct=True)
        # start() expects a healthy server; run the native executable directly for failure.
        import subprocess
        env = self.environment()
        failed = subprocess.run([str(fixtures.ROOT / "inventory-addon/build/punto-inventory")], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("Inventory store unavailable", failed.stderr)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 4)
            tables = {row[0] for row in database.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertFalse({"inventory_people", "inventory_identities", "inventory_identity_people", "inventory_entity_history", "inventory_asset_subjects"} & tables)
            self.assertEqual(database.execute("SELECT asset_tag FROM assets WHERE id=41").fetchone()[0], "LEGACY")
            self.assertEqual(database.execute("SELECT user_id FROM asset_custody WHERE id=19").fetchone()[0], 21)
            database.execute("DROP INDEX inventory_identity_people_active_pair")
            database.commit()
        self.start()
        self.assertEqual(self.request("GET", "/v1/people")[0], 200)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 6)
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_methods_routes_and_single_details_reject_ambiguous_requests(self):
        self.start()
        person = self.person()
        identity = self.identity()
        linked = self.link(identity, person)
        for endpoint in ("people", "identities"):
            for identifier in ("0", "-1", "x", "1x", "999999999999999999999999"):
                self.assertEqual(self.request("GET", "/v1/%s/%s" % (endpoint, identifier))[0], 400)
            self.assertEqual(self.request("GET", "/v1/%s/99999" % endpoint)[0], 404)
            self.assertEqual(self.request("GET", "/v1/%s/1?unknown=1" % endpoint)[0], 400)
            self.assertEqual(self.request("GET", "/v1/%s/1/history?active=1" % endpoint)[0], 400)
            self.assertEqual(self.request("GET", "/v1/%s/1/unknown" % endpoint)[0], 404)
            self.assertEqual(self.request("GET", "/v1/%s/1/" % endpoint)[0], 404)
            self.assertEqual(self.request("DELETE", "/v1/%s/1" % endpoint)[0], 405)
            self.assertEqual(self.request("POST", "/v1/%s/1/history" % endpoint, {})[0], 405)
            self.assertEqual(self.request("POST", "/v1/" + endpoint + "?limit=1", {})[0], 400)
            self.assertEqual(self.request("POST", "/v1/" + endpoint, raw="{}", content_type="application/json")[0], 415)
            self.assertEqual(self.request("GET", "/v1/" + endpoint, raw="unexpected")[0], 400)
        self.assertEqual(self.request("POST", "/v1/people/%d/identities" % person["id"], {})[0], 405)
        self.assertEqual(self.request("GET", "/v1/identities/%d/people/%d/remove" % (identity["id"], linked["link"]["id"]))[0], 405)
        self.assertEqual(self.request("POST", "/v1/identities/%d/people/%d/remove" % (identity["id"], linked["link"]["id"]), dict(version=2, actor_id=7, notes="unexpected"))[0], 400)


if __name__ == "__main__":
    unittest.main()
