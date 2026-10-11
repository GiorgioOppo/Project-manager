"""Live job overview scopes and access explanations, exercised over HTTP."""

from datetime import datetime, timedelta
import unittest
import sqlite3
from zoneinfo import ZoneInfo

import test_roles as roles


class JobOverviewIntegrationTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = roles.RoleIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    assert_error = roles.RoleIntegrationTests.assert_error
    create_user = roles.RoleIntegrationTests.create_user
    create_job = roles.RoleIntegrationTests.create_job
    assign_job = roles.RoleIntegrationTests.assign_job
    create_time = roles.RoleIntegrationTests.create_time
    time_fields = roles.RoleIntegrationTests.time_fields
    users = roles.RoleIntegrationTests.users
    edit_user = roles.RoleIntegrationTests.edit_user

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def post(self, path, fields, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def access(self, job_id, user_id):
        return self.get("/api/jobs/%d/access?user_id=%d" % (job_id, user_id))

    def overview(self, job_id, cookie=None):
        return self.get("/api/jobs/%d/overview" % job_id, cookie)

    def test_overview_scopes_hours_and_task_totals_without_leaking_people_or_notes(self):
        worker_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        manager_id = next(user["id"] for user in self.users() if user["role"] == "manager")
        team = self.post("/api/groups", {"name": "Squadra riepilogo"})["id"]
        self.post("/api/groups/%d/members" % team, {"user_id": manager_id})
        self.post("/api/groups/%d/members" % team, {"user_id": worker_id})
        job_id = self.create_job()
        self.assign_job(job_id, worker_id)
        task = self.post("/api/tasks", {"job_id": job_id, "title": "Consegna", "status": "done"})["id"]
        self.post("/api/blockers", {"job_id": job_id, "title": "Aperto"})
        self.create_time(worker, job_id, minutes=45, task_id=task, note="Nota privata operatore")
        self.create_time(self.manager_cookie, job_id, minutes=90, task_id=task, note="Nota privata gestore")
        self.create_time(self.manager_cookie, job_id, minutes=15)
        personal = self.overview(job_id, worker)
        all_users = self.overview(job_id)
        self.assertIs(personal["job"]["can_log_time"], True)
        self.assertIs(personal["job"]["can_view"], True)
        self.assertEqual((personal["scope"], personal["total_minutes"], personal["time_entries"]), ("personal", 45, 1))
        self.assertEqual((all_users["scope"], all_users["total_minutes"], all_users["time_entries"]), ("team", 150, 3))
        self.assertEqual(personal["tasks"], {"total": 1, "open": 0, "done": 1})
        self.assertEqual(personal["blockers"], {"total": 1, "open": 1, "resolved": 0})
        self.assertNotIn("collaborators", personal)
        self.assertEqual(len(all_users["collaborators"]), 2)
        self.assertEqual(personal["task_totals"][0]["total_minutes"], 45)
        self.assertEqual(all_users["task_totals"][0]["total_minutes"], 135)
        for response in (personal, all_users):
            self.assertNotIn("Nota privata", str(response))
        self.api("DELETE", "/api/jobs/%d/assignments/%d" % (job_id, worker_id), cookie=self.manager_cookie)
        self.assert_error(403, "GET", "/api/jobs/%d/overview" % job_id, cookie=worker)

    def test_overview_and_closure_exclude_outsiders_and_hide_commercial_metadata(self):
        worker_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        outside_id = self.create_user("outside@punto.local", "Outside-only!2026")
        outside = self.login("outside@punto.local", "Outside-only!2026")
        manager_id = next(user["id"] for user in self.users() if user["role"] == "manager")
        parent = self.post("/api/groups", {"name": "Area"})["id"]
        child = self.post("/api/groups", {"name": "Team", "parent_id": parent})["id"]
        self.post("/api/groups/%d/members" % parent, {"user_id": manager_id})
        self.post("/api/groups/%d/members" % child, {"user_id": worker_id})
        job = self.create_job()
        self.assign_job(job, worker_id); self.assign_job(job, outside_id)
        task = self.post("/api/tasks", {"job_id": job, "title": "Attività comune"})["id"]
        self.create_time(worker, job, minutes=40, task_id=task)
        self.create_time(outside, job, minutes=90, task_id=task)
        self.create_time(self.manager_cookie, job, minutes=10)
        with sqlite3.connect(self.db_path) as database:
            supplier = database.execute("INSERT INTO suppliers(code,name) VALUES('PRIVATE','Fornitore riservato')").lastrowid
            contract = database.execute("INSERT INTO contracts(code,title,supplier_id) VALUES('SECRET','Titolo riservato',?)", (supplier,)).lastrowid
            database.execute("UPDATE jobs SET contract_id=? WHERE id=?", (contract, job))
        overview = self.overview(job)
        self.assertEqual((overview["total_minutes"], overview["time_entries"]), (50, 2))
        self.assertEqual(overview["task_totals"][0]["total_minutes"], 40)
        for key in ("contract_id", "contract_code", "contract_title", "supplier_name"):
            self.assertNotIn(key, overview["job"])
            self.assertNotIn(key, self.overview(job, worker)["job"])
        closure = self.get("/api/jobs/%d/closure" % job)
        self.assertEqual((closure["total_minutes"], closure["time_entries"], closure["users_with_hours"]), (50, 2, 2))
        closed = self.post("/api/jobs/%d/lifecycle" % job,
            {"revision": 1, "action": "close", "reason": "Consegna"}, expected=200)
        self.assertEqual((closed["total_minutes"], closed["time_entries"]), (50, 2))
        self.post("/api/users/%d/access-groups" % manager_id, {"groups": '["finance"]'}, expected=200)
        self.assertEqual(self.overview(job)["job"]["contract_code"], "SECRET")
        status, data, _ = self.api("DELETE", "/api/groups/%d/members/%d" % (child, worker_id), cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertEqual(self.overview(job)["total_minutes"], 10)
        self.assertEqual(self.get("/api/jobs/%d/closure" % job)["total_minutes"], 10)

    def test_explanation_keeps_alternative_paths_and_shows_inherited_access(self):
        worker_id = self.create_user()
        job_id = self.create_job()
        parent = self.post("/api/groups", {"name": "Area tecnica"})["id"]
        child = self.post("/api/groups", {"name": "Sviluppo", "parent_id": parent})["id"]
        self.post("/api/groups/%d/members" % child, {"user_id": worker_id})
        self.post("/api/jobs/%d/group-grants" % job_id, {"group_id": parent, "role_id": 2})
        self.post("/api/jobs/%d/assignments" % job_id, {"user_id": worker_id, "role_id": 1})
        result = self.access(job_id, worker_id)
        self.assertEqual(result["effective"], {"can_view": True, "can_log_time": True})
        self.assertEqual({source["type"] for source in result["sources"]}, {"direct", "group"})
        inherited = next(source for source in result["sources"] if source["type"] == "group")
        self.assertEqual((inherited["group_id"], inherited["group_name"]), (parent, "Area tecnica"))
        self.assertTrue(inherited["inherited"])
        self.assertTrue(inherited["active"])
        direct = next(source for source in result["sources"] if source["type"] == "direct")
        self.assertFalse(direct["can_log_time"])
        status, data, _ = self.api("DELETE", "/api/jobs/%d/assignments/%d" % (job_id, worker_id),
                                   cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertTrue(self.access(job_id, worker_id)["effective"]["can_log_time"])
        self.post("/api/jobs/%d/lifecycle" % job_id,
                  {"action": "close", "revision": 1, "reason": "Fine lavoro"}, expected=200)
        closed = self.access(job_id, worker_id)
        self.assertEqual(closed["effective"], {"can_view": True, "can_log_time": False})
        self.assertTrue(closed["sources"][0]["can_log_time"])
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assertIs(self.overview(job_id, worker)["job"]["can_log_time"], False)

    def test_explanation_distinguishes_dates_common_manager_and_inactive_user(self):
        worker_id = self.create_user()
        job_id = self.create_job()
        today = datetime.now(ZoneInfo("Europe/Rome")).date()
        tomorrow = (today + timedelta(days=1)).isoformat()
        self.post("/api/jobs/%d/assignments" % job_id,
                  {"user_id": worker_id, "role_id": 2, "starts_on": tomorrow})
        result = self.access(job_id, worker_id)
        self.assertEqual(result["effective"], {"can_view": False, "can_log_time": False})
        self.assertFalse(result["sources"][0]["valid_today"])
        self.assertFalse(result["sources"][0]["active"])
        self.post("/api/jobs/%d" % job_id,
                  {"code": "ROLE-001", "name": "Commessa condivisa", "status": "active", "is_common": 1}, expected=200)
        result = self.access(job_id, worker_id)
        self.assertTrue(result["effective"]["can_log_time"])
        self.assertTrue(next(source for source in result["sources"] if source["type"] == "common")["active"])
        manager_id = next(user["id"] for user in self.users() if user["role"] == "manager")
        self.assertIn("manager", [source["type"] for source in self.access(job_id, manager_id)["sources"]])
        status, data, _ = self.edit_user(worker_id, active="0")
        self.assertEqual(status, 200, data)
        inactive = self.access(job_id, worker_id)
        self.assertEqual(inactive["effective"], {"can_view": False, "can_log_time": False})
        self.assertTrue(all(not source["active"] for source in inactive["sources"]))

    def test_access_endpoint_requires_manager_and_valid_query(self):
        worker_id = self.create_user()
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        job_id = self.create_job()
        path = "/api/jobs/%d/access?user_id=%d" % (job_id, worker_id)
        self.assert_error(401, "GET", path)
        self.assert_error(403, "GET", path, cookie=worker)
        self.assert_error(405, "POST", path, {}, cookie=self.manager_cookie)
        for query in ("", "?user_id=0", "?user_id=x", "?user_id=1&user_id=2", "?other=1"):
            self.assert_error(400, "GET", "/api/jobs/%d/access" % job_id + query, cookie=self.manager_cookie)
        self.assert_error(404, "GET", "/api/jobs/%d/access?user_id=999999" % job_id, cookie=self.manager_cookie)
        self.assert_error(404, "GET", "/api/jobs/999999/overview", cookie=self.manager_cookie)


if __name__ == "__main__":
    unittest.main()
