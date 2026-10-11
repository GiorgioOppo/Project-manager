"""Asset subject pagination and rejection boundaries against isolated fixtures."""

import unittest

import test_inventory_subjects as subjects


class InventorySubjectBoundsTests(unittest.TestCase):
    setUpClass = classmethod(subjects.InventorySubjectsTests.setUpClass.__func__)
    setUp = subjects.InventorySubjectsTests.setUp
    environment = subjects.InventorySubjectsTests.environment
    stop = subjects.InventorySubjectsTests.stop
    start = subjects.InventorySubjectsTests.start
    request = subjects.InventorySubjectsTests.request
    create = subjects.InventorySubjectsTests.create
    update = subjects.InventorySubjectsTests.update
    person = subjects.InventorySubjectsTests.person
    identity = subjects.InventorySubjectsTests.identity
    link = subjects.InventorySubjectsTests.link
    get = subjects.InventorySubjectsTests.get

    def test_response_budget_pages_preserve_all_subject_links_once(self):
        self.start()
        asset = self.create()
        expected = []
        for number in range(32):
            status, value = self.request("POST", "/v1/people", dict(
                person_code=f"P-{number}", display_name="Persona " * 20,
                notes="\n" * 2000, actor_id=7))
            self.assertEqual(status, 201, value)
            asset, link = self.link(asset, "person", value["person"], notes="\n" * 2000)
            expected.append(link["id"])

        seen, page_sizes, after = [], [], None
        while True:
            query = "?limit=100" + (f"&after_id={after}" if after is not None else "")
            page = self.get(f'/v1/assets/{asset["id"]}/subjects' + query)
            ids = [item["id"] for item in page["items"]]
            self.assertTrue(ids, "A page with a continuation cursor must make progress")
            page_sizes.append(len(ids))
            seen.extend(ids)
            cursor = page["next_after_id"]
            if cursor is None:
                break
            self.assertEqual(cursor, ids[-1])
            if after is not None:
                self.assertLess(cursor, after)
            after = cursor
            self.assertLess(len(page_sizes), 10, "Pagination must terminate")

        self.assertEqual(page_sizes, [9, 9, 9, 5])
        self.assertEqual(seen, list(reversed(expected)))
        self.assertEqual(len(set(seen)), 32)

    def test_removal_cannot_target_a_link_belonging_to_another_asset(self):
        self.start()
        person = self.person()
        first, link = self.link(self.create("PC-1"), "person", person)
        second = self.create("PC-2")
        route = f'/v1/assets/{second["id"]}/subjects/{link["id"]}/remove'
        status, value = self.request("POST", route, dict(version=second["version"], actor_id=8))
        self.assertEqual(status, 404, value)
        self.assertEqual(self.get(f'/v1/assets/{first["id"]}')["asset"], first)
        self.assertEqual(self.get(f'/v1/assets/{second["id"]}')["asset"], second)
        self.assertEqual(self.get(f'/v1/assets/{first["id"]}/subjects')["items"], [link])
        self.assertEqual(self.get(f'/v1/assets/{second["id"]}/subjects')["items"], [])
        self.assertEqual(len(self.get(f'/v1/assets/{first["id"]}/history')["items"]), 2)
        self.assertEqual(len(self.get(f'/v1/assets/{second["id"]}/history')["items"]), 1)

    def test_inactive_identity_keeps_existing_links_and_allows_only_removal(self):
        self.start()
        identity = self.identity()
        first, link = self.link(self.create("PC-1"), "identity", identity)
        second = self.create("PC-2")
        status, value = self.request("POST", f'/v1/identities/{identity["id"]}', dict(
            realm=identity["realm"], login=identity["login"], kind=identity["kind"],
            version=identity["version"], active=0, actor_id=8))
        self.assertEqual(status, 200, value)
        live = self.get(f'/v1/assets/{first["id"]}/subjects')["items"][0]
        self.assertFalse(live["subject"]["active"])
        self.assertTrue(live["subject_snapshot"]["active"])
        self.assertEqual(self.get(f'/v1/identities/{identity["id"]}/assets')["items"], [live])

        status, value = self.request("POST", f'/v1/assets/{second["id"]}/subjects', dict(
            version=second["version"], actor_id=8, subject_kind="identity", subject_id=identity["id"]))
        self.assertEqual(status, 409, value)
        self.assertEqual(self.get(f'/v1/assets/{second["id"]}')["asset"], second)
        self.assertEqual(len(self.get(f'/v1/assets/{second["id"]}/history')["items"]), 1)

        status, value = self.request("POST", f'/v1/assets/{first["id"]}/subjects/{link["id"]}/remove',
                                     dict(version=first["version"], actor_id=8))
        self.assertEqual(status, 200, value)
        self.assertFalse(value["link"]["active"])
        self.assertEqual(self.get(f'/v1/identities/{identity["id"]}/assets')["items"], [])

    def test_lost_asset_rejects_new_subjects_but_allows_removal(self):
        self.start()
        person, identity = self.person(), self.identity()
        asset, link = self.link(self.create(), "person", person)
        status, value = self.update(asset, status="lost")
        self.assertEqual(status, 200, value)
        asset = value["asset"]
        before_history = self.get(f'/v1/assets/{asset["id"]}/history')["items"]
        status, value = self.request("POST", f'/v1/assets/{asset["id"]}/subjects', dict(
            version=asset["version"], actor_id=8, subject_kind="identity", subject_id=identity["id"]))
        self.assertEqual(status, 409, value)
        self.assertEqual(self.get(f'/v1/assets/{asset["id"]}')["asset"], asset)
        self.assertEqual(self.get(f'/v1/assets/{asset["id"]}/history')["items"], before_history)
        self.assertEqual(self.get(f'/v1/identities/{identity["id"]}/assets')["items"], [])

        status, value = self.request("POST", f'/v1/assets/{asset["id"]}/subjects/{link["id"]}/remove',
                                     dict(version=asset["version"], actor_id=8))
        self.assertEqual(status, 200, value)
        self.assertEqual(value["asset"]["status"], "lost")
        self.assertFalse(value["link"]["active"])
        self.assertEqual(self.get(f'/v1/assets/{asset["id"]}/subjects')["items"], [])


if __name__ == "__main__":
    unittest.main()
