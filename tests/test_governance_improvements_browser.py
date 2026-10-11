"""Opt-in real-HTTP governance journeys in desktop and mobile Chromium."""
import json
import os
from pathlib import Path
import subprocess
import unittest

import test_roles as roles
import test_improvements_governance as governance


@unittest.skipUnless(os.environ.get("PUNTO_GOVERNANCE_BROWSER_TEST") == "1", "Optional Chromium governance journeys")
class GovernanceBrowserTests(unittest.TestCase):
    for name in ("setUp", "start_server", "stop_server", "api", "login", "create_user", "users", "edit_user", "assert_error", "create_time", "time_fields", "assign_job", "get", "post", "today", "manager", "worker", "group", "member", "job", "grant"):
        locals()[name] = getattr(governance.GovernanceIntegrationTests, name)

    def test_governance_desktop_mobile(self):
        self.stop_server()
        self.public = roles.ROOT / "public"
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        area, area_cookie = self.manager("area-browser@example.test")
        second, second_cookie = self.manager("second-admin-browser@example.test")
        worker, worker_cookie = self.worker("worker-browser@example.test")
        parent = self.group("Area browser")
        child = self.group("Sottogruppo browser", parent)
        destination = self.group("Destinazione browser")
        self.member(parent, area)
        self.member(child, worker)
        job = self.job("BROWSER-GOV")
        self.grant(job, parent, 1)
        self.grant(job, destination, 2)
        config = {"origin": self.origin, "manager_cookie": self.manager_cookie,
                  "area_cookie": area_cookie, "second_cookie": second_cookie,
                  "worker_cookie": worker_cookie, "area": area, "second": second,
                  "worker": worker, "group": parent, "child": child,
                  "destination": destination, "job": job, "today": self.today()}
        path = Path(self.fixture.name) / "governance-browser.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"),
            str(roles.ROOT / "tests/governance_improvements_browser.cjs"), str(path)],
            cwd=roles.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())
