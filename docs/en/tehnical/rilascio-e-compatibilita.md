# Release and compatibility

[Italiano](../../tehnical/rilascio-e-compatibilita.md) | **English**

Guide to local release of the stack present on **11 October 2026**. It distinguishes building, process updates, data migration and restore. This is an operational procedure to run when deploying a change; writing the manual did not rebuild or restart services.

Sources: [Makefile](../../../Makefile), [app Dockerfile](../../../Dockerfile), [Compose](../../../compose.yaml), [migrations](../../../src/storage.c), [MySQL schema](../../../src/mysql_schema.c), [cluster controller](../../../scripts/manage_cluster.py), [network controller](../../../scripts/configure_network.py), [email addon](../../../addon/README.md) and [Calendar addon](../../../calendar-addon/README.md).

## Units that may change

A release must state which elements it changes. The application image includes both the C binary and `public/` files; frontend updates therefore require a new image in Docker deployments. Squid configurations and certificates are also bind mounts: rebuilding the image alone does not guarantee that every mounted file has the expected content.

| Item | Current version/configuration | Compatibility relationship |
| --- | --- | --- |
| C app | C11 build with warnings as errors | Maximum application schema 14; API and frontend distributed together |
| MySQL backend | `mysql:8.4` | Three members with consistent Group Replication configuration |
| SQL application schema | 14 | Same logical version for MySQL and SQLite, with specific DDL and migrations |
| SQL proxy | `haproxy:3.2-alpine` and Python monitor | Checks multi-primary mode, quorum and TLS on three nodes |
| MongoDB | `mongo:8.0`, GridFS `contract_pdfs` | PDF data linked to the SQL manifest, without a distributed transaction |
| NPM | `jc21/nginx-proxy-manager:2.16.0` base with local patches | Administration entrypoint and configuration generation checked by the build |
| Squid | Debian Trixie package in the project image | ACLs, IPs and allowlists must match Compose networks |
| Email addon | Separate C binary; SQLite `deliveries` ledger | HTTPS `/v1` contract, outbox states introduced by schema 7 and retained in schema 14, and stable namespace |
| Calendar addon | Separate C binary; SQLite `user_version=2` | HTTPS `/v1` contract, AES key and persistent user identifiers |
| Inventory addon | Separate C binary; SQLite store version 5 | HTTPS `/v1` contract, persistent records and revisions |
| Networks | Eight bridges, explicit IPs and interfaces | Docker Engine ≥28, Compose ≥2.36.0 and consistent SANs |
| Private TLS | Local CA and per-service leaf certificates | Verified names/IPs/CA; files expected by mounts |

Local images follow `punto/<servizio>:${PUNTO_IMAGE_TAG:-local}` and `pull_policy: build`; they are not pulled from the identically named registry namespace. The [naming guide](nomi-servizi-e-immagini.md) describes the Compose identity change, separate from SQL migration.

MySQL, MongoDB, HAProxy and Debian tags are not all pinned to immutable digests. A later rebuild may use different packages or images. The project has no complete automatic manifest of versions actually downloaded: record their identifiers before and after a release when reproducibility is needed.

There is no general matrix promising compatibility with every future minor release. In particular, changing NPM requires reviewing patches; changing MySQL requires driver, schema, replication and recovery checks; changing libcurl/OpenSSL affects both addons.

## Minimum release record

Prepare a record without secrets containing:

- Git reference and presence of uncommitted changes;
- list of modified services, configuration files and API contracts;
- initial and expected schema versions;
- source and target image IDs, when available;
- tests actually run, skipped tests and their reasons;
- consistent backup point and materials needed to recover it;
- expected downtime window and session impact;
- success criterion, stopping criterion and possible return path.

Useful read-only commands to prepare the record:

```sh
git status --short
git rev-parse HEAD
git diff --check
docker compose --env-file .env.docker config --quiet
docker compose --env-file .env.docker images
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
```

`config --quiet` checks configuration without printing values. Do not attach complete `config`, `inspect` or `env` output if it may contain passwords. `git diff --check` checks formatting in tracked diffs; it does not automatically review new unadded files, sensitive content or application correctness.

## Preparation and tests before deployment

The [development and tests guide](sviluppo-test.md) explains suite prerequisites and effects. The release must select checks relevant to the change.

| Change | Checks to consider |
| --- | --- |
| Business rule or API | Build, module suite, roles, errors and data preservation |
| SQL migration | Upgrade from the previous version on a copy, final schema and audit, relevant SQLite suite and MySQL integration |
| Frontend | Real flow, manager/operator profiles, mobile, desktop, keyboard and API errors |
| Email | TLS SMTP fixture, ledger, outbox, uncertain outcomes and revocations |
| Calendar | OAuth/Google fixtures, isolation, encryption, ETags, idempotency and worker |
| NPM | Privacy transformation test and isolated Nginx test |
| Squid/networks/TLS | Build/configuration, isolated policy tests and live matrix checks |
| Group Replication | Controller, driver and HA checks in an environment with authorized outages |

Tests with simulated providers do not require activating real providers. SMTP and Google are deferred in the current deployment: a code release must not implicitly enable them merely because services become healthy.

For an ordinary C change, a build and test sequence is:

```sh
make
make test
docker compose --env-file .env.docker build app
```

The Docker target also checks the MySQL backend and image dependencies. The native suite does not replace image building, and image building does not automatically run all live stack tests.

For addons, build the components actually changed and run their suites. A documentation-only change does not require starting providers, running migrations or simulating container failures.

## Backup before deployment

An SQL dump is sufficient only for an intervention whose return plan does not involve other stores. If documents, addons, NPM configurations or keys change, prepare a consistent point according to [backup and restore](operazioni-backup-ripristino.md).

Save the email ledger and outbox together, retaining `MAIL_API_NAMESPACE`. Save the Calendar volume and its AES key without replacing it. SQL and MongoDB databases do not share a snapshot: suspend writes during a coordinated copy. Do not treat the three MySQL volumes as three backups of different versions.

Also retain the previous image or a reproducible reference: `docker compose build` updates the local build tag and is not an automatic rollback strategy. Document where the version to restore is located before overwriting the only useful reference.

## Targeted app deployment

On an already running stack with healthy dependencies, after checks and the appropriate backup:

```sh
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait app
```

The command uses the already built image. It does not recreate SQL or MongoDB; it interrupts app connections and loses in-memory sessions. Required migrations run in the new process during startup, before normal service becomes available.

If NPM also changes, build it first and explicitly recreate it after the app. For a larger service set, use only those actually modified:

```sh
docker compose --env-file .env.docker build proxy-egress mail calendar inventory app proxy-web
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait proxy-egress mail calendar inventory app proxy-web
```

This second sequence is for a joint release of the listed components; it is not required for every change. `--no-deps` requires excluded dependencies to be available already. Missing keys and certificates must be prepared before containers using the documented procedure, not by creating empty files to satisfy mounts.

## Migrations: applied path and limits

The app rejects databases declaring a version higher than supported. For SQLite it reads `PRAGMA user_version`; for MySQL it checks `schema_metadata`. Current code upgrades data to version 14; the actually deployed version must be read in the target environment. Calendar separately manages its version 2 and rejects higher versions; the email ledger does not manage `user_version` numbering like Calendar.

The current inventory release uses application schema 14: step 9 (`inventory_due`, `days_before`) is followed by step 10 (`inventory_return`, `inventory_verification`, `verification_days` 1–365, default 90), with no new app tables and preserving IDs, foreign keys, recipients, and history. The independent inventory schema is 5: step 3 → 4 introduced context, custody, and verifications, retaining the CSV previews from version 3; step 4 → 5 adds separate people and identity directories, many-to-many memberships, directory history, and direct asset links without changing existing assignments. It requires the app, inventory service and interface to be updated together. Retain backups of both stores; native verification does not certify MySQL cluster migration.

The tasks/blockers release moves from schema 7 to 8, adding two tables without transforming existing projects or hours. It requires the new binary together with `app.html`, `app.js` and `work-board.css`; addons retain their formats. Before deployment, check upgrades on a copy, Manager/Contributor/Viewer access, stale revisions, hierarchies and deletions. SQLite verification alone does not prove DDL or transactions on the MySQL cluster. See the [module specification](attivita-kanban-e-blocchi.md).

Application migrations include new tables, columns, constraints, triggers, initial data and auditing. Copying DDL from documentation is not enough to obtain the same update. MySQL DDL operations and controller steps are not one transaction that can automatically undo any partial update.

If startup is interrupted:

1. Retain sanitized logs, observed schema and volumes.
2. Read actual status and persisted version, distinguishing connection errors, incompatible versions and migration errors.
3. Determine whether current code can safely complete the intended transition; do not automatically retry by deleting tables or metadata.
4. If an earlier copy must be recovered, follow the restore plan and validate all affected stores together.

`manage_cluster.py init` prepares or recovers the group but is not a command for reverting to an earlier schema. `recover` reconstitutes the group using existing volumes; it does not import the release backup.

## Post-release checks

```sh
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
curl --fail --max-time 5 http://localhost/healthz
docker compose --env-file .env.docker logs --since=10m --tail=100 app proxy-db
```

Then complete specific checks: new sign-in, changed feature, role isolation, readable previous data, any downloadable PDFs with verified integrity, addon status and queues. Status APIs can be checked without triggering deliveries or changes to real calendars.

Success criteria for local deployment: affected services healthy, three SQL members `ONLINE`/`PRIMARY` when all should be available, expected schema, working NPM→app path and no observed regression in selected checks. Document any limitations, such as unconfigured providers or live tests not run.

`--wait` is not an end-to-end product test. Even a positive app health check does not verify MongoDB, SMTP or Google. Use [diagnostics](diagnostica-e-osservabilita.md) if a check fails.

## Returning to the previous version

| Situation | Possible path | Essential condition |
| --- | --- | --- |
| Code/frontend only, unchanged schema and formats | Redeploy a compatible previous image | Verify beforehand that it reads data and responses produced by the new version |
| New schema unsupported by the old binary | Complete/fix the release or restore a consistent copy | Do not manually lower the version number to bypass the check |
| Addon format change | Coordinated restore of binary, store and required secrets | Retain ledger/idempotency and correct keys |
| Network or CA change | Restore configuration and trust for all peers | IPs, binds, SANs, allowlists and mounts must become consistent again |
| Volume loss | Restore from a verified backup | `recover` alone cannot recreate lost data |

Code rollback does not undo data already changed, notifications issued, emails accepted by SMTP or events created in Google. Restoring an old database may make completed operations reappear: ledger, namespace and uncertain outcome handling must be evaluated before re-enabling providers.

Automatic down-migrations, blue/green app replacement, multi-host rollout and automatic rollback on failed health checks are not implemented. These are possible future developments, not properties of current Compose.

## Broader infrastructure changes

Upgrading an SQL major version, moving members to different hosts, changing subnets or replacing the CA requires a specific plan. The local controller assumes project names, addresses, group and markers; it must not be reused as a generic orchestrator for a new environment without review.

Even with three SQL members, the app, proxies and addon stores remain single instances. Host maintenance or a daemon update may interrupt the entire stack. Do not claim a release has no downtime based only on multi-primary mode.

## Historical evidence and new checks

The [test guide](sviluppo-test.md) reports results recorded on 10 October 2026: full suite of 270 cases, 241 passed and 29 optional cases skipped; specific Calendar and NPM privacy checks and 20 network tests passed in that validation. They are evidence for the version verified then, not certification of subsequent changes.

Every new release must report its own observed outcomes and distinguish tests run from proposed or unavailable checks. Do not update historical figures based solely on the number of methods in test files.
