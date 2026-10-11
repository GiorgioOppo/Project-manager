# Multi-app authentication with Keycloak

[Italiano](../../tehnical/keycloak.md) | **English**

Keycloak provides a shared identity and SSO session for Punto and Punto Tickets. Each app retains its own store, permissions and session cookies. The `punto` realm contains two distinct confidential OIDC clients, `punto` and `punto-tickets`, with Authorization Code, PKCE S256 and exact `/auth/callback` redirects. Open registration, password grants, implicit flow and demo users are not enabled.

The integration is optional. Without the overlays, local authentication remains. Apps support `AUTH_MODE=local`, `hybrid` and `oidc`: `hybrid` retains local access while associations are verified; `oidc` requires Keycloak sign-in. Compose variables use the `KEYCLOAK_` prefix to avoid collisions with standalone app configuration.

## Components and network

The `compose.keycloak.yaml` overlay adds Keycloak **26.8.0**, dedicated PostgreSQL **17.11** and the initial `auth-config-init` service. Versions are pinned: Keycloak runs with `start --import-realm` in production mode. PostgreSQL stores data in `keycloak_database`; applications receive no access to the identity database.

| Component | Network and address | Use |
| --- | --- | --- |
| Keycloak | `private_app`, `172.30.20.12:8443` | HTTPS from Nginx Proxy Manager |
| Keycloak | `auth_api`, `172.30.90.2:8443` | HTTPS backchannel from Punto and Tickets |
| Punto | `auth_api`, `172.30.90.10` | Provider tokens and keys |
| Tickets | `auth_api`, `172.30.90.11` | Provider tokens and keys |
| Keycloak | `auth_database`, `172.30.91.2` | Dedicated database connection |
| PostgreSQL | `auth_database`, `172.30.91.3:5432` | Identity store |

`auth_api` and `auth_database` are internal Docker networks. No Keycloak or PostgreSQL port is published on the host. Management port `9000` listens exclusively on container loopback and serves the `/health/ready` check. External ingress goes through NPM; the Punto app continues to listen only on `172.30.20.10`.

Browser requests use the public issuer, for example `https://auth.example.test/realms/punto`. Tokens and JWKS travel directly over `https://keycloak:8443/realms/punto`; both apps verify the private CA. `KC_HOSTNAME` remains the complete public origin and tokens must contain the public issuer. Proxy headers are accepted only from NPM (`172.30.20.2/32`).

## Preparation without startup

First configure HTTPS and public DNS names for identity and each app. Use different origins without trailing slashes or subpaths; also configure Punto's `PUBLIC_ORIGINS` and `TICKETS_PUBLIC_ORIGIN` with their respective HTTPS origins. The authentication domain's public certificate is managed by NPM.

```sh
python3 scripts/configure_keycloak.py init \
  --issuer https://auth.example.test/realms/punto \
  --punto-origin https://punto.example.test \
  --tickets-origin https://tickets.example.test
```

If Tickets is not used, omit `--tickets-origin`: its client is disabled and has no allowed redirects. The script only prepares local files and does not start containers, change databases or migrate passwords.

Materials are created in `data/keycloak`, excluded from Git:

| Path | Content |
| --- | --- |
| `compose.env` | URLs and authentication modes; no passwords |
| `config.json` | Initial initialization parameters |
| `secrets/bootstrap-password` | Bootstrap administrator password |
| `secrets/database-password` | Dedicated PostgreSQL password |
| `secrets/punto-client-secret`, `secrets/tickets-client-secret` | Distinct secrets for both clients |
| `realm/punto-realm.json` | Realm to import, with expanded secrets |
| `bindings/punto.json`, `bindings/tickets.json` | Explicit associations between Keycloak subjects and local email addresses |
| `tls/` | Private CA and Keycloak internal HTTPS certificate |

Sensitive directories and files have permissions `0700` and `0600`. Public `.crt` certificates are `0644` inside protected directories. Networkless `auth-config-init` copies only necessary files into separate volumes and assigns `0400` permissions to the respective UIDs: Keycloak `1000`, PostgreSQL `999`, Punto `10001`, Tickets `10006`. Application containers receive their own secret, mapping and CA, mounted read-only. The CA private key stays on the host. The Keycloak wrapper reads bootstrap and database credentials from files without putting passwords in arguments or the Compose model.

`init` is idempotent: it retains secrets, certificates, mappings and `compose.env`, even when the operator changes the mode. It rejects URLs different from the initial ones, an inconsistent realm, symbolic links and incomplete TLS bundles. It does not automatically regenerate expired certificates or rotate secrets. The CA lasts ten years; the internal certificate one year. Plan renewal before expiry by checking both internal paths.

## Nginx Proxy Manager ingress

Manually create a Proxy Host for the issuer domain with a valid public certificate and **Force SSL**:

| NPM field | Value |
| --- | --- |
| Domain Names | `auth.example.test` |
| Scheme | `https` |
| Forward Hostname/IP | `keycloak` |
| Forward Port | `8443` |
| Cache Assets | Disabled |
| Websockets Support | Disabled |

In the Proxy Host advanced configuration, enable internal certificate verification:

```nginx
proxy_ssl_server_name on;
proxy_ssl_name keycloak;
proxy_ssl_verify on;
proxy_ssl_trusted_certificate /etc/punto/keycloak-ca.crt;
proxy_ssl_verify_depth 2;
```

NPM must overwrite `X-Forwarded-Host`, `X-Forwarded-Proto`, `X-Forwarded-Port` and `X-Forwarded-For` with values from the external connection. Do not add an `/auth` prefix to the proxy or change the issuer based on the Host header.

For a local installation on the same machine, the names `punto.localhost`, `ticket.localhost` and `auth.localhost` are available. Initialize Keycloak with these three HTTPS origins, then prepare the web certificate signed by the local CA:

```sh
python3 scripts/configure_local_sso_proxy.py --prepare-only
```

When Keycloak, app and NPM are running with the correct overlays, apply the three Proxy Hosts by explicitly specifying the ID of an existing active NPM administrator:

```sh
python3 scripts/configure_local_sso_proxy.py \
  --container projectmanament-proxy-web-1 --admin-id 1
```

The helper creates a consistent SQLite backup with `.backup` in `data/keycloak/npm-before-sso.sqlite`, retains the NPM password and opens a five-minute owner session only for API operations. It imports a certificate for the three local names and configures upstreams from the previous table; it rejects incompatible hosts or certificates with the same names. It does not directly modify the NPM database. The certificate and key remain in `data/keycloak/tls/local-web.crt` and `local-web.key`, with key permissions `0600`. For macOS browsers, the following local procedure trusts only the web certificate, limited to the three names, in the user's keychain. The CA continues to verify internal connections. These local names and certificates do not replace public domains and certificates for a shared company service.

Before exposing the Proxy Host, restrict `/admin/` and `/realms/master/` to administrative VPN addresses through Custom Locations and an IP-based Access List in NPM; use the same HTTPS upstream and certificate verification. Public endpoints of realm `punto` and `/resources/` must remain available for sign-in. Do not publish port `9000`. Access the Keycloak panel from the administrative network, create a named administrator with MFA and remove the temporary `punto-bootstrap` account after bootstrap. The initial password is readable only from the file reported by `init`.

## Model validation and explicit startup

For Punto without Tickets:

```sh
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml config --quiet
```

For Punto and Tickets:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml config --quiet
```

Keep file order: base, Tickets if used, Keycloak, Tickets-Keycloak. Load `data/keycloak/compose.env` after other environment files. Add the Tickets-Keycloak overlay only when also using `compose.tickets.yaml`: the main overlay does not implicitly create the Tickets app.

When domains, NPM and bindings are ready, the same command with `up --build -d` instead of `config --quiet` starts the stack. If the stack is already running, plan app recreation to load the new configuration. The first startup may take longer to prepare Keycloak. Dependencies wait for secret copying, PostgreSQL availability and the Keycloak health check.

## Local startup with HTTPS

For local installation, the three names are `punto.localhost`, `ticket.localhost` and `auth.localhost`, all using HTTPS on NPM port `443`. The following commands assume the project directory and an already initialized MySQL cluster. If using other ports or another Docker project, also adjust origins, redirects and container names before preparation. Do not change URLs in an already initialized Keycloak bundle: `init` preserves them and rejects different configuration.

1. In `.env.docker`, add `https://punto.localhost` to `PUBLIC_ORIGINS`, retaining origins you still want to use. In `ticket-app/.env`, set `TICKETS_PUBLIC_ORIGIN=https://ticket.localhost`. Retain existing local credentials: they also support access during the `hybrid` transition.

2. Prepare the realm and web certificate for the three names:

   ```sh
   python3 scripts/configure_keycloak.py init \
     --issuer https://auth.localhost/realms/punto \
     --punto-origin https://punto.localhost \
     --tickets-origin https://ticket.localhost
   python3 scripts/configure_local_sso_proxy.py --prepare-only
   ```

   `data/keycloak/compose.env` initially retains `KEYCLOAK_AUTH_MODE=hybrid` and `KEYCLOAK_TICKETS_AUTH_MODE=hybrid`.

3. Start the stack with both overlays through the controller's conservative recovery. Then configure local Proxy Hosts by specifying the verified ID of an existing NPM administrator; the example uses `1`:

   ```sh
   python3 scripts/manage_cluster.py --docker-context desktop-linux \
     --keycloak --tickets start
   python3 scripts/configure_local_sso_proxy.py --context desktop-linux \
     --container projectmanament-proxy-web-1 --admin-id 1 --port 81
   ```

   `start` requires an initialized cluster; for initial cluster installation use `init` with the same flags. The controller also always loads `data/keycloak/compose.env` and `ticket-app/.env`, without regenerating Keycloak credentials or changing bindings. The proxy helper preserves incompatible preexisting hosts and the NPM administrator password.

4. On macOS, trust only the local web certificate for SSL use in the user's keychain:

   ```sh
   security add-trusted-cert -r trustAsRoot -p ssl \
     -k "$HOME/Library/Keychains/login.keychain-db" \
     data/keycloak/tls/local-web.crt
   ```

   This certificate covers `punto.localhost`, `ticket.localhost` and `auth.localhost`. After renewing the web certificate, update this trust entry too. In local macOS testing, `curl` automatically resolved `*.localhost` names: the hosts file was not changed. If Safari or another client uses a resolver that does not recognize them, manually add this line to the system hosts file:

   ```text
   127.0.0.1 punto.localhost ticket.localhost auth.localhost
   ```

   Browsers using a separate certificate store require trust configuration in their own store. The NPM administration port remains [http://127.0.0.1:81](http://127.0.0.1:81).

5. Open the [Keycloak console](https://auth.localhost/admin/master/console/). The initial administrator is `punto-bootstrap`; its password is in the protected `data/keycloak/secrets/bootstrap-password` file. Create an identity and associate it with existing local accounts as described below. To use the CLI helper, replace username, email and password file with those chosen for this installation:

   ```sh
   python3 scripts/manage_keycloak.py --context desktop-linux \
     --container projectmanament-keycloak-1 \
     create-user --username utente.sso --email utente@example.test \
     --name 'Utente SSO' --password-file /percorso/protetto/password-sso \
     --bind punto EMAIL_LOCALE_PUNTO --bind tickets EMAIL_LOCALE_TICKETS
   ```

   The password file must already exist with `0600` permissions. For an existing identity without a local manifest, also pass `--subject ID_VERIFICATO`; the helper does not select a user based solely on email.

6. After creating bindings, copy them to runtime volumes and restart both apps:

   ```sh
   docker --context desktop-linux compose \
     --env-file .env.docker --env-file ticket-app/.env \
     --env-file data/keycloak/compose.env \
     -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
     -f compose.tickets-keycloak.yaml run --rm --no-deps auth-config-init
   docker --context desktop-linux compose \
     --env-file .env.docker --env-file ticket-app/.env \
     --env-file data/keycloak/compose.env \
     -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
     -f compose.tickets-keycloak.yaml restart app tickets
   ```

   Open [Punto](https://punto.localhost/) and choose **Sign in with your company account**. After signing in, open [Tickets](https://ticket.localhost/) in the same browser and choose the same sign-in method: the provider session enables the second sign-in without re-entering the password. Both apps show the associated local accounts' permissions. Local sign-in remains available while the mode is `hybrid`.

## User management exclusively in Keycloak

`AUTH_USER_MANAGEMENT=keycloak` centralizes user creation, editing, credentials, enabled status and roles. In the overlays, enable it with `KEYCLOAK_USER_MANAGEMENT=keycloak` in `data/keycloak/compose.env`. It is distinct from `AUTH_MODE`: the local stack uses `hybrid`, with local access restricted to `KEYCLOAK_LOCAL_ADMIN_EMAIL=admin@punto.local`; `oidc` removes even this option. Without the new setting, previous local management remains available.

In this mode, Punto and Tickets hide user editors and reject local create/edit APIs. The **Manage users in Keycloak** / **Keycloak users** link opens the [punto realm console](https://auth.localhost/admin/punto/console/). The administrative role `realm-management → realm-admin` authorizes the shared administrator only in that realm's console; application roles alone do not grant administrative access to the provider.

For a new user, create a profile with email, first name, last name and credentials in the console, then assign roles in **Role mapping → Assign role → Filter by clients**:

| Client | Manager | Operator |
| --- | --- | --- |
| `punto` | `manager` | `worker` |
| `punto-tickets` | `admin` | `agent` |

Assign only the necessary clients. No application role is assigned by default: sign-in to an app without one of its roles is rejected. If both roles are assigned, the administrative role takes precedence. Clients retain `fullScopeAllowed=false` and include only their own roles in their ID token through the signed `app_roles` claim. The `profile` and `email` scopes must be enabled.

The first SSO for a new subject creates a technical app profile with an unusable local password. Name, email, role and enabled status are updated at the SSO callback; assignments and history use the stable local ID. The `oidc_identities` table retains the issuer/subject pair and prevents a second subject from claiming the same account. An email matching an existing profile does not create an association: an explicit administrative binding is required first. After the initial association, the persistent subject remains authoritative even if the email changes.

Disabling the user or removing roles in Keycloak blocks new sign-ins. Existing OIDC application sessions expire within token validity (five minutes in the initial realm, a maximum of eight hours); push revocation is not implemented. The local profile may retain the last known enabled status for history. The emergency administrator remains an independent local credential: disabling it in the provider does not disable the fallback authorized by `AUTH_LOCAL_ADMIN_EMAIL`.

### Migrating an already running local stack

First create consistent backups of MySQL, PostgreSQL and `data/keycloak`. Save a JSON snapshot from both apps' authenticated `GET /api/users` endpoints in the form `{"punto": [...], "tickets": [...]}`: each row must have `id`, `email`, `name`, `role`, `active`. The following helper concerns the stack with shared administrator `admin@punto.local`, already explicitly associated with both apps. Pass its verified subject without selecting it by matching email:

```sh
python3 scripts/configure_keycloak_managed.py \
  --container projectmanament-keycloak-1 \
  --legacy-users-file data/keycloak/legacy-users-before-managed.json \
  --admin-subject SUBJECT-ADMIN-VERIFICATO
```

The helper adds roles, scopes and mappers to existing clients through the Admin API, explicitly migrates every identity and records bindings. It preserves local IDs and users already disabled; existing provider users require a manifest or explicit binding. Rerunning it neither changes passwords nor recreates previously migrated identities that are now missing. Newly created accounts receive random temporary passwords, saved only in the private `data/keycloak/credentials/migrated-users.json` file (`0600`), to be changed at first sign-in. The `managed-users-migration.json` manifest records subjects and app IDs without passwords. Realm JSON is updated for future installations; live updates use APIs because `--import-realm` skips an existing realm.

Then set the following in `data/keycloak/compose.env`:

```dotenv
KEYCLOAK_USER_MANAGEMENT=keycloak
KEYCLOAK_LOCAL_ADMIN_EMAIL=admin@punto.local
KEYCLOAK_USERS_ADMIN_URL=https://auth.localhost/admin/punto/console/
KEYCLOAK_AUTH_MODE=hybrid
KEYCLOAK_TICKETS_AUTH_MODE=hybrid
```

Also retain the chosen administrator in local bootstrap variables `PUNTO_EMAIL/PUNTO_PASSWORD` and `TICKETS_EMAIL/TICKETS_PASSWORD`, used only on empty databases. Copy bindings, build and recreate the apps with all overlays:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml run --rm --no-deps auth-config-init
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml build app tickets
docker compose --env-file .env.docker --env-file ticket-app/.env \
  --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.keycloak.yaml \
  -f compose.tickets-keycloak.yaml up -d --no-deps --no-build app tickets
```

App sessions are in memory and are invalidated by recreation. Verify shared SSO, preserved local IDs, absent editors, rejected user write APIs, an accessible administration console and local fallback restricted to the administrator. Use the console directly for subsequent new users; there is no need to rerun migration or prepare profiles in the apps.

## Explicit association with local management

This section describes `AUTH_USER_MANAGEMENT=local`, useful for gradual migrations where apps retain role management.


Existing Punto or Tickets users retain their respective roles and permissions. Create the identity in Keycloak, set credentials according to company policy and retrieve its user ID, used as the `sub` claim. Associate that ID with the email of an **existing local account** in each authorized app:

```sh
python3 scripts/configure_keycloak.py bind \
  --app punto --subject ID-UTENTE-KEYCLOAK --email utente@example.test
python3 scripts/configure_keycloak.py bind \
  --app tickets --subject ID-UTENTE-KEYCLOAK --email utente@example.test
```

The generated file uses the format `{"ID-UTENTE-KEYCLOAK": "utente@example.test"}`. The script prepares the mapping; the app checks that the local account exists and is enabled at sign-in. A binding can also be provided during first preparation with `init ... --bind punto ID-UTENTE-KEYCLOAK utente@example.test`. An already associated subject cannot be reassigned to another email without explicit removal using `unbind --app punto --subject ID-UTENTE-KEYCLOAK`.

The email declared by Keycloak does not enable automatic linking. Identity verification depends on the verified issuer/subject pair, administrative mapping and local permissions. Local passwords are not exported or imported into Keycloak.

To create a new identity operationally with a private password file, you can use the administrative helper. First verify the existing local accounts to associate and prepare a new SSO password in a `0600` file without putting it in command arguments:

```sh
python3 scripts/manage_keycloak.py --container projectmanament-keycloak-1 \
  create-user --username mario.rossi --email mario.rossi@example.test \
  --name 'Mario Rossi' --password-file /percorso/protetto/password-sso \
  --bind punto mario.rossi@example.test \
  --bind tickets mario.ticket@example.test
```

Adjust the container name to the running Docker project. The helper uses the configured bootstrap administrator, verifies the internal CA and sends the credential payload through stdin. Passwords and tokens do not enter arguments or output. The administrative CLI session is kept in a temporary directory inside the container and removed at the end. `--temporary-password` requires a password change at first sign-in.

Always provide first and last name, for example `--name 'Gestore Punto'`: the realm's default profile requires both. The returned subject is recorded in protected manifest `data/keycloak/provisioned-users.json`, so repeating the command neither creates a second user nor changes an existing identity's password. A preexisting username without a manifest requires `--subject ID-VERIFICATO`; matching email alone is insufficient. The helper does not change existing users, saves previous mapping copies in `data/keycloak/binding-backups/` and applies only explicitly supplied bindings. After the bootstrap administrator is removed, provisioning continues through a named administrator in the provider panel.

After every binding change, copy the new mapping to the runtime volume and restart the affected app using the same Compose and environment files as the stack. For Punto without Tickets:

```sh
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml run --rm --no-deps auth-config-init
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml restart app
```

For Tickets, also include its environment and overlay files as in the previous validation command and restart `tickets`; if both apps were updated, restart `app tickets`. Restarting the app alone does not copy source files into volumes. Binding changes affect subsequent sign-ins; to revoke access immediately, also disable the app account, invalidate its local sessions and terminate the Keycloak session.

Verify sign-in in both apps, second access through the SSO session and rejection of a subject with no binding. Then set `KEYCLOAK_AUTH_MODE=oidc` and, if used, `KEYCLOAK_TICKETS_AUTH_MODE=oidc` in `data/keycloak/compose.env` and recreate the apps. `hybrid` remains available for an explicit transition.

## Subsequent management

To restart or recover the stack while retaining multi-app authentication, use the controller with explicit overlay selection:

```sh
python3 scripts/manage_cluster.py --keycloak --tickets start
python3 scripts/manage_cluster.py --keycloak --tickets recover
python3 scripts/manage_cluster.py --keycloak --tickets status
python3 scripts/manage_cluster.py --keycloak --tickets backup
```

`start` follows the same path as `recover`: it checks SQL status, reunites nodes or bootstraps the node with the most advanced GTIDs only when the existing procedure allows it, then starts the app, Tickets and reverse proxy. `status` reads the three nodes' status; `backup` saves MySQL without including Keycloak PostgreSQL. Neither operation starts services.

The controller combines files in base → Tickets → Keycloak → Tickets-Keycloak order and loads environments base → Tickets → Keycloak. Use `--keycloak` alone when you do not want Tickets. Default paths are `.env.docker`, `ticket-app/.env` and `data/keycloak/compose.env`; override them with `--env-file`, `--tickets-env-file` and `--keycloak-env-file`. The last two also require their respective selection flags. On macOS the default context is `desktop-linux`; for another daemon always pass `--docker-context NOME`.

Keycloak files or secrets do not automatically activate overlays. Repeat flags for every startup or recovery command, including after `down`: if Keycloak or Tickets containers exist in the project, including stopped ones, the controller blocks startup that omits their overlays before modifying containers. This prevents a later base-configuration `recover` from accidentally recreating the app with local sign-in only. The check does not reconstruct a previous selection after all containers are removed. For other Compose operations, including image updates and app recreation, retain the full file and environment lists shown above.

Explicit network migration also forwards the same selection to the controller that recovers and restarts the stack. If applying a network change with SSO and Tickets installed, use:

```sh
python3 scripts/configure_network.py --keycloak --tickets apply
```

`--docker-context`, `--env-file`, `--keycloak-env-file` and `--tickets-env-file` are also available with the same meanings. `prepare` still only prepares existing network files and does not activate Keycloak.

`--import-realm` applies to initial creation: Keycloak skips a realm already present in the database. Editing host JSON or rerunning `init` does not update users, URLs or secrets of an existing realm. To add Tickets after initialization, manually enable and configure the `punto-tickets` client in Keycloak with its origin and exact redirect, update `KEYCLOAK_TICKETS_*` variables and synchronize the client secret with the protected file; then rerun `auth-config-init` and recreate Tickets. Secret rotation requires the same coordination between provider and application.

The **Sign out** button ends that app's session. The Keycloak SSO session may allow subsequent sign-in without requesting the password again; terminate it using provider session management. Local app sessions remain valid until their own expiry or revocation. This integration does not implement federated logout or backchannel logout. Keycloak management mode synchronizes roles at the SSO callback; local management mode instead uses roles stored in the apps. A local session created through OIDC is limited to token validity and eight hours; the initial realm's token validity is five minutes.

Create a consistent PostgreSQL backup together with `data/keycloak`, protecting the realm, CA, secrets and credentials contained in the dump. Do not copy PostgreSQL volume files while the database is active. For a logical dump, using the same environment files and overlays as the stack:

```sh
umask 077
mkdir -p backups/keycloak
docker compose --env-file .env.docker --env-file data/keycloak/compose.env \
  -f compose.yaml -f compose.keycloak.yaml exec -T keycloak-db \
  pg_dump --username=keycloak --dbname=keycloak --format=custom \
  > backups/keycloak/keycloak-YYYYMMDD.dump
tar -czf backups/keycloak/config-YYYYMMDD.tar.gz data/keycloak
```

Replace `YYYYMMDD` with a unique date and check command outcomes before considering the backup valid. Retain dump and archive in a protected, preferably encrypted destination, and test restore in a separate environment. Existing MySQL and document backups do not automatically include Keycloak identities. Runtime volumes contain regenerable copies and do not replace source backups. Added authentication is a single instance: the app's MySQL cluster does not make Keycloak or PostgreSQL highly available.

Available offline tests:

```sh
python3 -m unittest discover -s tests -p 'test_keycloak_config.py' -v
```

To also verify production startup, both-client import and TLS availability against real Keycloak, explicitly enable the Docker test:

```sh
PUNTO_KEYCLOAK_DOCKER_TESTS=1 python3 -m unittest discover \
  -s tests -p 'test_keycloak_runtime.py' -v
```

The test creates a separate Docker project with temporary data and credentials, then removes project containers, networks and volumes at the end. It may download pinned images and requires a reachable Docker daemon; the real stack and application data are not used.

Official references checked on 11 October 2026: [Keycloak 26.8.0](https://www.keycloak.org/2026/10/keycloak-2680-released), [hostname and backchannel configuration](https://www.keycloak.org/server/hostname), [reverse proxy](https://www.keycloak.org/server/reverseproxy), [management interface](https://www.keycloak.org/server/management-interface), [administrator bootstrap](https://www.keycloak.org/server/bootstrap-admin-recovery) and [PostgreSQL 17.11](https://www.postgresql.org/docs/17/release-17-11.html).
