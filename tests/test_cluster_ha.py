"""Opt-in local HA checks. Pauses/restarts nodes and restores the cluster.

Run explicitly: PUNTO_CLUSTER_HA_TEST=1 python3 tests/test_cluster_ha.py -v
Creates a disposable replication probe and named codex-cluster-* job fixtures.
Never deletes application records or existing volumes.
"""

from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
import unittest
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    from manage_cluster import Cluster, NODES
finally:
    sys.path.pop(0)


@unittest.skipUnless(os.environ.get("PUNTO_CLUSTER_HA_TEST") == "1",
                     "Set PUNTO_CLUSTER_HA_TEST=1 to run local failover tests")
class ClusterHATests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        context = os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else "")
        cls.cluster = Cluster(Path(os.environ.get("PUNTO_CLUSTER_ENV_FILE", ROOT / ".env.docker")), context)
        cls.schema = "`" + cls.cluster.database + "`"
        cls.http_port = int(cls.cluster.values.get("NPM_HTTP_PORT", "80"))
        cls.host = "localhost" + (":" + str(cls.http_port) if cls.http_port != 80 else "")
        cls.origin = "http://" + cls.host

    def request(self, method, path, fields=None, cookie=None):
        headers = {"Host": self.host, "Accept": "application/json"}
        if cookie:
            headers["Cookie"] = cookie
        body = None
        if fields is not None:
            body = urlencode(fields)
            headers.update({"Content-Type": "application/x-www-form-urlencoded", "Origin": self.origin})
        connection = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=25)
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            raw = response.read()
            return response.status, json.loads(raw), response.getheader("Set-Cookie", "")
        finally:
            connection.close()

    def wait_health(self, expected, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                status, data, _ = self.request("GET", "/healthz")
                if status == expected:
                    return data
            except (OSError, ValueError, http.client.HTTPException):
                pass
            time.sleep(1)
        self.fail("Application health did not reach HTTP " + str(expected))

    def assert_all_writers(self):
        self.cluster.wait_online()
        for node in NODES:
            ready = self.cluster.sql(node, "SELECT CONCAT(MEMBER_STATE,':',MEMBER_ROLE,':',"
                "@@read_only,':',@@super_read_only) FROM performance_schema.replication_group_members "
                "WHERE MEMBER_ID=@@server_uuid;")
            self.assertEqual(ready, "ONLINE:PRIMARY:0:0", node)

    def login(self):
        values = self.cluster.values
        status, data, cookie = self.request("POST", "/api/login", {
            "email": values["PUNTO_EMAIL"], "password": values["PUNTO_PASSWORD"]})
        self.assertEqual(status, 200)
        self.assertTrue(data.get("ok"))
        self.assertTrue(cookie)
        return cookie.split(";", 1)[0]

    def create_job(self, cookie, suffix):
        status, data, _ = self.request("POST", "/api/jobs", {
            "code": "codex-cluster-" + secrets.token_hex(6),
            "name": "Test cluster " + suffix, "status": "planned"}, cookie)
        self.assertEqual(status, 201, "A write through the surviving nodes failed")
        self.assertIsInstance(data.get("id"), int)
        return data["id"]

    def application_counts(self):
        return self.cluster.sql("db", f"SELECT COUNT(*) FROM {self.schema}.users UNION ALL "
            f"SELECT COUNT(*) FROM {self.schema}.jobs UNION ALL SELECT COUNT(*) FROM {self.schema}.time_entries "
            f"UNION ALL SELECT COUNT(*) FROM {self.schema}.audit_events;")

    def test_01_every_primary_writes_and_conflicting_transactions_roll_back(self):
        self.assert_all_writers()
        table = "cluster_probe_" + secrets.token_hex(6)
        qualified = "`" + self.cluster.database + "`." + table
        self.cluster.sql("db", "CREATE TABLE " + qualified +
            "(id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,origin VARCHAR(20) NOT NULL) ENGINE=InnoDB;")
        try:
            ids = []
            for node in NODES:
                result = self.cluster.sql(node, f"START TRANSACTION; UPDATE {self.schema}.cluster_write_guard "
                    "SET generation=generation+1 WHERE id=1; INSERT INTO " + qualified +
                    "(origin) VALUES('" + node + "'); COMMIT; SELECT LAST_INSERT_ID();")
                ids.append(int(result))
            self.assertEqual(len(set(ids)), 3)
            self.assertEqual({number % 3 for number in ids}, {0, 1, 2})

            def conflicting_write(node):
                query = f"START TRANSACTION; UPDATE {self.schema}.cluster_write_guard SET generation=generation+1 WHERE id=1; "
                query += "INSERT INTO " + qualified + "(origin) VALUES('race-" + node + "'); DO SLEEP(2); COMMIT;"
                return self.cluster.run(("exec", "-T", node, "sh", "-c",
                    'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --user=root --batch --skip-column-names'),
                    data=query.encode(), check=False, timeout=30)

            with ThreadPoolExecutor(max_workers=2) as pool:
                outcomes = list(pool.map(conflicting_write, ("db", "db2")))
            self.assertEqual(sum(result.returncode == 0 for result in outcomes), 1)
            rejected = next(result for result in outcomes if result.returncode)
            self.assertIsNotNone(re.search(rb"ERROR 3101\b", rejected.stderr),
                                 "The conflicting transaction was not a certified rollback")
            for node in NODES:
                self.assertEqual(self.cluster.sql(node, "SELECT COUNT(*) FROM " + qualified + ";"), "4")
            print("Tre writer e rollback certificato senza duplicazioni: OK", flush=True)
        finally:
            # This table and every row in it were created solely by this test.
            self.cluster.sql("db", "DROP TABLE " + qualified + ";")

    def test_02_single_failure_majority_loss_and_rejoin(self):
        self.assert_all_writers()
        self.wait_health(200)
        cookie = self.login()
        paused = False
        try:
            print("Arresto del primo nodo: verifica scrittura con due nodi…", flush=True)
            self.cluster.run(("stop", "db"))
            self.wait_health(200)
            job_id = self.create_job(cookie, "due nodi")
            for node in ("db2", "db3"):
                self.assertEqual(self.cluster.sql(node, f"SELECT COUNT(*) FROM {self.schema}.jobs WHERE id=" + str(job_id) + ";"), "1")
            self.cluster.run(("up", "-d", "db"))
            self.assert_all_writers()
            self.wait_health(200)
            self.assertEqual(self.cluster.sql("db", f"SELECT COUNT(*) FROM {self.schema}.jobs WHERE id=" + str(job_id) + ";"), "1")
            print("Rientro automatico del nodo e recupero della scrittura: OK", flush=True)

            print("Pausa di due nodi: verifica protezione dalla perdita di maggioranza…", flush=True)
            self.cluster.run(("pause", "db2", "db3"))
            paused = True
            self.wait_health(503)
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                if self.cluster.sql("db", "SELECT @@super_read_only;") == "1":
                    break
                time.sleep(1)
            else:
                self.fail("The minority member was not fenced read-only")
            status, _, _ = self.request("POST", "/api/jobs", {
                "code": "codex-cluster-blocked-" + secrets.token_hex(4),
                "name": "Must not commit", "status": "planned"}, cookie)
            self.assertEqual(status, 503)
            print("Minoranza in sola lettura e app HTTP 503: OK", flush=True)
        finally:
            if paused:
                self.cluster.run(("unpause", "db2", "db3"), check=False)
            self.cluster.recover()
        self.assert_all_writers()
        self.wait_health(200)
        self.create_job(self.login(), "recupero maggioranza")
        print("Cluster ripristinato e scrittura tramite app: OK", flush=True)

    def test_03_complete_outage_recovers_without_losing_records(self):
        self.assert_all_writers()
        before = self.application_counts()
        print("Arresto completo: verifica recupero sicuro dai GTID…", flush=True)
        try:
            self.cluster.run(("stop", "app"))
            self.cluster.run(("stop", *NODES))
        finally:
            self.cluster.recover()
        self.assert_all_writers()
        self.wait_health(200)
        self.assertEqual(self.application_counts(), before)
        self.login()
        print("Recupero completo, conteggi invariati e login: OK", flush=True)


if __name__ == "__main__":
    unittest.main()
