#!/usr/bin/env python3
"""Create, verify or restore a coherent inventory SQLite copy to a new file."""

import argparse
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile
import time


def regular_file(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("Il database deve essere un file regolare, non un collegamento simbolico.")
    return info


def readonly(path):
    before = regular_file(path)
    connection = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=2)
    after = regular_file(path)
    if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
        connection.close()
        raise ValueError("Il database è cambiato durante l'apertura.")
    return connection


def verify(path):
    with closing(readonly(path)) as connection:
        result = connection.execute("PRAGMA integrity_check").fetchall()
        if result != [("ok",)]:
            raise ValueError("Verifica di integrità SQLite non riuscita.")
        return connection.execute("PRAGMA user_version").fetchone()[0]


def copy_to_new(source, destination):
    source = source.absolute()
    destination = destination.absolute()
    regular_file(source)
    if destination.exists() or destination.is_symlink():
        raise ValueError("La destinazione esiste già: scegliere un nuovo file vuoto.")
    # The temporary copy is private; the published filename must remain absent.
    descriptor, temporary_name = tempfile.mkstemp(prefix=".inventory-restore-", suffix=".sqlite3", dir=destination.parent)
    temporary = Path(temporary_name)
    os.fchmod(descriptor, 0o600)
    os.close(descriptor)
    started = time.monotonic()

    def progress(status, remaining, total):
        del status, remaining, total
        if time.monotonic() - started > 30:
            raise TimeoutError("Tempo massimo della copia SQLite superato.")

    try:
        with closing(readonly(source)) as original, closing(sqlite3.connect(temporary)) as restored:
            original.backup(restored, pages=256, progress=progress, sleep=0.02)
            restored.execute("PRAGMA journal_mode=DELETE").fetchone()
        version = verify(temporary)
        with temporary.open("rb") as copied:
            os.fsync(copied.fileno())
        # link() publishes atomically and refuses every existing target, even a symlink.
        os.link(temporary, destination)
        directory_fd = os.open(destination.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        return version
    finally:
        temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("verify", help="Verifica integrità e versione di un backup.")
    check.add_argument("backup", type=Path)
    for name in ("create", "restore"):
        command = commands.add_parser(name, help="Copia SQLite coerente verso un file che non esiste ancora.")
        command.add_argument("source", type=Path)
        command.add_argument("destination", type=Path)
    options = parser.parse_args(argv)
    try:
        if options.command == "verify":
            version = verify(options.backup)
            print("Backup SQLite integro; schema %d." % version)
        else:
            version = copy_to_new(options.source, options.destination)
            print("Copia SQLite verificata creata: %s (schema %d)." % (options.destination, version))
        return 0
    except (OSError, ValueError, sqlite3.Error, TimeoutError) as error:
        print("Backup inventory: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
