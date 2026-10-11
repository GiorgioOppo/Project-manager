#!/usr/bin/env python3
"""Provision, inspect and safely recover the local three-node MySQL cluster."""

import argparse
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time

from configure_proxy import read_env


# Stable node identities are also DNS names, certificate SANs and saved seed keys.
# Only Compose command arguments use the renamed service identifiers.
NODES = ("db", "db2", "db3")
NODE_SERVICES = {"db": "mysql-1", "db2": "mysql-2", "db3": "mysql-3"}
LEGACY_SERVICES = frozenset(("db", "db2", "db3", "db-proxy", "documents", "squid",
                             "nginx-proxy-manager", "mail-addon", "calendar-addon",
                             "inventory-addon", "ticket-app"))
GROUP_ID = "8de71c6b-d3bc-4287-9a2a-edf8436340b1"
PROJECT = Path(__file__).resolve().parent.parent
STATE_PATH = PROJECT / "data" / "cluster-state.json"


class ClusterError(Exception):
    pass


def sql_literal(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE_PATH.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    temporary.chmod(0o600)
    temporary.replace(STATE_PATH)


def load_state():
    if not STATE_PATH.exists():
        return {}
    try:
        state = json.loads(STATE_PATH.read_text(encoding="utf-8"))
        if not isinstance(state, dict) or state.get("group_id") != GROUP_ID:
            raise ValueError
        return state
    except (OSError, ValueError):
        raise ClusterError("Stato locale del cluster non valido; nessun bootstrap eseguito.") from None


def ensure_secrets(env_file):
    values = read_env(env_file)
    updates = {key: secrets.token_urlsafe(24) for key in
               ("CLUSTER_REPLICATION_PASSWORD", "CLUSTER_MONITOR_PASSWORD",
                "DOC_DB_ROOT_PASSWORD", "DOC_DB_PASSWORD")
               if not values.get(key)}
    if updates:
        lines = env_file.read_text(encoding="utf-8").splitlines()
        found = set()
        for index, line in enumerate(lines):
            key = line.partition("=")[0].strip()
            if key in updates:
                lines[index] = key + "=" + updates[key]
                found.add(key)
        lines.extend(key + "=" + value for key, value in updates.items() if key not in found)
        env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
        env_file.chmod(0o600)
        print("Credenziali interne del cluster generate nel file delle variabili.", flush=True)


class Cluster:
    def __init__(self, env_file, context):
        self.env_file = env_file.resolve()
        self.values = read_env(self.env_file)
        self.database = self.values.get("MYSQL_DATABASE", "punto")
        if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", self.database):
            raise ClusterError("MYSQL_DATABASE deve essere un identificatore alfanumerico valido.")
        binary = shutil.which("docker")
        if not binary and Path("/Applications/Docker.app/Contents/Resources/bin/docker").exists():
            binary = "/Applications/Docker.app/Contents/Resources/bin/docker"
        if not binary:
            raise ClusterError("Docker non disponibile.")
        self.command = [binary] + (["--context", context] if context else []) + [
            "compose", "--project-directory", str(PROJECT), "--env-file", str(self.env_file)]
        self.environment = os.environ.copy()
        self.environment["PATH"] = str(Path(binary).parent) + os.pathsep + self.environment.get("PATH", "")

    def run(self, arguments, *, data=None, timeout=180, check=True, output=None):
        try:
            result = subprocess.run(self.command + list(arguments), input=data,
                                    stdout=output or subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=self.environment, cwd=PROJECT, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired):
            raise ClusterError("Operazione Docker non completata; controlla lo stato dei servizi.") from None
        if result.returncode and check:
            # MySQL diagnostics may contain SQL values: report only numeric error codes.
            error_code = re.search(rb"ERROR\s+(\d+)", result.stderr)
            suffix = " (MySQL " + error_code.group(1).decode() + ")" if error_code else ""
            raise ClusterError("Operazione Docker/MySQL non riuscita" + suffix + ". Nessun comando viene ripetuto automaticamente.")
        return result

    def require_service_migration_complete(self):
        """Refuse to start beside old containers sharing the project's IPs/data."""
        try:
            # Resolved Compose configuration contains secrets: inspect only in
            # memory and never include its payload in output or error messages.
            config = json.loads(self.run(("config", "--format", "json"), timeout=30).stdout)
            project = config.get("name") if isinstance(config, dict) else None
            if not isinstance(project, str) or not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", project):
                raise ValueError
            docker = self.command[:self.command.index("compose")]
            result = subprocess.run(docker + [
                "ps", "--all", "--filter", "label=com.docker.compose.project=" + project,
                "--format", '{{.Label "com.docker.compose.project"}}\t{{.Label "com.docker.compose.service"}}',
            ], capture_output=True, env=self.environment, cwd=PROJECT, timeout=30)
            if result.returncode:
                raise ValueError
            legacy = set()
            for line in result.stdout.decode("utf-8").splitlines():
                if not line:
                    continue
                labels = line.split("\t")
                if len(labels) != 2:
                    raise ValueError
                container_project, service = labels
                if container_project == project and service in LEGACY_SERVICES:
                    legacy.add(service)
        except (OSError, subprocess.TimeoutExpired, ValueError, UnicodeError):
            raise ClusterError("Impossibile verificare la migrazione dei servizi; avvio interrotto senza modificare container.") from None
        if legacy:
            raise ClusterError("Container legacy presenti nello stesso progetto: " + ", ".join(sorted(legacy)) +
                ". Completa prima la migrazione descritta in docs/tehnical/nomi-servizi-e-immagini.md. "
                "Nessun container viene rimosso automaticamente.")

    def sql(self, node, query, *, check=True, timeout=30):
        result = self.run(("exec", "-T", NODE_SERVICES[node], "sh", "-c",
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --user=root --batch --raw --skip-column-names'),
            data=query.encode("utf-8"), timeout=timeout, check=check)
        if result.returncode:
            return None
        return result.stdout.decode("utf-8").strip()

    def wait_sql(self):
        deadline = time.monotonic() + 180
        pending = set(NODES)
        while pending and time.monotonic() < deadline:
            for node in tuple(pending):
                if self.sql(node, "SELECT 1;", check=False, timeout=10) == "1":
                    pending.remove(node)
            if pending:
                time.sleep(1)
        if pending:
            raise ClusterError("Nodi SQL non pronti: " + ", ".join(sorted(pending)))

    def local_state(self, node):
        return self.sql(node, "SELECT MEMBER_STATE FROM performance_schema.replication_group_members "
                        "WHERE MEMBER_ID=@@server_uuid;", check=False) or "OFFLINE"

    def wait_online(self, expected=3):
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            count = sum(self.local_state(node) == "ONLINE" for node in NODES)
            if count >= expected:
                return
            time.sleep(1)
        self.status()
        raise ClusterError("Il cluster non ha raggiunto lo stato ONLINE atteso.")

    def verify_config(self):
        for index, node in enumerate(NODES, 1):
            row = self.sql(node, "SELECT @@server_id,@@group_replication_group_name,"
                           "@@group_replication_single_primary_mode,@@gtid_mode;")
            if row != f"{index}\t{GROUP_ID}\t0\tON":
                raise ClusterError("Configurazione Group Replication incompatibile su " + node)

    def backup(self, node="db"):
        directory = PROJECT / "backups"
        directory.mkdir(mode=0o700, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        destination = directory / f"mysql-{stamp}-{secrets.token_hex(3)}.sql"
        fd = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        try:
            with os.fdopen(fd, "wb") as stream:
                self.run(("exec", "-T", NODE_SERVICES[node], "sh", "-c",
                    'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysqldump --user=root '
                    '--single-transaction --routines --triggers --events --hex-blob '
                    '--set-gtid-purged=OFF --databases "$MYSQL_DATABASE"'), output=stream)
        except Exception:
            destination.unlink(missing_ok=True)
            raise
        print("Backup MySQL salvato: " + str(destination), flush=True)
        return destination

    def provision_accounts(self, node):
        recovery = self.values.get("CLUSTER_REPLICATION_PASSWORD", "")
        monitor = self.values.get("CLUSTER_MONITOR_PASSWORD", "")
        if not recovery or not monitor:
            raise ClusterError("Mancano le credenziali interne: esegui init con il file corretto.")
        if len(recovery.encode("utf-8")) > 32:
            raise ClusterError("CLUSTER_REPLICATION_PASSWORD può contenere al massimo 32 byte, limite del canale di recupero MySQL.")
        self.sql(node, "SET SESSION SQL_LOG_BIN=0; SET GLOBAL super_read_only=OFF;"
            "CREATE USER IF NOT EXISTS 'punto_recovery'@'%' IDENTIFIED BY " + sql_literal(recovery) + " REQUIRE SSL;"
            "GRANT REPLICATION SLAVE,CONNECTION_ADMIN ON *.* TO 'punto_recovery'@'%';"
            "CREATE USER IF NOT EXISTS 'punto_monitor'@'%' IDENTIFIED BY " + sql_literal(monitor) + " REQUIRE SSL;"
            "GRANT SELECT ON performance_schema.* TO 'punto_monitor'@'%';"
            "CHANGE REPLICATION SOURCE TO SOURCE_USER='punto_recovery',SOURCE_PASSWORD=" + sql_literal(recovery) +
            " FOR CHANNEL 'group_replication_recovery';"
            "SET PERSIST_ONLY super_read_only=ON;")

    def start_group(self, node, *, bootstrap=False):
        if bootstrap:
            self.sql(node, "SET GLOBAL group_replication_bootstrap_group=ON;")
        try:
            self.sql(node, "START GROUP_REPLICATION;", timeout=90)
        finally:
            if bootstrap:
                try:
                    self.sql(node, "SET GLOBAL group_replication_bootstrap_group=OFF;")
                except ClusterError:
                    raise ClusterError("Impossibile disattivare bootstrap_group su " + node + "; interrompi i riavvii e verifica quel nodo prima di continuare.") from None

    def stop_group(self, node):
        self.sql(node, "STOP GROUP_REPLICATION;", check=False, timeout=90)
        running = self.sql(node, "SELECT COUNT(*) FROM performance_schema.replication_connection_status "
            "WHERE CHANNEL_NAME IN ('group_replication_applier','group_replication_recovery') AND SERVICE_STATE<>'OFF';")
        if running != "0" or self.local_state(node) in ("ONLINE", "RECOVERING"):
            raise ClusterError("Group Replication non arrestato su " + node + "; nessun bootstrap eseguito.")

    def start_services(self):
        self.require_service_migration_complete()
        print("Avvio del bilanciatore, dell’app e del reverse proxy…", flush=True)
        self.run(("up", "--build", "-d", "proxy-db", "app", "proxy-web"), timeout=600)

    def status(self):
        for node in NODES:
            result = self.sql(node, "SELECT @@hostname,@@server_uuid,"
                "COALESCE((SELECT MEMBER_STATE FROM performance_schema.replication_group_members WHERE MEMBER_ID=@@server_uuid),'OFFLINE'),"
                "COALESCE((SELECT MEMBER_ROLE FROM performance_schema.replication_group_members WHERE MEMBER_ID=@@server_uuid),''),"
                "@@read_only,@@super_read_only,@@auto_increment_increment,@@auto_increment_offset;", check=False)
            print(node + ": " + (result.replace("\t", " | ") if result else "non raggiungibile"), flush=True)

    def init(self):
        self.require_service_migration_complete()
        state = load_state()
        if state.get("initialized"):
            print("Cluster già inizializzato: verifica e recupero senza importare dati.", flush=True)
            self.recover()
            return
        self.run(("stop", "app"))
        # Preserve the old standalone database before changing its configuration.
        if not state and self.sql("db", "SELECT 1;", check=False) == "1":
            backup = self.backup()
            state = {"group_id": GROUP_ID, "backup": str(backup), "seeded": {}}
            save_state(state)
        print("Avvio dei tre nodi MySQL…", flush=True)
        self.run(("up", "-d", *(NODE_SERVICES[node] for node in NODES)))
        self.wait_sql()
        self.verify_config()
        if any(self.local_state(node) == "ONLINE" for node in NODES):
            if (state.get("source_uuid") != self.sql("db", "SELECT @@server_uuid;") or
                    any(state.get("seeded", {}).get(node) != self.sql(node, "SELECT @@server_uuid;") for node in NODES[1:])):
                raise ClusterError("Esiste un gruppo attivo senza un seed locale verificato; nessun dato modificato.")
            for node in NODES:
                if self.local_state(node) != "ONLINE":
                    self.stop_group(node)
                    self.start_group(node)
            self.wait_online()
            for node in NODES:
                self.sql(node, "SET PERSIST group_replication_start_on_boot=ON;")
            state["initialized"] = True
            save_state(state)
            self.status()
            self.start_services()
            return
        if not state:
            backup = self.backup()
            state = {"group_id": GROUP_ID, "backup": str(backup), "seeded": {}}
            save_state(state)
        source_uuid = self.sql("db", "SELECT @@server_uuid;")
        if self.sql("db", "SELECT @@gtid_executed;"):
            raise ClusterError("Il seed iniziale richiede una sorgente senza storico GTID; per database già replicati serve un clone o un seed con checkpoint GTID. Nessun dato sovrascritto.")
        if state.get("source_uuid") not in (None, source_uuid):
            raise ClusterError("Il volume del nodo sorgente è cambiato: inizializzazione interrotta.")
        state["source_uuid"] = source_uuid
        save_state(state)
        backup = Path(state["backup"])
        if not backup.is_file():
            raise ClusterError("Backup del seed non trovato; nessuna importazione eseguita.")
        for node in NODES:
            self.provision_accounts(node)
        for node in NODES[1:]:
            node_uuid = self.sql(node, "SELECT @@server_uuid;")
            if state["seeded"].get(node) == node_uuid:
                continue
            tables = self.sql(node, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=" + sql_literal(self.database) + ";")
            if tables != "0":
                raise ClusterError(node + " contiene già dati applicativi: importazione rifiutata per evitare sovrascritture.")
            initial_gtids = self.sql(node, "SELECT @@gtid_executed;")
            if initial_gtids:
                other_tables = self.sql(node, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema NOT IN "
                    "('mysql','information_schema','performance_schema','sys'," + sql_literal(self.database) + ");")
                # The official image logs its initial database/account setup with GTID ON.
                # Only a new, application-empty member with its own startup GTIDs is reset.
                if (other_tables != "0" or self.local_state(node) != "OFFLINE" or
                        not re.fullmatch(re.escape(node_uuid) + r":1(?:-[0-9]+)?", initial_gtids)):
                    raise ClusterError(node + " contiene uno storico GTID non riconducibile all'inizializzazione locale; seed bloccato.")
                self.sql(node, "RESET BINARY LOGS AND GTIDS;")
            print("Importazione del seed sul nodo nuovo " + node + "…", flush=True)
            self.sql(node, "SET SESSION SQL_LOG_BIN=0;\n" + backup.read_text(encoding="utf-8"), timeout=180)
            state["seeded"][node] = node_uuid
            save_state(state)
        self.start_group("db", bootstrap=True)
        for node in NODES[1:]:
            self.start_group(node)
        self.wait_online()
        for node in NODES:
            self.sql(node, "SET PERSIST group_replication_start_on_boot=ON;")
        state["initialized"] = True
        save_state(state)
        self.status()
        self.start_services()

    def recover(self):
        self.require_service_migration_complete()
        state = load_state()
        if not state.get("initialized"):
            raise ClusterError("Inizializzazione non conclusa: completa init prima del recupero.")
        self.run(("up", "-d", *(NODE_SERVICES[node] for node in NODES)))
        self.wait_sql()
        self.verify_config()
        online = [node for node in NODES if self.local_state(node) == "ONLINE"]
        if online:
            # Rejoin the existing group; never create a competing group.
            for node in NODES:
                if self.local_state(node) != "ONLINE":
                    self.stop_group(node)
                    self.start_group(node)
        else:
            for node in NODES:
                self.stop_group(node)
            sets = {}
            for node in NODES:
                executed = self.sql(node, "SELECT @@GLOBAL.gtid_executed;")
                certified = self.sql(node, "SELECT RECEIVED_TRANSACTION_SET FROM performance_schema.replication_connection_status "
                                     "WHERE CHANNEL_NAME='group_replication_applier';")
                sets[node] = ",".join(item for item in (executed, certified) if item)
            # Oracle's outage procedure requires a superset of executed AND certified GTIDs.
            candidates = [node for node in NODES if all(
                self.sql(node, "SELECT GTID_SUBSET(" + sql_literal(sets[other]) + "," + sql_literal(sets[node]) + ");") == "1"
                for other in NODES)]
            if not candidates:
                raise ClusterError("GTID divergenti: nessun nodo contiene tutte le transazioni. Recupero bloccato senza forzare quorum o cancellare dati.")
            chosen = candidates[0]
            print("Recupero dopo arresto completo: bootstrap del nodo con GTID più avanzati " + chosen, flush=True)
            self.start_group(chosen, bootstrap=True)
            for node in NODES:
                if node != chosen:
                    self.start_group(node)
        self.wait_online()
        self.status()
        self.start_services()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=PROJECT / ".env.docker")
    parser.add_argument("--docker-context", default=os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else ""))
    parser.add_argument("action", choices=("init", "status", "backup", "recover"))
    args = parser.parse_args()
    lock = None
    try:
        if args.action in ("init", "recover"):
            STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
            lock = (STATE_PATH.parent / "cluster-controller.lock").open("a")
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ClusterError("Un'altra inizializzazione o procedura di recupero è già in corso.") from None
        if args.action == "init":
            ensure_secrets(args.env_file)
        cluster = Cluster(args.env_file, args.docker_context)
        if args.action == "backup":
            online = [node for node in NODES if cluster.local_state(node) == "ONLINE"]
            cluster.backup(online[0] if online else "db")
        else:
            getattr(cluster, args.action)()
        return 0
    except Exception as error:
        # Never output API payloads, SQL text or passwords, even on unexpected errors.
        message = str(error) if isinstance(error, ClusterError) else "Operazione interrotta; verifica i servizi e il file delle variabili."
        print("Errore: " + message, file=sys.stderr)
        return 1
    finally:
        if lock is not None:
            lock.close()


if __name__ == "__main__":
    sys.exit(main())
