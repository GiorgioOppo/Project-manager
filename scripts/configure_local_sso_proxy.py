#!/usr/bin/env python3
"""Configure HTTPS for the three explicitly selected local SSO application names."""

import argparse
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from urllib.parse import quote

import configure_keycloak as configuration
from configure_proxy import ConfigurationError, NpmClient


DOMAINS = ("punto.localhost", "ticket.localhost", "auth.localhost")
CERTIFICATE_NAME = "Punto HTTPS locale"
KEYCLOAK_ADVANCED = "\n".join((
    "proxy_ssl_server_name on;",
    "proxy_ssl_name keycloak;",
    "proxy_ssl_verify on;",
    "proxy_ssl_trusted_certificate /etc/punto/keycloak-ca.crt;",
    "proxy_ssl_verify_depth 2;",
))
UPSTREAMS = {
    "punto.localhost": ("http", "app", 8080, ""),
    "ticket.localhost": ("http", "tickets", 8083, ""),
    "auth.localhost": ("https", "keycloak", 8443, KEYCLOAK_ADVANCED),
}


def verify_web_tls(directory):
    cert = configuration.regular_file(directory / "local-web.crt")
    key = configuration.regular_file(directory / "local-web.key")
    configuration.openssl("x509", "-in", cert, "-noout", "-checkend", "86400")
    if configuration.openssl("x509", "-in", cert, "-pubkey", "-noout") != configuration.openssl("pkey", "-in", key, "-pubout"):
        raise ConfigurationError("Certificato web e chiave non corrispondono.")
    names = configuration.openssl("x509", "-in", cert, "-noout", "-ext", "subjectAltName").decode("ascii")
    alternative_names = [part.strip() for part in " ".join(names.splitlines()[1:]).split(",")]
    if set(alternative_names) != {"DNS:" + name for name in DOMAINS} or len(alternative_names) != len(DOMAINS):
        raise ConfigurationError("Il certificato web deve coprire soltanto i tre domini locali previsti.")
    for hostname in DOMAINS:
        configuration.openssl("verify", "-CAfile", directory / "ca.crt", "-verify_hostname", hostname, cert)
    key.chmod(0o600)
    cert.chmod(0o644)


def prepare_web_tls(data):
    directory = data / "tls"
    if directory.is_symlink() or not directory.is_dir():
        raise ConfigurationError("Directory TLS non valida.")
    configuration.regular_file(directory / "ca.crt")
    configuration.regular_file(directory / "ca.key")
    cert, key = directory / "local-web.crt", directory / "local-web.key"
    if any(path.exists() or path.is_symlink() for path in (cert, key)):
        verify_web_tls(directory)
        return cert, key
    with tempfile.TemporaryDirectory(prefix=".local-web-", dir=directory) as temporary:
        staging = Path(temporary)
        staging.chmod(0o700)
        configuration.openssl("req", "-new", "-newkey", "rsa:3072", "-nodes", "-sha256",
                              "-keyout", staging / "local-web.key", "-out", staging / "local-web.csr",
                              "-subj", "/O=Punto Local/CN=punto.localhost")
        extension = staging / "local-web.ext"
        extension.write_text("basicConstraints=critical,CA:FALSE\n"
                             "keyUsage=critical,digitalSignature,keyEncipherment\n"
                             "extendedKeyUsage=serverAuth\n"
                             "subjectAltName=" + ",".join("DNS:" + name for name in DOMAINS) + "\n", encoding="ascii")
        configuration.openssl("x509", "-req", "-in", staging / "local-web.csr", "-CA", directory / "ca.crt",
                              "-CAkey", directory / "ca.key", "-set_serial", "0x" + secrets.token_hex(16),
                              "-days", "365", "-sha256", "-extfile", extension, "-out", staging / "local-web.crt")
        (staging / "local-web.key").chmod(0o600)
        (staging / "local-web.crt").chmod(0o644)
        (staging / "local-web.key").replace(key)
        (staging / "local-web.crt").replace(cert)
    verify_web_tls(directory)
    return cert, key


def backup_database(source, target):
    configuration.regular_file(source)
    if target.exists() or target.is_symlink():
        configuration.regular_file(target)
        target.chmod(0o600)
        with sqlite3.connect("file:" + quote(str(target.resolve())) + "?mode=ro", uri=True) as backup:
            if backup.execute("PRAGMA quick_check").fetchone() != ("ok",) or not backup.execute(
                    "SELECT count(*) FROM sqlite_master WHERE type='table'").fetchone()[0]:
                raise ConfigurationError("Backup NPM esistente non valido; file conservato per verifica.")
        return
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    try:
        with sqlite3.connect("file:" + quote(str(source.resolve())) + "?mode=ro", uri=True) as original:
            with sqlite3.connect(target) as backup:
                original.backup(backup)
                if backup.execute("PRAGMA quick_check").fetchone() != ("ok",):
                    raise ConfigurationError("La verifica del backup SQLite non è riuscita.")
    except Exception:
        target.unlink(missing_ok=True)
        raise
    target.chmod(0o600)


def administrator_token(container, admin_id, *, context=None):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", container) or not 1 <= admin_id <= 2147483647:
        raise ConfigurationError("Container o ID amministratore non valido.")
    binary = shutil.which("docker")
    if not binary:
        raise ConfigurationError("Docker non disponibile.")
    script = '''import User from '/app/models/user.js';
import Token from '/app/models/token.js';
const adminId = Number(process.argv[2]);
const user = await User.query().findById(adminId);
if (!user || user.is_deleted || user.is_disabled || !Array.isArray(user.roles) || !user.roles.includes('admin')) {
  throw new Error('Existing active administrator required');
}
const result = await Token().create({iss: 'api', attrs: {id: user.id}, scope: ['user'], expiresIn: '5m'});
if (!result || typeof result.token !== 'string') throw new Error('Invalid token response');
process.stdout.write('PUNTO_NPM_TOKEN:' + result.token + '\\n');
process.exit(0);
'''
    command = [binary] + (["--context", context] if context else []) + [
        "exec", "-i", "--workdir", "/app", container, "node", "--input-type=module", "-", str(admin_id)]
    try:
        result = subprocess.run(command, input=script.encode("utf-8"), capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        raise ConfigurationError("Sessione proprietario NPM non creata.") from None
    if result.returncode:
        raise ConfigurationError("Sessione proprietario NPM rifiutata; nessuna password modificata.")
    tokens = re.findall(rb"PUNTO_NPM_TOKEN:([A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)", result.stdout)
    if len(tokens) != 1:
        raise ConfigurationError("NPM non ha restituito una sessione valida.")
    return tokens[0].decode("ascii")


class LocalNpmClient(NpmClient):
    def __init__(self, port, *, container="projectmanament-proxy-web-1", context=None):
        super().__init__(port)
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", container):
            raise ConfigurationError("Container NPM non valido.")
        self.container = container
        self.context = context

    def verify_public_certificate(self, certificate_id, expected_certificate):
        if not isinstance(certificate_id, int) or certificate_id <= 0:
            raise ConfigurationError("ID certificato NPM non valido.")
        binary = shutil.which("docker")
        if not binary:
            raise ConfigurationError("Docker non disponibile.")
        command = [binary] + (["--context", self.context] if self.context else []) + [
            "exec", self.container, "openssl", "x509", "-in",
            f"/data/custom_ssl/npm-{certificate_id}/fullchain.pem", "-outform", "DER"]
        try:
            result = subprocess.run(command, capture_output=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            raise ConfigurationError("Verifica del certificato pubblico NPM non completata.") from None
        expected = configuration.openssl("x509", "-in", expected_certificate, "-outform", "DER")
        if result.returncode or result.stdout != expected:
            raise ConfigurationError("Certificato pubblico NPM diverso da quello locale previsto; materiale conservato.")

    def upload_certificate(self, certificate_id, certificate, key, ca):
        boundary = "punto-" + secrets.token_hex(24)
        parts = []
        for field, path in (("certificate", certificate), ("certificate_key", key), ("intermediate_certificate", ca)):
            parts.extend((f"--{boundary}\r\n".encode("ascii"),
                          f'Content-Disposition: form-data; name="{field}"; filename="{path.name}"\r\n'.encode("ascii"),
                          b"Content-Type: application/x-pem-file\r\n\r\n", path.read_bytes(), b"\r\n"))
        parts.append(f"--{boundary}--\r\n".encode("ascii"))
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        try:
            connection.request("POST", f"/api/nginx/certificates/{certificate_id}/upload", body=b"".join(parts), headers={
                "Authorization": "Bearer " + self.token, "Accept": "application/json",
                "Content-Type": "multipart/form-data; boundary=" + boundary,
            })
            response = connection.getresponse()
            raw, status = response.read(), response.status
        finally:
            connection.close()
        if not 200 <= status < 300:
            raise ConfigurationError(f"NPM ha rifiutato il certificato locale (HTTP {status}).")
        try:
            return json.loads(raw)
        except (ValueError, UnicodeError):
            raise ConfigurationError("Risposta di caricamento certificato non valida.") from None


def host_payload(domain, certificate_id):
    scheme, host, port, advanced = UPSTREAMS[domain]
    return {"domain_names": [domain], "forward_scheme": scheme, "forward_host": host, "forward_port": port,
            "certificate_id": certificate_id, "ssl_forced": True, "block_exploits": True,
            "caching_enabled": False, "allow_websocket_upgrade": False, "access_list_id": 0,
            "advanced_config": advanced, "enabled": True, "locations": [], "meta": {},
            "http2_support": True, "hsts_enabled": False, "hsts_subdomains": False}


def compatible_host(host, domain, certificate_id=None):
    expected = host_payload(domain, certificate_id)
    fields = ("forward_scheme", "forward_host", "forward_port", "ssl_forced", "block_exploits", "caching_enabled",
              "allow_websocket_upgrade", "access_list_id", "enabled", "locations")
    return (host.get("domain_names") == [domain]
            and all(host.get(field) == expected[field] for field in fields)
            and host.get("advanced_config", "").strip() == expected["advanced_config"].strip()
            and (certificate_id is None or host.get("certificate_id") == certificate_id))


def configure_hosts(client, data):
    hosts = client.request("GET", "/api/nginx/proxy-hosts")
    certificates = client.request("GET", "/api/nginx/certificates")
    if not isinstance(hosts, list) or not isinstance(certificates, list):
        raise ConfigurationError("Inventario NPM non valido.")
    matches = {}
    for domain in DOMAINS:
        matches[domain] = [host for host in hosts if domain in host.get("domain_names", [])]
        if len(matches[domain]) > 1 or any(not compatible_host(host, domain) for host in matches[domain]):
            raise ConfigurationError(f"Proxy Host incompatibile già presente per {domain}; inoltri conservati.")
    cert_matches = [cert for cert in certificates if cert.get("nice_name") == CERTIFICATE_NAME]
    if len(cert_matches) > 1 or any(cert.get("provider") != "other" for cert in cert_matches):
        raise ConfigurationError("Certificato omonimo incompatibile già presente; materiale conservato.")
    created_certificate = not cert_matches
    if created_certificate and any(matches.values()):
        raise ConfigurationError("Proxy Host esistente non associato al certificato locale previsto; configurazione conservata.")
    certificate = cert_matches[0] if cert_matches else client.request("POST", "/api/nginx/certificates", {
        "provider": "other", "nice_name": CERTIFICATE_NAME, "domain_names": list(DOMAINS), "meta": {},
    })
    if not isinstance(certificate, dict) or not isinstance(certificate.get("id"), int):
        raise ConfigurationError("NPM non ha restituito un ID certificato valido.")
    certificate_id = certificate["id"]
    if any(not compatible_host(host, domain, certificate_id) for domain in DOMAINS for host in matches[domain]):
        raise ConfigurationError("Proxy Host esistente usa un altro certificato; nessun inoltro modificato.")
    if created_certificate or not certificate.get("expires_on") or str(certificate["expires_on"]).startswith("0000-"):
        client.upload_certificate(certificate_id, data / "tls/local-web.crt", data / "tls/local-web.key", data / "tls/ca.crt")
    # NPM 2.16 overwrites domain_names with its parsed CN during upload. Its
    # display metadata cannot prove SAN coverage; compare the installed public
    # leaf to our verified certificate instead, without reading the private key.
    client.verify_public_certificate(certificate_id, data / "tls/local-web.crt")
    configured = []
    for domain in DOMAINS:
        host = matches[domain][0] if matches[domain] else client.request("POST", "/api/nginx/proxy-hosts", host_payload(domain, certificate_id))
        if not isinstance(host, dict) or not isinstance(host.get("id"), int):
            raise ConfigurationError("NPM non ha restituito un ID Proxy Host valido.")
        active = client.request("GET", f"/api/nginx/proxy-hosts/{host['id']}")
        if active.get("meta", {}).get("nginx_online") is not True:
            raise ConfigurationError(f"Proxy Host {domain} creato ma non attivo.")
        configured.append({"domain": domain, "id": host["id"], "created": not matches[domain]})
    return {"certificate_id": certificate_id, "hosts": configured}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=configuration.DEFAULT_DATA)
    parser.add_argument("--database", type=Path, default=configuration.PROJECT / "nginx-proxy-manager/data/database.sqlite")
    parser.add_argument("--container", default="projectmanament-proxy-web-1")
    parser.add_argument("--admin-id", type=int, help="ID esplicito dell'amministratore NPM esistente")
    parser.add_argument("--port", type=int, default=81)
    parser.add_argument("--context")
    parser.add_argument("--prepare-only", action="store_true", help="Genera e verifica soltanto il certificato web locale")
    args = parser.parse_args(argv)
    try:
        if not 1 <= args.port <= 65535 or (not args.prepare_only and args.admin_id is None):
            raise ConfigurationError("Specifica una porta valida e --admin-id per applicare la configurazione NPM.")
        with configuration.locked(args.data_dir):
            cert, key = prepare_web_tls(args.data_dir)
            if args.prepare_only:
                print(json.dumps({"certificate": str(cert), "private_key": str(key), "domains": DOMAINS}))
                return 0
            backup_database(args.database, args.data_dir / "npm-before-sso.sqlite")
            client = LocalNpmClient(args.port, container=args.container, context=args.context)
            client.wait_ready()
            client.token = administrator_token(args.container, args.admin_id, context=args.context)
            print(json.dumps(configure_hosts(client, args.data_dir)))
            return 0
    except (ConfigurationError, configuration.ConfigurationError) as exc:
        print(f"Errore: {exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError, UnicodeError, http.client.HTTPException, sqlite3.Error):
        print("Configurazione locale non completata; controlla servizi e file senza esporre credenziali.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
