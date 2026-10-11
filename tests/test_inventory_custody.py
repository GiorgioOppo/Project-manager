"""Structured inventory custody, handovers and physical checks over private TLS."""

from contextlib import closing
from datetime import date, timedelta
import csv
import io
import json
import sqlite3
import unittest
from urllib.parse import urlencode

import test_inventory_addon as fixtures


class InventoryCustodyTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update

    def get(self, asset):
        status, value = self.request("GET", "/v1/assets/%d" % asset["id"])
        self.assertEqual(status, 200, value)
        return value["asset"]

    def history(self, asset):
        status, value = self.request("GET", "/v1/assets/%d/history" % asset["id"])
        self.assertEqual(status, 200, value)
        return value["items"]

    def place(self, kind, name, parent=None):
        fields = {"kind": kind, "name": name, "actor_id": 7}
        if parent:
            fields["parent_id"] = parent["id"]
        status, value = self.request("POST", "/v1/places", fields)
        self.assertEqual(status, 201, value)
        return value["place"]

    def place_update(self, place, **fields):
        data = {key: place[key] for key in ("kind", "name", "version")}
        data.update(actor_id=8, parent_id=place["parent_id"] or "", active=int(place["active"]))
        data.update(fields)
        return self.request("POST", "/v1/places/%d" % place["id"], data)

    def act(self, asset, operation, **fields):
        data = {"version": asset["version"], "actor_id": 11}
        data.update(fields)
        return self.request("POST", "/v1/assets/%d/%s" % (asset["id"], operation), data)

    def assign_context(self, asset, user=None, site=None, department=None, location=None, user_name="Mario Rossi"):
        status, value = self.act(asset, "context", assignee_user_id=user or "", assignee_user_name=user_name if user else "",
                                 site_id=site["id"] if site else "", department_id=department["id"] if department else "",
                                 location_id=location["id"] if location else "")
        self.assertEqual(status, 200, value)
        return value["asset"]

    def checkout(self, asset, **fields):
        data = {"user_id": 21, "user_name": "Mario Rossi", "checkout_on": date.today().isoformat(), "condition_out": "good"}
        data.update(fields)
        status, value = self.act(asset, "checkout", **data)
        self.assertEqual(status, 201, value)
        return value["asset"], value["custody"]

    def test_places_hierarchy_uniqueness_versions_soft_deactivation_and_pagination(self):
        self.start()
        site = self.place("site", "Roma")
        department = self.place("department", "Tecnica", site)
        location = self.place("location", "Armadio A", site)
        self.place("department", "Globale")
        other = self.place("site", "Milano")
        self.place("location", "Armadio A", other)
        status, page = self.request("GET", "/v1/places?kind=location&limit=1")
        self.assertEqual(status, 200, page)
        self.assertEqual(len(page["places"]), 1)
        cursor = page["next_after_id"]
        self.assertIsInstance(cursor, int)
        status, next_page = self.request("GET", "/v1/places?" + urlencode({"kind": "location", "limit": 1, "after_id": cursor}))
        self.assertEqual(next_page["places"][0]["id"], location["id"])
        self.assertIsNone(next_page["next_after_id"])
        bad = [{"kind": "site", "name": "Invalid", "parent_id": site["id"]},
               {"kind": "location", "name": "Invalid"},
               {"kind": "location", "name": "Invalid", "parent_id": department["id"]},
               {"kind": "department", "name": "Invalid", "parent_id": location["id"]},
               {"kind": "unknown", "name": "Invalid"}, {"kind": "site", "name": " "},
               {"kind": "site", "name": "a" * 161}, {"kind": "site", "name": "New", "active": "true"}]
        for fields in bad:
            with self.subTest(fields=fields):
                self.assertEqual(self.request("POST", "/v1/places", dict(fields, actor_id=1))[0], 400)
        self.assertEqual(self.request("POST", "/v1/places", {"kind": "site", "name": "roma", "actor_id": 1})[0], 409)
        status, updated = self.place_update(site, name="Roma centro", active=0)
        self.assertEqual(status, 200, updated)
        self.assertFalse(updated["place"]["active"])
        self.assertEqual(updated["place"]["version"], 2)
        self.assertEqual(self.place_update(site, name="Stale")[0], 409)
        self.assertEqual(self.place_update(updated["place"], kind="department")[0], 409)
        self.assertEqual(self.request("POST", "/v1/places", {"kind": "location", "name": "New", "parent_id": site["id"], "actor_id": 1})[0], 400)
        status, inactive = self.request("GET", "/v1/places?active=0")
        self.assertEqual([p["id"] for p in inactive["places"]], [site["id"]])

    def test_structured_assignment_filter_export_snapshots_and_legacy_guards(self):
        self.start()
        site = self.place("site", "Roma")
        department = self.place("department", "Tecnica", site)
        location = self.place("location", "Scaffale 1", site)
        asset = self.create()
        self.assertIsNone(asset["context"])
        assigned = self.assign_context(asset, user=21, site=site, department=department, location=location)
        self.assertEqual((assigned["status"], assigned["assignee"], assigned["location"]), ("assigned", "Mario Rossi", "Scaffale 1"))
        self.assertEqual(assigned["context"]["assignee_user_id"], 21)
        self.assertEqual(assigned["version"], asset["version"] + 1)
        self.assertEqual(self.history(asset)[0]["change"]["operation"], "context_updated")
        self.assertIsNone(self.history(asset)[-1]["snapshot"]["context"])
        self.create(tag="OTHER")
        status, mine = self.request("GET", "/v1/assets?assignee_user_id=21")
        self.assertEqual([a["id"] for a in mine["items"]], [assigned["id"]])
        status, export = self.request("GET", "/v1/assets/export?assignee_user_id=21")
        self.assertEqual((status, export["exported"]), (200, 1))
        self.assertNotIn("OTHER", export["csv"])
        self.assertEqual(self.update(assigned, status="available", assignee="Mario Rossi", location="Scaffale 1")[0], 409)
        self.assertEqual(self.update(assigned, status="assigned", assignee="Fake", location="Scaffale 1")[0], 409)
        self.assertEqual(self.update(assigned, status="assigned", assignee="Mario Rossi", location="Fake")[0], 409)
        self.assertEqual(self.update(assigned, status="assigned", assignee="Mario Rossi", location="Scaffale 1", asset_type="software")[0], 409)
        status, edit = self.update(assigned, status="assigned", assignee="Mario Rossi", location="Scaffale 1", notes="Ammesso")
        self.assertEqual(status, 200, edit)
        assigned = edit["asset"]
        self.assertEqual(self.place_update(location, name="Nome nuovo")[0], 200)
        self.assertEqual(self.get(asset)["context"]["location_name"], "Scaffale 1")
        cleared = self.assign_context(assigned)
        self.assertEqual((cleared["status"], cleared["assignee"], cleared["location"]), ("available", "", ""))
        self.assertIsNone(cleared["context"]["assignee_user_id"])
        self.assertEqual(self.request("GET", "/v1/assets?assignee_user_id=21")[1]["items"], [])

    def test_context_rejects_wrong_place_kind_site_and_unavailable_assignment(self):
        self.start()
        site = self.place("site", "Roma")
        other = self.place("site", "Milano")
        department = self.place("department", "Tecnica", other)
        location = self.place("location", "Scaffale", other)
        asset = self.create()
        for fields in ({"site_id": location["id"]}, {"site_id": site["id"], "department_id": department["id"]},
                       {"site_id": site["id"], "location_id": location["id"]}, {"location_id": location["id"]},
                       {"assignee_user_id": 1, "assignee_user_name": ""}, {"assignee_user_name": "Fake"},
                       {"assignee_user_id": -1}, {"unknown": 1}):
            with self.subTest(fields=fields):
                self.assertEqual(self.act(asset, "context", **fields)[0], 400)
        for state in ("maintenance", "retired", "lost"):
            row = self.create(tag=state, status=state)
            self.assertEqual(self.act(row, "context", assignee_user_id=1, assignee_user_name="Mario")[0], 409)
            row = self.assign_context(row, site=site)
            self.assertEqual(row["status"], state)
        software = self.create(tag="SW", asset_type="software")
        self.assertEqual(self.act(software, "context")[0], 400)
        self.assertEqual(self.get(asset)["version"], asset["version"])

    def test_checkout_checkin_audit_and_legacy_location_preserved(self):
        self.start()
        asset = self.create(location="Magazzino precedente")
        checked, custody = self.checkout(asset, notes="Consegna completa")
        self.assertEqual((checked["status"], checked["assignee"], checked["location"]), ("assigned", "Mario Rossi", "Magazzino precedente"))
        self.assertEqual(custody["notes"], "Consegna completa")
        self.assertTrue(custody["active"])
        self.assertIsNone(custody["returned_on"])
        self.assertEqual(self.act(checked, "checkout", user_id=21, user_name="Mario Rossi", checkout_on=date.today().isoformat(), condition_out="good")[0], 409)
        self.assertEqual(self.act(checked, "context", assignee_user_id=22, assignee_user_name="Altro")[0], 409)
        status, returned = self.act(checked, "checkin", returned_on=date.today().isoformat(), condition_in="damaged", status="maintenance", notes="Cerniera rotta")
        self.assertEqual(status, 200, returned)
        row = returned["asset"]
        self.assertEqual((row["status"], row["assignee"], row["location"]), ("maintenance", "", "Magazzino precedente"))
        self.assertIsNone(row["custody"])
        self.assertIsNone(row["context"]["assignee_user_id"])
        self.assertFalse(returned["custody"]["active"])
        self.assertEqual(returned["custody"]["return_notes"], "Cerniera rotta")
        self.assertEqual(returned["custody"]["returned_actor_id"], 11)
        self.assertEqual(self.act(row, "checkin", returned_on=date.today().isoformat(), condition_in="good")[0], 409)
        status, log = self.request("GET", "/v1/assets/%d/custody" % row["id"])
        self.assertEqual((status, log["custody"]), (200, [returned["custody"]]))
        events = self.history(row)
        self.assertEqual([e.get("change", {}).get("operation") for e in events[:2]], ["checked_in", "checked_out"])
        self.assertTrue(events[1]["change"]["custody"]["active"])
        self.assertEqual(events[1]["snapshot"]["custody"], custody)
        self.start()
        self.assertEqual(self.get(row), row)

    def test_checkout_same_structured_assignee_and_context_movement_during_custody(self):
        self.start()
        site = self.place("site", "Roma")
        first = self.place("location", "A", site)
        second = self.place("location", "B", site)
        row = self.assign_context(self.create(), user=21, site=site, location=first)
        self.assertEqual(self.act(row, "checkout", user_id=22, user_name="Altro", checkout_on=date.today().isoformat(), condition_out="good")[0], 409)
        checked, custody = self.checkout(row)
        moved = self.assign_context(checked, user=21, site=site, location=second)
        self.assertEqual(moved["location"], "B")
        self.assertEqual(moved["custody"]["location_name"], "A")
        self.assertEqual(moved["custody"]["id"], custody["id"])
        self.assertEqual(self.act(moved, "context", site_id=site["id"])[0], 409)
        legacy = self.create(tag="LEGACY", status="assigned", assignee="Mario Rossi")
        self.assertEqual(self.act(legacy, "checkout", user_id=21, user_name="Mario Rossi", checkout_on=date.today().isoformat(), condition_out="good")[0], 409)

    def test_verifications_snapshots_match_mismatch_unknown_and_no_automatic_move(self):
        self.start()
        site = self.place("site", "Roma")
        first = self.place("location", "A", site)
        second = self.place("location", "B", site)
        row = self.assign_context(self.create(), site=site, location=first)
        records = []
        for observed, match in ((first, True), (second, False), (None, None)):
            status, result = self.act(row, "verify", verified_on=date.today().isoformat(), condition="fair", observed_location_id=observed["id"] if observed else "", notes="Controllato")
            self.assertEqual(status, 201, result)
            row = result["asset"]
            self.assertEqual(row["location"], "A")
            self.assertEqual(result["verification"]["location_matches"], match)
            self.assertEqual(result["verification"]["expected_location_name"], "A")
            records.append(result["verification"])
        self.assertEqual(row["last_verification"], records[-1])
        status, page = self.request("GET", "/v1/assets/%d/verifications?limit=2" % row["id"])
        self.assertEqual((status, page["verifications"]), (200, list(reversed(records))[:2]))
        status, page2 = self.request("GET", "/v1/assets/%d/verifications?limit=2&after_id=%d" % (row["id"], page["next_after_id"]))
        self.assertEqual(page2["verifications"], [records[0]])
        self.assertIsNone(page2["next_after_id"])
        self.assertEqual(self.place_update(first, name="Nuovo")[0], 200)
        self.assertEqual(self.get(row)["last_verification"]["expected_location_name"], "A")
        no_context = self.create(tag="NO-LOCATION", location="Solo testo")
        status, result = self.act(no_context, "verify", verified_on=date.today().isoformat(), condition="good", observed_location_id=second["id"])
        self.assertEqual(status, 201, result)
        self.assertIsNone(result["verification"]["location_matches"])
        self.assertEqual(result["asset"]["location"], "Solo testo")
        self.assertIsNone(result["asset"]["context"])

    def test_invalid_dates_conditions_stale_versions_and_atomic_history_failure(self):
        self.start()
        row = self.create()
        future = (date.today() + timedelta(days=1)).isoformat()
        for fields in ({"checkout_on": future}, {"checkout_on": "2026-02-30"}, {"checkout_on": ""},
                       {"due_on": "0000-01-01"}, {"checkout_on": "2026-01-02", "due_on": "2026-01-01"},
                       {"condition_out": "broken"}, {"user_name": "x" * 481}, {"notes": "x" * 2001},
                       {"user_id": 0}, {"version": row["version"] + 1}):
            data = {"user_id": 21, "user_name": "Mario", "checkout_on": date.today().isoformat(), "condition_out": "good"}
            data.update(fields)
            status, value = self.act(row, "checkout", **data)
            self.assertEqual(status, 409 if "version" in fields else 400, value)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER custody_history_failure BEFORE INSERT ON asset_history BEGIN SELECT RAISE(ABORT,'forced'); END")
            database.commit()
        status, value = self.act(row, "checkout", user_id=21, user_name="Mario", checkout_on=date.today().isoformat(), condition_out="good")
        self.assertEqual(status, 500, value)
        self.assertEqual(self.get(row), row)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_custody").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_context").fetchone()[0], 0)
            database.execute("DROP TRIGGER custody_history_failure")
            database.commit()
        checked, _ = self.checkout(row, checkout_on="2026-01-02")
        for fields in ({"returned_on": "2026-01-01"}, {"returned_on": future}, {"condition_in": "broken"}, {"status": "retired"}):
            data = {"returned_on": date.today().isoformat(), "condition_in": "good"}
            data.update(fields)
            self.assertEqual(self.act(checked, "checkin", **data)[0], 400)
        self.assertEqual(self.get(row), checked)

    def test_long_unicode_snapshot_is_validly_truncated_only_in_legacy_display(self):
        self.start()
        name = "è" * 239
        row = self.assign_context(self.create(), user=21, user_name=name)
        self.assertEqual(row["context"]["assignee_user_name"], name)
        self.assertLessEqual(len(row["assignee"].encode("utf-8")), 160)
        self.assertEqual(row["assignee"], "è" * 80)
        # Without a structured location, legacy free text remains editable.
        status, result = self.update(row, status="assigned", assignee=row["assignee"], location="Ubicazione libera")
        self.assertEqual(status, 200, result)
        checked, _ = self.checkout(result["asset"], user_name=name)
        self.assertEqual(checked["location"], "Ubicazione libera")
        self.assertEqual(self.update(checked, status="assigned", assignee=checked["assignee"], location="Changed")[0], 409)

    def test_place_only_and_empty_context_keep_legacy_editor_compatible(self):
        self.start()
        site = self.place("site", "Roma")
        placed = self.assign_context(self.create(), site=site)
        status, result = self.update(placed, status="assigned", assignee="Nome libero", location="Roma")
        self.assertEqual(status, 200, result)
        self.assertIsNone(result["asset"]["context"]["assignee_user_id"])
        self.assertEqual(self.update(result["asset"], status="assigned", assignee="Nome libero", location="Milano")[0], 409)
        empty = self.assign_context(result["asset"])
        status, result = self.update(empty, status="assigned", assignee="Legacy", location="Testo libero", asset_type="software")
        self.assertEqual(status, 200, result)
        self.assertEqual(result["asset"]["location"], "Testo libero")

    def test_csv_cannot_replace_structured_assignment_or_open_custody(self):
        self.start()
        row = self.assign_context(self.create(), user=21)
        status, exported = self.request("GET", "/v1/assets/export")
        self.assertEqual(status, 200)
        records = list(csv.reader(io.StringIO(exported["csv"])))
        records[1][records[0].index("assignee")] = "Sostituito dal CSV"
        stream = io.StringIO()
        csv.writer(stream).writerows(records)
        status, preview = self.request("POST", "/v1/import/preview", {"csv": stream.getvalue(), "mode": "upsert", "actor_id": 11})
        self.assertEqual(status, 200, preview)
        self.assertFalse(preview["can_commit"])
        self.assertEqual(preview["summary"]["errors"], 1)
        self.assertEqual(self.get(row), row)
        checked, _ = self.checkout(row)
        stream = io.StringIO()
        records[1][records[0].index("version")] = str(checked["version"])
        records[1][records[0].index("assignee")] = checked["assignee"]
        records[1][records[0].index("status")] = "retired"
        csv.writer(stream).writerows(records)
        status, preview = self.request("POST", "/v1/import/preview", {"csv": stream.getvalue(), "mode": "upsert", "actor_id": 11})
        self.assertEqual(status, 200, preview)
        self.assertFalse(preview["can_commit"])
        self.assertEqual(self.get(row), checked)

    def test_custody_pagination_and_failed_checkin_context_verification_roll_back(self):
        self.start()
        site = self.place("site", "Roma")
        place = self.place("location", "A", site)
        row = self.assign_context(self.create(), site=site, location=place)
        closed = []
        for _ in range(3):
            row, _ = self.checkout(row)
            status, result = self.act(row, "checkin", returned_on=date.today().isoformat(), condition_in="good")
            self.assertEqual(status, 200, result)
            row = result["asset"]
            closed.append(result["custody"])
        status, page = self.request("GET", "/v1/assets/%d/custody?limit=2" % row["id"])
        self.assertEqual((status, page["custody"]), (200, list(reversed(closed))[:2]))
        status, rest = self.request("GET", "/v1/assets/%d/custody?limit=2&after_id=%d" % (row["id"], page["next_after_id"]))
        self.assertEqual(rest["custody"], [closed[0]])
        row, _ = self.checkout(row)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER custody_history_failure BEFORE INSERT ON asset_history BEGIN SELECT RAISE(ABORT,'forced'); END")
            database.commit()
        for operation, fields in (("checkin", {"returned_on": date.today().isoformat(), "condition_in": "good"}),
                                  ("context", {"assignee_user_id": 21, "assignee_user_name": "Nome nuovo", "site_id": site["id"]}),
                                  ("verify", {"verified_on": date.today().isoformat(), "condition": "good", "observed_location_id": place["id"]})):
            with self.subTest(operation=operation):
                self.assertEqual(self.act(row, operation, **fields)[0], 500)
                self.assertEqual(self.get(row), row)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_verifications").fetchone()[0], 0)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_custody WHERE active=1").fetchone()[0], 1)
            self.assertEqual(database.execute("SELECT COUNT(*) FROM asset_custody WHERE returned_at IS NOT NULL").fetchone()[0], 3)

    def test_endpoint_methods_query_validation_and_inactive_places_preserve_history(self):
        self.start()
        site = self.place("site", "Roma")
        place = self.place("location", "A", site)
        row = self.assign_context(self.create(), site=site, location=place)
        status, inactive = self.place_update(place, active=0)
        self.assertEqual(status, 200, inactive)
        # Existing context can retain a historical place, but it cannot be assigned afresh.
        row = self.assign_context(row, site=site, location=place)
        fresh = self.create(tag="FRESH")
        self.assertEqual(self.act(fresh, "context", site_id=site["id"], location_id=place["id"])[0], 400)
        self.assertEqual(self.act(row, "verify", verified_on=date.today().isoformat(), condition="good", observed_location_id=place["id"])[0], 400)
        for suffix in ("context", "checkout", "checkin", "verify"):
            self.assertEqual(self.request("GET", "/v1/assets/%d/%s" % (row["id"], suffix))[0], 405)
        for suffix in ("custody", "verifications"):
            self.assertEqual(self.request("POST", "/v1/assets/%d/%s" % (row["id"], suffix), {})[0], 405)
            self.assertEqual(self.request("GET", "/v1/assets/%d/%s?unknown=1" % (row["id"], suffix))[0], 400)
            self.assertEqual(self.request("GET", "/v1/assets/9999/%s" % suffix)[0], 404)
        for query in ("kind=unknown", "active=true", "limit=101", "after_id=0", "unknown=1", "kind=site&kind=site"):
            self.assertEqual(self.request("GET", "/v1/places?" + query)[0], 400)
        self.assertEqual(self.request("GET", "/v1/assets?assignee_user_id=0")[0], 400)
        self.assertEqual(self.request("GET", "/v1/assets/export?assignee_user_id=-1")[0], 400)

    def test_place_reparent_rejects_current_context_and_preserves_historical_snapshots(self):
        self.start()
        site = self.place("site", "Roma")
        other = self.place("site", "Milano")
        department = self.place("department", "Tecnica", site)
        location = self.place("location", "A", site)
        row = self.assign_context(self.create(), site=site, department=department, location=location)
        for place in (department, location):
            self.assertEqual(self.place_update(place, parent_id=other["id"])[0], 409)
        self.assertEqual(self.get(row), row)
        row = self.assign_context(row)
        status, moved = self.place_update(location, parent_id=other["id"])
        self.assertEqual(status, 200, moved)
        self.assertEqual(moved["place"]["parent_id"], other["id"])
        previous = self.history(row)[1]["snapshot"]
        self.assertEqual((previous["context"]["site_name"], previous["context"]["location_name"]), ("Roma", "A"))

    def test_schema_three_migration_preserves_legacy_rows_snapshots_and_previews(self):
        snapshot = {"id": 41, "asset_tag": "LEGACY", "name": "Computer storico", "category": "hardware",
                    "serial_number": "OLD", "manufacturer": "", "model": "", "status": "assigned",
                    "location": "Roma", "assignee": "Mario precedente", "purchase_date": "", "warranty_until": "",
                    "notes": "Dati precedenti", "actor_id": 7, "version": 1,
                    "created_at": "2026-01-02T03:04:05Z", "updated_at": "2026-01-02T03:04:05Z",
                    "asset_type": "it_device", "details": {}}
        serialized = json.dumps(snapshot, ensure_ascii=False)
        with closing(sqlite3.connect(self.db)) as database:
            database.executescript("""
                CREATE TABLE assets(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE,name TEXT NOT NULL,category TEXT NOT NULL,serial_number TEXT NOT NULL,manufacturer TEXT NOT NULL,model TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('available','assigned','maintenance','retired','lost')),location TEXT NOT NULL,assignee TEXT NOT NULL,purchase_date TEXT NOT NULL,warranty_until TEXT NOT NULL,notes TEXT NOT NULL,actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL CHECK(version>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,asset_type TEXT NOT NULL DEFAULT 'it_device' CHECK(asset_type IN ('it_device','network_device','physical','software','license')),details TEXT NOT NULL DEFAULT '{}');
                CREATE TABLE asset_history(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),action TEXT NOT NULL CHECK(action IN ('created','updated')),actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL,created_at TEXT NOT NULL,snapshot TEXT NOT NULL,change TEXT,UNIQUE(asset_id,version));
                CREATE TABLE asset_relations(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER NOT NULL REFERENCES assets(id),target_id INTEGER REFERENCES assets(id),relation_type TEXT NOT NULL CHECK(relation_type IN ('installed_on','connected_to','licensed_to')),assignee TEXT NOT NULL DEFAULT '' COLLATE NOCASE,quantity INTEGER NOT NULL CHECK(quantity BETWEEN 1 AND 1000000),installed_version TEXT NOT NULL DEFAULT '',source_port TEXT NOT NULL DEFAULT '',target_port TEXT NOT NULL DEFAULT '',notes TEXT NOT NULL DEFAULT '',actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0,1)),removed_at TEXT,removed_actor_id INTEGER);
                CREATE UNIQUE INDEX active_relation_target ON asset_relations(source_id,relation_type,target_id) WHERE active=1 AND target_id IS NOT NULL;
                CREATE UNIQUE INDEX active_relation_assignee ON asset_relations(source_id,relation_type,assignee COLLATE NOCASE) WHERE active=1 AND target_id IS NULL;
                CREATE INDEX relation_target_id ON asset_relations(target_id,id);
                CREATE INDEX relation_source_id ON asset_relations(source_id,id);
                CREATE TABLE inventory_import_previews(preview_id TEXT PRIMARY KEY,actor_id INTEGER NOT NULL CHECK(actor_id>0),created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,used INTEGER NOT NULL DEFAULT 0 CHECK(used IN (0,1)),plan TEXT NOT NULL);
                CREATE INDEX inventory_import_previews_expiry ON inventory_import_previews(expires_at);
                PRAGMA user_version=3;
            """)
            database.execute("INSERT INTO assets VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                             [json.dumps(value) if key == "details" else value for key, value in snapshot.items()])
            database.execute("INSERT INTO asset_history(id,asset_id,action,actor_id,version,created_at,snapshot) VALUES(77,41,'created',7,1,?,?)",
                             (snapshot["created_at"], serialized))
            database.execute("INSERT INTO inventory_import_previews VALUES(?,7,1,2,0,'[]')", ("a" * 32,))
            database.execute("UPDATE sqlite_sequence SET seq=100 WHERE name='assets'")
            database.commit()
        self.start()
        migrated = self.get(snapshot)
        for key, value in snapshot.items():
            self.assertEqual(migrated[key], value)
        self.assertEqual(self.history(snapshot)[0]["snapshot"], snapshot)
        self.assertGreater(self.create(tag="NEW")["id"], 100)
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], serialized)
            self.assertEqual(database.execute("SELECT preview_id FROM inventory_import_previews").fetchone()[0], "a" * 32)
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])


if __name__ == "__main__":
    unittest.main()
