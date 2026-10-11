#!/usr/bin/env python3
"""Provision explicitly selected Keycloak identities without exposing credentials."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys

import configure_keycloak as configuration


class AdministrationError(Exception):
    pass


def read_password(path):
    configuration.regular_file(path)
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise AdministrationError("Il file password deve avere permessi 0600 o più restrittivi.")
    value = path.read_text(encoding="utf-8")
    if value.endswith("\n"):
        value = value[:-1]
    if not 12 <= len(value) <= 256 or any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise AdministrationError("La password deve contenere da 12 a 256 caratteri senza caratteri di controllo.")
    return value


class KeycloakAdmin:
    def __init__(self, container, ca_file, *, context=None):
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", container):
            raise AdministrationError("Nome container non valido.")
        binary = shutil.which("docker")
        if not binary:
            raise AdministrationError("Docker non disponibile.")
        self.docker = [binary] + (["--context", context] if context else [])
        self.container = container
        self.ca_file = configuration.regular_file(ca_file.resolve())
        self.work = None

    def run(self, arguments, *, data=None, timeout=45):
        try:
            result = subprocess.run(self.docker + list(arguments), input=data, capture_output=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired):
            raise AdministrationError("Operazione Docker/Keycloak non completata.") from None
        if result.returncode:
            # Admin responses can echo credential payloads. Never print them.
            raise AdministrationError(f"Operazione Docker/Keycloak rifiutata (uscita {result.returncode}).")
        return result.stdout

    def __enter__(self):
        work = self.run(["exec", self.container, "/bin/bash", "-c", "umask 077; mktemp -d /tmp/punto-keycloak-admin.XXXXXXXX"])
        self.work = work.decode("ascii").strip()
        if not re.fullmatch(r"/tmp/punto-keycloak-admin\.[A-Za-z0-9]{8}", self.work):
            self.work = None
            raise AdministrationError("Directory amministrativa temporanea non valida.")
        try:
            self.run(["cp", str(self.ca_file), self.container + ":" + self.work + "/ca.crt"])
            script = '''set -euo pipefail
umask 077
work=$1
keytool -importcert -noprompt -alias punto-ca -file "$work/ca.crt" -keystore "$work/trust.jks" -storepass changeit >/dev/null 2>&1
export KC_CLI_PASSWORD="$(cat /run/punto-auth/bootstrap-password)"
/opt/keycloak/bin/kcadm.sh config credentials --config "$work/kcadm.config" --server https://keycloak:8443 --realm master --user "$KC_BOOTSTRAP_ADMIN_USERNAME" --truststore "$work/trust.jks" --trustpass changeit >/dev/null 2>&1
'''
            self.run(["exec", self.container, "/bin/bash", "-c", script, "punto-admin", self.work])
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self.work:
            try:
                self.run(["exec", self.container, "/bin/bash", "-c", 'rm -rf -- "$1"', "punto-admin", self.work])
            except AdministrationError:
                pass
            self.work = None

    def command(self, arguments, *, payload=None):
        if not self.work:
            raise AdministrationError("Sessione amministrativa non inizializzata.")
        return self.run(["exec", "-i", self.container, "/opt/keycloak/bin/kcadm.sh", *arguments,
                         "--config", self.work + "/kcadm.config",
                         "--truststore", self.work + "/trust.jks", "--trustpass", "changeit"],
                        data=json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None)

    def find_user(self, username):
        raw = self.command(["get", "users", "-r", "punto", "-q", "username=" + username, "-q", "exact=true"])
        try:
            users = json.loads(raw)
        except (ValueError, UnicodeError):
            raise AdministrationError("Risposta utenti Keycloak non valida.") from None
        if not isinstance(users, list) or len(users) > 1 or any(not isinstance(user, dict) for user in users):
            raise AdministrationError("Risposta utenti Keycloak ambigua.")
        return users[0] if users else None

    def create_user(self, payload):
        raw = self.command(["create", "users", "-r", "punto", "-f", "-", "-i"], payload=payload)
        subject = raw.decode("utf-8").strip().strip('"')
        if not re.fullmatch(r"[A-Za-z0-9._~:@+-]{1,255}", subject):
            raise AdministrationError("Keycloak non ha restituito un subject valido; controlla gli utenti prima di riprovare.")
        return subject


def validate_request(username, email, name, bindings):
    if (not isinstance(username, str) or not 1 <= len(username) <= 254
            or not username.isascii() or any(ord(character) < 33 or ord(character) > 126 for character in username)):
        raise AdministrationError("Username non valido.")
    _, email = configuration.validate_binding("subject", email)
    if not name or len(name) > 255 or any(ord(character) < 32 or ord(character) == 127 for character in name):
        raise AdministrationError("Nome non valido.")
    first_name, _, last_name = name.strip().partition(" ")
    if not first_name or not last_name.strip():
        raise AdministrationError("Specifica nome e cognome con --name 'Nome Cognome': entrambi sono obbligatori nel realm.")
    selected = {}
    for app, local_email in bindings:
        if app not in configuration.APPS or app in selected:
            raise AdministrationError("Specifica al massimo un binding per ciascuna app: punto o tickets.")
        _, selected[app] = configuration.validate_binding("subject", local_email)
    if not selected:
        raise AdministrationError("Specifica almeno un binding esplicito con --bind APP EMAIL_LOCALE.")
    return email, selected


def create_user(admin, data, username, email, name, password_file, bindings, *, subject=None, temporary_password=False):
    email, selected = validate_request(username, email, name, bindings)
    if subject is not None:
        configuration.validate_binding(subject, email)
    # Validate local sources before authenticating or creating a remote identity.
    configuration.read_json(data / "config.json")
    with configuration.locked(data):
        paths = {app: data / "bindings" / f"{app}.json" for app in selected}
        maps = {app: configuration.load_bindings(path) for app, path in paths.items()}
        manifest_path = data / "provisioned-users.json"
        manifest = configuration.read_json(manifest_path) if manifest_path.exists() or manifest_path.is_symlink() else {}
        if not isinstance(manifest, dict):
            raise AdministrationError("Manifest utenti non valido.")
        previous = manifest.get(username)
        if previous is not None and (not isinstance(previous, dict) or not isinstance(previous.get("subject"), str)
                                     or previous.get("email") != email):
            raise AdministrationError("Manifest utente non coerente; specifica e verifica l'identità amministrativamente.")
        password = read_password(password_file)
        with admin as session:
            existing = session.find_user(username)
            created = existing is None
            if existing is not None:
                actual_subject = existing.get("id")
                expected_subject = subject or (previous["subject"] if previous else None)
                if (not isinstance(actual_subject, str) or actual_subject != expected_subject
                        or existing.get("username", "").lower() != username.lower()
                        or existing.get("email", "").lower() != email or not existing.get("enabled")):
                    raise AdministrationError("Username già presente: nessun utente modificato. Occorre --subject verificato o un manifest coerente.")
                subject = actual_subject
            else:
                if subject or previous:
                    raise AdministrationError("L'identità attesa non è presente; nessun nuovo subject creato.")
                first_name, _, last_name = name.strip().partition(" ")
                last_name = last_name.strip()
                payload = {"username": username, "email": email, "firstName": first_name, "lastName": last_name,
                           "enabled": True, "emailVerified": False,
                           "credentials": [{"type": "password", "value": password, "temporary": temporary_password}]}
                subject = session.create_user(payload)
            # Record the returned subject before local binding writes: retrying a
            # partially completed operation cannot select a different identity.
            if previous is None:
                manifest[username] = {"subject": subject, "email": email}
                if manifest_path.exists():
                    configuration.write_private(manifest_path, configuration.json_text(manifest))
                else:
                    configuration.write_new(manifest_path, configuration.json_text(manifest))
            for app, local_email in selected.items():
                if subject in maps[app] and maps[app][subject] != local_email:
                    raise AdministrationError("Subject già associato a un altro account locale; binding conservati.")
            changed = [app for app, local_email in selected.items() if maps[app].get(subject) != local_email]
            backup = None
            if changed:
                configuration.private_directory(data / "binding-backups")
                backup = data / "binding-backups" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                configuration.private_directory(backup)
                for app in changed:
                    configuration.write_new(backup / f"{app}.json", paths[app].read_text(encoding="utf-8"))
                for app in changed:
                    configuration.bind_account(data, app, subject, selected[app])
            return {"subject": subject, "created": created, "bindings": changed,
                    "manifest": str(manifest_path), "binding_backup": str(backup) if backup else None}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True, help="Container Keycloak già in esecuzione")
    parser.add_argument("--context", help="Contesto Docker facoltativo")
    parser.add_argument("--data-dir", type=Path, default=configuration.DEFAULT_DATA)
    parser.add_argument("--ca-file", type=Path, help="CA interna; default data-dir/tls/ca.crt")
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create-user", help="Crea un'identità o riconosce un subject esplicito senza modificarlo")
    create.add_argument("--username", required=True)
    create.add_argument("--email", required=True)
    create.add_argument("--name", required=True)
    create.add_argument("--password-file", required=True, type=Path)
    create.add_argument("--subject", help="ID verificato richiesto per un utente preesistente non creato da questo script")
    create.add_argument("--temporary-password", action="store_true", help="Richiedi cambio password al primo login")
    create.add_argument("--bind", nargs=2, action="append", default=[], metavar=("APP", "EMAIL_LOCALE"))
    args = parser.parse_args(argv)
    try:
        admin = KeycloakAdmin(args.container, args.ca_file or args.data_dir / "tls" / "ca.crt", context=args.context)
        result = create_user(admin, args.data_dir, args.username, args.email, args.name, args.password_file,
                             args.bind, subject=args.subject, temporary_password=args.temporary_password)
        print(json.dumps(result, ensure_ascii=False))
        print("Password e utenti preesistenti conservati. Riesegui auth-config-init e riavvia le app con binding modificati.")
        return 0
    except (AdministrationError, configuration.ConfigurationError) as exc:
        print(f"Provisioning non completato: {exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError, UnicodeError):
        # Do not expose file content or remote response bodies in diagnostics.
        print("Provisioning non completato; nessuna credenziale esposta. Verifica file protetti, subject e stato del servizio.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
