"""Optional deployment selection and downgrade prevention without Docker."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    spec = importlib.util.spec_from_file_location("punto_cluster_overlay_tests", ROOT / "scripts/manage_cluster.py")
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
    network_spec = importlib.util.spec_from_file_location("punto_network_overlay_tests", ROOT / "scripts/configure_network.py")
    network = importlib.util.module_from_spec(network_spec)
    network_spec.loader.exec_module(network)
finally:
    sys.path.pop(0)


class ClusterOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="punto-cluster-overlays-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name).resolve()
        (self.project / "ticket-app").mkdir()
        (self.project / "data/keycloak").mkdir(parents=True)
        self.env = self.project / ".env.docker"
        self.env.write_text("MYSQL_DATABASE=punto\nPUNTO_IMAGE_TAG=base\n")
        (self.project / "ticket-app/.env").write_text("TICKETS_PUBLIC_ORIGIN=https://ticket.localhost\nPUNTO_IMAGE_TAG=tickets\n")
        (self.project / "data/keycloak/compose.env").write_text("KEYCLOAK_AUTH_MODE=hybrid\nPUNTO_IMAGE_TAG=keycloak\n")
        self.patch = mock.patch.object(manager, "PROJECT", self.project)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        binary = mock.patch.object(manager.shutil, "which", return_value="/fixture/docker")
        binary.start()
        self.addCleanup(binary.stop)

    def selection(self, arguments, option):
        return [arguments[index + 1] for index, value in enumerate(arguments) if value == option]

    def test_existing_configuration_files_do_not_implicitly_activate_overlays(self):
        cluster = manager.Cluster(self.env, "fixture-context")
        self.assertEqual(cluster.command[:4], ["/fixture/docker", "--context", "fixture-context", "compose"])
        self.assertEqual(self.selection(cluster.command, "--env-file"), [str(self.env)])
        self.assertEqual(self.selection(cluster.command, "-f"), [str(self.project / "compose.yaml")])
        self.assertNotIn("KEYCLOAK_AUTH_MODE", cluster.values)
        self.assertNotIn("TICKETS_PUBLIC_ORIGIN", cluster.values)

    def test_overlay_and_environment_order_matches_the_supported_compose_model(self):
        combinations = [(False, True, ["compose.yaml", "compose.tickets.yaml"], "tickets"),
                        (True, False, ["compose.yaml", "compose.keycloak.yaml"], "keycloak"),
                        (True, True, ["compose.yaml", "compose.tickets.yaml", "compose.keycloak.yaml",
                                      "compose.tickets-keycloak.yaml"], "keycloak")]
        for keycloak, tickets, files, tag in combinations:
            with self.subTest(keycloak=keycloak, tickets=tickets):
                cluster = manager.Cluster(self.env, "", keycloak=keycloak, tickets=tickets)
                self.assertEqual(self.selection(cluster.command, "-f"), [str(self.project / name) for name in files])
                env_files = [str(self.env)]
                if tickets:
                    env_files.append(str(self.project / "ticket-app/.env"))
                if keycloak:
                    env_files.append(str(self.project / "data/keycloak/compose.env"))
                self.assertEqual(self.selection(cluster.command, "--env-file"), env_files)
                self.assertEqual(cluster.values["PUNTO_IMAGE_TAG"], tag)

    def test_selected_missing_environment_fails_without_docker_or_file_changes(self):
        before = self.env.read_bytes()
        missing = self.project / "missing.env"
        with mock.patch.object(manager.subprocess, "run") as docker:
            with self.assertRaisesRegex(manager.ClusterError, "overlay non trovato"):
                manager.Cluster(self.env, "", keycloak=True, keycloak_env_file=missing)
            docker.assert_not_called()
        self.assertEqual(self.env.read_bytes(), before)
        self.assertFalse(missing.exists())

    def test_custom_environment_paths_require_selection_and_preserve_order(self):
        tickets = self.project / "custom-tickets.env"
        keycloak = self.project / "custom-keycloak.env"
        tickets.write_text("PUNTO_IMAGE_TAG=custom-tickets\n")
        keycloak.write_text("PUNTO_IMAGE_TAG=custom-keycloak\n")
        cluster = manager.Cluster(self.env, "", tickets=True, keycloak=True,
                                  tickets_env_file=tickets, keycloak_env_file=keycloak)
        self.assertEqual(self.selection(cluster.command, "--env-file"), [str(self.env), str(tickets), str(keycloak)])
        for flag in ("--tickets-env-file", "--keycloak-env-file"):
            with self.subTest(flag=flag), mock.patch.object(manager, "Cluster") as controller, \
                 mock.patch.object(sys, "argv", ["manage_cluster.py", flag, str(tickets), "status"]), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(manager.main(), 1)
                controller.assert_not_called()

    def guarded(self, services, **selection):
        cluster = manager.Cluster(self.env, "", **selection)
        cluster.run = mock.Mock(return_value=mock.Mock(stdout=json.dumps({"name": "fixture", "secret": "never-print"}).encode()))
        rows = "".join(f"fixture\t{service}\n" for service in services)
        return cluster, mock.Mock(returncode=0, stdout=rows.encode())

    def test_active_or_stopped_overlay_prevents_a_base_only_recovery(self):
        for services, flag in ((["keycloak"], "--keycloak"), (["keycloak-db"], "--keycloak"),
                               (["auth-config-init"], "--keycloak"), (["tickets"], "--tickets")):
            with self.subTest(services=services):
                cluster, inventory = self.guarded(services)
                with mock.patch.object(manager.subprocess, "run", return_value=inventory) as docker, \
                     mock.patch.object(manager, "load_state") as state:
                    with self.assertRaisesRegex(manager.ClusterError, flag) as error:
                        cluster.recover()
                    state.assert_not_called()
                self.assertNotIn("never-print", str(error.exception))
                self.assertIn("--all", docker.call_args.args[0])
                cluster.run.assert_called_once_with(("config", "--format", "json"), timeout=30)

    def test_both_flags_preserve_an_installed_combination_and_other_projects_are_ignored(self):
        cluster, inventory = self.guarded(["app", "tickets", "keycloak", "keycloak-db", "auth-config-init"],
                                          keycloak=True, tickets=True)
        with mock.patch.object(manager.subprocess, "run", return_value=inventory):
            cluster.require_service_migration_complete()
        cluster, inventory = self.guarded(["app"])
        inventory.stdout += b"another-project\tkeycloak\nanother-project\ttickets\n"
        with mock.patch.object(manager.subprocess, "run", return_value=inventory):
            cluster.require_service_migration_complete()

    def test_start_uses_safe_recovery_and_tickets_are_included_in_application_start(self):
        cluster = manager.Cluster(self.env, "", keycloak=True, tickets=True)
        cluster.recover = mock.Mock()
        cluster.start()
        cluster.recover.assert_called_once_with()
        cluster.require_service_migration_complete = mock.Mock()
        cluster.run = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            cluster.start_services()
        cluster.require_service_migration_complete.assert_called_once_with()
        cluster.run.assert_called_once_with(("up", "--build", "-d", "proxy-db", "app", "proxy-web", "tickets"), timeout=600)

    def test_read_only_actions_do_not_start_or_require_overlay_selection(self):
        cluster = manager.Cluster(self.env, "")
        cluster.require_service_migration_complete = mock.Mock(side_effect=AssertionError("Read-only operation"))
        cluster.sql = mock.Mock(return_value="fixture\tONLINE")
        cluster.run = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            cluster.status()
        cluster.run.assert_not_called()
        cluster.require_service_migration_complete.assert_not_called()

    def test_network_apply_forwards_overlay_selection_to_recovery_controller(self):
        cluster = mock.Mock()
        cluster.local_state.return_value = "ONLINE"
        keycloak_env = self.project / "data/keycloak/compose.env"
        tickets_env = self.project / "ticket-app/.env"
        with mock.patch.object(network, "load_state", return_value={"initialized": True}), \
             mock.patch.object(network, "Cluster", return_value=cluster) as controller, \
             mock.patch.object(network, "prepare"), mock.patch.object(network, "check_engine"), \
             contextlib.redirect_stdout(io.StringIO()):
            network.apply(self.env, "fixture-context", keycloak=True, tickets=True,
                          keycloak_env_file=keycloak_env, tickets_env_file=tickets_env)
        controller.assert_called_once_with(self.env, "fixture-context", keycloak=True, tickets=True,
                                           keycloak_env_file=keycloak_env, tickets_env_file=tickets_env)
        cluster.require_service_migration_complete.assert_called_once_with()
        cluster.recover.assert_called_once_with()

    def test_network_cli_forwards_selected_env_files_without_activating_prepare(self):
        state = self.project / "data/cluster-state.json"
        argv = ["configure_network.py", "--env-file", str(self.env), "--docker-context", "fixture-context",
                "--keycloak", "--tickets", "--keycloak-env-file", str(self.project / "sso.env"),
                "--tickets-env-file", str(self.project / "tickets.env"), "apply"]
        with mock.patch.object(network, "STATE_PATH", state), mock.patch.object(sys, "argv", argv), \
             mock.patch.object(network, "apply") as apply:
            self.assertEqual(network.main(), 0)
        apply.assert_called_once_with(self.env, "fixture-context", keycloak=True, tickets=True,
                                      keycloak_env_file=self.project / "sso.env",
                                      tickets_env_file=self.project / "tickets.env")
        argv = ["configure_network.py", "prepare"]
        with mock.patch.object(network, "STATE_PATH", state), mock.patch.object(sys, "argv", argv), \
             mock.patch.object(network, "apply") as apply, mock.patch.object(network, "prepare") as prepare:
            self.assertEqual(network.main(), 0)
        prepare.assert_called_once_with()
        apply.assert_not_called()


if __name__ == "__main__":
    unittest.main()
