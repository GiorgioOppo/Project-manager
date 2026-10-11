#!/usr/bin/env python3
"""Recover only the provisioned, single-host three-member Group Replication group.

The controller has no Docker access and never provisions schemas, resets GTIDs,
forces quorum, or replaces a member. Missing observations are errors, not OFFLINE.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


NODES = ("db", "db2", "db3")
UUID_RE = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")
GTID_RE = re.compile(r"[0-9a-fA-F:,\-\s]*\Z")
MAX_SQL_OUTPUT = 4 * 1024 * 1024

SNAPSHOT_SQL = """SELECT JSON_OBJECT(
 'server_uuid',@@GLOBAL.server_uuid,
 'server_id',@@GLOBAL.server_id,
 'group_name',@@GLOBAL.group_replication_group_name,
 'single_primary_mode',@@GLOBAL.group_replication_single_primary_mode,
 'gtid_mode',@@GLOBAL.gtid_mode,
 'enforce_update_everywhere',@@GLOBAL.group_replication_enforce_update_everywhere_checks,
 'read_only',@@GLOBAL.read_only,
 'super_read_only',@@GLOBAL.super_read_only,
 'offline_mode',@@GLOBAL.offline_mode,
 'bootstrap',@@GLOBAL.group_replication_bootstrap_group,
 'force_members',@@GLOBAL.group_replication_force_members,
 'local_state',COALESCE((SELECT MEMBER_STATE FROM performance_schema.replication_group_members
                        WHERE MEMBER_ID=@@GLOBAL.server_uuid),'OFFLINE'),
 'view_id',COALESCE((SELECT VIEW_ID FROM performance_schema.replication_group_member_stats
                    WHERE MEMBER_ID=@@GLOBAL.server_uuid),''),
 'members',COALESCE((SELECT JSON_ARRAYAGG(JSON_OBJECT('id',MEMBER_ID,'state',MEMBER_STATE,'role',MEMBER_ROLE))
                     FROM performance_schema.replication_group_members),JSON_ARRAY()),
 'active_channels',(SELECT COUNT(*) FROM performance_schema.replication_connection_status
                     WHERE CHANNEL_NAME IN ('group_replication_applier','group_replication_recovery')
                       AND SERVICE_STATE <> 'OFF') +
                   (SELECT COUNT(*) FROM performance_schema.replication_applier_status
                     WHERE CHANNEL_NAME IN ('group_replication_applier','group_replication_recovery')
                       AND SERVICE_STATE <> 'OFF'),
 'executed',@@GLOBAL.gtid_executed,
 'certified',COALESCE((SELECT RECEIVED_TRANSACTION_SET FROM performance_schema.replication_connection_status
                       WHERE CHANNEL_NAME='group_replication_applier'),''));"""
STOP_SQL = "STOP GROUP_REPLICATION;"
START_SQL = "START GROUP_REPLICATION;"
BOOTSTRAP_ON_SQL = "SET GLOBAL group_replication_bootstrap_group=ON;"
BOOTSTRAP_OFF_SQL = "SET GLOBAL group_replication_bootstrap_group=OFF;"


class RecoveryError(Exception):
    """A fixed diagnostic code, never SQL text or connector diagnostics."""


def expected_identities(state):
    if not isinstance(state, dict) or state.get("initialized") is not True:
        raise RecoveryError("uninitialized_state")
    group = state.get("group_id")
    nodes = state.get("nodes")
    if nodes is None:
        seeded = state.get("seeded", {})
        if not isinstance(seeded, dict):
            raise RecoveryError("invalid_state")
        nodes = {"db": state.get("source_uuid"), "db2": seeded.get("db2"), "db3": seeded.get("db3")}
    if (not isinstance(group, str) or not UUID_RE.fullmatch(group)
            or not isinstance(nodes, dict) or set(nodes) != set(NODES)
            or any(not isinstance(value, str) or not UUID_RE.fullmatch(value) for value in nodes.values())
            or len({value.lower() for value in nodes.values()}) != 3):
        raise RecoveryError("invalid_state")
    return group.lower(), {node: value.lower() for node, value in nodes.items()}


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


class SqlClient:
    def __init__(self, *, password_file, ca_file, user="punto_autorecovery", timeout=15):
        self.password_file = Path(password_file)
        self.ca_file = str(ca_file)
        self.user = user
        self.timeout = timeout
        if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", user):
            raise RecoveryError("invalid_sql_user")

    def __call__(self, node, query):
        if node not in NODES:
            raise RecoveryError("invalid_node")
        try:
            password = self.password_file.read_text(encoding="utf-8").rstrip("\r\n")
            if not password or len(password) > 1024 or "\x00" in password or "\n" in password or "\r" in password:
                raise RecoveryError("invalid_password_file")
            environment = os.environ.copy()
            environment["MYSQL_PWD"] = password
            command = ["mariadb", "--no-defaults", "--protocol=TCP", "--host=" + node,
                       "--port=3306", "--user=" + self.user, "--batch", "--raw", "--skip-column-names",
                       "--ssl", "--ssl-verify-server-cert", "--ssl-ca=" + self.ca_file,
                       "--connect-timeout=5"]
            result = subprocess.run(command, input=query.encode("utf-8"), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=environment, timeout=self.timeout, check=False)
            if result.returncode:
                code = re.search(rb"ERROR\s+(\d{1,5})", result.stderr)
                raise RecoveryError("sql_failed_" + (code.group(1).decode("ascii") if code else "unknown"))
            if len(result.stdout) > MAX_SQL_OUTPUT:
                raise RecoveryError("sql_output_too_large")
            return result.stdout.decode("utf-8").strip()
        except (OSError, subprocess.TimeoutExpired, UnicodeError):
            raise RecoveryError("sql_unavailable") from None


class Recovery:
    def __init__(self, sql, state, *, now=time.monotonic, sleep=time.sleep,
                 lock_path="/run/punto-cluster.lock",
                 journal_path="/var/lib/punto-cluster/recovery.json",
                 status_path="/tmp/punto-cluster-status.json", stable_seconds=10):
        self.sql = sql
        self.group_id, self.identities = expected_identities(state)
        self.now, self.sleep = now, sleep
        self.lock_path, self.journal_path = Path(lock_path), Path(journal_path)
        self.status_path = Path(status_path) if status_path is not None else None
        self.stable_seconds = max(10, stable_seconds)
        self.offline_since = None
        self.offline_signature = None
        self.cleaned_journal = False
        self.partial_snapshots = {}

    def _status(self, state, reason, *, online=0, ready=False):
        result = {"state": state, "reason": reason, "online": online, "ready": ready,
                  "checked_at": self.now(), "updated_at": int(time.time())}
        if self.status_path is not None:
            atomic_json(self.status_path, result)
        return result

    def _snapshots(self):
        snapshots = {}
        errors = []
        self.partial_snapshots = {}
        for index, node in enumerate(NODES, 1):
            try:
                output = self.sql(node, SNAPSHOT_SQL)
            except RecoveryError as error:
                errors.append(error)
                continue
            try:
                row = json.loads(output)
            except (ValueError, TypeError):
                raise RecoveryError("invalid_snapshot") from None
            if not isinstance(row, dict):
                raise RecoveryError("invalid_snapshot")
            if (not isinstance(row.get("server_uuid"), str)
                    or not isinstance(row.get("group_name"), str)
                    or row["server_uuid"].lower() != self.identities[node]
                    or row.get("server_id") != index
                    or row["group_name"].lower() != self.group_id
                    or row.get("single_primary_mode") != 0 or row.get("gtid_mode") != "ON"
                    or row.get("enforce_update_everywhere") != 1):
                raise RecoveryError("identity_or_configuration_mismatch")
            for key in ("read_only", "super_read_only", "offline_mode", "bootstrap"):
                if type(row.get(key)) not in (int, bool) or row[key] not in (0, 1):
                    raise RecoveryError("invalid_snapshot")
            if (not isinstance(row.get("active_channels"), int) or row["active_channels"] < 0
                    or not isinstance(row.get("view_id"), str)
                    or not isinstance(row.get("force_members"), str)
                    or row.get("local_state") not in ("ONLINE", "OFFLINE", "RECOVERING", "ERROR", "UNREACHABLE")):
                raise RecoveryError("invalid_snapshot")
            for key in ("executed", "certified"):
                if (not isinstance(row.get(key), str) or len(row[key]) > 1024 * 1024
                        or not GTID_RE.fullmatch(row[key])):
                    raise RecoveryError("invalid_gtid_set")
            members = row.get("members")
            if not isinstance(members, list) or len(members) > 3:
                raise RecoveryError("unexpected_membership")
            seen = set()
            for member in members:
                # Some MySQL releases expose an empty identity in the sole
                # OFFLINE placeholder row. Server UUID was verified separately.
                if (isinstance(member, dict) and len(members) == 1 and row["local_state"] == "OFFLINE"
                        and member.get("id") == "" and member.get("state") == "OFFLINE"
                        and member.get("role") == ""):
                    member["id"] = self.identities[node]
                if (not isinstance(member, dict) or member.get("id") not in self.identities.values()
                        or member["id"] in seen
                        or member.get("state") not in ("ONLINE", "OFFLINE", "RECOVERING", "ERROR", "UNREACHABLE")
                        or member.get("role") not in ("PRIMARY", "SECONDARY", "")):
                    raise RecoveryError("unexpected_membership")
                seen.add(member["id"])
            if row["force_members"]:
                raise RecoveryError("forced_membership_active")
            snapshots[node] = row
        if errors:
            self.partial_snapshots = snapshots
            raise errors[0]
        return snapshots

    def _journal(self):
        if not self.journal_path.exists():
            return None
        try:
            journal = json.loads(self.journal_path.read_text(encoding="utf-8"))
            node = journal.get("node")
            if (journal.get("version") != 1 or journal.get("phase") not in ("bootstrap_pending", "joining")
                    or node not in NODES or journal.get("server_uuid") != self.identities[node]
                    or journal.get("group_id") != self.group_id):
                raise ValueError
            return journal
        except (OSError, ValueError, AttributeError):
            raise RecoveryError("invalid_recovery_journal") from None

    def _save_journal(self, node, phase):
        atomic_json(self.journal_path, {"version": 1, "phase": phase, "node": node,
                                      "server_uuid": self.identities[node], "group_id": self.group_id})

    def _clear_journal(self):
        if self.journal_path.exists():
            self.journal_path.unlink()
            descriptor = os.open(self.journal_path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        self.cleaned_journal = False

    @staticmethod
    def _offline(row):
        return (row["local_state"] == "OFFLINE" and row["read_only"] == 1
                and row["super_read_only"] == 1 and row["offline_mode"] == 0
                and row["active_channels"] == 0 and row["bootstrap"] == 0
                and all(member["state"] == "OFFLINE" for member in row["members"]))

    def _quorum(self, snapshots, *, partial=False):
        online = [node for node in NODES if node in snapshots and snapshots[node]["local_state"] == "ONLINE"]
        if len(online) < 2:
            return False, online
        online_ids = {self.identities[node] for node in online}
        view_ids = set()
        memberships = set()
        for node in online:
            row = snapshots[node]
            view_ids.add(row["view_id"])
            members = row["members"]
            membership_online = {member["id"] for member in members if member["state"] == "ONLINE"}
            memberships.add(tuple(sorted((member["id"], member["state"], member["role"]) for member in members)))
            if (not row["view_id"] or row["read_only"] or row["super_read_only"] or row["offline_mode"]
                    or (membership_online != online_ids if not partial else not online_ids <= membership_online)
                    or any(member["state"] == "UNREACHABLE" for member in members)
                    or any(member["role"] != "PRIMARY" for member in members if member["state"] == "ONLINE")):
                return False, online
        return len(view_ids) == 1 and len(memberships) == 1, online

    def _founder_confirmed(self, snapshots, founder):
        row = snapshots[founder]
        online = {self.identities[node] for node in NODES if snapshots[node]["local_state"] == "ONLINE"}
        return not (row["local_state"] != "ONLINE" or row["bootstrap"] or row["offline_mode"]
                or any(current["bootstrap"] for current in snapshots.values())
                or {member["id"] for member in row["members"] if member["state"] == "ONLINE"} != online
                or any(member["state"] in ("UNREACHABLE", "ERROR") for member in row["members"])
                or (len(online) >= 2 and not self._quorum(snapshots)[0])
                or not row["view_id"] or not any(member["id"] == self.identities[founder]
                    and member["state"] == "ONLINE" and member["role"] == "PRIMARY" for member in row["members"]))

    def _resume_join(self, snapshots, founder):
        if not self._founder_confirmed(snapshots, founder):
            raise RecoveryError("bootstrap_founder_not_confirmed")
        for node in NODES:
            if node != founder and self._offline(snapshots[node]):
                # Re-observe all identities immediately before each normal START.
                current = self._snapshots()
                if not self._founder_confirmed(current, founder) or not self._offline(current[node]):
                    raise RecoveryError("state_changed_before_join")
                self.sql(node, START_SQL)

    def _bootstrap(self, snapshots):
        for node in NODES:
            self.sql(node, STOP_SQL)
        current = self._snapshots()
        if not all(self._offline(current[node]) for node in NODES):
            raise RecoveryError("group_not_stopped")
        # Preserve certified observations from before STOP as well: a stop may
        # finish applying them or change channel instrumentation. Neither case
        # must make the candidate appear less up to date than it was observed.
        sets = {node: ",".join(value for value in (snapshots[node]["executed"], snapshots[node]["certified"],
                                                   current[node]["executed"], current[node]["certified"]) if value)
                for node in NODES}
        candidates = []
        for node in NODES:
            if all(self.sql(node, "SELECT GTID_SUBSET('" + sets[other] + "','" + sets[node] + "');") == "1"
                   for other in NODES):
                candidates.append(node)
        if not candidates:
            raise RecoveryError("divergent_gtid_sets")
        founder = candidates[0]
        final = self._snapshots()
        if (not all(self._offline(final[node]) for node in NODES)
                or any(final[node]["executed"] != current[node]["executed"]
                       or final[node]["certified"] != current[node]["certified"] for node in NODES)):
            raise RecoveryError("state_changed_before_bootstrap")
        self._save_journal(founder, "bootstrap_pending")
        try:
            self.sql(founder, BOOTSTRAP_ON_SQL)
            self.sql(founder, START_SQL)
        finally:
            self.sql(founder, BOOTSTRAP_OFF_SQL)
        self._save_journal(founder, "joining")
        self.cleaned_journal = True
        self._resume_join(self._snapshots(), founder)
        return self._status("joining", "bootstrap_completed", online=1)

    def _tick_locked(self):
        snapshots = self._snapshots()
        journal = self._journal()
        if ((journal is not None and not self.cleaned_journal)
                or any(row["bootstrap"] for row in snapshots.values())):
            # A crash can leave an in-memory bootstrap flag ON. Clear every
            # expected member, then inspect again; never START with this flag.
            for node in NODES:
                self.sql(node, BOOTSTRAP_OFF_SQL)
            self.cleaned_journal = True
            snapshots = self._snapshots()
            if any(row["bootstrap"] for row in snapshots.values()):
                raise RecoveryError("bootstrap_flag_not_cleared")
        quorum, online = self._quorum(snapshots)
        if quorum:
            self.offline_since = self.offline_signature = None
            if len(online) == 3:
                self._clear_journal()
                return self._status("healthy", "quorum_online", online=3, ready=True)
            for node in NODES:
                if node not in online and self._offline(snapshots[node]):
                    current = self._snapshots()
                    still_quorum, current_online = self._quorum(current)
                    if not still_quorum or current_online != online or not self._offline(current[node]):
                        raise RecoveryError("state_changed_before_join")
                    self.sql(node, START_SQL)
                    return self._status("joining", "rejoining_member", online=len(online), ready=True)
            return self._status("degraded", "waiting_for_member", online=len(online), ready=True)
        if online:
            self.offline_since = self.offline_signature = None
            if journal is not None and journal["node"] in online and len(online) == 1:
                self._resume_join(snapshots, journal["node"])
                return self._status("joining", "resuming_verified_bootstrap", online=1)
            return self._status("blocked", "no_consistent_quorum", online=len(online))
        if not all(self._offline(snapshots[node]) for node in NODES):
            self.offline_since = self.offline_signature = None
            return self._status("waiting", "members_not_safely_offline")
        if journal is not None:
            # An interrupted START may already have created a new group. With
            # no verified ONLINE founder, an OFFLINE view cannot prove whether
            # bootstrap completed; retain the journal for manual inspection.
            self.offline_since = self.offline_signature = None
            return self._status("blocked", "bootstrap_outcome_unknown")
        signature = tuple((node, snapshots[node]["executed"], snapshots[node]["certified"]) for node in NODES)
        if self.offline_since is None or self.offline_signature != signature:
            self.offline_since, self.offline_signature = self.now(), signature
            return self._status("observing", "offline_stability_window")
        if self.now() - self.offline_since < self.stable_seconds:
            return self._status("observing", "offline_stability_window")
        self.offline_since = self.offline_signature = None
        return self._bootstrap(snapshots)

    def tick(self):
        descriptor = None
        try:
            descriptor = os.open(self.lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                self.offline_since = self.offline_signature = None
                return self._status("paused", "controller_lock_held")
            return self._tick_locked()
        except RecoveryError as error:
            self.offline_since = self.offline_signature = None
            quorum, online = self._quorum(self.partial_snapshots, partial=True)
            if quorum:
                return self._status("degraded", "member_unreachable_no_mutations", online=len(online), ready=True)
            return self._status("blocked", str(error))
        except OSError:
            self.offline_since = self.offline_signature = None
            return self._status("blocked", "controller_storage_unavailable")
        finally:
            if descriptor is not None:
                os.close(descriptor)

    def check(self):
        """Verify real credentials and the live quorum without locks or writes."""
        return self._quorum(self._snapshots())[0]


def healthz(path, *, now=time.monotonic):
    try:
        status = json.loads(Path(path).read_text(encoding="utf-8"))
        age = now() - status["checked_at"]
        return status.get("ready") is True and status.get("online", 0) >= 2 and 0 <= age <= 30
    except (OSError, ValueError, KeyError, TypeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("run", "healthz", "check"))
    args = parser.parse_args()
    status_path = os.environ.get("CLUSTER_STATUS_FILE", "/tmp/punto-cluster-status.json")
    if args.mode == "healthz":
        return 0 if healthz(status_path) else 1
    try:
        state = json.loads(Path(os.environ.get("CLUSTER_STATE_FILE", "/etc/punto/cluster-state.json")).read_text(encoding="utf-8"))
        sql = SqlClient(password_file=os.environ.get("CLUSTER_PASSWORD_FILE", "/run/secrets/cluster-password"),
                        ca_file=os.environ.get("CLUSTER_CA_FILE", "/certs/ca.pem"),
                        user=os.environ.get("CLUSTER_USER", "punto_autorecovery"))
        recovery = Recovery(sql, state,
                            lock_path=os.environ.get("CLUSTER_LOCK_FILE", "/run/punto-cluster.lock"),
                            journal_path=os.environ.get("CLUSTER_JOURNAL_FILE", "/var/lib/punto-cluster/recovery.json"),
                            status_path=status_path)
        if args.mode == "check":
            ready = recovery.check()
            print("cluster_recovery " + ("check_ready" if ready else "check_not_ready"), flush=True)
            return 0 if ready else 1
        poll = float(os.environ.get("CLUSTER_POLL_SECONDS", "5"))
        if not 5 <= poll <= 60:
            raise ValueError
    except (OSError, ValueError, RecoveryError):
        print("cluster_recovery configuration_invalid", flush=True)
        return 1
    stopped = False

    def stop(_signal, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    previous = None
    while not stopped:
        try:
            result = recovery.tick()
            message = (result["state"], result["reason"], result["online"])
            if message != previous:
                print("cluster_recovery state=%s reason=%s online=%d" % message, flush=True)
                previous = message
            delay = min(30, poll * 3) if result["state"] == "blocked" else poll
        except Exception:
            print("cluster_recovery controller_error", flush=True)
            delay = 30
        deadline = time.monotonic() + delay
        while not stopped and time.monotonic() < deadline:
            time.sleep(min(1, max(0, deadline - time.monotonic())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
