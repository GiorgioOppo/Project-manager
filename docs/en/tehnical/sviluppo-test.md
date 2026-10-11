# Development and testing

[Italiano](../../tehnical/sviluppo-test.md) | **English**

This guide describes tools and tests present on **11 October 2026**. Run commands from the repository root. New module results are in the [11 October task report](verifica-attivita-2026-10-11.md); the instructions below do not by themselves attest to a test run. The [main README](../../../README.md) provides project context; [architecture](architettura.md) describes the components.

## Toolchain and backends

The [main Makefile](../../../Makefile) compiles C11 with `-Wall -Wextra -Wpedantic -Werror`, plus `-O2` unless overridden. Compiler warnings are treated as errors; do not disable them to make a change pass.

| Target | Main dependencies | Output |
| --- | --- | --- |
| SQLite app | C11 compiler, make, `pkg-config`, SQLite, libcurl, json-c; Security/CommonCrypto on macOS or OpenSSL on Linux | `build/sqlite/punto`, atomically copied to `build/punto` |
| MySQL app | Common C dependencies, MariaDB Connector/C (`libmariadb`), MongoDB C Driver (`libmongoc-1.0`), OpenSSL | `build/mysql/punto`, copied to `build/punto` |
| Email addon | OpenSSL, libcurl, SQLite, C11 compiler | `addon/build/punto-mailer` |
| Calendar addon | OpenSSL, libcurl, SQLite, json-c, `pkg-config`, pthread | `calendar-addon/build/punto-calendar` |
| Python suite | Python 3, build binaries and libraries, OpenSSL in PATH, local sockets | Fixtures and temporary databases |
| NPM privacy | Node.js; Docker only for the opt-in runtime case | Verification of Nginx configuration transformation |

The frontend is served directly from `public/` and requires no npm installation. Node is used for NPM tools and their tests, not to build the C app's interface.

```sh
make
make run
make DB_BACKEND=mysql
make -C addon
make -C calendar-addon
```

`make run` inherits variables exported in the shell and does not load `.env` automatically. Do not use deployment credentials in fixture databases. A native MySQL build does not start MySQL or MongoDB; configure the environment separately. The Docker procedure uses the [main Dockerfile](../../../Dockerfile), which compiles the MySQL backend in a builder stage and copies the binary to the runtime stage.

`make clean` removes the app's `build/` directory. Each addon has a `clean` target in its own directory. Databases, PDFs, and Docker volumes are not build outputs and must not be removed to resolve a compiler error.

## Ordinary suite

```sh
make test
```

The target builds the app and starts `python3 -m unittest discover -s tests -v`. Local suites use free ports, temporary databases, and temporary directories. SMTP and Calendar tests use simulated HTTPS/SMTP providers and CONNECT proxies on loopback: they neither send real email nor modify real calendars.

Opt-in deployment tests are skipped unless their variables are present. `skipped` means a test was not run and is not a positive result. Always retain counts of tests run, passed, skipped, and failed when interpreting a result.

To run a specific family:

```sh
python3 -m unittest discover -s tests -p 'test_reports.py' -v
python3 -m unittest discover -s tests -p 'test_work_board.py' -v
python3 -m unittest discover -s tests -p 'test_calendar_addon.py' -v
python3 -m unittest discover -s tests -p 'test_calendar_client.py' -v
```

Both Calendar families may compile binaries and generate fixture certificates; an environment blocking local sockets prevents these tests even if application logic is correct. Do not replace a TLS fixture with a real provider to bypass this limitation.

## Verification map

| Family | Main files | Coverage |
| --- | --- | --- |
| HTTP and authentication | [test_server.py](../../../tests/test_server.py), [test_roles.py](../../../tests/test_roles.py), [test_public_port.py](../../../tests/test_public_port.py), [test_reverse_proxy.py](../../../tests/test_reverse_proxy.py) | Parsing, sessions, profiles, Host/Origin, public ports, and behavior behind a proxy |
| Projects and hours | [test_workspace.py](../../../tests/test_workspace.py), [test_assignments.py](../../../tests/test_assignments.py), [test_job_defaults.py](../../../tests/test_job_defaults.py) | States, data, permissions, assignments, shared projects, and history |
| Tasks, Kanban, and blockers | [test_work_board.py](../../../tests/test_work_board.py) | Direct/group/shared permissions, revocations, assignees, hierarchies, stale revisions, Unicode validation and dedicated HTTP limit, atomic audit, deletion, and migration 7→8 |
| Organization | [test_groups.py](../../../tests/test_groups.py) | Tree, membership, roles, grants, cycles, intervals, and migration |
| Contracts and PDFs | [test_procurement.py](../../../tests/test_procurement.py), [test_documents_storage.py](../../../tests/test_documents_storage.py), [test_documents_backup.py](../../../tests/test_documents_backup.py) | Suppliers, contracts, relationships, upload/download, storage, and backup behavior |
| Reports and rates | [test_reports.py](../../../tests/test_reports.py), [test_labor_rates.py](../../../tests/test_labor_rates.py) | Filters, aggregates, visibility, costs, rounding, and time-dependent rates |
| Alerts | [test_alerts.py](../../../tests/test_alerts.py) | Rules, thresholds, 48 hours, working days, recipients, schedules, restart, and daylight-saving changes |
| Email | [test_mail_addon.py](../../../tests/test_mail_addon.py), [test_mail_client.py](../../../tests/test_mail_client.py) | SMTP TLS, proxy, API, ledger, uncertain outcomes, revocations, outbox, and migration |
| Calendar | [test_calendar_addon.py](../../../tests/test_calendar_addon.py), [test_calendar_client.py](../../../tests/test_calendar_client.py) | OAuth, user isolation, encryption, paging, synchronization, ETag, idempotency, timeout, and reconnection |
| Deployment | [test_deployment.py](../../../tests/test_deployment.py), [test_cluster_manager.py](../../../tests/test_cluster_manager.py) | Configurations, controller procedures, and safeguards |
| Proxy privacy | [test_proxy_privacy.py](../../../tests/test_proxy_privacy.py) | Preservation of ACLs/headers and removal of OAuth secrets from logs |

The task/blocker suite uses only a temporary HTTP server and SQLite databases. Kanban presents task states: HTTP tests cover values and permissions, while lists, columns, dialogs, and keyboard use also require UI testing. The presence of tests does not certify an updated MySQL deployment.

Support C code in `tests/` also checks document behavior, the alert scheduler, and driver TLS. Real-database integration tests complement SQLite tests without replacing them.

## MySQL/GridFS stack integration

These tests write synthetic data to an already-running environment. Prepare a test deployment and a dedicated manager. Credentials come from `PUNTO_MYSQL_TEST_EMAIL` / `PUNTO_MYSQL_TEST_PASSWORD`, falling back to `PUNTO_EMAIL` / `PUNTO_PASSWORD`; suites do not load `.env.docker` automatically or assume a demonstration account.

After supplying credentials securely:

```sh
PUNTO_MYSQL_TEST_URL=http://localhost python3 tests/test_mysql.py -v
PUNTO_PROCUREMENT_TEST_URL=http://localhost python3 tests/test_procurement_mysql.py -v
PUNTO_GROUPS_TEST_URL=http://localhost python3 tests/test_groups_mysql.py -v
```

URLs must be HTTP(S) origins without credentials. Fixtures have unique names, often prefixed `codex-test-`. These suites do not empty tables or remove existing accounts. SQL and procurement tests retain verification data; operators created by applicable tests are disabled during cleanup. Running them on operational data therefore leaves records and audit: they are not equivalent to a health read.

`PUNTO_MYSQL_TEST_MANIFEST` and `PUNTO_PROCUREMENT_TEST_MANIFEST` can save fixture IDs and hashes without passwords for later persistence checks. A manifest is not a backup.

## Networking and isolation

On a running local stack with correct TLS materials:

```sh
PUNTO_NETWORK_TEST=1 python3 tests/test_network_security.py -v
```

The suite checks the seven networks, IPs/interfaces, published ports, app access through NPM, private listeners, forbidden flows to databases and addons, prevention of direct egress, Squid policy, TLS, and secret separation. One test also performs HTTPS to an authorized Google host; it creates no event and requires no OAuth credentials. External network reachability can affect it.

The code supports `PUNTO_NETWORK_TEST_ALLOWED_HOST` to choose an authorized host and `PUNTO_NETWORK_TEST_EGRESS_IP` for the direct-egress test. Use values consistent with the lab, without turning the test into a production policy change. Cluster helpers read Docker context options; check the context before running tests against the daemon.

To check the specific Squid ACL for private DNS responses without changing the stack:

```sh
PUNTO_SQUID_POLICY_TEST=1 python3 tests/test_squid_policy.py -v
```

This test uses the Squid image in a temporary `--network none` container, with a read-only filesystem and no credentials. It adapts the listener to loopback and builds a controlled local DNS response. It tests policy, not a connection to a provider.

## MySQL driver TLS

```sh
PUNTO_MYSQL_TLS_TEST=1 python3 tests/test_mysql_tls.py -v
```

The test compiles and runs a native C verification program on loopback, with the driver and MariaDB Connector/C. It is independent of Docker and real databases and uses neither environment credentials nor production CAs. It checks eight TLS rejection cases, including configuration errors and downgrade attempts. As a manual alternative, the program can be built and run in a `--network none` container, as indicated in the test docstring. Do not connect it to an operational database to simulate incorrect certificates. Fix identity failures in the certificate or destination, not by weakening TLS mode.

## Nginx Proxy Manager privacy

The Node portion of the test checks transformations of generated configurations. If `node` is not in PATH, `PUNTO_NODE_BINARY` can point to a Node executable available in the environment. Relevant cases are skipped without Node.

To enable the isolated Nginx runtime test too:

```sh
PUNTO_DOCKER_TESTS=1 python3 tests/test_proxy_privacy.py -v
```

`PUNTO_PROXY_IMAGE` selects the NPM image built from this repository; the test's default name assumes the expected local Compose prefix. When the project name differs, set the actual image name. The test starts a temporary container, checks synthetic requests with sensitive query parameters, and inspects logs; it does not modify running NPM.

The patch depends on the pinned NPM version. Before updating that image, check both the static transformation and runtime test, including ACLs, authentication, and rejected custom configurations.

## High-availability tests

The following suite **temporarily disrupts availability**, although its cleanup block attempts recovery. Use it only on a development cluster, after backup and with authorization for the outage window:

```sh
PUNTO_CLUSTER_HA_TEST=1 python3 tests/test_cluster_ha.py -v
```

It reads the local configuration file, stops one member, pauses two members, and checks recovery and full restart. `PUNTO_CLUSTER_ENV_FILE` can specify another file. During quorum loss, the app must reject writes and may return `503`. The final procedure attempts to restore services, but their status must still be checked after an interrupted test.

The correct result includes writes possible with two members, blocking with an isolated member, nodes rejoining, and data preservation. `make test` without the enabling variable does not run these tests. Do not force a member writable to obtain a positive result.

## Interface testing

For frontend changes, check at least login, navigation, error messages, mobile and desktop layout, keyboard use, and the entire affected workflow with manager and operator profiles. Check API responses even when the interface hides an action: permission testing is server-side.

Use synthetic data in a dedicated environment for operations that generate notifications, delete projects/hours, or change grants. Retain evidence without credentials, personal data, or OAuth tokens. The [historical UI report of 10 October 2026](verifica-ui-2026-10-10.md) documents earlier testing; it does not replace subsequent verification after code changes.

Calendar currently exposes APIs, not a new calendar page; do not claim the graphical event–project workflow has been verified. Real SMTP and Google remain to be configured: simulated-provider tests do not attest to consent or real delivery through an external account.

## Change procedure

1. Identify the affected module and contract, consulting the [data model](modello-dati.md), [app API](api-app.md), and [addon APIs](api-addon.md).
2. Preserve the separation between native SQLite and Docker MySQL. If the schema changes, prepare migration and data-preservation checks; do not retroactively change an already-deployed database without an upgrade path.
3. Update server checks, errors, and frontend consistently. Do not trust browser-supplied user IDs to decide personal access.
4. Build with warnings as errors and run tests verifying changed behavior. Broaden verification when a change crosses modules or introduces a migration.
5. If images, TLS, or networks change, build affected images and repeat specific tests in a lab; distinguish them from unit tests.
6. Update contract, configuration, and operations documentation. Report what passed and what was skipped or remains to be configured.
7. Before deploying, save the appropriate backup and recreate only necessary services following [installation](installazione-configurazione.md).

Do not commit `.env.docker`, volumes, dumps, files in `data/`, certificates, keys, tokens, logs, or manifests containing sensitive data. `.gitignore` and `.dockerignore` reduce accidental exposure; reviewing the change remains necessary.

## Available historical results

The [main README](../../../README.md) records testing on **10 October 2026**, after adding Calendar:

- Complete suite: **270 tests**, with **241 passed and 29 optional tests skipped**.
- After the latest fixes: **23 Calendar addon tests**, **15 Calendar bridge tests**, and **8 NPM privacy tests** passed.
- Isolated Squid test passed and **20/20 live network checks** passed.
- Ten healthy services; three SQL members `ONLINE` / `PRIMARY`, schema 7; fingerprints of the eight protected tables unchanged during testing.

These are previously recorded results, not new runs performed while writing the manuals. Calendar tests used a simulated provider, and Google OAuth was not configured. Update a historical count only after a new observed and documented run.
