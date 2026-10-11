"""Opt-in desktop/mobile Chromium journeys for operational extensions."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import test_roles as roles
import test_operations_improvements as operations


@unittest.skipUnless(os.environ.get("PUNTO_OPERATIONS_BROWSER_TEST") == "1", "Optional Chromium operations journeys")
class OperationsBrowserTests(unittest.TestCase):
    for name in ("setUp", "start_server", "stop_server", "api", "login", "create_user", "assert_error",
                 "today", "time_fields", "create_time", "db", "get", "post", "task", "correction"):
        locals()[name] = getattr(operations.OperationsImprovementsTests, name)

    def test_operations_desktop_mobile(self):
        self.stop_server()
        self.public = roles.ROOT / "public"
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        task = self.task(title="Stima browser")
        template = self.post("/api/job-templates", {"name": "Modello browser", "definition": '{"name":"Proposta","client":"Cliente"}'}, expected=201)["id"]
        request = self.correction()
        schedule = self.post("/api/work-schedules", {"user_id": self.worker_id, "starts_on": self.today(-7),
                             "mon": 480, "tue": 480, "wed": 480, "thu": 480, "fri": 480,
                             "sat": 0, "sun": 0}, expected=201)["id"]
        config = {"origin": self.origin, "manager_cookie": self.manager_cookie, "worker_cookie": self.worker_cookie,
                  "worker_id": self.worker_id, "job_id": self.job_id, "task_id": task, "template_id": template,
                  "correction_id": request, "schedule_id": schedule, "schedule_start": self.today(-7),
                  "schedule_next": self.today(2), "today": self.today(), "tomorrow": self.today(1)}
        config_path = Path(self.fixture.name) / "operations-browser.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"), str(roles.ROOT / "tests/operations_improvements_browser.cjs"), str(config_path)], cwd=roles.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())
