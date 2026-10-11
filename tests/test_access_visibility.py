"""Cross-profile, cross-team and indirect-data disclosure over isolated HTTP."""
import json
import unittest

import test_user_workflows as workflows


class AccessVisibilityTests(unittest.TestCase):
    setUp = workflows.UserWorkflowTests.setUp
    start_server = workflows.UserWorkflowTests.start_server
    stop_server = workflows.UserWorkflowTests.stop_server
    api = workflows.UserWorkflowTests.api
    login = workflows.UserWorkflowTests.login
    create_user = workflows.UserWorkflowTests.create_user
    assert_error = workflows.UserWorkflowTests.assert_error
    today = workflows.UserWorkflowTests.today
    worker = workflows.UserWorkflowTests.worker
    get = workflows.UserWorkflowTests.get
    post = workflows.UserWorkflowTests.post
    job = workflows.UserWorkflowTests.job
    assign = workflows.UserWorkflowTests.assign
    task = workflows.UserWorkflowTests.task
    time_fields = workflows.UserWorkflowTests.time_fields
    create_time = workflows.UserWorkflowTests.create_time

    def profiles(self, user, *groups):
        return self.post(f"/api/users/{user}/access-groups", {"groups": json.dumps(groups)}, 200)

    def manager(self):
        return self.get("/api/session")["user_id"]

    def group(self, name, parent=None):
        return self.post("/api/groups", {"name": name, **({"parent_id": parent} if parent else {})})["id"]

    def member(self, group, user):
        self.post(f"/api/groups/{group}/members", {"user_id": user})

    def report(self, view="user", cookie=None, **filters):
        query = f"/api/reports?view={view}&from={self.today()}&to={self.today()}"
        for key, value in filters.items():
            query += f"&{key}={value}"
        return self.get(query, cookie)

    def seed_team(self):
        manager = self.manager()
        same, same_cookie = self.worker()
        child, child_cookie = self.worker("child@example.test")
        outside, outside_cookie = self.worker("outside@example.test")
        parent = self.group("Squadra del gestore")
        subgroup = self.group("Sottosquadra", parent)
        self.member(parent, manager)
        self.member(parent, same)
        self.member(subgroup, child)
        job = self.job()
        task = self.task(job)
        for user, cookie, minutes, note in ((same, same_cookie, 30, "Same team"),
                                           (child, child_cookie, 45, "Child team"),
                                           (outside, outside_cookie, 90, "OUTSIDE SECRET")):
            self.assign(job, user)
            self.create_time(cookie, job, date=self.today(), minutes=minutes, note=note, task_id=task)
        return manager, same, same_cookie, child, child_cookie, outside, outside_cookie, parent, job

    def test_personal_and_team_daily_weekly_reports_and_revocation(self):
        manager, same, cookie, child, _, outside, _, group, job = self.seed_team()
        for collection in ("", "/week"):
            path = f"/api/time-entries{collection}?date={self.today()}&user_id={child}"
            self.assertEqual(self.get(path)["total_minutes"], 45)
            self.assert_error(403, "GET", path, cookie=cookie)
            self.assert_error(403, "GET", f"/api/time-entries{collection}?date={self.today()}&user_id={outside}", cookie=self.manager_cookie)
        report = self.report()
        self.assertEqual(report["scope"], "team")
        self.assertEqual(report["summary"]["minutes"], 75)
        self.assertEqual({u["id"] for u in report["options"]["users"]}, {manager, same, child})
        self.assertEqual(self.get(f"/api/jobs/{job}/overview")["total_minutes"], 75)
        self.assertEqual(self.get(f"/api/jobs/{job}/closure")["total_minutes"], 75)
        status, payload, _ = self.api("DELETE", f"/api/groups/{group}/members/{manager}", cookie=self.manager_cookie)
        self.assertEqual(status, 200, payload)
        self.assertEqual(self.report()["summary"]["minutes"], 0)
        self.assertEqual(self.get(f"/api/jobs/{job}/overview")["total_minutes"], 0)
        self.assert_error(403, "GET", f"/api/time-entries?date={self.today()}&user_id={same}", cookie=self.manager_cookie)

    def test_manager_without_team_has_no_implicit_access_to_other_hours(self):
        user, cookie = self.worker()
        job = self.job()
        self.assign(job, user)
        self.create_time(cookie, job, date=self.today(), minutes=60, note="PRIVATE")
        self.assertEqual(self.report()["summary"]["minutes"], 0)
        self.assertEqual([u["id"] for u in self.get("/api/time-team")["users"]], [self.manager()])
        self.assert_error(403, "GET", f"/api/time-entries?date={self.today()}&user_id={user}", cookie=self.manager_cookie)
        self.assertNotIn("PRIVATE", json.dumps(self.get("/api/audit?limit=100")))

    def financial_fixture(self):
        manager = self.manager()
        self.profiles(manager, "sales", "finance")
        supplier = self.post("/api/suppliers", {"code": "FIN-SUP", "name": "Riservato"})["id"]
        contract = self.post("/api/contracts", {"code": "PRIVATE-CONTRACT", "title": "PRIVATE TERMS", "supplier_id": supplier,
                                              "status": "active", "value_cents": 100000})["id"]
        job = self.job(contract_id=contract)
        for email, minutes in (("one@example.test", 30), ("two@example.test", 45)):
            user, cookie = self.worker(email)
            self.assign(job, user)
            self.post("/api/labor-rates", {"user_id": user, "hourly_cents": 6000, "currency": "EUR", "starts_on": self.today()})
            self.create_time(cookie, job, date=self.today(), minutes=minutes)
        finance, finance_cookie = self.worker("finance@example.test")
        self.profiles(finance, "finance")
        self.profiles(manager)
        return manager, finance, finance_cookie, job, contract

    def test_finance_costs_are_per_job_without_third_party_personal_hours(self):
        _, finance, cookie, job, _ = self.financial_fixture()
        result = self.report("labor_cost", cookie)
        self.assertEqual(result["scope"], "financial")
        self.assertEqual(result["summary"]["estimated_cost_cents"], 7500)
        self.assertEqual(len(result["rows"]), 1)
        self.assertEqual(result["rows"][0]["job_id"], job)
        for key in ("user_id", "name", "email"):
            self.assertNotIn(key, result["rows"][0])
        self.assertEqual(result["options"]["users"], [])
        self.assertEqual(self.report("user", cookie)["summary"]["minutes"], 0)
        path = f"/api/reports?view=labor_cost&from={self.today()}&to={self.today()}"
        self.assert_error(403, "GET", path, cookie=self.manager_cookie)
        self.assert_error(403, "GET", path + f"&user_id={finance}", cookie=cookie)
        self.assert_error(403, "POST", "/api/labor-rates", {"user_id": finance, "hourly_cents": 100, "starts_on": self.today()}, cookie=cookie)
        self.assertEqual(len(self.get("/api/labor-rates", cookie)["rates"]), 2)
        self.profiles(finance)
        self.assert_error(403, "GET", path, cookie=cookie)
        self.assert_error(403, "GET", "/api/labor-rates", cookie=cookie)

    def test_contract_metadata_templates_and_audit_respect_functional_groups(self):
        manager, _, finance_cookie, job, contract = self.financial_fixture()
        for record in self.get("/api/jobs")["jobs"]:
            for key in ("contract_id", "contract_code", "contract_title", "supplier_name"):
                self.assertNotIn(key, record)
        summary = self.get(f"/api/jobs/{job}/overview")
        self.assertNotIn("PRIVATE TERMS", json.dumps(summary))
        self.assert_error(403, "GET", "/api/contracts", cookie=self.manager_cookie)
        self.assertEqual(self.get("/api/contracts", finance_cookie)["contracts"][0]["id"], contract)
        self.assert_error(403, "POST", f"/api/jobs/{job}", {"code": "BOARD-001", "name": "Cambio", "status": "active", "contract_id": ""}, cookie=self.manager_cookie)
        self.profiles(manager, "sales")
        self.post("/api/job-templates", {"name": "Commerciale", "definition": json.dumps({"name": "Template", "contract_id": contract})})
        self.profiles(manager)
        definition = json.loads(self.get("/api/job-templates")["templates"][0]["definition_json"])
        self.assertNotIn("contract_id", definition)
        audit = self.get("/api/audit?limit=100")
        self.assertNotIn("PRIVATE TERMS", json.dumps(audit))
        self.assertFalse(any(e["entity_type"] in ("contract", "supplier", "labor_rate") for e in audit["events"]))
