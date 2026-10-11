#!/usr/bin/env python3
"""Prepare optional Keycloak configuration without starting or changing services."""

import argparse
import contextlib
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit


PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_DATA = PROJECT / "data" / "keycloak"
REALM_TEMPLATE = PROJECT / "auth" / "keycloak" / "realm.template.json"
APPS = {"punto": "punto", "tickets": "punto-tickets"}


class ConfigurationError(Exception):
    pass


def validate_url(value, *, issuer=False):
    """Accept exact HTTPS origins or the one configured realm URL, never patterns."""
    if (not isinstance(value, str) or not value or any(ord(ch) <= 32 or ord(ch) >= 127 for ch in value)
            or any(ch in value for ch in "\\?#%")):
        raise ConfigurationError("URL HTTPS non valido.")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError:
        raise ConfigurationError("URL HTTPS non valido.") from None
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or parsed.path != ("/realms/punto" if issuer else "")
            or (port is not None and not 1 <= port <= 65535)):
        raise ConfigurationError("Usa HTTPS senza credenziali, query o slash finale; issuer termina con /realms/punto.")
    host = parsed.hostname.lower()
    try:
        ipaddress.ip_address(host)
    except ValueError:
        labels = host.split(".")
        if (len(host) > 253 or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label)
                                   for label in labels)):
            raise ConfigurationError("Hostname HTTPS non valido.") from None
    authority = f"[{host}]" if ":" in host else host
    if port is not None and port != 443:
        authority += f":{port}"
    # Reject syntax urlsplit tolerates but which can alter URL/Compose interpretation.
    if parsed.netloc.lower() not in {authority, authority + ":443"}:
        raise ConfigurationError("Autorità HTTPS non valida.")
    return "https://" + authority + parsed.path


def regular_file(path):
    if path.is_symlink() or not path.is_file() or not stat.S_ISREG(path.stat().st_mode):
        raise ConfigurationError(f"File non regolare: {path}")
    return path


def private_directory(path):
    if path.is_symlink():
        raise ConfigurationError(f"Directory simbolica non consentita: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        raise ConfigurationError(f"Directory non valida: {path}")
    path.chmod(0o700)


def write_new(path, content):
    if path.exists() or path.is_symlink():
        regular_file(path)
        return False
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        stream.write(content)
    return True


def write_private(path, content):
    regular_file(path)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=".update-", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            temporary.chmod(0o600)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def json_text(value):
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def read_json(path):
    regular_file(path)
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ConfigurationError(f"Chiave JSON duplicata: {path}")
            result[key] = value
        return result
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except (ValueError, UnicodeError):
        raise ConfigurationError(f"JSON non valido: {path}") from None


@contextlib.contextmanager
def locked(data):
    private_directory(data)
    path = data / ".configure.lock"
    write_new(path, "")
    regular_file(path).chmod(0o600)
    with path.open("r+") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def openssl(*arguments):
    binary = shutil.which("openssl")
    if not binary:
        raise ConfigurationError("OpenSSL necessario per generare e verificare TLS.")
    result = subprocess.run([binary, *map(str, arguments)], capture_output=True, timeout=60)
    if result.returncode:
        raise ConfigurationError("Generazione o verifica TLS fallita; materiale esistente conservato.")
    return result.stdout


def verify_tls(directory):
    for name in ("ca", "keycloak"):
        cert = regular_file(directory / f"{name}.crt")
        key = regular_file(directory / f"{name}.key")
        if openssl("x509", "-in", cert, "-pubkey", "-noout") != openssl("pkey", "-in", key, "-pubout"):
            raise ConfigurationError(f"Certificato e chiave {name} non corrispondono.")
        openssl("x509", "-in", cert, "-checkend", "86400", "-noout")
        key.chmod(0o600)
    openssl("verify", "-CAfile", directory / "ca.crt", "-verify_hostname", "keycloak", directory / "keycloak.crt")
    for address in ("172.30.20.12", "172.30.90.2"):
        openssl("verify", "-CAfile", directory / "ca.crt", "-verify_ip", address, directory / "keycloak.crt")


def prepare_tls(data):
    directory = data / "tls"
    if directory.exists() or directory.is_symlink():
        if directory.is_symlink() or not directory.is_dir():
            raise ConfigurationError("Directory TLS non valida.")
        directory.chmod(0o700)
        verify_tls(directory)
        return
    with tempfile.TemporaryDirectory(prefix=".tls-", dir=data) as temporary:
        staging = Path(temporary)
        openssl("req", "-x509", "-newkey", "rsa:3072", "-nodes", "-sha256", "-days", "3650",
                "-keyout", staging / "ca.key", "-out", staging / "ca.crt",
                "-subj", "/O=Punto Authentication/CN=Punto Authentication CA",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        openssl("req", "-new", "-newkey", "rsa:3072", "-nodes", "-sha256",
                "-keyout", staging / "keycloak.key", "-out", staging / "keycloak.csr",
                "-subj", "/O=Punto Authentication/CN=keycloak")
        extension = staging / "keycloak.ext"
        extension.write_text("basicConstraints=critical,CA:FALSE\n"
                             "keyUsage=critical,digitalSignature,keyEncipherment\n"
                             "extendedKeyUsage=serverAuth\n"
                             "subjectAltName=DNS:keycloak,IP:172.30.20.12,IP:172.30.90.2\n", encoding="ascii")
        openssl("x509", "-req", "-in", staging / "keycloak.csr", "-CA", staging / "ca.crt",
                "-CAkey", staging / "ca.key", "-set_serial", "0x" + secrets.token_hex(16),
                "-days", "365", "-sha256", "-extfile", extension, "-out", staging / "keycloak.crt")
        verify_tls(staging)
        private_directory(directory)
        for name in ("ca.crt", "ca.key", "keycloak.crt", "keycloak.key"):
            source = staging / name
            source.chmod(0o600 if name.endswith(".key") else 0o644)
            source.replace(directory / name)


def prepare_secret(path):
    write_new(path, secrets.token_hex(32) + "\n")
    value = regular_file(path).read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ConfigurationError(f"Segreto esistente non valido; ripristina il file originale: {path}")
    path.chmod(0o600)
    return value


def make_realm(configuration, client_secrets):
    realm = read_json(REALM_TEMPLATE)
    realm["roles"] = {"client": {}}
    realm["clientScopeMappings"] = {}
    for app, client_id in APPS.items():
        origin = configuration[f"{app}_origin"]
        roles = ("manager", "worker") if app == "punto" else ("admin", "agent")
        realm["roles"]["client"][client_id] = [{"name": role, "composite": False} for role in roles]
        realm["clientScopeMappings"][client_id] = [{"client": client_id, "roles": list(roles)}]
        realm["clients"].append({
            "clientId": client_id,
            "name": "Punto" if app == "punto" else "Punto Tickets",
            "enabled": origin is not None,
            "protocol": "openid-connect",
            "publicClient": False,
            "clientAuthenticatorType": "client-secret",
            "secret": client_secrets[app],
            "standardFlowEnabled": True,
            "implicitFlowEnabled": False,
            "directAccessGrantsEnabled": False,
            "serviceAccountsEnabled": False,
            "fullScopeAllowed": False,
            "redirectUris": [origin + "/auth/callback"] if origin else [],
            "webOrigins": [origin] if origin else [],
            "defaultClientScopes": ["profile", "email"],
            "optionalClientScopes": [],
            "protocolMappers": [managed_role_mapper(client_id)],
            "attributes": {
                "pkce.code.challenge.method": "S256",
                "id.token.signed.response.alg": "RS256",
                "access.token.signed.response.alg": "RS256",
                "post.logout.redirect.uris": origin + "/" if origin else "",
            },
        })
    return realm


def managed_role_mapper(client_id):
    return {
        "name": "punto-managed-app-roles", "protocol": "openid-connect",
        "protocolMapper": "oidc-usermodel-client-role-mapper",
        "config": {"usermodel.clientRoleMapping.clientId": client_id,
                   "claim.name": "app_roles", "jsonType.label": "String",
                   "multivalued": "true", "id.token.claim": "true",
                   "access.token.claim": "false", "userinfo.token.claim": "false",
                   "introspection.token.claim": "false", "lightweight.claim": "false"},
    }


def env_content(configuration):
    values = {
        "KEYCLOAK_PUBLIC_ORIGIN": configuration["issuer"].removesuffix("/realms/punto"),
        "KEYCLOAK_BOOTSTRAP_ADMIN": configuration["bootstrap_admin"],
        "KEYCLOAK_ISSUER_URL": configuration["issuer"],
        "KEYCLOAK_PUNTO_REDIRECT_URI": configuration["punto_origin"] + "/auth/callback",
        "KEYCLOAK_TICKETS_REDIRECT_URI": (configuration["tickets_origin"] + "/auth/callback"
                                          if configuration["tickets_origin"] else ""),
        "KEYCLOAK_AUTH_MODE": "hybrid",
        "KEYCLOAK_TICKETS_AUTH_MODE": "hybrid",
        "KEYCLOAK_USER_MANAGEMENT": "local",
        "KEYCLOAK_LOCAL_ADMIN_EMAIL": "",
        "KEYCLOAK_USERS_ADMIN_URL": configuration["issuer"].removesuffix("/realms/punto") + "/admin/punto/console/",
    }
    return ("# Generato da configure_keycloak.py; nessun segreto.\n"
            "# init conserva questo file, comprese modifiche AUTH_MODE dell'operatore.\n" +
            "".join(f"{key}={value}\n" for key, value in values.items()))


def validate_binding(subject, email):
    if not isinstance(subject, str) or not re.fullmatch(r"[A-Za-z0-9._~:@+-]{1,255}", subject):
        raise ConfigurationError("Subject Keycloak non valido.")
    if (not isinstance(email, str) or not email.isascii() or len(email) > 254
            or not re.fullmatch(r"[^\s@\x00-\x1f\x7f-\uffff]+@[^\s@\x00-\x1f\x7f-\uffff]+", email)):
        raise ConfigurationError("Email locale non valida.")
    email = email.lower()
    local, domain = email.split("@")
    if (len(local) > 64 or local.startswith(".") or local.endswith(".") or ".." in local
            or not re.fullmatch(r"[a-z0-9.!#$%&'*+/=?^_`{|}~-]+", local)
            or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label)
                   for label in domain.split("."))):
        raise ConfigurationError("Email locale non valida.")
    return subject, email


def load_bindings(path):
    regular_file(path)
    if path.stat().st_size > 256 * 1024:
        raise ConfigurationError(f"Mapping superiore a 256 KiB: {path}")
    values = read_json(path)
    if not isinstance(values, dict) or len(values) > 1024:
        raise ConfigurationError(f"Il mapping deve essere un oggetto subject:email: {path}")
    for subject, email in values.items():
        validate_binding(subject, email)
    path.chmod(0o600)
    return values


def bind_account(data, app, subject, email, *, remove=False):
    if app not in APPS:
        raise ConfigurationError("App non valida: scegli punto o tickets.")
    path = data / "bindings" / f"{app}.json"
    values = load_bindings(path)
    if remove:
        if subject in values:
            del values[subject]
            write_private(path, json_text(values))
    else:
        subject, email = validate_binding(subject, email)
        if subject in values and values[subject] != email:
            raise ConfigurationError("Subject già associato; rimuovi esplicitamente il binding prima di riassegnarlo.")
        if values.get(subject) != email:
            values[subject] = email
            if len(values) > 1024 or len(json_text(values).encode("utf-8")) > 256 * 1024:
                raise ConfigurationError("Limite mapping superato: massimo 1024 associazioni e 256 KiB.")
            write_private(path, json_text(values))
    return path


def initialize(data, issuer, punto_origin, tickets_origin=None, bootstrap_admin="punto-bootstrap", bindings=()):
    issuer = validate_url(issuer, issuer=True)
    punto_origin = validate_url(punto_origin)
    tickets_origin = validate_url(tickets_origin) if tickets_origin else None
    origins = [issuer.removesuffix("/realms/punto"), punto_origin]
    if tickets_origin:
        origins.append(tickets_origin)
    if len(origins) != len(set(origins)):
        raise ConfigurationError("Keycloak e ciascuna app richiedono origini HTTPS distinte.")
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", bootstrap_admin):
        raise ConfigurationError("Nome bootstrap non valido.")
    for app, subject, email in bindings:
        if app not in APPS:
            raise ConfigurationError("App non valida: scegli punto o tickets.")
        validate_binding(subject, email)
    configuration = {"issuer": issuer, "punto_origin": punto_origin,
                     "tickets_origin": tickets_origin, "bootstrap_admin": bootstrap_admin}
    with locked(data):
        config_path = data / "config.json"
        if config_path.exists() or config_path.is_symlink():
            if read_json(config_path) != configuration:
                raise ConfigurationError("Configurazione già inizializzata con URL diversi; init non modifica un realm esistente.")
        for directory in ("secrets", "realm", "bindings"):
            private_directory(data / directory)
        password_values = {name: prepare_secret(data / "secrets" / name)
                           for name in ("database-password", "bootstrap-password", "punto-client-secret", "tickets-client-secret")}
        prepare_tls(data)
        expected_realm = make_realm(configuration, {app: password_values[f"{app}-client-secret"] for app in APPS})
        realm_path = data / "realm" / "punto-realm.json"
        write_new(realm_path, json_text(expected_realm))
        if read_json(realm_path) != expected_realm:
            raise ConfigurationError("Realm generato non coerente con segreti e URL; ripristina i file originali.")
        realm_path.chmod(0o600)
        for app in APPS:
            path = data / "bindings" / f"{app}.json"
            write_new(path, "{}\n")
            load_bindings(path)
        for app, subject, email in bindings:
            bind_account(data, app, subject, email)
        write_new(data / "compose.env", env_content(configuration))
        regular_file(data / "compose.env").chmod(0o600)
        write_new(config_path, json_text(configuration))
        config_path.chmod(0o600)
    return data / "compose.env"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA,
                        help="Directory materiale; Compose usa data/keycloak nella root del progetto")
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init", help="Prepara file e certificati senza avviare container")
    init.add_argument("--issuer", required=True)
    init.add_argument("--punto-origin", required=True)
    init.add_argument("--tickets-origin")
    init.add_argument("--bootstrap-admin", default="punto-bootstrap")
    init.add_argument("--bind", nargs=3, action="append", default=[], metavar=("APP", "SUBJECT", "EMAIL"))
    bind = commands.add_parser("bind", help="Associa esplicitamente un subject a un account locale esistente")
    unbind = commands.add_parser("unbind", help="Rimuovi un'associazione esplicita")
    for command in (bind, unbind):
        command.add_argument("--app", required=True, choices=APPS)
        command.add_argument("--subject", required=True)
    bind.add_argument("--email", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "init":
            path = initialize(args.data_dir, args.issuer, args.punto_origin, args.tickets_origin,
                              args.bootstrap_admin, args.bind)
            print(f"Configurazione pronta: {path}")
            print(f"Password bootstrap: {args.data_dir / 'secrets' / 'bootstrap-password'}")
            print("Nessun servizio avviato. Nessuna password locale migrata.")
        else:
            with locked(args.data_dir):
                path = bind_account(args.data_dir, args.app, args.subject, getattr(args, "email", None),
                                    remove=args.command == "unbind")
            print(f"Binding aggiornati: {path}")
            print("Riesegui auth-config-init e ricrea l'app interessata per caricare il mapping.")
        return 0
    except (ConfigurationError, OSError, subprocess.TimeoutExpired, UnicodeError) as exc:
        print(f"Errore: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
