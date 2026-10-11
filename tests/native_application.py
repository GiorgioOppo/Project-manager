"""Link native fixtures against the same business modules as the application."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MODULES = ("workspace", "storage", "procurement", "documents", "organization",
           "alerts", "mail_client", "calendar_client", "work_board", "inventory_client",
           "access_control", "improvements", "improvements_governance",
           "improvements_operations", "improvements_integrations")


def missing_sources(*existing):
    return [str(ROOT / "src" / (name + ".c")) for name in MODULES
            if name + ".c" not in existing]


def link_flags():
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "json-c", "openssl"], text=True).split()
    return ["-lcurl", *flags, *(["-framework", "Security"] if sys.platform == "darwin" else [])]
