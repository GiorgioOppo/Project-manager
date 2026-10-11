# Diagnostics and observability

[Italiano](../../tehnical/diagnostica-e-osservabilita.md) | **English**

Manual referring to the code of **10 October 2026**. It provides read-only checks to identify the component involved in a problem and determine the next step. It does not install a monitoring system or introduce automatic repair procedures. For topology see [architecture](architettura.md); for operational intervention use [backup and restore](operazioni-backup-ripristino.md).

Sources: [Compose](../../../compose.yaml), [HTTP server](../../../src/server.c), [SQL health](../../../src/storage.c), [group monitor](../../../docker/db-proxy/health.py), [MongoDB health](../../../docker/mongodb/health.js), [Squid health](../../../docker/squid/proxy-health), [email client](../../../src/mail_client.c), [Calendar client](../../../src/calendar_client.c).

## Existing tools and limitations

The stack has Docker health checks, Group Replication status, process logs, addon status APIs, application auditing and persistent queues. Prometheus endpoints, Grafana dashboards, distributed tracing, centralized log storage, infrastructure thresholds with automatic alerts and automatic SLO measurement are not implemented.

UI-configurable alerts check missing hours and attempts involving nonexistent projects. They do not check disk space, certificate expiry, cluster health or Calendar delays. They are product checks run by the app process; if the app is stopped, they cannot report its own shutdown.

| Existing signal | Correct interpretation |
| --- | --- |
| Docker health check | Latest result of a limited test on an individual service |
| `manage_cluster.py status` | SQL member status and relevant group settings |
| HTTP `/healthz` | The app can execute `SELECT 1` on the configured backend |
| Application audit | Confirmed business changes with actor and before/after content |
| Notifications and outbox | Alert process outcomes and email requests |
| Calendar status API | Connection and progress for the authenticated user |
| Squid logs | Source, method, domain and tunnel result |

A successful check is a point-in-time measurement, not a guarantee of continuous availability. The absence of an error in logs does not prove that a remote write did not occur.

## Initial triage in five steps

1. Record time, public origin, feature, user profile and HTTP code; describe whether the problem affects one user or everyone. Do not copy passwords, cookies, tokens, OAuth queries or document content into the report.
2. Check whether ingress responds and whether the problem affects the page, sign-in or just one API.
3. Read container and SQL group status without restarting services to clear the symptom.
4. Collect a limited log excerpt from only the affected components and compare timestamps.
5. Before an ambiguous mutation, reread the resource or queue. Business APIs have no universal idempotency key; addons have their own rules.

From the project root, with the default domain and ports:

```sh
docker compose --env-file .env.docker ps
python3 scripts/manage_cluster.py --env-file .env.docker status
curl --silent --show-error --max-time 5 --output /dev/null --write-out '%{http_code}\n' http://localhost/healthz
docker compose --env-file .env.docker logs --since=15m --tail=100 app proxy-db
```

`status` is read-only. `init`, `recover`, `configure_network.py apply`, `up --force-recreate` and manually running an alert are interventions, not equivalent diagnostic tools. `prepare` can create missing materials and must not be treated as a completely write-free check.

## Interpreting health checks

| Service | Configured test | What remains outside its scope |
| --- | --- | --- |
| App | HTTP on the private bind and `SELECT 1`; `200` with `database:mysql` or `sqlite`, otherwise `503` | PDFs, business authorization, data completeness, SMTP and Google |
| `mysql-1` | Local SQL query with the application account | Quorum and Group Replication membership |
| `mysql-2`, `mysql-3` | Local administrative SQL query | Quorum and application writability |
| `proxy-db` | At least two ready monitor endpoints; each checks `ONLINE`, `PRIMARY`, no read-only state and at least two online members | Validity of all future transactions or MongoDB availability |
| `mongodb` | Authenticated ping with document account and TLS | Correctness of every PDF and correspondence with the SQL manifest |
| Squid | Deliberately forbidden local HTTP request that must receive `403` | Actual connection to Google or SMTP endpoints |
| Email / Calendar addons | HTTPS with verified CA and service name/IP, `/healthz` | Provider credential validity and remote operation outcome |
| NPM | Image's `/usr/bin/check-health` | Complete operation of all hosts, sign-in and application flows |

A container can be `running` and `unhealthy`. Waiting periods are expected after first startup; evaluate Compose `start_period`, interval and retry count before interpreting a single transition.

To distinguish NPM ingress from the app process, the same health check can be invoked from inside the app:

```sh
docker compose --env-file .env.docker exec -T app curl --noproxy '*' --fail --silent --show-error --max-time 3 --header 'Host: localhost:8080' http://172.30.20.10:8080/healthz
```

If this works but the public URL fails, focus diagnosis on NPM, hostname, DNS, public certificate and allowed origins. The command requires no cookies or tokens. Do not expose port 8080 on the host to bypass the proxy.

## Triage by symptom

### The app will not open or NPM returns 502

Check NPM and app status, the internal health check above and recent logs from both services:

```sh
docker compose --env-file .env.docker logs --since=15m --tail=100 proxy-web app
```

The intended proxy destination is `http://app:8080` on `private_app`. The app must listen on `172.30.20.10`; it does not respond on its other interfaces. Moving the listener to `0.0.0.0` or publishing it directly is not an equivalent fix.

If NPM does not start after customization, also check the callback privacy filter. Build and startup reject configurations incompatible with OAuth protection. Do not remove the patch or ACL as a generic recovery attempt. The [certificates and secrets](gestione-certificati-e-segreti.md) guide distinguishes public certificate issues from private CA issues.

### App `/healthz` returns 503

The problem concerns the SQL backend query used by the app. Check the group, HAProxy monitor, authentication and SQL TLS in that order:

```sh
python3 scripts/manage_cluster.py --env-file .env.docker status
docker compose --env-file .env.docker logs --since=15m --tail=100 proxy-db mysql-1 mysql-2 mysql-3 app
```

Three reachable MySQL processes may have Group Replication stopped. With only one available member, blocked writes are expected: do not set `super_read_only=OFF` or force a new bootstrap. If all members have stopped, the relevant procedure is `recover`, after verifying context and following [operations](operazioni-backup-ripristino.md).

A monitor error prevents HAProxy from treating members as usable. The proxy entrypoint stops the container if the monitor process exits, allowing the normal restart policy to act. Do not replace monitor checks with a simple TCP ping.

### Only PDF upload or download fails

A positive app `/healthz` does not rule this out. SQL stores the manifest; MongoDB stores the bytes. Check `mongodb`, TLS and application logs. Unauthorized uploads produce `403`; nonexistent contracts `404`; invalid format or signature `400`; unavailable storage may produce `503`.

Downloads check size and SHA-256 before returning the PDF. A mismatch must not be resolved by updating the hash to fit an unexpected blob: check the original copy, manifest and consistent backup. There is no automatic reconciliation of unreferenced objects.

Do not print GridFS documents or contract content during triage. If a file must be checked, use a test document or authorized procedure and retain only its ID, size and fingerprint in the record.

### Sign-in rejected or access lost

`401` indicates invalid credentials, an inactive account or an absent/expired session, depending on the endpoint. Login `429` comes from the process-wide counter: after eight failed attempts within 60 seconds, even different accounts may be temporarily blocked. Wait for the window; do not retry rapidly.

Restarting the app loses in-memory sessions. Changing `PUNTO_PASSWORD` in the env file does not change an existing account: that variable serves the first user in an empty database. `403` may instead indicate an incorrect Host/Origin or insufficient permission. Distinguish authentication errors from authorization for a project or manager feature.

User profile and status changes are checked server-side. The session API can be read using a private cookie jar to check the current profile without disclosing the cookie. See [app APIs](api-app.md).

### Hours rejected or a report appears incomplete

Check entry ownership, date, minutes and permission validity both today and on the worked day. A manager reads their personal daily log through the same API as an operator; company totals are in reports. Do not assume `jobs.total_minutes` totals all users.

Check report range, user/project filters, the distinction between zero and missing rates, and the 1000-row limit. The contract value view selects contracts associated with filtered hours and counts their full value only once. It is neither an invoice nor a value allocated by date. Rules are in the [APIs](api-app.md) and [data model](modello-dati.md).

### An alert did not arrive at the expected time

Check that the app was running, the rule is enabled, the owner is still an active manager and recipients are valid. The loop checks roughly every 30 seconds, in the same process as HTTP requests: it does not guarantee execution at the exact second.

Missing hours use Monday–Friday, a 480-minute target and a 48-hour grace period; days are tracked per rule and subject. Vacation and holidays are not automatically deducted. Distinguish checked subjects from recipients. Operators can read only notifications concerning them.

Read the last run, next run and notifications through authorized APIs. `POST .../run` actually executes the rule and may create notifications or queue emails: it is not a read-only test. Do not use it as the first check on operational data.

### Email remains pending despite a healthy addon

SMTP still needs configuring in the deployment. `pending_configuration` is therefore expected. The app API's `smtp_configured` flag derives from the last query to the email service: it may also be false if the private call fails or returns an invalid response. That flag alone cannot distinguish missing configuration from addon unavailability.

Check `mail` health and logs, CA, `mail-api` alias, token and Squid policy. The actual private endpoint `GET /v1/status` requires the token and an authorized source; there is no additional public SMTP panel. Query it only with tools that keep the bearer out of URLs, shell history and logs.

The app has a 3-second timeout for private email calls; the addon can wait up to 20 seconds for the provider with the Compose value. A request may therefore have an ambiguous local outcome while the service is still working. Retries must retain the same key and content: the ledger handles idempotency. `delivery_unknown` and `delivery_failed` require provider verification, not queue deletion. `sent` confirms SMTP acceptance, not inbox delivery.

### Calendar does not synchronize

If `oauth_configured=false`, Google configuration is absent or deferred; do not try connecting a real account to diagnose the container. If `connected=false`, the user has no active consent. The public endpoint always uses session identity, including for a manager.

With an already authenticated cookie jar stored outside the repository, you can read:

```sh
APP_ORIGIN='https://progetti.example.com'
COOKIE_JAR='/percorso/privato/session.cookies'
curl --fail-with-body --cookie "$COOKIE_JAR" "$APP_ORIGIN/api/calendar/status"
curl --fail-with-body --cookie "$COOKIE_JAR" "$APP_ORIGIN/api/calendar/changes"
```

These responses may contain personal identifiers: do not publish them in full. Useful fields are `last_sync`, `last_error`, `reauth_required`, `sync_requested`, `sync_running`, operation status and attempt count. `/changes` has row and byte limits and may indicate `truncated`; it is not a complete queue export.

| Condition | Interpretation / next step |
| --- | --- |
| `reauth_required` | New individual consent is required; restarting alone does not renew revoked credentials |
| `queued` | Request retained for worker processing |
| `unknown` | Remote outcome must be reconciled; do not create a new key to force a duplicate |
| `conflict` | Remote ETag differs; retrieve the updated version and decide how to proceed |
| `failed` | Terminal outcome to analyze before a new operation |
| `applied` | Operation confirmed by the service; also check cache refresh |
| `sync_running=true` | Worker busy; observe whether `last_sync` advances, accounting for errors and timeouts |
| `cache_integrity_failed` in the private API | Encrypted data cannot be decrypted/verified; the public bridge may report a generic error. Preserve volume and key and investigate |

Full or incremental refresh is atomic with respect to the local cache: a failed download retains the previous cache. Recurring events are series and exceptions, not already expanded occurrences. A series outside the apparent range therefore does not prove a faulty filter. The full description is in [addon APIs](api-addon.md).

### Squid returns 403 or the new list seems ignored

A `403` may result from a disallowed source, method, host, port or DNS response. Read proxy logs and the mounted non-secret file:

```sh
docker compose --env-file .env.docker logs --since=15m --tail=100 proxy-egress
docker compose --env-file .env.docker exec -T proxy-egress cat /etc/squid/allowed-domains.txt
docker compose --env-file .env.docker exec -T proxy-egress squid -k parse -f /etc/squid/squid.conf
```

Syntax verification does not reload the policy. An editor may have replaced the filesystem file while leaving the bind mount tied to previous content: targeted recreation after building and checking differs from read-only diagnosis. Policy cannot distinguish HTTPS paths inside the tunnel; do not add wildcards to bypass a rejection you have not understood.

## SQL reads limited to configuration

When the schema version is needed, an administrator can read just the metadata, avoiding user or contract table dumps:

```sh
docker compose --env-file .env.docker exec -T mysql-1 sh -c 'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --user=root --database="$MYSQL_DATABASE" --batch --skip-column-names --execute="SELECT version FROM schema_metadata WHERE id=1;"'
```

The command uses the credential already in the container environment without putting its value in the command line. It neither enables shell debugging nor prints the environment. The version expected by current code is 10. To check the group, use the controller first; it avoids printing queries and sensitive values on errors.

## Logs, privacy and evidence collection

Logs are diagnostic, whereas auditing records business history. Do not use the audit as complete HTTP tracing or seek an exhaustive list of every Calendar change or SMTP-accepted message in logs.

Squid and addons use `json-file` rotation with three files of 10 MB each. This setting is not applied uniformly to all Compose services. NPM also retains files under its `data` directory; OAuth queries must not be recovered by bypassing callback protection. Centralized retention periods and secure log transfer are not configured in the repository.

A useful report contains: code or image version, time and time zone, service, endpoint without sensitive queries, status, affected container status, group status where relevant, sanitized error message and last known change. It excludes cookies, API/OAuth tokens, passwords, private keys, env files, resolved Compose configuration and PDFs.

## Proposed improvements, not yet implemented

A future observability system could collect HTTP latency/errors, disk space, quorum status, age of the last verified backup, TLS expiry, queue age and synchronization lag. Each signal will need an owner, threshold, deduplication, retention and alert recipient.

This is an operational proposal: no exporters or automations currently implement these measurements. Before adding them, define which metadata may leave the service and how to prevent identifiers, queries or business content from entering metric labels.
