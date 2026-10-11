#!/usr/bin/env python3
"""Migrate explicit Punto groups and provision a read-only Keycloak sync client."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import secrets
import sys

import configure_keycloak as config
from manage_keycloak import AdministrationError, KeycloakAdmin

SPEC = importlib.util.spec_from_file_location("punto_group_sync", config.PROJECT / "auth/keycloak/sync_groups.py")
sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync)
CLIENT_ID = "punto-groups-sync"
READ_ROLES = ("query-groups", "view-users")


def query(admin, path, **parameters):
    arguments = ["get", path, "-r", "punto"]
    for name, value in parameters.items():
        arguments.extend(["-q", name + "=" + str(value)])
    try:
        return json.loads(admin.command(arguments))
    except (ValueError, UnicodeError):
        raise AdministrationError("Risposta amministrativa non valida.") from None


class AdminReader:
    def __init__(self, admin):
        self.admin = admin

    def get(self, path, **parameters):
        return query(self.admin, path, **parameters)


def save(path, value):
    if path.exists() or path.is_symlink():
        config.write_private(path, config.json_text(value))
    else:
        config.write_new(path, config.json_text(value))


def validate_legacy(value):
    if not isinstance(value, dict) or set(value) != {"groups"} or not isinstance(value["groups"], list):
        raise AdministrationError("Snapshot gruppi non valido.")
    if len(value["groups"]) > sync.GROUP_LIMIT:
        raise AdministrationError("Massimo 200 gruppi.")
    groups, siblings = {}, set()
    for row in value["groups"]:
        if not isinstance(row, dict) or type(row.get("id")) is not int or not 0 < row["id"] <= 9223372036854775807:
            raise AdministrationError("ID gruppo locale non valido.")
        local_id = row["id"]
        parent = row.get("parent_id")
        if parent == 0 and type(parent) is int:
            parent = None
        if parent is not None and (type(parent) is not int or parent <= 0):
            raise AdministrationError("Gruppo superiore non valido.")
        try:
            name = sync.group_name(row.get("name"))
        except sync.SyncError:
            raise AdministrationError("Nome gruppo non valido: massimo 120 caratteri.") from None
        members = row.get("members")
        if (not isinstance(members, list) or len(members) > sync.MEMBER_LIMIT
                or any(type(member) is not int or not 0 < member <= 9223372036854775807 for member in members)
                or len(set(members)) != len(members)):
            raise AdministrationError("Membri gruppo non validi.")
        if local_id in groups or (parent, name) in siblings:
            raise AdministrationError("Gruppi duplicati: Keycloak richiede nomi distinti nello stesso gruppo superiore.")
        siblings.add((parent, name))
        groups[local_id] = {"id": local_id, "name": name, "parent_id": parent, "members": members}
    ordered, visited, visiting = [], set(), set()

    def visit(local_id, depth):
        if local_id in visiting or depth > sync.DEPTH_LIMIT:
            raise AdministrationError("Gerarchia ciclica o superiore a 16 livelli.")
        if local_id not in groups:
            raise AdministrationError("Gruppo superiore assente dallo snapshot.")
        row = groups[local_id]
        if local_id in visited:
            return
        visiting.add(local_id)
        if row["parent_id"] is not None:
            visit(row["parent_id"], depth + 1)
        visiting.remove(local_id)
        visited.add(local_id)
        ordered.append(row)

    # Validate every ancestor chain independently, even when a parent was
    # already traversed, to enforce the complete hierarchy depth limit.
    for local_id in groups:
        chain, current = set(), local_id
        while current is not None:
            if current in chain or len(chain) >= sync.DEPTH_LIMIT or current not in groups:
                raise AdministrationError("Gerarchia non valida o superiore a 16 livelli.")
            chain.add(current)
            current = groups[current]["parent_id"]
        visit(local_id, 1)
    return ordered


def user_identities(manifest):
    if not isinstance(manifest, dict) or len(manifest) > 1024:
        raise AdministrationError("Manifest utenti non valido.")
    result = {}
    for email, person in manifest.items():
        if not isinstance(person, dict) or not isinstance(person.get("apps"), dict):
            raise AdministrationError("Manifest utenti non valido.")
        subject, _ = config.validate_binding(person.get("subject"), email)
        local = person["apps"].get("punto")
        if local is None:
            continue
        if not isinstance(local, dict) or type(local.get("id")) is not int or local["id"] <= 0 or local["id"] in result:
            raise AdministrationError("ID utenti Punto duplicati o non validi.")
        result[local["id"]] = subject
    return result


def ensure_sync_client(admin, data):
    manifest_path = data / "groups-sync-client.json"
    previous = config.read_json(manifest_path) if manifest_path.exists() or manifest_path.is_symlink() else None
    matches = query(admin, "clients", clientId=CLIENT_ID)
    if not isinstance(matches, list) or len(matches) > 1:
        raise AdministrationError("Client sincronizzazione ambiguo.")
    secret_path = data / "secrets/groups-client-secret"
    if matches:
        if (not isinstance(previous, dict) or previous.get("id") != matches[0].get("id")
                or matches[0].get("clientId") != CLIENT_ID):
            raise AdministrationError("Client sincronizzazione preesistente senza proprietà esplicita; nessun client adottato.")
        secret = config.regular_file(secret_path).read_text(encoding="ascii").strip()
        if not re.fullmatch(r"[0-9a-f]{64}", secret):
            raise AdministrationError("Segreto sincronizzazione non valido.")
        if previous.get("secret_sha256") != hashlib.sha256(secret.encode("ascii")).hexdigest():
            raise AdministrationError("Segreto sincronizzazione diverso dal manifest.")
        cid = previous["id"]
    else:
        if previous is not None:
            raise AdministrationError("Client sincronizzazione registrato ma assente; nessun nuovo ID creato.")
        config.private_directory(data / "secrets")
        secret = config.prepare_secret(secret_path)
        payload = {"clientId": CLIENT_ID, "name": "Punto group directory reader", "enabled": True,
                   "protocol": "openid-connect", "publicClient": False, "clientAuthenticatorType": "client-secret",
                   "secret": secret, "serviceAccountsEnabled": True, "standardFlowEnabled": False,
                   "implicitFlowEnabled": False, "directAccessGrantsEnabled": False, "fullScopeAllowed": False,
                   "redirectUris": [], "webOrigins": [], "defaultClientScopes": ["roles"], "optionalClientScopes": []}
        raw = admin.command(["create", "clients", "-r", "punto", "-f", "-", "-i"], payload=payload)
        cid = sync.identifier(raw.decode("utf-8").strip().strip('"'))
        save(manifest_path, {"id": cid, "client_id": CLIENT_ID,
                             "secret_sha256": hashlib.sha256(secret.encode("ascii")).hexdigest()})
    detail = query(admin, "clients/" + cid)
    if (not isinstance(detail, dict) or detail.get("clientId") != CLIENT_ID or detail.get("protocol") != "openid-connect"
            or detail.get("clientAuthenticatorType") != "client-secret"
            or any(detail.get(flag) is not expected for flag, expected in {
                "enabled": True, "publicClient": False, "serviceAccountsEnabled": True,
                "standardFlowEnabled": False, "implicitFlowEnabled": False,
                "directAccessGrantsEnabled": False, "fullScopeAllowed": False}.items())
            or detail.get("redirectUris") or detail.get("webOrigins")
            # Keycloak 26.8 automatically attaches its built-in service_account
            # scope to service-account clients, even when only roles is supplied.
            or not isinstance(detail.get("defaultClientScopes"), list)
            or set(detail["defaultClientScopes"]) not in ({"roles"}, {"roles", "service_account"})
            or detail.get("optionalClientScopes")):
        raise AdministrationError("Configurazione client sincronizzazione incompatibile; nessuna modifica implicita.")
    remote_secret = query(admin, "clients/" + cid + "/client-secret")
    if (not isinstance(remote_secret, dict) or not isinstance(remote_secret.get("value"), str)
            or not secrets.compare_digest(remote_secret["value"], secret)):
        raise AdministrationError("Segreto remoto sincronizzazione diverso dal file locale.")
    management = query(admin, "clients", clientId="realm-management")
    if not isinstance(management, list) or len(management) != 1 or management[0].get("clientId") != "realm-management":
        raise AdministrationError("Client realm-management mancante o ambiguo.")
    mid = sync.identifier(management[0]["id"])
    definitions = [query(admin, "clients/" + mid + "/roles/" + name) for name in READ_ROLES]
    account = query(admin, "clients/" + cid + "/service-account-user")
    subject = sync.identifier(account.get("id"))
    for path in ("users/" + subject + "/role-mappings/clients/" + mid,
                 "clients/" + cid + "/scope-mappings/clients/" + mid):
        current = query(admin, path)
        if not isinstance(current, list) or any(role.get("name") not in READ_ROLES for role in current):
            raise AdministrationError("Client sincronizzazione con permessi non previsti; nessun ruolo rimosso implicitamente.")
        present = {role["id"] for role in current}
        missing = [role for role in definitions if role["id"] not in present]
        if missing:
            admin.command(["create", path, "-r", "punto", "-f", "-"], payload=missing)
    return {"client_id": CLIENT_ID, "client_uuid": cid, "secret_file": str(secret_path)}


def migrate(admin, data, legacy, users_manifest):
    ordered = validate_legacy(legacy)
    identities = user_identities(users_manifest)
    needed = {member for row in ordered for member in row["members"]}
    if not needed.issubset(identities):
        raise AdministrationError("Membro locale senza subject esplicito nel manifest utenti.")
    for local_id in sorted(needed):
        subject = identities[local_id]
        user = query(admin, "users/" + subject)
        if not isinstance(user, dict) or user.get("id") != subject:
            raise AdministrationError("Identità utente migrata assente da Keycloak; nessun collegamento per email.")
    manifest_path = data / "managed-groups-migration.json"
    manifest = config.read_json(manifest_path) if manifest_path.exists() or manifest_path.is_symlink() else {}
    if not isinstance(manifest, dict):
        raise AdministrationError("Manifest gruppi non valido.")
    reader = AdminReader(admin)
    remote = {row["id"]: row for row in sync.directory(reader)}
    seen_subjects = set()
    for key, previous in manifest.items():
        if (not key.isdecimal() or not isinstance(previous, dict) or previous.get("subject") not in remote
                or previous["subject"] in seen_subjects or type(previous.get("complete")) is not bool):
            raise AdministrationError("Gruppo registrato assente o manifest incoerente; nessun gruppo ricreato.")
        seen_subjects.add(previous["subject"])
    # Refuse name adoption before creating anything. A parent not yet created
    # cannot already contain a remote child, because its identity is new.
    for row in ordered:
        previous = manifest.get(str(row["id"]))
        if previous is not None:
            continue
        parent = manifest.get(str(row["parent_id"])) if row["parent_id"] is not None else None
        if row["parent_id"] is not None and parent is None:
            continue
        parent_subject = parent["subject"] if parent else None
        if any(group["name"] == row["name"] and group["parent_id"] == parent_subject for group in remote.values()):
            raise AdministrationError("Gruppo Keycloak omonimo preesistente senza proprietà esplicita; nessuna adozione per nome.")
    config.private_directory(data / "bindings")
    binding_path = data / "bindings/groups.json"
    bindings = sync.load_group_bindings(binding_path) if binding_path.exists() or binding_path.is_symlink() else {}
    for key, previous in manifest.items():
        if previous["subject"] in bindings and bindings[previous["subject"]] != int(key):
            raise AdministrationError("Binding gruppo incompatibile con il manifest.")
    service = ensure_sync_client(admin, data)
    created = added = 0
    for row in ordered:
        key = str(row["id"])
        previous = manifest.get(key)
        if previous is None:
            parent = manifest[str(row["parent_id"])]["subject"] if row["parent_id"] is not None else None
            path = "groups/" + parent + "/children" if parent else "groups"
            # Parent was just created or already ownership-verified above.
            raw = admin.command(["create", path, "-r", "punto", "-f", "-", "-i"], payload={"name": row["name"]})
            subject = sync.identifier(raw.decode("utf-8").strip().strip('"'))
            previous = manifest[key] = {"subject": subject, "complete": False}
            save(manifest_path, manifest)
            created += 1
        subject = previous["subject"]
        bindings[subject] = row["id"]
        save(binding_path, bindings)
        # Completed migrations never restore a membership deliberately removed
        # later by the Keycloak administrator, nor overwrite provider renames.
        if previous["complete"]:
            continue
        current = {member["id"] for member in sync.pages(reader, "groups/" + subject + "/members", sync.MEMBER_LIMIT)}
        if len(current | {identities[local_id] for local_id in row["members"]}) > sync.MEMBER_LIMIT:
            raise AdministrationError("La migrazione supererebbe il limite di 200 membri nel gruppo.")
        for local_id in row["members"]:
            member = identities[local_id]
            if member not in current:
                admin.command(["update", "users/" + member + "/groups/" + subject, "-r", "punto", "-n"])
                added += 1
        previous["complete"] = True
        save(manifest_path, manifest)
    # Empty installations still need an explicit empty group binding file.
    if not binding_path.exists():
        save(binding_path, bindings)
    return dict(service, groups=len(ordered), created=created, memberships_added=added,
                bindings_file=str(binding_path), migration_manifest=str(manifest_path), local_ids_preserved=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--context")
    parser.add_argument("--data-dir", type=Path, default=config.DEFAULT_DATA)
    parser.add_argument("--legacy-groups-file", type=Path, required=True)
    parser.add_argument("--users-manifest", type=Path)
    args = parser.parse_args()
    try:
        data = args.data_dir.resolve()
        users_path = args.users_manifest or data / "managed-users-migration.json"
        with config.locked(data):
            legacy = config.read_json(args.legacy_groups_file)
            users = config.read_json(users_path)
            with KeycloakAdmin(args.container, data / "tls/ca.crt", context=args.context) as admin:
                report = migrate(admin, data, legacy, users)
            save(data / "managed-groups-report.json", report)
        print(config.json_text(report), end="")
        return 0
    except (AdministrationError, config.ConfigurationError, sync.SyncError, OSError, ValueError, KeyError, TypeError) as error:
        message = str(error) if isinstance(error, (AdministrationError, config.ConfigurationError)) else "Configurazione gruppi non completata; file e identità esistenti conservati."
        print("Errore: " + message, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
