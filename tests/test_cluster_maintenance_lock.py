"""CLI maintenance exclusion with fake Docker, SQL and guest lock holders.

These tests exercise the real controller methods but never invoke Docker or
connect to MySQL. Local host flock checks use only a disposable directory.
"""

import contextlib
import fcntl
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    spec = importlib.util.spec_from_file_location(
        "punto_cluster_maintenance_tests", ROOT / "scripts/manage_cluster.py")
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
    spec = importlib.util.spec_from_file_location(
        "punto_network_maintenance_tests", ROOT / "scripts/configure_network.py")
    network = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(network)
finally:
    sys.path.pop(0)


OFF = "SET GLOBAL group_replication_bootstrap_group=OFF;"
ON = "SET GLOBAL group_replication_bootstrap_group=ON;"
START = "START GROUP_REPLICATION;"


class FakeGuestLock:
    def __init__(self):
        self.lost = False
        self.checks = 0
        self.releases = 0

    def assert_held(self):
        self.checks += 1
        if self.lost:
            raise manager.GuestLockError("guest_lock_lost")

    def release(self):
        self.releases += 1


class MaintenanceCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-maintenance-command-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.cluster = object.__new__(manager.Cluster)
        self.cluster.command = ["fake-docker", "--context", "isolated-fixture", "compose"]
        self.cluster.environment = {"PATH": "/fixture"}
        self.cluster.guest_lock = FakeGuestLock()

    def response(self):
        return subprocess.CompletedProcess([], 0, stdout=b"1\n", stderr=b"")

    def test_lost_lock_blocks_command_even_when_error_checking_disabled(self):
        self.cluster.guest_lock.lost = True
        with mock.patch.object(manager.subprocess, "run") as run:
            with self.assertRaises(manager.ClusterError):
                self.cluster.run(("up", "-d", "mysql-1"), check=False)
        run.assert_not_called()

    def test_lock_loss_during_command_stops_following_mutations(self):
        def completed(*args, **kwargs):
            self.cluster.guest_lock.lost = True
            return self.response()
        with mock.patch.object(manager.subprocess, "run", side_effect=completed) as run:
            with self.assertRaises(manager.ClusterError):
                self.cluster.run(("stop", "app"), check=False)
            with self.assertRaises(manager.ClusterError):
                self.cluster.sql("db", START)
        self.assertEqual(run.call_count, 1)

    def test_only_exact_bootstrap_off_can_bypass_lost_lock(self):
        self.cluster.guest_lock.lost = True
        with mock.patch.object(manager.subprocess, "run", return_value=self.response()) as run:
            self.assertEqual(self.cluster.sql("db", OFF), "1")
            self.assertEqual(run.call_args.kwargs["input"], OFF.encode())
            for query in (ON, START, OFF + ON, "SELECT 1;" + OFF):
                with self.subTest(query=query):
                    with self.assertRaises(manager.ClusterError):
                        self.cluster.sql("db", query, check=False)
        self.assertEqual(run.call_count, 1)

    def test_lock_loss_after_successful_bootstrap_on_still_attempts_off(self):
        queries = []
        def completed(*args, **kwargs):
            query = kwargs["input"].decode()
            queries.append(query)
            if query == ON:
                self.cluster.guest_lock.lost = True
            return self.response()
        with mock.patch.object(manager.subprocess, "run", side_effect=completed):
            with self.assertRaises(manager.ClusterError):
                self.cluster.start_group("db", bootstrap=True)
        self.assertEqual(queries, [ON, OFF])

    def test_lock_loss_during_bootstrap_start_still_attempts_off(self):
        queries = []
        def completed(*args, **kwargs):
            query = kwargs["input"].decode()
            queries.append(query)
            if query == START:
                self.cluster.guest_lock.lost = True
            return self.response()
        with mock.patch.object(manager.subprocess, "run", side_effect=completed):
            with self.assertRaises(manager.ClusterError):
                self.cluster.start_group("db", bootstrap=True)
        self.assertEqual(queries, [ON, START, OFF])

    def test_release_clears_holder_and_is_idempotent(self):
        guest = self.cluster.guest_lock
        self.cluster.release_maintenance_lock()
        self.cluster.release_maintenance_lock()
        self.assertEqual(guest.releases, 1)
        self.assertIsNone(self.cluster.guest_lock)

    def test_independent_guest_lock_survives_final_service_up(self):
        events = []
        guest = self.cluster.guest_lock
        guest.release = lambda: events.append("release")
        self.cluster.require_service_migration_complete = lambda: events.append("verified")
        self.cluster.tickets = True
        def completed(command, **kwargs):
            self.assertIs(self.cluster.guest_lock, guest)
            events.append(tuple(command[len(self.cluster.command):]))
            return self.response()
        with mock.patch.object(manager.subprocess, "run", side_effect=completed), \
                contextlib.redirect_stdout(io.StringIO()):
            self.cluster.start_services()
        self.assertEqual(events, ["verified", (
            "up", "--build", "-d", "proxy-db", "app", "proxy-web", "tickets")])
        self.assertGreaterEqual(guest.checks, 2)

    def test_enable_holds_independent_guest_lock_through_recreation_and_tls_check(self):
        events = []
        self.cluster.require_service_migration_complete = lambda: events.append("verified")
        self.cluster.backup = lambda: events.append("backup")
        self.cluster.configure_autorecovery = lambda: events.append("configured")
        guest = self.cluster.guest_lock
        guest.release = lambda: events.append("release")
        def completed(command, **kwargs):
            self.assertIs(self.cluster.guest_lock, guest)
            events.append(tuple(command[len(self.cluster.command):]))
            return self.response()
        with mock.patch.object(manager.subprocess, "run", side_effect=completed), \
                contextlib.redirect_stdout(io.StringIO()):
            self.cluster.enable_autorecovery()
        self.assertEqual(events, ["verified", "backup", "configured", (
            "up", "--build", "--force-recreate", "--no-deps", "-d", "cluster-recovery"), (
            "exec", "-T", "cluster-recovery", "python3", "/app/recovery.py", "check")])
        self.assertGreaterEqual(guest.checks, 4)


class MaintenanceAcquisitionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-maintenance-acquire-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.state_path = self.project / "data/cluster-state.json"
        self.state_path.parent.mkdir()
        self.lock_path = self.state_path.parent / "cluster-controller.lock"
        self.lock_path.touch()
        self.cluster = object.__new__(manager.Cluster)
        self.cluster.command = ["fake-docker", "--context", "isolated-fixture", "compose"]
        self.cluster.environment = {"PATH": "/fixture"}
        self.cluster.guest_lock = None
        self.container_id = "a" * 64
        self.image = "sha256:" + "b" * 64
        self.info = {"Image": self.image, "State": {"Status": "restarting"},
                     "Config": {"Labels": {"com.docker.compose.service": "cluster-recovery"}},
                     "Mounts": [{"Type": "bind", "Destination": "/run/punto-cluster.lock",
                                 "Source": str(self.lock_path)}]}

    def responses(self, info):
        return [subprocess.CompletedProcess([], 0, (self.container_id + "\n").encode(), b""),
                subprocess.CompletedProcess([], 0, json.dumps([info]).encode(), b"")]

    def test_stopped_or_restarting_controller_uses_real_image_and_same_guest_bind(self):
        for state in ("exited", "restarting", "running"):
            with self.subTest(state=state):
                self.cluster.guest_lock = None
                self.info["State"]["Status"] = state
                helper = mock.Mock(spec=manager.GuestClusterLock)
                helper.acquire.return_value = helper
                with mock.patch.object(manager, "STATE_PATH", self.state_path), \
                        mock.patch.object(manager.subprocess, "run", side_effect=self.responses(self.info)) as run, \
                        mock.patch.object(manager, "GuestClusterLock", return_value=helper) as factory:
                    self.cluster.acquire_maintenance_lock()
                    # Re-entrant callers check the existing holder instead of
                    # starting a second competing helper.
                    self.cluster.acquire_maintenance_lock()
                factory.assert_called_once_with(self.lock_path.resolve(),
                    docker_command=self.cluster.command[:3], image=self.image)
                self.assertEqual(run.call_args_list[0].args[0][-4:],
                                 ["ps", "--all", "--quiet", "cluster-recovery"])
                self.assertEqual(run.call_args_list[1].args[0],
                    self.cluster.command[:3] + ["inspect", self.container_id])
                helper.acquire.assert_called_once_with()
                helper.assert_held.assert_called_once_with()
                self.assertIs(self.cluster.guest_lock, helper)

    def test_wrong_image_mount_or_identity_blocks_before_helper_or_mutating_command(self):
        variants = []
        for key, value in (("Image", "punto/cluster-recovery:local"),
                           ("Config", {"Labels": {"com.docker.compose.service": "different-service"}}),
                           ("Mounts", [{"Type": "bind", "Destination": "/run/punto-cluster.lock",
                                        "Source": str(self.project / "wrong.lock")}]),
                           ("Mounts", [{"Type": "volume", "Destination": "/run/punto-cluster.lock",
                                        "Source": str(self.lock_path)}])):
            variants.append({**self.info, key: value})
        for info in variants:
            with self.subTest(info=info):
                with mock.patch.object(manager, "STATE_PATH", self.state_path), \
                        mock.patch.object(manager.subprocess, "run", side_effect=self.responses(info)) as run, \
                        mock.patch.object(manager, "GuestClusterLock") as factory:
                    with self.assertRaises(manager.ClusterError):
                        self.cluster.acquire_maintenance_lock()
                factory.assert_not_called()
                self.assertIsNone(self.cluster.guest_lock)
                self.assertEqual(run.call_count, 2)

    def test_acquisition_failure_is_closed_and_does_not_install_a_holder(self):
        helper = mock.Mock()
        helper.acquire.side_effect = manager.GuestLockError("guest_lock_not_acquired")
        with mock.patch.object(manager, "STATE_PATH", self.state_path), \
                mock.patch.object(manager.subprocess, "run", side_effect=self.responses(self.info)), \
                mock.patch.object(manager, "GuestClusterLock", return_value=helper):
            with self.assertRaises(manager.ClusterError):
                self.cluster.acquire_maintenance_lock()
        self.assertIsNone(self.cluster.guest_lock)


class MaintenanceEntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-maintenance-entrypoint-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.state_path = self.project / "data/cluster-state.json"

    def host_lock_is_held(self):
        host_path = self.state_path.parent / "cluster-host.lock"
        with host_path.open("a") as competitor:
            with self.assertRaises(BlockingIOError):
                fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Host and Linux guest locks must be different files. Otherwise a
        # native Linux CLI self-deadlocks when its helper tries to take flock.
        guest_path = self.state_path.parent / "cluster-controller.lock"
        guest_path.touch(exist_ok=True)
        with guest_path.open("a") as guest:
            fcntl.flock(guest, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertNotEqual(host_path.stat().st_ino, guest_path.stat().st_ino)

    def invoke(self, action, fail=False, fail_acquire=False):
        events = []
        cluster = mock.Mock()
        def acquire():
            self.host_lock_is_held()
            events.append("acquire")
            if fail_acquire:
                raise manager.ClusterError("synthetic lock acquisition failure")
        def release():
            self.host_lock_is_held()
            events.append("release")
        cluster.acquire_maintenance_lock.side_effect = acquire
        cluster.release_maintenance_lock.side_effect = release
        def dispatched():
            events.append(action)
            if fail:
                raise manager.ClusterError("synthetic action failure")
        getattr(cluster, action.replace("-", "_")).side_effect = dispatched
        with mock.patch.object(manager, "PROJECT", self.project), \
                mock.patch.object(manager, "STATE_PATH", self.state_path), \
                mock.patch.object(manager, "ensure_secrets"), \
                mock.patch.object(manager, "Cluster", return_value=cluster), \
                mock.patch.object(sys, "argv", ["manage_cluster.py", action]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = manager.main()
        # Both success and an action error must release the host flock too.
        with (self.state_path.parent / "cluster-host.lock").open("a") as competitor:
            fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return result, events

    def test_mutating_actions_acquire_host_then_guest_before_dispatch(self):
        for action in ("init", "recover", "start", "enable-autorecovery"):
            with self.subTest(action=action):
                result, events = self.invoke(action)
                self.assertEqual(result, 0)
                self.assertEqual(events, ["acquire", action, "release"])

    def test_mutating_action_errors_release_guest_and_host_locks(self):
        for action in ("init", "recover", "start", "enable-autorecovery"):
            with self.subTest(action=action):
                result, events = self.invoke(action, fail=True)
                self.assertEqual(result, 1)
                self.assertEqual(events, ["acquire", action, "release"])

    def test_acquisition_failure_never_dispatches_and_releases_before_host_unlock(self):
        result, events = self.invoke("recover", fail_acquire=True)
        self.assertEqual(result, 1)
        self.assertEqual(events, ["acquire", "release"])

    def test_network_entrypoint_uses_separate_host_mutex_before_apply(self):
        with mock.patch.object(network, "STATE_PATH", self.state_path), \
                mock.patch.object(network, "apply", side_effect=lambda *args, **kwargs: self.host_lock_is_held()) as apply, \
                mock.patch.object(sys, "argv", ["configure_network.py", "apply"]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(network.main(), 0)
        apply.assert_called_once()
        with (self.state_path.parent / "cluster-host.lock").open("a") as competitor:
            fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_host_fifo_is_rejected_before_opening_or_constructing_controller(self):
        self.state_path.parent.mkdir()
        host_path = self.state_path.parent / "cluster-host.lock"
        os.mkfifo(host_path, 0o640)
        before = host_path.lstat()
        with mock.patch.object(manager, "STATE_PATH", self.state_path), \
                mock.patch.object(sys, "argv", ["manage_cluster.py", "recover"]), \
                mock.patch.object(manager, "Cluster") as controller, \
                mock.patch.object(Path, "open", side_effect=AssertionError("must not open FIFO")) as opened, \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(manager.main(), 1)
        self.assertIn("File lock del cluster non regolare", errors.getvalue())
        controller.assert_not_called()
        opened.assert_not_called()
        self.assertEqual((host_path.lstat().st_mode, host_path.lstat().st_ino),
                         (before.st_mode, before.st_ino))


class NetworkMaintenanceTests(unittest.TestCase):
    def test_network_apply_holds_guest_lock_through_every_mutating_step_and_error(self):
        for failure in (None, "acquire", "guard", "prepare", "engine", "stop", "up", "recover"):
            with self.subTest(failure=failure):
                events = []
                cluster = mock.Mock()
                def phase(name):
                    def performed(*args, **kwargs):
                        events.append(name)
                        if failure == name:
                            raise network.ClusterError("synthetic " + name + " failure")
                    return performed
                cluster.acquire_maintenance_lock.side_effect = phase("acquire")
                cluster.require_service_migration_complete.side_effect = phase("guard")
                cluster.release_maintenance_lock.side_effect = phase("release")
                cluster.local_state.return_value = "ONLINE"
                cluster.backup.side_effect = phase("backup")
                cluster.recover.side_effect = phase("recover")
                def run(arguments, **kwargs):
                    phase(arguments[0])()
                cluster.run.side_effect = run
                with mock.patch.object(network, "load_state", return_value={"initialized": True}), \
                        mock.patch.object(network, "Cluster", return_value=cluster), \
                        mock.patch.object(network, "prepare", side_effect=phase("prepare")), \
                        mock.patch.object(network, "check_engine", side_effect=phase("engine")), \
                        contextlib.redirect_stdout(io.StringIO()):
                    if failure is None:
                        network.apply(Path("isolated.env"), "isolated-context")
                    else:
                        with self.assertRaises(network.ClusterError):
                            network.apply(Path("isolated.env"), "isolated-context")
                expected = ["acquire", "guard", "prepare", "engine", "stop", "backup", "up", "recover"]
                if failure is not None:
                    expected = expected[:expected.index(failure) + 1]
                self.assertEqual(events, expected + ["release"])


if __name__ == "__main__":
    unittest.main()
