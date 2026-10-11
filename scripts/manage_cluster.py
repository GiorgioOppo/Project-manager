#!/usr/bin/env python3
"""Provision, inspect and safely recover the local three-node MySQL cluster."""

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time

from configure_proxy import read_env
from cluster_guest_lock import GuestClusterLock, GuestLockError


# Stable node identities are also DNS names, certificate SANs and saved seed keys.
# Only Compose command arguments use the renamed service identifiers.
NODES = ("db", "db2", "db3")
NODE_SERVICES = {"db": "mysql-1", "db2": "mysql-2", "db3": "mysql-3"}
LEGACY_SERVICES = frozenset(("db", "db2", "db3", "db-proxy", "documents", "squid",
                             "nginx-proxy-manager", "mail-addon", "calendar-addon",
                             "inventory-addon", "ticket-app"))
GROUP_ID = "8de71c6b-d3bc-4287-9a2a-edf8436340b1"
AUTORECOVERY_USER = "punto_autorecovery"
AUTORECOVERY_ADDRESS = "172.30.30.24"
PROJECT = Path(__file__).resolve().parent.parent
STATE_PATH = PROJECT / "data" / "cluster-state.json"
KEYCLOAK_SERVICES = frozenset(("keycloak", "keycloak-db", "auth-config-init"))


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


def regular_file_or_missing(path, message):
    """Reject directories, FIFOs and symlinks before opening or chmodding them."""
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return False
    except OSError:
        raise ClusterError(message) from None
    if not stat.S_ISREG(mode):
        raise ClusterError(message)
    return True


def expected_member_identities(state):
    seeded = state.get("seeded")
    expected = {"db": state.get("source_uuid")}
    if isinstance(seeded, dict):
        expected.update({node: seeded.get(node) for node in NODES[1:]})
    if any(not isinstance(expected.get(node), str) or not re.fullmatch(
            r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", expected[node]) for node in NODES):
        raise ClusterError("Identità del cluster incomplete; recupero non avviato.")
    expected = {node: expected[node].lower() for node in NODES}
    if len(set(expected.values())) != len(NODES):
        raise ClusterError("Identità del cluster duplicate; recupero non avviato.")
    return expected


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
    def __init__(self, env_file, context, *, keycloak=False, tickets=False, keycloak_groups=False,
                 keycloak_env_file=None, tickets_env_file=None):
        if keycloak_groups and not keycloak:
            raise ClusterError("--keycloak-groups richiede --keycloak.")
        self.env_file = env_file.resolve()
        self.keycloak = keycloak
        self.tickets = tickets
        self.keycloak_groups = keycloak_groups
        env_files = [self.env_file]
        compose_files = [PROJECT / "compose.yaml"]
        self.values = read_env(self.env_file)
        if tickets:
            env_files.append((tickets_env_file or PROJECT / "ticket-app" / ".env").resolve())
            compose_files.append(PROJECT / "compose.tickets.yaml")
        if keycloak:
            env_files.append((keycloak_env_file or PROJECT / "data" / "keycloak" / "compose.env").resolve())
            compose_files.append(PROJECT / "compose.keycloak.yaml")
            if tickets:
                compose_files.append(PROJECT / "compose.tickets-keycloak.yaml")
        if keycloak_groups:
            compose_files.append(PROJECT / "compose.keycloak-groups.yaml")
        for selected in env_files[1:]:
            if not selected.is_file():
                raise ClusterError("File ambiente dell'overlay non trovato: " + str(selected) +
                                   ". Prepara prima la configurazione; nessun servizio avviato.")
            self.values.update(read_env(selected))
        self.database = self.values.get("MYSQL_DATABASE", "punto")
        if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", self.database):
            raise ClusterError("MYSQL_DATABASE deve essere un identificatore alfanumerico valido.")
        binary = shutil.which("docker")
        if not binary and Path("/Applications/Docker.app/Contents/Resources/bin/docker").exists():
            binary = "/Applications/Docker.app/Contents/Resources/bin/docker"
        if not binary:
            raise ClusterError("Docker non disponibile.")
        self.command = [binary] + (["--context", context] if context else []) + [
            "compose", "--project-directory", str(PROJECT)]
        for selected in env_files:
            self.command.extend(("--env-file", str(selected)))
        for selected in compose_files:
            self.command.extend(("-f", str(selected)))
        self.environment = os.environ.copy()
        self.environment["PATH"] = str(Path(binary).parent) + os.pathsep + self.environment.get("PATH", "")
        self.guest_lock = None

    def acquire_maintenance_lock(self):
        """Exclude the Linux controller even when the CLI runs on macOS.

        The host flock serializes host CLIs. A separate networkless helper holds
        the actual controller bind's flock in Docker's kernel, across restarts
        or recreation of the controller itself.
        """
        if getattr(self, "guest_lock", None) is not None:
            self._check_maintenance_lock()
            return
        identifiers = self.run(("ps", "--all", "--quiet", "cluster-recovery"), timeout=30).stdout.decode().split()
        if not identifiers:
            # A removed/not-yet-installed controller cannot restart itself.
            # init/recover start it only after their final SQL modification.
            return
        if len(identifiers) != 1 or not re.fullmatch(r"[a-f0-9]{64}", identifiers[0]):
            raise ClusterError("Impossibile identificare il controller automatico; manutenzione interrotta.")
        docker = self.command[:self.command.index("compose")]
        try:
            result = subprocess.run(docker + ["inspect", identifiers[0]], capture_output=True,
                                    env=self.environment, timeout=30, check=True)
            info = json.loads(result.stdout)[0]
            locks = [mount for mount in info["Mounts"] if mount["Destination"] == "/run/punto-cluster.lock"]
            image = info["Image"]
            expected = (STATE_PATH.parent / "cluster-controller.lock").resolve()
            if (info["Config"].get("Labels", {}).get("com.docker.compose.service") != "cluster-recovery"
                    or not re.fullmatch(r"sha256:[a-f0-9]{64}", image)
                    or len(locks) != 1 or locks[0]["Type"] != "bind"
                    or Path(locks[0]["Source"]).resolve() != expected):
                raise ValueError
            self.guest_lock = GuestClusterLock(expected, docker_command=docker, image=image).acquire()
        except (OSError, subprocess.SubprocessError, ValueError, KeyError, IndexError, TypeError, GuestLockError):
            raise ClusterError("Lock Linux del recupero automatico non disponibile; nessuna manutenzione avviata.") from None

    def _check_maintenance_lock(self):
        guard = getattr(self, "guest_lock", None)
        if guard is not None:
            try:
                guard.assert_held()
            except GuestLockError:
                raise ClusterError("Lock Linux del recupero automatico perso; operazione interrotta.") from None

    def release_maintenance_lock(self):
        guard, self.guest_lock = getattr(self, "guest_lock", None), None
        if guard is not None:
            try:
                guard.release()
            except GuestLockError:
                raise ClusterError("Impossibile verificare la chiusura del lock Linux; controlla i servizi.") from None

    def run(self, arguments, *, data=None, timeout=180, check=True, output=None, guard=True):
        if guard:
            self._check_maintenance_lock()
        try:
            result = subprocess.run(self.command + list(arguments), input=data,
                                    stdout=output or subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=self.environment, cwd=PROJECT, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired):
            raise ClusterError("Operazione Docker non completata; controlla lo stato dei servizi.") from None
        if guard:
            self._check_maintenance_lock()
        if result.returncode and check:
            # MySQL diagnostics may contain SQL values: report only numeric error codes.
            error_code = re.search(rb"ERROR\s+(\d+)", result.stderr)
            suffix = " (MySQL " + error_code.group(1).decode() + ")" if error_code else ""
            raise ClusterError("Operazione Docker/MySQL non riuscita" + suffix + ". Nessun comando viene ripetuto automaticamente.")
        return result

    def require_service_migration_complete(self):
        """Refuse legacy conflicts or silently dropping an installed overlay."""
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
            configured = set()
            for line in result.stdout.decode("utf-8").splitlines():
                if not line:
                    continue
                labels = line.split("\t")
                if len(labels) != 2:
                    raise ValueError
                container_project, service = labels
                if container_project == project:
                    configured.add(service)
                    if service in LEGACY_SERVICES:
                        legacy.add(service)
        except (OSError, subprocess.TimeoutExpired, ValueError, UnicodeError):
            raise ClusterError("Impossibile verificare la migrazione dei servizi; avvio interrotto senza modificare container.") from None
        if legacy:
            raise ClusterError("Container legacy presenti nello stesso progetto: " + ", ".join(sorted(legacy)) +
                ". Completa prima la migrazione descritta in docs/tehnical/nomi-servizi-e-immagini.md. "
                "Nessun container viene rimosso automaticamente.")
        missing = []
        if configured & KEYCLOAK_SERVICES and not getattr(self, "keycloak", False):
            missing.append("--keycloak")
        if "tickets" in configured and not getattr(self, "tickets", False):
            missing.append("--tickets")
        if "keycloak-groups-sync" in configured and not getattr(self, "keycloak_groups", False):
            missing.append("--keycloak-groups")
        if missing:
            raise ClusterError("Overlay già presenti nel progetto: ripeti l'operazione selezionando " +
                " ".join(missing) + ". I container arrestati sono inclusi nel controllo; "
                "nessun servizio viene ricreato con una configurazione ridotta. "
                "Vedi docs/tehnical/keycloak.md.")

    def sql(self, node, query, *, check=True, timeout=30):
        # Clearing a bootstrap flag must still be attempted if the lock helper
        # disappears after ON succeeded but before its acknowledgement arrived.
        cleanup = query.strip().upper() == "SET GLOBAL GROUP_REPLICATION_BOOTSTRAP_GROUP=OFF;"
        result = self.run(("exec", "-T", NODE_SERVICES[node], "sh", "-c",
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --user=root --batch --raw --skip-column-names'),
            data=query.encode("utf-8"), timeout=timeout, check=check, **({"guard": False} if cleanup else {}))
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

    def verify_member_identities(self, expected):
        for node in NODES:
            actual = self.sql(node, "SELECT @@server_uuid;")
            if not isinstance(actual, str) or actual.lower() != expected[node]:
                raise ClusterError("Identità MySQL diversa da quella salvata su " + node +
                                   "; nessuna modifica Group Replication eseguita.")

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

    def configure_autorecovery(self):
        """Provision only the recovery identity/configuration on an existing group.

        Runs under the host mutex and, when installed, the Linux helper lock.
        No application schema, account password or recovery channel is changed.
        """
        state = load_state()
        if not state.get("initialized"):
            raise ClusterError("Completa l'inizializzazione prima del recupero automatico.")
        expected = expected_member_identities(state)
        directory = PROJECT / "data" / "cluster-recovery"
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            raise ClusterError("La directory del recupero automatico non è una directory regolare.")
        password_path = directory / "password"
        config_path = directory / "cluster.json"
        lock_path = STATE_PATH.parent / "cluster-controller.lock"
        password_exists = regular_file_or_missing(password_path,
            "File credenziale del recupero automatico non regolare.")
        regular_file_or_missing(config_path, "File identità del recupero automatico non regolare.")
        regular_file_or_missing(lock_path, "File lock del cluster non regolare.")
        # Validate all existing artifacts and the secret before changing any
        # permissions or SQL accounts. In particular, never read a FIFO.
        password = None
        if password_exists:
            password = password_path.read_text(encoding="ascii").strip()
            if not re.fullmatch(r"[0-9a-f]{64}", password):
                raise ClusterError("Credenziale del recupero automatico non valida; conserva il file originale.")
        self.verify_config()
        self.verify_member_identities(expected)
        if any(self.local_state(node) != "ONLINE" for node in NODES):
            raise ClusterError("Servono i tre nodi originali ONLINE prima di configurare il recupero automatico.")
        account = sql_literal(AUTORECOVERY_USER) + "@" + sql_literal(AUTORECOVERY_ADDRESS)
        if not password_exists:
            # A lost secret must never silently rotate a pre-existing identity.
            for node in NODES:
                if self.sql(node, "SELECT COUNT(*) FROM mysql.user WHERE User=" +
                            sql_literal(AUTORECOVERY_USER) + " AND Host=" +
                            sql_literal(AUTORECOVERY_ADDRESS) + ";") != "0":
                    raise ClusterError("Credenziale del recupero automatico mancante; ripristina il file originale.")
        directory.mkdir(parents=True, mode=0o700, exist_ok=True)
        directory.chmod(0o700)
        if not password_exists:
            password = secrets.token_hex(32)
            fd = os.open(password_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w", encoding="ascii") as stream:
                stream.write(password + "\n")
        password_path.chmod(0o444)
        for node in NODES:
            # Local account provisioning does not introduce new replication GTIDs.
            self.sql(node, "SET SESSION SQL_LOG_BIN=0;CREATE USER IF NOT EXISTS " + account +
                " IDENTIFIED BY " + sql_literal(password) + " REQUIRE SSL;"
                "GRANT GROUP_REPLICATION_ADMIN,SYSTEM_VARIABLES_ADMIN ON *.* TO " + account + ";" +
                "".join("GRANT SELECT ON performance_schema." + table + " TO " + account + ";"
                    for table in ("replication_group_members", "replication_group_member_stats",
                                  "replication_connection_status", "replication_applier_status")) +
                "SET PERSIST group_replication_start_on_boot=OFF;")
        if config_path.exists():
            config_path.chmod(0o600)
        # Write in place: a running container's file bind mount keeps this inode.
        config_path.write_text(json.dumps({"initialized": True, "group_id": GROUP_ID,
            "source_uuid": expected["db"], "seeded": {node: expected[node] for node in NODES[1:]}},
            indent=2) + "\n", encoding="utf-8")
        config_path.chmod(0o444)
        lock_path.touch(exist_ok=True)
        # Only this empty lock is writable by the isolated service UID. Its
        # parent and the credential directory retain their host protections.
        lock_path.chmod(0o666)
        print("Recupero automatico configurato con account dedicato e identità verificate.", flush=True)

    def enable_autorecovery(self):
        self.require_service_migration_complete()
        self.backup()
        self.configure_autorecovery()
        # Never rebuild/recreate app or other installed overlays here.
        self.run(("up", "--build", "--force-recreate", "--no-deps", "-d", "cluster-recovery"), timeout=600)
        # The daemon cannot take our exclusive maintenance lock until this CLI
        # exits. A separate SELECT-only probe verifies TLS/account credentials
        # from its actual isolated address without releasing that lock early.
        self.run(("exec", "-T", "cluster-recovery", "python3", "/app/recovery.py", "check"), timeout=90)
        print("Servizio di recupero automatico avviato: autenticazione TLS e gruppo verificati.", flush=True)

    def start_group(self, node, *, bootstrap=False):
        try:
            if bootstrap:
                self.sql(node, "SET GLOBAL group_replication_bootstrap_group=ON;")
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
        services = ("proxy-db", "app", "proxy-web")
        if getattr(self, "tickets", False):
            services += ("tickets",)
        self.run(("up", "--build", "-d", *services), timeout=600)

    def start(self):
        """Restart an initialized deployment through the same safe recovery path."""
        self.recover()

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
                self.sql(node, "SET PERSIST group_replication_start_on_boot=OFF;")
            state["initialized"] = True
            save_state(state)
            self.configure_autorecovery()
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
            self.sql(node, "SET PERSIST group_replication_start_on_boot=OFF;")
        state["initialized"] = True
        save_state(state)
        self.configure_autorecovery()
        self.status()
        self.start_services()

    def recover(self):
        self.require_service_migration_complete()
        state = load_state()
        if not state.get("initialized"):
            raise ClusterError("Inizializzazione non conclusa: completa init prima del recupero.")
        expected = expected_member_identities(state)
        self.run(("up", "-d", *(NODE_SERVICES[node] for node in NODES)))
        self.wait_sql()
        self.verify_config()
        # Containers may need starting for SELECTs to work, but no GR changes
        # are allowed until every persisted member identity is verified.
        self.verify_member_identities(expected)
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
        if not (PROJECT / "data" / "cluster-recovery" / "cluster.json").is_file():
            self.configure_autorecovery()
        self.start_services()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=PROJECT / ".env.docker")
    parser.add_argument("--docker-context", default=os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else ""))
    parser.add_argument("--keycloak", action="store_true", help="Carica l'overlay Keycloak e il suo ambiente preparato")
    parser.add_argument("--keycloak-groups", action="store_true", help="Conserva la sincronizzazione gruppi Keycloak; richiede --keycloak")
    parser.add_argument("--tickets", action="store_true", help="Carica anche Punto Tickets e, se selezionato, il suo overlay Keycloak")
    parser.add_argument("--keycloak-env-file", type=Path, help="Ambiente Keycloak alternativo, usato solo con --keycloak")
    parser.add_argument("--tickets-env-file", type=Path, help="Ambiente Tickets alternativo, usato solo con --tickets")
    parser.add_argument("action", choices=("init", "status", "backup", "recover", "start", "enable-autorecovery"))
    args = parser.parse_args()
    try:
        if args.keycloak_groups and not args.keycloak:
            raise ClusterError("--keycloak-groups richiede --keycloak.")
        if args.keycloak_env_file and not args.keycloak:
            raise ClusterError("--keycloak-env-file richiede la selezione esplicita --keycloak.")
        if args.tickets_env_file and not args.tickets:
            raise ClusterError("--tickets-env-file richiede la selezione esplicita --tickets.")
        with ExitStack() as cleanup:
            mutating = args.action in ("init", "recover", "start", "enable-autorecovery")
            if mutating:
                STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
                # A distinct host mutex avoids self-deadlock on Linux, where
                # the helper and host otherwise contend on the same inode.
                lock_path = STATE_PATH.parent / "cluster-host.lock"
                regular_file_or_missing(lock_path, "File lock del cluster non regolare.")
                lock = cleanup.enter_context(lock_path.open("a"))
                if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
                    raise ClusterError("File lock del cluster non regolare.")
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    raise ClusterError("Un'altra inizializzazione o procedura di recupero è già in corso.") from None
            if args.action == "init":
                ensure_secrets(args.env_file)
            cluster = Cluster(args.env_file, args.docker_context, keycloak=args.keycloak, tickets=args.tickets,
                              keycloak_groups=args.keycloak_groups,
                              keycloak_env_file=args.keycloak_env_file, tickets_env_file=args.tickets_env_file)
            if mutating:
                cleanup.callback(cluster.release_maintenance_lock)
                cluster.acquire_maintenance_lock()
            if args.action == "backup":
                online = [node for node in NODES if cluster.local_state(node) == "ONLINE"]
                cluster.backup(online[0] if online else "db")
            elif args.action == "enable-autorecovery":
                cluster.enable_autorecovery()
            else:
                getattr(cluster, args.action)()
        return 0
    except Exception as error:
        # Never output API payloads, SQL text or passwords, even on unexpected errors.
        message = str(error) if isinstance(error, ClusterError) else "Operazione interrotta; verifica i servizi e il file delle variabili."
        print("Errore: " + message, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
