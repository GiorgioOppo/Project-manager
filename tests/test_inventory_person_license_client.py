"""Structured recipients retain session guards and independent ID namespaces."""

import itertools
import unittest
from urllib.parse import urlencode

import test_inventory_client as bridge


class InventoryPersonLicenseBridgeTests(unittest.TestCase):
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

    def mutations(self, person_id="987654"):
        return (
            ("context", {"version": "4", "assignee_person_id": person_id, "site_id": "2"}),
            ("checkout", {"version": "4", "person_id": person_id, "checkout_on": "2026-10-10",
                          "condition_out": "good", "notes": "Consegna & qualità"}),
            ("relations", {"version": "4", "relation_type": "licensed_to", "person_id": person_id,
                           "quantity": "2", "notes": "Licenza nominativa"}),
            ("relations", {"version": "4", "relation_type": "licensed_to", "identity_id": person_id,
                           "quantity": "3", "notes": "Utenza condivisa"}),
        )

    def test_person_and_identity_recipients_never_resolve_as_application_users(self):
        self.assertEqual(self.edit_user(self.worker_id, active="0")[0], 200)
        for subject_id in (str(self.worker_id), "987654"):
            for action, fields in self.mutations(subject_id):
                status, value, _ = self.api("POST", "/api/inventory/assets/7/" + action,
                                            fields, cookie=self.manager_cookie)
                self.assertEqual(status, 200, value)
                request = self.service.requests[-1]
                self.assertEqual(request["path"], "/v1/assets/7/" + action)
                self.assertEqual(request["fields"], {key: [value] for key, value in
                                 dict(fields, actor_id=str(self.manager_id)).items()})

    def test_structured_recipients_require_manager_origin_and_trusted_actor(self):
        for action, fields in self.mutations():
            route = "/api/inventory/assets/7/" + action
            self.assert_error(401, "POST", route, fields)
            self.assert_error(403, "POST", route, fields, cookie=self.worker_cookie)
            self.assert_error(403, "POST", route, fields, cookie=self.manager_cookie,
                              headers={"Origin": "https://hostile.invalid"})
            self.assert_error(415, "POST", route, cookie=self.manager_cookie, body="{}",
                              headers={"Content-Type": "application/json"})
            self.assert_error(400, "POST", route + "?version=4", fields, cookie=self.manager_cookie)
        self.assertEqual(self.service.requests, [])

    def test_conflicting_recipients_are_rejected_before_lookup_or_transport(self):
        self.assertEqual(self.edit_user(self.worker_id, active="0")[0], 200)
        for action, fields in (
            ("context", {"version": "4", "assignee_person_id": "7", "assignee_user_id": str(self.worker_id)}),
            ("checkout", {"version": "4", "person_id": "7", "user_id": str(self.worker_id)}),
            ("checkout", {"version": "4", "checkout_on": "2026-10-10", "condition_out": "good"}),
        ):
            self.assert_error(400, "POST", "/api/inventory/assets/7/" + action,
                              fields, cookie=self.manager_cookie)
        alternatives = {"person_id": "7", "identity_id": "9", "target_id": "11", "assignee": "Legacy"}
        for first, second in itertools.combinations(alternatives, 2):
            fields = {"version": "4", "relation_type": "licensed_to",
                      first: alternatives[first], second: alternatives[second]}
            self.assert_error(400, "POST", "/api/inventory/assets/7/relations",
                              fields, cookie=self.manager_cookie)
        for relation_type in ("installed_on", "connected_to"):
            for key in ("person_id", "identity_id"):
                self.assert_error(400, "POST", "/api/inventory/assets/7/relations",
                                  {"version": "4", "relation_type": relation_type, key: "7"},
                                  cookie=self.manager_cookie)
        self.assertEqual(self.service.requests, [])

    def test_recipient_snapshots_duplicate_fields_and_invalid_ids_are_rejected(self):
        for action, fields in self.mutations():
            route = "/api/inventory/assets/7/" + action
            for key in ("actor_id", "person_name", "assignee_person_name", "user_name", "assignee_user_name",
                        "subject_snapshot", "subject_kind", "subject_id", "password", "access_token"):
                self.assert_error(400, "POST", route, dict(fields, **{key: "Forged"}), cookie=self.manager_cookie)
            recipient = next(key for key in fields if key in ("assignee_person_id", "person_id", "identity_id"))
            for invalid in ("0", "-1", "1.5", "9223372036854775808"):
                self.assert_error(400, "POST", route, dict(fields, **{recipient: invalid}), cookie=self.manager_cookie)
            duplicate = "&%{:02x}{}=8".format(ord(recipient[0]), recipient[1:])
            self.assert_error(400, "POST", route, cookie=self.manager_cookie,
                              body=urlencode(fields) + duplicate,
                              headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(self.service.requests, [])

    def test_legacy_application_users_and_text_licenses_remain_supported(self):
        for action, fields, name_key in (
            ("context", {"version": "4", "assignee_user_id": str(self.worker_id)}, "assignee_user_name"),
            ("checkout", {"version": "4", "user_id": str(self.worker_id), "checkout_on": "2026-10-10",
                          "condition_out": "good"}, "user_name"),
        ):
            status, value, _ = self.api("POST", "/api/inventory/assets/7/" + action, fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            forwarded = self.service.requests[-1]["fields"]
            self.assertTrue(forwarded[name_key][0])
            self.assertNotIn("person_id", forwarded)
            self.assertNotIn("assignee_person_id", forwarded)
        for fields in (
            {"version": "4", "relation_type": "licensed_to", "assignee": "Persona & legacy", "quantity": "2"},
            {"version": "4", "relation_type": "licensed_to", "target_id": "11", "quantity": "2"},
            {"version": "4", "relation_type": "installed_on", "target_id": "11"},
        ):
            status, value, _ = self.api("POST", "/api/inventory/assets/7/relations", fields, cookie=self.manager_cookie)
            self.assertEqual(status, 200, value)
            self.assertEqual(self.service.requests[-1]["fields"], {key: [value] for key, value in
                             dict(fields, actor_id=str(self.manager_id)).items()})


if __name__ == "__main__":
    unittest.main()
