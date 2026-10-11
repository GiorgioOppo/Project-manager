"""Opt-in real Chromium journeys against an isolated SQLite HTTP fixture."""
import json
import os
from pathlib import Path
import subprocess
import unittest

import test_roles as roles
import test_user_workflows as workflows


@unittest.skipUnless(os.environ.get("PUNTO_WORKFLOWS_BROWSER_TEST") == "1", "Optional Chromium journeys")
class UserWorkflowBrowserTests(unittest.TestCase):
    setUp = workflows.UserWorkflowTests.setUp
    start_server = workflows.UserWorkflowTests.start_server
    stop_server = workflows.UserWorkflowTests.stop_server
    api = workflows.UserWorkflowTests.api
    login = workflows.UserWorkflowTests.login
    create_user = workflows.UserWorkflowTests.create_user
    today = workflows.UserWorkflowTests.today
    worker = workflows.UserWorkflowTests.worker
    get = workflows.UserWorkflowTests.get
    post = workflows.UserWorkflowTests.post
    job = workflows.UserWorkflowTests.job
    assign = workflows.UserWorkflowTests.assign
    task = workflows.UserWorkflowTests.task
    blocker = workflows.UserWorkflowTests.blocker
    time_fields = workflows.UserWorkflowTests.time_fields
    create_time = workflows.UserWorkflowTests.create_time

    def test_desktop_and_mobile_journeys(self):
        self.stop_server()
        self.public = roles.ROOT / "public"
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        manager = self.get("/api/session")["user_id"]
        user, cookie = self.worker()
        group = self.post("/api/groups", {"name": "Squadra browser"})["id"]
        for member in (manager, user):
            self.post(f"/api/groups/{group}/members", {"user_id": member})
        job = self.job("BROWSER-1", name="Verifica dei flussi")
        self.assign(job, user)
        task = self.task(job, title="Consegna di prova", assigned_user_id=user, due_date=self.today(1))
        self.blocker(job, cookie=cookie, title="Documento da ricevere", owner_id=user)
        self.create_time(cookie, job, date=self.today(), minutes=60, note="Lavoro originale", task_id=task)
        self.create_time(cookie, job, date=self.today(-7), minutes=30, note="Settimana precedente", task_id=task)
        self.task(job, title="Controllo del gestore", assigned_user_id=manager)
        config = {"origin": self.origin, "manager_cookie": self.manager_cookie,
                  "worker_cookie": cookie, "worker_id": user, "job_id": job,
                  "task_id": task, "today": self.today()}
        config_path = Path(self.fixture.name) / "browser.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"),
                                 str(roles.ROOT / "tests/user_workflows_browser.cjs"), str(config_path)],
                                cwd=roles.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())
