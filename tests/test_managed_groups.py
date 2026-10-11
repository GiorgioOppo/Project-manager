"""Real SQLite projection, migration and authorization checks for Keycloak groups."""

import copy
import ctypes
from contextlib import closing, contextmanager
import json
import os
from pathlib import Path
import shlex
import sqlite3
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
ISSUER = "https://auth.example.test/realms/punto"


@contextmanager
def database(path):
    with closing(sqlite3.connect(path)) as connection, connection:
        yield connection


SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE users(id INTEGER PRIMARY KEY,email TEXT COLLATE NOCASE UNIQUE,name TEXT,role TEXT,active INTEGER);
CREATE TABLE oidc_identities(issuer TEXT,subject TEXT,user_id INTEGER UNIQUE REFERENCES users(id),PRIMARY KEY(issuer,subject));
CREATE TABLE organization_groups(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,parent_id INTEGER REFERENCES organization_groups(id),CHECK(parent_id IS NULL OR parent_id<>id));
CREATE TABLE oidc_groups(issuer TEXT,subject TEXT,group_id INTEGER UNIQUE REFERENCES organization_groups(id),active INTEGER,PRIMARY KEY(issuer,subject));
CREATE TABLE group_members(group_id INTEGER REFERENCES organization_groups(id),user_id INTEGER REFERENCES users(id),PRIMARY KEY(group_id,user_id));
CREATE TABLE job_roles(id INTEGER PRIMARY KEY,code TEXT UNIQUE,name TEXT,can_log_time INTEGER,builtin INTEGER);
CREATE TABLE jobs(id INTEGER PRIMARY KEY,is_common INTEGER);
CREATE TABLE job_assignments(job_id INTEGER,user_id INTEGER,role_id INTEGER,starts_on TEXT,ends_on TEXT);
CREATE TABLE job_group_grants(job_id INTEGER,group_id INTEGER REFERENCES organization_groups(id),role_id INTEGER,starts_on TEXT,ends_on TEXT,PRIMARY KEY(job_id,group_id));
CREATE TABLE audit_events(id INTEGER PRIMARY KEY,actor_id INTEGER,actor_name TEXT,actor_email TEXT,action TEXT,entity_type TEXT,entity_id INTEGER,before_json TEXT,after_json TEXT);
INSERT INTO users VALUES(1,'admin@example.test','Admin','manager',1),(2,'worker@example.test','Worker','worker',1),(3,'other@example.test','Other','worker',1);
INSERT INTO organization_groups VALUES(10,'Engineering',NULL),(11,'Design',10);
INSERT INTO group_members VALUES(11,2);
INSERT INTO job_roles VALUES(1,'reader','Lettore',0,1),(2,'contributor','Collaboratore',1,1);
INSERT INTO jobs VALUES(10,0),(11,0),(12,1);
INSERT INTO job_group_grants VALUES(10,10,2,NULL,NULL);
INSERT INTO job_assignments VALUES(11,2,1,NULL,NULL);
"""


class Response(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("body", ctypes.c_void_p)]


class ManagedGroupsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="punto-managed-groups-build-")
        cls.addClassCleanup(cls.build.cleanup)
        helper = Path(cls.build.name) / "helper.c"
        helper.write_text('''
#include "organization.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
static time_t fixed_clock;
time_t managed_test_time(time_t *out) { struct timespec ts; time_t now=fixed_clock; if(!now) { clock_gettime(CLOCK_REALTIME,&ts); now=ts.tv_sec; } if(out)*out=now;return now; }
void test_clock(time_t now) { fixed_clock=now; }
int oidc_users_managed(void) { const char *s=getenv("AUTH_USER_MANAGEMENT"); return s && !strcmp(s,"keycloak"); }
int test_access(const char *path,DbInt job,DbInt user) { Db *db=NULL; if(db_open_v2(path,&db,DB_OPEN_READWRITE,NULL)!=DB_OK)return -1; int answer=organization_job_access(db,job,user,"2026-10-11",NULL);db_close(db);return answer; }
void test_free(void *p) { free(p); }
''')
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "json-c"], text=True))
        binary = Path(cls.build.name) / "groups.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-D_POSIX_C_SOURCE=200809L", "-Dtime=managed_test_time", "-std=c11",
                        "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I" + str(ROOT / "src"),
                        str(ROOT / "src/organization.c"), str(ROOT / "src/storage.c"),
                        str(ROOT / "src/access_control.c"), str(helper),
                        "-o", str(binary), "-lsqlite3", *flags], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(binary))
        cls.lib.organization_open.argtypes = [ctypes.c_char_p]
        cls.lib.organization_open.restype = ctypes.c_bool
        cls.lib.organization_sync_groups.restype = ctypes.c_bool
        cls.lib.organization_groups_managed.restype = ctypes.c_bool
        cls.lib.organization_groups_available.restype = ctypes.c_bool
        cls.lib.organization_handle.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                               ctypes.c_char_p, ctypes.c_longlong, ctypes.c_char_p,
                                               ctypes.POINTER(Response)]
        cls.lib.organization_handle.restype = ctypes.c_bool
        cls.lib.test_access.argtypes = [ctypes.c_char_p, ctypes.c_longlong, ctypes.c_longlong]
        cls.lib.test_access.restype = ctypes.c_int
        cls.lib.test_free.argtypes = [ctypes.c_void_p]
        cls.lib.test_clock.argtypes = [ctypes.c_long]

    def setUp(self):
        self.lib.test_clock(0)
        self.fixture = tempfile.TemporaryDirectory(prefix="punto-managed-groups-")
        self.addCleanup(self.fixture.cleanup)
        self.db = Path(self.fixture.name) / "groups.db"
        with database(self.db) as conn:
            conn.executescript(SCHEMA)
        self.snapshot = Path(self.fixture.name) / "groups.json"
        keys = ("AUTH_GROUP_MANAGEMENT", "AUTH_GROUPS_SNAPSHOT_FILE", "OIDC_ISSUER_URL", "AUTH_USER_MANAGEMENT")
        previous = {key: os.environ.get(key) for key in keys}
        def restore():
            self.lib.organization_close()
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        self.addCleanup(restore)
        os.environ.update(AUTH_GROUP_MANAGEMENT="keycloak", AUTH_USER_MANAGEMENT="keycloak",
                          AUTH_GROUPS_SNAPSHOT_FILE=str(self.snapshot), OIDC_ISSUER_URL=ISSUER)
        self.document = {"version": 1, "issuer": ISSUER, "generated_at": int(time.time()), "groups": [
            {"id": "root-uuid", "parent_id": None, "name": "Engineering", "local_id": 10, "members": []},
            {"id": "child-uuid", "parent_id": "root-uuid", "name": "Design", "local_id": 11, "members": ["worker-sub"]}],
            "user_bindings": {"worker-sub": "worker@example.test"}}

    def save(self, document=None):
        self.snapshot.write_text(json.dumps(self.document if document is None else document))

    def sync(self, document=None):
        self.save(document)
        self.lib.organization_close()
        self.assertTrue(self.lib.organization_open(os.fsencode(self.db)))
        return self.lib.organization_sync_groups()

    def rows(self, sql, params=()):
        with database(self.db) as conn:
            return conn.execute(sql, params).fetchall()

    def api(self, method, path, body=""):
        response = Response()
        handled = self.lib.organization_handle(method.encode(), path.encode(), b"", body.encode(),
                                               1, b"manager", ctypes.byref(response))
        self.assertTrue(handled)
        payload = json.loads(ctypes.string_at(response.body))
        self.lib.test_free(response.body)
        return response.status, payload

    def access(self, job=10, user=2):
        return self.lib.test_access(os.fsencode(self.db), job, user)

    def test_explicit_migration_preserves_ids_grants_and_inherited_access(self):
        self.assertTrue(self.sync())
        self.assertTrue(self.lib.organization_groups_managed())
        self.assertTrue(self.lib.organization_groups_available())
        self.assertEqual(self.rows("SELECT subject,group_id FROM oidc_groups ORDER BY group_id"),
                         [("root-uuid", 10), ("child-uuid", 11)])
        self.assertEqual(self.rows("SELECT group_id FROM job_group_grants"), [(10,)])
        self.assertEqual(self.rows("SELECT subject,user_id FROM oidc_identities"), [("worker-sub", 2)])
        self.assertEqual(self.access(), 3)
        self.assertEqual(self.api("GET", "/api/groups/11/members")[1]["members"][0]["user_id"], 2)

    def test_rename_and_reparent_keep_identity_and_change_inherited_access(self):
        self.assertTrue(self.sync())
        self.document["groups"][0]["name"] = "Renamed"
        self.document["groups"].append({"id": "other-root", "parent_id": None, "name": "New root",
                                        "local_id": None, "members": []})
        self.document["groups"][1]["parent_id"] = "other-root"
        self.assertTrue(self.sync())
        self.assertEqual(self.rows("SELECT name FROM organization_groups WHERE id=10"), [("Renamed",)])
        self.assertEqual(self.rows("SELECT group_id FROM job_group_grants"), [(10,)])
        self.assertEqual(self.access(), 0)
        self.document["groups"][1]["parent_id"] = "root-uuid"
        self.assertTrue(self.sync())
        self.assertEqual(self.access(), 3)

    def test_delete_and_same_name_recreate_cannot_reclaim_historic_grants(self):
        self.assertTrue(self.sync())
        self.document["groups"] = []
        self.assertTrue(self.sync())
        self.assertEqual(self.rows("SELECT COUNT(*) FROM organization_groups"), [(2,)])
        self.assertEqual(self.rows("SELECT group_id FROM job_group_grants"), [(10,)])
        self.assertEqual(self.rows("SELECT COUNT(*) FROM group_members"), [(0,)])
        self.document["groups"] = [{"id": "new-root-uuid", "parent_id": None, "name": "Engineering",
                                    "local_id": None, "members": ["worker-sub"]}]
        self.assertTrue(self.sync())
        new_id = self.rows("SELECT group_id FROM oidc_groups WHERE subject='new-root-uuid'")[0][0]
        self.assertNotEqual(new_id, 10)
        self.assertEqual(self.access(), 0)
        self.assertEqual([g["id"] for g in self.api("GET", "/api/groups")[1]["groups"]], [new_id])
        self.assertEqual(self.api("POST", "/api/jobs/10/group-grants", "group_id=10&role_id=2")[0], 409)

    def test_unknown_subject_has_no_email_takeover_and_appears_immediately_after_jit_link(self):
        self.lib.test_clock(int(time.time()))
        self.document["groups"][1]["members"] = ["unknown-sub"]
        self.document["user_bindings"] = {}
        self.assertTrue(self.sync())
        self.assertEqual(self.rows("SELECT * FROM group_members"), [])
        self.assertEqual(self.access(), 0)
        with database(self.db) as conn:
            conn.execute("INSERT INTO oidc_identities VALUES(?,?,?)", (ISSUER, "unknown-sub", 2))
        self.assertTrue(self.lib.organization_sync_groups())
        self.assertEqual(self.rows("SELECT group_id,user_id FROM group_members"), [(11, 2)])
        self.assertEqual(self.access(), 3)

    def test_explicit_binding_cannot_steal_already_linked_user(self):
        self.assertTrue(self.sync())
        before = self.rows("SELECT group_id,user_id FROM group_members")
        self.document["groups"][1]["members"] = ["attacker-sub"]
        self.document["user_bindings"] = {"attacker-sub": "worker@example.test"}
        self.assertFalse(self.sync())
        self.assertEqual(self.rows("SELECT group_id,user_id FROM group_members"), before)
        self.assertEqual(self.rows("SELECT subject,user_id FROM oidc_identities"), [("worker-sub", 2)])
        self.assertEqual(self.access(), 0)

    def test_partial_invalid_graph_duplicates_stale_and_wrong_issuer_roll_back(self):
        self.assertTrue(self.sync())
        before = self.rows("SELECT id,name,parent_id FROM organization_groups ORDER BY id")
        cases = []
        for change in ("orphan", "cycle", "duplicate", "duplicate-member", "stale", "issuer", "partial", "binding", "local-id"):
            document = copy.deepcopy(self.document)
            if change == "orphan": document["groups"][1]["parent_id"] = "missing"
            elif change == "cycle": document["groups"][0]["parent_id"] = "child-uuid"
            elif change == "duplicate": document["groups"].append(copy.deepcopy(document["groups"][0]))
            elif change == "duplicate-member": document["groups"][1]["members"].append("worker-sub")
            elif change == "stale": document["generated_at"] -= 121
            elif change == "issuer": document["issuer"] += "-other"
            elif change == "partial": del document["groups"][1]["members"]
            elif change == "binding": document["groups"][0]["local_id"] = 11
            elif change == "local-id": document["groups"][0]["local_id"] = True
            cases.append((change, document))
        for change, document in cases:
            with self.subTest(change=change):
                self.assertFalse(self.sync(document))
                self.assertEqual(self.rows("SELECT id,name,parent_id FROM organization_groups ORDER BY id"), before)
                self.assertEqual(self.access(), 0)
                self.assertEqual(self.access(11), 1, "direct grants remain available")
                self.assertEqual(self.access(12), 3, "common jobs remain available")
                self.assertEqual(self.api("GET", "/api/groups")[0], 503)

    def test_first_import_requires_all_existing_group_bindings(self):
        self.document["groups"][1]["local_id"] = None
        self.assertFalse(self.sync())
        self.assertEqual(self.rows("SELECT COUNT(*) FROM oidc_groups"), [(0,)])
        self.assertEqual(self.rows("SELECT group_id,user_id FROM group_members"), [(11, 2)])

    def test_snapshot_bounds_reject_excess_depth_groups_members_and_invalid_names(self):
        self.assertTrue(self.sync())
        before = self.rows("SELECT id,name,parent_id FROM organization_groups ORDER BY id")
        for change in ("groups", "depth", "members", "name", "future", "nul-name"):
            document = copy.deepcopy(self.document)
            if change == "groups":
                document["groups"] += [{"id": "new-" + str(i), "parent_id": None, "name": "New",
                                         "local_id": None, "members": []} for i in range(199)]
            elif change == "depth":
                document["groups"] += [{"id": "deep-" + str(i),
                                         "parent_id": "child-uuid" if i == 0 else "deep-" + str(i - 1),
                                         "name": "Deep", "local_id": None, "members": []} for i in range(15)]
            elif change == "members": document["groups"][1]["members"] = ["sub-" + str(i) for i in range(201)]
            elif change == "name": document["groups"][0]["name"] = "x" * 121
            elif change == "future": document["generated_at"] = int(time.time()) + 60
            elif change == "nul-name": document["groups"][0]["name"] = "Truncated\x00name"
            with self.subTest(change=change):
                self.assertFalse(self.sync(document))
                self.assertEqual(self.rows("SELECT id,name,parent_id FROM organization_groups ORDER BY id"), before)

    def test_bound_group_cannot_change_local_id_or_be_reclaimed_after_deletion(self):
        self.assertTrue(self.sync())
        self.document["groups"][0]["local_id"] = 999
        self.assertFalse(self.sync())
        self.assertEqual(self.rows("SELECT group_id FROM oidc_groups WHERE subject='root-uuid'"), [(10,)])
        self.document["groups"] = []
        self.assertTrue(self.sync())
        self.document["groups"] = [{"id": "new-uuid", "parent_id": None, "name": "Engineering",
                                    "local_id": 10, "members": []}]
        self.assertFalse(self.sync())
        self.assertEqual(self.rows("SELECT subject FROM oidc_groups WHERE group_id=10"), [("root-uuid",)])

    def test_group_crud_blocked_but_local_job_roles_and_grants_remain_operational(self):
        self.assertTrue(self.sync())
        for method, path, body in (("POST", "/api/groups", "name=New"),
                                    ("POST", "/api/groups/10", "name=New"),
                                    ("DELETE", "/api/groups/10", ""),
                                    ("POST", "/api/groups/11/members", "user_id=3"),
                                    ("DELETE", "/api/groups/11/members/2", "")):
            self.assertEqual(self.api(method, path, body)[0], 403)
        self.assertEqual(self.api("POST", "/api/job-roles", "code=reviewer&name=Reviewer&can_log_time=0")[0], 201)
        self.assertEqual(self.api("POST", "/api/jobs/10/group-grants", "group_id=11&role_id=1")[0], 201)
        self.assertEqual(self.api("DELETE", "/api/jobs/10/group-grants/11")[0], 200)

    def test_file_symlink_oversize_nul_and_database_failure_do_not_replace_projection(self):
        self.assertTrue(self.sync())
        before = self.rows("SELECT group_id,user_id FROM group_members")
        for blob in (b"{}\x00", b" " * (4 * 1024 * 1024 + 1), b"{\"version\":"):
            self.lib.organization_close()
            self.snapshot.write_bytes(blob)
            self.assertTrue(self.lib.organization_open(os.fsencode(self.db)))
            self.assertFalse(self.lib.organization_sync_groups())
            self.assertEqual(self.rows("SELECT group_id,user_id FROM group_members"), before)
        target = self.snapshot.with_suffix(".target")
        self.snapshot.rename(target)
        self.snapshot.symlink_to(target)
        self.lib.organization_close()
        self.assertTrue(self.lib.organization_open(os.fsencode(self.db)))
        self.assertFalse(self.lib.organization_sync_groups())
        self.snapshot.unlink()
        with database(self.db) as conn:
            conn.execute("CREATE TRIGGER deny_sync BEFORE INSERT ON audit_events BEGIN SELECT RAISE(ABORT,'audit failure'); END")
        self.document["groups"][0]["name"] = "Must roll back"
        self.assertFalse(self.sync())
        self.assertEqual(self.rows("SELECT name FROM organization_groups WHERE id=10"), [("Engineering",)])

    def test_invalid_managed_configuration_fails_and_local_mode_is_noop(self):
        os.environ["AUTH_USER_MANAGEMENT"] = "local"
        self.assertFalse(self.lib.organization_open(os.fsencode(self.db)))
        os.environ["AUTH_GROUP_MANAGEMENT"] = "local"
        self.assertTrue(self.lib.organization_open(os.fsencode(self.db)))
        self.assertTrue(self.lib.organization_sync_groups())
        self.assertTrue(self.lib.organization_groups_available())
        self.assertFalse(self.lib.organization_groups_managed())
        self.assertEqual(self.access(), 3)


if __name__ == "__main__":
    unittest.main()
