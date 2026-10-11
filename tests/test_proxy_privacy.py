"""Preserve NPM access controls while excluding OAuth callback secrets from logs.

The isolated Nginx runtime test is opt-in and does not touch the running stack:
PUNTO_DOCKER_TESTS=1 PUNTO_PROXY_IMAGE=punto/proxy-web:local ...
"""
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "docker/nginx-proxy-manager/proxy-privacy.mjs"
CALLBACK = "/api/calendar/oauth/callback"
FIXTURE = '''# Generated NPM app host
map $scheme $hsts_header { https "max-age=63072000; preload"; }
server {
  set $forward_scheme http;
  set $server "app";
  set $port 8080;
  access_log /tmp/access.log proxy;
  error_log /tmp/error.log warn;
  location / {
    auth_basic "Authorization required";
    auth_basic_user_file /data/access/9;
    proxy_set_header Authorization "";
    allow 192.0.2.0/24;
    deny all;
    satisfy all;
    add_header Strict-Transport-Security $hsts_header always;
    proxy_set_header Host $host;
    include conf.d/include/proxy.conf;
  }
  include /data/nginx/custom/server_proxy[.]conf;
}
'''


class ProxyPrivacyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.node = os.environ.get("PUNTO_NODE_BINARY") or shutil.which("node")
        if cls.node is None:
            raise unittest.SkipTest("Node required for NPM generated-config privacy filter")

    def protect(self, source=FIXTURE, custom_directory=None, *, valid=True):
        script = '''import fs from "node:fs";
import { protectProxyConfig } from %s;
const input = JSON.parse(fs.readFileSync(0, "utf8"));
try { process.stdout.write(protectProxyConfig(input.source, input.options)); }
catch(error) { console.error(error.message); process.exitCode = 1; }
''' % json.dumps(HELPER.as_uri())
        result = subprocess.run([self.node, "--input-type=module", "-e", script],
                                input=json.dumps({"source": source, "options": {
                                    "customDirectory": str(custom_directory or "/nonexistent-punto-fixture")}}),
                                capture_output=True, text=True, timeout=5)
        if valid:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsupported app proxy configuration", result.stderr)
        return result.stdout

    def test_exact_callback_clones_auth_acl_headers_and_tls_policy(self):
        result = self.protect()
        self.assertIn("location = " + CALLBACK, result)
        self.assertEqual(result.count('auth_basic "Authorization required";'), 2)
        self.assertEqual(result.count('proxy_set_header Authorization "";'), 2)
        self.assertEqual(result.count('allow 192.0.2.0/24;'), 2)
        self.assertEqual(result.count('deny all;'), 2)
        self.assertEqual(result.count('satisfy all;'), 2)
        self.assertEqual(result.count('add_header Strict-Transport-Security $hsts_header always;'), 2)
        self.assertEqual(result.count('include conf.d/include/proxy.conf;'), 2)
        self.assertEqual(result.count('access_log off;'), 1)
        self.assertEqual(result.count('error_log /dev/null crit;'), 1)
        self.assertEqual(self.protect(result), result)

    def test_non_app_hosts_are_unchanged(self):
        other = FIXTURE.replace('set $server "app";', 'set $server "other-service";')
        self.assertEqual(self.protect(other), other)
        other_port = FIXTURE.replace('set $port 8080;', 'set $port 8088;')
        self.assertEqual(self.protect(other_port), other_port)

    def test_conflicting_custom_locations_and_internal_redirects_fail_closed(self):
        for location in ("location /api { deny all; }", "location ^~ /api/calendar { deny all; }",
                         "location ~ \\.php$ { deny all; }", "location = " + CALLBACK + " { deny all; }"):
            with self.subTest(location=location):
                self.protect(FIXTURE.replace("  location / {", location + "\n  location / {"), valid=False)
        for statement in ("rewrite ^ /other last;", "try_files $uri /other;", "error_page 502 /other;",
                          "include /data/nginx/custom/unknown.conf;"):
            with self.subTest(statement=statement):
                self.protect(FIXTURE.replace("    auth_basic", "    " + statement + "\n    auth_basic"), valid=False)
        self.assertIn("location = " + CALLBACK,
                      self.protect(FIXTURE.replace("  location / {", "location /other { deny all; }\n  location / {")))
        self.protect(FIXTURE.replace("  location / {", "include /unknown-server-policy.conf;\n  location / {"), valid=False)
        self.protect(FIXTURE.replace("  location / {", "error_page 502 /other;\n  location / {"), valid=False)

    def test_custom_server_include_cannot_hide_callback_access_restrictions(self):
        with tempfile.TemporaryDirectory(prefix="punto-proxy-custom-") as directory:
            custom = Path(directory) / "server_proxy.conf"
            custom.write_text("location /api { deny all; }", encoding="utf-8")
            self.protect(custom_directory=directory, valid=False)
            custom.write_text("include /unknown.conf;", encoding="utf-8")
            self.protect(custom_directory=directory, valid=False)
            custom.write_text("# No route changes\nadd_header X-Policy present;", encoding="utf-8")
            self.assertIn("location = " + CALLBACK, self.protect(custom_directory=directory))

    def test_existing_marker_must_match_current_root_and_cannot_be_forged(self):
        result = self.protect()
        self.protect(result.replace("    deny all;", "    allow all;", 1), valid=False)
        self.protect(result.replace("access_log off;", "access_log /tmp/unsafe.log proxy;"), valid=False)
        self.protect(FIXTURE + "\n# punto-oauth-callback-private-v1\n", valid=False)

    def test_startup_migration_preserves_mode_backup_and_is_idempotent(self):
        with tempfile.TemporaryDirectory(prefix="punto-proxy-migrate-") as directory:
            config = Path(directory) / "1.conf"
            config.write_text(FIXTURE, encoding="utf-8")
            config.chmod(0o660)
            arguments = [self.node, str(HELPER), directory, "/nonexistent-punto-fixture"]
            first = subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=5)
            self.assertIn("1 app proxy", first.stdout)
            self.assertEqual(config.stat().st_mode & 0o777, 0o660)
            self.assertEqual(Path(str(config) + ".before-oauth-privacy").read_text(), FIXTURE)
            self.assertEqual(Path(str(config) + ".before-oauth-privacy").stat().st_mode & 0o777, 0o600)
            after = config.read_text()
            second = subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=5)
            self.assertIn("0 app proxy", second.stdout)
            self.assertEqual(config.read_text(), after)
            # A failure in a second host is detected before modifying any host.
            config.write_text(FIXTURE)
            (Path(directory) / "2.conf").write_text(FIXTURE.replace("location / {", "location /api {"))
            failed = subprocess.run(arguments, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(config.read_text(), FIXTURE)

    def test_root_logging_overrides_are_replaced_only_in_callback(self):
        source = FIXTURE.replace("    auth_basic", "    access_log /tmp/custom.log proxy;\n    error_log /tmp/custom-error.log warn;\n    auth_basic", 1)
        result = self.protect(source)
        self.assertEqual(result.count("access_log /tmp/custom.log proxy;"), 1)
        self.assertEqual(result.count("error_log /tmp/custom-error.log warn;"), 1)
        self.assertEqual(self.protect(result), result)


@unittest.skipUnless(os.environ.get("PUNTO_DOCKER_TESTS") == "1", "isolated Docker privacy test opt-in")
class ProxyPrivacyNginxTests(unittest.TestCase):
    setUpClass = classmethod(ProxyPrivacyTests.setUpClass.__func__)
    protect = ProxyPrivacyTests.protect
    def test_runtime_callback_suppresses_access_and_upstream_errors_preserving_acl(self):
        docker = shutil.which("docker") or "/Applications/Docker.app/Contents/Resources/bin/docker"
        if not Path(docker).exists():
            self.skipTest("Docker unavailable")
        image = os.environ.get("PUNTO_PROXY_IMAGE") or "punto/proxy-web:" + os.environ.get("PUNTO_IMAGE_TAG", "local")
        marker = "PUNTO_OAUTH_LOG_FIXTURE_30f4b1"
        with tempfile.TemporaryDirectory(prefix="punto-nginx-privacy-") as directory:
            data = Path(directory)
            with closing(socket.socket()) as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            with closing(socket.socket()) as probe:
                probe.bind(("127.0.0.1", 0))
                denied_port = probe.getsockname()[1]
            simple = '''server {
  listen 8080;
  set $server "app";
  set $port 8080;
  access_log /fixture/access.log proxy;
  error_log /fixture/error.log warn;
  location / {
    proxy_set_header Host $host;
    proxy_pass http://127.0.0.1:1;
  }
}
server {
  listen 8081;
  location / { deny all; }
}
'''
            # Each app server is protected independently. The denied server has
            # the same inherited root ACL and therefore cannot bypass it.
            deny = simple.split("\nserver {\n  listen 8081;")[0].replace("listen 8080;", "listen 8081;").replace(
                "    proxy_set_header Host $host;", "    deny all;\n    proxy_set_header Host $host;")
            private = self.protect(simple.split("\nserver {\n  listen 8081;")[0]) + self.protect(deny)
            formats = (ROOT / "docker/nginx-proxy-manager/log-proxy.conf").read_text().split("access_log /data/")[0]
            config = 'user root;\ndaemon off;\npid /tmp/punto-nginx.pid;\nerror_log /fixture/master-error.log warn;\nevents {}\nhttp {\naccess_log off;\nclient_body_temp_path /tmp/client_temp;\nproxy_temp_path /tmp/proxy_temp;\nfastcgi_temp_path /tmp/fastcgi_temp;\nuwsgi_temp_path /tmp/uwsgi_temp;\nscgi_temp_path /tmp/scgi_temp;\n' + formats + private + '\n}\n'
            (data / "nginx.conf").write_text(config, encoding="utf-8")
            migration = data / "proxy_host"
            migration.mkdir()
            original = migration / "1.conf"
            original.write_text(simple.split("\nserver {\n  listen 8081;")[0], encoding="utf-8")
            original.chmod(0o660)
            result = subprocess.run([docker, "run", "-d", "--network", "bridge",
                                     "-p", "127.0.0.1:%d:8080" % port,
                                     "-p", "127.0.0.1:%d:8081" % denied_port,
                                     "--mount", "type=bind,src=%s,dst=/fixture" % directory,
                                     "--entrypoint", "nginx", image, "-e", "/dev/stderr", "-c", "/fixture/nginx.conf"],
                                    check=True, capture_output=True, text=True, timeout=30)
            container = result.stdout.strip()
            try:
                # Verify this very image has both the generation filter and the
                # pre-Nginx hook; then exercise its migration implementation.
                wiring = """import fs from 'node:fs';
const writer=fs.readFileSync('/app/internal/nginx.js','utf8');
const startup=fs.readFileSync('/etc/s6-overlay/s6-rc.d/prepare/00-all.sh','utf8');
if(!writer.includes('protectProxyConfig(config_text)') ||
   !startup.includes('node /app/punto-proxy-privacy.mjs') ||
   startup.indexOf('node /app/punto-proxy-privacy.mjs') > startup.indexOf('90-banner.sh'))
  throw new Error('NPM privacy wiring missing');"""
                subprocess.run([docker, "exec", container, "node", "--input-type=module", "-e", wiring],
                               check=True, capture_output=True, text=True, timeout=5)
                subprocess.run([docker, "exec", container, "node", "/app/punto-proxy-privacy.mjs",
                                "/fixture/proxy_host", "/nonexistent-punto-fixture"],
                               check=True, capture_output=True, text=True, timeout=5)
                self.assertIn("# punto-oauth-callback-private-v1", original.read_text())
                self.assertEqual(original.stat().st_mode & 0o777, 0o660)
                self.assertTrue(Path(str(original) + ".before-oauth-privacy").exists())
                def request(endpoint, request_port, *, sensitive=False):
                    connection = http.client.HTTPConnection("127.0.0.1", request_port, timeout=3)
                    connection.request("GET", endpoint, headers={"Host": "privacy.example.test",
                        "Referer": marker if sensitive else "fixture-referer",
                        "User-Agent": marker if sensitive else "fixture-agent"})
                    response = connection.getresponse()
                    status = response.status
                    response.read()
                    connection.close()
                    return status
                for _ in range(50):
                    try:
                        if request("/fixture-ready", port) == 502:
                            break
                    except (OSError, http.client.HTTPException):
                        pass
                    time.sleep(0.1)
                else:
                    diagnostic = subprocess.run([docker, "logs", container], capture_output=True, text=True, timeout=5)
                    self.fail("Disposable Nginx did not start: " + diagnostic.stdout + diagnostic.stderr)
                self.assertEqual(request(CALLBACK + "?code=" + marker + "&state=" + marker, port, sensitive=True), 502)
                self.assertEqual(request(CALLBACK + "?code=" + marker, denied_port, sensitive=True), 403)
                self.assertEqual(request("/ordinary?normal_marker=ordinary-diagnostic", denied_port), 403)
                self.assertEqual(request("/ordinary?normal_marker=ordinary-diagnostic", port), 502)
                time.sleep(0.2)
                logs = "\n".join(log.read_text(errors="replace") for log in data.glob("*.log"))
                self.assertNotIn(marker, logs)
                self.assertIn("ordinary-diagnostic", logs, "Normal upstream failure diagnostics were suppressed")
                access = (data / "access.log").read_text()
                self.assertIn('"/ordinary"', access)
                self.assertNotIn("fixture-referer", access)
                self.assertNotIn("fixture-agent", access)
            finally:
                subprocess.run([docker, "rm", "-f", container], capture_output=True, text=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
