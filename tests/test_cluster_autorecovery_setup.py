"""Recovery provisioning preserves credentials, limits privileges and deployment scope."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
try:
    spec = importlib.util.spec_from_file_location('autorecovery_setup_manager', ROOT / 'scripts/manage_cluster.py')
    manager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manager)
finally:
    sys.path.pop(0)


class AutoRecoverySetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='punto-recovery-provision-')
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        (self.project / 'data').mkdir()
        self.uuids = {node: f'{index:08d}-1111-1111-1111-111111111111'
                      for index, node in enumerate(manager.NODES, 1)}
        self.state = {'initialized': True, 'group_id': manager.GROUP_ID,
                      'source_uuid': self.uuids['db'],
                      'seeded': {node: self.uuids[node] for node in manager.NODES[1:]}}
        self.cluster = object.__new__(manager.Cluster)
        self.cluster.verify_config = mock.Mock()
        self.cluster.local_state = mock.Mock(return_value='ONLINE')
        self.cluster.run = mock.Mock()
        self.cluster.backup = mock.Mock()
        self.cluster.require_service_migration_complete = mock.Mock()
        self.mutations = []
        self.account_exists = False
        def sql(node, query, **kwargs):
            if query == 'SELECT @@server_uuid;':
                return self.uuids[node]
            if query.startswith('SELECT COUNT(*) FROM mysql.user'):
                return '1' if self.account_exists else '0'
            self.mutations.append((node, query))
            return ''
        self.cluster.sql = mock.Mock(side_effect=sql)
        patches = [mock.patch.object(manager, 'PROJECT', self.project),
                   mock.patch.object(manager, 'STATE_PATH', self.project / 'data/cluster-state.json'),
                   mock.patch.object(manager, 'load_state', return_value=self.state)]
        for patch in patches:
            patch.start(); self.addCleanup(patch.stop)

    def configure(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.cluster.configure_autorecovery()
        return output.getvalue()

    def test_dedicated_identity_has_no_application_data_or_root_privileges(self):
        output = self.configure()
        self.assertEqual({node for node, _ in self.mutations}, set(manager.NODES))
        password = (self.project / 'data/cluster-recovery/password').read_text().strip()
        self.assertRegex(password, r'^[a-f0-9]{64}$')
        self.assertNotIn(password, output)
        for _, query in self.mutations:
            self.assertIn("'punto_autorecovery'@'172.30.30.24'", query)
            self.assertIn('REQUIRE SSL', query)
            self.assertIn('GROUP_REPLICATION_ADMIN,SYSTEM_VARIABLES_ADMIN', query)
            self.assertIn('SET SESSION SQL_LOG_BIN=0', query)
            self.assertIn('SET PERSIST group_replication_start_on_boot=OFF', query)
            for forbidden in ('SUPER,', 'ALL PRIVILEGES', 'GRANT OPTION', 'ON punto.',
                              'ALTER USER', 'STOP GROUP_REPLICATION', 'RESET ',
                              'group_replication_bootstrap_group=ON'):
                self.assertNotIn(forbidden, query)
        config = json.loads((self.project / 'data/cluster-recovery/cluster.json').read_text())
        self.assertEqual(config, self.state)
        self.assertEqual((self.project / 'data/cluster-recovery').stat().st_mode & 0o777, 0o700)

    def test_repeated_enable_preserves_secret_and_shared_lock_inode(self):
        self.configure()
        password = self.project / 'data/cluster-recovery/password'
        lock = self.project / 'data/cluster-controller.lock'
        original = password.read_bytes(), password.stat().st_ino, lock.stat().st_ino
        self.account_exists = True
        self.configure()
        self.assertEqual((password.read_bytes(), password.stat().st_ino, lock.stat().st_ino), original)
        self.assertEqual(lock.stat().st_mode & 0o777, 0o666)
        self.assertEqual(lock.read_bytes(), b'')

    def test_missing_secret_for_existing_identity_never_rotates_it(self):
        self.account_exists = True
        with self.assertRaisesRegex(manager.ClusterError, 'Credenziale.*mancante'):
            self.configure()
        self.assertEqual(self.mutations, [])
        self.assertFalse((self.project / 'data/cluster-recovery/password').exists())

    def test_unknown_or_offline_member_blocks_before_credentials_and_mutations(self):
        self.uuids['db3'] = 'aaaaaaaa-2222-2222-2222-222222222222'
        with self.assertRaisesRegex(manager.ClusterError, 'Identità MySQL diversa'):
            self.configure()
        self.assertEqual(self.mutations, [])
        self.assertFalse((self.project / 'data/cluster-recovery').exists())

    def test_incomplete_state_blocks_without_sql_changes(self):
        self.state['seeded'].pop('db3')
        with self.assertRaisesRegex(manager.ClusterError, 'Identità.*incomplete'):
            self.configure()
        self.assertEqual(self.mutations, [])
        self.cluster.verify_config.assert_not_called()

    def test_invalid_existing_secret_is_preserved_and_rejected(self):
        directory = self.project / 'data/cluster-recovery'
        directory.mkdir()
        secret = directory / 'password'
        secret.write_text('original-invalid-secret\n')
        with self.assertRaisesRegex(manager.ClusterError, 'Credenziale.*non valida'):
            self.configure()
        self.assertEqual(secret.read_text(), 'original-invalid-secret\n')
        self.assertEqual(self.mutations, [])

    def assert_invalid_artifacts_rejected(self, kind):
        directory = self.project / 'data/cluster-recovery'
        directory.mkdir(mode=0o750)
        paths = [directory / 'password', directory / 'cluster.json',
                 self.project / 'data/cluster-controller.lock']
        target = self.project / 'original-target'
        target.write_text('original target\n')
        target.chmod(0o640)
        for path in paths:
            with self.subTest(path=path.name, kind=kind):
                for artifact in paths:
                    artifact.write_text('a' * 64 + '\n')
                    artifact.chmod(0o640)
                path.unlink()
                if kind == 'directory':
                    path.mkdir(mode=0o750)
                elif kind == 'fifo':
                    os.mkfifo(path, 0o640)
                else:
                    path.symlink_to(target)
                before = {artifact: artifact.lstat() for artifact in [directory, target, *paths]}
                try:
                    with self.assertRaisesRegex(manager.ClusterError, 'non regolare'):
                        self.configure()
                    self.assertEqual(self.mutations, [])
                    self.cluster.sql.assert_not_called()
                    for artifact, previous in before.items():
                        current = artifact.lstat()
                        self.assertEqual((current.st_mode, current.st_ino),
                                         (previous.st_mode, previous.st_ino))
                    self.assertEqual(target.read_text(), 'original target\n')
                finally:
                    if path.is_dir() and not path.is_symlink():
                        path.rmdir()
                    else:
                        path.unlink()

    def test_directory_artifacts_fail_before_sql_or_permissions_change(self):
        self.assert_invalid_artifacts_rejected('directory')

    def test_fifo_artifacts_fail_without_reading_or_changing_permissions(self):
        self.assert_invalid_artifacts_rejected('fifo')

    def test_symlink_artifacts_fail_without_changing_the_target(self):
        self.assert_invalid_artifacts_rejected('symlink')

    def test_enable_only_builds_and_starts_recovery_service(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.cluster.enable_autorecovery()
        self.cluster.require_service_migration_complete.assert_called_once_with()
        self.cluster.backup.assert_called_once_with()
        self.assertEqual(self.cluster.run.call_args_list, [
            mock.call(('up', '--build', '--force-recreate', '--no-deps', '-d', 'cluster-recovery'), timeout=600),
            mock.call(('exec', '-T', 'cluster-recovery', 'python3', '/app/recovery.py', 'check'), timeout=90)])

    def test_failed_authentication_check_does_not_report_success(self):
        self.cluster.run.side_effect = [mock.Mock(), manager.ClusterError('Verifica MySQL non riuscita.')]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(manager.ClusterError):
                self.cluster.enable_autorecovery()
        self.assertNotIn('autenticazione TLS e gruppo verificati', output.getvalue())


if __name__ == '__main__':
    unittest.main()
