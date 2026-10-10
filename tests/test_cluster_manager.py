"""Safe cluster recovery decisions with a fake controller, without Docker."""

import contextlib
import importlib.util
import io
from pathlib import Path
import re
import sys
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))
try:
    spec = importlib.util.spec_from_file_location("punto_cluster_manager_tests", SCRIPTS / "manage_cluster.py")
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
finally:
    sys.path.pop(0)

UUID_A = "11111111-1111-1111-1111-111111111111"
UUID_B = "22222222-2222-2222-2222-222222222222"


def fixture_gtids(value):
    """Expand only small fixture GTID sets to emulate MySQL's GTID_SUBSET."""
    transactions = set()
    for component in filter(None, value.split(",")):
        server, *intervals = component.split(":")
        for interval in intervals:
            bounds = interval.split("-")
            start = int(bounds[0])
            end = int(bounds[-1])
            transactions.update((server, number) for number in range(start, end + 1))
    return transactions


class FakeCluster(manager.Cluster):
    def __init__(self, executed=None, certified=None, states=None, stop_refuses=()):
        self.executed = {node: "" for node in manager.NODES}
        self.executed.update(executed or {})
        self.certified = {node: "" for node in manager.NODES}
        self.certified.update(certified or {})
        self.states = {node: "OFFLINE" for node in manager.NODES}
        self.states.update(states or {})
        self.stop_refuses = set(stop_refuses)
        self.events = []

    def run(self, arguments, **kwargs):
        self.events.append(("docker", tuple(arguments)))

    def wait_sql(self):
        self.events.append(("wait_sql",))

    def verify_config(self):
        self.events.append(("verify_config",))

    def local_state(self, node):
        return self.states[node]

    def sql(self, node, query, **kwargs):
        if query == "STOP GROUP_REPLICATION;":
            self.events.append(("stop_requested", node))
            if node in self.stop_refuses:
                return None
            self.states[node] = "OFFLINE"
            return ""
        if "SELECT COUNT(*) FROM performance_schema.replication_connection_status" in query and "SERVICE_STATE" in query:
            self.events.append(("stop_verified", node))
            return "1" if node in self.stop_refuses else "0"
        if query == "SELECT @@GLOBAL.gtid_executed;":
            self.events.append(("executed", node))
            return self.executed[node]
        if "SELECT RECEIVED_TRANSACTION_SET" in query:
            self.events.append(("certified", node))
            return self.certified[node]
        match = re.fullmatch(r"SELECT GTID_SUBSET\('([^']*)','([^']*)'\);", query)
        if match:
            self.events.append(("subset", node))
            return "1" if fixture_gtids(match[1]) <= fixture_gtids(match[2]) else "0"
        raise AssertionError("Unexpected controller SQL in fake test")

    def start_group(self, node, *, bootstrap=False):
        self.events.append(("start", node, bootstrap))
        self.states[node] = "ONLINE"

    def wait_online(self, expected=3):
        self.events.append(("wait_online",))
        if sum(value == "ONLINE" for value in self.states.values()) < expected:
            raise AssertionError("Controller waited before starting enough fake nodes")

    def status(self):
        self.events.append(("status",))

    def start_services(self):
        self.events.append(("start_services",))


class ClusterRecoveryTests(unittest.TestCase):
    def recover(self, cluster):
        with mock.patch.object(manager, "load_state", return_value={
                "group_id": manager.GROUP_ID, "initialized": True}), contextlib.redirect_stdout(io.StringIO()):
            cluster.recover()

    def test_bootstrap_uses_superset_node_instead_of_first_node(self):
        cluster = FakeCluster(executed={
            "db": UUID_A + ":1-5",
            "db2": UUID_A + ":1-9," + UUID_B + ":1-2",
            "db3": UUID_A + ":1-7",
        })
        self.recover(cluster)
        starts = [event for event in cluster.events if event[0] == "start"]
        self.assertEqual(starts[0], ("start", "db2", True))
        self.assertEqual({event[1] for event in starts}, set(manager.NODES))
        self.assertEqual(sum(event[2] for event in starts), 1)
        self.assertEqual(cluster.events[-1], ("start_services",))

    def test_certified_unapplied_transactions_determine_superset(self):
        cluster = FakeCluster(executed={
            "db": UUID_A + ":1-5", "db2": UUID_A + ":1-4", "db3": UUID_A + ":1-5",
        }, certified={"db2": UUID_A + ":5-9"})
        self.recover(cluster)
        starts = [event for event in cluster.events if event[0] == "start"]
        self.assertEqual(starts[0], ("start", "db2", True))
        self.assertEqual(sum(event[2] for event in starts), 1)

    def test_divergent_sets_block_bootstrap_and_application_start(self):
        cluster = FakeCluster(executed={
            "db": UUID_A + ":1-8",
            "db2": UUID_A + ":1-5," + UUID_B + ":1",
            "db3": UUID_A + ":1-5",
        })
        with self.assertRaisesRegex(manager.ClusterError, "GTID divergenti"):
            self.recover(cluster)
        self.assertFalse(any(event[0] == "start" for event in cluster.events))
        self.assertNotIn(("start_services",), cluster.events)

    def test_existing_online_group_is_joined_without_bootstrap(self):
        cluster = FakeCluster(states={"db": "ONLINE", "db2": "RECOVERING"})
        self.recover(cluster)
        starts = [event for event in cluster.events if event[0] == "start"]
        self.assertEqual(starts, [("start", "db2", False), ("start", "db3", False)])
        self.assertNotIn(("stop_requested", "db"), cluster.events)
        self.assertFalse(any(event[0] in ("executed", "certified", "subset") for event in cluster.events))

    def test_all_members_are_stopped_and_verified_before_gtid_collection(self):
        cluster = FakeCluster(executed={node: UUID_A + ":1-5" for node in manager.NODES})
        self.recover(cluster)
        first_read = next(index for index, event in enumerate(cluster.events)
                          if event[0] in ("executed", "certified"))
        preceding = cluster.events[:first_read]
        for node in manager.NODES:
            self.assertIn(("stop_requested", node), preceding)
            self.assertIn(("stop_verified", node), preceding)

    def test_failed_stop_blocks_gtid_selection_bootstrap_and_application_start(self):
        cluster = FakeCluster(states={"db2": "RECOVERING"}, stop_refuses={"db2"})
        with self.assertRaisesRegex(manager.ClusterError, "non arrestato su db2"):
            self.recover(cluster)
        self.assertFalse(any(event[0] in ("executed", "certified", "subset", "start")
                             for event in cluster.events))
        self.assertNotIn(("start_services",), cluster.events)


if __name__ == "__main__":
    unittest.main()
