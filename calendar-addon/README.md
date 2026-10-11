# Google Calendar addon

**English** | [Italiano](README.it.md)

The Compose service is named `calendar`, and its local image is `punto/calendar:${PUNTO_IMAGE_TAG:-local}`. To update an installation using the previous names, follow the [Docker naming guide](../docs/en/tehnical/nomi-servizi-e-immagini.md); directories, volumes and TLS identities retain their existing names.

`punto-calendar` is a C service independent of the app, with a private HTTPS API and a persistent queue for changes. It imports Google events into its own storage and sends Google the creations, updates and deletions requested by the app. It does not access the MySQL or MongoDB databases.

Google credentials are not yet configured: the container and APIs work, but the status exposes `oauth_configured: false`. No account is connected and no Google calls are made in this configuration.

## Private API

The service listens on `https://calendar-api:8091`. The app must verify the certificate against the internal CA and send `Authorization: Bearer <dedicated token>`. The app's IP address is checked separately from the token. Only `GET /healthz` requires no authentication, and it remains restricted to allowed sources or the container itself.

The user ID in the path is that of the user authenticated by the app. The browser cannot call this API directly or choose another ID through the app's public bridge.

| Method and path | Usage |
| --- | --- |
| `GET /healthz` | Checks HTTPS and process availability. |
| `GET /v1/status` | Global status, provider, interval and OAuth configuration. |
| `GET /v1/users/<id>/status` | The user's connection, selected calendar, synchronization, queue and conflicts. |
| `POST /v1/users/<id>/oauth/start` | Creates a Google consent flow with a single-use state and PKCE. |
| `POST /v1/users/<id>/oauth/callback` | Completes consent through the app. |
| `GET /v1/users/<id>/calendars` | One page of available calendars. |
| `POST /v1/users/<id>/selection` | Selects a calendar with Google `writer` or `owner` permission. |
| `GET /v1/users/<id>/events` | Reads one page of the local event copy. |
| `GET /v1/users/<id>/event?event_id=...` | Reads one exact cached event, including its ETag and `calendar_id`; missing entries return `404`. |
| `GET /v1/users/<id>/changes` | Recent operations and their outcomes, up to 100 and 90 KiB; `truncated` indicates further results. |
| `GET /v1/users/<id>/change?idempotency_key=...` | Reads one exact operation and its original `calendar_id`; missing keys return `404`. |
| `POST /v1/users/<id>/events/create` | Queues a new event. |
| `POST /v1/users/<id>/events/update` | Queues an update with an ETag check. |
| `POST /v1/users/<id>/events/delete` | Queues a deletion with an ETag check. |
| `POST /v1/users/<id>/sync` | Requests background synchronization; responds immediately with `202`. |
| `POST /v1/users/<id>/disconnect` | Revokes consent when possible and deletes the user's local storage. |

All POSTs use `application/x-www-form-urlencoded`, with only the fields described below. Unknown or duplicate parameters are rejected. JSON objects with duplicate keys are also rejected.

- `oauth/start`: `session_binding`, the hexadecimal SHA-256 of the app's current session. Returns `authorization_url` and `expires_in: 600`.
- `oauth/callback`: `state`, `code`, `session_binding`; if consent is denied, `error` replaces `code`. The session and user must match those at the start. The state is consumed before the exchange and cannot be reused.
- `selection`: `calendar_id`, obtained from the Google list. A queue with `queued` or `unknown` operations prevents calendar changes.
- `events/create`: `idempotency_key` and `event`.
- `events/update`: `idempotency_key`, `event_id`, `etag` and `event`.
- `events/delete`: `idempotency_key`, `event_id`, `etag`.
- `sync` and `disconnect`: empty body.

`event` is a JSON object with `summary`, `start`, `end` and, optionally, `description` and `location`. For timed events, `start` and `end` contain an RFC 3339 `dateTime` with whole seconds and an explicit offset or `Z`, and may contain `timeZone`. For all-day events, they contain `date: YYYY-MM-DD`; the end date is exclusive, as in the Google API. The end must follow the start. Attendees, notifications, attachments and arbitrary private properties are not accepted; writes use `sendUpdates=none`.

Example value for the `event` field:

```json
{
  "summary": "Riunione di progetto",
  "start": {"dateTime": "2026-10-12T09:00:00+02:00", "timeZone": "Europe/Rome"},
  "end": {"dateTime": "2026-10-12T10:00:00+02:00", "timeZone": "Europe/Rome"}
}
```

`GET calendars` accepts `page_token` and `limit` from 1 to 50, with a default of 50. The response contains `items` and `next_page_token`, or `null` at the end. Only one Google request is made per page. The response has a 100 KiB limit: if an exceptionally large page exceeds the limit, the caller must reduce `limit`.

`GET events` accepts `limit` from 1 to 100, with a default of 20, and `after_id`. It returns `items`, `next_after_id`, `last_sync`, `calendar_id` and `recurrence_mode`. The effective limit may be lower to stay within 100 KiB; the cursor indicates how to continue. Optional `from` and `to` filters must be supplied together in `YYYY-MM-DD` format, with inclusive endpoints and a maximum interval of 366 days. The filter compares timestamps for timed events and dates for all-day events; the filter endpoints are UTC days.

A single synchronization allows at most 100 pages, 20,000 events, 64 KiB per event and 16 MiB of total payload. Exceeding a limit produces an error while retaining the previous cache and its token.

Recurring events are retained as **series and exceptions**, without being expanded into individual occurrences. A recurring series remains visible even when its initial event precedes the filter; deleted events remain identifiable through `status: cancelled`. This is therefore not a view of only the occurrences within the period. Creating new recurring series is outside the scope of this first version; updating a Google series ID may affect the entire series.

## Synchronization and conflicts

The worker uses a separate SQLite connection from the API. Google calls occur outside write transactions; health checks and status reads remain available during synchronization. Queued changes are processed in the background, while the default periodic polling interval is 300 seconds. Status exposes `sync_requested` and `sync_running`; `last_sync` changes only after a complete, successful cache update.

The initial and subsequent downloads follow all Google pages. The incremental token changes only together with the transaction that updates the events. If Google responds with `410`, a new complete copy is downloaded, and the previous copy remains available until the download succeeds. Rebuilding the cache does not delete the change queue.

Each operation has an idempotency key per user. Reusing it with the same content returns the previously recorded status; different content returns `409`. Accepted content is normalized before its fingerprint is calculated. Creations use a deterministic Google ID. A private marker binds the user, key and requested content. Updates and deletions send `If-Match`, and a stale ETag produces a `conflict` status without overwriting the remote version.

Schema version 2 stores each operation's target `calendar_id` when it is queued. Queue responses, exact lookups and replays retain that target after calendar reselection. Startup upgrades schema version 1 transactionally, preserving existing records and ciphertext. Legacy `queued` and `unknown` operations inherit the selected calendar as metadata, but their old encrypted payloads do not authenticate that target. They are never reencrypted from an assumed target: processing marks them `failed` with `queue_integrity_failed`, without contacting Google for a write or reconciliation. Review their original target and remote outcome before explicitly recreating legacy operations. Processing has no legacy decryption fallback. Legacy `applied`, `conflict` and `failed` operations keep an empty target because their historical calendar cannot be established safely; resolve that uncertainty before associating them with an event.

Status also exposes an opaque `connection_id`. It survives restarts, reauthorization of the same account and calendar selection, but explicit disconnection deletes it; the next connection receives a new identity. A caller can safely retry an absent operation with its immutable key and payload only after verifying the same connection and calendar. Mutation forms accept optional `expected_connection_id` and `expected_calendar_id`, checked atomically before enqueue or replay; a changed target returns `409`. A missing operation after disconnection or ledger deletion requires explicit resolution.

Before a remote write, the status is saved as `unknown`. After a shutdown, timeout or ambiguous response, the service checks the remote event before retrying. A matching marker confirms an already completed write; a different ETag requires conflict resolution. Temporary errors use increasing backoff delays; `failed` and `conflict` states require caller intervention. Exactly-once delivery is not guaranteed for every external operation.

Each new consent and calendar selection changes the connection generation: responses from a worker started before the change cannot update the new connection's cache, tokens or status.

Reconnecting the same Google account retains the calendar, cache, queue and idempotency history. A different Google account is rejected with `google_account_mismatch`: explicit disconnection is required first. Disconnecting deletes that user's local data and attempts to revoke the refresh token. A remote request already in progress may finish during disconnection; no new operations are started after the account is removed.

## Security and retention

OAuth tokens, the PKCE verifier, event payloads and change payloads are encrypted with **AES-256-GCM**, a random nonce and authenticated data that includes the user and row identity. Transplanting ciphertext between users, events or idempotency keys prevents decryption. An encrypted startup check prevents opening the storage with a different key and inadvertently resetting it.

Queued writes also authenticate the destination calendar. Before delivery or reconciliation, the operation, event ID and ETag must match the encrypted envelope; tampered routing data is rejected with `queue_integrity_failed`. Changes encrypted before this security update lack the calendar binding and are rejected as well. Before upgrading, resolve pending `queued` and `unknown` changes. After upgrading, review the remote outcome before submitting any rejected legacy change with a new idempotency key, especially when its previous delivery was uncertain.

Operational metadata remains in plaintext: user ID, Google account/calendar (often an email address), event ID, ETag, status, incremental synchronization token, idempotency key, fingerprints, counters and dates. The cryptographic key is not in the database. The service decrypts only the data of the user requested by the app, in memory; this solution does not perform computations on encrypted data.

The service uses only the designated Google destinations, HTTPS with CA and hostname verification, a minimum of TLS 1.2, no automatic redirects and a CONNECT tunnel enforced through Squid. `NO_PROXY` cannot bypass the proxy. The container has no published port, direct Internet access or app database credentials. The API token and encryption key are mounted as read-only secrets.

For backups, save the `calendar_addon_data` volume consistently with SQLite, including the WAL or using the SQLite Backup function. Keep the `calendar-token-key` key and API token **separately**. Without the key, the data cannot be recovered. Do not delete or replace the key and volume separately; future rotation requires an explicit ciphertext migration. The local secret file is an initial setup, not a KMS. The container is a single instance: MySQL cluster HA does not automatically make this storage redundant.

## Google configuration, to be completed later

1. In Google Cloud, enable Calendar API, configure the consent screen and create an OAuth client of type **Web application**.
2. Register the app's exact public redirect URI, for example `https://progetti.tuaazienda.it/api/calendar/oauth/callback`.
3. Set `GOOGLE_CLIENT_ID` and `GOOGLE_REDIRECT_URI`; place the secret in the file prepared for `GOOGLE_CLIENT_SECRET_FILE`, without putting it in the repository or chat interface.
4. Recreate the service with the complete configuration, then start the consent flow from the app for each user. Partial configurations are rejected at startup.

The scopes are `calendar.events` and `calendar.calendarlist.readonly`. Consent requires offline access and PKCE `S256`. Revoked or expired refresh tokens require new consent. Google's rules for applications in testing, authorized users and scope verification must be configured in the Google project before real use.

Main variables: `CALENDAR_BIND`, `CALENDAR_PORT`, `CALENDAR_ALLOWED_CLIENT`, `CALENDAR_TLS_CERT_FILE`, `CALENDAR_TLS_KEY_FILE`, `CALENDAR_API_TOKEN_FILE`, `CALENDAR_TOKEN_KEY_FILE`, `CALENDAR_DB_PATH`, `CALENDAR_PROXY_URL`, `CALENDAR_SYNC_INTERVAL`, `CALENDAR_HTTP_TIMEOUT`, `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET_FILE`, `GOOGLE_REDIRECT_URI`.

## Build and testing

```sh
make -C calendar-addon
python3 -m unittest discover -s tests -p 'test_calendar_addon.py' -v
```

Dependencies: a C11 compiler, pthread, OpenSSL, libcurl, SQLite, json-c and pkg-config. The Dockerfile uses a Debian image with an unprivileged user, UID/GID 10004.

Tests use local HTTPS OAuth/Calendar servers and CONNECT proxies. The `CALENDAR_GOOGLE_ORIGIN` override is allowed only with `CALENDAR_TEST_MODE=1`, a loopback listener and a loopback proxy. `CALENDAR_GOOGLE_CA_FILE` allows verification of the fixture's CA; TLS verification remains mandatory. Tests do not use Google accounts or create real events.

The service and API bridge are used by Punto's Calendar screen and the explicit synchronization of Ticket service appointments. Time entries are not created automatically.

References: [Google web server OAuth](https://developers.google.com/identity/protocols/oauth2/web-server), [incremental synchronization](https://developers.google.com/workspace/calendar/api/guides/sync), [versions and ETags](https://developers.google.com/workspace/calendar/api/guides/version-resources).

## Interface and Ticket

Punto's **Calendar** screen manages the Google connection, personal calendar selection, viewing and queue status. The Ticket app provides a service appointment agenda and explicit synchronization of events associated with a ticket and project; see the [operational workflow](../ticket-app/README.md#agenda-projects-and-google-calendar). Appointments do not automatically generate logged hours.
