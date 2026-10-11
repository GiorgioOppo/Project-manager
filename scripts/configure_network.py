#!/usr/bin/env python3
"""Prepare private TLS material and apply the segmented Docker network."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile

from manage_cluster import Cluster, ClusterError, NODES, NODE_SERVICES, PROJECT, STATE_PATH, load_state


TLS_PATH = PROJECT / "data" / "network-tls"
ADDRESSES = {"db": "172.30.30.21", "db2": "172.30.30.22", "db3": "172.30.30.23"}
ADDON_SECRETS = PROJECT / "data" / "addon-secrets"


def openssl(arguments):
    binary = shutil.which("openssl")
    if not binary:
        raise ClusterError("OpenSSL necessario per preparare i certificati privati.")
    result = subprocess.run([binary, *map(str, arguments)], capture_output=True, timeout=60)
    if result.returncode:
        raise ClusterError("Generazione o verifica TLS non riuscita; nessuna chiave esistente sovrascritta.")
    return result.stdout


def verify_bundle(directory):
    for name in ("ca", *NODES):
        cert, key = directory / (name + ".crt"), directory / (name + ".key")
        if not cert.is_file() or not key.is_file():
            raise ClusterError("Bundle TLS incompleto: ripristina i certificati e le chiavi originali prima dell'avvio.")
        openssl(("x509", "-in", cert, "-noout", "-checkend", "86400"))
        certificate_public = openssl(("x509", "-in", cert, "-pubkey", "-noout"))
        key_public = openssl(("pkey", "-in", key, "-pubout"))
        if certificate_public != key_public:
            raise ClusterError("Certificato e chiave TLS non corrispondono: " + name)
    for node in NODES:
        cert = directory / (node + ".crt")
        for hostname in (node, "db-proxy"):
            openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_hostname", hostname, cert))
        openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_ip", ADDRESSES[node], cert))


def prepare_document_certificate(directory):
    """Extend an existing CA bundle without rotating the SQL certificates."""
    paths = [directory / ("documents" + suffix) for suffix in (".crt", ".key", ".pem")]
    if any(path.exists() for path in paths):
        if not all(path.is_file() for path in paths):
            raise ClusterError("Certificati MongoDB incompleti: ripristina il bundle originale.")
    else:
        with tempfile.TemporaryDirectory(prefix="documents-tls-", dir=directory) as temporary:
            staging = Path(temporary)
            key, csr, cert = (staging / ("documents" + suffix) for suffix in (".key", ".csr", ".crt"))
            openssl(("req", "-new", "-newkey", "rsa:3072", "-nodes", "-sha256",
                     "-keyout", key, "-out", csr, "-subj", "/O=Punto Local/CN=documents"))
            extension = staging / "documents.ext"
            extension.write_text("basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth,clientAuth\n"
                "subjectAltName=DNS:documents,DNS:localhost,IP:127.0.0.1,IP:172.30.30.30\n", encoding="ascii")
            openssl(("x509", "-req", "-in", csr, "-CA", directory / "ca.crt",
                     "-CAkey", directory / "ca.key", "-set_serial", "0x" + secrets.token_hex(16),
                     "-days", "365", "-sha256", "-extfile", extension, "-out", cert))
            pem = staging / "documents.pem"
            pem.write_bytes(key.read_bytes() + cert.read_bytes())
            key.chmod(0o444)
            pem.chmod(0o444)
            cert.chmod(0o644)
            for path in (key, cert, pem):
                path.replace(directory / path.name)
    cert, key, pem = paths
    openssl(("x509", "-in", cert, "-noout", "-checkend", "86400"))
    if openssl(("x509", "-in", cert, "-pubkey", "-noout")) != openssl(("pkey", "-in", key, "-pubout")):
        raise ClusterError("Certificato e chiave MongoDB non corrispondono.")
    if pem.read_bytes() != key.read_bytes() + cert.read_bytes():
        raise ClusterError("Bundle PEM MongoDB non corrispondente al certificato.")
    for hostname in ("documents", "localhost"):
        openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_hostname", hostname, cert))
    for address in ("127.0.0.1", "172.30.30.30"):
        openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_ip", address, cert))


def prepare_addon_certificate(directory, service, api_name, address):
    """Issue a missing addon leaf without changing existing keys or the CA."""
    cert, key = (directory / (service + suffix) for suffix in (".crt", ".key"))
    if cert.exists() or key.exists():
        if not cert.is_file() or not key.is_file():
            raise ClusterError("Certificato addon incompleto: ripristina il bundle originale.")
    else:
        with tempfile.TemporaryDirectory(prefix=service + "-tls-", dir=directory) as temporary:
            staging = Path(temporary)
            issued_key, csr, issued_cert = (staging / (service + suffix)
                                             for suffix in (".key", ".csr", ".crt"))
            openssl(("req", "-new", "-newkey", "rsa:3072", "-nodes", "-sha256",
                     "-keyout", issued_key, "-out", csr, "-subj", "/O=Punto Local/CN=" + api_name))
            extension = staging / (service + ".ext")
            extension.write_text("basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\n"
                f"subjectAltName=DNS:{api_name},DNS:{service},DNS:localhost,IP:127.0.0.1,IP:{address}\n",
                encoding="ascii")
            openssl(("x509", "-req", "-in", csr, "-CA", directory / "ca.crt",
                     "-CAkey", directory / "ca.key", "-set_serial", "0x" + secrets.token_hex(16),
                     "-days", "365", "-sha256", "-extfile", extension, "-out", issued_cert))
            issued_key.chmod(0o444)
            issued_cert.chmod(0o644)
            issued_key.replace(key)
            issued_cert.replace(cert)
    openssl(("x509", "-in", cert, "-noout", "-checkend", "86400"))
    if openssl(("x509", "-in", cert, "-pubkey", "-noout")) != openssl(("pkey", "-in", key, "-pubout")):
        raise ClusterError("Certificato e chiave addon non corrispondono.")
    for hostname in (api_name, service, "localhost"):
        openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_hostname", hostname, cert))
    for ip_address in ("127.0.0.1", address):
        openssl(("verify", "-CAfile", directory / "ca.crt", "-verify_ip", ip_address, cert))


def prepare_mail_certificate(directory):
    prepare_addon_certificate(directory, "mail-addon", "mail-api", "172.30.60.2")


def prepare_calendar_certificate(directory):
    prepare_addon_certificate(directory, "calendar-addon", "calendar-api", "172.30.70.2")


def prepare_inventory_certificate(directory):
    prepare_addon_certificate(directory, "inventory-addon", "inventory-api", "172.30.80.2")


def prepare_addon_secrets():
    """Create local file-backed secrets once; never rotate an existing token."""
    if ADDON_SECRETS.is_symlink():
        raise ClusterError("La directory dei segreti addon non può essere un collegamento simbolico.")
    ADDON_SECRETS.mkdir(mode=0o700, parents=True, exist_ok=True)
    ADDON_SECRETS.chmod(0o700)
    hex_secrets = {"mail-api-token", "calendar-api-token", "calendar-token-key", "inventory-api-token"}
    for name, initial in (("mail-api-token", secrets.token_hex(32) + "\n"), ("smtp-password", ""),
                          ("calendar-api-token", secrets.token_hex(32) + "\n"),
                          ("calendar-token-key", secrets.token_hex(32) + "\n"),
                          ("google-client-secret", ""),
                          ("inventory-api-token", secrets.token_hex(32) + "\n")):
        path = ADDON_SECRETS / name
        if not path.exists() and not path.is_symlink():
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w", encoding="ascii") as handle:
                handle.write(initial)
        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise ClusterError("File segreto addon non regolare: " + name)
        if name in hex_secrets:
            value = path.read_text(encoding="ascii").strip()
            if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
                raise ClusterError("Token addon esistente non valido; ripristina il file originale.")
        # Compose file-backed secrets keep host ownership. Protected parent 0700
        # limits host access; readonly files allow the two distinct service UIDs.
        path.chmod(0o444)


def prepare():
    if TLS_PATH.exists():
        verify_bundle(TLS_PATH)
        prepare_document_certificate(TLS_PATH)
        prepare_mail_certificate(TLS_PATH)
        prepare_calendar_certificate(TLS_PATH)
        prepare_inventory_certificate(TLS_PATH)
        prepare_addon_secrets()
        print("CA verificata; certificati e segreti addon pronti, materiali esistenti conservati.", flush=True)
        return
    TLS_PATH.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="network-tls-", dir=TLS_PATH.parent) as temporary:
        directory = Path(temporary)
        directory.chmod(0o700)
        openssl(("req", "-x509", "-newkey", "rsa:3072", "-nodes", "-sha256", "-days", "3650",
                 "-keyout", directory / "ca.key", "-out", directory / "ca.crt",
                 "-subj", "/O=Punto Local/CN=Punto Private Network CA",
                 "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
                 "-addext", "keyUsage=critical,keyCertSign,cRLSign"))
        for node in NODES:
            key, csr, cert = (directory / (node + suffix) for suffix in (".key", ".csr", ".crt"))
            openssl(("req", "-new", "-newkey", "rsa:3072", "-nodes", "-sha256",
                     "-keyout", key, "-out", csr, "-subj", "/O=Punto Local/CN=" + node))
            extension = directory / (node + ".ext")
            extension.write_text("basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth,clientAuth\n"
                f"subjectAltName=DNS:{node},DNS:db-proxy,IP:{ADDRESSES[node]}\n", encoding="ascii")
            openssl(("x509", "-req", "-in", csr, "-CA", directory / "ca.crt",
                     "-CAkey", directory / "ca.key", "-set_serial", "0x" + secrets.token_hex(16),
                     "-days", "365", "-sha256", "-extfile", extension, "-out", cert))
            csr.unlink()
            extension.unlink()
            # Only this key is mounted in its node. The host directory is 0700;
            # readable bind files let MySQL's UID 999 use its key without root.
            key.chmod(0o444)
            cert.chmod(0o644)
        (directory / "ca.key").chmod(0o600)
        (directory / "ca.crt").chmod(0o644)
        verify_bundle(directory)
        prepare_document_certificate(directory)
        prepare_mail_certificate(directory)
        prepare_calendar_certificate(directory)
        prepare_inventory_certificate(directory)
        directory.rename(TLS_PATH)
    prepare_addon_secrets()
    print("CA privata e certificati interni creati in data/network-tls (esclusi da Git e dalle immagini).", flush=True)
    print("Conserva una copia protetta del bundle per ripristino e rinnovo; la chiave CA non viene montata nei servizi.", flush=True)


def check_engine(cluster):
    command = cluster.command[:cluster.command.index("compose")]
    result = subprocess.run(command + ["version", "--format", "{{.Server.Version}}"],
                            capture_output=True, env=cluster.environment, timeout=30)
    try:
        major = int(result.stdout.decode().strip().split(".")[0])
    except (ValueError, UnicodeError):
        major = 0
    if result.returncode or major < 28:
        raise ClusterError("Docker Engine 28 o successivo necessario per il gateway isolato.")
    # Parse in memory only: the resolved config also contains database secrets.
    # Its project name respects --project-name, COMPOSE_PROJECT_NAME and Compose's
    # normal directory-name normalization, so a repeated apply stays idempotent.
    project_name = json.loads(cluster.run(("config", "--format", "json")).stdout)["name"]
    # Refuse subnet overlap with unrelated deployments before stopping services.
    networks = subprocess.run(command + ["network", "ls", "--format", "{{.ID}}"],
                              capture_output=True, env=cluster.environment, timeout=30, check=True)
    ids = networks.stdout.decode().split()
    if ids:
        inspection = subprocess.run(command + ["network", "inspect", *ids], capture_output=True,
                                    env=cluster.environment, timeout=30, check=True)
        import ipaddress
        targets = {"public": "172.30.10.0/24", "private_app": "172.30.20.0/24",
                   "backend_app": "172.30.30.0/24", "app_egress": "172.30.40.0/24",
                   "proxy_wan": "172.30.50.0/24", "mail_api": "172.30.60.0/24",
                   "calendar_api": "172.30.70.0/24", "inventory_api": "172.30.80.0/24"}
        for network in json.loads(inspection.stdout):
            labels = network.get("Labels") or {}
            name = labels.get("com.docker.compose.network")
            same_project = labels.get("com.docker.compose.project") == project_name
            for entry in network.get("IPAM", {}).get("Config") or []:
                subnet = entry.get("Subnet")
                if not subnet:
                    continue
                existing = ipaddress.ip_network(subnet)
                for target, subnet in targets.items():
                    if existing.overlaps(ipaddress.ip_network(subnet)) and not (same_project and name == target):
                        raise ClusterError("Subnet di rete già utilizzata; aggiorna indirizzi, bind, allowlist e SAN prima di applicare.")


def apply(env_file, context):
    if not load_state().get("initialized"):
        raise ClusterError("Cluster non inizializzato: esegui prepare e poi manage_cluster.py init.")
    cluster = Cluster(env_file, context)
    cluster.require_service_migration_complete()
    prepare()
    check_engine(cluster)
    online = [node for node in NODES if cluster.local_state(node) == "ONLINE"]
    if not online:
        raise ClusterError("Prima della migrazione ripristina il cluster e verifica il suo stato ONLINE.")
    print("Migrazione delle reti: arresto app, proxy e Nginx, preservando volumi e configurazione.", flush=True)
    cluster.run(("stop", "proxy-web", "app", "proxy-db"))
    cluster.backup(online[0])
    print("Ricreazione dei tre membri sulla rete backend isolata…", flush=True)
    cluster.run(("up", "-d", "--no-deps", "--force-recreate", *(NODE_SERVICES[node] for node in NODES)))
    cluster.recover()
    print("Reti applicate. Esegui la suite network per verificare i flussi consentiti e vietati.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=PROJECT / ".env.docker")
    parser.add_argument("--docker-context", default=os.environ.get("DOCKER_CONTEXT", "desktop-linux" if sys.platform == "darwin" else ""))
    parser.add_argument("action", choices=("prepare", "apply"))
    args = parser.parse_args()
    lock = None
    try:
        STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
        lock = (STATE_PATH.parent / "cluster-controller.lock").open("a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ClusterError("Un'altra modifica del cluster è già in corso.") from None
        if args.action == "prepare":
            prepare()
        else:
            apply(args.env_file, args.docker_context)
        return 0
    except Exception as error:
        message = str(error) if isinstance(error, ClusterError) else "Operazione non completata; verifica lo stato dei servizi prima di procedere."
        print("Errore: " + message, file=sys.stderr)
        return 1
    finally:
        if lock is not None:
            lock.close()


if __name__ == "__main__":
    sys.exit(main())
