"""Direct asset/person/identity links stay independent from shared identity membership."""
from contextlib import closing
import csv
import io
import sqlite3
import unittest

import test_inventory_addon as fixtures


class InventorySubjectsTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update

    def person(self, code="P-1"):
        status, value = self.request("POST", "/v1/people", dict(person_code=code, display_name="Mario Rossi", actor_id=7))
        self.assertEqual(status, 201, value)
        return value["person"]

    def identity(self, login="shared"):
        status, value = self.request("POST", "/v1/identities", dict(realm="Dominio", login=login, kind="shared", actor_id=7))
        self.assertEqual(status, 201, value)
        return value["identity"]

    def link(self, asset, kind, entity, **fields):
        data = dict(version=asset["version"], actor_id=7, subject_kind=kind, subject_id=entity["id"])
        data.update(fields)
        status, value = self.request("POST", f'/v1/assets/{asset["id"]}/subjects', data)
        self.assertEqual(status, 201, value)
        return value["asset"], value["link"]

    def get(self, path):
        status, value = self.request("GET", path)
        self.assertEqual(status, 200, value)
        return value

    def test_direct_links_and_many_to_many_do_not_propagate_assignment(self):
        self.start()
        person, other, identity = self.person(), self.person("P-2"), self.identity()
        for entry in (person, other):
            status, value = self.request("POST", f'/v1/identities/{identity["id"]}/people',
                                        dict(version=identity["version"], person_id=entry["id"], actor_id=7))
            self.assertEqual(status, 201, value)
            identity = value["identity"]
        asset = self.create()
        asset, link = self.link(asset, "identity", identity, role="administrator", notes="Utenza amministrativa condivisa")
        self.assertEqual(link["subject"]["people_count"], 2)
        self.assertEqual(link["asset"]["id"], asset["id"])
        self.assertIsNone(asset["context"])
        self.assertIsNone(asset["custody"])
        self.assertEqual(asset["status"], "available")
        self.assertEqual(self.get(f'/v1/people/{person["id"]}/assets')["items"], [])
        self.assertEqual(self.get(f'/v1/assets?person_id={person["id"]}')["items"], [])
        self.assertEqual(len(self.get(f'/v1/identities/{identity["id"]}/assets')["items"]), 1)
        asset, _ = self.link(asset, "person", person, role="owner")
        self.assertEqual(len(self.get(f'/v1/people/{person["id"]}/assets')["items"]), 1)
        self.assertEqual(len(self.get(f'/v1/assets/{asset["id"]}/subjects')["items"]), 2)

    def test_duplicate_conflict_rollback_and_immutable_snapshots(self):
        self.start()
        person, asset = self.person(), self.create()
        original = asset
        asset, link = self.link(asset, "person", person)
        path = f'/v1/assets/{asset["id"]}/subjects'
        fields = dict(version=asset["version"], actor_id=7, subject_kind="person", subject_id=person["id"])
        self.assertEqual(self.request("POST", path, fields)[0], 409)
        self.assertEqual(self.request("POST", path, dict(fields, version=original["version"]))[0], 409)
        person_fields = dict(person_code=person["person_code"], display_name="Nuovo nome", version=person["version"], actor_id=8)
        self.assertEqual(self.request("POST", f'/v1/people/{person["id"]}', person_fields)[0], 200)
        live = self.get(path)["items"][0]
        self.assertEqual(live["subject"]["display_name"], "Nuovo nome")
        self.assertEqual(live["subject_snapshot"]["display_name"], "Mario Rossi")
        event = self.get(f'/v1/assets/{asset["id"]}/history')["items"][0]
        self.assertEqual(event["change"]["operation"], "subject_added")
        self.assertEqual(event["change"]["link"], link)
        self.assertEqual(event["snapshot"], asset)
        with closing(sqlite3.connect(self.db)) as db:
            db.execute("CREATE TRIGGER reject_subject_history BEFORE INSERT ON asset_history BEGIN SELECT RAISE(FAIL,'fixture'); END")
            db.commit()
        remove = path + f'/{link["id"]}/remove'
        self.assertEqual(self.request("POST", remove, dict(version=asset["version"], actor_id=8))[0], 500)
        self.assertEqual(self.get(f'/v1/assets/{asset["id"]}')["asset"], asset)
        self.assertEqual(len(self.get(path)["items"]), 1)
        other = self.person("P-2")
        self.assertEqual(self.request("POST", path, dict(fields, subject_id=other["id"]))[0], 500)
        self.assertEqual(self.get(f'/v1/people/{other["id"]}/assets')["items"], [])

    def test_inactive_subjects_retired_assets_and_removal_relink(self):
        self.start()
        person, asset = self.person(), self.create()
        asset, link = self.link(asset, "person", person)
        status, value = self.request("POST", f'/v1/people/{person["id"]}', dict(person_code="P-1", display_name="Mario Rossi", version=1, actor_id=7, active=0))
        self.assertEqual(status, 200, value)
        self.assertFalse(self.get(f'/v1/assets/{asset["id"]}/subjects')["items"][0]["subject"]["active"])
        other = self.create("PC-2")
        data = dict(version=1, actor_id=7, subject_kind="person", subject_id=person["id"])
        self.assertEqual(self.request("POST", f'/v1/assets/{other["id"]}/subjects', data)[0], 409)
        status, value = self.update(asset, status="retired")
        self.assertEqual(status, 200, value)
        asset = value["asset"]
        remove = f'/v1/assets/{asset["id"]}/subjects/{link["id"]}/remove'
        status, value = self.request("POST", remove, dict(version=asset["version"], actor_id=8))
        self.assertEqual(status, 200, value)
        asset = value["asset"]
        self.assertFalse(value["link"]["active"])
        self.assertEqual(value["link"]["removed_actor_id"], 8)
        self.assertEqual(self.request("POST", remove, dict(version=asset["version"], actor_id=8))[0], 404)
        self.assertEqual(self.get(f'/v1/people/{person["id"]}/assets')["items"], [])
        active = self.person("P-3")
        self.assertEqual(self.request("POST", f'/v1/assets/{asset["id"]}/subjects', dict(data,version=asset["version"],subject_id=active["id"]))[0], 409)
        asset, first = self.link(other, "person", active)
        status, value = self.request("POST", f'/v1/assets/{asset["id"]}/subjects/{first["id"]}/remove', dict(version=asset["version"], actor_id=7))
        self.assertEqual(status, 200, value)
        _, second = self.link(value["asset"], "person", active)
        self.assertGreater(second["id"], first["id"])

    def test_reverse_pagination_catalog_dashboard_and_export_filters(self):
        self.start()
        person, identity = self.person(), self.identity()
        first, _ = self.link(self.create("PC-1"), "person", person)
        first, _ = self.link(first, "identity", identity)
        second, _ = self.link(self.create("PC-2"), "person", person)
        self.create("PC-3")
        page = self.get(f'/v1/people/{person["id"]}/assets?limit=1')
        self.assertEqual(page["items"][0]["asset"]["id"], second["id"])
        cursor = page["next_after_id"]
        self.assertEqual(cursor, page["items"][0]["id"])
        page = self.get(f'/v1/people/{person["id"]}/assets?limit=1&after_id={cursor}')
        self.assertEqual(page["items"][0]["asset"]["id"], first["id"])
        self.assertIsNone(page["next_after_id"])
        for query, expected in ((f'person_id={person["id"]}', 2), (f'identity_id={identity["id"]}', 1),
                                (f'person_id={person["id"]}&identity_id={identity["id"]}', 1)):
            self.assertEqual(len(self.get('/v1/assets?' + query)["items"]), expected)
            self.assertEqual(self.get('/v1/dashboard?' + query)["counts"]["total"], expected)
            rows = list(csv.reader(io.StringIO(self.get('/v1/assets/export?' + query)["csv"])))
            self.assertEqual(len(rows)-1, expected)
        for query in ("person_id=0", "identity_id=-1", "person_id=", "identity_id=no", "person_id=1&person_id=2"):
            for path in ("/v1/assets", "/v1/dashboard", "/v1/assets/export"):
                self.assertEqual(self.request("GET", path + '?' + query)[0], 400)

    def test_strict_paths_methods_fields_and_fk_integrity(self):
        self.start()
        asset, person = self.create(), self.person()
        path = f'/v1/assets/{asset["id"]}/subjects'
        data = dict(version=1, actor_id=7, subject_kind="person", subject_id=person["id"])
        for bad in (dict(data,role="unknown"),dict(data,subject_kind="user"),dict(data,subject_id=0),dict(data,notes="x"*2001),dict(data,password="secret")):
            self.assertEqual(self.request("POST", path, bad)[0], 400)
        self.assertEqual(self.request("POST", path, dict(data,subject_id=9999))[0], 404)
        self.assertEqual(self.request("POST", path + '?limit=1', data)[0], 400)
        self.assertEqual(self.request("POST", path, data, content_type="application/json")[0], 415)
        self.assertEqual(self.request("GET", path + '/1/remove')[0], 405)
        self.assertEqual(self.request("GET", path + '/1/remove/extra')[0], 404)
        self.assertEqual(self.request("GET", path + '?q=x')[0], 400)
        self.assertEqual(self.request("POST", f'/v1/people/{person["id"]}/assets', data)[0], 405)
        self.assertEqual(self.request("GET", '/v1/people/9999/assets')[0], 404)
        with closing(sqlite3.connect(self.db)) as db:
            db.execute("PRAGMA foreign_keys=ON")
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 6)


if __name__ == '__main__':
    unittest.main()
