"""Hierarchical groups and least-privilege job roles, through real HTTP requests."""

from contextlib import closing
from datetime import datetime, timedelta
import json
import sqlite3
import unittest
from zoneinfo import ZoneInfo

import test_roles as roles


class GroupIntegrationTests(unittest.TestCase):
    # Share the isolated server fixture, without inheriting the account test cases.
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    day = roles.RoleIntegrationTests.day

    def today(self, offset=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=offset)).isoformat()

    def worker(self, email=roles.WORKER_EMAIL):
        user_id = self.create_user(email=email)
        return user_id, self.login(email, roles.WORKER_PASSWORD)

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def post(self, path, fields, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=self.manager_cookie)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), True)
        return data

    def delete(self, path):
        status, data, _ = self.api("DELETE", path, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)

    def group(self, name, parent_id=""):
        return self.post("/api/groups", {"name": name, "parent_id": parent_id})["id"]

    def member(self, group_id, user_id, expected=201):
        return self.post("/api/groups/%d/members" % group_id,
                         {"user_id": user_id}, expected)

    def job(self, code="GROUP-001"):
        return self.post("/api/jobs", {"code": code, "name": "Commessa " + code,
                                       "status": "active"})["id"]

    def role(self, code="reviewer", can_log_time="0"):
        return self.post("/api/job-roles", {"code": code, "name": "Verificatore",
                                           "can_log_time": can_log_time})["id"]

    def grant(self, job_id, group_id, role_id=2, starts_on="", ends_on="", expected=201):
        return self.post("/api/jobs/%d/group-grants" % job_id, {
            "group_id": group_id, "role_id": role_id,
            "starts_on": starts_on, "ends_on": ends_on}, expected)

    def assign(self, job_id, user_id, role_id=None, expected=201, **dates):
        fields = {"user_id": user_id, **dates}
        if role_id is not None:
            fields["role_id"] = role_id
        return self.post("/api/jobs/%d/assignments" % job_id, fields, expected)

    def jobs(self, cookie):
        return self.get("/api/jobs", cookie)["jobs"]

    def events(self, entity_type=None):
        query = "?limit=100" + ("&entity_type=" + entity_type if entity_type else "")
        return self.get("/api/audit" + query)["events"]

    def test_group_tree_membership_counts_idempotency_and_empty_deletion(self):
        user_id, _ = self.worker()
        parent = self.group('Direzione "Tecnica" ✓')
        child = self.group("Progettazione", parent)
        other = self.group("Ufficio commerciale")
        self.member(child, user_id)
        before_events = self.events()
        self.member(child, user_id, expected=200)
        self.assertEqual(self.events(), before_events)
        self.member(other, user_id)
        groups = {group["id"]: group for group in self.get("/api/groups")["groups"]}
        self.assertEqual(groups[parent], {"id": parent, "name": 'Direzione "Tecnica" ✓',
                                         "parent_id": None, "members_count": 0,
                                         "children_count": 1})
        self.assertEqual(groups[child]["parent_id"], parent)
        self.assertEqual(groups[child]["members_count"], 1)
        members = self.get("/api/groups/%d/members" % child)
        self.assertEqual(members, {"group_id": child, "members": [{
            "user_id": user_id, "name": "Operatore", "email": roles.WORKER_EMAIL,
            "role": "worker", "active": True}]})
        self.assert_error(409, "DELETE", "/api/groups/%d" % parent, cookie=self.manager_cookie)
        self.assert_error(409, "DELETE", "/api/groups/%d" % child, cookie=self.manager_cookie)
        self.delete("/api/groups/%d/members/%d" % (child, user_id))
        self.assert_error(404, "DELETE", "/api/groups/%d/members/%d" % (child, user_id),
                          cookie=self.manager_cookie)
        self.delete("/api/groups/%d" % child)
        self.delete("/api/groups/%d" % parent)
        self.assertEqual([group["id"] for group in self.get("/api/groups")["groups"]], [other])

    def test_cycles_reparenting_and_depth_limit_preserve_valid_tree(self):
        first = self.group("Livello 1")
        chain = [first]
        for level in range(2, 17):
            chain.append(self.group("Livello %d" % level, chain[-1]))
        self.assert_error(400, "POST", "/api/groups", {
            "name": "Troppo profondo", "parent_id": chain[-1]}, cookie=self.manager_cookie)
        for group_id, parent_id in ((first, first), (first, chain[-1]), (chain[5], chain[10])):
            self.assert_error(400, "POST", "/api/groups/%d" % group_id,
                              {"parent_id": parent_id}, cookie=self.manager_cookie)
        independent = self.group("Radice indipendente")
        self.assert_error(400, "POST", "/api/groups/%d" % first,
                          {"parent_id": independent}, cookie=self.manager_cookie)
        before = self.get("/api/groups")["groups"]
        self.post("/api/groups/%d" % chain[1], {"parent_id": ""}, 200)
        groups = {group["id"]: group for group in self.get("/api/groups")["groups"]}
        self.assertIsNone(groups[chain[1]]["parent_id"])
        self.assertEqual(groups[first]["children_count"], 0)
        self.assertEqual(len(groups), len(before))
        self.post("/api/groups/%d" % first, {"parent_id": independent, "name": "Trasferito"}, 200)
        groups = {group["id"]: group for group in self.get("/api/groups")["groups"]}
        self.assertEqual(groups[first]["name"], "Trasferito")
        self.assertEqual(groups[first]["parent_id"], independent)

    def test_group_capacity_rejects_addition_without_truncating_existing_tree(self):
        for index in range(200):
            self.group("Gruppo %03d" % index)
        before = self.get("/api/groups")["groups"]
        self.assertEqual(len(before), 200)
        self.assert_error(409, "POST", "/api/groups", {"name": "Oltre il limite"},
                          cookie=self.manager_cookie)
        self.assertEqual(self.get("/api/groups")["groups"], before)
        self.delete("/api/groups/%d" % before[0]["id"])
        self.group("Posto liberato")
        self.assertEqual(len(self.get("/api/groups")["groups"]), 200)

    def test_inheritance_is_downward_and_reparent_or_membership_revokes_immediately(self):
        child_user, child_cookie = self.worker()
        parent_user, parent_cookie = self.worker("parent@punto.local")
        sibling_user, sibling_cookie = self.worker("sibling@punto.local")
        parent = self.group("Area")
        child = self.group("Squadra A", parent)
        sibling = self.group("Squadra B", parent)
        outside = self.group("Altra area")
        self.member(child, child_user)
        self.member(parent, parent_user)
        self.member(sibling, sibling_user)
        parent_job, child_job = self.job(), self.job("GROUP-002")
        self.grant(parent_job, parent)
        self.grant(child_job, child)
        self.assertEqual({job["id"] for job in self.jobs(child_cookie)}, {parent_job, child_job})
        self.assertEqual([job["id"] for job in self.jobs(parent_cookie)], [parent_job])
        self.assertEqual([job["id"] for job in self.jobs(sibling_cookie)], [parent_job])
        self.create_time(child_cookie, parent_job, date=self.today(), minutes=25)
        self.post("/api/groups/%d" % child, {"parent_id": outside}, 200)
        self.assertEqual([job["id"] for job in self.jobs(child_cookie)], [child_job])
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(parent_job, date=self.today()), cookie=child_cookie)
        self.delete("/api/groups/%d/members/%d" % (child, child_user))
        self.assertEqual(self.jobs(child_cookie), [])
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(child_job, date=self.today()), cookie=child_cookie)
        status, history, _ = self.api("GET", "/api/time-entries?date=" + self.today(), cookie=child_cookie)
        self.assertEqual(status, 200, history)
        self.assertEqual(history["total_minutes"], 25)

    def test_builtin_roles_are_immutable_and_custom_role_updates_existing_access(self):
        builtin = self.get("/api/job-roles")["roles"]
        self.assertEqual([(role["id"], role["code"], role["can_log_time"], role["builtin"])
                          for role in builtin], [(1, "reader", False, True), (2, "contributor", True, True)])
        for role_id in (1, 2):
            self.assert_error(409, "POST", "/api/job-roles/%d" % role_id,
                              {"can_log_time": "0"}, cookie=self.manager_cookie)
        role_id = self.role()
        user_id, worker = self.worker()
        group = self.group("Verificatori")
        self.member(group, user_id)
        job = self.job()
        self.grant(job, group, role_id)
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today()), cookie=worker)
        self.post("/api/job-roles/%d" % role_id, {"can_log_time": "1"}, 200)
        self.assertIs(self.jobs(worker)[0]["can_log_time"], True)
        self.create_time(worker, job, date=self.today(), minutes=15)
        self.assert_error(403, "GET", "/api/users", cookie=worker)
        self.assert_error(403, "POST", "/api/jobs", {
            "code": "ESCALATION", "name": "Vietata", "status": "active"}, cookie=worker)
        self.post("/api/job-roles/%d" % role_id, {"name": "Solo consultazione", "can_log_time": "0"}, 200)
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today()), cookie=worker)
        assignments = self.get("/api/jobs/%d/group-grants" % job)["grants"]
        self.assertEqual(assignments[0]["role_code"], "reviewer")
        self.assertEqual(assignments[0]["role_name"], "Solo consultazione")

    def test_multiple_memberships_and_direct_grants_use_strongest_current_role(self):
        user_id, worker = self.worker()
        read_group, work_group = self.group("Lettori"), self.group("Collaboratori")
        self.member(read_group, user_id)
        self.member(work_group, user_id)
        job = self.job()
        self.grant(job, read_group, 1)
        self.grant(job, work_group, 2)
        jobs = self.jobs(worker)
        self.assertEqual(len(jobs), 1, "Multiple memberships must not duplicate a job")
        self.assertIs(jobs[0]["can_log_time"], True)
        self.assign(job, user_id, role_id=1)
        self.create_time(worker, job, date=self.today(), minutes=20)
        self.delete("/api/jobs/%d/group-grants/%d" % (job, work_group))
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(job, date=self.today()), cookie=worker)
        self.assign(job, user_id, role_id=2, expected=200)
        self.assertIs(self.jobs(worker)[0]["can_log_time"], True)
        self.delete("/api/jobs/%d/assignments/%d" % (job, user_id))
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.delete("/api/jobs/%d/group-grants/%d" % (job, read_group))
        self.assertEqual(self.jobs(worker), [])

    def test_direct_assignment_default_and_omitted_role_preserve_compatibility(self):
        user_id, worker = self.worker()
        job = self.job()
        self.assign(job, user_id)
        path = "/api/jobs/%d/assignments" % job
        entry = self.get(path)["assignments"][0]
        self.assertEqual(entry["role_id"], 2)
        self.assertEqual(entry["role_code"], "contributor")
        self.assertIs(entry["can_log_time"], True)
        self.assign(job, user_id, role_id=1, expected=200)
        self.assign(job, user_id, expected=200, starts_on=self.today(-1))
        entry = self.get(path)["assignments"][0]
        self.assertEqual(entry["role_id"], 1)
        self.assertEqual(entry["starts_on"], self.today(-1))
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.assert_error(403, "POST", "/api/time-entries", self.time_fields(job, date=self.today()), cookie=worker)
        self.assign(job, user_id, role_id=2, expected=200)
        self.create_time(worker, job, date=self.today())
        for role_id, status in ((0, 400), ("2x", 400), (999999, 404)):
            self.assert_error(status, "POST", path, {"user_id": user_id, "role_id": role_id},
                              cookie=self.manager_cookie)

    def test_grant_dates_require_one_contributor_grant_covering_today_and_work_date(self):
        user_id, worker = self.worker()
        past, present = self.group("Attività passate"), self.group("Attività correnti")
        self.member(past, user_id)
        self.member(present, user_id)
        job = self.job()
        self.grant(job, past, 2, self.today(-3), self.today(-1))
        self.grant(job, present, 2, self.today(), self.today(1))
        self.assertIs(self.jobs(worker)[0]["can_log_time"], True)
        self.create_time(worker, job, date=self.today())
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today(-1)), cookie=worker)
        # A reader covering both dates cannot be combined with a separate contributor grant today.
        self.grant(job, past, 1, self.today(-3), self.today(1), expected=200)
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today(-1)), cookie=worker)
        self.assign(job, user_id, role_id=2, starts_on=self.today(-3), ends_on=self.today(-1))
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today(-1)), cookie=worker)
        self.grant(job, present, 2, self.today(-1), self.today(1), expected=200)
        self.create_time(worker, job, date=self.today(-1))
        self.assert_error(403, "POST", "/api/time-entries",
                          self.time_fields(job, date=self.today(2)), cookie=worker)
        self.grant(job, present, 2, self.today(1), self.today(2), expected=200)
        self.assertIs(self.jobs(worker)[0]["can_log_time"], False)
        self.delete("/api/jobs/%d/group-grants/%d" % (job, past))
        self.assertEqual(self.jobs(worker), [])

    def test_grant_list_edits_explicit_date_clear_and_group_delete_guard(self):
        group = self.group('Ricerca "R&D"')
        job = self.job()
        self.grant(job, group, 1, self.today(-1), self.today(2))
        path = "/api/jobs/%d/group-grants" % job
        role = next(role for role in self.get("/api/job-roles")["roles"] if role["id"] == 1)
        self.assertEqual(self.get(path), {"job_id": job, "grants": [{
            "group_id": group, "group_name": 'Ricerca "R&D"', "role_id": 1,
            "role_code": "reader", "role_name": role["name"], "can_log_time": False,
            "starts_on": self.today(-1), "ends_on": self.today(2)}]})
        self.post(path, {"group_id": group, "role_id": 2}, 200)
        entry = self.get(path)["grants"][0]
        self.assertEqual((entry["starts_on"], entry["ends_on"]), (self.today(-1), self.today(2)))
        self.post(path, {"group_id": group, "role_id": 2, "starts_on": "", "ends_on": ""}, 200)
        self.assertEqual((self.get(path)["grants"][0]["starts_on"],
                          self.get(path)["grants"][0]["ends_on"]), ("", ""))
        self.assert_error(409, "DELETE", "/api/groups/%d" % group, cookie=self.manager_cookie)
        self.delete(path + "/%d" % group)
        self.assert_error(404, "DELETE", path + "/%d" % group, cookie=self.manager_cookie)
        self.delete("/api/groups/%d" % group)

    def test_new_management_routes_enforce_authentication_role_and_origin(self):
        user_id, worker = self.worker()
        group, job = self.group("Sicurezza"), self.job()
        role = self.role()
        routes = (("GET", "/api/groups", None),
                  ("POST", "/api/groups", {"name": "Nuovo"}),
                  ("POST", "/api/groups/%d" % group, {"name": "Modificato"}),
                  ("DELETE", "/api/groups/%d" % group, None),
                  ("GET", "/api/groups/%d/members" % group, None),
                  ("POST", "/api/groups/%d/members" % group, {"user_id": user_id}),
                  ("DELETE", "/api/groups/%d/members/%d" % (group, user_id), None),
                  ("GET", "/api/job-roles", None),
                  ("POST", "/api/job-roles", {"code": "new", "name": "Nuovo", "can_log_time": "1"}),
                  ("POST", "/api/job-roles/%d" % role, {"name": "Modificato"}),
                  ("GET", "/api/jobs/%d/group-grants" % job, None),
                  ("POST", "/api/jobs/%d/group-grants" % job, {"group_id": group, "role_id": 2}),
                  ("DELETE", "/api/jobs/%d/group-grants/%d" % (job, group), None))
        before = self.events()
        for method, path, fields in routes:
            with self.subTest(method=method, path=path):
                self.assert_error(401, method, path, fields)
                self.assert_error(403, method, path, fields, cookie=worker)
                if method != "GET":
                    self.assert_error(403, method, path, fields, cookie=self.manager_cookie,
                                      headers={"Origin": "https://attacker.example"})
        self.assertEqual(self.events(), before)

    def test_group_role_and_grant_validation_rejects_unknown_duplicate_and_invalid_fields(self):
        user_id, _ = self.worker()
        group, job = self.group("Validazione"), self.job()
        grant_path = "/api/jobs/%d/group-grants" % job
        requests = (("/api/groups", {"name": ""}),
                    ("/api/groups", {"name": "x" * 121}),
                    ("/api/groups", {"name": "Nuovo", "parent_id": "0"}),
                    ("/api/groups", {"name": "Nuovo", "parent_id": "-1"}),
                    ("/api/groups", {"name": "Nuovo", "role": "manager"}),
                    ("/api/groups/%d" % group, {"id": group}),
                    ("/api/groups/%d/members" % group, {"user_id": "1x"}),
                    ("/api/groups/%d/members" % group, {"user_id": user_id, "role_id": 2}),
                    ("/api/job-roles", {"code": "Upper", "name": "Ruolo", "can_log_time": "0"}),
                    ("/api/job-roles", {"code": "has space", "name": "Ruolo", "can_log_time": "0"}),
                    ("/api/job-roles", {"code": "x" * 41, "name": "Ruolo", "can_log_time": "0"}),
                    ("/api/job-roles", {"code": "valid", "name": "Ruolo", "can_log_time": "true"}),
                    ("/api/job-roles", {"code": "valid", "name": "Ruolo", "can_log_time": "2"}),
                    ("/api/job-roles", {"code": "valid", "name": "Ruolo", "can_log_time": "1", "builtin": "1"}),
                    (grant_path, {"group_id": group, "role_id": "2x"}),
                    (grant_path, {"group_id": group, "role_id": 2, "starts_on": "2026-02-29"}),
                    (grant_path, {"group_id": group, "role_id": 2, "starts_on": self.today(1), "ends_on": self.today()}),
                    (grant_path, {"group_id": group, "role_id": 2, "user_id": user_id}))
        before = self.events()
        for path, fields in requests:
            with self.subTest(path=path, fields=fields):
                self.assert_error(400, "POST", path, fields, cookie=self.manager_cookie)
        for path, body in (("/api/groups", "name=Uno&name=Due"),
                           ("/api/groups", "name=%ZZ"),
                           ("/api/groups", "name=%ff"),
                           ("/api/job-roles", "code=valid&name=Ruolo&can_log_time=0&can_log_time=1"),
                           (grant_path, "group_id=%d&role_id=1&role_id=2" % group)):
            self.assert_error(400, "POST", path, cookie=self.manager_cookie,
                              headers={"Content-Type": "application/x-www-form-urlencoded"}, body=body)
        self.assertEqual(self.events(), before)
        for path in ("/api/groups", "/api/job-roles", "/api/groups/%d/members" % group, grant_path):
            self.assert_error(400, "GET", path + "?role=manager", cookie=self.manager_cookie)
        self.assert_error(415, "POST", "/api/groups", cookie=self.manager_cookie,
                          headers={"Content-Type": "application/json"}, body=json.dumps({"name": "JSON"}))
        for path, fields in (("/api/groups", {"name": "Orfano", "parent_id": 999999}),
                             ("/api/groups/999999", {"name": "Assente"}),
                             ("/api/groups/%d/members" % group, {"user_id": 999999}),
                             (grant_path, {"group_id": 999999, "role_id": 2}),
                             (grant_path, {"group_id": group, "role_id": 999999})):
            self.assert_error(404, "POST", path, fields, cookie=self.manager_cookie)
        self.role("unique_role")
        self.assert_error(409, "POST", "/api/job-roles", {
            "code": "unique_role", "name": "Duplicato", "can_log_time": "1"}, cookie=self.manager_cookie)
        status, data, _ = self.edit_user(user_id, active="0")
        self.assertEqual(status, 200, data)
        self.assert_error(400, "POST", "/api/groups/%d/members" % group,
                          {"user_id": user_id}, cookie=self.manager_cookie)

    def test_audit_covers_new_entities_and_failure_rolls_back_every_mutation(self):
        user_id, _ = self.worker()
        group, job, role = self.group("Audit"), self.job(), self.role()
        empty_group = self.group("Vuoto da conservare")
        self.member(group, user_id)
        self.grant(job, group, role)
        before_groups = self.get("/api/groups")
        before_members = self.get("/api/groups/%d/members" % group)
        before_roles = self.get("/api/job-roles")
        before_grants = self.get("/api/jobs/%d/group-grants" % job)
        before_events = self.events()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER block_group_audit BEFORE INSERT ON audit_events "
                       "BEGIN SELECT RAISE(ABORT, 'fixture audit failure'); END")
            db.commit()
        requests = (("POST", "/api/groups", {"name": "Rollback"}),
                    ("POST", "/api/groups/%d" % group, {"name": "Rollback"}),
                    ("DELETE", "/api/groups/%d" % empty_group, None),
                    ("POST", "/api/groups/%d/members" % group, {"user_id": self.users()[0]["id"]}),
                    ("DELETE", "/api/groups/%d/members/%d" % (group, user_id), None),
                    ("POST", "/api/job-roles", {"code": "rollback", "name": "Rollback", "can_log_time": "1"}),
                    ("POST", "/api/job-roles/%d" % role, {"can_log_time": "1"}),
                    ("POST", "/api/jobs/%d/group-grants" % job, {"group_id": empty_group, "role_id": 2}),
                    ("POST", "/api/jobs/%d/group-grants" % job, {"group_id": group, "role_id": 2}),
                    ("DELETE", "/api/jobs/%d/group-grants/%d" % (job, group), None))
        for method, path, fields in requests:
            with self.subTest(method=method, path=path):
                self.assert_error(500, method, path, fields, cookie=self.manager_cookie)
        self.assertEqual(self.get("/api/groups"), before_groups)
        self.assertEqual(self.get("/api/groups/%d/members" % group), before_members)
        self.assertEqual(self.get("/api/job-roles"), before_roles)
        self.assertEqual(self.get("/api/jobs/%d/group-grants" % job), before_grants)
        self.assertEqual(self.events(), before_events)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TRIGGER block_group_audit")
            db.commit()
        self.post("/api/groups/%d" % group, {"name": "Revisionato"}, 200)
        self.post("/api/job-roles/%d" % role, {"can_log_time": "1"}, 200)
        self.grant(job, group, 2, expected=200)
        self.delete("/api/jobs/%d/group-grants/%d" % (job, group))
        self.delete("/api/groups/%d/members/%d" % (group, user_id))
        self.delete("/api/groups/%d" % group)
        actions = {event["action"] for event in self.events()}
        self.assertTrue({"groups.create", "groups.update", "groups.delete", "group_members.create",
                         "group_members.delete", "job_roles.create", "job_roles.update",
                         "group_grants.create", "group_grants.update", "group_grants.delete"} <= actions)
        for entity in ("group", "group_member", "job_role", "group_grant"):
            events = self.events(entity)
            self.assertTrue(events)
            self.assertTrue(all(event["entity_type"] == entity for event in events))
            self.assertTrue(all(event["actor_id"] == self.users()[0]["id"] for event in events))
        update = next(event for event in self.events("job_role") if event["action"] == "job_roles.update")
        self.assertIs(update["before"]["can_log_time"], False)
        self.assertIs(update["after"]["can_log_time"], True)

    def test_version_two_upgrade_defaults_legacy_assignments_and_preserves_business_history(self):
        user_id, worker = self.worker()
        job = self.job()
        self.assign(job, user_id, starts_on=self.today(-2), ends_on=self.today(2))
        entry = self.create_time(worker, job, date=self.today(), minutes=35, note="Storico versione due")
        before_users = self.users()
        self.stop_server()
        old_tables = {"jobs", "users", "time_entries", "job_assignments", "suppliers", "contracts",
                      "contract_documents", "audit_events", "sqlite_sequence"}
        with closing(sqlite3.connect(self.db_path)) as db:
            # Restore the previous deployed table shape, with genuine persisted assignments/history.
            rows = db.execute("SELECT job_id,user_id,starts_on,ends_on FROM job_assignments").fetchall()
            for (table,) in db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
                if table not in old_tables:
                    db.execute('DROP TABLE "' + table.replace('"', '""') + '"')
            db.execute("DROP TABLE job_assignments")
            db.execute("CREATE TABLE job_assignments (job_id INTEGER NOT NULL REFERENCES jobs(id),"
                       "user_id INTEGER NOT NULL REFERENCES users(id),starts_on TEXT,ends_on TEXT,"
                       "PRIMARY KEY(job_id,user_id),CHECK(starts_on IS NULL OR length(starts_on)=10),"
                       "CHECK(ends_on IS NULL OR length(ends_on)=10),"
                       "CHECK(starts_on IS NULL OR ends_on IS NULL OR starts_on<=ends_on))")
            db.executemany("INSERT INTO job_assignments VALUES(?,?,?,?)", rows)
            # Remove only later migration records manufactured while creating this old fixture.
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (3,4,5,6,7,8,9,10)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=2")
            db.commit()
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assertEqual(self.users(), before_users)
        assignment = self.get("/api/jobs/%d/assignments" % job)["assignments"][0]
        self.assertEqual((assignment["user_id"], assignment["role_id"], assignment["role_code"]),
                         (user_id, 2, "contributor"))
        self.assertEqual((assignment["starts_on"], assignment["ends_on"]), (self.today(-2), self.today(2)))
        self.assertIs(self.jobs(worker)[0]["can_log_time"], True)
        status, history, _ = self.api("GET", "/api/time-entries?date=" + self.today(), cookie=worker)
        self.assertEqual(status, 200, history)
        self.assertEqual((history["total_minutes"], history["entries"][0]["id"]), (35, entry))
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (10,))
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE NOT(action='system.migration' AND entity_id IN (3,4,5,6,7,8,9,10)) ORDER BY id").fetchall(), old_audit)
        events = self.events()
        self.assertEqual(sum(event["action"] == "system.migration" and event["entity_id"] == 3 for event in events), 1)
        self.assertEqual(sum(event["action"] == "system.migration" and event["entity_id"] == 4 for event in events), 1)
        self.assertEqual(sum(event["action"] == "system.migration" and event["entity_id"] == 5 for event in events), 1)
        self.assertEqual(sum(event["action"] == "system.migration" and event["entity_id"] == 6 for event in events), 1)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.events(), events, "Restart must not repeat the migration")


if __name__ == "__main__":
    unittest.main()
