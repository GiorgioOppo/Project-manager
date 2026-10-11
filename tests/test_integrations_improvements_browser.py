"""Opt-in desktop/mobile journeys using real Punto and isolated private add-ons."""
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
import test_roles as roles
import test_improvements_integrations as integrations


@unittest.skipUnless(os.environ.get("PUNTO_INTEGRATIONS_BROWSER_TEST") == "1", "Optional Chromium integration journeys")
class IntegrationBrowserTests(unittest.TestCase):
    for name in ("setUpClass", "setUp", "start_server", "stop_server", "api", "login", "create_user", "edit_user", "assert_error", "assign_job", "supplier", "contract", "raw", "upload", "create_time", "time_fields", "get", "post", "sql", "rate", "seed_email"):
        locals()[name] = integrations.IntegrationImprovementsTests.__dict__[name]

    def test_integrations_desktop_mobile(self):
        self.stop_server()
        self.public = roles.ROOT / "public"
        with patch.dict(os.environ, self.environment):
            self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.worker_cookie = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.rate()
        contract = self.contract(value_cents=50000)
        self.post(f"/api/jobs/{self.job_id}", {"code": "INT-01", "name": "Integrazioni", "status": "active", "contract_id": contract})
        pending = self.seed_email("pending_configuration", "browser:pending")
        unknown = self.seed_email("delivery_unknown", "browser:unknown")
        self.service.reply_value = {"ok": True, "configured": True, "oauth_configured": True, "connected": False,
            "items": [{"id": 7, "person_code": "P-7", "display_name": "Persona verificata", "active": True,
                       "asset_tag": "IT-7", "name": "Dotazione fixture", "asset_type": "it_device", "status": "available"}],
            "person": {"id": 7, "display_name": "Persona verificata", "active": True}}
        config = {"origin": self.origin, "worker_cookie": self.worker_cookie, "manager_cookie": self.manager_cookie,
                  "worker_id": self.worker_id, "job_id": self.job_id, "contract_id": contract, "pending": pending, "unknown": unknown}
        config_path = Path(self.fixture.name) / "integrations-browser.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"), str(roles.ROOT / "tests/integrations_improvements_browser.cjs"), str(config_path)], cwd=roles.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())
