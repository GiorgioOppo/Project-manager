"""Live space/area governance, sensitive-profile approval, and impact previews."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from datetime import datetime, timedelta
import json
import sqlite3
import unittest
from zoneinfo import ZoneInfo

import test_roles as roles

BASE = "/api/improvements/governance/"


class GovernanceIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user
    assert_error = roles.RoleIntegrationTests.assert_error
    create_time = roles.RoleIntegrationTests.create_time
    time_fields = roles.RoleIntegrationTests.time_fields
    assign_job = roles.RoleIntegrationTests.assign_job

    def get(self, path, cookie=None):
        code, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(code, 200, data)
        return data

    def post(self, path, fields, cookie=None, expected=201):
        code, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(code, expected, data)
        self.assertTrue(data.get("ok"), data)
        return data

    def db(self, sql, values=()):
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("PRAGMA foreign_keys=ON")
            rows = db.execute(sql, values).fetchall()
            db.commit()
            return rows

    def today(self, offset=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=offset)).isoformat()

    def manager(self, email="area@example.test"):
        user = self.create_user(email=email, name="Gestore area", role="manager")
        return user, self.login(email, roles.WORKER_PASSWORD)

    def worker(self, email="person@example.test"):
        user = self.create_user(email=email, name="Persona")
        return user, self.login(email, roles.WORKER_PASSWORD)

    def group(self, name="Area", parent=""):
        return self.post("/api/groups", {"name": name, "parent_id": parent})["id"]

    def member(self, group, user):
        return self.post("/api/groups/%d/members" % group, {"user_id": user})

    def job(self, code="GOV-1"):
        return self.post("/api/jobs", {"code": code, "name": code, "status": "active"})["id"]

    def grant(self, job, group, role=2):
        return self.post("/api/jobs/%d/group-grants" % job,
                         {"group_id": group, "role_id": role, "starts_on": "", "ends_on": ""})

    def delegate(self, user, group, **changes):
        fields = {"user_id": user, "group_id": group, "starts_on": self.today(),
                  "ends_on": "", "reason": "Responsabilità di area verificata", "can_manage_jobs": 1,
                  "can_manage_groups": 1, "can_manage_users": 1, "can_approve_corrections": 1}
        fields.update(changes)
        return self.post(BASE + "delegations", fields)

    def make_admin(self, user):
        return self.post(BASE + "admins", {"user_id": user, "admin": 1, "reason": "Secondo amministratore verificato"}, expected=200)

    def profiles(self, user, groups, cookie=None, expected=200, revision=None):
        fields = {"groups": json.dumps(groups), "reason": "Necessità funzionale verificata"}
        if revision is not None:
            fields["revision"] = revision
        return self.post("/api/users/%d/access-groups" % user, fields, cookie, expected)

    def decide(self, request, cookie, action="approve", expected=200):
        return self.post(BASE + "profile-requests/%d/decision" % request,
                         {"action": action, "reason": "Verifica indipendente completata"}, cookie, expected)

    def test_bootstrap_admin_is_explicit_new_managers_are_area_only(self):
        session = self.get("/api/session")
        self.assertTrue(session["permissions"]["manage_space"])
        self.assertFalse(session["permissions"]["view_costs"])
        self.assertFalse(session["permissions"]["view_contracts"])
        user, cookie = self.manager()
        session = self.get("/api/session", cookie)
        self.assertFalse(session["permissions"]["manage_space"])
        self.assertFalse(session["permissions"]["manage_access_groups"])
        self.assertEqual(self.get("/api/users/%d/access-groups" % user)["selected"], [])
        self.assert_error(403, "POST", BASE + "admins", {"user_id": user, "admin": 1, "reason": "Self"}, cookie=cookie)
        self.assert_error(403, "POST", "/api/users", {"name": "Unbound", "email": "unbound@example.test", "password": roles.WORKER_PASSWORD, "role": "worker"}, cookie=cookie)
        self.assert_error(403, "POST", "/api/users/%d/access-groups" % user, {"groups": '["finance"]'}, cookie=cookie)
        self.assertEqual(self.get("/api/users", cookie)["users"][0]["id"], user)
        self.assertEqual(self.get("/api/groups", cookie)["groups"], [])
        person, _ = self.worker()
        self.assert_error(400, "POST", BASE + "admins", {"user_id": person, "admin": 1, "reason": "Role required"}, cookie=self.manager_cookie)

    def test_single_admin_profile_exception_is_explicit_audited_and_atomic(self):
        user, _ = self.worker()
        changed = self.profiles(user, ["finance"])
        self.assertTrue(changed["bootstrap"])
        self.assertFalse(changed["pending"])
        self.assertEqual(changed["revision"], 2)
        rows = self.get(BASE + "profile-requests")["requests"]
        self.assertEqual((rows[0]["status"], rows[0]["bootstrap"]), ("approved", 1))
        self.assertIn("bootstrap", rows[0]["decision_reason"])
        self.db("CREATE TRIGGER reject_governance BEFORE INSERT ON audit_events WHEN NEW.action='governance.profiles.bootstrap' BEGIN SELECT RAISE(ABORT,'fixture'); END")
        code, data, _ = self.api("POST", "/api/users/%d/access-groups" % user, {"groups": '["sales"]', "reason": "Atomic test"}, cookie=self.manager_cookie)
        self.assertEqual(code, 500, data)
        profile = self.get("/api/users/%d/access-groups" % user)
        self.assertEqual((profile["selected"], profile["revision"]), (["finance"], 2))
        self.assertEqual(len(self.get(BASE + "profile-requests")["requests"]), 1)

    def test_two_admins_require_distinct_approval_even_for_self(self):
        second, cookie = self.manager()
        self.make_admin(second)
        target = self.get("/api/session")["user_id"]
        changed = self.profiles(target, ["sales", "finance"], expected=202, revision=1)
        request = changed["request_id"]
        self.assertTrue(changed["pending"])
        self.assertEqual(changed["selected"], [])
        self.assert_error(403, "POST", BASE + "profile-requests/%d/decision" % request,
                          {"action": "approve", "reason": "Self approval"}, cookie=self.manager_cookie)
        self.assert_error(409, "POST", "/api/users/%d/access-groups" % target, {"groups": "[]"}, cookie=self.manager_cookie)
        approved = self.decide(request, cookie)
        self.assertEqual((approved["selected"], approved["revision"]), (["sales", "finance"], 2))
        self.assert_error(409, "POST", BASE + "profile-requests/%d/decision" % request,
                          {"action": "approve", "reason": "Replay"}, cookie=cookie)
        self.assert_error(409, "POST", "/api/users/%d/access-groups" % target,
                          {"groups": "[]", "revision": 1}, cookie=self.manager_cookie)

    def test_cancel_reject_conflict_and_concurrent_decisions_keep_one_history(self):
        second, cookie = self.manager()
        self.make_admin(second)
        user, _ = self.worker()
        request = self.profiles(user, ["sales"], expected=202)["request_id"]
        self.decide(request, self.manager_cookie, "cancel")
        request = self.profiles(user, ["sales"], expected=202)["request_id"]
        self.decide(request, cookie, "reject")
        self.assertEqual(self.get("/api/users/%d/access-groups" % user)["selected"], [])
        request = self.profiles(user, ["finance"], expected=202)["request_id"]
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.api("POST", BASE + "profile-requests/%d/decision" % request,
                               {"action": "approve", "reason": "Concurrent review"}, cookie=cookie), range(2)))
        self.assertEqual(sorted(result[0] for result in results), [200, 409])
        self.assertEqual(self.get("/api/users/%d/access-groups" % user)["revision"], 2)
        request = self.profiles(user, [], expected=202)["request_id"]
        self.post(BASE + "admins", {"user_id": self.get("/api/session")["user_id"], "admin": 0, "reason": "Revoked proposer"}, cookie, 200)
        self.assert_error(409, "POST", BASE + "profile-requests/%d/decision" % request,
                          {"action": "approve", "reason": "Recheck proposer"}, cookie=cookie)
        statuses = self.db("SELECT status,pending_user_id FROM access_profile_requests ORDER BY id")
        self.assertEqual(statuses, [("cancelled", None), ("rejected", None), ("approved", None), ("conflict", None)])

    def test_profile_json_and_decision_validation_prevent_hidden_codes(self):
        user, _ = self.worker()
        for payload in ('["sales"]true', '["sales\\u0000"]', '["finance",]', '["sales","sales"]', '["operator"]', '{}'):
            self.assert_error(400, "POST", "/api/users/%d/access-groups" % user, {"groups": payload}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", BASE + "profile-requests", {"user_id": user, "groups": "[]", "reason": " "}, cookie=self.manager_cookie)
        self.assertEqual(self.get("/api/users/%d/access-groups" % user)["selected"], [])
        self.assertEqual(self.db("SELECT COUNT(*) FROM access_profile_requests"), [(0,)])

    def test_delegated_area_includes_descendants_and_rechecks_live_revocation(self):
        user, cookie = self.manager()
        root = self.group("Parent outside delegated area")
        area = self.group("Delegated area", root)
        child = self.group("Subgroup", area)
        outside = self.group("Outside")
        person, _ = self.worker()
        other, _ = self.worker("other@example.test")
        self.member(child, person)
        self.member(outside, other)
        job = self.job()
        self.grant(job, child)
        delegation = self.delegate(user, area)
        visible = self.get("/api/groups", cookie)["groups"]
        self.assertEqual({g["id"] for g in visible}, {area, child})
        users = self.get("/api/users", cookie)["users"]
        self.assertEqual({u["id"] for u in users}, {user, person})
        self.post("/api/groups/%d" % area, {"name": "Renamed area", "parent_id": root}, cookie, 200)
        self.post("/api/users/%d" % person, {"name": "Updated by area", "role": "worker", "active": 1}, cookie, 200)
        self.assert_error(403, "POST", "/api/users/%d" % other, {"name": "Forbidden", "role": "worker", "active": 1}, cookie=cookie)
        self.assert_error(403, "POST", "/api/users/%d" % person, {"name": "Persona", "role": "manager", "active": 1}, cookie=cookie)
        self.assert_error(403, "POST", "/api/groups/%d" % area, {"parent_id": outside}, cookie=cookie)
        self.post(BASE + "delegations/%d/revoke" % delegation["id"], {"revision": 1, "reason": "End of responsibility"}, expected=200)
        self.assert_error(403, "POST", "/api/users/%d" % person, {"name": "After revocation", "role": "worker", "active": 1}, cookie=cookie)
        self.assert_error(409, "POST", BASE + "delegations/%d/revoke" % delegation["id"], {"revision": 1, "reason": "Stale"}, cookie=self.manager_cookie)
        self.assertEqual(self.get(BASE + "delegations", cookie)["delegations"][0]["active"], 0)

    def test_delegation_dates_and_capabilities_do_not_grant_other_powers(self):
        user, cookie = self.manager()
        group = self.group()
        worker, _ = self.worker()
        self.member(group, worker)
        for dates in ({"starts_on": self.today(1)}, {"starts_on": self.today(-2), "ends_on": self.today(-1)}):
            self.delegate(user, group, **dates)
        self.assert_error(403, "POST", "/api/users/%d" % worker, {"name": "Outside dates", "role": "worker", "active": 1}, cookie=cookie)
        self.delegate(user, group, can_manage_users=0, can_manage_groups=0, can_manage_jobs=0)
        self.assert_error(403, "POST", "/api/groups/%d" % group, {"name": "Only approval", "role": "worker", "active": 1}, cookie=cookie)
        self.assert_error(403, "POST", "/api/users/%d" % worker, {"name": "Only approval", "role": "worker", "active": 1}, cookie=cookie)
        self.assertFalse(self.get("/api/session", cookie)["permissions"]["view_contracts"])

    def test_correction_delegation_requires_live_team_and_blocks_self_review(self):
        reviewer, cookie = self.manager()
        person, worker_cookie = self.worker()
        group = self.group()
        self.member(group, person)
        job = self.job()
        self.assign_job(job, person)
        entry = self.create_time(worker_cookie, job, date=self.today())
        request = self.post("/api/time-corrections", {"entry_id": entry, "revision": 1, "job_id": job,
                    "date": self.today(), "minutes": 45, "note": "Corrected", "reason": "Verified", "delete_requested": 0}, worker_cookie)["id"]
        self.delegate(reviewer, group, can_manage_jobs=0, can_manage_groups=0, can_manage_users=0)
        self.assertEqual(self.get("/api/time-corrections", cookie)["requests"], [])
        self.member(group, reviewer)
        queue = self.get("/api/time-corrections", cookie)["requests"]
        self.assertTrue(queue[0]["can_decide"])
        code, data, _ = self.api("DELETE", "/api/groups/%d/members/%d" % (group, reviewer), cookie=self.manager_cookie)
        self.assertEqual(code, 200, data)
        self.assert_error(404, "POST", "/api/time-corrections/%d/decision" % request,
                          {"decision": "approved", "reason": "Membership removed"}, cookie=cookie)
        self.member(group, reviewer)
        self.post("/api/time-corrections/%d/decision" % request, {"decision": "approved", "reason": "Reviewed"}, cookie, 200)
        self.assertEqual(self.db("SELECT minutes FROM time_entries WHERE id=?", (entry,)), [(45,)])
        # Space administration is still not a substitute for team membership.
        self.assertEqual(self.get("/api/time-team")["users"][0]["id"], self.get("/api/session")["user_id"])
        self.assertNotIn(person, {u["id"] for u in self.get("/api/time-team")["users"]})

    def test_group_preview_reports_permission_and_time_scope_changes_without_writes(self):
        reader = self.group("Reader area")
        writer = self.group("Writer area")
        child = self.group("Child", reader)
        user, cookie = self.worker()
        old_manager, _ = self.manager("old-manager@example.test")
        new_manager, _ = self.manager("new-manager@example.test")
        self.member(reader, old_manager)
        self.member(writer, new_manager)
        self.member(child, user)
        job = self.job()
        self.grant(job, reader, 1)
        self.grant(job, writer, 2)
        audits = self.db("SELECT COUNT(*) FROM audit_events")[0][0]
        preview = self.post(BASE + "group-impact", {"group_id": child, "operation": "move", "parent_id": writer}, expected=200)
        self.assertFalse(preview["persisted"])
        changes = {row["user_id"]: row for row in preview["changes"]}
        self.assertEqual(changes[user]["job_access_added"], [])
        self.assertEqual(changes[user]["job_permission_changes"], [{"job_id": job, "before_can_log_time": False, "after_can_log_time": True}])
        self.assertIn(user, changes[old_manager]["hour_users_removed"])
        self.assertIn(user, changes[new_manager]["hour_users_added"])
        self.assertEqual(self.db("SELECT parent_id FROM organization_groups WHERE id=?", (child,)), [(reader,)])
        self.assertEqual(self.db("SELECT COUNT(*) FROM audit_events")[0][0], audits)
        self.assert_error(400, "POST", BASE + "group-impact", {"group_id": reader, "operation": "move", "parent_id": child}, cookie=self.manager_cookie)
        self.assert_error(403, "POST", BASE + "group-impact", {"group_id": child, "operation": "remove_member", "user_id": user}, cookie=cookie)
        preview = self.post(BASE + "group-impact", {"group_id": child, "operation": "remove_member", "user_id": user}, expected=200)
        self.assertIn(job, next(row for row in preview["changes"] if row["user_id"] == user)["job_access_removed"])
        self.assertEqual(self.db("SELECT COUNT(*) FROM group_members WHERE group_id=? AND user_id=?", (child, user)), [(1,)])

    def test_last_admin_and_demotion_guards_preserve_a_live_administrator(self):
        own = self.get("/api/session")["user_id"]
        self.assert_error(409, "POST", BASE + "admins", {"user_id": own, "admin": 0, "reason": "Last admin"}, cookie=self.manager_cookie)
        second, cookie = self.manager()
        self.assert_error(409, "POST", "/api/users/%d" % own, {"name": "Gestore", "role": "worker", "active": 1}, cookie=self.manager_cookie)
        self.make_admin(second)
        self.post("/api/users/%d" % own, {"name": "Gestore", "role": "worker", "active": 1}, cookie, 200)
        self.assertEqual(self.db("SELECT COUNT(*) FROM access_space_admins WHERE user_id=?", (own,)), [(0,)])
        self.assert_error(409, "POST", "/api/users/%d" % second, {"name": "Secondo", "role": "manager", "active": 0}, cookie=cookie)
        self.post("/api/users/%d" % own, {"name": "Gestore", "role": "manager", "active": 1}, cookie, 200)
        self.assertFalse(self.get("/api/session")["permissions"]["manage_space"])

    def test_schema14_migration_preserves_previous_managers_once_without_departments(self):
        old_manager, _ = self.manager()
        worker, _ = self.worker()
        self.profiles(worker, ["finance"])
        job = self.job()
        entry = self.create_time(self.manager_cookie, job, minutes=37)
        self.stop_server()
        self.db("DELETE FROM access_governance_state")
        self.db("DELETE FROM access_space_admins")
        self.db("PRAGMA user_version=13")
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.db("PRAGMA user_version"), [(14,)])
        self.assertEqual(self.db("SELECT user_id FROM access_space_admins ORDER BY user_id"), [(1,), (old_manager,)])
        self.assertEqual(self.db("SELECT id,minutes FROM time_entries"), [(entry, 37)])
        self.assertEqual(self.get("/api/users/%d/access-groups" % worker)["selected"], ["finance"])
        self.assertEqual(self.get("/api/users/%d/access-groups" % old_manager)["selected"], [])
        new_manager, cookie = self.manager("after-migration@example.test")
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        cookie = self.login("after-migration@example.test", roles.WORKER_PASSWORD)
        self.assertFalse(self.get("/api/session", cookie)["permissions"]["manage_space"])
        self.assertEqual(self.db("SELECT COUNT(*) FROM access_space_admins WHERE user_id=?", (new_manager,)), [(0,)])
