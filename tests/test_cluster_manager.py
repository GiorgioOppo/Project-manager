"""Safe cluster recovery decisions with a fake controller, without Docker."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import tempfile
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
    network_spec = importlib.util.spec_from_file_location("punto_network_prepare_tests", SCRIPTS / "configure_network.py")
    network = importlib.util.module_from_spec(network_spec)
    network_spec.loader.exec_module(network)
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

    def require_service_migration_complete(self):
        # Recovery decision tests use no daemon; the guard is covered separately.
        pass

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
        self.assertEqual(cluster.events[0], ("docker", ("up", "-d", "mysql-1", "mysql-2", "mysql-3")))

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


class ClusterServiceNamingTests(unittest.TestCase):
    def test_sql_uses_service_name_without_rewriting_query_or_node_identity(self):
        cluster = manager.Cluster.__new__(manager.Cluster)
        cluster.run = mock.Mock(return_value=mock.Mock(returncode=0, stdout=b"1\n"))
        query = "SELECT 'db', 'db2', 'db-proxy', 'documents';"
        for node, service in (("db", "mysql-1"), ("db2", "mysql-2"), ("db3", "mysql-3")):
            with self.subTest(node=node):
                self.assertEqual(cluster.sql(node, query), "1")
                arguments = cluster.run.call_args.args[0]
                self.assertEqual(arguments[:5], ("exec", "-T", service, "sh", "-c"))
                self.assertEqual(cluster.run.call_args.kwargs["data"], query.encode("utf-8"))

    def test_backup_targets_renamed_node_and_keeps_dump_stream(self):
        cluster = manager.Cluster.__new__(manager.Cluster)
        cluster.require_service_migration_complete = mock.Mock(side_effect=AssertionError("Read-only backup must not run lifecycle guard"))
        calls = []
        def capture(arguments, *, output):
            calls.append(arguments)
            output.write(b"-- preserved backup fixture\n")
        cluster.run = capture
        with tempfile.TemporaryDirectory(prefix="punto-cluster-backup-names-") as temporary:
            with mock.patch.object(manager, "PROJECT", Path(temporary)), contextlib.redirect_stdout(io.StringIO()):
                destination = cluster.backup("db2")
            self.assertEqual(calls[0][:5], ("exec", "-T", "mysql-2", "sh", "-c"))
            self.assertEqual(destination.read_bytes(), b"-- preserved backup fixture\n")
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)

    def test_recovery_keeps_persisted_seed_identities(self):
        original = {"group_id": manager.GROUP_ID, "initialized": True,
                    "source_uuid": UUID_A, "seeded": {"db2": UUID_B, "db3": UUID_A}}
        cluster = FakeCluster(states={node: "ONLINE" for node in manager.NODES})
        with tempfile.TemporaryDirectory(prefix="punto-cluster-state-names-") as temporary:
            state_path = Path(temporary) / "cluster-state.json"
            state_path.write_text(json.dumps(original), encoding="utf-8")
            with mock.patch.object(manager, "STATE_PATH", state_path), contextlib.redirect_stdout(io.StringIO()):
                cluster.recover()
            self.assertEqual(json.loads(state_path.read_text(encoding="utf-8")), original)
        self.assertEqual(cluster.events[0], ("docker", ("up", "-d", "mysql-1", "mysql-2", "mysql-3")))
        self.assertEqual(cluster.events[-1], ("start_services",))

    def test_app_start_targets_current_proxy_services(self):
        cluster = manager.Cluster.__new__(manager.Cluster)
        cluster.run = mock.Mock()
        cluster.require_service_migration_complete = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            cluster.start_services()
        cluster.require_service_migration_complete.assert_called_once_with()
        cluster.run.assert_called_once_with(("up", "--build", "-d", "proxy-db", "app", "proxy-web"), timeout=600)


class ServiceMigrationGuardTests(unittest.TestCase):
    def cluster(self):
        cluster = manager.Cluster.__new__(manager.Cluster)
        cluster.command = ["docker", "--context", "fixture", "compose", "--env-file", "unused"]
        cluster.environment = {}
        config = {"name": "projectmanament", "services": {"app": {"environment": {"SECRET": "never-print-this"}}}}
        cluster.run = mock.Mock(return_value=mock.Mock(stdout=json.dumps(config).encode("utf-8")))
        return cluster

    def test_same_project_legacy_containers_block_including_stopped_ones(self):
        cluster = self.cluster()
        result = mock.Mock(returncode=0, stdout=b"projectmanament\tdb\nprojectmanament\tnginx-proxy-manager\n")
        with mock.patch.object(manager.subprocess, "run", return_value=result) as docker:
            with self.assertRaisesRegex(manager.ClusterError, "Container legacy") as error:
                cluster.require_service_migration_complete()
        self.assertIn("db", str(error.exception))
        self.assertNotIn("never-print-this", str(error.exception))
        arguments = docker.call_args.args[0]
        self.assertEqual(arguments[:4], ["docker", "--context", "fixture", "ps"])
        self.assertIn("--all", arguments)
        self.assertIn("label=com.docker.compose.project=projectmanament", arguments)
        self.assertNotIn("rm", arguments)
        cluster.run.assert_called_once_with(("config", "--format", "json"), timeout=30)

    def test_only_current_services_allow_start(self):
        cluster = self.cluster()
        cluster.tickets = True
        services = ("app", "mysql-1", "mysql-2", "mysql-3", "proxy-db", "proxy-web",
                    "proxy-egress", "mongodb", "mail", "calendar", "inventory", "tickets")
        result = mock.Mock(returncode=0, stdout="".join("projectmanament\t" + name + "\n" for name in services).encode("ascii"))
        with mock.patch.object(manager.subprocess, "run", return_value=result):
            cluster.require_service_migration_complete()

    def test_another_project_with_legacy_names_does_not_interfere(self):
        cluster = self.cluster()
        result = mock.Mock(returncode=0, stdout=b"otherproject\tdb\notherproject\tsquid\nprojectmanament\tmysql-1\n")
        with mock.patch.object(manager.subprocess, "run", return_value=result):
            cluster.require_service_migration_complete()

    def test_missing_project_or_unavailable_daemon_fails_closed(self):
        cluster = self.cluster()
        cluster.run.return_value.stdout = b'{"services":{"app":{"SECRET":"never-print-this"}}}'
        with mock.patch.object(manager.subprocess, "run") as docker:
            with self.assertRaisesRegex(manager.ClusterError, "Impossibile verificare"):
                cluster.require_service_migration_complete()
            docker.assert_not_called()
        cluster = self.cluster()
        with mock.patch.object(manager.subprocess, "run", return_value=mock.Mock(returncode=1, stdout=b"")):
            with self.assertRaisesRegex(manager.ClusterError, "Impossibile verificare"):
                cluster.require_service_migration_complete()

    def test_guard_precedes_every_starting_lifecycle_operation(self):
        for action in ("init", "recover", "start_services"):
            with self.subTest(action=action):
                cluster = self.cluster()
                cluster.require_service_migration_complete = mock.Mock(side_effect=manager.ClusterError("migration required"))
                with mock.patch.object(manager, "load_state") as load:
                    with self.assertRaisesRegex(manager.ClusterError, "migration required"):
                        getattr(cluster, action)()
                    load.assert_not_called()
                cluster.run.assert_not_called()

    def test_network_apply_checks_legacy_before_preparing_or_mutating(self):
        cluster = mock.Mock()
        cluster.require_service_migration_complete.side_effect = network.ClusterError("migration required")
        with mock.patch.object(network, "load_state", return_value={"initialized": True}), \
             mock.patch.object(network, "Cluster", return_value=cluster), \
             mock.patch.object(network, "prepare") as prepare:
            with self.assertRaisesRegex(network.ClusterError, "migration required"):
                network.apply(Path("unused"), "fixture")
            prepare.assert_not_called()
            cluster.run.assert_not_called()

    def test_status_does_not_require_lifecycle_migration(self):
        cluster = self.cluster()
        cluster.require_service_migration_complete = mock.Mock(side_effect=AssertionError("Status must not run lifecycle guard"))
        cluster.sql = mock.Mock(return_value="db\tONLINE")
        with contextlib.redirect_stdout(io.StringIO()):
            cluster.status()
        self.assertEqual(cluster.sql.call_count, 3)
        cluster.require_service_migration_complete.assert_not_called()



class InventoryPreparationTests(unittest.TestCase):
    def test_inventory_token_is_created_once_and_preserves_other_secrets(self):
        with tempfile.TemporaryDirectory(prefix="punto-inventory-secret-test-") as temporary:
            directory = Path(temporary) / "secrets"
            directory.mkdir()
            originals = {"mail-api-token": "a" * 64 + "\n",
                         "calendar-api-token": "b" * 64 + "\n",
                         "calendar-token-key": "c" * 64 + "\n",
                         "smtp-password": "original-smtp-secret",
                         "google-client-secret": "original-google-secret"}
            for name, value in originals.items():
                (directory / name).write_text(value, encoding="ascii")
            with mock.patch.object(network, "ADDON_SECRETS", directory):
                network.prepare_addon_secrets()
                token = (directory / "inventory-api-token").read_bytes()
                self.assertRegex(token.decode("ascii"), r"\A[0-9a-f]{64}\n\Z")
                network.prepare_addon_secrets()
                self.assertEqual((directory / "inventory-api-token").read_bytes(), token)
            for name, value in originals.items():
                self.assertEqual((directory / name).read_text(encoding="ascii"), value)
            self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
            self.assertEqual((directory / "inventory-api-token").stat().st_mode & 0o777, 0o444)

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL is required for certificate fixtures")
    def test_inventory_certificate_extends_existing_ca_without_rotation(self):
        with tempfile.TemporaryDirectory(prefix="punto-inventory-tls-test-") as temporary:
            directory = Path(temporary)
            network.openssl(("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
                             "-keyout", directory / "ca.key", "-out", directory / "ca.crt",
                             "-subj", "/CN=Inventory Fixture CA",
                             "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
                             "-addext", "keyUsage=critical,keyCertSign,cRLSign"))
            original = {path.name: path.read_bytes() for path in directory.iterdir()}
            network.prepare_inventory_certificate(directory)
            cert, key = directory / "inventory-addon.crt", directory / "inventory-addon.key"
            issued = (cert.read_bytes(), key.read_bytes())
            network.prepare_inventory_certificate(directory)
            self.assertEqual((cert.read_bytes(), key.read_bytes()), issued)
            for name, contents in original.items():
                self.assertEqual((directory / name).read_bytes(), contents)
            self.assertEqual(key.stat().st_mode & 0o777, 0o444)
            self.assertEqual(cert.stat().st_mode & 0o777, 0o644)

    def test_partial_inventory_certificate_is_not_replaced(self):
        with tempfile.TemporaryDirectory(prefix="punto-inventory-partial-test-") as temporary:
            directory = Path(temporary)
            key = directory / "inventory-addon.key"
            key.write_bytes(b"original key")
            with self.assertRaisesRegex(network.ClusterError, "incompleto"):
                network.prepare_inventory_certificate(directory)
            self.assertEqual(key.read_bytes(), b"original key")
            self.assertFalse((directory / "inventory-addon.crt").exists())

    def test_existing_invalid_inventory_token_is_not_rotated(self):
        with tempfile.TemporaryDirectory(prefix="punto-inventory-invalid-test-") as temporary:
            directory = Path(temporary) / "secrets"
            directory.mkdir()
            token = directory / "inventory-api-token"
            token.write_bytes(b"invalid original")
            with mock.patch.object(network, "ADDON_SECRETS", directory):
                with self.assertRaisesRegex(network.ClusterError, "esistente non valido"):
                    network.prepare_addon_secrets()
            self.assertEqual(token.read_bytes(), b"invalid original")


if __name__ == "__main__":
    unittest.main()
