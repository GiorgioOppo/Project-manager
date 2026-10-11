# Asset Inventory addon

**English** | [Italiano](README.it.md)

The Compose service is named `inventory`, and its local image is `punto/inventory:${PUNTO_IMAGE_TAG:-local}`. To update an installation using the previous names, follow the [Docker naming guide](../docs/en/tehnical/nomi-servizi-e-immagini.md); directories, volumes, and TLS identities retain their established names.

`punto-inventory` is an independent C application that stores the asset catalog, type-specific details, relationships, and change history in SQLite. It manages IT devices, network equipment, physical assets, software, and licenses. The main app authenticates users, allows all authenticated users to read, and reserves creation and updates for Managers. Registration, installations, links, and license assignments are manual: the service does not perform scans or automatic discovery.

## Private API

The HTTPS listener is `https://inventory-api:8092`, on the internal `inventory_api` network only. The allowed source is the app at `172.30.80.10`; the addon listens on `172.30.80.2`. Requests to `/v1/*` require `Authorization: Bearer <dedicated token>`. `GET /healthz` checks the service without a token, while remaining restricted to allowed sources or the container itself. The browser uses the app's `/api/inventory/*` bridge.

| Private method and path | Purpose |
| --- | --- |
| `GET /healthz` | HTTPS health check. |
| `GET /v1/status` | Service status (`ok`, `configured`) and catalog `summary`. |
| `GET /v1/dashboard` | Counts and distributions for the filtered catalog. |
| `GET /v1/operational-due` | Paginated events for overdue returns and checks due for repetition. |
| `GET /v1/assets` | Catalog with search, status, type, and pagination. |
| `GET /v1/assets/<id>` | Asset details. |
| `GET /v1/assets/<id>/history` | Asset history with snapshots. |
| `POST /v1/assets` | Creates an asset. |
| `POST /v1/assets/<id>` | Updates an asset at the specified version. |
| `GET /v1/assets/<id>/relations` | Paginated incoming and outgoing relationships. |
| `POST /v1/assets/<id>/relations` | Adds a relationship from the specified record. |
| `POST /v1/assets/<id>/relations/<relation_id>/remove` | Deactivates an outgoing relationship without deleting its history. |
| `GET /v1/places` | Lists sites, departments, and locations. |
| `POST /v1/places` | Creates a place record. |
| `POST /v1/places/<id>` | Updates or deactivates a record at the specified version. |
| `POST /v1/assets/<id>/context` | Replaces the structured assignment and placement. |
| `POST /v1/assets/<id>/checkout` | Records a handover to a person or a legacy Punto user. |
| `POST /v1/assets/<id>/checkin` | Records the return for the open handover. |
| `POST /v1/assets/<id>/verify` | Records a physical check. |
| `GET /v1/assets/<id>/custody` | Lists paginated handover history. |
| `GET /v1/assets/<id>/verifications` | Lists paginated check history. |

POST requests use `application/x-www-form-urlencoded`. Unknown, repeated, or invalid parameters are rejected. The app adds `actor_id`, deriving it from the session: the browser cannot choose the actor. This identity is a trusted assertion by the app, not an account authenticated separately by the addon.

| Field | Rule |
| --- | --- |
| `asset_tag` | Required, maximum 64 UTF-8 bytes, unique using SQLite `NOCASE` comparison. |
| `name` | Required, maximum 160 bytes. |
| `category` | Maximum 80 bytes, defaults to `hardware`. |
| `serial_number`, `manufacturer`, `model` | Optional, maximum 120 bytes each. |
| `status` | `available`, `assigned`, `maintenance`, `retired`, or `lost`; defaults to `available`. |
| `location` | Free text, maximum 160 bytes. |
| `assignee` | Free text, maximum 160 bytes; required with `assigned` status. |
| `purchase_date`, `warranty_until` | Optional, valid `YYYY-MM-DD` dates; warranty cannot precede purchase. |
| `notes` | Optional, maximum 4000 bytes. |
| `asset_type` | `it_device`, `network_device`, `physical`, `software`, or `license`; defaults to `it_device` on creation. |
| `details` | JSON object serialized in the form, maximum 4096 bytes, containing only fields for the chosen type; unknown or repeated keys are not accepted. |
| `version` | Required for updates, a positive integer returned by the current details. |
| `actor_id` | Positive user ID, added by the bridge for writes. |

An update sends the common fields of the complete record, including those to clear; it is not a PATCH. For compatibility with older clients, `asset_type` and `details` omitted from an update retain their current values. If `details` is sent, it replaces the type-specific details. An existing `asset_tag` or an outdated version returns `409`. Error messages are in Italian. To retire an asset, use `status=retired`: deletion is not available.

### Type-specific details

Text limits are in UTF-8 bytes. Fields are optional except those indicated for licenses; dates use `YYYY-MM-DD`. VLAN, port count, references, and quantities are JSON integers, not strings or booleans.

| Type | `details` fields |
| --- | --- |
| `it_device` | `hostname` (253), `ip_address` (valid IPv4 or IPv6), `mac_address` (six hexadecimal pairs separated by `:`), `operating_system` (120). |
| `network_device` | The same fields as IT devices, plus `network_role` (80), `vlan` (1–4094), `port_count` (0–65535). |
| `physical` | `asset_kind` (80), `department` (120), `maintenance_due` (date). |
| `software` | `publisher` (120), `software_version` (80), `edition` (80), `software_kind` (80). |
| `license` | Required `software_id` (ID of a software record), required `seats_total` (1–1000000), `license_model` (`per_device`, `per_user`, `concurrent`, `site`; defaults to `per_device`), `expires_on` and `renewal_on` (dates), `renewal_mode` (`manual`, `automatic`, `none`; defaults to `manual`), `license_reference` (120). |

`license_reference` is an administrative reference, for example an order or contract; it is not a field for storing activation keys. The model and renewal are declared information: the service does not renew subscriptions or query licensing systems. License records expose `license_usage` with `allocated`, `available`, and `expired`; availability is calculated from recorded active assignments, not from detected program usage.

A license remains valid throughout the day specified in `expires_on`: `expired` becomes true when that date precedes the current day in the `INVENTORY_BUSINESS_TIMEZONE` time zone, which defaults to `Europe/Rome`. Without an expiration date, `expired` is false. Events and creation/update dates continue to use UTC timestamps. `renewal_on` and `renewal_mode` describe renewal without performing it.

`GET /v1/assets` accepts `q` (maximum 200 UTF-8 bytes, also searches details), `status`, `asset_type`, `limit` from 1 to 100 (default 30), and `after_id`. The list is sorted by descending ID; `after_id` continues with IDs below the cursor. The response contains `items`, `next_after_id`, and `summary`, with the total and counts by status for the entire catalog, independently of filters. Pages may contain fewer items than the requested limit to respect the 128 KiB response budget; the cursor still allows continuation. The details response returns `asset`. History accepts `limit` (default 30, maximum 100) and `after_id`, uses the same descending order, and returns `items` and `next_after_id`: each event contains `id`, `action` (`created` or `updated`), `actor_id`, `version`, `created_at`, and a `snapshot` of the record after the write.

### Filters and operational dashboard

The catalog and export also accept `assignee_user_id`, `site_id`, and `department_id` as positive IDs, `attention=open_custody|overdue_returns|never_verified|verification_due|location_mismatch`, `verification_days` from 1 to 365 (default 90), and optional `today=YYYY-MM-DD`. The public bridge allows only `mine=1` for the assignee and derives the user ID from the session. Site and department references filter structured contexts; text search also includes their names and the assignee's name.

`GET /v1/dashboard` uses the same filters, without `limit` and `after_id`, and returns `{ok,today,verification_days,counts,by_status,by_type,by_site,by_department,by_user,groups_truncated}`. `counts` contains `total`, `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, and `location_mismatch`. Totals cover the entire filtered population. The three distributions contain `{id,name,count}`, sorted by descending count, with at most 20 groups each; the group without a reference retains `id=null` and is included when present, even if it replaces the last group. `groups_truncated` indicates limited distributions. The catalog's existing `summary` retains global counts independently of filters.

Operational counts consider only `it_device`, `network_device`, and `physical`, and exclude `retired`/`lost`. An open handover is overdue when `due_on < today`; a check is due for repetition when its date plus the interval is `<= today`. Assets without checks have a separate count. The latest check is the one with the greatest `verified_on`, then the greatest ID for the same date; a location discrepancy in that check contributes to `location_mismatch`. These reads do not update asset status, version, or history.

### Relationships and license assignments

Creating a relationship requires `relation_type`, the source record's current `version`, and `actor_id`. It also accepts `target_id`, `person_id`, `identity_id` (positive IDs), `assignee` (160 bytes), `quantity` (integer 1–1000000, default 1), `installed_version` (80), `source_port` and `target_port` (80 each), and `notes` (2000), according to the relationship's rules. `person_id` and `identity_id` are allowed only for `licensed_to`.

| Relationship | Source and destination |
| --- | --- |
| `installed_on` | Software → IT device or network equipment. Quantity 1, no assignee or port; the installed version can be specified. |
| `connected_to` | IT device or network equipment → network equipment other than the source. Quantity 1; allows both ports. |
| `licensed_to` | `per_device`: IT device or network equipment (`target_id`). `per_user`: person (`person_id`), identity (`identity_id`), or legacy text assignee (`assignee`). `concurrent` and `site`: also allow a device. Exactly one recipient is required. |

New structured recipients must exist and be active. The relationship retains `person_id` or `identity_id`, `subject_kind`, `subject_id`, and `subject_snapshot`, a complete and immutable copy of the directory record resolved by the service. `assignee` retains the person's name or the identity's display name, falling back to the login; it is not accepted from the caller together with a structured recipient. For legacy relationships, these new fields are null and the previous text remains unchanged. Renaming, deactivating, or changing an identity's members does not rewrite existing assignments. Structured duplicates are checked by recipient ID, allowing distinct people or identities with the same name.

Each active relationship consumes only `quantity` seats: an identity shared by three people with `quantity=1` consumes one seat, without multiplying by the number of members. This administrative count does not verify the supplier's terms. Assignment does not create login users, person-identity associations, custody contexts, or implicit `inventory_asset_subjects` links.

Duplicate active relationships are rejected. Links cannot be created with retired or lost records, nor can new assignments be made against an expired license. The assigned total cannot exceed `seats_total`, and capacity cannot be reduced below the amount already assigned. A type cannot change while the record has active relationships, and software referenced by licenses must remain software. A license's model and software cannot change while active assignments exist.

Removal requires the source's current `version` and `actor_id`, deactivates the relationship, and releases license seats. Addition and removal return the updated `asset` and `relation`, incrementing only the source's version. History retains `action=updated` and adds `change` with `operation=relation_added` or `relation_removed` and an immutable copy of the relationship. Old events are not rewritten.

The list contains active relationships and accepts `limit` and `after_id`, with descending ID order and a 128 KiB budget like the other lists. Each item indicates `direction` (`incoming` or `outgoing`) and the `source` and `target` record summaries; a person/identity recipient uses the references and snapshot described above, with `target=null`. Removed relationships remain viewable through history, even after the recipient is deactivated.

The asset, any relationship, and its history event are saved in the same SQLite transaction. A write is not committed without its snapshot. Versions prevent overwriting a concurrent update; after a conflict, the caller must reread the data.

## People and identities

Inventory schema 5 distinguishes `inventory_people` (real people), `inventory_identities` (registered accounts), and authenticated Punto users. It does not create application accounts or store passwords, tokens, or keys. `inventory_identity_people` represents a many-to-many relationship: a person can use several identities, and an identity can be shared by several people, even if classified as `personal`. The type is descriptive.

| `/v1` paths | Operations |
| --- | --- |
| `/people`, `/identities` | GET list, POST creation. |
| `/people/<id>`, `/identities/<id>` | GET details, POST complete update with `version`. |
| `/people/<id>/history`, `/identities/<id>/history` | GET immutable history with snapshots. |
| `/people/<id>/identities`, `/identities/<id>/people` | GET active links; POST to the second path links a person. |
| `/identities/<id>/people/<link_id>/remove` | POST logically removes a link. |
| `/assets/<id>/subjects` | GET direct links, POST links a person or identity to the asset. |
| `/assets/<id>/subjects/<link_id>/remove` | POST logically removes the direct link. |
| `/people/<id>/assets`, `/identities/<id>/assets` | GET directly linked assets, paginated by link ID. |

A person requires `person_code` (64 UTF-8 bytes, unique NOCASE) and `display_name` (160); accepts `email` (254), `organization` (160), `job_title` (120), `notes` (2000), and `active=0|1`. An identity requires `realm` and `login` (160 each, unique NOCASE pair); accepts `display_name` (160), `kind=personal|shared|service`, `notes` (2000), and `active=0|1`. Updates replace fields, require `version`, and do not delete history. All writes require a trusted actor.

The person-identity link accepts `person_id`, `role=owner|user`, `notes` (2000), and the identity's current `version`; removal requires the same version. Both operations increment the version and history of the person and identity in the same transaction. Active duplicates and outdated versions produce `409`. Deactivation preserves links; new links require both subjects to be active, while removal is allowed even after deactivation.

The asset link accepts `subject_kind=person|identity`, `subject_id`, `role=owner|user|administrator`, `notes` (2000), and the asset's `version`. Addition and removal return `{ok,asset,link}`, update only the asset's version and history, with `change.operation=subject_added|subject_removed` and a copy of the link. `subject_snapshot` preserves the subject at the time of linking; `subject` shows the current record. It does not change status, handovers, license seats, or access rights. It does not propagate an identity's assets to associated people. New links require an active subject and an asset that is not retired/lost; removal remains possible.

Lists return `{ok,items,next_after_id}`, with `limit=1..100` (default 30), `after_id`, descending order, and a 128 KiB budget. Directories accept `q` (200 bytes) and `active=0|1`; identities also accept `kind` and `person_id`. People expose `identity_count`, and identities expose `people_count`. The catalog, CSV, and dashboard accept positive `person_id` and `identity_id` values, combinable with AND across active direct `inventory_asset_subjects` links only: these filters and **View assets** do not automatically include operational assignments, handovers, or licenses. CSV retains the asset record format and does not import/export directory records or links.

## Structured assignments, handovers, and checks

Schema 4 adds `places` directory records, record contexts, handovers, and checks. Directory records use `kind=site|department|location`, `name`, `parent_id`, `active`, `version`, and a trusted actor. A site has no parent; a department can specify a site, while a location must specify one. The type cannot be changed. Changing the parent of a place used in current contexts returns `409`; references retained only in history do not prevent it. Deletion is not available: deactivating a place preserves references and the names stored in history.

Records return `context` with `assignee_person_id`, `assignee_person_name`, `assignee_user_id`, `assignee_user_name`, `site_id`, `site_name`, `department_id`, `department_name`, `location_id`, and `location_name`, along with the active handover in `custody` and `last_verification`; these fields are null if not yet recorded. Context is supported for `it_device`, `network_device`, and `physical`; software and licenses retain their own details and relationships. `POST /assets/<id>/context` requires `version` and replaces assignment and place references: it can specify `assignee_person_id` or `assignee_user_id`, never both, or neither to remove the assignment. The department and location must belong to the specified site. Setting an assignee changes the status to `assigned`; removing the assignee returns a previously assigned asset to `available`. Placement remains independent of the assignee.

Handover uses `version`, exactly one of `person_id` or legacy `user_id`, `checkout_on`, `due_on`, `condition_out=good|fair|damaged`, optional notes, and an actor. The recipient must be active; a person does not need a Punto account. It requires an asset that is available or already structurally assigned to the same recipient, always without an open handover; it assigns the asset to the recipient. Return uses `version`, `returned_on`, `condition_in`, optional notes, and `status=available|maintenance`: it closes the handover, removes the assignee, and preserves places. A return is possible even after the person is deactivated. A due date before the current day makes a still-open handover overdue. Actual dates cannot be in the future, and `due_on` cannot precede `checkout_on`.

A check uses `version`, `verified_on`, `condition=good|fair|damaged`, optional `observed_location_id`, optional notes, and an actor. It preserves expected and observed context; a discrepancy does not automatically move the asset, and `location_matches` is null if either location is missing. Handover and check lists use descending IDs, `limit` from 1 to 100, and `after_id`. A handover retains `person_id`/`person_name` or `user_id`/`user_name`, with the other ID null, initial notes in `notes`, final notes in `return_notes`, and actors in `actor_id`/`returned_actor_id`, along with dates and the `active` flag. The name is a snapshot resolved by the server. Asset record history records `change.operation=context_updated|checked_out|checked_in|verified`. Each operation checks the revision and saves the record, event, and history in the same transaction.

The service resolves and validates people in its own store. It does not query the main database: for legacy Punto users, the app validates active users, resolves names, and transmits this trusted identity, as it already does for the actor. The browser cannot supply arbitrary person/user or actor names. Later deactivation does not rewrite snapshots. The bridge exposes `GET /api/inventory/users` with `id`, `name`, and `email` to Managers only. The public `mine=1` filter and `by_user` distribution concern only Punto users, without implicit associations with people. Old `assignee` and `location` strings and handovers to users remain unchanged; they are not converted automatically. Operational assignments and handovers do not create `inventory_asset_subjects` links or person-identity relationships. CSV/the general editor cannot bypass context or an open handover.

### QR labels

The app generates a 90 × 50 mm SVG with the code, name, and absolute URL `/app#inventory?asset_id=ID`. Scanning requires authentication before opening the record. The URL incorporates the current domain and port: generate labels from the final address reachable by devices, and reprint them if the origin changes.

The [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) generator is vendored in `public/qrcodegen.js` under the MIT license, with provenance in `public/qrcodegen.LICENSE.txt`. `public/inventory-qr.js` builds SVG through the DOM without HTML derived from data, uses at least medium error correction, and keeps a four-module white border. Download and printing operate locally without a CDN or external service; printing isolates the label with CSS without changing the CSP. Preserve contrast and the white border when printing or resizing.

## CSV

`GET /v1/assets/export` uses the same filters and cursors as the catalog and returns `{ok,csv,exported,next_after_id}`. Each page includes the header; when concatenating pages, keep it only from the first. The 16 columns are `id`, `asset_tag`, `name`, `category`, `serial_number`, `manufacturer`, `model`, `status`, `location`, `assignee`, `purchase_date`, `warranty_until`, `notes`, `asset_type`, `details`, and `version`. All fields are quoted; `details` is JSON. It neither exports nor imports relationships.

`POST /v1/import/preview` accepts `csv`, `mode=create_only|upsert` (default create_only), and a trusted actor. CSV uses UTF-8, an optional BOM, comma separators, LF/CRLF, quoted multiline fields, and doubled quotation marks according to RFC4180; maximum 256 KiB and 200 rows. The `asset_tag` and `name` columns are required. Unknown/duplicate headers, repeated tags, invalid fields, and conflicts are reported per row. `upsert` resolves by tag and requires the current version to update; an `id` belonging to another tag is rejected. Omitted columns preserve existing values; explicit empty cells clear optional fields. `details` requires a JSON object (`{}` to clear optional details), while type, status, and update version must remain valid. License `software_id` references must belong to software records already persisted at the start of the preview: they cannot use temporary IDs of software created in the same batch. IDs refer to the local archive, so check licenses imported from a different inventory.

The preview performs the same validation as saving without persisting records or history. It returns `summary`, `items`, `can_commit`, and a 32-hexadecimal-digit `preview_id`, tied to the actor, single-use, and valid for 15 minutes (maximum 64 open plans per actor). `POST /v1/import/commit` accepts the ID and trusted actor: it revalidates versions and saves the entire batch together with one snapshot per row. Even a conflict consumes the preview and leaves no partial updates.

To protect opening files in spreadsheets, export prepends an apostrophe to values that begin with an apostrophe or whose first character other than ASCII space/tab/CR/LF is `=`, `+`, `-`, or `@`. Import removes exactly one apostrophe if the remainder meets this condition. The convention allows exported files to round-trip; in manually prepared files, double the literal apostrophe before such prefixes.

## Due dates

`GET /v1/due?today=YYYY-MM-DD&days_before=30&limit=100&after_id=...` returns `{ok,items,next_after_id}`. Today is optional and follows the business time zone; the lead time ranges from 1 to 365 days. It includes warranty, maintenance, expiration, and renewal up to the threshold, including dates already past; it excludes retired or lost records. Fields are `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind` (field name), `due_on`, and `days_remaining`. The limit counts assets, and all due dates for an asset stay on the same page. The service exposes the data; scheduling and notification deduplication are in the main app.

`GET /v1/operational-due?kind=return|verification&verification_days=90&today=YYYY-MM-DD&limit=100&after_id=...` returns the same paginated envelope. `kind` is required; date, interval, and pagination are optional, with the same limits as the catalog. Items include the due-date fields and `source_id`. For `return`, `due_kind=return_due`, `source_id` identifies the open handover, and `due_on` is the expected return date, strictly before the requested day. For `verification`, `due_kind=verification_due`, `source_id` identifies the latest check, and `due_on` is its date plus the interval; for assets never checked, `source_id=0` and `due_on` is the creation date. This latter check includes both cases. Nonphysical, retired, and lost assets are excluded. The service does not create rules: the main app configures `inventory_return` and `inventory_verification`, retrieves all pages, and deduplicates alerts by including the originating handover or check.

## Startup and configuration

From the project root, to add the service to an already configured Docker installation:

```sh
python3 scripts/configure_network.py --env-file .env.docker prepare
docker compose --env-file .env.docker build inventory app
docker compose --env-file .env.docker up -d inventory app
```

For a new installation, first follow cluster startup in the [main README](../README.md). `prepare` extends the existing CA with the missing inventory certificate and creates `data/addon-secrets/inventory-api-token` only once. It does not replace preexisting CAs, certificates, or tokens; incomplete or invalid material requires restoring the original.

| Addon variable | Compose value |
| --- | --- |
| `INVENTORY_BIND` | `172.30.80.2` |
| `INVENTORY_PORT` | `8092` |
| `INVENTORY_ALLOWED_CLIENT` | `172.30.80.10` |
| `INVENTORY_API_TOKEN_FILE` | `/run/secrets/inventory-api-token` |
| `INVENTORY_TLS_CERT_FILE` | `/etc/punto/tls/inventory-addon.crt` |
| `INVENTORY_TLS_KEY_FILE` | `/etc/punto/tls/inventory-addon.key` |
| `INVENTORY_DB_PATH` | `/var/lib/punto-inventory/inventory.sqlite3` |
| `INVENTORY_BUSINESS_TIMEZONE` | `Europe/Rome` (service default; time zone for the license expiration day). |
| `INVENTORY_BACKUP_DIR` | `/var/backups/punto-inventory`; enables automatic copies. Missing/empty in native mode: backups disabled. |
| `INVENTORY_BACKUP_INTERVAL_HOURS` | `24`, configurable from 1 to 168 hours. |
| `INVENTORY_BACKUP_KEEP` | `14`, configurable from 1 to 365 verified copies. |

The app uses `INVENTORY_API_URL=https://inventory-api:8092`, the same token, and `INVENTORY_API_CA_FILE=/etc/punto/tls/ca.crt`. In native mode, the module is optional: leave all three app variables empty to disable it. To enable it, configure them together, with HTTPS and a verified CA; internal names on port 8092 or `localhost`/`127.0.0.1` with an explicit port are allowed. To start the addon natively, set the bind and allowed client to loopback, existing TLS/token paths, and a writable SQLite path. HTTP mode without TLS is not available.

## Persistence and isolation

The dedicated `inventory_addon_data` volume contains `inventory.sqlite3` and any SQLite WAL/SHM files. The addon uses UID/GID `10005:10005`, a read-only root filesystem, removed Linux capabilities, and no ports published on the host. It is connected only to `inventory_api` (`172.30.80.0/24`, an `internal` network with an `isolated` gateway), without `app_egress`, access to Squid, or MySQL/MongoDB networks and credentials. Only the app and addon receive the inventory token.

Data and snapshots are stored in plaintext in the volume; TLS protects transport. The MySQL backup and document backup do not include inventory. The addon is a single instance and does not inherit the MySQL cluster's HA. Do not delete the volume to update the image.

### Automatic backups and verified recovery

Compose enables a consistent SQLite copy at initial startup, then every 24 hours, in the separate `inventory_addon_backups` volume, retaining the last 14 verified copies. A restart recovers the last intact copy and its due time without producing a duplicate on every startup. The directory belongs to UID/GID `10005:10005`; files are created with `0600` permissions. The copy uses the SQLite Backup API, includes committed WAL writes, passes `PRAGMA integrity_check`, and is published with an atomic rename. Rotation affects only verified regular files with the service's name; it does not follow symbolic links or remove unrelated files or corrupt copies.

`GET /v1/status` includes `backup` with whether backups are enabled, interval, retention, last attempt, last success, copy name, next due time, and any error. Times are UTC, and the host path is not exposed. A backup error keeps inventory available, is recorded in logs, and delays the next attempt by at least five minutes. Local copies must also be transferred to external storage to protect against loss of the host; keep tokens and TLS materials separately.

The helper program requires Python with SQLite and operates on files accessible to the administrator:

```sh
python3 scripts/inventory_backup.py create /percorso/inventory.sqlite3 /percorso/backup-manuale.sqlite3
python3 scripts/inventory_backup.py verify /percorso/backup-manuale.sqlite3
python3 scripts/inventory_backup.py restore /percorso/backup-manuale.sqlite3 /percorso/inventory-ripristinato.sqlite3
```

`create` is a consistent online copy; `restore` produces a new verified database and never overwrites an existing file. To activate a restore, stop the addon, retain the previous store and any WAL/SHM files, place the new file as `inventory.sqlite3` in a store without the old WAL/SHM files, then restore UID/GID `10005:10005` ownership before restarting. Check the catalog and history. Do not copy only the main database while the service is writing.

Inventory schema is at version 6. On startup, the addon automatically migrates a version 1 store, preserving IDs, records, actors, and history: existing records become `it_device` with empty details. Previous snapshots remain in their original format; new ones contain type and details. Make a consistent backup before updating. Migration is transactional; a database with a future version is rejected. Migration 2 → 3 adds persistent CSV previews, and migration 3 → 4 adds directory records, contexts, handovers, and checks without reinterpreting previous text. Migration 4 → 5 adds people, identities, many-to-many links, directory record history, and direct asset links. Migration 5 → 6 adds people to operational assignments and handovers and person/identity recipients to licenses, preserving existing IDs, legacy users, text, references, and snapshots without implicit conversions. Images supporting at most schema 5 cannot open schema 6. Returning to an earlier image requires restoring a backup compatible with it; data added after migration requires an up-to-date schema 6 copy: changing `PRAGMA user_version` is not enough.

## Build and checks

```sh
make -C inventory-addon
python3 -m unittest discover -s tests -p 'test_inventory*.py' -v
python3 -m unittest discover -s tests -p 'test_cluster_manager.py' -v
```

Native dependencies: C11 compiler, pkg-config, OpenSSL, SQLite, json-c, and an up-to-date time zone database. The Docker image uses Debian, includes `tzdata` for IANA time zones, and starts the service with an unprivileged user. Functional tests use temporary stores and certificates. The network suite is separate and optional, against an already running stack:

```sh
PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
```

Further references: [operational guide](../docs/en/prod/asset-inventory.md), [architecture and public contract](../docs/en/tehnical/asset-inventory.md).
