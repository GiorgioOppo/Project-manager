# Container migration — 11 October 2026

[Italiano](../../tehnical/migrazione-container-2026-10-11.md) | **English**

[Technical index](README.md) · [Service and image names](nomi-servizi-e-immagini.md) · [Backup and restore](operazioni-backup-ripristino.md)

**Status: migration and validation completed.** The eleven services with new names are `running` and `healthy`; each of the three MySQL members sees three `ONLINE` / `PRIMARY` members and uses schema 8. Existing data, original volumes and cluster identities were retained. API, PDF, network and browser interface checks passed.

This report concerns execution on Docker project **`projectmanament`**. Preparation and build tests described in the [naming guide](nomi-servizi-e-immagini.md#preparation-checks--11-october-2026) precede this maintenance and are not equivalent to deployment validation.

## Scope and initial situation

The initial inventory contained ten legacy project containers, all already stopped (`exited`): `app`, `nginx-proxy-manager`, `squid`, `db-proxy`, `db`, `db2`, `db3`, `documents`, `mail-addon` and `calendar-addon`. Stored health status was `unhealthy`; for stopped containers this is not new evidence of process malfunction.

Ticket was not installed and is not added by migration. `inventory` is a new dependency of current application code and started with new volume `projectmanament_inventory_addon_data`; it does not replace a preexisting inventory store. The total is therefore seven retained original volumes plus one new volume.

The initial operational database used **schema 7 and 22 MySQL tables**. Deployed code requires schema 8 and added `job_tasks` and `job_blockers`. The Compose name change and application migration are therefore two distinct steps in the same maintenance.

| Initial data | Count |
| --- | ---: |
| Users | 15 |
| Projects | 4 |
| Time entries | 0 |
| Suppliers / contracts | 2 / 2 |
| SQL PDF metadata | 4 |
| Groups / memberships | 5 / 2 |
| Audit events | 205 |
| GridFS: files / chunks / PDF keys | 4 / 12 / 4 |

Other tables are inventoried with counts and fingerprints in the private manifest. Operational alert, assignment and rate tables were empty. Mail and Calendar SQLite stores had integrity `ok`; the delivery ledger, Google accounts, events, operations and OAuth states were empty. Calendar retained one metadata row.

## Safety copies

The backup window is identified by `backups/container-names-20261011T054307Z/`. Logical dumps and **eight cold archives** were prepared, totaling **21,022,888 bytes**, about 21.0 MB. The eight archives include the seven existing persistent volumes and operational configuration. The manifest records each archive's size, SHA-256 fingerprint and regular-file count.

| Original volume preserved | Purpose |
| --- | --- |
| `projectmanament_mysql_data` | First MySQL member |
| `projectmanament_mysql_data_2` | Second MySQL member |
| `projectmanament_mysql_data_3` | Third MySQL member |
| `projectmanament_documents_data` | MongoDB data |
| `projectmanament_documents_config` | Persistent MongoDB configuration |
| `projectmanament_mail_addon_data` | Email idempotency ledger |
| `projectmanament_calendar_addon_data` | Calendar store |

The backup directory is private (`0700`), and files have `0600` permissions. Dumps, operational archives, credentials and real configurations are not reproduced in documentation. Archives and their hashes do not constitute a complete restore test in a separate environment.

## Operations already completed

- Stopped and removed the ten selected legacy project containers, **without `-v`**. Retained the seven original volumes and persistent bind mounts.
- Recreated members with services `mysql-1`, `mysql-2`, `mysql-3`, retaining internal identities `db`, `db2`, `db3`.
- Recovered the MySQL group: all three members are `ONLINE` / `PRIMARY`. Comparison confirms original UUIDs and retention of GTIDs on every member.
- Started `mongodb` on existing document stores. The three compared collections are identical to the baseline.
- Retained original credentials, certificates and cluster state. New names did not cause TLS identities to be regenerated.
- Retained actual vendor images: MySQL and MongoDB use the same IDs as the previous installation. Actual started service images match IDs recorded before changing containers.
- Confirmed no residual legacy containers; controller check `require_service_migration_complete` passed.

| Vendor image | Actual retained ID |
| --- | --- |
| `mysql:8.4` | `sha256:6ea90827b1100f8f2ae306a539f86d2c264a26ed435a2a9f75551dd5c3aeb242` |
| `mongo:8.0` | `sha256:d0d926f94df099bff534b7ee5b5986458131a22489dfff8664509af0c1e2ca9c` |

MySQL reports version **8.4.11** in the baseline. Before the new app started, all 22 schema 7 tables were identical to the baseline on each of the three members. This check precedes creation of the two new application tables and their audit record.

## Checks after application startup

Migration brought all three members to **schema 8, with 24 tables**. Comparison with initial state distinguishes expected technical updates from business data:

- **19 original tables identical**, including users, projects, hours, contracts, suppliers, PDF metadata, groups and permissions.
- All **205 original audit rows retained**; one new row brings the total to 206 in the check immediately after migration.
- `schema_metadata` moves from version 7 to 8 and the `cluster_write_guard` counter changes during transactions. These are the other two original tables with expected differences.
- Mail and Calendar: all original tables identical, SQLite integrity check `ok`.
- Nginx Proxy Manager: all original tables identical, SQLite integrity check `ok`.

The audit total refers to comparison after startup: subsequent test sign-ins may add legitimate events and do not change verification of original history preservation.

| Check | Outcome |
| --- | --- |
| Services with new names | 11 out of 11 `running` / `healthy` |
| Inventory | Started, API configured, new dedicated store |
| MySQL schema | Version 8 on all three members |
| Data before app startup | All 22 original tables identical on every node; three MongoDB collections identical |
| Data after app startup | Business data retained; only technical differences described above |
| Application APIs | 34 checks passed, including sign-in/sign-out, management sections, reports, tasks and addon status |
| PDFs | Four downloads verified by hash |
| Network and security | 23 opt-in tests passed in 83.552 seconds, no tests skipped |
| Browser interface | 19 aggregate checks passed; no JavaScript errors or HTTP 5xx |

The `tests/test_network_security.py` suite ran on the stack: it checks isolation, TLS transport, addon authentication and private connections, egress restrictions and proxy policy. The result concerns the configuration actually started, in addition to static preparation tests.

Browser validation covers manager sign-in, nine management tabs, five report views, tasks with five Kanban columns and a list, inventory and sign-out. Expected empty views respond correctly. The responsive inventory check detects no horizontal page scrolling at widths of **1440, 390 and 320 pixels**. No business data mutations were attempted during this verification.

The **19 UI checks** are those aggregated in `ui-smoke.json`: widths and subactions are not extra tests to add. The four PDF downloads are also included in the 34 API checks. These results are not added into one heterogeneous total. Browser verification ended at **05:54:47 UTC** on 11 October 2026, corresponding to 07:54:47 in Europe/Rome.

SMTP and Google OAuth still need configuring; real deliveries or provider synchronization are not considered verified. No new HA failure tests or complete backup restore are claimed.

## Evidence consulted

This report uses only non-sensitive backup-window metadata and operational outcomes communicated during maintenance: `inventory-before.json`, `cold-backup-manifest.json`, `data-before.json`, `mongodb-before.json`, `sqlite-before.json`, `image-ids-before.json`, `data-after-before-app.json`, `data-after-app.json`, `api-smoke.json`, `ui-smoke.json` and `network-verification.json`. Original files remain in the private backup path; the report exposes aggregate results and technical identities useful for verification, without application content or secrets.
