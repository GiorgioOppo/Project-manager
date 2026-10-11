# Private mail and calendar addon APIs

[Italiano](../../tehnical/api-addon.md) | **English**

Verified against the code on **10 October 2026**. The services are C programs separate from the application, running in dedicated containers. SMTP and Google credentials are provisioned for, but **configuration of real providers is deferred**: tests use simulated services.

Sources: [mailer.c](../../../addon/src/mailer.c), [mail_client.c](../../../src/mail_client.c), [calendar.c](../../../calendar-addon/src/calendar.c), [calendar_client.c](../../../src/calendar_client.c), [alert scheduler](../../../src/alerts.c), [Compose](../../../compose.yaml). Component guides: [mail](../../../addon/README.md) and [Calendar](../../../calendar-addon/README.md).

## Transport, authentication and boundaries

| Service | Private API | Application source | API network | Egress |
| --- | --- | --- | --- | --- |
| `mail` | `https://mail-api:8090`, bind `172.30.60.2` | `172.30.60.10` | `mail_api` | `172.30.40.11` on `app_egress`, towards Squid. |
| `calendar` | `https://calendar-api:8091`, bind `172.30.70.2` | `172.30.70.10` | `calendar_api` | `172.30.40.12` on `app_egress`, towards Squid. |

The APIs do not publish host ports and are not routed by Nginx to the browser. The app uses an internal CA, verifies hostnames and requires at least TLS 1.2; it does not follow redirects or use proxies for these private connections. The service separately checks the source IP and `Authorization: Bearer <token dedicato>`, with constant-time comparison. This is not mTLS: application authentication uses a token plus source checking, while the certificate authenticates the server.

Only `GET /healthz` does not require a bearer. The listener accepts configured sources, loopback or its own address for health checks; disallowed sources are rejected/closed. A `200` health check does not prove that SMTP or Google are configured and reachable.

Responses are JSON, without caching. POST bodies are URL-encoded forms; text content is not interpolated into shell commands. No endpoint accepts an arbitrary remote URL to contact. Persistent content, encryption and tables are described in the [data model](modello-dati.md); browser/session checks are in the [app API contract](api-app.md).

## Mail-addon: HTTP contracts

| Method and path | Authentication | Response |
| --- | --- | --- |
| `GET /healthz` | No bearer, allowed source. | `200 {"status":"ok"}`. |
| `GET /v1/status` | Bearer and app IP. | `200 {"smtp_configured":false,"transport":"smtp_tls","idempotency":true}`; flag `true` when the provider is configured. |
| `POST /v1/emails` | Bearer and app IP. | Synchronous SMTP outcome, with persistent idempotency. |

`POST /v1/emails` accepts exactly `application/x-www-form-urlencoded` or `application/x-www-form-urlencoded; charset=UTF-8`. No query, attachment or recipient batch is implemented.

| Field | Validation |
| --- | --- |
| `idempotency_key` | 1–128 ASCII characters: letters, digits, `_`, `.`, `:`, `-`. |
| `to` | A single valid email address, maximum 254 bytes; no lists, display names or CR/LF. |
| `subject` | Nonempty UTF-8, up to 250 bytes, without control characters. |
| `body` | Nonempty UTF-8, up to 8192 bytes; tabs and line breaks allowed. |

Duplicate, unknown or missing fields are rejected. The encoded HTTP body is limited to 32768 bytes and headers to 8192 bytes. The generated message is `text/plain`; the sender comes from service configuration, not the form. There is no public app endpoint for sending arbitrary emails: currently the caller is the alert outbox.

| HTTP | Body/outcome | Interpretation |
| --- | --- | --- |
| `200` | `status:"sent"`, `idempotency_key`, `replayed` | The SMTP server accepted the message. Does not certify mailbox delivery or reading. |
| `503` | `error:"smtp_not_configured", status:"pending_configuration"` | Provider not configured; no key consumed and no SMTP attempt. |
| `409` | `error:"idempotency_conflict"` | Existing key with different recipient/subject/body. |
| `409` | `error:"delivery_unknown", status:"unknown"`, key and `replayed` | The message may already have been accepted; do not automatically resend using a new key. |
| `502` | `error:"smtp_delivery_failed", status:"failed"`, key and `replayed` | Attempt concluded as failed; terminal for that key. |
| `503` | `error:"ledger_unavailable"` | The key could not be reserved/checked; retryable while retaining content and key. |
| `400` | `invalid_request` or `invalid_email` | Invalid request/form. |
| `401`, `403` | `unauthorized`, `forbidden` | Unauthorized token or source. |
| `404`, `405`, `413`, `415` | Unsupported path, method, size, format. | Correct the call; do not retry indiscriminately in a loop. |

Example response for an already delivered message requested again:

```json
{"status":"sent","idempotency_key":"ambiente:outbox:123","replayed":true}
```

### Idempotency, timeouts and outbox

Before SMTP, the service saves `sending` in SQLite, with a payload hash, WAL and `synchronous=FULL`. The same key and hash return the persisted outcome; a different hash gives `409`. On restart, any remaining `sending` becomes `unknown`. `failed` is also terminal: repeating the key does not perform another send.

The app uses `MAIL_API_NAMESPACE:outbox:ID`, where `ID` is from `alert_email_outbox`. Do not change the namespace or reset the ledger to resolve a timeout: this could duplicate a delivery already accepted. The outbox persists recipient, text and state; the ledger retains only key, hash and timestamps.

| Mail client outcome | Application outbox state |
| --- | --- |
| Send accepted | `sent` |
| SMTP not configured | `pending_configuration` |
| Timeout/network/unrecognized response | `retrying` with the same key |
| Ambiguous delivery | `delivery_unknown` |
| Failure or idempotency conflict | `delivery_failed` |

The app checks SMTP status approximately every 60 seconds and handles at most one email per scheduler cycle, approximately every 30 seconds, with a circular cursor. Before sending, it rechecks that the manager is active and the recipient remains allowed. The call occurs outside the MySQL transaction; outcome and audit are saved together afterward.

App→mail client timeouts: 1 second to connect, 3 seconds total; maximum response 8192 bytes. Service SMTP timeout: `MAIL_SMTP_TIMEOUT`, 1–60 seconds, default 20. The client can therefore lose the response while SMTP continues; the persistent key allows querying the same send again without duplicating it. The mail server handles requests sequentially; a long send may delay reading status.

There is no endpoint to retry terminal outcomes or inspect the entire ledger. For `delivery_unknown`, check the provider's log and explicitly decide whether to send again. A consistent backup includes MySQL, ledger and namespace: see [backup/restore](operazioni-backup-ripristino.md).

### SMTP configuration to complete

| Variables | Function |
| --- | --- |
| `MAIL_BIND`, `MAIL_PORT`, `MAIL_ALLOWED_CLIENT` | Private listener and authorized source. |
| `MAIL_TLS_CERT_FILE`, `MAIL_TLS_KEY_FILE` | Listener certificate and key. |
| `MAIL_API_TOKEN_FILE` | App's private token. |
| `MAIL_LEDGER_PATH` | Persistent SQLite file. |
| `MAIL_SMTP_HOST`, `MAIL_SMTP_PORT`, `MAIL_SMTP_MODE` | Provider, port 587/STARTTLS or 465/SMTPS in the Compose policy. |
| `MAIL_SMTP_FROM`, `MAIL_SMTP_USER`, `MAIL_SMTP_PASSWORD_FILE` | Sender and provider authentication. The secret is a file, not an HTTP field. |
| `MAIL_PROXY_URL` | Mandatory CONNECT proxy on the egress path, `http://squid:3128`. |
| `MAIL_SMTP_TIMEOUT`, `MAIL_SMTP_CA_FILE` | Timeout and optional SMTP CA for a dedicated provider/fixture. |

The current configuration leaves SMTP disabled and the SMTP allowlist with a placeholder. Later, add the exact hostname to the [SMTP allowlist](../../../docker/squid/allowed-smtp-domains.txt), provision the secret and recreate Squid (`proxy-egress`) and `mail` according to the [mail guide](../../../addon/README.md). Partial configuration is rejected at startup. Enabling may also process already queued emails, so it affects real pending sends.

The container cannot contact Google endpoints: Squid policy for its source allows only authorized SMTP hostnames on permitted ports. Plaintext SMTP on port 25 is not provided for in the current policy.

## Calendar-addon: HTTP contracts

All `/v1/users/{id}/...` paths receive identity from the app. `{id}` is currently limited to **1–2147483647** by the addon parser, even though MySQL IDs are `BIGINT`. The bearer belongs to the service and authorizes the trusted caller to specify the user; the service does not interpret the user's cookie or global roles. Do not expose these paths to the browser or untrusted callers.

| Method and path | Fields/query | Main response |
| --- | --- | --- |
| `GET /healthz` | None; no bearer. | `200`, HTTPS process availability. |
| `GET /v1/status` | None. | `provider`, `oauth_configured`, `bidirectional`, `sync_interval`. |
| `GET /v1/users/{id}/status` | None. | Global status plus the user's connection, calendar and queue. |
| `POST /v1/users/{id}/oauth/start` | `session_binding` | Consent URL and state lifetime. |
| `POST /v1/users/{id}/oauth/callback` | `state`, `session_binding`, `code` or `error`. | `200 {"ok":true}` on completion; OAuth errors otherwise. |
| `GET /v1/users/{id}/calendars` | Optional `page_token`, `limit`. | One Google page, `items`, `next_page_token`. |
| `POST /v1/users/{id}/selection` | `calendar_id` | Calendar selection with Google permission checking. |
| `GET /v1/users/{id}/events` | Optional `from`, `to`, `limit`, `after_id`. | Cache page with cursor, calendar and last sync. |
| `GET /v1/users/{id}/changes` | None. | Latest operations, `items` and `truncated`. |
| `POST /v1/users/{id}/events/create` | `idempotency_key`, `event`. | `202` new operation or `200` replay. |
| `POST /v1/users/{id}/events/update` | `idempotency_key`, `event_id`, `etag`, `event`. | `202` new operation or `200` replay. |
| `POST /v1/users/{id}/events/delete` | `idempotency_key`, `event_id`, `etag`. | `202` new operation or `200` replay. |
| `POST /v1/users/{id}/sync` | Empty. | `202`, user status with `sync_requested:true`. |
| `POST /v1/users/{id}/disconnect` | Empty. | `200 {"disconnected":true,"remote_revoked":...}`. |

The service requires `Content-Type: application/x-www-form-urlencoded` for private POSTs (without a charset parameter); the bridge normalizes the format. Headers up to 8192 bytes, private body up to 65536 bytes, public bridge form up to 32768. No duplicate/unknown parameters; duplicate JSON keys in events are also rejected.

### Status and OAuth authorization

Example without Google credentials and without a user connection:

```json
{
  "provider": "google",
  "oauth_configured": false,
  "bidirectional": true,
  "sync_interval": 300,
  "connected": false,
  "calendar_id": "",
  "last_sync": 0,
  "last_error": "",
  "reauth_required": false,
  "sync_requested": false,
  "sync_running": false,
  "queued_changes": 0,
  "conflicts": 0,
  "unknown_changes": 0
}
```

`last_sync` is a Unix timestamp updated only after a successful full pull. `connected:true` indicates an account record exists: also read `reauth_required`, because consent may need renewal. `queued_changes` includes neither `unknown_changes` nor conflicts.

`session_binding` is the hexadecimal SHA-256 of the current session token, produced on the app server. At OAuth start, the service generates random state and PKCE S256, storing the state hash, user/session binding and encrypted verifier, with a 600-second expiration. It returns `authorization_url` and `expires_in:600`. The client must open that URL, not construct its own with different scopes.

Implemented scopes are `https://www.googleapis.com/auth/calendar.events` and `https://www.googleapis.com/auth/calendar.calendarlist.readonly`; offline access and explicit consent are requested. The callback verifies user, session and state, consumes it before exchanging the code and rejects replay. The browser goes through `/api/calendar/oauth/callback`; only the app calls the private POST. NPM configuration protects the callback against saving code/state in logs for the dedicated location.

Reconnecting the same account preserves the calendar, cache and queue history. A different account returns `409 google_account_mismatch`: explicit disconnection is required first. The check uses the primary calendar identity obtained from Google. Every new consent/selection changes a local generation; worker responses for an old generation cannot update the new connection.

### Available calendars and selection

`GET calendars`: `limit` 1–50, default 50; `page_token` returned by the previous page. One Google call per page; `next_page_token:null` at the end. Results are limited to 100 KiB: reduce the limit if a page is excessive. Do not assume the first page contains all calendars.

`selection` requires an ID obtained from the provider and `writer` or `owner` access; read-only access is insufficient for bidirectional sync. Pending `queued`/`unknown` operations prevent changing calendars (`409 pending_changes`). Selection prepares the new synchronization cycle; read `/status` and `/events` to distinguish a successful choice from an already updated cache.

### Events accepted for writing

The `event` field contains a JSON string. Allowed properties:

| Property | Constraint |
| --- | --- |
| `summary` | Required nonempty UTF-8 string, maximum 1000 bytes. |
| `description` | Optional, maximum 8192 bytes. |
| `location` | Optional, maximum 1024 bytes. |
| `start`, `end` | Required objects with either `date` or `dateTime`, never both; optional `timeZone`. |
| `date` | `YYYY-MM-DD`, year 1900–2199. For all-day events, `end.date` is exclusive. |
| `dateTime` | RFC 3339 with whole seconds and `Z` or an explicit offset, for example `2026-10-12T09:00:00+02:00`. No fractional seconds on writes. |
| `timeZone` | Nonempty string up to 100 bytes, characters compatible with names such as `Europe/Rome`; the provider completes time-zone validation. |

Start/end must use the same type, end after start, maximum duration 366 days. `attendees`, attachments, notifications, new recurrences, arbitrary private properties or webhook URLs are not allowed. Remote writes use `sendUpdates=none`.

```json
{
  "summary": "Revisione requisiti",
  "description": "Verifica del perimetro della commessa",
  "start": {"dateTime":"2026-10-12T09:00:00+02:00","timeZone":"Europe/Rome"},
  "end": {"dateTime":"2026-10-12T10:00:00+02:00","timeZone":"Europe/Rome"}
}
```

`idempotency_key`: 8–128 ASCII characters from `[A-Za-z0-9_.:-]`. `event_id`: 1–1024 alphanumeric characters, `_` or `-`. `etag`: double-quoted string, 2–200 bytes, copied exactly from a read; the ETag includes the quotes as part of the value. The bridge limits `event` JSON to 24576 bytes before form encoding.

Update operations have the same required event fields as create: they do not accept an object containing only the changed property. The service normalizes JSON and performs a Google PATCH with the validated subset. Do not rely on omission to remove an optional remote field: the current contract does not expose a dedicated property-deletion operation.

### Cache reads and recurrences

`GET events` accepts `limit` 1–100 (default 20), `after_id`, and `from`/`to` together. Inclusive endpoints, maximum 366 days; period boundaries are **UTC** days, not the UI time zone. Response: `items`, `calendar_id`, `last_sync`, `next_after_id` and `recurrence_mode:"masters_and_exceptions"`. Each page respects 100 KiB and may contain fewer items than the requested limit; continue with `next_after_id` until it is `null`, retaining the same filters.

Ordering is by event ID, not date. Pagination is not an immutable snapshot across requests: synchronization may update the cache during browsing. Public fields include `id`, `etag`, `status`, `summary`, `description`, `location`, `start`, `end`, `recurrence`, `recurringEventId`, `originalStartTime`, `updated`, `created`, `htmlLink`, `sequence`, `eventType`, if present. Private properties and credentials are not exposed.

Recurring series remain masters with exceptions; **they are not expanded**. A master may appear even if its original start precedes the filter; `status:cancelled` tombstones may have no times. Do not interpret item counts as the number of appointments in the period. Editing the master ID may affect the entire series; this version does not implement a UI choice between occurrence and series.

### Queue, conflicts and retries

A new mutation responds before remote execution:

```json
{
  "id": 123,
  "idempotency_key": "calendar-create-example-001",
  "event_id": "identificativo-generato",
  "state": "queued",
  "replayed": false
}
```

`GET changes` returns `items` with `id`, `idempotency_key`, `event_id`, `state`, `error`, `attempts`, `replayed:false`. Maximum 100 recent operations in descending ID order and 90 KiB of data; `truncated:true` signals more results, but no cursor is yet available for browsing the full history. Replaying the same request returns the individual outcome with `replayed:true`.

| State | Meaning and handling |
| --- | --- |
| `queued` | Persisted, awaiting the worker or another attempt. |
| `unknown` | Remote write started/potentially executed; the Google event is checked before retrying. |
| `applied` | The service confirmed application or recognized an already applied result. |
| `conflict` | Divergent ETag or a conflict that cannot be resolved automatically. Reread the event and deliberately decide on a new operation. |
| `failed` | Permanent error/remote validation/integrity; caller intervention required. |

The key is unique per user. Same normalized content = `200` replay; different content = `409 idempotency_conflict`. For creates, the Google ID is deterministically derived from user and key, while the private `puntoMutation` marker also binds the payload; update/delete send `If-Match` with the ETag. A Google `412` leads to `conflict` without blind overwriting.

`unknown` is saved before the remote call. After a crash/timeout, the service queries the event: a matching marker proves an already applied write; for deletions, it checks absence/remote state; a different ETag may require a conflict. Exponential delays start at 30 seconds and are capped at 3840 seconds; timestamps are minimum thresholds, not guarantees of precise execution. No maximum attempt count for temporary errors is defined as a user policy.

The worker runs approximately every 5 seconds, chooses one user per turn with a circular cursor and handles at most 20 changes per synchronization cycle. Default remote polling is 300 seconds. The API and worker use separate SQLite connections; Google calls do not keep long write transactions open. Polling is implemented; Google push webhooks are not.

Initial/incremental download follows all pages and changes `sync_token` only together with the transaction updating the cache. A Google `410` starts a new full snapshot, retaining the previous one until success. Limits per synchronization: 100 pages, 20,000 events, 64 KiB per event, 16 MiB total payload; beyond the limit, the previous cache remains valid and an error is reported. A single provider HTTP response is limited to 4 MiB.

`POST sync` immediately returns `202`; check `sync_running`, `last_sync`, `last_error` and the queue. This is not proof that all operations have been applied or a promise of exactly-once behavior towards external services.

### Errors and disconnection

| HTTP/code | Caller action |
| --- | --- |
| `400 invalid_form`, `invalid_event`, `invalid_event_reference`, `invalid_idempotency_key` | Correct the payload; no valid remote write has been queued. |
| `400 invalid_oauth_state`, `invalid_oauth_callback`, `oauth_cancelled` | Restart consent from the correct session when appropriate. |
| `401 unauthorized`, `403 source_not_allowed` | Check token/private routing. |
| `403` for a calendar without write access | Select a calendar on which Google grants writer/owner. |
| `409 calendar_not_connected`, `calendar_not_selected` | Complete connection/selection. |
| `409 pending_changes`, `google_account_mismatch` | Resolve the queue or change accounts through explicit disconnection. |
| `409 idempotency_conflict` | Do not reuse a key for two different intents. |
| `503 oauth_not_configured` | Configure the provider before consent. |
| `500`, `502`, `503` with other codes | Dependency, storage or provider unavailable; retain the key, reread status and apply a controlled retry. |

The public app bridge sanitizes internal/unexpected errors into `503`; do not expect the same error surface as the private service. Revoked/expired tokens set `reauth_required` and require new consent.

`disconnect` attempts refresh-token revocation, then locally deletes the account, events, queue and OAuth state. `remote_revoked:false` indicates remote revocation is unconfirmed, even if `disconnected:true` confirms local cleanup. A request already sent to Google may complete during disconnection: local removal does not retroactively undo an event created on the provider. Disconnection also deletes the user's idempotency history: do not intentionally reuse old keys for new operations after reconnecting.

### Google configuration to complete

| Variables | Function |
| --- | --- |
| `CALENDAR_BIND`, `CALENDAR_PORT`, `CALENDAR_ALLOWED_CLIENT` | Private listener and app source. |
| `CALENDAR_TLS_CERT_FILE`, `CALENDAR_TLS_KEY_FILE` | API certificate and key. |
| `CALENDAR_API_TOKEN_FILE` | Service token shared with the app. |
| `CALENDAR_TOKEN_KEY_FILE` | AES-256 key of 64 hexadecimal characters, separate from the database. |
| `CALENDAR_DB_PATH` | SQLite store in the persistent volume. |
| `CALENDAR_PROXY_URL` | Mandatory egress proxy, `http://squid:3128`. |
| `CALENDAR_SYNC_INTERVAL` | Polling 30–86400 seconds in production, default 300. |
| `CALENDAR_HTTP_TIMEOUT` | Total per Google call 1–60 seconds, default 10; connection 3 seconds. |
| `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET_FILE`, `GOOGLE_REDIRECT_URI` | Web OAuth client and exact public callback. |
| `CALENDAR_GOOGLE_CA_FILE` | Optional custom CA, used by test fixtures. |

To enable: enable Calendar API in the Google Cloud project, configure consent/users/scopes and a Web client, register for example `https://progetti.tuaazienda.it/api/calendar/oauth/callback`, provision ID, secret and redirect, then recreate the service. Partial settings are rejected at startup. Every user must grant their own consent. The secret must not enter URLs, the repository, frontend payloads or logs.

The only intended egress destinations are `accounts.google.com`, `oauth2.googleapis.com`, `www.googleapis.com` over HTTPS 443 through Squid. Service calls verify CA/hostname, require at least TLS 1.2, disable redirects and force the proxy even in the presence of `NO_PROXY`. Changing the allowlist alone does not authorize the code to use arbitrary providers. Outlook, CalDAV and other providers are not yet implemented.

## Internal call examples and isolated tests

The example is for a process already authorized on the API network, with a CA and a private header file prepared by the operator. The file indicated by `API_HEADER_FILE` contains the bearer header; do not print or commit it. Ports are not published on the host.

```sh
ADDON_CA='/percorso/privato/ca.crt'
API_HEADER_FILE='/percorso/privato/calendar-authorization.header'

curl --fail-with-body --cacert "$ADDON_CA" \
  --header "@$API_HEADER_FILE" \
  'https://calendar-api:8091/v1/users/123/status'

curl --fail-with-body --cacert "$ADDON_CA" \
  --header "@$API_HEADER_FILE" \
  --header 'Content-Type: application/x-www-form-urlencoded' \
  --data '' \
  'https://calendar-api:8091/v1/users/123/sync'
```

Normally use the app's authenticated public bridge; the internal example is for diagnostics and development. Do not pass an ID received from the browser without verifying application identity.

The [mail-addon](../../../tests/test_mail_addon.py), [mail client](../../../tests/test_mail_client.py), [calendar-addon](../../../tests/test_calendar_addon.py), [calendar client](../../../tests/test_calendar_client.py), [network](../../../tests/test_network_security.py) and [proxy privacy](../../../tests/test_proxy_privacy.py) suites verify contracts with simulated HTTPS SMTP/OAuth/Calendar services, test certificates and local CONNECT proxies. They do not demonstrate an already completed production Google/SMTP configuration.

The `CALENDAR_GOOGLE_ORIGIN` override is accepted only with `CALENDAR_TEST_MODE=1`, a loopback listener and a loopback proxy. That mode does not appear in the production Compose configuration; TLS verification remains enabled in fixtures too. Do not weaken TLS checks or enable test mode to connect a real provider.

Existing calendar integration is at the API and service level. Calendar UI, event→project selection, automatic time accounting, task/event mapping and advanced conflict-resolution policies remain future product work.
