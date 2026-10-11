"""People and identities are inventory subjects, independent of app accounts."""

import http.client
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit

import test_inventory_client as bridge


class InventoryPeopleBridgeTests(unittest.TestCase):
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

    READS = ("people", "people/7", "people/7/history", "people/7/identities", "people/7/assets",
             "identities", "identities/9", "identities/9/history", "identities/9/people", "identities/9/assets",
             "assets/5/subjects")

    def mutations(self):
        return (
            ("people", {"person_code": "EXT-42", "display_name": "Persona senza account", "active": "1"}),
            ("people/7", {"person_code": "EXT-42", "display_name": "Persona aggiornata", "version": "3", "active": "0"}),
            ("identities", {"realm": "Entra ID", "login": "shared@example.test", "kind": "shared"}),
            ("identities/9", {"realm": "Entra ID", "login": "service@example.test", "kind": "service", "version": "4"}),
            ("identities/9/people", {"person_id": "987654", "version": "4", "role": "owner", "notes": "Accesso condiviso"}),
            ("identities/9/people/13/remove", {"version": "5"}),
            ("assets/5/subjects", {"subject_kind": "person", "subject_id": "987654", "version": "6", "role": "administrator"}),
            ("assets/5/subjects/17/remove", {"version": "7"}),
        )

    def test_current_authenticated_accounts_can_read_all_subject_directories(self):
        for path in self.READS:
            route = "/api/inventory/" + path
            self.assert_error(401, "GET", route)
            for cookie in (self.manager_cookie, self.worker_cookie):
                status, _, _ = self.api("GET", route, cookie=cookie)
                self.assertEqual(status, 200)
                request = self.service.requests[-1]
                self.assertEqual(request["path"], "/v1/" + path)
                self.assertEqual(request["fields"], {})
                self.assertEqual(request["authorization"], "Bearer " + bridge.TOKEN)

    def test_subject_mutations_require_manager_origin_and_receive_trusted_actor(self):
        for path, fields in self.mutations():
            route = "/api/inventory/" + path
            self.assert_error(401, "POST", route, fields)
            self.assert_error(403, "POST", route, fields, cookie=self.worker_cookie)
            self.assert_error(403, "POST", route, fields, cookie=self.manager_cookie,
                              headers={"Origin": "https://hostile.invalid"})
            status, value, _ = self.api("POST", route, fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            request = self.service.requests[-1]
            self.assertEqual((request["method"], request["path"]), ("POST", "/v1/" + path))
            self.assertEqual(request["fields"], {key: [value] for key, value in dict(fields, actor_id=str(self.manager_id)).items()})

    def test_shared_identity_links_do_not_resolve_people_as_app_accounts(self):
        # An arbitrary inventory person ID is forwarded without looking for a
        # corresponding app account. The private inventory owns its validity.
        self.assertEqual(self.edit_user(self.worker_id, active="0")[0], 200)
        cases = (
            ("identities/9/people", {"person_id": str(self.worker_id), "version": "1"}),
            ("identities/9/people", {"person_id": "987654", "version": "1"}),
            ("assets/5/subjects", {"subject_kind": "identity", "subject_id": "987654", "version": "1"}),
        )
        for path, fields in cases:
            status, value, _ = self.api("POST", "/api/inventory/" + path, fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(self.service.requests[-1]["fields"],
                             {key: [value] for key, value in dict(fields, actor_id=str(self.manager_id)).items()})

    def test_directory_fields_are_forwarded_canonically_and_remain_separate(self):
        cases = (
            ("people", {"person_code": "EXT & 42", "display_name": "Éva caffè", "email": "eva@example.test",
                        "organization": "Società & soci", "job_title": "Tecnico", "notes": "Riga 1\nRiga 2", "active": "1"}),
            ("identities", {"realm": "AD & cloud", "login": "team@example.test", "display_name": "Team qualità",
                            "kind": "shared", "notes": "Utenza di reparto\nAccesso autorizzato", "active": "0"}),
        )
        for path, fields in cases:
            status, value, _ = self.api("POST", "/api/inventory/" + path, fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(self.service.requests[-1]["fields"],
                             {key: [value] for key, value in dict(fields, actor_id=str(self.manager_id)).items()})
        for path, query in (
            ("people", {"q": "Éva & qualità", "active": "0", "limit": "100", "after_id": "99"}),
            ("identities", {"q": "Team & qualità", "active": "1", "kind": "shared", "person_id": "7", "limit": "25", "after_id": "99"}),
        ):
            status, value, _ = self.api("GET", "/api/inventory/" + path + "?" + urlencode(query), cookie=self.worker_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(parse_qs(urlsplit(self.service.requests[-1]["path"]).query),
                             {key: [value] for key, value in query.items()})

    def test_history_links_and_reverse_asset_lists_accept_only_pagination(self):
        for path in self.READS:
            if path in ("people", "identities", "people/7", "identities/9"):
                continue
            status, value, _ = self.api("GET", "/api/inventory/" + path + "?limit=10&after_id=20", cookie=self.worker_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(self.service.requests[-1]["path"], "/v1/" + path + "?limit=10&after_id=20")
            for query in ("q=hidden", "active=1", "person_id=7", "subject_kind=person", "limit=101", "after_id=0"):
                self.assert_error(400, "GET", "/api/inventory/" + path + "?" + query, cookie=self.worker_cookie)

    def test_catalog_dashboard_and_export_forward_direct_subject_filters(self):
        for path in ("assets", "assets/export", "dashboard"):
            query = {"q": "Portatile & qualità", "person_id": "7", "identity_id": "9", "site_id": "3",
                     "department_id": "4", "mine": "1", "attention": "verification_due", "verification_days": "90",
                     "today": "2026-10-11", "status": "assigned", "asset_type": "it_device"}
            status, value, _ = self.api("GET", "/api/inventory/" + path + "?" + urlencode(query), cookie=self.worker_cookie)
            self.assertEqual(status, 200, value)
            expected = dict(query)
            del expected["mine"]
            expected["assignee_user_id"] = str(self.worker_id)
            self.assertEqual(parse_qs(urlsplit(self.service.requests[-1]["path"]).query),
                             {key: [value] for key, value in expected.items()})
            for invalid in ("person_id=0", "identity_id=-1", "person_id=", "identity_id=9223372036854775808",
                            "person_id=7&person%5fid=9", "identity_id=7&%69dentity_id=9", "subject_kind=person"):
                self.assert_error(400, "GET", "/api/inventory/" + path + "?" + invalid, cookie=self.worker_cookie)

    def test_browser_actor_and_account_snapshots_never_cross_new_mutations(self):
        for path, fields in self.mutations():
            for key in ("actor_id", "user_id", "user_name", "assignee_user_name", "owner_id", "password", "access_token", "provider_url"):
                self.assert_error(400, "POST", "/api/inventory/" + path, dict(fields, **{key: "Forged"}), cookie=self.manager_cookie)
            key = next(iter(fields))
            duplicate = "&" + "%{:02x}".format(ord(key[0])) + key[1:] + "=1"
            for suffix in (duplicate, "&actor%5fid=99", "&unknown=1", "&"):
                self.assert_error(400, "POST", "/api/inventory/" + path, cookie=self.manager_cookie,
                                  body=urlencode(fields) + suffix, headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.service.requests, [])

    def test_required_fields_bounds_and_subject_types_are_checked_before_transport(self):
        cases = (
            ("people", ({}, {"person_code": "EXT-1"}, {"display_name": "Only name"},
                        {"person_code": "", "display_name": "Name"}, {"person_code": "X", "display_name": ""},
                        {"person_code": "X", "display_name": "Name", "version": "1"},
                        {"person_code": "X" * 65, "display_name": "Name"},
                        {"person_code": "X", "display_name": "Name", "active": "true"})),
            ("people/7", ({"person_code": "X", "display_name": "Name"},
                          {"person_code": "X", "display_name": "Name", "version": "0"})),
            ("identities", ({"realm": "AD"}, {"login": "login"}, {"realm": "", "login": "login"},
                            {"realm": "AD", "login": "login", "kind": "admin"},
                            {"realm": "AD", "login": "login", "person_code": "X"},
                            {"realm": "AD", "login": "login", "notes": "x" * 2001})),
            ("identities/9", ({"realm": "AD", "login": "login"},)),
            ("identities/9/people", ({"person_id": "1"}, {"person_id": "0", "version": "1"},
                                    {"person_id": "1", "version": "1", "role": "administrator"},
                                    {"person_id": "1", "version": "1", "subject_id": "3"})),
            ("assets/5/subjects", ({"subject_kind": "person", "version": "1"},
                                  {"subject_kind": "account", "subject_id": "7", "version": "1"},
                                  {"subject_kind": "identity", "subject_id": "0", "version": "1"},
                                  {"subject_kind": "identity", "subject_id": "7", "version": "1", "role": "shared"})),
            ("identities/9/people/13/remove", ({}, {"version": "1", "person_id": "3"})),
            ("assets/5/subjects/17/remove", ({}, {"version": "1", "subject_id": "3"})),
        )
        for path, invalid_forms in cases:
            for fields in invalid_forms:
                self.assert_error(400, "POST", "/api/inventory/" + path, fields, cookie=self.manager_cookie)
        for path, invalid_queries in (
            ("people", ("q=" + "x" * 201, "active=2", "kind=shared", "person_id=1", "limit=0", "after_id=0")),
            ("identities", ("kind=admin", "person_id=0", "person_id=1&person%5fid=2", "active=true", "identity_id=1")),
            ("people/7", ("limit=1", "q=x")), ("identities/9", ("active=1", "limit=1")),
        ):
            for query in invalid_queries:
                self.assert_error(400, "GET", "/api/inventory/" + path + "?" + query, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_strict_routes_methods_media_and_body_guards_apply_to_subjects(self):
        for path in ("people/0", "people/-1", "people/9223372036854775808", "people/7/other", "people/7/identities/9",
                     "identities/9/people/0/remove", "identities/9/people/13/remove/extra", "identities/9/people/13/other",
                     "identities/9/assets/3", "assets/5/subjects/0/remove", "assets/5/subjects/17/remove/extra"):
            self.assert_error(404, "GET", "/api/inventory/" + path, cookie=self.worker_cookie)
        for path in self.READS:
            route = "/api/inventory/" + path
            self.assert_error(405, "DELETE", route, cookie=self.manager_cookie)
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
            try:
                connection.request("HEAD", route, headers={"Cookie": self.worker_cookie})
                response = connection.getresponse()
                self.assertEqual(response.status, 405)
                self.assertEqual(response.read(), b"")
            finally:
                connection.close()
        for path in ("people/7/history", "people/7/identities", "people/7/assets", "identities/9/history", "identities/9/assets"):
            self.assert_error(405, "POST", "/api/inventory/" + path, {}, cookie=self.manager_cookie)
        for path, fields in self.mutations():
            self.assert_error(415, "POST", "/api/inventory/" + path, cookie=self.manager_cookie,
                              body="{}", headers={"Content-Type": "application/json"})
            self.assert_error(400, "POST", "/api/inventory/" + path + "?active=1", fields, cookie=self.manager_cookie)
        for path in self.READS:
            self.assert_error(400, "GET", "/api/inventory/" + path, cookie=self.worker_cookie, body="unexpected")
        for path in ("identities/9/people/13/remove", "assets/5/subjects/17/remove"):
            self.assert_error(405, "GET", "/api/inventory/" + path, cookie=self.worker_cookie)
        self.assertEqual(self.service.requests, [])

    def test_new_directories_preserve_public_errors_and_sanitize_credentials(self):
        for status in (400, 404, 409, 422, 429, 503):
            self.service.reply_status = status
            self.service.reply_value = {"ok": False, "error": "Anagrafica modificata"}
            self.assert_error(status, "GET", "/api/inventory/identities/9", cookie=self.worker_cookie)
        for response in (b"not-json", {"ok": True, "identity": {"password": "private"}},
                         {"ok": True, "identity": {"access_token": "private"}}):
            self.service.reply_status = 200
            self.service.reply_value = response
            status, value, _ = self.api("GET", "/api/inventory/identities/9", cookie=self.worker_cookie)
            self.assertEqual(status, 503)
            self.assertNotIn("private", str(value))


if __name__ == "__main__":
    unittest.main()
