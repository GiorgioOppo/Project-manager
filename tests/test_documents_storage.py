"""Native document storage boundaries independent from HTTP endpoint tests."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DocumentsStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CC", "cc")):
            raise unittest.SkipTest("C compiler unavailable")
        cls.build = tempfile.TemporaryDirectory(prefix="punto-documents-check-")
        cls.binary = Path(cls.build.name) / "documents-check"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-D_POSIX_C_SOURCE=200809L", "-std=c11",
             "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(ROOT / "src"),
             str(ROOT / "src/documents.c"), str(ROOT / "tests/documents_native_check.c"),
             "-o", str(cls.binary)],
            check=True, capture_output=True, text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def check_scenario(self, scenario, *, symlink_directory=False):
        with tempfile.TemporaryDirectory(prefix="punto-documents-data-") as directory:
            root = Path(directory).resolve()
            documents = root / "documents"
            database = root / "punto.db"
            database.write_bytes(b"fixture outside document directory")
            if symlink_directory:
                target = root / "target"
                target.mkdir()
                documents.symlink_to(target, target_is_directory=True)
            subprocess.run(
                [str(self.binary), scenario, str(documents), str(database)],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(database.read_bytes(), b"fixture outside document directory")
            if documents.exists() and not documents.is_symlink():
                self.assertFalse(any(name.name.endswith(".tmp") for name in documents.iterdir()))

    def test_roundtrip_and_idempotent_remove(self):
        self.check_scenario("roundtrip")

    def test_invalid_names_keys_sizes_and_pdf_signature(self):
        self.check_scenario("invalid")

    def test_duplicate_key_preserves_original(self):
        self.check_scenario("duplicate")

    def test_exact_length_and_bounded_allocations(self):
        self.check_scenario("lengths")

    def test_symlink_file_cannot_read_or_replace_target(self):
        self.check_scenario("file_symlink")

    def test_symlink_directory_is_rejected(self):
        self.check_scenario("directory_symlink", symlink_directory=True)

    def test_private_directory_and_file_permissions(self):
        self.check_scenario("permissions")


if __name__ == "__main__":
    unittest.main()
