"""Explicit Ticket calendar delegation over authenticated, verified private TLS."""

import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlencode, urlsplit

import test_calendar_client as calendar
import test_calendar_addon as addon
import test_roles as roles


PREFIX = "/api/integrations/tickets/"
EVENT = {"summary": "Intervento caffè", "description": "Ticket #42, commessa canonica",
         "start": {"dateTime": "2030-01-02T09:00:00+01:00", "timeZone": "Europe/Rome"},
         "end": {"dateTime": "2030-01-02T10:00:00+01:00", "timeZone": "Europe/Rome"}}


class TicketCalendarDelegationTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    assign_job = roles.RoleIntegrationTests.assign_job
    edit_user = roles.RoleIntegrationTests.edit_user
    assert_error = roles.RoleIntegrationTests.assert_error

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make"], cwd=roles.ROOT, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        calendar.CalendarBridgeTests.setUpClass.__func__(cls)

    def setUp(self):
        self.ticket_origin = "http://127.0.0.1:19837"
        with patch.dict(os.environ, {"TICKETS_APP_ORIGIN": self.ticket_origin, "AUTH_MODE": "local"}):
            calendar.CalendarBridgeTests.setUp(self)

    def grant(self, cookie=None, *, calendar_access=False):
        fields = {"state": "a" * 64}
        if calendar_access:
            fields["calendar"] = "1"
        status, body, _headers = self.api("POST", PREFIX + "grant", fields, cookie=cookie or self.worker_cookie)
        self.assertEqual(status, 200, body)
        self.assertTrue(body["url"].startswith(self.ticket_origin + "/#punto_grant="))
        fragment = parse_qs(urlsplit(body["url"]).fragment)
        self.assertEqual(fragment["state"], ["a" * 64])
        return fragment["punto_grant"][0]

    def delegated(self, method, operation, grant, fields=None, expected=200, **kwargs):
        headers = {"Authorization": "Bearer " + grant, **kwargs.pop("headers", {})}
        status, body, _headers = self.api(method, PREFIX + "calendar/" + operation, fields,
                                        headers=headers, **kwargs)
        self.assertEqual(status, expected, body)
        return body

    def job(self, code):
        status, body, _headers = self.api("POST", "/api/jobs", {"code": code, "name": "Commessa " + code,
                                                               "status": "active"}, cookie=self.manager_cookie)
        self.assertEqual(status, 201, body)
        return body["id"]

    def test_catalog_grants_remain_readonly_and_calendar_requires_explicit_capability(self):
        readonly = self.grant()
        status, scope, _headers = self.api("GET", PREFIX + "scope", headers={"Authorization": "Bearer " + readonly})
        self.assertEqual(status, 200, scope)
        self.assertIs(scope["calendar_access"], False)
        for operation in ("status", "events", "changes", "event?event_id=remote-event", "change?idempotency_key=ticket-key"):
            self.delegated("GET", operation, readonly, expected=403)
        self.delegated("POST", "create", readonly,
                       {"idempotency_key": "ticket-create-42", "event": json.dumps(EVENT)}, expected=403)
        self.assertEqual(self.service.requests, [])
        writable = self.grant(calendar_access=True)
        status, scope, _headers = self.api("GET", PREFIX + "scope", headers={"Authorization": "Bearer " + writable})
        self.assertEqual(status, 200, scope)
        self.assertIs(scope["calendar_access"], True)
        self.assertEqual(scope["user"]["id"], self.worker_id)
        self.delegated("GET", "status", writable)
        self.delegated("GET", "status", readonly, expected=403)

    def test_delegated_exact_lookups_use_grant_actor_and_forbid_identity_injection(self):
        grant = self.grant(calendar_access=True)
        for operation, query, value in (
                ("event", {"event_id": "remote-event"}, {"event": {"id": "remote-event", "etag": '"v3"'}, "calendar_id": "primary@example.test"}),
                ("change", {"idempotency_key": "ticket:42:plan"}, {"id": 7, "event_id": "remote-event", "state": "applied",
                 "idempotency_key": "ticket:42:plan", "error": "", "attempts": 1, "calendar_id": "primary@example.test"})):
            self.service.reply_value = value
            self.assertEqual(self.delegated("GET", operation + "?" + urlencode(query), grant), value)
            sent = self.service.requests[-1]
            self.assertEqual(urlsplit(sent["path"]).path, f"/v1/users/{self.worker_id}/{operation}")
            self.assertEqual(parse_qs(urlsplit(sent["path"]).query), {key: [item] for key, item in query.items()})
            self.assertEqual(sent["authorization"], "Bearer " + calendar.TOKEN)
        count = len(self.service.requests)
        for operation in ("status?user_id=1", "events?user_id=1", "event?event_id=remote-event&user_id=1",
                          "change?idempotency_key=ticket-key&user_id=1", "change?idempotency_key=ticket-key&idempotency_key=other-key"):
            self.delegated("GET", operation, grant, expected=400)
        self.assertEqual(len(self.service.requests), count)

    def test_calendar_writes_check_live_job_acl_and_strip_authorization_context(self):
        allowed, denied = self.job("ALLOWED"), self.job("DENIED")
        self.assign_job(allowed, self.worker_id)
        grant = self.grant(calendar_access=True)
        fields = {"idempotency_key": "ticket:42:create", "event": json.dumps(EVENT),
                  "expected_connection_id": "f" * 64, "expected_calendar_id": "primary@example.test"}
        self.service.reply_status = 202
        self.service.reply_value = {"id": 7, "event_id": "remote-event", "state": "queued", "calendar_id": "primary@example.test"}
        for job, status in ((denied, 403), (999999, 404), (0, 400)):
            self.delegated("POST", "create", grant, {**fields, "job_id": job}, expected=status)
        self.assertEqual(self.service.requests, [])
        queued = self.delegated("POST", "events/create", grant, {**fields, "job_id": allowed}, expected=202)
        self.assertEqual(queued["event_id"], "remote-event")
        self.assertEqual(self.service.requests[-1]["path"], f"/v1/users/{self.worker_id}/events/create")
        self.assertEqual(self.service.requests[-1]["fields"], {key: [value] for key, value in fields.items()})
        self.api("DELETE", f"/api/jobs/{allowed}/assignments/{self.worker_id}", cookie=self.manager_cookie)
        count = len(self.service.requests)
        self.delegated("POST", "update", grant, {**fields, "event_id": "remote-event", "etag": '"v3"', "job_id": allowed}, expected=403)
        self.delegated("POST", "create", grant, {**fields, "user_id": self.manager_id}, expected=400)
        self.assertEqual(len(self.service.requests), count)
        self.delegated("POST", "create", grant, fields, expected=202)

    def test_capability_parameters_methods_and_session_revocation_fail_closed(self):
        for body in ("state=" + "a" * 64 + "&calendar=0", "state=" + "a" * 64 + "&calendar=true",
                     "state=" + "a" * 64 + "&calendar=1&%63alendar=1", "state=" + "a" * 64 + "&calendar=1&",
                     "state=" + "a" * 64 + "&calendar=1&user_id=1"):
            self.assert_error(400, "POST", PREFIX + "grant", cookie=self.worker_cookie, body=body,
                              headers={"Content-Type": "application/x-www-form-urlencoded"})
        grant = self.grant(calendar_access=True)
        self.delegated("GET", "create", grant, expected=405)
        self.delegated("POST", "status", grant, {}, expected=405)
        self.delegated("POST", "oauth/start", grant, {}, expected=404)
        self.delegated("POST", "selection", grant, {"calendar_id": "other"}, expected=404)
        self.delegated("POST", "create", grant, body="{}", headers={"Content-Type": "application/json"}, expected=415)
        self.assertEqual(self.service.requests, [])
        self.api("POST", "/api/logout", {}, cookie=self.worker_cookie)
        self.delegated("GET", "status", grant, expected=401)
        self.delegated("POST", "create", grant, {"idempotency_key": "ticket:42:create", "event": json.dumps(EVENT)}, expected=401)
        self.assertEqual(self.service.requests, [])


class TicketCalendarProviderRoundtripTests(unittest.TestCase):
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    assign_job = roles.RoleIntegrationTests.assign_job
    grant = TicketCalendarDelegationTests.grant
    delegated = TicketCalendarDelegationTests.delegated
    job = TicketCalendarDelegationTests.job

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make"], cwd=roles.ROOT, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        addon.CalendarAddonTests.setUpClass()
        cls.addClassCleanup(addon.CalendarAddonTests.doClassCleanups)

    def setUp(self):
        self.provider = addon.CalendarAddonTests(methodName="runTest")
        self.provider.setUp()
        self.addCleanup(self.provider.doCleanups)
        self.provider.start()
        self.ticket_origin = "http://127.0.0.1:19837"
        self.main_environment = {"TICKETS_APP_ORIGIN": self.ticket_origin, "AUTH_MODE": "local",
                                 "CALENDAR_API_URL": f"https://127.0.0.1:{self.provider.port}",
                                 "CALENDAR_API_CA_FILE": str(self.provider.cert),
                                 "CALENDAR_API_TOKEN_FILE": str(self.provider.token_file)}
        with patch.dict(os.environ, self.main_environment):
            roles.RoleIntegrationTests.setUp(self)
        self.worker_id = self.create_user()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.provider.authorize(uid=self.worker_id, code="delegated-worker")
        self.provider.select(uid=self.worker_id)

    def test_real_queue_provider_lookup_and_replay_preserve_one_event_across_restarts(self):
        job = self.job("SCHEDULED")
        self.assign_job(job, self.worker_id)
        grant = self.grant(calendar_access=True)
        current = self.delegated("GET", "status", grant)
        self.assertRegex(current["connection_id"], r"\A[0-9a-f]{64}\Z")
        fields = {"job_id": str(job), "idempotency_key": "ticket:42:calendar-create", "event": json.dumps(EVENT),
                  "expected_connection_id": current["connection_id"], "expected_calendar_id": current["calendar_id"]}
        queued = self.delegated("POST", "create", grant, fields, expected=202)
        self.assertEqual(queued["calendar_id"], "primary@example.test")
        self.assertRegex(queued["event_id"], r"\A[0-9a-f]{64}\Z")
        self.provider.synchronize(uid=self.worker_id)
        query = "change?" + urlencode({"idempotency_key": fields["idempotency_key"]})
        applied = self.delegated("GET", query, grant)
        self.assertEqual((applied["id"], applied["event_id"], applied["state"]),
                         (queued["id"], queued["event_id"], "applied"))
        remote = self.delegated("GET", "event?event_id=" + queued["event_id"], grant)
        self.assertEqual(remote["event"]["etag"], '"etag-written"')
        self.assertEqual(remote["event"]["start"], EVENT["start"])
        self.assertNotIn("extendedProperties", remote["event"])
        self.stop_server()
        self.provider.stop()
        self.provider.start()
        self.main_environment["CALENDAR_API_URL"] = f"https://127.0.0.1:{self.provider.port}"
        with patch.dict(os.environ, self.main_environment):
            self.start_server()
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.delegated("GET", query, grant, expected=401)
        replacement = self.grant(calendar_access=True)
        self.assertEqual(self.delegated("GET", "status", replacement)["connection_id"], current["connection_id"])
        self.assertEqual(self.delegated("GET", query, replacement)["event_id"], queued["event_id"])
        replay = self.delegated("POST", "events/create", replacement, fields)
        self.assertIs(replay["replayed"], True)
        self.assertEqual(replay["event_id"], queued["event_id"])
        self.provider.synchronize(uid=self.worker_id)
        creates = [item for item in self.provider.google.mutations if item["method"] == "POST"]
        self.assertEqual(len(creates), 1)
        self.assertEqual(json.loads(creates[0]["body"])["id"], queued["event_id"])
        self.assertEqual(creates[0]["query"].get("sendUpdates"), ["none"])
        self.assertTrue(self.provider.proxy.requests)


if __name__ == "__main__":
    unittest.main()
