"""Opt-in Chromium authorization journeys against isolated real HTTP/SQLite."""
from datetime import datetime
from zoneinfo import ZoneInfo
import json
import os
from pathlib import Path
import subprocess
import unittest

import test_roles as roles
import test_procurement as procurement


@unittest.skipUnless(os.environ.get("PUNTO_ACCESS_BROWSER_TEST") == "1", "Optional Chromium authorization journeys")
class AccessProfilesBrowserTests(unittest.TestCase):
    setUp = roles.RoleIntegrationTests.setUp
    start_server = procurement.ProcurementIntegrationTests.start_server
    stop_server = roles.RoleIntegrationTests.stop_server
    api = roles.RoleIntegrationTests.api
    login = roles.RoleIntegrationTests.login
    create_user = roles.RoleIntegrationTests.create_user
    time_fields = roles.RoleIntegrationTests.time_fields
    create_time = roles.RoleIntegrationTests.create_time
    raw = procurement.ProcurementIntegrationTests.raw
    upload = procurement.ProcurementIntegrationTests.upload

    def post(self, path, fields, expected=201):
        status, data, _ = self.api("POST", path, fields, cookie=self.manager_cookie)
        self.assertEqual(status, expected, data)
        return data

    def test_real_desktop_mobile_profiles_and_team_privacy(self):
        self.stop_server(); self.public = roles.ROOT / "public"; self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        status, session, _ = self.api("GET", "/api/session", cookie=self.manager_cookie)
        self.assertEqual(status, 200, session); manager = session["user_id"]
        team = self.create_user(name="Persona squadra")
        outsider = self.create_user(email="outside@punto.local", name="Persona altro gruppo")
        finance = self.create_user(email="finance@punto.local", name="Profilo dipartimento")
        cookies = {user: self.login(email, roles.WORKER_PASSWORD) for user, email in
                   [(team, roles.WORKER_EMAIL), (outsider, "outside@punto.local"), (finance, "finance@punto.local")]}
        self.post("/api/users/%d/access-groups" % manager, {"groups": '["sales","finance"]'}, 200)
        supplier = self.post("/api/suppliers", {"code": "AUTH-SUP", "name": "Fornitore fixture"})["id"]
        contract = self.post("/api/contracts", {"code": "AUTH-CTR", "title": "Accordo segregato", "supplier_id": supplier, "status": "active", "value_cents": 100000})["id"]
        status, document, _ = self.upload(contract, b"%PDF-1.7\nprivate fixture\n%%EOF", filename="accordo-fixture.pdf")
        self.assertEqual(status, 201, document)
        job = self.post("/api/jobs", {"code": "AUTH-JOB", "name": "Commessa segregata", "status": "active", "contract_id": contract})["id"]
        today = datetime.now(ZoneInfo("Europe/Rome")).date().isoformat()
        for user in (team, outsider, finance):
            self.post("/api/jobs/%d/assignments" % job, {"user_id": user})
        for user, cookie, minutes, note in [(manager, self.manager_cookie, 30, "Ore proprie"),
                (team, cookies[team], 60, "Ore della squadra"), (outsider, cookies[outsider], 90, "DETTAGLIO-ALTRO-GRUPPO"),
                (finance, cookies[finance], 15, "Ore Finance personali")]:
            self.create_time(cookie, job, date=today, minutes=minutes, note=note)
            self.post("/api/labor-rates", {"user_id": user, "hourly_cents": 1000, "starts_on": today, "ends_on": "", "currency": "EUR"})
        parent = self.post("/api/groups", {"name": "Squadra del gestore"})["id"]
        child = self.post("/api/groups", {"name": "Sottosquadra", "parent_id": parent})["id"]
        other = self.post("/api/groups", {"name": "Altro gruppo"})["id"]
        for group, user in [(parent, manager), (child, team), (other, outsider), (other, finance)]:
            self.post("/api/groups/%d/members" % group, {"user_id": user})
        self.post("/api/users/%d/access-groups" % manager, {"groups": "[]"}, 200)
        config = {"origin": self.origin, "manager_cookie": self.manager_cookie, "manager_id": manager,
                  "finance_cookie": cookies[finance], "finance_id": finance, "team_id": team,
                  "outsider_id": outsider, "today": today, "job_id": job, "contract_id": contract}
        file = Path(self.fixture.name) / "browser-access.json"; file.write_text(json.dumps(config), encoding="utf-8")
        result = subprocess.run([os.environ.get("PUNTO_NODE_BINARY", "node"), str(roles.ROOT / "tests/access_profiles_real_browser.cjs"), str(file)],
                                cwd=roles.ROOT, env=os.environ.copy(), capture_output=True, text=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())
