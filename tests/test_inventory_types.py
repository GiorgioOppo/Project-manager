"""Typed inventory, manual relations and v1 migration over the private TLS API."""

from contextlib import closing
from datetime import date, timedelta
import json
import sqlite3
import subprocess
import unittest
from urllib.parse import urlencode

import test_inventory_addon as fixtures


class InventoryTypesTests(unittest.TestCase):
    # Reuse only transport/fixture helpers; do not inherit the original test cases.
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update

    def typed(self, tag, asset_type, details=None, **fields):
        fields["asset_type"] = asset_type
        if details is not None:
            fields["details"] = json.dumps(details, ensure_ascii=False)
        return self.create(tag=tag, **fields)

    def get(self, asset):
        status, value = self.request("GET", "/v1/assets/%d" % asset["id"])
        self.assertEqual(status, 200, value)
        return value["asset"]

    def history(self, asset):
        status, value = self.request("GET", "/v1/assets/%d/history" % asset["id"])
        self.assertEqual(status, 200, value)
        return value["items"]

    def add_relation(self, source, relation_type, target=None, **fields):
        data = {"relation_type": relation_type, "version": source["version"], "actor_id": 31}
        if target is not None:
            data["target_id"] = target["id"]
        data.update(fields)
        return self.request("POST", "/v1/assets/%d/relations" % source["id"], data)

    def relate(self, source, relation_type, target=None, **fields):
        status, value = self.add_relation(source, relation_type, target, **fields)
        self.assertEqual(status, 201, value)
        self.assertEqual(value["asset"]["version"], source["version"] + 1)
        return value["asset"], value["relation"]

    def remove_relation(self, source, relation, **fields):
        data = {"version": source["version"], "actor_id": 32}
        data.update(fields)
        return self.request("POST", "/v1/assets/%d/relations/%d/remove" % (source["id"], relation["id"]), data)

    def relations(self, asset, query=""):
        status, value = self.request("GET", "/v1/assets/%d/relations%s" % (asset["id"], query))
        self.assertEqual(status, 200, value)
        return value

    def test_type_specific_fields_and_license_defaults_round_trip(self):
        self.start()
        rows = [
            self.typed("IT", "it_device", {"hostname": "pc-roma", "ip_address": "2001:db8::1", "mac_address": "AA:BB:CC:DD:EE:FF", "operating_system": "Linux"}),
            self.typed("NET", "network_device", {"hostname": "core", "ip_address": "192.0.2.1", "mac_address": "00:11:22:33:44:55", "operating_system": "NOS", "network_role": "switch", "vlan": 4094, "port_count": 65535}),
            self.typed("PHYS", "physical", {"asset_kind": "trapano", "department": "Officina", "maintenance_due": "2028-02-29"}),
            self.typed("SW", "software", {"publisher": "Editore", "software_version": "3.2", "edition": "Enterprise", "software_kind": "CAD"}),
        ]
        self.assertEqual(rows[0]["details"], {"hostname": "pc-roma", "ip_address": "2001:db8::1", "mac_address": "aa:bb:cc:dd:ee:ff", "operating_system": "Linux"})
        self.assertEqual(rows[1]["details"], {"hostname": "core", "ip_address": "192.0.2.1", "mac_address": "00:11:22:33:44:55", "operating_system": "NOS", "network_role": "switch", "vlan": 4094, "port_count": 65535})
        self.assertEqual(rows[2]["details"], {"asset_kind": "trapano", "department": "Officina", "maintenance_due": "2028-02-29"})
        self.assertEqual(rows[3]["details"], {"publisher": "Editore", "software_version": "3.2", "edition": "Enterprise", "software_kind": "CAD"})
        license_asset = self.typed("LIC", "license", {"software_id": rows[3]["id"], "seats_total": 8, "license_reference": "Ordine-2026-42"})
        rows.append(license_asset)
        self.assertEqual(license_asset["details"]["license_model"], "per_device")
        self.assertEqual(license_asset["details"]["renewal_mode"], "manual")
        self.assertEqual(license_asset["license_usage"], {"allocated": 0, "available": 8, "expired": False})
        for asset in rows:
            with self.subTest(asset_type=asset["asset_type"]):
                self.assertEqual(self.get(asset), asset)
                self.assertEqual(self.history(asset)[0]["snapshot"], asset)
        self.start()
        for asset in rows:
            self.assertEqual(self.get(asset), asset)

    def test_typed_validation_rejects_wrong_fields_and_values_without_writes(self):
        self.start()
        software = self.typed("BASE-SW", "software")
        invalid = [
            ("it_device", {"hostname": "a" * 254}),
            ("it_device", {"hostname": "riga\nnuova"}),
            ("it_device", {"operating_system": "a" * 121}),
            ("it_device", {"ip_address": "999.0.0.1"}),
            ("it_device", {"ip_address": "2001:db8:::1"}),
            ("it_device", {"mac_address": "001122334455"}),
            ("it_device", {"mac_address": "gg:11:22:33:44:55"}),
            ("it_device", {"vlan": 1}),
            ("network_device", {"network_role": "a" * 81}),
            ("network_device", {"vlan": 0}),
            ("network_device", {"vlan": 4095}),
            ("network_device", {"vlan": "12"}),
            ("network_device", {"vlan": True}),
            ("network_device", {"port_count": -1}),
            ("network_device", {"port_count": 65536}),
            ("network_device", {"port_count": 1.5}),
            ("physical", {"asset_kind": "a" * 81}),
            ("physical", {"department": "a" * 121}),
            ("physical", {"maintenance_due": "2027-02-29"}),
            ("physical", {"hostname": "not-a-physical-field"}),
            ("software", {"publisher": "a" * 121}),
            ("software", {"software_version": "a" * 81}),
            ("software", {"edition": "a" * 81}),
            ("software", {"software_kind": "a" * 81}),
            ("software", {"publisher": {"nested": "value"}}),
            ("software", {"software_version": ["1"]}),
            ("license", {}),
            ("license", {"software_id": software["id"], "seats_total": 0}),
            ("license", {"software_id": software["id"], "seats_total": 1000001}),
            ("license", {"software_id": software["id"], "seats_total": True}),
            ("license", {"software_id": software["id"], "seats_total": "3"}),
            ("license", {"software_id": software["id"], "seats_total": 3, "license_model": "floating"}),
            ("license", {"software_id": software["id"], "seats_total": 3, "renewal_mode": "weekly"}),
            ("license", {"software_id": software["id"], "seats_total": 3, "expires_on": "2027-02-29"}),
            ("license", {"software_id": software["id"], "seats_total": 3, "renewal_on": "0000-01-01"}),
            ("license", {"software_id": software["id"], "seats_total": 3, "license_reference": "a" * 121}),
            ("license", {"software_id": software["id"], "seats_total": 3, "license_key": "secret"}),
        ]
        for index, (asset_type, details) in enumerate(invalid):
            with self.subTest(asset_type=asset_type, details=details):
                status, value = self.request("POST", "/v1/assets", {"asset_tag": "BAD-%d" % index, "name": "Invalid", "actor_id": 1, "asset_type": asset_type, "details": json.dumps(details)})
                self.assertEqual(status, 400, value)
        self.assertEqual(self.request("GET", "/v1/status")[1]["summary"]["total"], 1)

    def test_details_json_is_strict_and_duplicate_keys_are_rejected(self):
        self.start()
        for raw in ("[]", "null", '"text"', "{} trailing", '{"hostname":"one","hostname":"two"}', '{"hostname":"one","host\\u006eame":"two"}', '{"unknown":1}', '{"hostname":false}', '{"hostname":"\\u0000"}', '{"hostname":"unterminated}', " " * 4095 + "{}"):
            with self.subTest(raw=raw):
                status, value = self.request("POST", "/v1/assets", {"asset_tag": "BAD", "name": "Invalid", "actor_id": 1, "asset_type": "it_device", "details": raw})
                self.assertEqual(status, 400, value)
        for asset_type in ("", "computer", "IT_DEVICE"):
            self.assertEqual(self.request("POST", "/v1/assets", {"asset_tag": "BAD", "name": "Invalid", "actor_id": 1, "asset_type": asset_type})[0], 400)
        self.assertEqual(self.request("GET", "/v1/status")[1]["summary"]["total"], 0)

    def test_legacy_update_preserves_type_details_but_replaces_common_fields(self):
        self.start()
        legacy = self.create(tag="LEGACY")
        self.assertEqual((legacy["asset_type"], legacy["details"]), ("it_device", {}))
        asset = self.typed("TYPED", "network_device", {"hostname": "edge", "vlan": 2, "port_count": 0}, location="Roma", serial_number="SERIAL")
        status, value = self.update(asset, name="Updated by old client")
        self.assertEqual(status, 200, value)
        updated = value["asset"]
        self.assertEqual(updated["asset_type"], asset["asset_type"])
        self.assertEqual(updated["details"], asset["details"])
        self.assertEqual((updated["location"], updated["serial_number"]), ("", ""))
        status, value = self.update(updated, details=json.dumps({"hostname": "new-edge"}))
        self.assertEqual(status, 200, value)
        self.assertEqual(value["asset"]["details"]["hostname"], "new-edge")
        self.assertNotIn("vlan", value["asset"]["details"])
        self.assertEqual(self.history(asset)[-1]["snapshot"], asset)

    def test_type_filter_and_literal_search_include_typed_fields(self):
        self.start()
        pc = self.typed("PC", "it_device", {"hostname": "pc-unique", "ip_address": "192.0.2.77"})
        net = self.typed("NET", "network_device", {"network_role": "core-unique"})
        physical = self.typed("PHYS", "physical", {"department": "Lab-unique"})
        software = self.typed("SW", "software", {"publisher": 'Publisher-unique "Studio"', "edition": "100%_edition"})
        lic = self.typed("LIC", "license", {"software_id": software["id"], "seats_total": 1, "license_reference": "REF-unique"})
        for query, expected in (("asset_type=network_device", net), ("q=PC-UNIQUE", pc), ("q=192.0.2.77", pc), ("q=core-unique", net), ("q=Lab-unique", physical), ("q=Publisher-unique", software), (urlencode({"q": 'unique "Studio"'}), software), ("q=REF-unique", lic), ("q=100%25_edition", software)):
            with self.subTest(query=query):
                status, value = self.request("GET", "/v1/assets?" + query)
                self.assertEqual(status, 200, value)
                self.assertEqual([item["id"] for item in value["items"]], [expected["id"]])
                self.assertEqual(value["summary"]["total"], 5)
        self.assertEqual(self.request("GET", "/v1/assets?asset_type=software&q=pc-unique")[1]["items"], [])
        for query in ("asset_type=", "asset_type=bad", "asset_type=software&asset_type=license"):
            self.assertEqual(self.request("GET", "/v1/assets?" + query)[0], 400)

    def test_schema_one_migration_keeps_ids_rows_and_original_snapshots(self):
        # This is the actual previous schema, not a v2 database with its pragma changed.
        snapshot = {"id": 41, "asset_tag": "OLD-PC", "name": "PC originale", "category": "hardware", "serial_number": "SER", "manufacturer": "", "model": "", "status": "available", "location": "Roma", "assignee": "", "purchase_date": "", "warranty_until": "", "notes": "Originale", "actor_id": 7, "version": 1, "created_at": "2026-01-02T03:04:05Z", "updated_at": "2026-01-02T03:04:05Z"}
        with closing(sqlite3.connect(self.db)) as database:
            database.executescript("""
                CREATE TABLE assets(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_tag TEXT NOT NULL COLLATE NOCASE UNIQUE,name TEXT NOT NULL,category TEXT NOT NULL,serial_number TEXT NOT NULL,manufacturer TEXT NOT NULL,model TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('available','assigned','maintenance','retired','lost')),location TEXT NOT NULL,assignee TEXT NOT NULL,purchase_date TEXT NOT NULL,warranty_until TEXT NOT NULL,notes TEXT NOT NULL,actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL CHECK(version>0),created_at TEXT NOT NULL,updated_at TEXT NOT NULL);
                CREATE TABLE asset_history(id INTEGER PRIMARY KEY AUTOINCREMENT,asset_id INTEGER NOT NULL REFERENCES assets(id),action TEXT NOT NULL CHECK(action IN ('created','updated')),actor_id INTEGER NOT NULL CHECK(actor_id>0),version INTEGER NOT NULL,created_at TEXT NOT NULL,snapshot TEXT NOT NULL,UNIQUE(asset_id,version));
                CREATE INDEX assets_status_id ON assets(status,id);
                CREATE INDEX asset_history_asset_id ON asset_history(asset_id,id);
                PRAGMA user_version=1;
            """)
            columns = list(snapshot)
            database.execute("INSERT INTO assets(%s) VALUES(%s)" % (",".join(columns), ",".join("?" for _ in columns)), [snapshot[key] for key in columns])
            serialized = json.dumps(snapshot, ensure_ascii=False)
            database.execute("INSERT INTO asset_history(id,asset_id,action,actor_id,version,created_at,snapshot) VALUES(77,41,'created',7,1,?,?)", (snapshot["created_at"], serialized))
            database.commit()
        self.start()
        migrated = self.get(snapshot)
        self.assertEqual(migrated, dict(snapshot, asset_type="it_device", details={}, context=None, custody=None, last_verification=None))
        old_event = self.history(snapshot)[0]
        self.assertEqual((old_event["id"], old_event["snapshot"]), (77, snapshot))
        status, value = self.update(migrated, name="Dopo migrazione")
        self.assertEqual(status, 200, value)
        self.assertEqual(self.history(snapshot)[-1]["snapshot"], snapshot)
        self.assertGreater(self.create(tag="AFTER")["id"], 41)
        self.start()
        self.assertEqual(self.get(snapshot), value["asset"])
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 6)
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            self.assertEqual(database.execute("SELECT snapshot FROM asset_history WHERE id=77").fetchone()[0], serialized)

    def test_installations_connections_and_relation_rules(self):
        self.start()
        sw = self.typed("SW", "software")
        pc = self.typed("PC", "it_device")
        net = self.typed("NET", "network_device")
        chair = self.typed("CHAIR", "physical")
        sw, installed = self.relate(sw, "installed_on", pc, installed_version="4.1", notes="Installazione manuale")
        pc, connected = self.relate(pc, "connected_to", net, source_port="eth0", target_port="Gi1/0/1")
        self.assertEqual(installed["installed_version"], "4.1")
        self.assertEqual((connected["source_port"], connected["target_port"]), ("eth0", "Gi1/0/1"))
        self.assertEqual(self.add_relation(sw, "installed_on", pc)[0], 409)
        self.assertEqual(self.add_relation(pc, "connected_to", net)[0], 409)
        cases = [(pc, "installed_on", net, {}), (sw, "installed_on", chair, {}), (sw, "installed_on", net, {"quantity": 2}), (sw, "installed_on", net, {"source_port": "p1"}), (sw, "installed_on", net, {"assignee": "Mario"}), (net, "connected_to", net, {}), (pc, "connected_to", chair, {}), (chair, "connected_to", net, {}), (pc, "connected_to", net, {"quantity": 2}), (pc, "unknown", net, {})]
        for source, kind, target, fields in cases:
            with self.subTest(kind=kind, source=source["asset_type"], target=target["asset_type"], fields=fields):
                self.assertEqual(self.add_relation(source, kind, target, **fields)[0], 400)
        incoming = self.relations(net)["items"]
        self.assertEqual([(item["id"], item["direction"]) for item in incoming], [(connected["id"], "incoming")])
        self.assertEqual(incoming[0]["source"]["id"], pc["id"])
        self.assertEqual(incoming[0]["target"]["id"], net["id"])
        for state in ("retired", "lost"):
            disabled = self.typed("OFF-" + state, "it_device", status=state)
            self.assertEqual(self.add_relation(sw, "installed_on", disabled)[0], 409)
            self.assertEqual(self.add_relation(disabled, "connected_to", net)[0], 409)

    def test_relation_validation_versions_authorization_and_audit(self):
        self.start()
        sw = self.typed("SW", "software")
        pc = self.typed("PC", "it_device")
        path = "/v1/assets/%d/relations" % sw["id"]
        base = {"relation_type": "installed_on", "target_id": pc["id"], "version": sw["version"], "actor_id": 91}
        for fields in ({"version": ""}, {"version": 0}, {"actor_id": 0}, {"quantity": 0}, {"quantity": 1000001}, {"installed_version": "a" * 81}, {"notes": "a" * 2001}, {"unknown": "x"}):
            with self.subTest(fields=fields):
                self.assertEqual(self.request("POST", path, dict(base, **fields))[0], 400)
        missing_version = dict(base)
        del missing_version["version"]
        self.assertEqual(self.request("POST", path, missing_version)[0], 400)
        self.assertEqual(self.request("POST", path, base, authenticated=False)[0], 401)
        self.assertEqual(self.request("GET", path, token=fixtures.TOKEN[:-1] + "b")[0], 401)
        self.assertEqual(len(self.history(sw)), 1)
        status, result = self.request("POST", path, base)
        self.assertEqual(status, 201, result)
        current = result["asset"]
        relation = result["relation"]
        self.assertEqual(current["actor_id"], 91)
        self.assertEqual(self.add_relation(sw, "installed_on", pc)[0], 409)
        event = self.history(sw)[0]
        self.assertEqual((event["action"], event["actor_id"], event["version"]), ("updated", 91, current["version"]))
        self.assertEqual(event["snapshot"], current)
        self.assertEqual(event["change"]["operation"], "relation_added")
        self.assertEqual(event["change"]["relation"]["id"], relation["id"])
        before_change = event["change"]
        self.assertEqual(self.remove_relation(sw, relation)[0], 409)
        status, removed = self.remove_relation(current, relation)
        self.assertEqual(status, 200, removed)
        self.assertFalse(removed["relation"]["active"])
        events = self.history(sw)
        self.assertEqual(events[0]["change"]["operation"], "relation_removed")
        self.assertEqual(events[1]["change"], before_change)
        self.assertEqual(self.remove_relation(removed["asset"], relation)[0], 409)
        self.assertEqual(self.get(pc)["version"], 1)

    def test_license_capacity_release_expiry_and_reference_guards(self):
        self.start()
        sw = self.typed("SW", "software")
        other_sw = self.typed("SW2", "software")
        pc = self.typed("PC", "it_device")
        pc2 = self.typed("PC2", "it_device")
        lic = self.typed("LIC", "license", {"software_id": sw["id"], "seats_total": 4})
        lic, relation = self.relate(lic, "licensed_to", pc, quantity=3)
        self.assertEqual(lic["license_usage"], {"allocated": 3, "available": 1, "expired": False})
        self.assertEqual(self.add_relation(lic, "licensed_to", pc2, quantity=2)[0], 409)
        self.assertEqual(self.add_relation(lic, "licensed_to", pc)[0], 409)
        for details in ({"software_id": sw["id"], "seats_total": 2}, {"software_id": other_sw["id"], "seats_total": 4}, {"software_id": sw["id"], "seats_total": 4, "license_model": "per_user"}):
            self.assertEqual(self.update(lic, details=json.dumps(details))[0], 409)
        self.assertEqual(self.update(sw, asset_type="physical", details="{}")[0], 409)
        self.assertEqual(self.update(pc, asset_type="physical", details="{}")[0], 409)
        self.assertEqual(self.update(lic, asset_type="physical", details="{}")[0], 409)
        self.assertEqual(self.get(lic), lic)
        status, removed = self.remove_relation(lic, relation)
        self.assertEqual(status, 200, removed)
        lic = removed["asset"]
        self.assertEqual(lic["license_usage"]["allocated"], 0)
        self.assertEqual(lic["license_usage"]["available"], 4)
        lic, again = self.relate(lic, "licensed_to", pc, quantity=4)
        self.assertNotEqual(again["id"], relation["id"])
        self.assertEqual(self.get(lic)["license_usage"]["available"], 0)
        expired = self.typed("OLD-LIC", "license", {"software_id": sw["id"], "seats_total": 2, "expires_on": (date.today() - timedelta(days=2)).isoformat()})
        self.assertTrue(expired["license_usage"]["expired"])
        self.assertEqual(self.add_relation(expired, "licensed_to", pc)[0], 409)
        not_software = self.typed("PHYS", "physical")
        for reference in (0, -1, 99999, not_software["id"]):
            status, value = self.request("POST", "/v1/assets", {"asset_tag": "BAD-REF", "name": "License", "actor_id": 1, "asset_type": "license", "details": json.dumps({"software_id": reference, "seats_total": 1})})
            self.assertEqual(status, 400, value)

    def test_license_models_accept_only_matching_allocation_targets(self):
        self.start()
        sw = self.typed("SW", "software")
        pc = self.typed("PC", "it_device")
        chair = self.typed("CHAIR", "physical")
        for model in ("per_device", "per_user", "concurrent", "site"):
            lic = self.typed("LIC-" + model, "license", {"software_id": sw["id"], "seats_total": 5, "license_model": model})
            if model == "per_device":
                self.assertEqual(self.add_relation(lic, "licensed_to", assignee="Mario")[0], 400)
                lic, _ = self.relate(lic, "licensed_to", pc)
            else:
                if model == "per_user":
                    self.assertEqual(self.add_relation(lic, "licensed_to", pc)[0], 400)
                lic, _ = self.relate(lic, "licensed_to", assignee="Mario", quantity=2)
                self.assertEqual(self.add_relation(lic, "licensed_to", assignee="Mario")[0], 409)
            self.assertEqual(self.add_relation(lic, "licensed_to")[0], 400)
            self.assertEqual(self.add_relation(lic, "licensed_to", pc, assignee="Mario")[0], 400)
            self.assertEqual(self.add_relation(lic, "licensed_to", chair)[0], 400)
            if model in ("concurrent", "site"):
                lic, _ = self.relate(lic, "licensed_to", pc, quantity=3)
                self.assertEqual(lic["license_usage"]["available"], 0)

    def test_active_relations_block_type_changes_until_removed(self):
        self.start()
        sw = self.typed("SW", "software")
        pc = self.typed("PC", "it_device")
        sw, relation = self.relate(sw, "installed_on", pc)
        for asset in (sw, pc):
            self.assertEqual(self.update(asset, asset_type="physical", details="{}")[0], 409)
        status, value = self.remove_relation(sw, relation)
        self.assertEqual(status, 200, value)
        for asset in (value["asset"], pc):
            status, changed = self.update(asset, asset_type="physical", details="{}")
            self.assertEqual(status, 200, changed)
            self.assertEqual(changed["asset"]["asset_type"], "physical")

    def test_relation_history_failure_rolls_back_add_and_remove_atomically(self):
        self.start()
        sw = self.typed("SW", "software")
        pc = self.typed("PC", "it_device")
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_history BEFORE INSERT ON asset_history BEGIN SELECT RAISE(FAIL, 'fixture failure'); END")
            database.commit()
        self.assertEqual(self.add_relation(sw, "installed_on", pc)[0], 500)
        self.assertEqual(self.get(sw), sw)
        self.assertEqual(self.relations(sw)["items"], [])
        self.assertEqual(len(self.history(sw)), 1)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("DROP TRIGGER reject_history")
            database.commit()
        sw, relation = self.relate(sw, "installed_on", pc)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_history BEFORE INSERT ON asset_history BEGIN SELECT RAISE(FAIL, 'fixture failure'); END")
            database.commit()
        self.assertEqual(self.remove_relation(sw, relation)[0], 500)
        self.assertEqual(self.get(sw), sw)
        self.assertTrue(self.relations(sw)["items"][0]["active"])
        self.assertEqual(len(self.history(sw)), 2)
        self.start()
        self.assertEqual(self.get(sw), sw)
        self.assertTrue(self.relations(pc)["items"][0]["active"])

    def test_relation_pages_include_incoming_outgoing_and_survive_restart(self):
        self.start()
        sw = self.typed("SW", "software")
        center = self.typed("CENTER", "it_device")
        net = self.typed("NET", "network_device")
        sw, incoming = self.relate(sw, "installed_on", center)
        center, outgoing = self.relate(center, "connected_to", net)
        first = self.relations(center, "?limit=1")
        self.assertEqual([row["id"] for row in first["items"]], [outgoing["id"]])
        self.assertEqual(first["items"][0]["direction"], "outgoing")
        second = self.relations(center, "?limit=1&after_id=%d" % first["next_after_id"])
        self.assertEqual([row["id"] for row in second["items"]], [incoming["id"]])
        self.assertEqual(second["items"][0]["direction"], "incoming")
        self.assertIsNone(second["next_after_id"])
        for query in ("?limit=0", "?limit=101", "?after_id=0", "?q=term", "?asset_type=it_device", "?limit=1&limit=2"):
            self.assertEqual(self.request("GET", "/v1/assets/%d/relations%s" % (center["id"], query))[0], 400)
        self.assertEqual(self.request("GET", "/v1/assets/99999/relations")[0], 404)
        self.start()
        self.assertEqual(self.relations(center)["items"], first["items"] + second["items"])

    def test_relation_byte_budget_preserves_complete_pages_and_history(self):
        self.start()
        software = self.typed("BIG-SW", "software")
        created = []
        for index in range(35):
            device = self.typed("BIG-PC-%02d" % index, "it_device")
            software, relation = self.relate(software, "installed_on", device, notes="\\\n" * 1000)
            created.append(relation["id"])
        seen, cursor, pages = [], None, 0
        while True:
            query = "?limit=100" + ("&after_id=%d" % cursor if cursor else "")
            value = self.relations(software, query)
            self.assertTrue(value["items"])
            seen.extend(row["id"] for row in value["items"])
            pages += 1
            self.assertLess(pages, 10)
            cursor = value["next_after_id"]
            if cursor is None:
                break
            self.assertEqual(cursor, value["items"][-1]["id"])
        self.assertGreater(pages, 1)
        self.assertEqual(seen, list(reversed(created)))
        versions, cursor, pages = [], None, 0
        while True:
            query = "?limit=100" + ("&after_id=%d" % cursor if cursor else "")
            status, value = self.request("GET", "/v1/assets/%d/history%s" % (software["id"], query))
            self.assertEqual(status, 200, value)
            self.assertTrue(value["items"])
            versions.extend(row["version"] for row in value["items"])
            pages += 1
            self.assertLess(pages, 10)
            cursor = value["next_after_id"]
            if cursor is None:
                break
            self.assertEqual(cursor, value["items"][-1]["id"])
        self.assertGreater(pages, 1)
        self.assertEqual(versions, list(range(36, 0, -1)))


if __name__ == "__main__":
    unittest.main()
