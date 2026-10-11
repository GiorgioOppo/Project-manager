# Operations, backup and restore

[Italiano](../../tehnical/operazioni-backup-ripristino.md) | **English**

This guide describes procedures available in the repository as of **10 October 2026**. It neither performs nor promises a complete automatic restore: the existing scripts cover SQL group initialization, status, dumps and recovery. See [installation](installazione-configurazione.md), [architecture](architettura.md) and the [main README](../../../README.md) for context.

## Routine checks

From the project root:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker ps
curl --fail http://localhost/healthz
docker compose --env-file .env.docker logs --tail=100 app proxy-db mysql-1 mysql-2 mysql-3
```

Adjust the domain and port if different. `status` reports node, UUID, status, role, read-only state and ID settings. Normal group status is three `ONLINE` / `PRIMARY` members; all Compose services must pass their health checks.

| Check | What it demonstrates | What it does not demonstrate |
| --- | --- | --- |
| App `/healthz` | HTTP server and SQL queries available, `database: mysql` or `sqlite` | PDF or external provider availability |
| MySQL health | SQL process reachable | Quorum membership and multi-primary writability |
| `proxy-db` health | At least two monitor endpoints ready | Absence of future errors or complete data integrity |
| MongoDB health | TLS, application account and document service available | Consistency of every SQL/GridFS reference |
| Squid health | Listener and expected local response | Google or SMTP reachability |
| Addon health | Local HTTPS process available | Valid Google/SMTP credentials and external delivery |
| Addon status | Local configuration present and queue status | Delivery to an email inbox or completion of all remote operations |

Addon and Squid logs use Docker rotation with three 10 MB files. NPM log protection excludes sensitive OAuth information. Do not attach dumps, env files or complete resolved configuration output during diagnosis.

## Shutdown and restart

To stop the stack while retaining volumes:

```sh
docker compose --env-file .env.docker down
```

This command removes project containers and networks but retains named volumes and bind mounts. The `--volumes`/`-v` option would delete volumes: it is not part of a normal restart or update.

After stopping **all three** SQL members, use:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker recover
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker ps
```

If an active group already exists, `recover` rejoins offline nodes without creating a competing group. If no node is online, it stops group channels, compares executed and certified GTIDs and selects a member whose set contains those of the others. It does not automatically select the first node (`mysql-1`, internal identity `db`). If the sets diverge, it stops without forcing quorum or deleting data.

`recover` recovers **existing volumes**. It does not read an SQL dump, recreate PDFs from backup, restore secrets or resolve total disk loss. `data/cluster-state.json` must match the existing cluster; it must not be manually recreated with invented UUIDs or markers.

## Single-member failure

With two members online the cluster can continue serving the app. A restarted node attempts to rejoin and perform distributed recovery. For an ordinary rejoin of a stopped service:

```sh
docker compose --env-file .env.docker start mysql-2
python3 scripts/manage_cluster.py --env-file .env.docker status
```

Check its return to `ONLINE`, the other members and app availability. Do not delete the node volume as a first attempt. If rejoining fails, check TLS, configuration, replication accounts and GTIDs before designing a new seed.

With two nodes unavailable, write unavailability is the expected protective behavior. Do not force the isolated member to become writable: restore quorum or follow a verified recovery procedure. Failure tests in the HA suite are described in [development and tests](sviluppo-test.md) and cause intentional outages.

## Required backup inventory

| Item | Content | Restore dependencies |
| --- | --- | --- |
| MySQL dump | Application database, schema, rows, routines, triggers and events included by the dump | Application/schema version and rebuilt or retained credentials |
| MongoDB archive | Document database, GridFS collections and metadata | SQL document metadata from the same logical point |
| `calendar_addon_data` | Calendar SQLite and its journals/WAL | `calendar-token-key`, OAuth configuration and SQL user identities |
| `mail_addon_data` | SQLite with SMTP idempotency history | SQL outbox and stable `MAIL_API_NAMESPACE` |
| `inventory_addon_data` | SQLite with asset catalog and revisions | Consistent inventory database copy and its WAL files |
| NPM `data` and `letsencrypt` directories | Proxy configuration, administration database, public certificates | Same proxy version/configuration and associated files |
| `data/network-tls` | CA, CA key, server certificates and keys | Deployment SANs, names, IPs and mounts |
| `data/addon-secrets` | API tokens, Calendar key and provider secrets | Corresponding addon databases and configuration |
| `.env.docker` | Accounts, passwords and environment configuration | Actual identities of persisted accounts |
| `data/cluster-state.json` | Local markers and cluster UUIDs | Useful for recovering the same volumes; not a generic seed |
| Code/configuration and manifest | Version, schema, images, volume names, time and archive hashes | Copy reproducibility and verification |

MySQL replication retains three copies of the same state, including mistakes or deletions; it is not recoverable history. SQL dumps and gzip archives are not encrypted by the scripts. Keep copies outside the single host, protect access and transfers, and separate key custody from encrypted data. Frequency, retention, RPO and RTO policies are not automated in the repository.

## Available dumps

```sh
python3 scripts/manage_cluster.py --env-file .env.docker backup
python3 scripts/backup_documents.py --env-file .env.docker
```

The first command selects an online member when available and writes `backups/mysql-<UTC>-<suffisso>.sql`, with `0600` permissions. It uses `mysqldump --single-transaction --routines --triggers --events --hex-blob --set-gtid-purged=OFF --databases ...`. An incomplete file is removed. The dump includes the application database, not global MySQL accounts, complete cluster configuration or a checkpoint ready for GTID recovery.

The second uses authenticated `mongodump` with TLS for the document database only, writing `backups/documents-<UTC>-<suffisso>.archive.gz`, with `0700` directories and `0600` files. It does not pass the password in a URI or visible command arguments; it uses a temporary file inside the container. It checks the gzip header and removes incomplete copies. By itself, it does not prove that all PDFs can be restored.

Neither command automatically stops the app for a coordinated backup; a standalone SQL backup can be internally consistent through its transaction, but it is not atomic with MongoDB or addons.

## Consistent backup window for the entire stack

Assuming no other clients write directly to the databases:

1. Announce a maintenance window and prevent new changes or deliveries.
2. Stop the app, allowing work in progress to finish; then stop addons and NPM. Wait for them to actually exit. A request already sent to Google or SMTP may have an uncertain remote outcome even after shutdown.
3. With SQL and MongoDB still available, create both dumps.
4. With addons stopped, archive the full contents of their volumes, including any `-wal` and `-shm` files; with NPM stopped, archive both its directories. Do not copy only the `.sqlite3` file while the service is writing.
5. Save operational materials and keys with separate protection, recording which archives belong to the same window.
6. Check sizes, hashes, readability, permissions and command results; retain the manifest without passwords or tokens.
7. Restart addons and the app first, then NPM, and check health and queue status.

The intended shutdown and dump commands are:

```sh
docker compose --env-file .env.docker stop app
docker compose --env-file .env.docker stop mail calendar inventory proxy-web
python3 scripts/manage_cluster.py --env-file .env.docker backup
python3 scripts/backup_documents.py --env-file .env.docker
```

**Before restarting, complete the volume, directory and key copies in steps 4–6.** The repository does not include a single exporter for these items. Use a daemon/host backup system that mounts volumes read-only, preserves ownership and permissions and does not alter originals. Alternatively, for SQLite, an operation through the SQLite Backup API is valid if implemented and verified using the environment's tools. A live copy of just the database is not equivalent.

After verified completion of the copies:

```sh
docker compose --env-file .env.docker up -d --no-deps --wait mail calendar inventory app
docker compose --env-file .env.docker up -d --no-deps --wait proxy-web
```

If a copy fails, retain original data and clearly distinguish a partial copy from a complete restore point. An error does not authorize deleting or regenerating volumes or keys. Backup space must also accommodate temporary archives.

## Restore from backup: controlled procedure

There is no `restore` script in the project. Restore must first be tested in a separate environment with new volumes and real SMTP deliveries and Google operations disabled. Do not directly import a dump three times into the active multi-primary group.

1. Identify a consistent archive set, its code/schema version, keys and configuration. Check the recorded hashes; retain an intact copy of the damaged environment before further intervention.
2. Prepare an isolated SQL instance and restore the application dump. Global accounts and credentials must be rebuilt consistently; the application dump does not contain them. Validate constraints, counts and data before connecting the app.
3. Design seeding for the other members with the correct GTID checkpoint. `init` rejects sources with non-empty GTID history and incompatible markers: do not bypass this protection by clearing GTIDs or `cluster-state.json`. A logical restore and group rebuild require an explicit, verified SQL procedure for that situation.
4. Restore the document archive into the corresponding isolated MongoDB database. Check collections, multi-chunk documents and SHA-256 hashes against SQL metadata. An unreferenced object must be evaluated, not automatically deleted.
5. Restore the entire Calendar directory with the **same AES key**, appropriate secrets and corresponding SQL user IDs. Do not start real synchronization while validating the copy. An incorrect key must prevent opening, not lead to database deletion.
6. Restore the email ledger, SQL outbox and namespace together. Examine `unknown` and `failed` statuses and deliveries already accepted by the provider. Restoring a backup can roll back the local database but does not undo an email already sent.
7. Restore NPM and TLS, checking names, SANs, expiry dates, permissions, proxy destinations and OAuth callback protection. Materials must not enter the repository.
8. Check sign-in, roles, projects, hours, reports, PDF downloads, queues and group status. Only after validation, prepare to switch ingress to the restored environment and deliberately re-enable external integrations.

A successful restore drill must record actual timings, limitations encountered and data verified. Historical suite results alone do not prove that a new backup copy can be restored.

## Queue and integration maintenance

For email, `sent` means SMTP acceptance, not delivery to an inbox. `delivery_unknown` and terminal outcomes require provider verification. Do not change the namespace or clear the ledger to force resending: this could duplicate messages.

Calendar retains `queued`, `unknown`, `applied`, `conflict` and `failed` operations. An ETag conflict must be resolved by retrieving the remote version and making an explicit content decision. Disconnecting deletes the user's local data and attempts consent revocation; it does not undo events already created or a remote request in progress. Details are in the [Calendar manual](../../../calendar-addon/README.md).

APIs and status checks are documented in [addon APIs](api-addon.md). Do not manually clean stores to remove synchronization errors before understanding their remote outcome.

## Diagnosing common errors

| Symptom | Initial checks |
| --- | --- |
| App `503`, SQL nodes started | Group Replication status, quorum, read-only state, HAProxy health monitor; a healthy SQL process is insufficient |
| Internal TLS error | System time, expiry, mounted CA, SANs, destination name and key/certificate match |
| PDF unavailable | MongoDB health, document account, SQL metadata, GridFS object and hash; do not regenerate missing bytes |
| Healthy addon but inactive integration | `smtp_configured` / `oauth_configured`; provider configuration is still deferred |
| Squid `403` | Source IP, CONNECT method, exact host, port and public DNS response |
| New allowlist not read | An editor may have replaced the bind mount inode; check the file in the container and recreate Squid after a valid build |
| NPM rejects a custom configuration | Check OAuth privacy filter restrictions and locations/ACLs; do not disable the filter |
| Sign-in lost after an update | In-memory sessions: sign in again |
| `recover` rejects divergent GTIDs | Retain volumes and analyze sets; do not force bootstrap or quorum |
| Calendar rejects the key | Restore the correct secret for that volume; do not remove the cryptographic check |

TLS leaf renewal is manual and must be scheduled before expiry. `prepare` verifies the bundle but does not rotate it automatically: the procedure is described in [installation and configuration](installazione-configurazione.md).
