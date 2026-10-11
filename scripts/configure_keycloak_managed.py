#!/usr/bin/env python3
"""Enable central user roles and explicitly migrate existing application identities."""
import argparse
import json
from pathlib import Path
import secrets
import sys

import configure_keycloak as config
from manage_keycloak import KeycloakAdmin, AdministrationError

ROLES = {'punto': ('manager', 'worker'), 'tickets': ('admin', 'agent')}


def query(admin, path, *arguments):
    return json.loads(admin.command(['get', path, '-r', 'punto', *arguments]))


def client(admin, client_id):
    matches = query(admin, 'clients', '-q', 'clientId=' + client_id)
    if not isinstance(matches, list) or len(matches) != 1 or matches[0].get('clientId') != client_id:
        raise AdministrationError('Client Keycloak mancante o ambiguo: ' + client_id)
    return matches[0]['id']


def ensure_clients(admin):
    ids, definitions = {}, {}
    for app, client_id in config.APPS.items():
        cid = ids[app] = client(admin, client_id)
        definitions[app] = {}
        for name in ROLES[app]:
            existing = [r for r in query(admin, 'clients/' + cid + '/roles') if r.get('name') == name]
            if not existing:
                admin.command(['create', 'clients/' + cid + '/roles', '-r', 'punto', '-f', '-'], payload={'name': name})
                existing = [query(admin, 'clients/' + cid + '/roles/' + name)]
            definitions[app][name] = existing[0]
        scoped = query(admin, 'clients/' + cid + '/scope-mappings/clients/' + cid)
        scoped_ids = {r['id'] for r in scoped}
        missing = [r for r in definitions[app].values() if r['id'] not in scoped_ids]
        if missing:
            admin.command(['create', 'clients/' + cid + '/scope-mappings/clients/' + cid, '-r', 'punto', '-f', '-'], payload=missing)
        desired = config.managed_role_mapper(client_id)
        mappers = [m for m in query(admin, 'clients/' + cid + '/protocol-mappers/models') if m.get('name') == desired['name']]
        if len(mappers) > 1:
            raise AdministrationError('Mapper ruoli ambiguo; nessun mapper sovrascritto.')
        if mappers:
            existing = {k: v for k, v in mappers[0].items() if k != 'id'}
            if existing != desired:
                raise AdministrationError('Mapper ruoli omonimo incompatibile; configurazione conservata.')
        else:
            admin.command(['create', 'clients/' + cid + '/protocol-mappers/models', '-r', 'punto', '-f', '-'], payload=desired)
    return ids, definitions


def validate_legacy(value):
    if not isinstance(value, dict) or set(value) != set(ROLES):
        raise AdministrationError('Snapshot utenti deve contenere punto e tickets.')
    people = {}
    for app, rows in value.items():
        if not isinstance(rows, list) or len(rows) > 1024:
            raise AdministrationError('Snapshot utenti non valido.')
        seen, seen_ids = set(), set()
        for row in rows:
            if not isinstance(row, dict):
                raise AdministrationError('Utente non valido.')
            _, email = config.validate_binding('migration', row.get('email'))
            active = row.get('active')
            valid_active = type(active) is bool or (type(active) is int and active in (0, 1))
            if (email in seen or row.get('role') not in ROLES[app] or type(row.get('id')) is not int
                    or row['id'] <= 0 or row['id'] in seen_ids or not valid_active):
                raise AdministrationError('Identità locale duplicata o non valida.')
            seen.add(email)
            seen_ids.add(row['id'])
            person = people.setdefault(email, {'name': row.get('name'), 'apps': {}})
            if not isinstance(person['name'], str) or not person['name'].strip() or any(ord(c) < 32 for c in person['name']):
                raise AdministrationError('Nome locale non valido.')
            person['apps'][app] = {'id': row['id'], 'role': row['role'], 'active': bool(row.get('active'))}
    return people


def migrate(admin, data, legacy, admin_subject):
    people = validate_legacy(legacy)
    administrator = people.get('admin@punto.local')
    if not administrator or set(administrator['apps']) != set(ROLES) or any(
            not r['active'] or r['role'] != ROLES[app][0] for app, r in administrator['apps'].items()):
        raise AdministrationError('Lo snapshot deve includere l’amministratore attivo di entrambe le app.')
    manifest_path = data / 'managed-users-migration.json'
    manifest = config.read_json(manifest_path) if manifest_path.exists() else {}
    credentials_path = data / 'credentials/migrated-users.json'
    credentials = config.read_json(credentials_path) if credentials_path.exists() else {}
    maps = {app: config.load_bindings(data / 'bindings' / (app + '.json')) for app in ROLES}
    found = {email: admin.find_user(email) for email in people}
    if not found['admin@punto.local'] or found['admin@punto.local'].get('id') != admin_subject:
        raise AdministrationError('Amministratore Keycloak assente o subject diverso da quello verificato.')
    for email, existing in found.items():
        if not existing:
            if email in manifest:
                raise AdministrationError('Identità già migrata assente da Keycloak; nessun subject ricreato.')
            continue
        subject = existing.get('id')
        if email in manifest:
            known = manifest[email].get('subject') == subject
        else:
            known = any(maps[app].get(subject) == email for app in people[email]['apps'])
        if not known or existing.get('email', '').lower() != email:
            raise AdministrationError('Identità Keycloak preesistente non collegata esplicitamente; migrazione rifiutata.')
        if email == 'admin@punto.local' and subject != admin_subject:
            raise AdministrationError('Subject amministratore diverso da quello verificato.')
    config.private_directory(data / 'credentials')
    ids, roles = ensure_clients(admin)
    created = 0
    for email, person in people.items():
        existing = found[email]
        if not existing:
            password = credentials.get(email, {}).get('password') or secrets.token_urlsafe(32)
            credentials[email] = {'password': password, 'temporary': True}
            if credentials_path.exists(): config.write_private(credentials_path, config.json_text(credentials))
            else: config.write_new(credentials_path, config.json_text(credentials))
            first, _, last = person['name'].strip().partition(' ')
            payload = {'username': email, 'email': email, 'firstName': first, 'lastName': last.strip() or 'Punto',
                       'enabled': any(r['active'] for r in person['apps'].values()), 'emailVerified': False,
                       'credentials': [{'type': 'password', 'value': password, 'temporary': True}]}
            subject = admin.create_user(payload)
            created += 1
        else:
            subject = existing['id']
        manifest[email] = {'subject': subject, 'apps': person['apps']}
        if manifest_path.exists(): config.write_private(manifest_path, config.json_text(manifest))
        else: config.write_new(manifest_path, config.json_text(manifest))
        for app, local in person['apps'].items():
            config.bind_account(data, app, subject, email)
            current = query(admin, 'users/' + subject + '/role-mappings/clients/' + ids[app])
            allowed = roles[app][local['role']] if local['active'] else None
            remove = [r for r in current if r.get('name') in ROLES[app] and (not allowed or r['id'] != allowed['id'])]
            path = 'users/' + subject + '/role-mappings/clients/' + ids[app]
            if remove: admin.command(['delete', path, '-r', 'punto', '-f', '-'], payload=remove)
            if allowed and not any(r['id'] == allowed['id'] for r in current):
                admin.command(['create', path, '-r', 'punto', '-f', '-'], payload=[allowed])
    management_id = client(admin, 'realm-management')
    realm_admin = query(admin, 'clients/' + management_id + '/roles/realm-admin')
    path = 'users/' + admin_subject + '/role-mappings/clients/' + management_id
    if not any(r['id'] == realm_admin['id'] for r in query(admin, path)):
        admin.command(['create', path, '-r', 'punto', '-f', '-'], payload=[realm_admin])
    return {'identities': len(people), 'created': created, 'admin_subject': admin_subject,
            'migration_manifest': str(manifest_path), 'initial_passwords': str(credentials_path),
            'admin_console': 'punto', 'local_ids_preserved': True}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--container', required=True)
    p.add_argument('--context')
    p.add_argument('--data-dir', type=Path, default=config.DEFAULT_DATA)
    p.add_argument('--legacy-users-file', type=Path, required=True)
    p.add_argument('--admin-subject', required=True)
    args = p.parse_args()
    try:
        config.validate_binding(args.admin_subject, 'admin@punto.local')
        legacy = config.read_json(args.legacy_users_file)
        with config.locked(args.data_dir):
            with KeycloakAdmin(args.container, args.data_dir / 'tls/ca.crt', context=args.context) as admin:
                result = migrate(admin, args.data_dir, legacy, args.admin_subject)
            settings = config.read_json(args.data_dir / 'config.json')
            secrets_by_app = {app: (args.data_dir / 'secrets' / (app + '-client-secret')).read_text().strip() for app in config.APPS}
            config.write_private(args.data_dir / 'realm/punto-realm.json', config.json_text(config.make_realm(settings, secrets_by_app)))
        print(json.dumps(result))
        return 0
    except (AdministrationError, config.ConfigurationError):
        print('Migrazione non completata; verifica snapshot, subject e configurazione Keycloak. Nessuna credenziale mostrata.', file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError):
        print('Migrazione non completata per file o risposta non valida; nessuna credenziale mostrata.', file=sys.stderr)
        return 1

if __name__ == '__main__': sys.exit(main())
