"""Tasks, Kanban and blockers: HTTP permissions, consistency and schema migration.

Every case owns a temporary SQLite database and a private ephemeral HTTP server.
These tests do not use the deployment database, Docker or external services.
"""

from contextlib import closing
from datetime import datetime, timedelta
import json
import sqlite3
import unittest
from zoneinfo import ZoneInfo

import test_roles as roles


class WorkBoardIntegrationTests(unittest.TestCase):
    # Share the HTTP fixture without inheriting its unrelated account test cases.
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

    def today(self, offset=0):
        return (datetime.now(ZoneInfo("Europe/Rome")).date() + timedelta(days=offset)).isoformat()

    def worker(self, email=roles.WORKER_EMAIL, **changes):
        user_id = self.create_user(email=email, **changes)
        return user_id, self.login(email, roles.WORKER_PASSWORD)

    def get(self, path, cookie=None):
        status, data, _ = self.api("GET", path, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, 200, data)
        return data

    def post(self, path, fields, expected=201, cookie=None):
        status, data, _ = self.api("POST", path, fields, cookie=cookie or self.manager_cookie)
        self.assertEqual(status, expected, data)
        self.assertIs(data.get("ok"), True)
        return data

    def delete(self, path):
        status, data, _ = self.api("DELETE", path, cookie=self.manager_cookie)
        self.assertEqual(status, 200, data)
        self.assertIs(data.get("ok"), True)
        return data

    def job(self, code="BOARD-001", **changes):
        return self.post("/api/jobs", {"code": code, "name": "Commessa " + code,
                                       "status": "active", **changes})["id"]

    def assign(self, job_id, user_id, role_id=2, expected=201, **dates):
        return self.post("/api/jobs/%d/assignments" % job_id,
                         {"user_id": user_id, "role_id": role_id, **dates}, expected)

    def board(self, job_id, cookie=None):
        return self.get("/api/work-board?job_id=%d" % job_id, cookie)

    def task(self, job_id, **changes):
        return self.post("/api/tasks", {"job_id": job_id, "title": "Attività di prova", **changes})["id"]

    def blocker(self, job_id, cookie=None, **changes):
        return self.post("/api/blockers", {"job_id": job_id, "title": "Impedimento di prova", **changes},
                         cookie=cookie)["id"]

    def item(self, job_id, collection, item_id, cookie=None):
        return next(item for item in self.board(job_id, cookie)[collection] if item["id"] == item_id)

    def task_item(self, job_id, task_id, cookie=None):
        return self.item(job_id, "tasks", task_id, cookie)

    def blocker_item(self, job_id, blocker_id, cookie=None):
        return self.item(job_id, "blockers", blocker_id, cookie)

    def update_task(self, job_id, task_id, cookie=None, **changes):
        fields = {"revision": self.task_item(job_id, task_id)["revision"], **changes}
        return self.post("/api/tasks/%d" % task_id, fields, 200, cookie)

    def update_blocker(self, job_id, blocker_id, cookie=None, **changes):
        fields = {"revision": self.blocker_item(job_id, blocker_id)["revision"], **changes}
        return self.post("/api/blockers/%d" % blocker_id, fields, 200, cookie)

    def delete_task(self, job_id, task_id):
        return self.post("/api/tasks/%d/delete" % task_id,
                         {"revision": self.task_item(job_id, task_id)["revision"]}, 200)

    def delete_blocker(self, job_id, blocker_id):
        return self.post("/api/blockers/%d/delete" % blocker_id,
                         {"revision": self.blocker_item(job_id, blocker_id)["revision"]}, 200)

    def snapshot(self, tables=("job_tasks", "job_blockers", "audit_events")):
        with closing(sqlite3.connect(self.db_path)) as db:
            return {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                    for table in tables}

    def events(self):
        return self.get("/api/audit?limit=100")["events"]

    def test_endpoints_require_authentication_and_mutations_check_origin(self):
        job = self.job()
        task = self.task(job)
        blocker = self.blocker(job)
        requests = (("GET", "/api/work-board?job_id=%d" % job, None),
                    ("POST", "/api/tasks", {"job_id": job, "title": "Nuova"}),
                    ("POST", "/api/tasks/%d" % task, {"revision": 1, "status": "done"}),
                    ("POST", "/api/tasks/%d/delete" % task, {"revision": 1}),
                    ("POST", "/api/blockers", {"job_id": job, "title": "Nuovo"}),
                    ("POST", "/api/blockers/%d" % blocker, {"revision": 1, "status": "resolved"}),
                    ("POST", "/api/blockers/%d/delete" % blocker, {"revision": 1}))
        before = self.snapshot()
        for method, path, fields in requests:
            with self.subTest(path=path):
                self.assert_error(401, method, path, fields)
                if method == "POST":
                    self.assert_error(403, method, path, fields, cookie=self.manager_cookie,
                                      headers={"Origin": "https://attacker.example"})
        self.assertEqual(self.snapshot(), before)

    def test_task_defaults_details_and_restart_preserve_board(self):
        job = self.job()
        task = self.task(job)
        row = self.task_item(job, task)
        self.assertEqual(row["job_id"], job)
        self.assertEqual((row["title"], row["description"], row["status"], row["priority"]),
                         ("Attività di prova", "", "todo", "normal"))
        self.assertIsNone(row["due_date"])
        self.assertIsNone(row["parent_id"])
        self.assertIsNone(row["assigned_user_id"])
        self.assertEqual(row["revision"], 1)
        self.assertTrue(row["created_at"])
        self.assertTrue(row["updated_at"])
        self.assertIs(row["can_edit"], True)
        self.assertIs(row["can_update_status"], True)
        manager_id = self.users()[0]["id"]
        self.update_task(job, task, title="Preparare la consegna", description="Piano e verifica",
                         assigned_user_id=manager_id, priority="high", due_date=self.today(3))
        blocker = self.blocker(job, task_id=task, owner_id=manager_id,
                               next_action="Richiedere documentazione", due_date=self.today(2))
        before = self.board(job)
        self.assertIs(before["permissions"]["manage_tasks"], True)
        self.assertIs(before["permissions"]["report_blockers"], True)
        self.assertEqual(self.blocker_item(job, blocker)["task_id"], task)
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.board(job), before)

    def test_private_job_and_item_ids_do_not_leak_board_or_user_names(self):
        job = self.job()
        secret_user, _ = self.worker("secret@punto.local", name="Nome riservato")
        self.assign(job, secret_user)
        task = self.task(job, assigned_user_id=secret_user, title="Contenuto riservato")
        blocker = self.blocker(job, owner_id=secret_user, title="Blocco riservato")
        _, outsider = self.worker("outside@punto.local")
        for path, fields in (("/api/work-board?job_id=%d" % job, None),
                             ("/api/tasks/%d" % task, {"revision": 1, "status": "done"}),
                             ("/api/blockers/%d" % blocker, {"revision": 1, "status": "resolved"}),
                             ("/api/blockers", {"job_id": job, "title": "Non consentito"})):
            method = "GET" if fields is None else "POST"
            status, payload, _ = self.api(method, path, fields, cookie=outsider)
            self.assertEqual(status, 404, payload)
            for private in ("Nome riservato", "Contenuto riservato", "Blocco riservato", "secret@punto.local"):
                self.assertNotIn(private, json.dumps(payload, ensure_ascii=False))
        self.assert_error(404, "GET", "/api/work-board?job_id=999999", cookie=self.manager_cookie)
        for resource, item_id in (("tasks", task), ("blockers", blocker)):
            for suffix in ("", "/delete"):
                hidden = self.api("POST", "/api/%s/%d%s" % (resource, item_id, suffix),
                                  {"revision": 1}, cookie=outsider)
                missing = self.api("POST", "/api/%s/999999%s" % (resource, suffix),
                                   {"revision": 1}, cookie=outsider)
                self.assertEqual(hidden[:2], missing[:2], "Private IDs must not be enumerable")

    def test_reader_is_read_only_and_contributor_updates_only_assigned_task_status(self):
        worker_id, worker = self.worker()
        reader_id, reader = self.worker("reader@punto.local")
        job = self.job()
        self.assign(job, worker_id)
        self.assign(job, reader_id, role_id=1)
        mine = self.task(job, assigned_user_id=worker_id)
        unassigned = self.task(job, title="Senza responsabile")
        for cookie in (worker, reader):
            board = self.board(job, cookie)
            self.assertEqual({item["id"] for item in board["tasks"]}, {mine, unassigned})
            self.assertIs(board["permissions"]["manage_tasks"], False)
            self.assertEqual(board.get("assignees", []), [])
            self.assertTrue(all(item["can_edit"] is False for item in board["tasks"]))
            self.assert_error(403, "POST", "/api/tasks", {"job_id": job, "title": "No"}, cookie=cookie)
            self.assert_error(403, "POST", "/api/tasks/%d/delete" % mine,
                              {"revision": 1}, cookie=cookie)
        self.assertIs(self.board(job, reader)["permissions"]["report_blockers"], False)
        self.assertIs(self.board(job, worker)["permissions"]["report_blockers"], True)
        self.assertIs(self.task_item(job, mine, worker)["can_update_status"], True)
        self.assertIs(self.task_item(job, unassigned, worker)["can_update_status"], False)
        self.assertIs(self.task_item(job, mine, reader)["can_update_status"], False)
        self.assert_error(403, "POST", "/api/tasks/%d" % mine,
                          {"revision": 1, "status": "done"}, cookie=reader)
        self.assert_error(403, "POST", "/api/tasks/%d" % unassigned,
                          {"revision": 1, "status": "done"}, cookie=worker)
        for forbidden in ({"title": "Cambio titolo"}, {"priority": "urgent"},
                          {"assigned_user_id": worker_id}, {"description": "Modifica"},
                          {"parent_id": unassigned}, {"due_date": self.today(1)}):
            self.assert_error(403, "POST", "/api/tasks/%d" % mine,
                              {"revision": 1, "status": "done", **forbidden}, cookie=worker)
        self.update_task(job, mine, worker, status="in_progress")
        self.assertEqual(self.task_item(job, mine)["status"], "in_progress")

    def test_group_inheritance_and_role_changes_revoke_write_access_immediately(self):
        user_id, worker = self.worker()
        parent = self.post("/api/groups", {"name": "Area"})["id"]
        child = self.post("/api/groups", {"name": "Squadra", "parent_id": parent})["id"]
        self.post("/api/groups/%d/members" % child, {"user_id": user_id})
        job = self.job()
        role = self.post("/api/job-roles", {"code": "board-editor", "name": "Collaboratore tecnico",
                                          "can_log_time": "1"})["id"]
        self.post("/api/jobs/%d/group-grants" % job, {"group_id": parent, "role_id": role})
        task = self.task(job, assigned_user_id=user_id)
        blocker = self.blocker(job, worker)
        self.update_task(job, task, worker, status="in_progress")
        self.post("/api/job-roles/%d" % role, {"can_log_time": "0"}, 200)
        self.assertIs(self.task_item(job, task, worker)["can_update_status"], False)
        self.assert_error(403, "POST", "/api/tasks/%d" % task,
                          {"revision": 2, "status": "done"}, cookie=worker)
        self.assert_error(403, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=worker)
        self.delete("/api/groups/%d/members/%d" % (child, user_id))
        self.assert_error(404, "GET", "/api/work-board?job_id=%d" % job, cookie=worker)
        self.assert_error(404, "POST", "/api/tasks/%d" % task,
                          {"revision": 2, "status": "done"}, cookie=worker)
        self.assert_error(404, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=worker)

    def test_assignees_include_only_active_current_contributors_and_managers(self):
        eligible, _ = self.worker()
        reader, _ = self.worker("reader@punto.local")
        outsider, _ = self.worker("outsider@punto.local")
        expired, _ = self.worker("expired@punto.local")
        future, _ = self.worker("future@punto.local")
        inactive, _ = self.worker("inactive@punto.local")
        job = self.job()
        self.assign(job, eligible)
        self.assign(job, reader, role_id=1)
        self.assign(job, expired, ends_on=self.today(-1))
        self.assign(job, future, starts_on=self.today(1))
        self.assign(job, inactive)
        status, data, _ = self.edit_user(inactive, active="0")
        self.assertEqual(status, 200, data)
        assignees = self.board(job)["assignees"]
        # Assignee records use stable user IDs, independent of list ordering.
        ids = {person.get("id", person.get("user_id")) for person in assignees}
        manager = self.users()[0]["id"]
        self.assertEqual(ids, {eligible, manager})
        for user_id in (reader, outsider, expired, future, inactive, 999999):
            with self.subTest(user_id=user_id):
                self.assert_error(400, "POST", "/api/tasks",
                                  {"job_id": job, "title": "Assegnazione negata", "assigned_user_id": user_id},
                                  cookie=self.manager_cookie)
                self.assert_error(400, "POST", "/api/blockers",
                                  {"job_id": job, "title": "Responsabile negato", "owner_id": user_id},
                                  cookie=self.manager_cookie)
        task = self.task(job, assigned_user_id=eligible)
        self.delete("/api/jobs/%d/assignments/%d" % (job, eligible))
        # Existing ownership is history: a complete manager form may preserve it.
        # A new assignment to the same revoked account must still be rejected.
        self.update_task(job, task, assigned_user_id=eligible, title="Mantiene responsabile storico")
        self.assertEqual(self.task_item(job, task)["assigned_user_id"], eligible)
        unassigned = self.task(job, title="Nuova assegnazione vietata")
        self.assert_error(400, "POST", "/api/tasks/%d" % unassigned,
                          {"revision": 1, "assigned_user_id": eligible}, cookie=self.manager_cookie)
        self.update_task(job, task, assigned_user_id="")
        self.assertIsNone(self.task_item(job, task)["assigned_user_id"])

    def test_common_jobs_allow_contributors_without_grants_and_disabling_account_ends_session(self):
        user_id, worker = self.worker()
        job = self.job(is_common="1")
        task = self.task(job, assigned_user_id=user_id)
        self.update_task(job, task, worker, status="blocked")
        blocker = self.blocker(job, worker, task_id=task)
        self.assertEqual(self.blocker_item(job, blocker)["created_by"], user_id)
        self.assertIs(self.board(job, worker)["permissions"]["report_blockers"], True)
        status, data, _ = self.edit_user(user_id, active="0")
        self.assertEqual(status, 200, data)
        self.assert_error(401, "GET", "/api/work-board?job_id=%d" % job, cookie=worker)
        self.assert_error(401, "POST", "/api/tasks/%d" % task,
                          {"revision": 2, "status": "done"}, cookie=worker)

    def test_hierarchy_rejects_cross_job_cycles_and_depth_over_eight(self):
        job, other = self.job(), self.job("BOARD-002")
        first = self.task(job)
        chain = [first]
        for depth in range(2, 9):
            chain.append(self.task(job, title="Livello %d" % depth, parent_id=chain[-1]))
        other_task = self.task(other)
        for parent in (chain[-1], other_task, 999999):
            self.assert_error(400, "POST", "/api/tasks",
                              {"job_id": job, "title": "Non valida", "parent_id": parent},
                              cookie=self.manager_cookie)
        for child, parent in ((first, first), (first, chain[-1]), (chain[3], chain[6]), (first, other_task)):
            self.assert_error(400, "POST", "/api/tasks/%d" % child,
                              {"revision": 1, "parent_id": parent}, cookie=self.manager_cookie)
        independent = self.task(job, title="Radice indipendente")
        # Moving an existing subtree must account for all descendants, not just its root.
        self.assert_error(400, "POST", "/api/tasks/%d" % first,
                          {"revision": 1, "parent_id": independent}, cookie=self.manager_cookie)
        self.update_task(job, chain[1], parent_id="")
        self.assertIsNone(self.task_item(job, chain[1])["parent_id"])
        self.update_task(job, first, parent_id=independent)
        self.assertEqual(self.task_item(job, first)["parent_id"], independent)

    def test_all_kanban_states_and_priorities_round_trip_without_implicit_blocker_changes(self):
        job = self.job()
        task = self.task(job)
        blocker = self.blocker(job, task_id=task)
        for status in ("in_progress", "blocked", "in_review", "done", "todo"):
            self.update_task(job, task, status=status)
            self.assertEqual(self.task_item(job, task)["status"], status)
            self.assertEqual(self.blocker_item(job, blocker)["status"], "open")
        for priority in ("low", "normal", "high", "urgent"):
            self.update_task(job, task, priority=priority)
            self.assertEqual(self.task_item(job, task)["priority"], priority)
        self.update_blocker(job, blocker, status="resolved")
        self.assertEqual(self.task_item(job, task)["status"], "todo")

    def test_task_revision_rejects_stale_update_and_delete_without_losing_changes(self):
        job = self.job()
        task = self.task(job)
        initial = self.task_item(job, task)["revision"]
        self.update_task(job, task, title="Versione aggiornata")
        row = self.task_item(job, task)
        self.assertEqual(row["revision"], initial + 1)
        before = self.snapshot()
        for path, fields in (("/api/tasks/%d" % task, {"revision": initial, "title": "Sovrascritta"}),
                             ("/api/tasks/%d/delete" % task, {"revision": initial})):
            self.assert_error(409, "POST", path, fields, cookie=self.manager_cookie)
        self.assertEqual(self.snapshot(), before)
        for revision in (None, "0", "-1", "1x", "1.0", "9223372036854775808"):
            fields = {"status": "done"}
            if revision is not None:
                fields["revision"] = revision
            self.assert_error(400, "POST", "/api/tasks/%d" % task, fields, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/tasks/%d/delete" % task, {}, cookie=self.manager_cookie)
        self.delete_task(job, task)
        self.assert_error(404, "POST", "/api/tasks/%d" % task,
                          {"revision": initial + 1, "status": "done"}, cookie=self.manager_cookie)

    def test_form_contract_rejects_unknown_duplicate_and_invalid_task_fields(self):
        job = self.job()
        valid = {"job_id": job, "title": "Valida"}
        invalid = ({"title": ""}, {"title": "   "}, {"title": "a" * 161},
                   {"description": "a" * 2001}, {"status": "completed"}, {"priority": "critical"},
                   {"due_date": "2026-02-29"}, {"due_date": "2026-13-01"},
                   {"due_date": "2026-1-01"}, {"due_date": "0000-01-01"},
                   {"parent_id": "-1"}, {"assigned_user_id": "1x"},
                   {"job_id": "0"}, {"job_id": "1x"}, {"created_by": "1"}, {"revision": "1"})
        before = self.snapshot()
        for changes in invalid:
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/tasks", dict(valid, **changes), cookie=self.manager_cookie)
        self.assert_error(415, "POST", "/api/tasks", cookie=self.manager_cookie,
                          headers={"Content-Type": "application/json"}, body=json.dumps(valid))
        for body in ("job_id=%d&title=One&title=Two" % job,
                     "job_id=%d&title=%%ZZ" % job,
                     "job_id=%d&title=Test%%00hidden" % job):
            self.assert_error(400, "POST", "/api/tasks", cookie=self.manager_cookie,
                              headers={"Content-Type": "application/x-www-form-urlencoded"}, body=body)
        self.assertEqual(self.snapshot(), before)
        for query in ("", "?job_id=0", "?job_id=1x", "?job_id=%d&job_id=%d" % (job, job),
                      "?job_id=%d&user_id=1" % job):
            self.assert_error(400, "GET", "/api/work-board" + query, cookie=self.manager_cookie)
        task = self.task(job, title="a" * 160, description="b" * 2000, due_date="2028-02-29")
        self.assertEqual(self.task_item(job, task)["due_date"], "2028-02-29")
        self.update_task(job, task, due_date="", description="")
        row = self.task_item(job, task)
        self.assertEqual((row["due_date"], row["description"]), (None, ""))
        self.assert_error(400, "POST", "/api/tasks/%d" % task,
                          {"revision": row["revision"], "job_id": job}, cookie=self.manager_cookie)

    def test_worker_blocker_creation_defaults_owner_to_self_and_limits_ownership_changes(self):
        first_id, first = self.worker()
        second_id, second = self.worker("second@punto.local")
        job = self.job()
        self.assign(job, first_id)
        self.assign(job, second_id)
        task = self.task(job, assigned_user_id=second_id)
        blocker = self.blocker(job, first, task_id=task, description="Manca un documento")
        row = self.blocker_item(job, blocker, first)
        self.assertEqual((row["created_by"], row["owner_id"], row["status"], row["impact"]),
                         (first_id, first_id, "open", "medium"))
        self.assertIs(row["can_edit"], True)
        self.assertIs(self.blocker_item(job, blocker, second)["can_edit"], False)
        self.assert_error(403, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=second)
        self.assert_error(403, "POST", "/api/blockers", {"job_id": job, "title": "Altro",
                                                               "owner_id": second_id}, cookie=first)
        self.assert_error(403, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "owner_id": second_id}, cookie=first)
        self.update_blocker(job, blocker, first, status="in_progress", next_action="Contattare il fornitore",
                            impact="high", due_date=self.today(2))
        self.update_blocker(job, blocker, owner_id=second_id)
        # Both the creator and the new owner can maintain a blocker while contributors.
        for cookie in (first, second):
            self.assertIs(self.blocker_item(job, blocker, cookie)["can_edit"], True)
        self.update_blocker(job, blocker, second, status="resolved")
        self.update_blocker(job, blocker, first, status="open")
        self.assert_error(403, "POST", "/api/blockers/%d/delete" % blocker,
                          {"revision": self.blocker_item(job, blocker)["revision"]}, cookie=first)
        self.delete_blocker(job, blocker)
        self.assertEqual(self.board(job)["blockers"], [])

    def test_reader_cannot_report_blockers_and_revoke_removes_creator_access(self):
        user_id, worker = self.worker()
        job = self.job()
        self.assign(job, user_id, role_id=1)
        self.assert_error(403, "POST", "/api/blockers", {"job_id": job, "title": "No"}, cookie=worker)
        self.assign(job, user_id, expected=200)
        blocker = self.blocker(job, worker)
        self.assign(job, user_id, role_id=1, expected=200)
        self.assertIs(self.blocker_item(job, blocker, worker)["can_edit"], False)
        self.assert_error(403, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=worker)
        self.delete("/api/jobs/%d/assignments/%d" % (job, user_id))
        self.assert_error(404, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=worker)
        self.assertEqual(self.blocker_item(job, blocker)["created_by"], user_id)

    def test_blocker_validation_revision_and_immutable_job(self):
        job = self.job()
        other_job = self.job("BOARD-002")
        task = self.task(other_job)
        valid = {"job_id": job, "title": "Valido"}
        for changes in ({"title": ""}, {"title": "a" * 161}, {"description": "x" * 2001},
                        {"next_action": "x" * 1001}, {"status": "closed"}, {"impact": "urgent"},
                        {"due_date": "2026-02-29"}, {"due_date": "2026-10-32"},
                        {"owner_id": "-1"}, {"task_id": task}, {"task_id": 999999},
                        {"created_by": 1}):
            with self.subTest(changes=changes):
                self.assert_error(400, "POST", "/api/blockers", dict(valid, **changes), cookie=self.manager_cookie)
        blocker = self.blocker(job, next_action="x" * 1000)
        self.update_blocker(job, blocker, status="in_progress")
        before = self.snapshot()
        self.assert_error(409, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 1, "status": "resolved"}, cookie=self.manager_cookie)
        self.assert_error(409, "POST", "/api/blockers/%d/delete" % blocker,
                          {"revision": 1}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 2, "job_id": job}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/blockers/%d" % blocker,
                          {"revision": 2, "created_by": 999999}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/blockers/%d" % blocker,
                          {"status": "resolved"}, cookie=self.manager_cookie)
        self.assert_error(400, "POST", "/api/blockers/%d/delete" % blocker,
                          {}, cookie=self.manager_cookie)
        self.assertEqual(self.snapshot(), before)
        for impact in ("low", "medium", "high", "critical"):
            self.update_blocker(job, blocker, impact=impact)
            self.assertEqual(self.blocker_item(job, blocker)["impact"], impact)

    def test_task_delete_rejects_children_and_detaches_blockers_atomically(self):
        job = self.job()
        parent = self.task(job)
        child = self.task(job, parent_id=parent)
        blocker = self.blocker(job, task_id=child)
        self.assert_error(409, "POST", "/api/tasks/%d/delete" % parent,
                          {"revision": 1}, cookie=self.manager_cookie)
        before_blocker = self.blocker_item(job, blocker)
        self.delete_task(job, child)
        row = self.blocker_item(job, blocker)
        self.assertIsNone(row["task_id"])
        self.assertEqual(row["title"], before_blocker["title"])
        self.assertGreater(row["revision"], before_blocker["revision"])
        # A form opened before the task disappeared must not restore the stale link.
        self.assert_error(409, "POST", "/api/blockers/%d" % blocker,
                          {"revision": before_blocker["revision"], "status": "resolved"},
                          cookie=self.manager_cookie)
        self.delete_task(job, parent)
        self.assertEqual(self.board(job)["tasks"], [])
        self.assertEqual(len(self.board(job)["blockers"]), 1)
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_audit_failure_rolls_back_task_blocker_and_detachment_mutations(self):
        job = self.job()
        task = self.task(job)
        blocker = self.blocker(job, task_id=task)
        before = self.snapshot()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER fail_work_board_audit BEFORE INSERT ON audit_events "
                       "BEGIN SELECT RAISE(ABORT,'fixture audit failure'); END")
            db.commit()
        requests = (("/api/tasks", {"job_id": job, "title": "Rollback"}),
                    ("/api/tasks/%d" % task, {"revision": 1, "title": "Rollback"}),
                    ("/api/tasks/%d/delete" % task, {"revision": 1}),
                    ("/api/blockers", {"job_id": job, "title": "Rollback"}),
                    ("/api/blockers/%d" % blocker, {"revision": 1, "status": "resolved"}),
                    ("/api/blockers/%d/delete" % blocker, {"revision": 1}))
        for path, fields in requests:
            with self.subTest(path=path):
                self.assert_error(500, "POST", path, fields, cookie=self.manager_cookie)
                self.assertEqual(self.snapshot(), before)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TRIGGER fail_work_board_audit")
            db.commit()
        self.update_task(job, task, status="done")
        self.delete_task(job, task)
        self.update_blocker(job, blocker, status="resolved")
        self.delete_blocker(job, blocker)
        actions = {event["action"] for event in self.events()}
        self.assertTrue({"tasks.create", "tasks.update", "tasks.delete", "blockers.create",
                         "blockers.update", "blockers.delete"} <= actions)
        updated = next(event for event in self.events() if event["action"] == "tasks.update")
        self.assertEqual(updated["entity_type"], "task")
        self.assertEqual(updated["entity_id"], task)
        self.assertEqual(updated["before"]["status"], "todo")
        self.assertEqual(updated["after"]["status"], "done")
        deleted = next(event for event in self.events() if event["action"] == "blockers.delete")
        self.assertEqual(deleted["entity_type"], "blocker")
        self.assertIsNone(deleted["after"])

    def test_job_delete_removes_only_its_board_and_rolls_back_on_audit_failure(self):
        job = self.job()
        survivor = self.job("BOARD-002")
        root = self.task(job)
        leaf = self.task(job, parent_id=root)
        self.blocker(job, task_id=leaf)
        self.blocker(job)
        self.task(survivor, title="Attività da conservare")
        self.blocker(survivor, title="Blocco da conservare")
        preserved = self.board(survivor)
        tables = ("jobs", "job_tasks", "job_blockers", "audit_events")
        before = self.snapshot(tables)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("CREATE TRIGGER fail_board_job_delete BEFORE INSERT ON audit_events "
                       "WHEN NEW.action='jobs.delete' BEGIN SELECT RAISE(ABORT,'audit unavailable'); END")
            db.commit()
        self.assert_error(500, "DELETE", "/api/jobs/%d" % job, cookie=self.manager_cookie)
        self.assertEqual(self.snapshot(tables), before)
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TRIGGER fail_board_job_delete")
            db.commit()
        self.delete("/api/jobs/%d" % job)
        self.assertEqual(self.board(survivor), preserved)
        self.assert_error(404, "GET", "/api/work-board?job_id=%d" % job, cookie=self.manager_cookie)
        with closing(sqlite3.connect(self.db_path)) as db:
            for table in ("job_tasks", "job_blockers"):
                self.assertEqual(db.execute('SELECT COUNT(*) FROM "' + table + '" WHERE job_id=?',
                                            (job,)).fetchone(), (0,))
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_untrusted_titles_and_descriptions_are_round_tripped_as_json_strings(self):
        user_id, _ = self.worker(name='Mario "Qualità" ✓')
        job = self.job()
        self.assign(job, user_id)
        text = '<script>alert("x")</script> & "citazione" ✓'
        description = 'Prima riga\nSeconda riga </script><img src=x onerror=alert(1)>'
        task = self.task(job, title=text, description=description, assigned_user_id=user_id)
        blocker = self.blocker(job, title=text, description=description,
                               next_action=text, owner_id=user_id)
        status, board, headers = self.api("GET", "/api/work-board?job_id=%d" % job,
                                         cookie=self.manager_cookie)
        self.assertEqual(status, 200, board)
        self.assertIn("application/json", headers["Content-Type"])
        task_row = next(row for row in board["tasks"] if row["id"] == task)
        blocker_row = next(row for row in board["blockers"] if row["id"] == blocker)
        self.assertEqual((task_row["title"], task_row["description"]), (text, description))
        self.assertEqual(task_row["assigned_user_name"], 'Mario "Qualità" ✓')
        self.assertEqual((blocker_row["title"], blocker_row["description"], blocker_row["next_action"]),
                         (text, description, text))
        self.assertNotIn(roles.WORKER_EMAIL, json.dumps(board))

    def test_maximum_multibyte_blocker_form_fits_route_limit_without_raising_other_limits(self):
        job = self.job()
        task = self.task(job)
        fields = {"title": "📋" * 160, "description": "🔎" * 2000,
                  "next_action": "📞" * 1000, "task_id": task}
        # 3,160 valid Unicode characters exceed 32 KiB after form URL encoding.
        blocker = self.blocker(job, **fields)
        self.update_blocker(job, blocker, **fields, status="in_progress")
        row = self.blocker_item(job, blocker)
        for name in ("title", "description", "next_action"):
            self.assertEqual(row[name], fields[name])
        for path in ("/api/tasks", "/api/tasks/%d" % task,
                     "/api/blockers", "/api/blockers/%d" % blocker):
            with self.subTest(path=path):
                # Declare an oversized body without sending it: rejection occurs
                # at the header boundary, without risking a TCP reset from unread bytes.
                self.assert_error(413, "POST", path, cookie=self.manager_cookie,
                                  headers={"Content-Type": "application/x-www-form-urlencoded",
                                           "Content-Length": str(65536 + 1)})
        for path in ("/api/jobs", "/api/users", "/api/alert-rules"):
            self.assert_error(413, "POST", path, cookie=self.manager_cookie,
                              headers={"Content-Type": "application/x-www-form-urlencoded",
                                       "Content-Length": str(32768 + 1)})
        self.assert_error(413, "POST", "/api/login", cookie=self.manager_cookie,
                          headers={"Content-Type": "application/x-www-form-urlencoded",
                                   "Content-Length": str(8192 + 1)})

    def test_schema_seven_upgrade_preserves_existing_accounts_jobs_hours_and_audit(self):
        user_id, worker = self.worker()
        job = self.job()
        self.assign(job, user_id)
        entry = self.create_time(worker, job, date=self.today(), minutes=35, note="Prima dello schema otto")
        self.stop_server()
        with closing(sqlite3.connect(self.db_path)) as db:
            db.execute("DROP TABLE job_blockers")
            db.execute("DROP TABLE job_tasks")
            trigger = db.execute("SELECT sql FROM sqlite_master WHERE type='trigger' "
                                 "AND name='audit_events_no_delete'").fetchone()[0]
            db.execute("DROP TRIGGER audit_events_no_delete")
            db.execute("DELETE FROM audit_events WHERE action='system.migration' AND entity_id IN (8,9,10)")
            db.execute(trigger)
            db.execute("PRAGMA user_version=7")
            db.commit()
            preserved_tables = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")
                                if row[0] not in ("audit_events", "sqlite_sequence")]
            preserved = {table: db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall()
                         for table in preserved_tables}
            old_audit = db.execute("SELECT * FROM audit_events ORDER BY id").fetchall()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        worker = self.login(roles.WORKER_EMAIL, roles.WORKER_PASSWORD)
        self.assertEqual(self.board(job)["tasks"], [])
        self.assertEqual(self.board(job)["blockers"], [])
        history = self.get("/api/time-entries?date=" + self.today(), worker)
        self.assertEqual((history["total_minutes"], history["entries"][0]["id"]), (35, entry))
        with closing(sqlite3.connect(self.db_path)) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (10,))
            for table, rows in preserved.items():
                self.assertEqual(db.execute('SELECT * FROM "' + table + '" ORDER BY rowid').fetchall(), rows, table)
            self.assertEqual(db.execute("SELECT * FROM audit_events WHERE NOT(action='system.migration' "
                                        "AND entity_id IN (8,9,10)) ORDER BY id").fetchall(), old_audit)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE action='system.migration' "
                                        "AND entity_id=8").fetchone(), (1,))
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])
        task = self.task(job, assigned_user_id=user_id)
        self.update_task(job, task, worker, status="in_progress")
        before = self.snapshot()
        self.stop_server()
        self.start_server()
        self.manager_cookie = self.login(roles.MANAGER_EMAIL, roles.MANAGER_PASSWORD)
        self.assertEqual(self.snapshot(), before, "Restart must not repeat migration or alter the board")


if __name__ == "__main__":
    unittest.main()
