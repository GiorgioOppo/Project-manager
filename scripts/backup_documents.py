#!/usr/bin/env python3
"""Back up the scoped MongoDB document database without exposing credentials."""

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import secrets
import sys

from manage_cluster import Cluster, ClusterError, PROJECT


# The password is read inside MongoDB's container and written as a JSON-quoted
# YAML scalar. mongodump recommends --config for secret values; neither a URI
# nor a password is passed in a process argument or sent back to the host.
DUMP_SCRIPT = r'''set -eu
umask 077
temporary=$(mktemp /tmp/punto-documents-backup.XXXXXXXX)
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
chmod 600 "$temporary"
export PUNTO_DOCUMENTS_BACKUP_CONFIG="$temporary"
mongosh --nodb --quiet --eval '
  const fs = require("fs");
  if (!process.env.DOC_DB_PASSWORD ||
      !/^[A-Za-z0-9_]{1,64}$/.test(process.env.DOC_DB_NAME || "") ||
      !/^[A-Za-z0-9_]{1,64}$/.test(process.env.DOC_DB_USER || "")) {
    throw new Error("Invalid document backup configuration");
  }
  fs.writeFileSync(process.env.PUNTO_DOCUMENTS_BACKUP_CONFIG,
    "password: " + JSON.stringify(process.env.DOC_DB_PASSWORD) + "\n",
    {mode: 0o600});
' >/dev/null
mongodump --quiet --config "$temporary" --host documents --port 27017 \
  --ssl --sslCAFile /etc/mongo/tls/ca.crt \
  --authenticationDatabase "$DOC_DB_NAME" --username "$DOC_DB_USER" \
  --db "$DOC_DB_NAME" --archive --gzip
'''


def backup_documents(cluster):
    directory = PROJECT / "backups"
    directory.mkdir(mode=0o700, exist_ok=True)
    if directory.is_symlink():
        raise ClusterError("Directory dei backup documentali non valida.")
    directory.chmod(0o700)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = directory / f"documents-{stamp}-{secrets.token_hex(4)}.archive.gz"
    descriptor = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            cluster.run(("exec", "-T", "documents", "sh", "-c", DUMP_SCRIPT),
                        output=stream, timeout=600)
            stream.flush()
            os.fsync(stream.fileno())
        with destination.open("rb") as stream:
            if stream.read(2) != b"\x1f\x8b":
                raise ClusterError("Archivio documentale non valido; backup incompleto rimosso.")
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=PROJECT / ".env.docker")
    parser.add_argument("--docker-context", default=os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else ""))
    arguments = parser.parse_args()
    try:
        destination = backup_documents(Cluster(arguments.env_file, arguments.docker_context))
    except (ClusterError, OSError, ValueError):
        print("Backup documentale non riuscito; verifica servizio, certificati e configurazione.",
              file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Backup documentale interrotto; archivio incompleto rimosso.", file=sys.stderr)
        return 130
    print(str(destination))
    return 0


if __name__ == "__main__":
    sys.exit(main())
