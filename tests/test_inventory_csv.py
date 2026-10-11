"""CSV dry-run/atomic transfer and deterministic due dates over private TLS."""

from contextlib import closing
import csv
import io
import json
import sqlite3
import unittest
from urllib.parse import urlencode

import test_inventory_addon as fixtures


class InventoryCsvTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update

    def preview(self, text, mode="create_only", actor=17):
        return self.request("POST", "/v1/import/preview", {"csv": text, "mode": mode, "actor_id": actor})

    def commit(self, preview, actor=17):
        return self.request("POST", "/v1/import/commit", {"preview_id": preview["preview_id"], "actor_id": actor})

    def rows(self):
        status, value = self.request("GET", "/v1/assets?limit=100")
        self.assertEqual(status, 200, value)
        return value["items"]

    def history_count(self):
        with closing(sqlite3.connect(self.db)) as database:
            return database.execute("SELECT COUNT(*) FROM asset_history").fetchone()[0]

    @staticmethod
    def document(headers, rows, newline="\r\n"):
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator=newline)
        writer.writerow(headers)
        writer.writerows(rows)
        return stream.getvalue()

    def test_preview_has_no_asset_or_history_writes_and_commit_is_single_use(self):
        self.start()
        status, preview = self.preview('\ufeffasset_tag,name,notes\r\nONE,"Caffè, sede","Prima riga\r\nSeconda ""citata"""\r\nTWO,Altro,\r\n')
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview["can_commit"])
        self.assertRegex(preview["preview_id"], r"^[a-f0-9]{32}$")
        self.assertEqual(preview["summary"], {"total": 2, "create": 2, "update": 0, "errors": 0})
        self.assertEqual(self.rows(), [])
        self.assertEqual(self.history_count(), 0)
        self.start()  # Previews survive a process restart, without applying anything.
        status, committed = self.commit(preview)
        self.assertEqual(status, 200, committed)
        self.assertEqual(committed["summary"], {"total": 2, "create": 2, "update": 0})
        rows = {row["asset_tag"]: row for row in self.rows()}
        self.assertEqual(rows["ONE"]["name"], "Caffè, sede")
        self.assertEqual(rows["ONE"]["notes"], 'Prima riga\r\nSeconda "citata"')
        self.assertEqual({row["actor_id"] for row in rows.values()}, {17})
        self.assertEqual(self.history_count(), 2)
        self.assertEqual(self.commit(preview)[0], 409)
        self.assertEqual(self.history_count(), 2)

    def test_invalid_rows_and_duplicates_block_whole_preview(self):
        self.start()
        self.create(tag="EXISTS")
        text = self.document(["asset_tag", "name", "status"], [
            ["same", "Uno", "available"], ["SAME", "Due", "available"],
            ["OK", "Valido", "available"], ["BAD", "Errato", "broken"],
            ["EXISTS", "Esistente", "available"], ["MISSING", "", "available"],
        ])
        status, preview = self.preview(text)
        self.assertEqual(status, 200, preview)
        self.assertFalse(preview["can_commit"])
        self.assertIsNone(preview["preview_id"])
        self.assertEqual(preview["summary"], {"total": 6, "create": 1, "update": 0, "errors": 5})
        self.assertEqual([item["row"] for item in preview["items"]], list(range(2, 8)))
        self.assertTrue(all(item["errors"] for item in preview["items"] if item["action"] == "error"))
        self.assertEqual(len(self.rows()), 1)
        self.assertEqual(self.history_count(), 1)

    def test_headers_framing_unknown_fields_and_limits_are_rejected(self):
        self.start()
        invalid = ["", "asset_tag,name\n", "name\nUno\n", "asset_tag,name,token\nX,Uno,secret\n",
                   "asset_tag,name,name\nX,Uno,Due\n", "asset_tag,name\nX\n",
                   'asset_tag,name\nX,"non chiuso\n', 'asset_tag,name\nX,non"valido\n',
                   'asset_tag,name\nX,"chiuso" extra\n', "asset_tag,name\rX,Uno\r",
                   "asset_tag,name\n" + "X,Uno\n" * 201,
                   "asset_tag,name,notes\nX,Uno," + "a" * (256 * 1024)]
        for text in invalid:
            with self.subTest(text=text[:80]):
                self.assertEqual(self.preview(text)[0], 400)
        self.assertEqual(self.preview("asset_tag,name\nX,Uno\n", mode="overwrite")[0], 400)
        self.assertEqual(self.request("POST", "/v1/import/preview", {"csv": "asset_tag,name\nX,Uno\n", "mode": "create_only", "actor_id": 17, "extra": 1})[0], 400)
        self.assertEqual(self.request("GET", "/v1/import/preview")[0], 405)
        self.assertEqual(self.request("POST", "/v1/assets/export", {})[0], 405)
        self.assertEqual(self.rows(), [])

    def test_upsert_requires_current_version_and_preserves_omitted_fields(self):
        self.start()
        asset = self.create(tag="PC", name="Prima", location="Roma", notes="Conservata",
                            asset_type="it_device", details=json.dumps({"hostname": "pc-roma"}))
        for version in ("", "2", "-1"):
            status, preview = self.preview(self.document(["asset_tag", "name", "version"], [["pc", "Dopo", version]]), "upsert")
            self.assertEqual(status, 200, preview)
            self.assertFalse(preview["can_commit"])
        status, preview = self.preview("asset_tag,name,version\npc,Dopo,1\n", "upsert")
        self.assertEqual(status, 200, preview)
        self.assertEqual(preview["summary"], {"total": 1, "create": 0, "update": 1, "errors": 0})
        self.assertEqual(self.commit(preview)[0], 200)
        updated = self.rows()[0]
        self.assertEqual((updated["id"], updated["version"], updated["name"]), (asset["id"], 2, "Dopo"))
        self.assertEqual((updated["location"], updated["notes"], updated["details"]), ("Roma", "Conservata", asset["details"]))
        self.assertEqual(self.history_count(), 2)

    def test_stale_update_rolls_back_prior_creates_and_consumes_preview(self):
        self.start()
        asset = self.create(tag="OLD", name="Prima")
        status, preview = self.preview("asset_tag,name,version\nNEW,Nuovo,\nOLD,CSV,1\n", "upsert")
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview["can_commit"])
        self.assertEqual(self.update(asset, name="Modifica concorrente")[0], 200)
        self.assertEqual(self.commit(preview)[0], 409)
        self.assertEqual([(row["asset_tag"], row["name"]) for row in self.rows()], [("OLD", "Modifica concorrente")])
        self.assertEqual(self.history_count(), 2)
        self.assertEqual(self.commit(preview)[0], 409)

    def test_new_duplicate_after_preview_is_rechecked_atomically(self):
        self.start()
        status, preview = self.preview("asset_tag,name\nFIRST,Uno\nSECOND,Due\n")
        self.assertEqual(status, 200, preview)
        self.create(tag="second", name="Concorrente")
        self.assertEqual(self.commit(preview)[0], 409)
        self.assertEqual([row["asset_tag"] for row in self.rows()], ["second"])
        self.assertEqual(self.history_count(), 1)

    def test_preview_is_actor_bound_expires_and_rejects_tampered_form(self):
        self.start()
        status, preview = self.preview("asset_tag,name\nNEW,Nuovo\n")
        self.assertEqual(status, 200, preview)
        self.assertEqual(self.commit(preview, actor=18)[0], 409)
        self.assertEqual(self.request("POST", "/v1/import/commit", {"preview_id": preview["preview_id"], "actor_id": 17, "csv": "asset_tag,name\nOTHER,Altro\n"})[0], 400)
        self.assertEqual(self.commit(preview)[0], 200)
        status, expired = self.preview("asset_tag,name\nEXPIRED,Scaduta\n")
        self.assertEqual(status, 200, expired)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("UPDATE inventory_import_previews SET expires_at=1 WHERE preview_id=?", (expired["preview_id"],))
            database.commit()
        self.assertEqual(self.commit(expired)[0], 409)
        self.assertEqual([row["asset_tag"] for row in self.rows()], ["NEW"])

    def test_history_write_failure_rolls_back_all_rows(self):
        self.start()
        status, preview = self.preview("asset_tag,name\nFIRST,Uno\nSECOND,Due\n")
        self.assertEqual(status, 200, preview)
        with closing(sqlite3.connect(self.db)) as database:
            database.execute("CREATE TRIGGER reject_csv_history BEFORE INSERT ON asset_history WHEN json_extract(NEW.snapshot,'$.asset_tag')='SECOND' BEGIN SELECT RAISE(ABORT,'test history failure'); END")
            database.commit()
        self.assertEqual(self.commit(preview)[0], 500)
        self.assertEqual(self.rows(), [])
        self.assertEqual(self.history_count(), 0)
        self.assertEqual(self.commit(preview)[0], 409)

    def test_typed_details_and_license_references_use_same_validation(self):
        self.start()
        software = self.create(tag="APP", asset_type="software", details="{}")
        text = self.document(["asset_tag", "name", "asset_type", "details"], [
            ["NET", "Switch", "network_device", json.dumps({"ip_address": "192.0.2.3", "vlan": 23, "port_count": 48})],
            ["LIC", "Licenza", "license", json.dumps({"software_id": software["id"], "seats_total": 3})],
        ])
        status, preview = self.preview(text)
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview["can_commit"])
        self.assertEqual(self.commit(preview)[0], 200)
        rows = {row["asset_tag"]: row for row in self.rows()}
        self.assertEqual(rows["NET"]["details"]["vlan"], 23)
        self.assertEqual(rows["LIC"]["license_usage"]["available"], 3)
        invalid = self.document(["asset_tag", "name", "asset_type", "details"], [["BAD", "Errato", "it_device", '{"ip_address":"bad"}']])
        self.assertFalse(self.preview(invalid)[1]["can_commit"])
        invalid = self.document(["asset_tag", "name", "asset_type", "details"], [["BAD", "Errato", "software", '{"publisher":"A","publisher":"B"}']])
        self.assertFalse(self.preview(invalid)[1]["can_commit"])

    def test_license_cannot_bind_to_temporary_software_id_reused_by_concurrent_create(self):
        self.start()
        text = self.document(["asset_tag", "name", "asset_type", "details"], [
            ["DRAFT-SW", "Software del CSV", "software", "{}"],
            ["DRAFT-LIC", "Licenza del CSV", "license", json.dumps({"software_id": 1, "seats_total": 2})],
        ])
        status, preview = self.preview(text)
        self.assertEqual(status, 200, preview)
        self.assertFalse(preview["can_commit"])
        self.assertIsNone(preview["preview_id"])
        self.assertEqual([item["action"] for item in preview["items"]], ["create", "error"])
        self.assertIn("prima dell'anteprima", preview["items"][1]["errors"][0])
        self.assertEqual(self.rows(), [])
        other = self.create(tag="OTHER-SW", name="Software di un altro gestore", actor_id=28,
                            asset_type="software", details="{}")
        self.assertEqual(other["id"], 1)
        # The rejected preview has no usable token; it cannot create a license
        # whose software_id now resolves to this unrelated concurrent software.
        self.assertEqual(self.commit(preview)[0], 400)
        self.assertEqual(self.rows(), [other])
        self.assertEqual(self.history_count(), 1)

    def test_old_license_preview_without_persistent_reference_proof_rolls_back(self):
        self.start()
        other = self.create(tag="OTHER-SW", asset_type="software", details="{}")
        status, preview = self.preview('asset_tag,name,asset_type,details\nDRAFT-SW,Software CSV,software,{}\n')
        self.assertEqual(status, 200, preview)
        # Reconstruct a still-open preview from the former implementation,
        # where a license could refer to a temporary software ID during dry-run.
        license_asset = dict(other, asset_tag="DRAFT-LIC", name="Licenza CSV", asset_type="license",
                             details={"software_id": other["id"], "seats_total": 2, "license_model": "per_device", "renewal_mode": "manual"})
        with closing(sqlite3.connect(self.db)) as database:
            plan = json.loads(database.execute("SELECT plan FROM inventory_import_previews WHERE preview_id=?", (preview["preview_id"],)).fetchone()[0])
            plan.append({"id": 0, "expected_version": 0, "asset": license_asset})
            database.execute("UPDATE inventory_import_previews SET plan=? WHERE preview_id=?", (json.dumps(plan), preview["preview_id"]))
            database.commit()
        self.assertEqual(self.commit(preview)[0], 409)
        self.assertEqual(self.rows(), [other])
        self.assertEqual(self.history_count(), 1)
        self.assertEqual(self.commit(preview)[0], 409)

    def test_export_safe_cells_round_trip_quotes_unicode_multiline_and_apostrophes(self):
        self.start()
        assets = [self.create(tag="FORMULA", name="=1+1", notes=" \t\r\n@SUM(A1)", location="'Roma", asset_type="it_device", details="{}"),
                  self.create(tag="QUOTE", name="'semplice", notes='Una "citazione", caffè\nseconda riga', asset_type="it_device", details="{}"),
                  self.create(tag="MINUS", name=" -formula", notes="''due apostrofi", asset_type="it_device", details="{}")]
        status, exported = self.request("GET", "/v1/assets/export?limit=100")
        self.assertEqual(status, 200, exported)
        self.assertEqual(exported["exported"], 3)
        table = list(csv.DictReader(io.StringIO(exported["csv"], newline="")))
        by_tag = {row["asset_tag"]: row for row in table}
        self.assertEqual(by_tag["FORMULA"]["name"], "'=1+1")
        self.assertEqual(by_tag["FORMULA"]["notes"], "' \t\r\n@SUM(A1)")
        self.assertEqual(by_tag["FORMULA"]["location"], "''Roma")
        self.assertEqual(by_tag["QUOTE"]["name"], "''semplice")
        status, preview = self.preview(exported["csv"], "upsert")
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview["can_commit"])
        self.assertEqual(self.commit(preview)[0], 200)
        after = {row["id"]: row for row in self.rows()}
        for original in assets:
            updated = after[original["id"]]
            for field in ("asset_tag", "name", "notes", "location", "asset_type", "details"):
                self.assertEqual(updated[field], original[field])
            self.assertEqual(updated["version"], 2)

    def test_large_export_pages_are_bounded_complete_and_filtered(self):
        self.start()
        # Escaped multiline/quotes cost more in the JSON envelope than in SQLite.
        text = self.document(["asset_tag", "name", "notes"], [["BULK-%03d" % i, "Bene %d" % i, '"\n' * 1900] for i in range(45)])
        status, preview = self.preview(text)
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview["can_commit"])
        self.assertEqual(self.commit(preview)[0], 200)
        self.create(tag="OTHER", status="retired")
        cursor = None
        exported_tags = []
        pages = 0
        while True:
            query = {"limit": 100, "q": "BULK-", "status": "available", "asset_type": "it_device"}
            if cursor:
                query["after_id"] = cursor
            status, page = self.request("GET", "/v1/assets/export?" + urlencode(query))
            self.assertEqual(status, 200, page)
            rows = list(csv.DictReader(io.StringIO(page["csv"], newline="")))
            self.assertEqual(page["exported"], len(rows))
            self.assertTrue(rows)
            exported_tags.extend(row["asset_tag"] for row in rows)
            pages += 1
            cursor = page["next_after_id"]
            if not cursor:
                break
            self.assertLess(pages, 50)
        self.assertGreater(pages, 1)
        self.assertEqual(len(exported_tags), 45)
        self.assertEqual(len(set(exported_tags)), 45)
        self.assertEqual(exported_tags, sorted(exported_tags, reverse=True))
        self.assertEqual(self.request("GET", "/v1/assets/export?asset_type=wrong")[0], 400)

    def test_due_dates_include_overdue_boundary_and_atomic_asset_groups(self):
        self.start()
        software = self.create(tag="SW", asset_type="software", details="{}")
        self.create(tag="WARRANTY", warranty_until="2028-02-29")
        self.create(tag="PHYSICAL", asset_type="physical", details=json.dumps({"maintenance_due": "2028-03-01"}))
        license_asset = self.create(tag="LICENSE", warranty_until="2028-01-01", asset_type="license", details=json.dumps({"software_id": software["id"], "seats_total": 4, "expires_on": "2028-03-30", "renewal_on": "2028-03-31"}))
        self.create(tag="RETIRED", status="retired", warranty_until="2020-01-01")
        self.create(tag="LOST", status="lost", warranty_until="2020-01-01")
        self.create(tag="LATER", warranty_until="2028-04-01")
        status, page = self.request("GET", "/v1/due?today=2028-02-29&days_before=30&limit=1")
        self.assertEqual(status, 200, page)
        self.assertEqual({item["asset_id"] for item in page["items"]}, {license_asset["id"]})
        self.assertEqual({item["due_kind"]: item["days_remaining"] for item in page["items"]}, {"warranty_until": -59, "expires_on": 30})
        self.assertEqual(page["next_after_id"], license_asset["id"])
        status, rest = self.request("GET", "/v1/due?today=2028-02-29&days_before=30&after_id=%d" % page["next_after_id"])
        self.assertEqual(status, 200, rest)
        self.assertEqual([(item["asset_tag"], item["due_kind"], item["days_remaining"]) for item in rest["items"]], [("PHYSICAL", "maintenance_due", 1), ("WARRANTY", "warranty_until", 0)])
        self.assertIsNone(rest["next_after_id"])
        for query in ("today=2028-02-30", "days_before=0", "days_before=366", "limit=101", "after_id=-1", "unknown=1", "today="):
            self.assertEqual(self.request("GET", "/v1/due?" + query)[0], 400)
        self.assertEqual(self.request("POST", "/v1/due", {})[0], 405)

    def test_schema_two_migration_preserves_assets_and_history(self):
        self.start()
        asset = self.create(tag="BEFORE", asset_type="network_device", details=json.dumps({"vlan": 27, "port_count": 24}))
        self.stop()
        with closing(sqlite3.connect(self.db)) as database:
            # Remove all later additions, preserving the actual previous v2 schema.
            for table in ("inventory_asset_subjects", "inventory_entity_history", "inventory_identity_people", "inventory_identities", "inventory_people",
                          "asset_verifications", "asset_custody", "asset_context", "inventory_places"):
                database.execute("DROP TABLE " + table)
            database.execute("DROP TABLE inventory_import_previews")
            database.execute("PRAGMA user_version=2")
            database.commit()
        self.start()
        self.assertEqual(self.rows(), [asset])
        self.assertEqual(self.history_count(), 1)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 5)
            self.assertEqual(database.execute("PRAGMA integrity_check").fetchone()[0], "ok")
        status, preview = self.preview("asset_tag,name\nAFTER,Nuovo\n")
        self.assertEqual(status, 200, preview)
        self.assertEqual(self.commit(preview)[0], 200)


if __name__ == "__main__":
    unittest.main()
