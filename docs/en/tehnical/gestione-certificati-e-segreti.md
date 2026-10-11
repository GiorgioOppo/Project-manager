# Certificate and secret management

[Italiano](../../tehnical/gestione-certificati-e-segreti.md) | **English**

Inventory and procedures referring to the code of **10 October 2026**. This manual describes what is currently generated or read and separates manual maintenance paths from rotations that still need dedicated tools. It contains no real values and has neither read nor modified installation materials.

Sources: [Compose](../../../compose.yaml), [TLS and secret preparation](../../../scripts/configure_network.py), [cluster accounts](../../../scripts/manage_cluster.py), [email client](../../../src/mail_client.c), [Calendar client](../../../src/calendar_client.c), [email service](../../../addon/src/mailer.c), [Calendar service](../../../calendar-addon/src/calendar.c). For protection boundaries, read [network and security](rete-sicurezza.md).

## Three categories to manage separately

1. **TLS certificates and keys**: authenticate the service and protect transport; renewing a leaf certificate does not re-encrypt databases.
2. **Authentication passwords and tokens**: grant access to databases, private APIs or providers; they must match between caller and recipient.
3. **Calendar AES key**: decrypts persisted data; changing it without migrating ciphertexts makes the store unreadable.

End-user passwords are derived with PBKDF2 and salts, not encrypted with a key to retain. There is no per-user/field key vault and no central key capable of recovering sign-in passwords.

## Private TLS inventory

The shared host path is `data/network-tls/`, excluded from Git and image contexts. The table lists filenames, not links to installation materials. Historical file and SAN names remain valid through Compose aliases; see [Docker naming](nomi-servizi-e-immagini.md).

| Host file | Consumer and mount | Function |
| --- | --- | --- |
| `ca.crt` | App, proxy-db, mysql-1/mysql-2/mysql-3, mongodb, mail, calendar, inventory; service-specific TLS paths | Public internal trust root |
| `ca.key` | Host operator/script only; no container mount | Signing internal certificates |
| `db.crt`, `db.key` | `mysql-1` → `/etc/mysql/tls/server.crt` and `server.key` | First node identity |
| `db2.crt`, `db2.key` | `mysql-2`, same internal paths | Second node identity |
| `db3.crt`, `db3.key` | `mysql-3`, same internal paths | Third node identity |
| `documents.crt`, `documents.key` | Original materials on the host | Document certificate and key used to create the PEM |
| `documents.pem` | `mongodb` → `/etc/mongo/tls/server.pem` | Concatenated key and certificate for MongoDB |
| `mail-addon.crt`, `mail-addon.key` | `mail` → `/etc/punto/tls/mail-addon.crt` and `.key` | HTTPS `mail-api:8090` |
| `calendar-addon.crt`, `calendar-addon.key` | `calendar` → `/etc/punto/tls/calendar-addon.crt` and `.key` | HTTPS `calendar-api:8091` |
| `inventory-addon.crt`, `inventory-addon.key` | `inventory` → `/etc/punto/tls/inventory-addon.crt` and `.key` | HTTPS `inventory-api:8092` |

The app and proxy-db receive `ca.crt` at `/etc/punto/tls/ca.crt`; SQL nodes at `/etc/mysql/tls/ca.crt`; MongoDB at `/etc/mongo/tls/ca.crt`. Squid uses system CAs for its own needs and does not receive the private CA key. Encryption of tunnels to Google/SMTP terminates at clients, not Squid.

### Certificate identities

| Leaf | DNS names required by configuration | Expected IPs |
| --- | --- | --- |
| db | `db`, `db-proxy` | `172.30.30.21` |
| db2 | `db2`, `db-proxy` | `172.30.30.22` |
| db3 | `db3`, `db-proxy` | `172.30.30.23` |
| documents | `documents`, `localhost` | `127.0.0.1`, `172.30.30.30` |
| mail-addon | `mail-api`, `mail-addon`, `localhost` | `127.0.0.1`, `172.30.60.2` |
| calendar-addon | `calendar-api`, `calendar-addon`, `localhost` | `127.0.0.1`, `172.30.70.2` |
| inventory-addon | `inventory-api`, `inventory-addon`, `localhost` | `127.0.0.1`, `172.30.80.2` |

`db-proxy` appears in SQL certificates because HAProxy forwards TCP without terminating TLS: the client still verifies the configured destination. Loopback IPs in addon/document certificates serve the intended checks; they do not publish new ports.

The CA is RSA 3072/SHA-256, valid for 3650 days from generation; leaf certificates are RSA 3072/SHA-256, valid for 365 days. These are issuance lifetimes, not the actual installation's expiry dates: read public certificates using the commands below for those.

## Addon secrets and provider credentials

The host path is `data/addon-secrets/`, protected by the parent directory. These are Compose secrets based on local files, not secrets managed by a KMS or Docker Swarm.

| Host file | Compose name/mount | Recipients | Use |
| --- | --- | --- | --- |
| `mail-api-token` | `mail_api_token` → `/run/secrets/mail_api_token` | App and mail | Shared bearer for the private email API |
| `smtp-password` | `mail_smtp_password` → `/run/secrets/mail_smtp_password` | Mail only | SMTP provider password when configured |
| `calendar-api-token` | `calendar_api_token` → `/run/secrets/calendar_api_token` | App and calendar | Shared bearer for the private Calendar API |
| `calendar-token-key` | `calendar_token_key` → `/run/secrets/calendar_token_key` | Calendar only | AES-256-GCM key, 32 bytes represented as 64 hexadecimal digits |
| `google-client-secret` | `google_client_secret` → `/run/secrets/google_client_secret` | Calendar only | Google OAuth client credential |

`prepare` generates the three hexadecimal files with distinct random values and initially creates the two provider files empty. It preserves existing files, rejects symlinks/non-regular files and validates the format of the three expected hexadecimal values. It does not print values or rotate them automatically.

Generated API tokens have 64 hexadecimal characters. C clients accept tokens of 32–128 ASCII characters in their allowed set, but retaining the generated format also preserves compatibility with `prepare` validation. The API token must not be used as an AES key or provider password.

The bearer is loaded into memory when processes start. Changing the disk file does not hot-reload the token. Also, atomic file replacement may leave an existing container attached to the previous inode: coordinated recreation is the intended way to reread files and environment.

## Environment credentials

| Local variable | Actual consumer | Operational note |
| --- | --- | --- |
| `PUNTO_EMAIL`, `PUNTO_PASSWORD` | App | Bootstrap of the first manager in an empty database; do not update an already saved user |
| `MYSQL_USER`, `MYSQL_PASSWORD` | Node initialization and app through `DB_USER`/`DB_PASSWORD` | The app receives its own SQL account, not root |
| `MYSQL_ROOT_PASSWORD` | Nodes and controller administration operations inside containers | Not provided to the app or addons |
| `CLUSTER_REPLICATION_PASSWORD` | Controller, which configures accounts and channels on nodes | `punto_recovery` account, maximum 32 bytes |
| `CLUSTER_MONITOR_PASSWORD` | Controller and proxy-db | `punto_monitor` account limited to intended status reads |
| `DOC_DB_PASSWORD` | App and MongoDB | Document account with a role limited to the document database |
| `DOC_DB_ROOT_PASSWORD` | MongoDB as administration password | Not provided to the app |
| `NPM_ADMIN_EMAIL`, `NPM_ADMIN_PASSWORD` | `configure_proxy.py` script | Panel credentials, not variables passed to the app container |
| `MAIL_SMTP_USER` | Mail | Provider login name; password in a separate secret |
| `GOOGLE_CLIENT_ID`, `GOOGLE_REDIRECT_URI` | Calendar | OAuth identifier and URI; separate client secret |

The controller generates the four missing internal replication, monitor and document passwords during `init`. It does not interpret a later env file change as a request to update existing accounts. `CREATE USER IF NOT EXISTS` is not a rotation procedure for existing passwords.

Environment variables and mounts are readable by the daemon/host administrator. Container separation reduces exposure to individual processes but is not a defense against infrastructure administrators.

## Permissions, copies and public materials

TLS and secret directories have host permissions `0700`. The CA key has `0600`. Public certificates have `0644`; mounted server keys and secret files are set to `0444`, because Compose preserves host file ownership and permissions and services use different UIDs. Local protection depends on the parent directory as well as host controls; container mounts are read-only.

Do not make files group- or world-writable to resolve access problems: clients reject sensitive non-regular files or overly permissive write access. Instead, check the process user, directories, mounts and filename.

Public certificates, dates and fingerprints can be shown during checks. Private keys, passwords, bearers, refresh tokens and provider file contents must not enter shared consoles, logs, tickets or Git. Backups must retain required materials without placing the AES key together with the only ciphertext copy in an unprotected archive.

## Read-only TLS checks

The following commands read only local public certificates. They do not contact providers or renew anything:

```sh
openssl x509 -in data/network-tls/ca.crt -noout -subject -issuer -dates -fingerprint -sha256
openssl x509 -in data/network-tls/db.crt -noout -subject -issuer -dates -fingerprint -sha256
openssl x509 -in data/network-tls/calendar-addon.crt -noout -ext subjectAltName
openssl x509 -in data/network-tls/calendar-addon.crt -noout -checkend 2592000
openssl verify -CAfile data/network-tls/ca.crt -verify_hostname db-proxy data/network-tls/db.crt
openssl verify -CAfile data/network-tls/ca.crt -verify_hostname mail-api data/network-tls/mail-addon.crt
openssl verify -CAfile data/network-tls/ca.crt -verify_ip 172.30.70.2 data/network-tls/calendar-addon.crt
```

`-checkend 2592000` checks a proposed 30-day operational renewal window. `prepare` instead uses 86400 seconds as its technical rejection threshold. Repeat checks for affected leaf certificates, not just the example. Analyze nonzero outcomes: expiry, CA, identity or files may be incorrect.

These checks do not prove that the process is actually using the new certificate. That requires a real TLS connection from an authorized network, such as the addon health check:

```sh
docker compose --env-file .env.docker exec -T app curl --noproxy '*' --fail --silent --show-error --max-time 3 --cacert /etc/punto/tls/ca.crt https://calendar-api:8091/healthz
```

The call sends no bearer and performs no synchronization. The equivalent mail check uses `https://mail-api:8090/healthz`. Do not use `-k` or disable name verification to consider the test successful. A valid certificate does not verify API token authentication.

For certificate/key matching, `prepare` compares derived public keys without printing private materials. Since `prepare` can also create missing elements, it is not a universally read-only command: run it only when environment preparation is appropriate.

## Manual leaf renewal under the same CA

**Available as manual maintenance; not automated by the script.** Before renewal, identify the service, all required names/IPs, expiry date and return procedure. Retain the previous leaf and its matching key in a protected copy.

1. Generate new materials in a protected staging directory without overwriting the active bundle. Use the correct CA, extensions and SANs consistent with the inventory.
2. Check dates, chain, all required names/IPs and leaf/key matching. For MongoDB update `.crt`, `.key` and `.pem` together; the PEM must have the expected concatenation.
3. Plan consistent certificate and key replacement during maintenance. Do not start the service with half the new materials.
4. Recreate the affected service to reread bind mounts and the TLS context; verify identity through a real connection.
5. Repeat health and application flow checks; retain a record with the public fingerprint and new expiry, without keys.

For SQL nodes under the same CA, a procedure can renew one member at a time while keeping two healthy members and waiting for each to return `ONLINE` before proceeding. It must first be tested in a lab and is not an automatic rolling command in the repository. If quorum decreases, stop the sequence and analyze status.

The app and addons are single instances: restarting them causes a brief outage. Renewal under the same CA does not require changing the Calendar AES key, bearers, email namespace or business data.

## Manual addon API token rotation

**Coordinated maintenance, without simultaneous support for two tokens.** The app and addons each load one bearer. Changing it on only one side causes authentication errors.

1. Define the window, save required materials and stop new addon requests by stopping the app.
2. Allow in-progress operations to finish and stop the affected addon. Account for uncertain remote outcomes as described in the operations guide.
3. Generate a new random token in the project format without printing it. Install it only in `mail-api-token` or `calendar-api-token`, retaining expected directories, ownership and permissions.
4. Recreate addon and app so they read the same new file. Simply updating the file does not reload tokens in memory.
5. Verify both TLS and an authorized authenticated API without exposing the bearer. Check queue resumption with the same idempotency keys.

Do not change `MAIL_API_NAMESPACE`, clear the ledger or Calendar volume, or touch `calendar-token-key`. These are different identities and materials. If rotation responds to compromise, the plan must also assess revocation of the old value and possible access during the period, without using the old token as an automatic fallback.

## Database and provider password rotation

**Must be explicitly planned; no single command is implemented.** SQL password rotation requires updating the actual account and its clients. For the replication account, this also includes Group Replication channels on all members; for the monitor, it includes HAProxy. The recovery password retains the 32-byte limit.

`.env.docker` does not rewrite persisted accounts on every run. Updating it without changing the account leaves new processes unable to authenticate. The procedure must use an order that retains quorum, administrative access and verification ability, tested for that SQL version.

For SMTP, coordinate the provider credential, `smtp-password` file and addon restart; first check pending deliveries, since a newly authenticated service can resume processing. For Google, changing the client secret may require managing consent and already issued tokens: keep client secret, user OAuth tokens and local AES key separate. The repository does not automate revocation or rotation in the Google project.

For NPM, changing `NPM_ADMIN_PASSWORD` in the local file alone does not change the initialized panel password. Punto users currently have no public password-reset endpoint. These updates require a dedicated flow, not database reinitialization.

## Replacing the internal CA

**A future procedure to design and test; not a `prepare` feature.** The CA is a shared trust root for databases, monitor, app and addons. Replacing only `ca.crt` or `ca.key` breaks the existing leaf chains.

The plan must inventory every consumer, produce new leaf certificates with correct identities, preserve data access, distribute trust and account for SQL nodes rejoining. The project does not implement a verified dual-CA transition or online bundle rotation. Any dual-trust period must be validated with the drivers and script, which currently manages a local bundle consistent with one CA.

Maintenance with coordinated shutdown may be simpler, but still requires backups, copies of previous materials, Group Replication recovery tests and updates to every mount. Do not delete the current directory to obtain a random new CA with `prepare`: that loses the reference needed to validate existing materials.

Replacing the private CA does not renew NPM public certificates. They remain under `nginx-proxy-manager/letsencrypt` and are administered by the proxy.

## Calendar AES key rotation

**Not implemented. Do not perform it by replacing the file.** The database stores an encrypted check that rejects an incorrect key; events, requests, OAuth tokens and PKCE verifiers are bound through AAD to the user and row identity.

A future procedure must at least:

1. Stop the API and worker and obtain a consistent copy of the entire store with the previous key.
2. Validate every ciphertext with the old key without exporting plaintext to logs or unprotected files.
3. Re-encrypt all covered categories and the key check with fresh nonces and identical authenticated data, preserving IDs and relationships.
4. Ensure an atomic transition between the re-encrypted store and new secret, with a strategy for midway interruptions.
5. Verify reads, OAuth, queue, idempotency and user isolation on a copy; retain the previous key for historical backups that require it.

There is currently no per-row key ID or mode that automatically accepts multiple keys. Losing the key prevents ciphertext recovery; having only the database or API token is insufficient. A KMS/HSM could improve custody but would require an integration that is not present.

## Maintenance record and final check

Record intervention type, services, public fingerprints, expiry dates, code version, check outcomes and references to protected copies. Do not record secret values. After intervention, check cluster, health, sign-in, addon APIs and queues according to [diagnostics](diagnostica-e-osservabilita.md).

Data copies and keys must follow a consistent policy: deleting an old key before backups that use it can make those backups unusable. Check frequency, expiry alerts and periodic rotations are not scheduled by the project; they must be defined operationally and, if desired, implemented in a future management system.
