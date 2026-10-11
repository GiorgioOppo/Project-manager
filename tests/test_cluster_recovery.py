"""Automatic recovery safety with synthetic SQL, clocks, and temporary journals.

No Docker calls, real database connections, application writes, or deployment
credentials are used. The fixture models delayed joins rather than making every
START immediately healthy.
"""
import copy
import contextlib
import fcntl
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "punto_automatic_cluster_recovery_tests", ROOT / "docker/cluster-recovery/recovery.py")
recovery = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = recovery
SPEC.loader.exec_module(recovery)

NODES = ("db", "db2", "db3")
UUIDS = {node: str(index) * 8 + "-" + str(index) * 4 + "-" + str(index) * 4 + "-" + str(index) * 4 + "-" + str(index) * 12
         for index, node in enumerate(NODES, 1)}
GROUP = "8de71c6b-d3bc-4287-9a2a-edf8436340b1"
MUTATION = re.compile(r"\b(?:SET|START|STOP|RESET|CREATE|ALTER|DROP|TRUNCATE|INSERT|UPDATE|DELETE|CHANGE)\b", re.I)
FORBIDDEN = re.compile(r"\b(?:RESET|CREATE|ALTER|DROP|TRUNCATE|INSERT|UPDATE|DELETE|CHANGE)\b", re.I)


def transactions(value):
    """Independent small-set oracle for fixture GTID_SUBSET responses."""
    result = set()
    for component in filter(None, value.split(",")):
        server, *intervals = component.split(":")
        for interval in intervals:
            bounds = interval.split("-")
            result.update((server, identifier) for identifier in range(int(bounds[0]), int(bounds[-1]) + 1))
    return result


class SQLFixture:
    def __init__(self):
        self.nodes = {node: {
            "server_uuid": UUIDS[node], "server_id": index,
            "group_name": GROUP, "single_primary_mode": 0, "gtid_mode": "ON",
            "enforce_update_everywhere": 1, "read_only": 1, "super_read_only": 1,
            "offline_mode": 0, "bootstrap": 0, "force_members": "",
            "local_state": "OFFLINE", "view_id": "", "members": [],
            "active_channels": 0, "executed": UUIDS["db"] + ":1-5", "certified": "",
        } for index, node in enumerate(NODES, 1)}
        self.calls = []
        self.unreachable = set()
        self.fail_start = set()
        self.fail_off = set()
        self.lose_on_ack = set()
        self.after_subset = None
        self.before_on = None
        self.clear_certified_on_stop = set()

    def healthy(self, online=NODES, *, view="view:1"):
        members = [{"id": UUIDS[node], "state": "ONLINE", "role": "PRIMARY"} for node in online]
        for node in NODES:
            self.nodes[node].update(local_state="ONLINE" if node in online else "OFFLINE",
                                    read_only=0 if node in online else 1,
                                    super_read_only=0 if node in online else 1,
                                    active_channels=1 if node in online else 0,
                                    view_id=view if node in online else "",
                                    members=copy.deepcopy(members) if node in online else [])

    def mutations(self):
        return [(node, query) for node, query in self.calls if MUTATION.search(query)]

    def __call__(self, node, query):
        self.calls.append((node, query))
        if node in self.unreachable:
            raise recovery.RecoveryError("synthetic SQL endpoint unavailable")
        record = self.nodes[node]
        if query == recovery.SNAPSHOT_SQL:
            return json.dumps(record)
        match = re.fullmatch(r"\s*SELECT GTID_SUBSET\('([^']*)',\s*'([^']*)'\);?\s*", query)
        if match:
            result = "1" if transactions(match[1]) <= transactions(match[2]) else "0"
            if self.after_subset:
                self.after_subset()
            return result
        if query == recovery.STOP_SQL:
            record.update(local_state="OFFLINE", active_channels=0, read_only=1, super_read_only=1,
                          members=[], view_id="")
            if node in self.clear_certified_on_stop:
                record["certified"] = ""
            return ""
        if query == recovery.BOOTSTRAP_ON_SQL:
            if self.before_on:
                self.before_on(node)
            record["bootstrap"] = 1
            if node in self.lose_on_ack:
                raise recovery.RecoveryError("synthetic ON acknowledgement lost")
            return ""
        if query == recovery.BOOTSTRAP_OFF_SQL:
            if node in self.fail_off:
                raise recovery.RecoveryError("synthetic OFF unavailable")
            record["bootstrap"] = 0
            return ""
        if query == recovery.START_SQL:
            if node in self.fail_start:
                raise recovery.RecoveryError("synthetic START failed")
            founder = bool(record["bootstrap"])
            record.update(local_state="ONLINE" if founder else "RECOVERING", active_channels=1,
                          read_only=0 if founder else 1, super_read_only=0 if founder else 1,
                          view_id="view:recovered",
                          members=[{"id": UUIDS[node], "state": "ONLINE", "role": "PRIMARY"}] if founder else [])
            return ""
        raise AssertionError("Unexpected SQL in isolated recovery test: " + query)


class AutomaticClusterRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="punto-automatic-recovery-")
        self.addCleanup(self.files.cleanup)
        self.directory = Path(self.files.name)
        self.sql = SQLFixture()
        self.now = 1000.0
        self.state = {"initialized": True, "group_id": GROUP,
                      "source_uuid": UUIDS["db"], "seeded": {node: UUIDS[node] for node in NODES[1:]},
                      "nodes": dict(UUIDS)}
        self.monitor = self.new_monitor()

    def new_monitor(self):
        return recovery.Recovery(sql=self.sql, state=copy.deepcopy(self.state), now=lambda: self.now,
                                 sleep=lambda seconds: self.fail("tick must not sleep"),
                                 lock_path=self.directory / "cluster-controller.lock",
                                 journal_path=self.directory / "recovery-journal.json",
                                 status_path=self.directory / "recovery-status.json", stable_seconds=10)

    def tearDown(self):
        for _, query in self.sql.calls:
            self.assertIsNone(FORBIDDEN.search(query), "Automatic recovery must not migrate/import/reset data")
            self.assertNotRegex(query, r"(?i)\b(?:PERSIST|group_replication_force_members)\b.*=")

    def warm_up(self):
        first = self.monitor.tick()
        self.assertFalse(first["ready"])
        self.assertEqual(self.sql.mutations(), [])
        self.now += 10

    def bootstrap_calls(self):
        return [(node, query) for node, query in self.sql.calls if query == recovery.BOOTSTRAP_ON_SQL]

    def test_healthy_poll_has_no_sql_mutations_and_preserves_user_state(self):
        self.sql.healthy()
        original = copy.deepcopy(self.state)
        for _ in range(3):
            self.assertTrue(self.monitor.tick()["ready"])
            self.now += 15
        self.assertEqual(self.sql.mutations(), [])
        self.assertEqual(self.state, original)

    def test_cold_restart_waits_ten_seconds_and_bootstraps_certified_gtid_superset_once(self):
        self.sql.nodes["db2"].update(executed=UUIDS["db"] + ":1-4", certified=UUIDS["db"] + ":5-9")
        journals = []
        self.sql.before_on = lambda node: journals.append((node, json.loads((self.directory / "recovery-journal.json").read_text())))
        self.assertFalse(self.monitor.tick()["ready"])
        self.now += 9.999
        self.monitor.tick()
        self.assertEqual(self.sql.mutations(), [])
        self.now += 0.001
        self.monitor.tick()
        self.assertEqual(self.bootstrap_calls(), [("db2", recovery.BOOTSTRAP_ON_SQL)])
        starts = [(node, query) for node, query in self.sql.calls if query == recovery.START_SQL]
        self.assertEqual(starts[0], ("db2", recovery.START_SQL))
        self.assertEqual({node for node, _ in starts}, set(NODES))
        self.assertEqual(journals[0][1]["phase"], "bootstrap_pending")
        self.assertEqual(journals[0][1]["server_uuid"], UUIDS["db2"])
        self.assertTrue(all(record["bootstrap"] == 0 for record in self.sql.nodes.values()))
        self.monitor.tick()  # peers are still RECOVERING: do not reset their join.
        self.assertEqual(len(self.bootstrap_calls()), 1)
        self.sql.healthy(view="view:recovered")
        self.assertTrue(self.monitor.tick()["ready"])
        mutations = list(self.sql.mutations())
        self.assertTrue(self.monitor.tick()["ready"])
        self.assertEqual(self.sql.mutations(), mutations)

    def test_stop_clearing_channel_instrumentation_does_not_erase_certified_candidate_gtids(self):
        self.sql.nodes["db2"].update(executed=UUIDS["db"] + ":1-4", certified=UUIDS["db"] + ":5-9")
        self.sql.clear_certified_on_stop.add("db2")
        self.warm_up()
        self.monitor.tick()
        self.assertEqual(self.sql.nodes["db2"]["certified"], "")
        self.assertEqual(self.bootstrap_calls(), [("db2", recovery.BOOTSTRAP_ON_SQL)])
        self.assertEqual(next(node for node, query in self.sql.calls if query == recovery.START_SQL), "db2")

    def test_read_only_check_and_cli_work_under_maintenance_lock_without_creating_files(self):
        self.sql.healthy()
        path = self.directory / "cluster-controller.lock"
        with path.open("a") as manual_controller:
            fcntl.flock(manual_controller, fcntl.LOCK_EX | fcntl.LOCK_NB)
            before = {item.name: item.read_bytes() for item in self.directory.iterdir()}
            with mock.patch.object(recovery.fcntl, "flock", side_effect=AssertionError("check must not acquire a lock")):
                self.assertTrue(self.monitor.check())
            self.assertEqual({node for node, _ in self.sql.calls}, set(NODES))
            self.assertEqual(self.sql.mutations(), [])
            self.assertEqual({item.name: item.read_bytes() for item in self.directory.iterdir()}, before)

            state_path = self.directory / "state.json"
            state_path.write_text(json.dumps(self.state))
            environment = {"CLUSTER_STATE_FILE": str(state_path), "CLUSTER_LOCK_FILE": str(path),
                           "CLUSTER_JOURNAL_FILE": str(self.directory / "recovery-journal.json"),
                           "CLUSTER_STATUS_FILE": str(self.directory / "recovery-status.json")}
            before = {item.name: item.read_bytes() for item in self.directory.iterdir()}
            with mock.patch.dict(os.environ, environment), mock.patch.object(sys, "argv", ["recovery.py", "check"]), \
                 mock.patch.object(recovery, "SqlClient", return_value=self.sql), \
                 mock.patch.object(recovery.Recovery, "tick", side_effect=AssertionError("CLI check must not run recovery")), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(recovery.main(), 0)
            self.assertEqual(self.sql.mutations(), [])
            self.assertEqual({item.name: item.read_bytes() for item in self.directory.iterdir()}, before)
            self.sql.unreachable.add("db3")
            with self.assertRaises(recovery.RecoveryError):
                self.monitor.check()  # Administrative check verifies all three real SQL endpoints.
            self.assertEqual(self.sql.mutations(), [])

    def test_only_the_single_verified_offline_empty_identity_placeholder_is_normalized(self):
        for record in self.sql.nodes.values():
            record["members"] = [{"id": "", "state": "OFFLINE", "role": ""}]
        self.warm_up()
        self.monitor.tick()
        self.assertEqual(self.bootstrap_calls(), [("db", recovery.BOOTSTRAP_ON_SQL)])
        for invalid in ("wrong_server_uuid", "duplicate_placeholder", "online_placeholder", "nonempty_role"):
            with self.subTest(invalid=invalid):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                self.sql.nodes["db2"]["members"] = [{"id": "", "state": "OFFLINE", "role": ""}]
                if invalid == "wrong_server_uuid":
                    self.sql.nodes["db2"]["server_uuid"] = UUIDS["db3"]
                elif invalid == "duplicate_placeholder":
                    self.sql.nodes["db2"]["members"].append({"id": "", "state": "OFFLINE", "role": ""})
                elif invalid == "online_placeholder":
                    self.sql.nodes["db2"]["members"][0]["state"] = "ONLINE"
                else:
                    self.sql.nodes["db2"]["members"][0]["role"] = "PRIMARY"
                self.assertFalse(self.monitor.tick()["ready"])
                self.assertEqual(self.sql.mutations(), [])

    def test_two_consistent_writers_remain_healthy_with_third_sql_unreachable_without_mutations(self):
        for membership in (("db", "db2"), NODES):
            with self.subTest(membership=membership):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                self.sql.healthy(membership)
                self.sql.unreachable.add("db3")
                result = self.monitor.tick()
                self.assertTrue(result["ready"])
                self.assertEqual(result["online"], 2)
                self.assertEqual(result["state"], "degraded")
                self.assertEqual(self.sql.mutations(), [])
                self.assertTrue(recovery.healthz(self.directory / "recovery-status.json", now=lambda: self.now))
                self.sql.nodes["db2"]["view_id"] = "conflicting:view"
                self.assertFalse(self.monitor.tick()["ready"])
                self.assertEqual(self.sql.mutations(), [])

    def test_unreachable_recovering_or_single_online_member_never_bootstraps(self):
        for condition in ("unreachable", "recovering", "one_online"):
            with self.subTest(condition=condition):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                if condition == "unreachable":
                    self.sql.unreachable.add("db3")
                elif condition == "recovering":
                    self.sql.nodes["db3"]["local_state"] = "RECOVERING"
                else:
                    self.sql.healthy(("db",))
                self.monitor.tick()
                self.now += 60
                self.assertFalse(self.monitor.tick()["ready"])
                self.assertEqual(self.bootstrap_calls(), [])
                self.assertEqual(self.sql.mutations(), [])

    def test_unreachable_poll_resets_the_full_outage_stability_window(self):
        self.monitor.tick()
        self.now += 20
        self.sql.unreachable.add("db3")
        self.monitor.tick()
        self.sql.unreachable.clear()
        self.now += 1
        self.monitor.tick()
        self.now += 9
        self.monitor.tick()
        self.assertEqual(self.bootstrap_calls(), [])
        self.now += 1
        self.monitor.tick()
        self.assertEqual(len(self.bootstrap_calls()), 1)

    def test_divergent_executed_or_certified_sets_block_bootstrap(self):
        for field in ("executed", "certified"):
            with self.subTest(field=field):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                self.sql.nodes["db"]["executed"] = UUIDS["db"] + ":1-8"
                self.sql.nodes["db2"][field] = UUIDS["db2"] + ":1"
                self.warm_up()
                result = self.monitor.tick()
                self.assertFalse(result["ready"])
                self.assertEqual(self.bootstrap_calls(), [])
                self.assertFalse(any(query == recovery.START_SQL for _, query in self.sql.calls))
                self.assertTrue(result["reason"])

    def test_rejoin_requires_two_online_members_in_the_same_membership_view(self):
        self.sql.healthy(("db", "db2"))
        self.monitor.tick()
        self.assertEqual(self.sql.mutations(), [("db3", recovery.START_SQL)])
        self.assertEqual(self.bootstrap_calls(), [])
        self.assertEqual(self.sql.nodes["db3"]["local_state"], "RECOVERING")
        self.monitor.tick()
        self.assertEqual(self.sql.mutations(), [("db3", recovery.START_SQL)])

    def test_conflicting_online_views_never_rejoin_or_bootstrap(self):
        self.sql.healthy(("db", "db2"))
        self.sql.nodes["db2"]["view_id"] = "different:view"
        self.monitor.tick()
        self.now += 60
        self.assertFalse(self.monitor.tick()["ready"])
        self.assertEqual(self.sql.mutations(), [])

    def test_online_members_with_inconsistent_membership_do_not_count_as_quorum(self):
        self.sql.healthy(("db", "db2"))
        self.sql.nodes["db2"]["members"][0]["state"] = "UNREACHABLE"
        self.monitor.tick()
        self.now += 60
        self.assertFalse(self.monitor.tick()["ready"])
        self.assertEqual(self.sql.mutations(), [])

    def test_gtid_change_after_candidate_selection_aborts_before_bootstrap(self):
        def concurrent_certification():
            self.sql.nodes["db3"]["certified"] = UUIDS["db"] + ":6"
        self.sql.after_subset = concurrent_certification
        self.warm_up()
        result = self.monitor.tick()
        self.assertFalse(result["ready"])
        self.assertEqual(result["reason"], "state_changed_before_bootstrap")
        self.assertEqual(self.bootstrap_calls(), [])
        self.assertFalse(any(query == recovery.START_SQL for _, query in self.sql.calls))

    def test_wrong_group_or_server_identity_blocks_before_mutations(self):
        for field, value in (("group_name", UUIDS["db3"]), ("server_uuid", UUIDS["db3"]),
                             ("gtid_mode", "OFF"), ("single_primary_mode", 1)):
            with self.subTest(field=field):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                self.sql.nodes["db2"][field] = value
                self.monitor.tick()
                self.now += 60
                self.assertFalse(self.monitor.tick()["ready"])
                self.assertEqual(self.sql.mutations(), [])

    def test_manual_controller_lock_blocks_every_mutation(self):
        path = self.directory / "cluster-controller.lock"
        with path.open("a") as manual_controller:
            fcntl.flock(manual_controller, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.monitor.tick()
            self.now += 60
            self.assertFalse(self.monitor.tick()["ready"])
            self.assertEqual(self.sql.mutations(), [])
        self.monitor.tick()
        self.assertEqual(self.sql.mutations(), [])
        self.now += 10
        self.monitor.tick()
        self.assertEqual(len(self.bootstrap_calls()), 1)

    def test_bootstrap_start_failure_always_disables_the_flag(self):
        self.sql.fail_start.add("db")
        self.warm_up()
        self.assertFalse(self.monitor.tick()["ready"])
        changes = self.sql.mutations()
        self.assertIn(("db", recovery.BOOTSTRAP_ON_SQL), changes)
        self.assertIn(("db", recovery.BOOTSTRAP_OFF_SQL), changes)
        self.assertEqual(self.sql.nodes["db"]["bootstrap"], 0)
        self.assertFalse(any(node != "db" and query == recovery.START_SQL for node, query in changes))

    def test_lost_bootstrap_on_ack_is_cleaned_up_before_any_start(self):
        self.sql.lose_on_ack.add("db")
        self.warm_up()
        self.assertFalse(self.monitor.tick()["ready"])
        self.assertEqual(self.sql.nodes["db"]["bootstrap"], 0)
        self.assertIn(("db", recovery.BOOTSTRAP_OFF_SQL), self.sql.mutations())
        self.assertFalse(any(query == recovery.START_SQL for _, query in self.sql.calls))

    def test_pending_journal_survives_failed_cleanup_and_recovers_after_monitor_restart(self):
        self.sql.fail_off.add("db")
        self.warm_up()
        self.assertFalse(self.monitor.tick()["ready"])
        path = self.directory / "recovery-journal.json"
        journal = json.loads(path.read_text())
        self.assertEqual(journal["node"], "db")
        self.assertEqual(journal["server_uuid"], UUIDS["db"])
        self.assertEqual(self.sql.nodes["db"]["bootstrap"], 1)
        original_bootstraps = len(self.bootstrap_calls())
        self.monitor = self.new_monitor()
        self.monitor.tick()
        self.assertEqual(len(self.bootstrap_calls()), original_bootstraps)
        self.sql.fail_off.clear()
        self.sql.calls.clear()
        self.monitor.tick()
        self.assertEqual(self.sql.nodes["db"]["bootstrap"], 0)
        self.assertEqual(self.bootstrap_calls(), [])
        self.assertFalse(any(query == recovery.STOP_SQL for _, query in self.sql.calls))

    def test_journal_for_a_replaced_server_cannot_authorize_rejoining_a_single_writer(self):
        self.sql.healthy(("db",))
        (self.directory / "recovery-journal.json").write_text(json.dumps({
            "version": 1, "phase": "joining", "node": "db",
            "server_uuid": UUIDS["db3"], "group_id": GROUP,
        }))
        result = self.monitor.tick()
        self.assertFalse(result["ready"])
        self.assertEqual(result["reason"], "invalid_recovery_journal")
        self.assertEqual(self.sql.mutations(), [])

    def test_offline_pending_or_joining_journal_blocks_another_bootstrap_even_after_restart(self):
        for phase in ("bootstrap_pending", "joining"):
            with self.subTest(phase=phase):
                self.sql = SQLFixture()
                self.monitor = self.new_monitor()
                self.monitor._save_journal("db", phase)
                path = self.directory / "recovery-journal.json"
                journal = path.read_bytes()
                self.sql.nodes["db"]["bootstrap"] = 1
                # Even a previously established stability window must not
                # authorize bootstrapping over the interrupted operation.
                self.monitor.offline_since = self.now - 60
                self.monitor.offline_signature = tuple(
                    (node, self.sql.nodes[node]["executed"], self.sql.nodes[node]["certified"])
                    for node in NODES)
                for restarted in (False, False, True):
                    if restarted:
                        self.monitor = self.new_monitor()
                    result = self.monitor.tick()
                    self.assertFalse(result["ready"])
                    self.assertEqual(result["state"], "blocked")
                    self.assertEqual(result["reason"], "bootstrap_outcome_unknown")
                    self.assertEqual(path.read_bytes(), journal)
                    self.assertIsNone(self.monitor.offline_since)
                    self.assertIsNone(self.monitor.offline_signature)
                    self.assertTrue(all(record["bootstrap"] == 0 for record in self.sql.nodes.values()))
                    self.assertTrue(all(query == recovery.BOOTSTRAP_OFF_SQL
                                        for _, query in self.sql.mutations()))
                    self.assertTrue(all(query in (recovery.SNAPSHOT_SQL, recovery.BOOTSTRAP_OFF_SQL)
                                        for _, query in self.sql.calls))
                    self.now += 60

    def test_pending_or_joining_journal_can_resume_a_verified_online_founder(self):
        for phase in ("bootstrap_pending", "joining"):
            with self.subTest(phase=phase):
                self.sql = SQLFixture()
                self.sql.healthy(("db2",))
                self.sql.nodes["db2"]["bootstrap"] = 1
                self.monitor = self.new_monitor()
                self.monitor._save_journal("db2", phase)
                path = self.directory / "recovery-journal.json"
                journal = path.read_bytes()
                result = self.monitor.tick()
                self.assertEqual(result["reason"], "resuming_verified_bootstrap")
                self.assertEqual(result["online"], 1)
                self.assertFalse(result["ready"])
                self.assertEqual(path.read_bytes(), journal)
                self.assertEqual([(node, query) for node, query in self.sql.calls
                                  if query == recovery.START_SQL],
                                 [("db", recovery.START_SQL), ("db3", recovery.START_SQL)])
                self.assertEqual(self.bootstrap_calls(), [])
                self.assertFalse(any(query == recovery.STOP_SQL for _, query in self.sql.calls))
                self.assertTrue(all(record["bootstrap"] == 0 for record in self.sql.nodes.values()))
                self.sql.healthy(view="view:recovered")
                self.assertTrue(self.monitor.tick()["ready"])
                self.assertFalse(path.exists())

    def test_resume_revalidates_the_entire_founder_view_before_every_peer_start(self):
        for condition in ("unreachable", "error", "empty_view", "not_primary"):
            # Observation 1 precedes journal cleanup; observation 2 confirms
            # its founder. Race at 3 targets the first START, at 4 the second.
            for changed_at in (3, 4):
                with self.subTest(condition=condition, changed_at=changed_at):
                    self.sql = SQLFixture()
                    self.sql.healthy(("db",))
                    self.monitor = self.new_monitor()
                    self.monitor._save_journal("db", "joining")
                    observations = 0

                    def changing_view(node, query):
                        nonlocal observations
                        if node == "db" and query == recovery.SNAPSHOT_SQL:
                            observations += 1
                            if observations == changed_at:
                                founder = self.sql.nodes["db"]
                                if condition in ("unreachable", "error"):
                                    founder["members"].append({"id": UUIDS["db3"],
                                                               "state": condition.upper(), "role": ""})
                                elif condition == "empty_view":
                                    founder["view_id"] = ""
                                else:
                                    founder["members"][0]["role"] = "SECONDARY"
                        return self.sql(node, query)

                    self.monitor.sql = changing_view
                    result = self.monitor.tick()
                    self.assertFalse(result["ready"])
                    self.assertEqual(result["reason"], "state_changed_before_join")
                    self.assertEqual(observations, changed_at)
                    starts = [(node, query) for node, query in self.sql.calls if query == recovery.START_SQL]
                    self.assertEqual(starts, [] if changed_at == 3 else [("db2", recovery.START_SQL)])
                    self.assertEqual(self.bootstrap_calls(), [])
                    self.assertFalse(any(query == recovery.STOP_SQL for _, query in self.sql.calls))
                    self.assertTrue(all(record["bootstrap"] == 0 for record in self.sql.nodes.values()))
                    self.assertEqual(json.loads((self.directory / "recovery-journal.json").read_text())["phase"], "joining")

    def test_health_expires_and_never_accepts_a_future_or_minority_observation(self):
        self.sql.healthy()
        self.monitor.tick()
        path = self.directory / "recovery-status.json"
        self.assertTrue(recovery.healthz(path, now=lambda: self.now))
        self.assertFalse(recovery.healthz(path, now=lambda: self.now + 31))
        self.assertFalse(recovery.healthz(path, now=lambda: self.now - 1))
        status = json.loads(path.read_text())
        status["online"] = 1
        path.write_text(json.dumps(status))
        self.assertFalse(recovery.healthz(path, now=lambda: self.now))


class RecoverySQLTransportTests(unittest.TestCase):
    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="punto-recovery-sql-")
        self.addCleanup(self.files.cleanup)
        self.password = "synthetic-password-never-log"
        self.secret = Path(self.files.name) / "password"
        self.secret.write_text(self.password + "\n")
        self.ca = Path(self.files.name) / "synthetic-ca.crt"
        self.ca.write_text("synthetic public trust fixture\n")
        self.client = recovery.SqlClient(password_file=self.secret, ca_file=self.ca)

    def test_transport_verifies_tls_identity_and_keeps_password_out_of_command(self):
        with mock.patch.object(recovery.subprocess, "run", return_value=mock.Mock(
                returncode=0, stdout=b"1\n", stderr=b"")) as run:
            self.assertEqual(self.client("db2", "SELECT 1;"), "1")
        command = run.call_args.args[0]
        self.assertIn("--ssl-verify-server-cert", command)
        self.assertIn("--ssl-ca=" + str(self.ca), command)
        self.assertIn("--host=db2", command)
        self.assertIn("--no-defaults", command)
        self.assertNotIn(self.password, " ".join(command))
        self.assertEqual(run.call_args.kwargs["env"]["MYSQL_PWD"], self.password)
        self.assertEqual(run.call_args.kwargs["input"], b"SELECT 1;")
        self.assertGreater(run.call_args.kwargs["timeout"], 0)

    def test_sql_failure_reports_only_numeric_code_not_upstream_secrets(self):
        diagnostic = ("ERROR 2013: " + self.password + " private SQL details").encode()
        with mock.patch.object(recovery.subprocess, "run", return_value=mock.Mock(
                returncode=1, stdout=b"", stderr=diagnostic)):
            with self.assertRaises(recovery.RecoveryError) as caught:
                self.client("db", "SELECT 1;")
        self.assertEqual(str(caught.exception), "sql_failed_2013")
        self.assertNotIn(self.password, str(caught.exception))

    def test_unknown_node_and_malformed_secret_never_invoke_the_sql_client(self):
        with mock.patch.object(recovery.subprocess, "run") as run:
            with self.assertRaises(recovery.RecoveryError):
                self.client("other-server", "SELECT 1;")
            self.secret.write_text("line one\nline two\n")
            with self.assertRaises(recovery.RecoveryError):
                self.client("db", "SELECT 1;")
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
