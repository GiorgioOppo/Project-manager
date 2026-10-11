import fcntl
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("guest_lock", ROOT / "scripts" / "cluster_guest_lock.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GuestLockTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="guest-lock-test-")
        self.path = Path(self.temporary.name) / "lock"
        self.path.touch()
        self.commands = []

    def tearDown(self):
        self.temporary.cleanup()

    def local_process(self, command, **options):
        self.commands.append(command)
        return subprocess.Popen([sys.executable, "-u", "-c", command[-3], str(self.path), command[-1]], **options)

    def helper(self, **options):
        return module.GuestClusterLock(self.path, popen=self.local_process, **options)

    def test_readiness_means_lock_held_and_eof_releases_it(self):
        with self.helper():
            descriptor = os.open(self.path, os.O_RDWR)
            try:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                os.close(descriptor)
        descriptor = os.open(self.path, os.O_RDWR)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            os.close(descriptor)

    def test_competitor_times_out_without_claiming_lock(self):
        with self.helper():
            with self.assertRaisesRegex(module.GuestLockError, "^guest_lock_not_acquired$"):
                self.helper(acquire_timeout=0.2).acquire()

    def test_manual_kernel_lock_prevents_helper_readiness(self):
        descriptor = os.open(self.path, os.O_RDWR)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(module.GuestLockError, "^guest_lock_not_acquired$"):
                self.helper(acquire_timeout=0.2).acquire()
        finally:
            os.close(descriptor)

    def test_command_is_nonroot_networkless_and_mounts_only_lock(self):
        command = self.helper().command()
        for argument in ("--network=none", "--read-only", "--user=10001:10001", "--cap-drop=ALL", "--pull=never", "--rm"):
            self.assertIn(argument, command)
        self.assertEqual(command.count("--mount"), 1)
        self.assertNotIn("/var/run/docker.sock", " ".join(command))
        self.assertNotIn("--publish", command)
        self.assertNotIn("--env", command)

    def test_missing_or_symlink_file_never_starts_helper(self):
        self.path.unlink()
        with self.assertRaises(module.GuestLockError):
            self.helper().acquire()
        self.assertEqual(self.commands, [])
        target = self.path.with_name("target")
        target.touch()
        self.path.symlink_to(target)
        with self.assertRaises(module.GuestLockError):
            self.helper().acquire()
        self.assertEqual(self.commands, [])

    def test_exception_releases_lock(self):
        with self.assertRaisesRegex(RuntimeError, "synthetic"):
            with self.helper():
                raise RuntimeError("synthetic")
        with self.helper():
            pass


if __name__ == "__main__":
    unittest.main()
