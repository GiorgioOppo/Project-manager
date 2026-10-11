"""Opt-in Squid destination ACL check in an isolated disposable container.

Run: PUNTO_SQUID_POLICY_TEST=1 python3 tests/test_squid_policy.py -v
Build the Compose proxy-egress image first. No stack containers are changed, host
paths mounted, credentials injected or external network connections attempted.
"""

import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    from manage_cluster import Cluster
finally:
    sys.path.pop(0)


# The ACLs and allow/deny lists are those baked into the production image.
# Only addresses needed to run on an isolated container's loopback are changed.
PRIVATE_ANSWER_CHECK = r"""set -eu
test "$(id -u)" = 10002
grep -qx 'http_port 172.30.40.2:3128' /etc/squid/squid.conf
grep -qx 'tcp_outgoing_address 172.30.50.2' /etc/squid/squid.conf
grep -qx 'acl app_clients src 172.30.40.10/32' /etc/squid/squid.conf
grep -qx 'acl proxy_clients src 172.30.40.10/32 172.30.40.11/32 172.30.40.12/32' /etc/squid/squid.conf
grep -qx 'accounts.google.com' /etc/squid/allowed-domains.txt
sed \
  -e 's/^http_port 172\.30\.40\.2:3128$/http_port 127.0.0.1:3128/' \
  -e 's/^tcp_outgoing_address 172\.30\.50\.2$/tcp_outgoing_address 127.0.0.1/' \
  -e 's|^acl app_clients src 172\.30\.40\.10/32$|acl app_clients src 127.0.0.1/32|' \
  -e 's|^acl proxy_clients src 172\.30\.40\.10/32 172\.30\.40\.11/32 172\.30\.40\.12/32$|acl proxy_clients src 127.0.0.1/32|' \
  /etc/squid/squid.conf > /tmp/check-squid.conf
printf '\nhosts_file /tmp/check-hosts\n' >> /tmp/check-squid.conf
# Give this ACL a distinct fixture-only status. A generic 403 could otherwise
# pass because an earlier source, port, or domain restriction denied the call.
printf 'deny_info 451:ERR_ACCESS_DENIED denied_destinations\n' >> /tmp/check-squid.conf
printf '127.0.0.1 accounts.google.com\n' > /tmp/check-hosts
squid -N -d 1 -f /tmp/check-squid.conf > /tmp/check-squid.log 2>&1 &
squid_pid=$!
trap 'kill "$squid_pid" 2>/dev/null || true; wait "$squid_pid" 2>/dev/null || true' EXIT
if ! timeout 5 sh -c '
  until [ "$(curl --silent --connect-timeout 0.3 --max-time 0.5 \
    --proxy http://127.0.0.1:3128 --noproxy "" --output /dev/null \
    --write-out "%{http_code}" http://startup.invalid/ || true)" = 403 ]; do
    sleep 0.1
  done'; then
  cat /tmp/check-squid.log
  exit 1
fi
curl_status=0
result=$(curl --silent --show-error --connect-timeout 2 --max-time 4 \
  --proxy http://127.0.0.1:3128 --noproxy "" --output /dev/null \
  --write-out '%{http_connect}' https://accounts.google.com/ \
  2>/tmp/check-curl.log) || curl_status=$?
if [ "$curl_status" = 0 ] || [ "$result" != 451 ]; then
  cat /tmp/check-squid.log /tmp/check-curl.log
  printf 'Expected destination ACL CONNECT 451, received %s (curl exit %s)\n' "$result" "$curl_status"
  exit 1
fi
printf 'Allowlisted hostname resolving to loopback rejected by denied_destinations: CONNECT 451\n'
"""


@unittest.skipUnless(os.environ.get("PUNTO_SQUID_POLICY_TEST") == "1",
                     "Set PUNTO_SQUID_POLICY_TEST=1 for isolated Squid destination policy checks")
class SquidDestinationPolicyTests(unittest.TestCase):
    def test_allowlisted_host_resolving_to_private_address_is_denied(self):
        context = os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else "")
        cluster = Cluster(Path(os.environ.get("PUNTO_CLUSTER_ENV_FILE", ROOT / ".env.docker")), context)
        docker = cluster.command[:cluster.command.index("compose")]
        # Inspect the configured image tag, not an existing container's old
        # image ID: a rebuild may precede the deployment of that new image.
        images = cluster.run(("config", "--images", "proxy-egress"), timeout=20).stdout.decode("ascii").split()
        self.assertEqual(len(set(images)), 1, "The Compose proxy-egress service must name exactly one image")
        inspected = subprocess.run(docker + ["image", "inspect", images[0], "--format", "{{.Id}}"],
                                   capture_output=True, text=True, env=cluster.environment,
                                   cwd=ROOT, timeout=20)
        self.assertEqual(inspected.returncode, 0, "Build the Compose proxy-egress image before running this test")
        image = inspected.stdout.strip()
        self.assertRegex(image, r"\A(?:sha256:)?[a-f0-9]{12,64}\Z", "Unexpected Squid image identifier")
        name = "punto-squid-policy-" + uuid.uuid4().hex
        try:
            executed = subprocess.run(docker + [
                "run", "-i", "--rm", "--pull=never", "--name", name,
                "--network", "none", "--read-only", "--cap-drop", "ALL",
                "--security-opt", "no-new-privileges:true",
                "--tmpfs", "/tmp:rw,noexec,nosuid,size=16m,mode=1777",
                "--entrypoint", "sh", image, "-s",
            ], input=PRIVATE_ANSWER_CHECK, capture_output=True, text=True,
                env=cluster.environment, cwd=ROOT, timeout=25)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertIn("Allowlisted hostname resolving to loopback rejected by denied_destinations: CONNECT 451", executed.stdout)
        finally:
            # --rm handles normal completion; remove only this named disposable
            # fixture if startup/probe timeout interrupted the Docker client.
            subprocess.run(docker + ["rm", "-f", name], capture_output=True,
                           env=cluster.environment, cwd=ROOT, timeout=15)


if __name__ == "__main__":
    unittest.main()
