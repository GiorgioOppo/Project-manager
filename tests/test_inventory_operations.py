"""Operational inventory counts, shared filters and append-only alert sources."""

from datetime import date, timedelta
import unittest
from urllib.parse import urlencode

import test_inventory_addon as fixtures
import test_inventory_custody as custody


class InventoryOperationsTests(unittest.TestCase):
    setUpClass = classmethod(fixtures.InventoryAddonTests.setUpClass.__func__)
    setUp = fixtures.InventoryAddonTests.setUp
    environment = fixtures.InventoryAddonTests.environment
    stop = fixtures.InventoryAddonTests.stop
    start = fixtures.InventoryAddonTests.start
    request = fixtures.InventoryAddonTests.request
    create = fixtures.InventoryAddonTests.create
    update = fixtures.InventoryAddonTests.update
    place = custody.InventoryCustodyTests.place
    place_update = custody.InventoryCustodyTests.place_update
    act = custody.InventoryCustodyTests.act
    assign_context = custody.InventoryCustodyTests.assign_context
    checkout = custody.InventoryCustodyTests.checkout
    get = custody.InventoryCustodyTests.get

    def fetch(self, endpoint, **query):
        status, value = self.request("GET", endpoint + ("?" + urlencode(query) if query else ""))
        self.assertEqual(status, 200, value)
        return value

    def verify(self, asset, when, location=None):
        status, value = self.act(asset, "verify", verified_on=when.isoformat(), condition="good",
                                 observed_location_id=location["id"] if location else "")
        self.assertEqual(status, 201, value)
        return value["asset"], value["verification"]

    def test_dashboard_counts_full_population_and_chronological_verification(self):
        self.start()
        today = date.today()
        site = self.place("site", "Roma")
        department = self.place("department", "Tecnica", site)
        expected = self.place("location", "Scaffale A", site)
        other = self.place("location", "Scaffale B", site)
        first = self.assign_context(self.create(tag="LATE"), site=site, department=department)
        first, loan = self.checkout(first, checkout_on=(today - timedelta(days=5)).isoformat(),
                                    due_on=(today - timedelta(days=1)).isoformat())
        stale = self.assign_context(self.create(tag="STALE"), site=site, department=department, location=expected)
        stale, record = self.verify(stale, today - timedelta(days=90), expected)
        stale, backdated = self.verify(stale, today - timedelta(days=100), other)
        self.assertGreater(backdated["id"], record["id"])
        self.assertEqual(stale["last_verification"]["id"], record["id"])
        moved = self.assign_context(self.create(tag="MOVED", asset_type="physical"), site=site, location=expected)
        moved, _ = self.verify(moved, today, other)
        self.create(tag="NET", asset_type="network_device")
        self.create(tag="SW", asset_type="software")
        self.create(tag="RETIRED", status="retired")
        self.create(tag="LOST", status="lost")
        # Catalog pages remain bounded; dashboard counts include all matching rows.
        page = self.fetch("/v1/assets", limit=1)
        self.assertEqual(len(page["items"]), 1)
        self.assertIsInstance(page["next_after_id"], int)
        dashboard = self.fetch("/v1/dashboard", today=today.isoformat())
        self.assertEqual(dashboard["counts"], {"total": 7, "open_custody": 1, "overdue_returns": 1,
                                               "never_verified": 2, "verification_due": 1, "location_mismatch": 1})
        self.assertEqual(dashboard["verification_days"], 90)
        self.assertEqual(dashboard["today"], today.isoformat())
        self.assertEqual(dashboard["by_status"], {"available": 4, "assigned": 1, "maintenance": 0, "retired": 1, "lost": 1})
        self.assertEqual(dashboard["by_type"]["software"], 1)
        self.assertEqual(dashboard["by_type"]["license"], 0)
        self.assertFalse(dashboard["groups_truncated"])
        self.assertEqual(dashboard["by_site"], [{"id": None, "name": "", "count": 4},
                                               {"id": site["id"], "name": "Roma", "count": 3}])
        self.assertEqual(dashboard["by_user"][0], {"id": None, "name": "", "count": 6})
        scoped = self.fetch("/v1/dashboard", site_id=site["id"], department_id=department["id"])
        self.assertEqual(scoped["counts"]["total"], 2)
        self.assertEqual(scoped["counts"]["never_verified"], 1)
        self.assertEqual(scoped["counts"]["location_mismatch"], 0)
        self.assertEqual(self.fetch("/v1/dashboard", verification_days=91)["counts"]["verification_due"], 0)
        self.assertEqual(self.fetch("/v1/operational-due", kind="return")["items"][0]["source_id"], loan["id"])

    def test_catalog_export_dashboard_share_site_department_attention_and_search(self):
        self.start()
        today = date.today()
        site = self.place("site", "Roma")
        department = self.place("department", "Tecnica", site)
        other = self.place("site", "Milano")
        late = self.assign_context(self.create(tag="LATE"), site=site, department=department)
        late, _ = self.checkout(late, checkout_on=(today - timedelta(days=3)).isoformat(),
                                due_on=(today - timedelta(days=1)).isoformat())
        stale = self.assign_context(self.create(tag="STALE"), site=site, department=department, user=22, user_name="Giulia")
        stale, _ = self.verify(stale, today - timedelta(days=30))
        other_asset = self.assign_context(self.create(tag="OTHER"), site=other)
        for query, expected in (({"site_id": site["id"]}, [stale["id"], late["id"]]),
                                ({"department_id": department["id"], "attention": "open_custody"}, [late["id"]]),
                                ({"site_id": site["id"], "attention": "overdue_returns"}, [late["id"]]),
                                ({"attention": "never_verified", "site_id": other["id"]}, [other_asset["id"]]),
                                ({"attention": "verification_due", "verification_days": 30}, [stale["id"]]),
                                ({"assignee_user_id": 22}, [stale["id"]]),
                                ({"q": "Tecnica"}, [stale["id"], late["id"]]),
                                ({"q": "Giulia"}, [stale["id"]]),
                                ({"site_id": site["id"], "department_id": 999999}, [])):
            with self.subTest(query=query):
                page = self.fetch("/v1/assets", **query)
                self.assertEqual([a["id"] for a in page["items"]], expected)
                exported = self.fetch("/v1/assets/export", **query)
                self.assertEqual(exported["exported"], len(expected))
                self.assertEqual(self.fetch("/v1/dashboard", **query)["counts"]["total"], len(expected))
        status, renamed = self.place_update(site, name="Roma centro")
        self.assertEqual(status, 200, renamed)
        group = self.fetch("/v1/dashboard", site_id=site["id"])["by_site"][0]
        self.assertEqual(group["name"], "Roma centro")
        self.assertEqual(len(self.fetch("/v1/assets", q="Roma centro")["items"]), 2)

    def test_operational_returns_exclude_due_today_and_closed_custody_and_keep_generation(self):
        self.start()
        today = date.today()
        first = self.create(tag="LATE")
        first, old = self.checkout(first, checkout_on=(today - timedelta(days=10)).isoformat(),
                                  due_on=(today - timedelta(days=1)).isoformat())
        on_time, _ = self.checkout(self.create(tag="TODAY"), due_on=today.isoformat())
        self.checkout(self.create(tag="UNSET"))
        self.checkout(self.create(tag="FUTURE"), due_on=(today + timedelta(days=1)).isoformat())
        due = self.fetch("/v1/operational-due", kind="return", today=today.isoformat())
        self.assertEqual(len(due["items"]), 1)
        self.assertEqual(due["items"][0], {"asset_id": first["id"], "asset_tag": "LATE", "name": first["name"],
                                           "asset_type": "it_device", "due_kind": "return_due", "due_on": old["due_on"],
                                           "days_remaining": -1, "source_id": old["id"]})
        status, result = self.act(first, "checkin", returned_on=today.isoformat(), condition_in="good", status="available")
        self.assertEqual(status, 200, result)
        self.assertEqual(self.fetch("/v1/operational-due", kind="return")["items"], [])
        first, new = self.checkout(result["asset"], checkout_on=(today - timedelta(days=2)).isoformat(), due_on=old["due_on"])
        self.assertGreater(new["id"], old["id"])
        self.assertEqual(self.fetch("/v1/operational-due", kind="return")["items"][0]["source_id"], new["id"])
        # Prior due endpoint keeps its original four deadline kinds only.
        self.assertEqual(self.fetch("/v1/due", today=today.isoformat())["items"], [])

    def test_verification_due_calendar_boundary_unverified_and_backdated_record(self):
        self.start()
        stale, first = self.verify(self.create(tag="STALE"), date(2024, 2, 29))
        stale, older = self.verify(stale, date(2024, 2, 28))
        self.assertGreater(older["id"], first["id"])
        before = self.fetch("/v1/operational-due", kind="verification", today="2024-03-29", verification_days=30)
        self.assertEqual(before["items"], [])
        boundary = self.fetch("/v1/operational-due", kind="verification", today="2024-03-30", verification_days=30)
        self.assertEqual(boundary["items"][0]["due_on"], "2024-03-30")
        self.assertEqual(boundary["items"][0]["days_remaining"], 0)
        self.assertEqual(boundary["items"][0]["source_id"], first["id"])
        later = self.fetch("/v1/operational-due", kind="verification", today="2024-03-31", verification_days=30)
        self.assertEqual(later["items"][0]["days_remaining"], -1)
        unverified = self.create(tag="NEW")
        historical = self.fetch("/v1/operational-due", kind="verification", today="2024-03-30", verification_days=30)
        self.assertEqual([e["asset_id"] for e in historical["items"]], [stale["id"]])
        self.create(tag="SW", asset_type="software")
        self.create(tag="RETIRED", status="retired")
        self.create(tag="LOST", status="lost")
        events = self.fetch("/v1/operational-due", kind="verification")["items"]
        self.assertEqual([e["asset_id"] for e in events], [unverified["id"], stale["id"]])
        self.assertEqual(events[0]["source_id"], 0)
        self.assertEqual(events[0]["due_on"], unverified["created_at"][:10])
        self.assertEqual(self.fetch("/v1/dashboard")["counts"]["never_verified"], 1)
        self.assertEqual(self.get(stale)["last_verification"]["id"], first["id"])

    def test_operational_due_asset_cursor_pages_are_complete_and_descending(self):
        self.start()
        expected = [self.create(tag="ASSET-%02d" % i)["id"] for i in range(7)]
        cursor = None
        seen = []
        while True:
            query = {"kind": "verification", "limit": 2}
            if cursor:
                query["after_id"] = cursor
            page = self.fetch("/v1/operational-due", **query)
            seen.extend(item["asset_id"] for item in page["items"])
            next_cursor = page["next_after_id"]
            if next_cursor is None:
                break
            self.assertEqual(next_cursor, page["items"][-1]["asset_id"])
            if cursor:
                self.assertLess(next_cursor, cursor)
            cursor = next_cursor
        self.assertEqual(seen, expected[::-1])

    def test_dashboard_top_groups_bounded_and_unassigned_retained(self):
        self.start()
        self.create(tag="UNASSIGNED")
        sites = []
        for index in range(22):
            site = self.place("site", "Sede %02d" % index)
            sites.append(site)
            # Give every named group 2 members, so the null bucket is below top20.
            for suffix in ("A", "B"):
                self.assign_context(self.create(tag="%02d-%s" % (index, suffix)), site=site,
                                    user=index + 100, user_name="Persona %02d" % index)
        dashboard = self.fetch("/v1/dashboard")
        self.assertEqual(dashboard["counts"]["total"], 45)
        self.assertTrue(dashboard["groups_truncated"])
        self.assertEqual(len(dashboard["by_site"]), 20)
        self.assertEqual(dashboard["by_site"][0]["id"], sites[0]["id"])
        self.assertEqual(dashboard["by_site"][-1], {"id": None, "name": "", "count": 1})
        self.assertEqual(len(dashboard["by_user"]), 20)
        self.assertEqual(dashboard["by_user"][-1]["id"], None)
        self.assertEqual(dashboard["by_department"], [{"id": None, "name": "", "count": 45}])

    def test_query_validation_and_read_only_endpoints(self):
        self.start()
        invalid = ({"site_id": ""}, {"site_id": 0}, {"department_id": -1}, {"attention": ""},
                   {"attention": "unknown"}, {"verification_days": 0}, {"verification_days": 366},
                   {"today": "2024-02-30"}, {"today": "a"}, {"today": ""}, {"status": "bad"},
                   {"asset_type": "bad"}, {"assignee_user_id": 0})
        for endpoint in ("/v1/assets", "/v1/assets/export", "/v1/dashboard"):
            for query in invalid:
                with self.subTest(endpoint=endpoint, query=query):
                    self.assertEqual(self.request("GET", endpoint + "?" + urlencode(query))[0], 400)
        for query in ({"limit": 1}, {"after_id": 1}, {"unknown": 1}):
            self.assertEqual(self.request("GET", "/v1/dashboard?" + urlencode(query))[0], 400)
        for query in ({}, {"kind": "bad"}, {"kind": "return", "limit": 101}, {"kind": "return", "after_id": 0},
                      {"kind": "verification", "verification_days": 366}, {"kind": "return", "today": "a"}):
            self.assertEqual(self.request("GET", "/v1/operational-due?" + urlencode(query))[0], 400)
        self.assertEqual(self.request("GET", "/v1/dashboard?site_id=1&site_id=2")[0], 400)
        for endpoint in ("/v1/dashboard", "/v1/operational-due"):
            self.assertEqual(self.request("POST", endpoint, {"actor_id": 7})[0], 405)
        empty = self.fetch("/v1/dashboard")
        self.assertEqual(empty["counts"]["total"], 0)
        self.assertEqual(empty["by_site"], [])
        self.assertFalse(empty["groups_truncated"])


if __name__ == "__main__":
    unittest.main()
