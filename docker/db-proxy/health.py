#!/usr/bin/env python3
"""Authenticated Group Replication checks on loopback-only HTTP endpoints."""

import concurrent.futures
import json
import os
import signal
import subprocess
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


NODES = (("db", 9201), ("db2", 9202), ("db3", 9203))
QUERY = """
SELECT (
  @@GLOBAL.read_only = 0
  AND @@GLOBAL.super_read_only = 0
  AND @@GLOBAL.offline_mode = 0
  AND @@GLOBAL.group_replication_single_primary_mode = 0
  AND EXISTS (
    SELECT 1 FROM performance_schema.replication_group_members
    WHERE MEMBER_ID = @@GLOBAL.server_uuid
      AND MEMBER_STATE = 'ONLINE' AND MEMBER_ROLE = 'PRIMARY'
  )
  AND (
    SELECT COUNT(*) FROM performance_schema.replication_group_members
    WHERE MEMBER_STATE = 'ONLINE'
  ) >= 2
);
"""


def writer_ready(host):
    password = os.environ.get("CLUSTER_MONITOR_PASSWORD", "")
    if not password:
        return False
    child_env = os.environ.copy()
    child_env["MYSQL_PWD"] = password
    command = [
        "mariadb", "--protocol=TCP", "--host=" + host, "--port=3306",
        "--user=" + os.environ.get("CLUSTER_MONITOR_USER", "punto_monitor"),
        # MySQL's container generates its own internal certificate. Keep the
        # health-query connection encrypted on the private Docker network.
        "--ssl", "--skip-ssl-verify-server-cert",
        "--connect-timeout=2", "--batch", "--skip-column-names", "--raw",
        "--execute=" + QUERY,
    ]
    try:
        result = subprocess.run(command, env=child_env, capture_output=True,
                                text=True, timeout=4, check=False)
        return result.returncode == 0 and result.stdout.strip() == "1"
    except (OSError, subprocess.TimeoutExpired):
        return False


class HealthHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/health":
            self.send_error(404)
            return
        ready = writer_ready(self.server.mysql_host)
        body = json.dumps({"node": self.server.mysql_host, "ready": ready},
                          separators=(",", ":")).encode("utf-8")
        self.send_response(200 if ready else 503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, format_string, *args):
        pass


def endpoint_ready(node):
    _, port = node
    try:
        request = urllib.request.Request(f"http://127.0.0.1:{port}/health")
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(request, timeout=5) as response:
            return response.status == 200
    except (OSError, urllib.error.URLError):
        return False


def cluster_ready():
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(NODES)) as pool:
        return sum(pool.map(endpoint_ready, NODES)) >= 2


def serve():
    if not os.environ.get("CLUSTER_MONITOR_PASSWORD"):
        print("CLUSTER_MONITOR_PASSWORD obbligatoria.", file=sys.stderr)
        return 1
    servers = []
    threads = []
    try:
        for host, port in NODES:
            server = ThreadingHTTPServer(("127.0.0.1", port), HealthHandler)
            server.daemon_threads = True
            server.mysql_host = host
            servers.append(server)
        for server in servers:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            threads.append(thread)
        stopped = threading.Event()
        signal.signal(signal.SIGTERM, lambda *_: stopped.set())
        signal.signal(signal.SIGINT, lambda *_: stopped.set())
        while not stopped.wait(1):
            if any(not thread.is_alive() for thread in threads):
                print("Endpoint del monitor MySQL non disponibile.", file=sys.stderr)
                return 1
        return 0
    finally:
        for index, server in enumerate(servers):
            if index < len(threads):
                server.shutdown()
            server.server_close()


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "ready":
        sys.exit(0 if cluster_ready() else 1)
    if len(sys.argv) == 2 and sys.argv[1] == "serve":
        sys.exit(serve())
    print("Uso: punto-db-health.py serve|ready", file=sys.stderr)
    sys.exit(2)
