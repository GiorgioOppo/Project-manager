# Asset Inventory: service, API, and operations

[Italiano](../../tehnical/asset-inventory.md) | **English**

[Technical index](README.md) · [Product guide](../prod/asset-inventory.md) · [Addon manual](../../../inventory-addon/README.md)

Inventory is a third independent C addon. The browser uses the main app's session and API; `src/inventory_client.c` forwards requests to the `inventory` service over HTTPS, with a service token and verification of the CA and certificate name. The service uses SQLite in its own volume, with inventory schema version 5. The MySQL/SQLite application schema is at version 10, with return and verification alert types and the corresponding interval.

## Public contract and authorization

| Method and path | Permissions and result |
| --- | --- |
| `GET /api/inventory/status` | Authenticated user: module and service status. |
| `GET /api/inventory/assets/export` | Authenticated user: CSV page with `csv`, `exported`, `next_after_id`; same filters as the catalog. |
| `POST /api/inventory/import/preview` | Manager: validates `csv` and `mode=create_only|upsert`, returns a preview and single-use ID. |
| `POST /api/inventory/import/commit` | Manager: confirms `preview_id` and atomically applies the batch. |
| `GET /api/inventory/due` | Authenticated user: paginated due dates, with `today`, `days_before`, `limit`, `after_id`. |
| `GET /api/inventory/assets` | Authenticated user: catalog page with `items`, `next_after_id`, `summary`. |
| `GET /api/inventory/dashboard` | Authenticated user: counts and distributions with the same catalog filters, without cursors. |
| `GET /api/inventory/operational-due` | Authenticated user: paginated events with `kind=return|verification`, `verification_days`, `today`, `limit`, `after_id`. |
| `GET /api/inventory/assets/<id>` | Authenticated user: record in `asset`. |
| `GET /api/inventory/assets/<id>/history` | Authenticated user: paginated asset events and snapshots. |
| `POST /api/inventory/assets` | Manager: creates a record, `201` response. |
| `POST /api/inventory/assets/<id>` | Manager: edits the record at its current `version`, `200` response. |
| `GET /api/inventory/assets/<id>/relations` | Authenticated user: page of incoming and outgoing relationships. |
| `POST /api/inventory/assets/<id>/relations` | Manager: creates a relationship from the source, returns the relationship and updated record. |
| `POST /api/inventory/assets/<id>/relations/<relation_id>/remove` | Manager: deactivates an outgoing relationship and updates the source version. |
| `GET /api/inventory/users` | Manager: directory of active app users with `id`, `name`, `email`. |
| `GET /api/inventory/places` | Authenticated user: site, department, and location records. |
| `POST /api/inventory/places` | Manager: creates a place record. |
| `POST /api/inventory/places/<id>` | Manager: updates or deactivates a place record at its current version. |
| `POST /api/inventory/assets/<id>/context` | Manager: replaces the asset's structured assignment and placement. |
| `POST /api/inventory/assets/<id>/checkout` | Manager: opens a checkout for an available asset or one already structurally assigned to the same user. |
| `POST /api/inventory/assets/<id>/checkin` | Manager: closes the checkout and makes the asset available or under maintenance. |
| `POST /api/inventory/assets/<id>/verify` | Manager: records a physical verification. |
| `GET /api/inventory/assets/<id>/custody` | Authenticated user: paginated checkout history. |
| `GET /api/inventory/assets/<id>/verifications` | Authenticated user: paginated verification history. |

Writes use `application/x-www-form-urlencoded` and the app's origin checks. The bridge accepts only expected parameters and adds `actor_id` from the authenticated session. It does not let the browser select the author or send arbitrary private paths. Inventory is global: visibility does not depend on project grants.

With the module disabled, `GET /api/inventory/status` returns `200` with `ok: true` and `configured: false`; other operations return `503`. A configured but unreachable service produces an explicit error.

The catalog accepts `q` (at most 200 UTF-8 bytes, including type-specific details), `status`, `asset_type`, `mine=1`, `limit` from 1 to 100 (default 30), and `after_id`; history and relationships accept `limit` and `after_id`. `mine=1` filters the catalog and export for the authenticated user: the bridge derives identity from the session and does not accept an arbitrary user ID from the browser. It is a convenience filter, without restricting global catalog visibility. Ordering is by descending ID, continuing with IDs lower than the cursor. `next_after_id` is null at the end. Pages may contain fewer items than the requested limit to keep responses within 128 KiB; the cursor preserves reading continuity. `summary` contains `total`, `available`, `assigned`, `maintenance`, `retired`, and `lost` for the entire catalog, regardless of filters.

Catalog, export, and dashboard also share `site_id`, `department_id` (positive IDs), `attention=open_custody|overdue_returns|never_verified|verification_due|location_mismatch`, `verification_days` (1–365, default 90), and `today` (optional valid date, otherwise the business day). Search includes structured site, department, and assignee names. Placement filters use structured references; old free text does not automatically become place records.

`dashboard` returns `today`, `verification_days`, `counts: {total,open_custody,overdue_returns,never_verified,verification_due,location_mismatch}`, `by_status`, `by_type`, `by_site`, `by_department`, `by_user`, and `groups_truncated`. Counts cover the entire filtered population, not the first page. Each distribution contains `{id,name,count}`, ordered by descending count, with at most 20 groups and the group without a reference retained when present; its `id` is null. Operational counts consider only tangible assets that are neither retired nor lost. Returns require an open checkout with `due_on < today`; repeat verifications require `last verified_on + verification_days <= today`. Never-verified assets are counted separately, and a location mismatch corresponds to the latest chronological verification with `location_matches=false`. Queries use the verification with the latest date, then the highest ID in a tie.

## People, identities, and linked assets

Inventory schema 5 adds `inventory_people`, `inventory_identities`, `inventory_identity_people`, `inventory_entity_history`, and `inventory_asset_subjects`. Real people and cataloged accounts are independent of app login users; the person–identity relationship is many-to-many without cardinality restrictions by identity type. Credential fields are not accepted by the API. The directories share the catalog's permissions: authenticated reads, Manager writes, and actor identity derived from the session.

Under `/api/inventory`, the bridge exposes `/people`, `/identities`, their `/<id>` details, and `/<id>/history`; GET and POST on a collection mean listing and creation, while POST on a detail performs a full update with its current version. `/people/<id>/identities` and `/identities/<id>/people` list active links. POST on the latter accepts the identity's `version`, `person_id`, `role=owner|user`, and `notes`; POST on `/identities/<id>/people/<link_id>/remove` deactivates a link. Both entities' versions and history are updated atomically.

GET/POST `/assets/<id>/subjects` lists/creates direct links to a person or identity. POST requires the asset's `version`, `subject_kind=person|identity`, `subject_id`, `role=owner|user|administrator`, and `notes`; POST on `/assets/<id>/subjects/<link_id>/remove` soft-removes the link with the asset's version. These writes update only the asset, with an immutable subject snapshot and a history event. GET `/people/<id>/assets` and `/identities/<id>/assets` return reverse links, without propagating membership of shared accounts. Custody, status, and access rights do not change.

Directories, links, and history use descending-ID pagination with `{ok,items,next_after_id}`, `limit=1..100`, and a 128 KiB budget; individual details return `person` or `identity`. Directory filters are `q` and `active`; identities also accept `kind` and `person_id`. Catalog, CSV, and dashboard accept positive `person_id` and `identity_id`, combined with AND over active direct links. Field formats and limits are in the [addon manual](../../../inventory-addon/README.md#persone-e-identità). Deactivation does not delete references or history; new links require active entities, while removal remains possible. `409` indicates duplicates or stale versions.

## Assignments, checkouts, and verifications

`places` records accept `kind=site|department|location`, `name`, `parent_id`, `active`, and, when editing, `version`. A site has no parent; a department may specify a site, while a location must specify one. `kind` cannot be changed, and deletion is not provided. Changing `parent_id` of a place referenced in current contexts returns `409`, avoiding invalidating the site–location/department association; historical references alone do not prevent the change. A record's context contains `assignee_user_id`, `assignee_user_name`, `site_id`, `site_name`, `department_id`, `department_name`, `location_id`, `location_name`; department and location must belong to the selected site. Names are snapshots resolved by the service and bridge, not fields the browser can write freely. The `/api/inventory/users` directory queries the main database and returns only active users to Managers.

`POST /assets/<id>/context` requires `version` and replaces all context references: `assignee_user_id`, `site_id`, `department_id`, `location_id`. It is intended for IT devices, network equipment, and physical assets. Assigning a user sets `assigned`; removing the user returns a previously assigned asset to `available`. Users must be active at the time of a new assignment; later deactivation does not erase names already recorded. Textual assignments in license relationships remain compatible with the previous contract.

Checkout requires `version`, `user_id`, `checkout_on`, `due_on`, `condition_out=good|fair|damaged`, and optional notes; it is permitted for an available asset or one already structurally assigned to the same user, always without an open checkout. Return requires `version`, `returned_on`, `condition_in`, optional notes, and `status=available|maintenance`; it closes the checkout, removes the user, and preserves places. Actual dates cannot exceed the current business-time-zone day; the expected due date cannot precede checkout. An open checkout past `due_on` is overdue without automatically changing status.

Verification requires `version`, `verified_on`, `condition=good|fair|damaged`, optional notes, and optional `observed_location_id`. It records expected and observed context without moving the asset to the observed location; `location_matches` is null if either location is missing. Record responses include `context`, the active checkout in `custody`, and `last_verification`, null where not yet recorded. The `custody` and `verifications` routes expose history by descending ID with `limit` from 1 to 100 and `after_id`. A checkout retains `notes`, `return_notes`, `checkout_on`, `due_on`, `returned_on`, `actor_id`, `returned_actor_id`, and `active`. Historical snapshots use `change.operation=context_updated|checked_out|checked_in|verified`. All these operations increment the version and save the event in the record's transaction. An open checkout cannot be bypassed through the common editor or CSV import.

## Local QR codes and record links

The format is an absolute installation URL, `/app#inventory?asset_id=ID`. After authentication, the interface opens the specified record; the fragment introduces neither an anonymous API route nor authorization. The URL contains only origin, page, and ID. Labels depend on the origin used during generation: if the domain or port changes, they must be reprinted.

`public/qrcodegen.js` is the official [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) library, compiled from TypeScript and distributed locally with its MIT license retained at the top of the file and in `public/qrcodegen.LICENSE.txt`. The file is vendored unchanged and without remote dependencies. `public/inventory-qr.js` builds the SVG using DOM APIs, safe text, at least medium error correction, and a four-module white border. It exposes `InventoryQr.assetUrl`, `createLabel`, `downloadLabel`, `printLabel`.

Download retains the 90 × 50 mm vector label. Printing uses a temporary DOM container and `@media print` rules that hide the rest of the app, with cleanup afterward. It requires neither iframes nor Content Security Policy exceptions and sends no data to external services. `tests/test_inventory_qr.py` checks URLs, text not interpreted as HTML, print area, download, and cleanup; on macOS with Swift, independent Vision decoding checks the QR code actually generated.

Fields, limits, and private `/v1/*` routes are documented in the [addon manual](../../../inventory-addon/README.md). Editing replaces the full record's common fields. Omitting `asset_type` and `details` preserves current values for older clients; sending `details` replaces type-specific details. `asset_tag` uses a SQLite `NOCASE` unique constraint; this comparison does not provide general Unicode normalization. `version` controls concurrency and increases with every change to the record or its outgoing relationships. `409` indicates a duplicate tag, stale version, or relationship/capacity conflicts; `404` a nonexistent resource; `400` invalid parameters. User-readable service errors are in Italian.

## CSV and due-date checks

The corresponding private routes are `/v1/assets/export`, `/v1/import/preview`, `/v1/import/commit`, and `/v1/due`. Preview POST accepts an encoded form up to 1 MiB only on the exact public route; decoded CSV remains limited to 256 KiB and 200 rows. All other inventory forms retain the 32 KiB limit. Responses remain within 128 KiB.

Preview returns `preview_id` (null if it cannot be committed), `can_commit`, `summary: {total,create,update,errors}`, and `items` with row, code, name, type, action, and errors. The normalized plan is persistent, tied to the author, valid for 15 minutes, and single-use; at most 64 open previews per author are permitted. Commit revalidates versions and confirms records and snapshots in one transaction; an error rolls back the whole batch and consumes the preview. Format and formula protection are in the [addon manual](../../../inventory-addon/README.md#csv).

`GET /v1/due` uses the service's local day if `today=YYYY-MM-DD` is absent and defaults to `days_before=30` (1–365). It includes dates <= today + lead time, without a lower bound, for `warranty_until`, `maintenance_due`, `expires_on`, `renewal_on`; it excludes `retired` and `lost`. Each item contains `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind`, `due_on`, `days_remaining`. Ordering is by descending asset ID; `limit` counts assets with due dates, and all due dates for the same asset remain on the same page.

The application's `inventory_due` rule saves `days_before` and uses the existing Europe/Rome scheduler. The client acquires all pages before the SQL transaction, within limits of 100 pages, 10000 events, and 8 seconds; incomplete or invalid data or an unavailable service prevent consuming the run. Unique constraints per rule/recipient deduplicate `inventory_due:<asset_id>:<due_kind>:<due_on>:<days_before>`. Notifications expose `event_kind`, `asset_id`, `due_kind`, `due_on`, `days_before`; `work_date` and `recorded_minutes` are null. For this rule, all selected users receive global inventory alerts.

`GET /v1/operational-due` exposes `return_due` or `verification_due` events according to `kind=return|verification`. In addition to common due-date fields, each event contains `source_id`: the active checkout ID for a return, the latest verification ID for a repeat verification, or zero for a never-verified asset. In the latter case, `due_on` is the asset's creation date. Asset pagination, limits, and complete acquisition before the transaction follow the due-date client's guarantees.

The `inventory_return` and `inventory_verification` rules use the daily scheduler and global catalog; all selected recipients receive alerts. The first reports only overdue returns; the second also includes never-verified assets and saves `verification_days` from 1 to 365, default 90. Deduplication keys are `inventory_return:<asset_id>:<source_id>:<due_on>` and `inventory_verification:<asset_id>:<source_id>:<due_on>:<verification_days>`. A new checkout or verification can therefore generate a new alert, while repeating the same check does not duplicate it. Notifications expose `event_kind`, `asset_id`, `due_kind`, `due_on`, `source_id`, and, for verification, `verification_days`. Application migration 9 → 10 preserves existing rules, recipients, and notifications; it does not create rules automatically or change the meaning of `inventory_due`.

## Types and relationships

`asset_type` accepts `it_device` (default for legacy creation), `network_device`, `physical`, `software`, `license`. The form carries `details` as a string containing a JSON object; responses expose it as an object. Validation depends on type and rejects unknown fields, duplicate keys, incorrect JSON types, and out-of-limit values. IP, MAC, VLAN, quantities, and dates are validated by the service, including private requests without the interface. A license requires a positive reference to a `software` record and `seats_total` from 1 to 1000000. `license_reference` stores an administrative reference, not an activation key.

`installed_on` links software to IT devices or network equipment, with quantity 1 and an optional installed version. `connected_to` links an IT device or network equipment to network equipment different from the source, with quantity 1 and optional ports. `licensed_to` consumes license seats: `per_device` requires a device, `per_user` a textual assignee, `concurrent` and `site` exactly one of the two. Relationships are manual declarations; devices, directories, and license servers are not queried.

Licenses expose `license_usage: {allocated, available, expired}`. `allocated` sums active assignment quantities, and `available` subtracts that total from purchased seats. Assignments beyond capacity or against expired licenses are rejected; removing a relationship frees seats. The service rejects duplicate active relationships and new links involving `retired`/`lost` sources or destinations. It does not permit reducing capacity below allocations, changing type with active relationships, or changing the software/model of an allocated license. Software referenced by a license record must remain software.

Expiration is inclusive: a license with `expires_on` remains valid throughout the specified date and becomes `expired` the following day, calculated in `INVENTORY_BUSINESS_TIMEZONE` (default `Europe/Rome`). An empty expiration imposes no time limit. Evaluation happens on read or on a new assignment request, without scheduled processes; existing assignments remain counted. `renewal_on` and `renewal_mode` are administrative information and do not trigger automatic renewal. Event and record timestamps remain UTC.

Adding and removing require the source's current `version`. The response contains the relationship and source asset at the next revision; the destination retains its revision. The list includes `direction` and abbreviated `source` and `target` records to make both directions viewable. Removal is deactivation: history retains the relationship's identity.

## Persistence and history

The separate `inventory.sqlite3` database stores records, relationships, and `asset_history`. Each asset retains common fields, type and JSON details, last-write author, version, and creation/update dates. Each event retains `action` (`created`, `updated`), `actor_id`, version, date, and the complete JSON snapshot after writing. Relationship events add `change`, with `operation` (`relation_added`, `relation_removed`) and an immutable copy of the relationship. Asset, relationship, version increment, and history are saved in the same transaction: if history cannot be recorded, the entire change rolls back. Records cannot be deleted; decommissioning uses `retired`.

Automatic migration from inventory schema 1 to 2 runs at startup in a transaction, preserving IDs and data. Old records receive type `it_device` and empty details; old snapshots remain unchanged, so history readers must accept snapshots without `asset_type`, `details`, `context`, and `change`. A store with a version above that supported prevents startup. Migration 2 → 3 adds persistent CSV previews. Migration 3 → 4 adds structured contexts, places, checkouts, and verifications without reinterpreting existing textual assignees and locations. Migration 4 → 5 adds people, identities and many-to-many links without changing existing assignments. Previous images cannot open schema 5.

Author identity is an app ID recorded in the independent database; it is not a MySQL foreign key. New assignee users are validated in the main database and transferred as trusted identities with a name snapshot; previous textual assignees and locations remain preserved. Inventory data and snapshots are plaintext in the volume; TLS protects transport.

## Deployment and configuration

| Element | Configuration |
| --- | --- |
| Network | `inventory_api`, `172.30.80.0/24`, `internal: true`, IPv4 gateway `isolated`. |
| App | `172.30.80.10`, new `inventory_api` interface; no HTTP listener on this interface. |
| Addon | `172.30.80.2:8092`, DNS alias `inventory-api`, permitted client `172.30.80.10`. |
| Token | `data/addon-secrets/inventory-api-token`, mounted as `/run/secrets/inventory-api-token` only in app and addon. |
| Addon TLS | Certificate and key `data/network-tls/inventory-addon.crt` and `.key`; shared internal CA. |
| Store | `inventory_addon_data` volume, `/var/lib/punto-inventory/inventory.sqlite3` file. |
| Process | UID/GID `10005:10005`, read-only root, temporary `/tmp`, `cap_drop: ALL`, `no-new-privileges`. |
| Expiration day | `INVENTORY_BUSINESS_TIMEZONE`, default `Europe/Rome`; Docker runtime includes `tzdata` for IANA zones. |

The addon connects only to the inventory network, not to `app_egress` or `backend_app`; it has no host port, Squid access, database credentials, or need for Internet egress. The eight Compose networks retain other addons in their segments. Bind, permitted IP, subnet, and SAN must change together if addresses change; `configure_network.py` includes this subnet in overlap checks.

For an existing, already-configured installation, from the repository root:

```sh
python3 scripts/configure_network.py --env-file .env.docker prepare
docker compose --env-file .env.docker build inventory app
docker compose --env-file .env.docker up -d inventory app
```

`prepare` generates only missing materials, checks existing ones, and preserves the CA, SQL certificates, and existing tokens. Do not print or version secrets. Compose waits for the addon's HTTPS healthcheck before starting the app. The update recreates app and addon; retain volumes. For a cluster not yet initialized, follow the general procedure in the [README](../../../README.md).

In the app: `INVENTORY_API_URL`, `INVENTORY_API_TOKEN_FILE`, `INVENTORY_API_CA_FILE`. In native mode, all absent disables the module; partial configuration is rejected. When enabled, HTTPS, token, and CA remain mandatory. The client accepts `https://inventory-api:8092`, `https://inventory-addon:8092`, and HTTPS loopback URLs with an explicit port; the proxy is disabled for these private calls. The addon's native listener requires `INVENTORY_BIND`, `INVENTORY_ALLOWED_CLIENT`, `INVENTORY_PORT`, `INVENTORY_TLS_CERT_FILE`, `INVENTORY_TLS_KEY_FILE`, `INVENTORY_API_TOKEN_FILE`, and `INVENTORY_DB_PATH` consistent with the environment.

## Backup, recovery, and checks

SQL cluster and MongoDB backups do not include inventory. Compose enables automatic addon backup through `INVENTORY_BACKUP_DIR=/var/backups/punto-inventory`, on the separate `inventory_addon_backups` volume owned by UID/GID `10005:10005`. In native mode, backup remains disabled without an explicit, absolute, real directory not writable by others. `INVENTORY_BACKUP_INTERVAL_HOURS` is 24 (range 1–168); `INVENTORY_BACKUP_KEEP` is 14 (range 1–365).

The first copy is created at startup, subsequent copies at the expected interval. The SQLite Backup API also captures committed WAL writes; each private `0600` temporary copy passes `PRAGMA integrity_check` before `fsync` and atomic rename. Busy attempts have a time limit; errors do not stop the addon and introduce a pause of at least five minutes before retrying. Startup searches for intact copies and recovers persistent `.inventory-backup-status.json` state: restart does not duplicate a still-valid copy. Retention deletes only regular service-owned copies with the expected name and verified integrity; symbolic links and unrelated files are untouched.

`GET /v1/status` exposes `backup.enabled`, `interval_hours`, `keep`, `last_success`, `last_attempt`, `next_due`, `latest_file`, and any `last_error`. Times are UTC, missing values `null`, and the file is a name without a host path. Also save copies off-host; retain token/TLS separately and do not rotate them for an ordinary update.

`scripts/inventory_backup.py` provides `create SORGENTE NUOVO_FILE`, `verify BACKUP`, and `restore BACKUP NUOVO_FILE`. Copies use SQLite Backup and integrity verification; publication rejects any existing destination, including a symbolic link. An example:

```sh
python3 scripts/inventory_backup.py verify /backup/inventory-copia.sqlite3
python3 scripts/inventory_backup.py restore /backup/inventory-copia.sqlite3 /ripristino/inventory.sqlite3
```

Before upgrading to schema 5, create a consistent backup of the previous store. Rollback requires the old image and restoring a compatible backup: do not manually change `PRAGMA user_version` to simulate a downgrade. Records, relationships, previews, checkouts, verifications, people, and identities added after migration require an updated schema 5 copy to be recovered.

To activate the restored file, stop the addon, preserve the old store and any WAL/SHM files, place the new database at the configured path without the old sidecar files, and check UID/GID `10005:10005` ownership and permissions before restarting. Copying only `inventory.sqlite3` during writes can lose data. Then check healthcheck, catalog, and history through the app. A single SQLite instance does not inherit MySQL quorum, replication, or failover.

The native build is `make -C inventory-addon`. `python3 -m unittest discover -s tests -p 'test_inventory*.py' -v` verifies the HTTPS contract with temporary certificates and stores, type-specific details, migration from a real schema 1, legacy compatibility, relationships, allocations, authorization, and transactional rollback. Controller tests verify CA and token preservation. The optional `PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v` suite checks TLS, service authentication, hostname, segmentation, absence of egress, and unrelated credentials. These instructions describe available tests, not certification of an untested live stack.

Sources: `inventory-addon/src/inventory.c`, `inventory-addon/Dockerfile`, `src/inventory_client.c`, `src/server.c`, `compose.yaml`, `scripts/configure_network.py`, `tests/test_network_security.py`.

### CSV, due-date, and backup package verification

Verified in native mode with temporary stores and certificates: CSV import/update, duplicates, stale versions, filtered export, due-date rule, second run without duplicates, and opening the record from a notification. Docker builds of the app (MySQL backend) and addon succeeded. The full suite ran 419 tests: 385 passed, 33 optional tests skipped, and one TCP reset in the oversized-login rejection test; that individual test passed on isolated rerun. Dedicated tests cover atomic import, complete due-date pages, service unavailability, WAL copies, recovery, backup retention, and migrations. After the last fix to software references in CSV licenses, 91 inventory tests passed in a new dedicated run; an additional rollback test for an earlier preview without the verified reference also passed. The active stack was not updated, and migration was not run on the MySQL cluster.

### Dashboard and operational alert verification

The final inventory suite includes 146 passed tests: counts across the entire filtered set, filters shared with CSV, bounded distributions, chronological dates and verifications, new rules, deduplication per record, rollback, and application migrations 8/9 → 10. A cursor different from the page's last asset is rejected before creating notifications, preventing silent gaps in the acquired set. Native and Docker builds of the MySQL app and C addon succeeded; three static i18n checks passed. In the browser, with five fictional assets and temporary stores, site/department filters, card selection, verification interval, distributions, rule creation/execution, notifications, and opening records were checked. Mobile layout at 390 px without horizontal overflow and new English entries were also checked; no console errors or warnings. The active stack and MySQL cluster were not updated.

### People and identities package verification

The updated inventory suite passes 175 tests, including 29 new cases for separate directories, many-to-many relationships, deactivation, duplicates, versions, atomic history, schema 4 → 5 migration and migration rollback, permissions and direct filters. Pages beyond the 128 KiB response budget are traversed without missing or duplicate links. Native builds with `-Werror` and Docker images for the MySQL app and addon succeeded. Three static i18n checks passed; the optional automated i18n browser test was not run.

Browser checks used temporary stores and fictional data: one person with two identities, one shared identity with two people, direct asset links, reverse navigation, catalog/dashboard filters, history and recovery after a duplicate person code. At 390 px there was no horizontal overflow; the console contained no errors or warnings. No deployment to the active stack or migration of production data was performed.
