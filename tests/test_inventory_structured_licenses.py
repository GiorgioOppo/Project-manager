"""Structured license allocations retain recipient snapshots and count allocations once."""

from contextlib import closing
from datetime import date, timedelta
import sqlite3
import unittest

import test_inventory_addon as fixtures
import test_inventory_directory as directory
import test_inventory_types as types_fixture


class InventoryStructuredLicenseTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update
    typed = types_fixture.InventoryTypesTests.typed
    get = types_fixture.InventoryTypesTests.get
    history = types_fixture.InventoryTypesTests.history
    add_relation = types_fixture.InventoryTypesTests.add_relation
    relate = types_fixture.InventoryTypesTests.relate
    remove_relation = types_fixture.InventoryTypesTests.remove_relation
    relations = types_fixture.InventoryTypesTests.relations
    person = directory.InventoryDirectoryTests.person
    identity = directory.InventoryDirectoryTests.identity
    detail = directory.InventoryDirectoryTests.detail
    update_entity = directory.InventoryDirectoryTests.update_entity
    link = directory.InventoryDirectoryTests.link

    def license(self, tag="LIC", model="per_user", seats=20, **details):
        if not hasattr(self, "software"):
            self.software = self.typed("SW", "software")
        details.update(software_id=self.software["id"], license_model=model, seats_total=seats)
        return self.typed(tag, "license", details)

    def test_people_and_shared_identities_consume_quantity_once_without_implied_links(self):
        self.start()
        alice = self.person("ALICE", "Alice Rossi")
        bob = self.person("BOB", "Bob Bianchi")
        shared = self.identity("support", kind="shared", display_name="Supporto")
        first = self.link(shared, alice)
        second = self.link(first["identity"], bob)
        shared = second["identity"]
        license_asset = self.license(seats=7)
        license_asset, person_relation = self.relate(license_asset, "licensed_to", person_id=alice["id"], quantity=2)
        license_asset, identity_relation = self.relate(license_asset, "licensed_to", identity_id=shared["id"], quantity=3)
        self.assertEqual(license_asset["license_usage"], {"allocated": 5, "available": 2, "expired": False})
        self.assertEqual((person_relation["person_id"], person_relation["identity_id"], person_relation["subject_kind"], person_relation["subject_id"]),
                         (alice["id"], None, "person", alice["id"]))
        self.assertEqual((identity_relation["person_id"], identity_relation["identity_id"], identity_relation["subject_kind"]),
                         (None, shared["id"], "identity"))
        self.assertEqual(identity_relation["subject_snapshot"], shared)
        self.assertEqual(identity_relation["subject_snapshot"]["people_count"], 2)
        self.assertEqual(identity_relation["assignee"], "Supporto")
        self.assertIsNone(identity_relation["target_id"])
        self.assertIsNone(identity_relation["target"])
        self.assertEqual(self.detail(True, shared), shared)
        charlie = self.person("CHARLIE", "Charlie Verdi")
        self.link(shared, charlie)
        self.assertEqual(self.get(license_asset), license_asset)
        self.assertEqual(self.request("GET", "/v1/people/%d/assets" % alice["id"])[1]["items"], [])
        self.assertEqual(self.request("GET", "/v1/identities/%d/assets" % shared["id"])[1]["items"], [])
        self.assertEqual(len(self.history(license_asset)), 3)
        with closing(sqlite3.connect(self.db)) as database:
            self.assertEqual(database.execute("SELECT COUNT(*) FROM inventory_asset_subjects").fetchone()[0], 0)
            self.assertEqual(database.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_models_accept_only_their_supported_recipient_types(self):
        self.start()
        person = self.person()
        identity = self.identity()
        device = self.typed("PC", "it_device")
        physical = self.typed("CHAIR", "physical")
        for model in ("per_user", "per_device", "concurrent", "site"):
            license_asset = self.license("LIC-" + model, model)
            for field, recipient in (("person_id", person), ("identity_id", identity)):
                with self.subTest(model=model, field=field):
                    if model == "per_device":
                        self.assertEqual(self.add_relation(license_asset, "licensed_to", **{field: recipient["id"]})[0], 400)
                    else:
                        license_asset, _ = self.relate(license_asset, "licensed_to", **{field: recipient["id"]})
            if model == "per_user":
                self.assertEqual(self.add_relation(license_asset, "licensed_to", device)[0], 400)
            else:
                license_asset, _ = self.relate(license_asset, "licensed_to", device)
            self.assertEqual(self.add_relation(license_asset, "licensed_to", physical)[0], 400)
        software = self.software
        for kind in ("installed_on", "connected_to"):
            source = software if kind == "installed_on" else device
            target = device if kind == "installed_on" else self.typed("NET", "network_device")
            for field, recipient in (("person_id", person), ("identity_id", identity)):
                self.assertEqual(self.add_relation(source, kind, target, **{field: recipient["id"]})[0], 400)

    def test_exactly_one_recipient_and_strict_fields_leave_no_writes(self):
        self.start()
        person = self.person()
        identity = self.identity()
        device = self.typed("PC", "it_device")
        license_asset = self.license(model="concurrent")
        invalid = [
            {}, {"person_id": person["id"], "identity_id": identity["id"]},
            {"person_id": person["id"], "target_id": device["id"]},
            {"identity_id": identity["id"], "target_id": device["id"]},
            {"person_id": person["id"], "assignee": "Alice"},
            {"identity_id": identity["id"], "assignee": "support"},
            {"person_id": ""}, {"person_id": 0}, {"person_id": -1},
            {"person_id": "1.5"}, {"person_id": "9223372036854775808"},
            {"identity_id": ""}, {"identity_id": 0}, {"identity_id": "1x"},
            {"person_id": person["id"], "subject_kind": "person"},
            {"person_id": person["id"], "subject_snapshot": "{}"},
        ]
        for fields in invalid:
            with self.subTest(fields=fields):
                self.assertEqual(self.add_relation(license_asset, "licensed_to", **fields)[0], 400)
        base = "version=1&actor_id=7&relation_type=licensed_to&person_id=%d" % person["id"]
        for suffix in ("&person_id=2", "&person%5fid=2"):
            self.assertEqual(self.request("POST", "/v1/assets/%d/relations" % license_asset["id"], raw=base + suffix)[0], 400)
        self.assertEqual(self.get(license_asset), license_asset)
        self.assertEqual(self.relations(license_asset)["items"], [])
        self.assertEqual(len(self.history(license_asset)), 1)

    def test_missing_and_inactive_directory_recipients_are_rejected_but_removal_works(self):
        self.start()
        license_asset = self.license()
        for field in ("person_id", "identity_id"):
            self.assertEqual(self.add_relation(license_asset, "licensed_to", **{field: 999999})[0], 404)
        person = self.person()
        identity = self.identity()
        license_asset, person_relation = self.relate(license_asset, "licensed_to", person_id=person["id"])
        license_asset, identity_relation = self.relate(license_asset, "licensed_to", identity_id=identity["id"])
        for is_identity, entity in ((False, person), (True, identity)):
            status, value = self.update_entity(is_identity, entity, active=0)
            self.assertEqual(status, 200, value)
        other_license = self.license("OTHER")
        self.assertEqual(self.add_relation(other_license, "licensed_to", person_id=person["id"])[0], 409)
        self.assertEqual(self.add_relation(other_license, "licensed_to", identity_id=identity["id"])[0], 409)
        for relation in (person_relation, identity_relation):
            status, value = self.remove_relation(license_asset, relation)
            self.assertEqual(status, 200, value)
            license_asset = value["asset"]
            self.assertFalse(value["relation"]["active"])
        self.assertEqual(license_asset["license_usage"]["allocated"], 0)
        self.assertEqual(self.get(other_license), other_license)

    def test_duplicate_identity_is_by_reference_and_legacy_names_do_not_collide(self):
        self.start()
        person = self.person("P1", "Stesso Nome")
        other_person = self.person("P2", "Stesso Nome")
        identity = self.identity("same", "one.example", display_name="Stesso Nome")
        other_identity = self.identity("same", "two.example", display_name="Stesso Nome")
        license_asset = self.license()
        for field, entity in (("person_id", person), ("person_id", other_person), ("identity_id", identity), ("identity_id", other_identity)):
            license_asset, relation = self.relate(license_asset, "licensed_to", **{field: entity["id"]})
            self.assertEqual(relation["assignee"], "Stesso Nome")
            self.assertEqual(self.add_relation(license_asset, "licensed_to", **{field: entity["id"]})[0], 409)
        license_asset, legacy = self.relate(license_asset, "licensed_to", assignee=" Stesso Nome ")
        self.assertEqual(legacy["assignee"], "Stesso Nome")
        self.assertIsNone(legacy["person_id"])
        self.assertIsNone(legacy["identity_id"])
        self.assertIsNone(legacy["subject_snapshot"])
        self.assertEqual(self.add_relation(license_asset, "licensed_to", assignee="stesso nome")[0], 409)
        self.assertEqual(license_asset["license_usage"]["allocated"], 5)
        target = self.relations(license_asset)["items"][-1]
        status, removed = self.remove_relation(license_asset, target)
        self.assertEqual(status, 200, removed)
        license_asset, renewed = self.relate(removed["asset"], "licensed_to", person_id=person["id"])
        self.assertNotEqual(renewed["id"], target["id"])
        self.assertEqual(license_asset["license_usage"]["allocated"], 5)

    def test_capacity_expiry_and_stale_versions_keep_structured_allocations_atomic(self):
        self.start()
        person = self.person()
        identity = self.identity(kind="shared")
        license_asset = self.license(seats=3)
        license_asset, relation = self.relate(license_asset, "licensed_to", identity_id=identity["id"], quantity=2)
        self.assertEqual(self.add_relation(license_asset, "licensed_to", person_id=person["id"], quantity=2)[0], 409)
        self.assertEqual(self.add_relation(dict(license_asset, version=1), "licensed_to", person_id=person["id"])[0], 409)
        self.assertEqual(self.get(license_asset), license_asset)
        for status in ("retired", "lost"):
            disabled = self.license("DISABLED-" + status)
            code, changed = self.update(disabled, status=status)
            self.assertEqual(code, 200, changed)
            self.assertEqual(self.add_relation(changed["asset"], "licensed_to", person_id=person["id"])[0], 409)
        expired = self.license("EXPIRED", expires_on=(date.today() - timedelta(days=1)).isoformat())
        self.assertEqual(self.add_relation(expired, "licensed_to", identity_id=identity["id"])[0], 409)
        status, removed = self.remove_relation(license_asset, relation)
        self.assertEqual(status, 200, removed)
        license_asset, _ = self.relate(removed["asset"], "licensed_to", person_id=person["id"], quantity=3)
        self.assertEqual(license_asset["license_usage"]["available"], 0)

    def test_snapshots_survive_recipient_renames_deactivation_removal_and_restart(self):
        self.start()
        person = self.person()
        identity = self.identity(display_name="", kind="shared")
        license_asset = self.license()
        license_asset, person_relation = self.relate(license_asset, "licensed_to", person_id=person["id"])
        license_asset, identity_relation = self.relate(license_asset, "licensed_to", identity_id=identity["id"])
        self.assertEqual(identity_relation["assignee"], identity["login"])
        self.assertEqual(person_relation["subject_snapshot"], person)
        self.assertEqual(identity_relation["subject_snapshot"], identity)
        original_history = self.history(license_asset)
        for is_identity, entity, fields in ((False, person, dict(display_name="Nuovo nome", person_code="NEW")),
                                             (True, identity, dict(realm="new.example", login="new-login", display_name="Nuovo account"))):
            status, value = self.update_entity(is_identity, entity, active=0, **fields)
            self.assertEqual(status, 200, value)
        self.assertEqual(self.relations(license_asset)["items"], [identity_relation, person_relation])
        self.assertEqual(self.history(license_asset), original_history)
        for relation in (identity_relation, person_relation):
            status, value = self.remove_relation(license_asset, relation)
            self.assertEqual(status, 200, value)
            license_asset = value["asset"]
            self.assertEqual(value["relation"]["subject_snapshot"], relation["subject_snapshot"])
        events = self.history(license_asset)
        self.assertEqual(events[-3:], original_history)
        self.start()
        self.assertEqual(self.history(license_asset), events)
        self.assertEqual(self.get(license_asset), license_asset)

    def test_history_failure_rolls_back_structured_add_and_remove(self):
        self.start()
        person = self.person()
        license_asset = self.license()

        def reject_history(enabled):
            with closing(sqlite3.connect(self.db)) as database:
                database.execute("CREATE TRIGGER reject_structured_history BEFORE INSERT ON asset_history BEGIN SELECT RAISE(FAIL, 'fixture failure'); END" if enabled else "DROP TRIGGER reject_structured_history")
                database.commit()

        reject_history(True)
        self.assertEqual(self.add_relation(license_asset, "licensed_to", person_id=person["id"])[0], 500)
        self.assertEqual(self.get(license_asset), license_asset)
        self.assertEqual(self.relations(license_asset)["items"], [])
        reject_history(False)
        license_asset, relation = self.relate(license_asset, "licensed_to", person_id=person["id"])
        reject_history(True)
        self.assertEqual(self.remove_relation(license_asset, relation)[0], 500)
        self.assertEqual(self.get(license_asset), license_asset)
        self.assertEqual(self.relations(license_asset)["items"], [relation])
        self.assertEqual(len(self.history(license_asset)), 2)
        self.assertEqual(self.detail(False, person), person)

    def test_structured_relation_pages_preserve_byte_budget_and_history(self):
        self.start()
        license_asset = self.license(seats=40)
        created = []
        for index in range(18):
            person = self.person("LARGE-%d" % index, "Persona %d" % index, notes="\\\n" * 1000)
            license_asset, relation = self.relate(license_asset, "licensed_to", person_id=person["id"], notes="\\\n" * 1000)
            created.append(relation["id"])
        seen, cursor, pages = [], None, 0
        while True:
            query = "?limit=100" + ("&after_id=%d" % cursor if cursor else "")
            page = self.relations(license_asset, query)
            self.assertTrue(page["items"])
            seen.extend(row["id"] for row in page["items"])
            cursor = page["next_after_id"]
            pages += 1
            self.assertLess(pages, 10)
            if cursor is None:
                break
            self.assertEqual(cursor, page["items"][-1]["id"])
        self.assertGreater(pages, 1)
        self.assertEqual(seen, list(reversed(created)))
        self.assertEqual(self.get(license_asset)["license_usage"]["allocated"], 18)


if __name__ == "__main__":
    unittest.main()
