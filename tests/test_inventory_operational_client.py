"""The app resolves account identities before forwarding inventory operations."""

import http.client
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

import test_inventory_client as bridge


class InventoryOperationalBridgeTests(unittest.TestCase):
    setUpClass = classmethod(bridge.InventoryBridgeTests.setUpClass.__func__)
    setUp = bridge.InventoryBridgeTests.setUp
    start_server = bridge.InventoryBridgeTests.start_server
    stop_server = bridge.InventoryBridgeTests.stop_server
    api = bridge.InventoryBridgeTests.api
    login = bridge.InventoryBridgeTests.login
    create_user = bridge.InventoryBridgeTests.create_user
    assert_error = bridge.InventoryBridgeTests.assert_error
    users = bridge.InventoryBridgeTests.users
    edit_user = bridge.InventoryBridgeTests.edit_user

    def test_operational_reads_keep_account_authorization_and_pagination(self):
        for path in ("places", "assets/7/custody", "assets/7/verifications"):
            self.assert_error(401, "GET", "/api/inventory/" + path)
            status, _, _ = self.api("GET", "/api/inventory/" + path + "?limit=10&after_id=8",
                                    cookie=self.worker_cookie)
            self.assertEqual(status, 200)
            self.assertEqual(self.service.requests[-1]["path"], "/v1/" + path + "?limit=10&after_id=8")

    def test_directory_exposes_only_active_identity_fields_to_managers(self):
        inactive = self.create_user("inactive@example.test", "Inactive-only!2026")
        self.assertEqual(self.edit_user(inactive, active="0")[0], 200)
        self.assert_error(401, "GET", "/api/inventory/users")
        self.assert_error(403, "GET", "/api/inventory/users", cookie=self.worker_cookie)
        status, result, _ = self.api("GET", "/api/inventory/users", cookie=self.manager_cookie)
        self.assertEqual(status, 200)
        self.assertEqual({row["id"] for row in result["users"]}, {self.manager_id, self.worker_id})
        for row in result["users"]:
            self.assertEqual(set(row), {"id", "name", "email"})
        self.assertEqual(self.service.requests, [])
        self.assert_error(400, "GET", "/api/inventory/users?active=1", cookie=self.manager_cookie)

    def test_context_and_checkout_resolve_the_current_active_user_name(self):
        long_name = "😀" * 120
        self.assertEqual(self.edit_user(self.worker_id, name=long_name)[0], 200)
        for action, fields, expected_key in (
            ("context", {"version": "4", "assignee_user_id": str(self.worker_id),
                         "site_id": "2", "department_id": "", "location_id": "3"}, "assignee_user_name"),
            ("checkout", {"version": "4", "user_id": str(self.worker_id), "checkout_on": "2026-10-10",
                          "due_on": "2026-10-20", "condition_out": "good", "notes": "Consegna & caffè"}, "user_name"),
        ):
            status, value, _ = self.api("POST", "/api/inventory/assets/7/" + action, fields,
                                        cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            request = self.service.requests[-1]
            self.assertEqual(request["path"], "/v1/assets/7/" + action)
            expected = dict(fields, actor_id=str(self.manager_id), **{expected_key: long_name})
            self.assertEqual(request["fields"], {key: [value] for key, value in expected.items()})

    def test_missing_and_inactive_users_are_rejected_before_service_call(self):
        self.assertEqual(self.edit_user(self.worker_id, active="0")[0], 200)
        for user_id in (self.worker_id, 987654):
            for action, fields in (
                ("context", {"version": "4", "assignee_user_id": str(user_id)}),
                ("checkout", {"version": "4", "user_id": str(user_id), "checkout_on": "2026-10-10",
                              "condition_out": "good"}),
            ):
                self.assert_error(422, "POST", "/api/inventory/assets/7/" + action, fields,
                                  cookie=self.manager_cookie)
        self.assertEqual(self.service.requests, [])

    def test_my_assets_filter_is_bound_to_the_authenticated_account(self):
        for path in ("assets", "assets/export"):
            status, _, _ = self.api("GET", "/api/inventory/" + path + "?q=Portatile&mine=1&limit=10",
                                    cookie=self.worker_cookie)
            self.assertEqual(status, 200)
            query = parse_qs(urlsplit(self.service.requests[-1]["path"]).query)
            self.assertEqual(query, {"q": ["Portatile"], "assignee_user_id": [str(self.worker_id)], "limit": ["10"]})
            self.assert_error(400, "GET", "/api/inventory/" + path + "?assignee_user_id=1", cookie=self.worker_cookie)
            self.assert_error(400, "GET", "/api/inventory/" + path + "?mine=0", cookie=self.worker_cookie)

    def test_mutations_are_manager_only_and_receive_trusted_actor(self):
        cases = (
            ("places", {"kind": "site", "name": "Milano", "parent_id": "", "active": "1"}),
            ("places/3", {"version": "2", "kind": "location", "name": "Magazzino", "parent_id": "1", "active": "0"}),
            ("assets/7/context", {"version": "4", "assignee_user_id": "", "site_id": "", "department_id": "", "location_id": ""}),
            ("assets/7/checkout", {"version": "4", "user_id": str(self.worker_id), "checkout_on": "2026-10-10", "condition_out": "good"}),
            ("assets/7/checkin", {"version": "4", "returned_on": "2026-10-11", "condition_in": "damaged", "status": "maintenance", "notes": "Schermo"}),
            ("assets/7/verify", {"version": "4", "verified_on": "2026-10-11", "condition": "fair", "observed_location_id": "3"}),
        )
        for path, fields in cases:
            route = "/api/inventory/" + path
            self.assert_error(401, "POST", route, fields)
            self.assert_error(403, "POST", route, fields, cookie=self.worker_cookie)
            self.assert_error(403, "POST", route, fields, cookie=self.manager_cookie,
                              headers={"Origin": "https://hostile.invalid"})
            if path != "places":
                self.assert_error(405, "GET", route, cookie=self.manager_cookie)
            status, result, _ = self.api("POST", route, fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, result)
            self.assertEqual(self.service.requests[-1]["fields"]["actor_id"], [str(self.manager_id)])

    def test_browser_identity_snapshots_and_duplicate_fields_are_rejected(self):
        for action, identity in (("context", "assignee_user_name"), ("checkout", "user_name")):
            fields = {"version": "4", "assignee_user_id" if action == "context" else "user_id": str(self.worker_id)}
            for key in (identity, "actor_id", "user_name", "assignee_user_name", "owner_id"):
                self.assert_error(400, "POST", "/api/inventory/assets/7/" + action, dict(fields, **{key: "Forged"}),
                                  cookie=self.manager_cookie)
            for suffix in ("&%76ersion=5", "&actor%5fid=1", "&user%5fname=Forged", "&unknown=1", "&"):
                self.assert_error(400, "POST", "/api/inventory/assets/7/" + action,
                                  cookie=self.manager_cookie, body=urlencode(fields) + suffix,
                                  headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.service.requests, [])

    def test_operational_route_and_parameter_bounds(self):
        for path in ("places/0", "places/1/extra", "assets/1/checkout/extra", "assets/0/context"):
            self.assert_error(404, "POST", "/api/inventory/" + path, {}, cookie=self.manager_cookie)
        for path in ("assets/7/custody", "assets/7/verifications"):
            self.assert_error(405, "POST", "/api/inventory/" + path, {}, cookie=self.manager_cookie)
            for query in ("limit=101", "after_id=0", "actor_id=1", "user_id=2"):
                self.assert_error(400, "GET", "/api/inventory/" + path + "?" + query, cookie=self.worker_cookie)
        for body in ("version=0", "version=1&condition=unknown", "version=1&observed_location_id=-1", "version=1&notes=" + "a" * 2001):
            self.assert_error(400, "POST", "/api/inventory/assets/7/verify", cookie=self.manager_cookie, body=body,
                              headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.service.requests, [])

    def test_dashboard_and_operational_deadlines_are_authenticated_read_only(self):
        for path in ("dashboard", "operational-due"):
            route = "/api/inventory/" + path
            self.assert_error(401, "GET", route)
            for cookie in (self.manager_cookie, self.worker_cookie):
                status, _, _ = self.api("GET", route, cookie=cookie)
                self.assertEqual(status, 200)
                self.assertEqual(self.service.requests[-1]["path"], "/v1/" + path)
            self.assert_error(405, "POST", route, {}, cookie=self.manager_cookie)
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
            try:
                connection.request("HEAD", route, headers={"Cookie": self.worker_cookie})
                response = connection.getresponse()
                self.assertEqual(response.status, 405)
                self.assertEqual(response.read(), b"")
            finally:
                connection.close()
        self.assertEqual(len(self.service.requests), 4)

    def test_dashboard_catalog_and_export_share_scoped_filters(self):
        for path in ("dashboard", "assets", "assets/export"):
            fields = {"q": "Portatile & qualità", "site_id": "3", "department_id": "4", "mine": "1",
                      "attention": "verification_due", "verification_days": "90", "today": "2026-10-11",
                      "status": "assigned", "asset_type": "it_device"}
            status, value, _ = self.api("GET", "/api/inventory/" + path + "?" + urlencode(fields),
                                        cookie=self.worker_cookie)
            self.assertEqual(status, 200, value)
            expected = dict(fields)
            del expected["mine"]
            expected["assignee_user_id"] = str(self.worker_id)
            request = self.service.requests[-1]
            self.assertEqual(parse_qs(urlsplit(request["path"]).query), {key: [value] for key, value in expected.items()})
            self.assertEqual(request["fields"], {})

    def test_new_filters_are_strict_before_contacting_private_service(self):
        for path in ("dashboard", "assets", "assets/export"):
            for query in ("site_id=0", "site_id=-1", "department_id=", "department_id=9223372036854775808",
                          "verification_days=0", "verification_days=366", "verification_days=1.5", "attention=unknown",
                          "today=2026-02-30", "today=0000-01-01", "today=2026-13-01", "assignee_user_id=1",
                          "site_id=1&site%5fid=2", "actor_id=1"):
                self.assert_error(400, "GET", "/api/inventory/" + path + "?" + query, cookie=self.worker_cookie)
        for query in ("limit=10", "after_id=7"):
            self.assert_error(400, "GET", "/api/inventory/dashboard?" + query, cookie=self.worker_cookie)
        for query in ("kind=unknown", "kind=return&kind=verification", "mine=1", "site_id=1", "today=2026-02-30",
                      "verification_days=366", "limit=101", "after_id=0", "actor_id=1"):
            self.assert_error(400, "GET", "/api/inventory/operational-due?" + query, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_operational_deadline_query_is_forwarded_without_identity(self):
        for kind in ("return", "verification"):
            fields = {"kind": kind, "today": "2026-10-11", "verification_days": "365", "limit": "100", "after_id": "51"}
            status, value, _ = self.api("GET", "/api/inventory/operational-due?" + urlencode(fields), cookie=self.worker_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(parse_qs(urlsplit(self.service.requests[-1]["path"]).query),
                             {key: [value] for key, value in fields.items()})


if __name__ == "__main__":
    unittest.main()
