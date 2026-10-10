"""Protect local document backup files and remove incomplete archives."""

import gzip
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("backup_documents", SCRIPTS / "backup_documents.py")
BACKUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BACKUP)


class FakeCluster:
    def __init__(self, payload, fail=False):
        self.payload = payload
        self.fail = fail

    def run(self, arguments, *, output, timeout):
        output.write(self.payload)
        if self.fail:
            raise BACKUP.ClusterError("Simulated failure")


class DocumentBackupTests(unittest.TestCase):
    def test_complete_archive_has_private_permissions(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(BACKUP, "PROJECT", Path(temporary)):
            destination = BACKUP.backup_documents(FakeCluster(gzip.compress(b"archive fixture")))
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
            self.assertEqual(destination.parent.stat().st_mode & 0o777, 0o700)
            self.assertEqual(gzip.decompress(destination.read_bytes()), b"archive fixture")

    def test_failed_dump_removes_partial_archive(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(BACKUP, "PROJECT", Path(temporary)):
            with self.assertRaises(BACKUP.ClusterError):
                BACKUP.backup_documents(FakeCluster(b"partial", fail=True))
            self.assertEqual(list((Path(temporary) / "backups").iterdir()), [])

    def test_non_archive_output_is_removed(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(BACKUP, "PROJECT", Path(temporary)):
            with self.assertRaises(BACKUP.ClusterError):
                BACKUP.backup_documents(FakeCluster(b"unexpected diagnostic output"))
            self.assertEqual(list((Path(temporary) / "backups").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
