# Punto Ticket — ticket management in C

**English** | [Italiano](README.it.md)

The interface supports Italian, English, Simplified Chinese, Spanish, French and German. The Language selector is available before and after sign-in; the choice remains in the browser and follows links to Punto. Shared catalogs are updated with `python3 scripts/sync_locales.py` from the project root.

The Compose service is named `tickets` and its local image is `punto/tickets:${PUNTO_IMAGE_TAG:-local}`. To update an installation using the previous names, follow the [Docker naming guide](../docs/en/tehnical/nomi-servizi-e-immagini.md); directories, volumes and network aliases retain their existing names.

Standalone web app alongside Punto. HTTP server in C11, dedicated SQLite store, HTML/CSS/JavaScript interface without npm. Accounts, sessions and tickets have dedicated storage. The optional Punto connection allows projects and assets to be associated with tickets, respecting the connected Punto session's permissions. Sign-in can use Keycloak shared with the other apps, explicitly associating the company identity with an active local Ticket account.

## Features

- Ticket creation with description, requester, priority, assignee and due date.
- **Open**, **In progress**, **Pending**, **Resolved**, **Closed** statuses, with reopening supported.
- **Low**, **Normal**, **High**, **Urgent** priorities.
- Board, list and weekly intervention agenda, with search and combinable filters.
- Scheduling with start, end and time zone, separate from the ticket's due date.
- Explicit synchronization of interventions with the personal Google calendar connected to Punto, with a persistent queue and conflict checking.
- Quick queues: **To do**, **Mine**, **Unassigned**, **Overdue** and **Due soon** within the next seven days.
- Sorting by last update, priority or due date; dates and alerts readable on mobile too.
- View and filter preferences retained per account in the browser, without saving search text.
- Direct ticket links and a **Take ownership** action to assign yourself a request and start working on it.
- Comments and change history retained alongside tickets.
- References to real projects and assets, dedicated filters and links in both directions.
- Managers and operators with personal accounts. The manager manages Ticket accounts; linked tickets follow the Punto session's access grants, including for the Ticket administrator.
- Revision checking: a concurrent change returns a conflict and requires the ticket to be reloaded.
- IMAPS reception in a separate C process, with mailboxes and authorized senders managed by administrators.
- **RAW messages** inbox, original `.eml` files, text previews, attachments and quarantine.
- Ownership, reasoned dismissal, manual conversion or linking to an existing ticket, with duplicate protection and history.

The requester is a descriptive field, not an external account. The internal queue includes RAW intake, customer conversations, internal notes, SLAs and actual hours linked to Punto. A customer portal remains outside this app. Tickets are closed without deleting history. The catalog connection uses a read-only delegation from the Punto session; company OIDC sign-in remains a separate configuration.

The initial board shows open work in the **Open**, **In progress** and **Pending** columns. The **All** queue also includes resolved and closed tickets. Each column loads twenty tickets and allows more to be shown; counts include all filtered results, including those not yet loaded. Open a card to edit status, priority, assignee and due date. Queue counters represent all tickets accessible to the session, while the view total and column counts follow the selected filters.

Operational queues exclude resolved and closed tickets. **Overdue** contains due dates before today; **Due soon** includes today and the following seven days. The interface uses the date in **Europe/Rome**. Ticket links still require an authenticated account.

## Operator workflows (items 10–20)

The **Operator** workspace places the list beside the detail, supports next/previous navigation and saves per-operator drafts on the server with revision checks. Colleague presence is temporary. Shared views store filters, including **Needs reply**, **Waiting for customer**, **SLA today**, **Unscheduled interventions** and **Overdue follow-ups**; every result and dashboard metric still uses the live Punto permissions.

Impact and urgency propose priority; an override requires a reason. Rules suggest category, responsible group and project from mailbox, sender and content, while RAW conversion remains manual. SLA agreements can target a project and priority, with business weekdays, daily hours and an IANA timezone (default Monday–Friday 09:00–18:00 Europe/Rome). Deadlines account for non-working hours, daylight-saving changes and pre-conversion RAW time. Only a sent customer reply satisfies first response; internal notes do not. Waiting requires a reason and follow-up and pauses resolution time. Real holidays and contractual agreements are not preloaded.

Bulk actions require a preview and confirmation, then recheck revisions and permissions and record history. The operational dashboard includes RAW backlog, unassigned work, SLA deadlines, forgotten follow-ups, group workload, timing and reopenings.

### Customer conversations and delivery

Customer replies have explicit recipients, subjects, drafts and delivery states, separate from internal notes. Linked RAW emails retain their originals. Editable response templates and solutions derived from resolved tickets remain reusable; ticket-derived solutions follow their source ticket permissions.

**The real mailbox is deliberately unconfigured.** Drafts work without SMTP and delivery remains unavailable. Outbound delivery requires `MAIL_API_URL`, `MAIL_API_TOKEN_FILE`, `MAIL_API_CA_FILE` and a stable dedicated `MAIL_API_NAMESPACE`, using verified TLS to Punto’s existing C mail add-on. The optional [compose.tickets-mail.yaml](../compose.tickets-mail.yaml), added after the private base and Ticket overlays, connects only the private mail API and does not configure SMTP or a real mailbox. Rebuild Ticket and the mail add-on together before enabling replies.

Confirmed replies use stable delivery keys and Message-ID, with In-Reply-To and References when available. The add-on ledger prevents duplicate delivery across restarts. Interrupted or ambiguous delivery stays **unknown** for operator verification and is never automatically resent. Changing service or namespace requires checking drafts before delivery.

### Confirmed project work hours

Hours require a separate explicit Punto delegation. The form shows the linked Punto actor; the caller cannot select an arbitrary identity. The operator confirms date, minutes and description. Calendar plans suggest context and never automatically become actual hours.

The canonical Punto timesheet flow enforces date-specific project access, closed projects/periods, daily limits and audit. Persistent idempotency keys recover interrupted outcomes without duplicate entries. Recording hours is unavailable until the current session has the hours capability.

## Native startup

Dependencies: C11 compiler, `make`, `pkg-config`, SQLite, OpenSSL, json-c, libcurl and GMime 3. On macOS they can be installed with `brew install pkg-config openssl@3 json-c sqlite curl gmime`; on Debian with `apt install build-essential pkg-config libssl-dev libsqlite3-dev libjson-c-dev libcurl4-openssl-dev libgmime-3.0-dev`. The build produces the HTTP app and `build/punto-ticket-receiver`.

From the repository root:

```sh
make tickets
cd ticket-app
export TICKETS_EMAIL='gestore@example.test'
export TICKETS_NAME='Gestore'
read -rs 'TICKETS_PASSWORD?Password iniziale (almeno 12 caratteri): '
export TICKETS_PASSWORD
./build/punto-tickets
```

The `read` command shown is for zsh, the default macOS shell. In bash, use `read -rs -p 'Password iniziale: ' TICKETS_PASSWORD` and then `export TICKETS_PASSWORD`.

Open [http://localhost:8083](http://localhost:8083) and sign in with the credentials you chose. The server listens on loopback; `Ctrl+C` stops it. The default database is `ticket-app/data/tickets.db` when started from the `ticket-app` directory. Initial startup requires an email and a password of at least 12 characters; on an already initialized database, these variables do not change accounts. Passwords are updated through user management.

To start again from the root:

```sh
make run-tickets
```

## Standalone Docker

```sh
cp ticket-app/.env.example ticket-app/.env
```

Fill in `TICKETS_EMAIL` and `TICKETS_PASSWORD` in the local file, then:

```sh
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml up --build -d
docker compose --env-file ticket-app/.env -f ticket-app/compose.yaml ps
```

Open [http://localhost:8083](http://localhost:8083). The port is published only on `127.0.0.1`. The `tickets_data` volume retains SQLite; the app runs with UID/GID `10006`, a read-only filesystem and no dependency on Punto's databases. Standalone mode uses a standard bridge network to allow publication of the local port; the container can reach the external network. The overlay alongside Punto instead uses the stack's internal private network.

`TICKETS_HOST_PORT` changes the host port: also update `TICKETS_PUBLIC_ORIGIN`, for example `http://localhost:8183`. The origin is exact, without a trailing slash. When set, use the configured name in the browser.

## Running alongside the Punto stack and Nginx Proxy Manager

The optional [compose.tickets.yaml](../compose.tickets.yaml) overlay adds the `tickets` service to the private network already used by NPM, at `172.30.20.11:8083`, without publishing host ports or connecting it to database networks. The Punto stack must already be configured and running according to the [main README](../README.md).

1. Prepare `ticket-app/.env` as above and set `TICKETS_PUBLIC_ORIGIN` to the dedicated public origin, for example `https://ticket.tuaazienda.it`.
2. Start only the new service from the repository root:

   ```sh
   docker compose --env-file .env.docker --env-file ticket-app/.env \
     -f compose.yaml -f compose.tickets.yaml up --build -d tickets
   ```

3. In Nginx Proxy Manager, create a **Proxy Host** for the chosen name: scheme `http`, destination `ticket-app` (an alias of the `tickets` service), port `8083`. Preserve the browser's Host header, as in the standard NPM configuration. Configure the domain certificate and HTTPS forwarding consistently with the chosen origin.
4. Open the configured origin. To check status, use the same Compose files with `ps tickets`.

To configure multiple addresses, separate sessions and repeatable NPM rules, see
the [Ticket publishing guide](../docs/en/tehnical/ticket-reverse-proxy.md).
`TICKETS_ADDITIONAL_ORIGINS` allows up to three additional exact origins, separated
by commas without spaces. Each Host must have only one scheme; HTTP ingress
for an HTTPS site is redirected by Nginx. Company sign-in remains
available only on the origin of the configured OIDC redirect URI.

The two Compose files describe alternative modes. The standalone mode and the mode alongside Punto use potentially different project names and actual volumes: do not switch between them expecting an automatic import. To move data, stop the source app and explicitly back up and restore the store.

## Connecting to projects and assets

Set `TICKETS_APP_ORIGIN` on Punto to Ticket's public origin and `TICKETS_PUNTO_ORIGIN` on Ticket to Punto's, without a trailing slash. Both must use HTTPS in production. The `compose.tickets.yaml` overlay passes the first variable to Punto and the connector variables to Ticket. With NPM on the private network, also set `TICKETS_PUNTO_RESOLVE_IP=172.30.20.2`: the client reaches NPM directly while preserving Host, SNI and verification of the public domain's certificate. Public CAs are already installed in the image. For a private CA, mount the PEM read-only with a Compose override and set `TICKETS_PUNTO_CA_FILE` in the Ticket service.

After the change, rebuild and recreate **both** services, using the stack's same files and secrets:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  -f compose.yaml -f compose.tickets.yaml up --build -d app tickets
```

For a native test: Punto at `http://127.0.0.1:8080` with `TICKETS_APP_ORIGIN=http://127.0.0.1:8083`; Ticket with `TICKETS_PUNTO_ORIGIN=http://127.0.0.1:8080`. HTTP is allowed only for loopback origins. Do not use `127.0.0.1` to connect two containers.

1. Sign in to Ticket and press **Connect Punto**. The browser opens Punto and requests sign-in if needed.
2. On returning to Ticket, the panel shows the connected Punto account's name. The ticket form lets you search for and select a project and/or asset. No ownership relationship is created between the project and asset.
3. **Ticket** and **+ Ticket** in the project row, or **Linked tickets** and **New ticket** in the asset record, open the filtered queue or a form with the context already set. In ticket details, references link back to the project or asset.

Without a connected Punto session, only tickets without references are visible. With the connection active, tickets for assets and projects viewable by the Punto account are visible. Permissions are checked on the server on every request, including details, comments, changes and counters. Accounts are not associated by email or ID. Catalog delegation is read-only; calendar and hours each require separate explicit consent. Delegation lasts at most eight hours, remains in memory and is revoked by Punto session logout/expiry or by either service restarting; repeat **Connect Punto**. **Disconnect** removes it from the current Ticket session.

The server resolves IDs and names from Punto's catalogs through its read APIs; it does not access the main databases or receive the inventory's private token. Codes and names are retained in the ticket as historical references. Editing other fields does not replace these references. If Punto is unavailable, reading linked tickets fails without broadening access; unlinked tickets remain usable. An unavailable asset does not prevent retaining a previously saved link.

SQLite migration from version 1 to 2 adds nullable references and indexes in a transaction; existing tickets remain unlinked. Perform the backup described below before upgrading. Restoring an earlier executable version also requires restoring its corresponding database.

## Agenda, projects and Google Calendar

In ticket details, set **Intervention start** and **Intervention end** and save. The due date remains a separate date: it indicates when the request must be resolved, while the intervention occupies an interval in the agenda. Times are saved as UTC timestamps and displayed in the time zone specified by the form; check the time zone before confirming.

The **Agenda** view shows the week and allows navigation with **Today**, previous and next. It retains the status, priority, assignee, project and asset filters and applies the board's permissions. It automatically loads all available pages, including very busy weeks. Interventions remain usable without Google. From Punto's project row, **Agenda** opens that project's interventions directly; from the **Calendar** section, you can select the project and open the Ticket agenda.

For Google:

1. The administrator configures the Calendar service and OAuth client according to the [Calendar manual](../calendar-addon/README.md). The example configuration leaves Google disabled; no Google secrets are required in the Ticket app.
2. In Punto, open **Calendar**, choose **Connect Google Calendar** and complete consent in your Google account. Select a writable calendar with **Use this calendar**. **Sync now** requests a refresh; the screen shows the last completion and pending operations.
3. In the ticket, choose **Connect calendar** and authorize the calendar delegation for the current session in Punto. The regular **Connect Punto** retains read-only catalog access.
4. Save the intervention and choose the Google synchronization action in the ticket. The server composes the event with the ticket and project references. **Pending** means the operation is queued, not that Google has already applied it. Refresh status to check the outcome.

The calendar belongs to the connected Punto account, even when the ticket is assigned to someone else. Other operators can view the internal intervention if authorized for the project; the manager role does not grant access to another user's Google calendar. The connection retains owner, calendar, event and operation: changing account or calendar does not automatically move an existing event.

Local changes require another explicit synchronization. Changes made directly in Google do not automatically overwrite the internal intervention: refresh status and resolve any differences in the ticket. An ETag conflict prevents overwriting an event modified elsewhere. On a timeout, the server retains the same operation to check its outcome without inventing a new creation. Removal from Google retains the link until deletion is confirmed; internal scheduling is managed separately.

Punto lists personal Google events, with pagination and separate recurring series; it does not automatically expand every occurrence. The integration does not generate worked hours, invitations or participant notifications. Before disconnecting Google, resolve pending operations: disconnecting deletes the local cache and queue, while events already created may remain in Google.

The current SQLite schema is version 5 and preserves tickets, references, scheduling and synchronization metadata, alongside RAW, operations and communications. The historical version 3 migration introduced scheduling and calendar data; subsequent upgrades preserve them. Back up before upgrading. Session delegations remain temporary and are not included in the backup; Punto must be reconnected after a restart. The Calendar volume and its encryption key require a separate backup from the Ticket store.

## Incoming email and RAW messages

The workflow is **dedicated IMAP mailbox → sender verification → RAW messages → operator review → new ticket or link to an existing ticket**. Receiving email does not automatically create tickets or send replies. Real reception remains disabled until a mailbox and the receiver process are configured and enabled.

In **RAW messages**, administrators open **Mailboxes and authorized senders** and enter the destination address, label, IMAPS host and port, username, folder, authentication method (`password` or `xoauth2`) and credential filename. Inactive mailboxes may be saved and completed later. Never enter passwords or tokens in app fields: the filename identifies a file inside `TICKETS_IMAP_SECRET_DIR`. The receiver reads it on the server; secrets are not returned by the API, stored in SQLite or logged. OAuth2 tokens need external renewal; the receiver does not perform consent or automatically refresh tokens.

Each mailbox has an exact sender allowlist, optionally with expiry and project binding. Wildcards and implicit authorization of an entire domain are excluded. A mailbox or sender bound to a project restricts visibility and conversion to that project; the operator needs an authorized Punto session. Incoming authorization does not create a Ticket account.

Configure the exact authentication server identifier and confirm trust only when the provider removes forged external `Authentication-Results` headers and adds its own result. The allowlist alone does not prevent a forged `From` field. Reception requires one authorized author and a valid DMARC result aligned with the author domain; domain checks alone do not authenticate an individual person. Missing, ambiguous, untrusted or failed results and unauthorized senders produce administrator-only quarantine that cannot be converted. Provider setup must respect the trust boundary described in [RFC 8601](https://www.rfc-editor.org/rfc/rfc8601.html).

The receiver is a separate process with no HTTP listener or SMTP server. It uses IMAPS with certificate and hostname verification and does not delete remote messages. Poll status, UID cursor and IMAP generation are persisted. Downloads and restarts do not create another RAW origin; `Message-ID` alone is not considered proof of uniqueness.

### Starting the receiver

Prepare a protected directory containing credential files with the names entered in mailbox settings. In Docker these files must be readable by UID `10006`; avoid access by other users and keep them out of the repository. Set `TICKETS_IMAP_SECRETS_PATH` to the absolute directory path. The overlay mounts it read-only and reuses the installation's `tickets_data` volume.

For standalone Ticket, from the repository root:

```sh
docker compose --env-file ticket-app/.env \
  -f ticket-app/compose.yaml -f ticket-app/compose.inbound.yaml \
  up --build -d tickets tickets-receiver
```

For the private Punto stack, first replace the placeholder in `docker/squid/allowed-imap-domains.txt` with the exact authorized public IMAP hostname. The proxy permits receiver `172.30.40.13` only to that list on port `993`, retaining blocks on private destinations, IP literals and other ports. Apply the proxy configuration when enabling reception:

```sh
docker compose --env-file .env.docker --env-file ticket-app/.env \
  -f compose.yaml -f compose.tickets.yaml -f compose.tickets-inbound.yaml \
  up --build -d proxy-egress tickets tickets-receiver
```

For native operation, start the app first to migrate storage, then run the receiver from `ticket-app` with the same `TICKETS_DB_PATH` and the protected `TICKETS_IMAP_SECRET_DIR`. `TICKETS_IMAP_POLL_SECONDS` defaults to 30; `TICKETS_IMAP_CA_FILE` supports a verified private CA. Optional `TICKETS_IMAP_PROXY_URL` selects an explicit HTTP proxy for TLS tunnels. `--once` performs one poll; local `--import-file` imports are unverified and quarantined, never a bypass of provider authentication.

### Operator workflow

States are **New**, **Under review**, **Converted**, **Linked**, **Dismissed**, and administrator-only **Quarantine**. Open a message for its text preview, verification results, attachments and history. **Take ownership** reserves it for the operator; **Release** makes it available again. Dismissal requires a reason and preserves the original.

**Convert to ticket** proposes title, description and requester, leaving priority, assignee, due date, project, asset and selected attachments to the operator. **Link to ticket** searches existing requests and requires confirmation. Ticket creation or linking, selected attachments and history are committed together. Retries and concurrent conversions cannot create a second ticket. Permissions and current sender authorization are checked again at confirmation.

The immutable original is downloadable as `.eml`. The app displays text without executing incoming HTML or loading remote images. Limits: 10 MiB original, at most 20 attachments, 5 MiB per attachment and 10 MiB decoded total. Downloads are authenticated and forced as attachments. No antivirus is included; an available file is not certified harmless. The parser also limits MIME structure to 128 parts and 16 levels and text bodies to 64 KiB; parsing errors and limit violations go to quarantine with the original preserved within its size limit. If the original exceeds 10 MiB, the receiver interrupts the download and records a mailbox error without advancing past that UID: move or reduce the message at the provider before reception for that mailbox can resume. Other mailboxes continue to be processed.

Unbound messages are shared with the internal team; project-bound messages follow Punto permissions. Converted or linked RAW messages also require access to their destination ticket. Selected files are available from the ticket; unselected files remain with the original. Originals and history currently have no automatic expiry: size and protect storage and define operational retention before real use.

Customer correspondence requires current access to every linked RAW source because replies may quote their contents. If any source becomes private or access is revoked, conversation, draft and send endpoints return 404. Shared local tickets and internal notes remain available; tickets without RAW sources follow their own permissions.

SQLite upgrades to version 5 without removing existing tickets or links. App and receiver must use the same store and schema version. Stop `tickets-receiver` too for an offline backup; the database contains original emails and attachments, while credential files require separate safekeeping.

## Configuration

| Variable | Native default | Use |
| --- | --- | --- |
| `TICKETS_BIND` | `127.0.0.1` | Numeric listening IP address |
| `TICKETS_PORT` | `8083` | Internal HTTP port |
| `TICKETS_PUBLIC_DIR` | `public` | Interface directory, relative to the startup directory |
| `TICKETS_DB_PATH` | `data/tickets.db` | Dedicated database; the directory is created if needed |
| `TICKETS_PUBLIC_ORIGIN` | Loopback origins on the configured port | Exact authorized HTTP/HTTPS origin, without a path |
| `TICKETS_EMAIL` | None | First manager's email |
| `TICKETS_PASSWORD` | None | First manager's password, at least 12 characters |
| `TICKETS_NAME` | `Gestore` | First manager's name |
| `TICKETS_PUNTO_ORIGIN` | Disabled | Punto's HTTPS origin; HTTP allowed only on loopback |
| `TICKETS_PUNTO_RESOLVE_IP` | DNS resolution | Optional fixed IPv4 to reach the HTTPS origin while retaining TLS verification |
| `TICKETS_PUNTO_CA_FILE` | System CAs | Optional private PEM CA, readable by the process |
| `AUTH_MODE` | `local` | `local`, `hybrid` (local + Keycloak), `oidc` (Keycloak only) |
| `OIDC_ISSUER_URL` | None | Exact public issuer for the Keycloak realm, for example `https://auth.example.it/realms/punto` |
| `OIDC_BACKCHANNEL_URL` | Public issuer | Realm URL reachable by the server for tokens and keys; must correspond to the same realm |
| `OIDC_CLIENT_ID` | None | OIDC client dedicated to Ticket |
| `OIDC_CLIENT_SECRET_FILE` | None | Client secret file, readable by the Ticket process |
| `OIDC_REDIRECT_URI` | None | Exact public URL of `/auth/callback` on Ticket |
| `OIDC_BINDINGS_FILE` | None | JSON file associating Keycloak subjects with local Ticket emails |
| `OIDC_CA_FILE` | System CAs | PEM CA for HTTPS connections to the provider |
| `OIDC_ALLOW_HTTP` | `0` | Explicitly enables HTTP only for local tests |

Passwords are stored as PBKDF2 derivations with a random salt. Sessions reside in memory and expire after eight hours; those created through Keycloak are also limited by the verified token's lifetime. A restart requires signing in again. The browser receives HttpOnly and SameSite=Strict cookies; Secure is added for HTTPS origins. Writes require the CSRF token returned by the session. The server checks Host and Origin, uses prepared queries and records tickets and history in the same transaction.

In `hybrid` and `oidc`, the browser opens Keycloak sign-in and returns to the callback with a single-use code; tokens and secrets remain on the server. The transaction uses state, nonce, PKCE and a temporary HttpOnly SameSite=Lax cookie. The shared module verifies the token and resolves the subject through the bindings file; Ticket then requires the local account to be active. The `admin`/`agent` roles remain those in the Ticket store. The email declared in the token does not automatically create or link accounts. Incomplete configuration prevents startup before the database is opened.

The `OIDC_BINDINGS_FILE` file contains a JSON object, for example `{"subject-keycloak-del-gestore":"gestore@example.test"}`. Use the user's stable `sub` value in the configured realm, not their Keycloak username or email. The file is app-specific: the same identity can be associated with a different account in Punto. Retain and mount both this file and the client secret read-only; a change is applied by restarting Ticket.

For multi-app Docker, use the Keycloak overlays and the `KEYCLOAK_*` configuration generated by `scripts/configure_keycloak.py`. The Ticket overlay uses the TLS backchannel `https://keycloak:8443/realms/punto` and mounts `/run/punto-auth/client-secret`, `/run/punto-auth/bindings.json` and `/run/punto-auth/ca.crt`. The variables in the table are those read by the process; for native startup, export them with absolute host paths. Standalone Ticket Compose retains local sign-in.

Prepare the first account and other users as before, without changing IDs or importing credentials from Punto. In `oidc`, the local form is hidden and `POST /api/login` returns `403`; local passwords retained in the store cannot be used while this mode remains active. The **Sign out** button closes only the Ticket session: the Keycloak session and other apps remain active, so another company sign-in can be immediate.

The native server speaks HTTP. Public TLS is terminated by the reverse proxy. For remote exposure, configure a dedicated HTTPS origin and its proxy.

## API

Added operator endpoints use JSON, session authentication and CSRF like other Ticket endpoints:

| Method and path | Purpose |
| --- | --- |
| `POST /api/operations/suggest`, `GET /api/raw/messages/:id/suggestions` | Classification suggestions; no automatic conversion |
| `GET/POST /api/operations/rules`, `PUT /api/operations/rules/:id` | Revisioned rules; administrator writes |
| `GET/POST /api/operations/sla-policies`, `PUT /api/operations/sla-policies/:id` | SLA agreements and business hours |
| `GET/POST /api/operations/views`, `PUT /api/operations/views/:id` | Shared views; owner or administrator updates |
| `GET/POST /api/tickets/:id/presence`, `PUT /api/tickets/:id/draft`, `POST /api/tickets/:id/draft/discard` | Temporary presence and revisioned personal drafts |
| `POST /api/tickets/bulk/preview`, `POST /api/tickets/bulk/apply` | Session-bound expiring preview, confirmation and replay |
| `GET /api/reports/operations` | Dashboard scoped by current permissions |
| `GET /api/tickets/:id/conversation`, `POST /api/tickets/:id/messages`, `PUT /api/tickets/:id/messages/:message` | Conversation and customer drafts |
| `POST /api/tickets/:id/messages/:message/send` | Explicit delivery of the confirmed revision; configured SMTP required |
| `GET/POST /api/tickets/:id/time` | Proposed context and confirmed work hours with persistent keys |
| `GET/POST /api/response-templates`, `PUT /api/response-templates/:id` | Revisioned response templates |
| `GET /api/solutions`, `POST /api/tickets/:id/solution` | Resolved-ticket solutions following source permissions |

The Punto bridge adds explicit `time=1` delegation consent and `scope.time_access`. Bearer routes `GET /api/integrations/tickets/time/status`, `GET .../time/entries?job_id=N&request_key=K` and `POST .../time/create` are separate from calendar delegation. Creation accepts form fields `request_key` and JSON `entries` containing one row (`job_id,date,minutes,note`), rejects caller-selected identities and checks live project ACL on replays. Keys are namespaced with `ticket-` in the canonical timesheet ledger.

Ticket time history belongs to the currently connected Punto account: it requires live time consent, the same Punto origin and current access to each entry’s project. Reading a ticket does not grant access to other accounts’ hours. Only the original Ticket author can resume a pending or uncertain attempt with its original key, for the same Punto account and project; this requires another confirmation and does not automatically create a new entry.


APIs return JSON. Errors have the form `{"ok":false,"error":"messaggio"}`. Sign-in and creation use `POST`; updates use `PUT`. For authenticated mutations, send `Content-Type: application/json`, the session cookie and `X-CSRF-Token`.

| Method and path | Content / result |
| --- | --- |
| `GET /healthz` | Service health, without authentication |
| `GET /api/auth/config` | `{sso,local}`; public sign-in modes, without secrets |
| `GET /auth/login` | Starts company sign-in with HTTP `303` redirect and temporary cookie |
| `GET /auth/callback` | Verifies the OIDC code, creates a Ticket session and redirects to `/` |
| `POST /api/login` | `{email,password}` → `{ok,user,csrf_token}` and cookie; `403` in `oidc` mode |
| `GET /api/session` | User and CSRF token; `401` without a session |
| `POST /api/logout` | Ends the current session |
| `GET /api/users` | `{items}`; operators see active accounts |
| `POST /api/users` | Managers only: `{name,email,password,role}` → `{user}` |
| `PUT /api/users/:id` | Managers only: optional `name`, `role`, `active`, `password` fields |
| `GET /api/integrations/punto/status` | `{configured,connected,user_name,expires_at,origin}` for the current session |
| `POST /api/integrations/punto/start` | `{}` → `{state,url}` to open Punto |
| `POST /api/integrations/punto/connect` | `{state,grant}` → status; requires the pending state from the same session |
| `POST /api/integrations/punto/disconnect` | `{}` → disconnected status |
| `GET /api/integrations/punto/jobs` | Catalog of authorized projects; `search`, `limit`, `offset` |
| `GET /api/integrations/punto/assets` | Asset catalog; `q`, `limit`, `after_id` |
| `GET /api/integrations/punto/validate` | `job_id` and/or `asset_id` → canonical `{job,asset}` |
| `GET /api/tickets` | `{items,total,stats,queues,reference_date}`; parameters `search`, `status`, `priority`, `assignee_id`, `job_id`, `asset_id`, `queue`, `sort`, `today`, `limit`, `offset` |
| `POST /api/tickets` | `{title,description,requester,priority,assignee_id,due_date,job_id,asset_id}` → `{ticket}` |
| `GET /api/tickets/:id` | `{ticket,comments,history,raw_sources,attachments}` |
| `PUT /api/tickets/:id` | Ticket fields, optional `status` and required `revision` → `{ticket}` |
| `POST /api/tickets/:id/comments` | `{body}` → `{comment}` |
| `GET /api/raw/messages` | Queue `{items,total,stats,mailboxes}`; filters `state`, `search`, `mailbox_id`, `limit`, `offset` |
| `GET /api/raw/messages/:id` | `{raw,attachments,history,eml_url}` with permission checks |
| `GET /api/raw/messages/:id/eml` | Immutable original, authenticated download |
| `GET /api/raw/messages/:id/attachments/:aid` | RAW attachment, authenticated download |
| `POST /api/raw/messages/:id/claim` | `{revision}` → claim message |
| `POST /api/raw/messages/:id/release` | `{revision}` → return to queue |
| `POST /api/raw/messages/:id/discard` | `{revision,reason}` → discard with reason |
| `POST /api/raw/messages/:id/convert` | `{revision,ticket,attachment_ids}` → `{ticket,raw,replayed}` |
| `POST /api/raw/messages/:id/link` | `{revision,ticket_id,attachment_ids}` → `{ticket,raw,replayed}` |
| `GET /api/tickets/:id/attachments/:aid` | Selected attachments only, with ticket and RAW permissions |
| `GET /api/raw/mailboxes`, `GET /api/raw/senders` | Admin configuration lists; senders filterable by `mailbox_id` |
| `POST /api/raw/mailboxes`, `POST /api/raw/senders` | Admin configuration creation without secrets |
| `PUT /api/raw/mailboxes/:id`, `PUT /api/raw/senders/:id` | Admin update with required `revision` |

Roles: `admin`, `agent`. Statuses: `open`, `in_progress`, `waiting`, `resolved`, `closed`. Priorities: `low`, `normal`, `high`, `urgent`. The due date is a valid `YYYY-MM-DD` date or `null`; the assignee is an active account's ID or `null`. The `assignee_id=unassigned` filter selects unassigned tickets. The `stats` and `queues` counters refer to all tickets visible to the account, regardless of filters. The revision protects ticket updates; a comment updates the last activity date without changing the revision.

`job_id` and `asset_id` are positive integer IDs or `null` in writes; in filters they are positive IDs. Names are not accepted from the browser: the server adds `job_code`, `job_name`, `asset_tag`, `asset_name` from the catalogs. Search includes these references. An update omitting IDs retains existing references; `null` removes them. Catalog responses contain only ID, code/tag, name, status and asset type; no inventory credentials.

Punto exposes `POST /api/integrations/tickets/grant` to its authenticated session, with a 64-character lowercase hexadecimal `state` form field and origin verification. Reads at `/api/integrations/tickets/{scope,jobs,assets,validate}` require the opaque Bearer delegation, never a user ID chosen by the client. None of these APIs modifies projects or assets.

`queue` accepts `all` (default), `active`, `mine`, `unassigned`, `overdue` and `due_soon`. All queues except `all` include only active statuses. `mine` uses the session's user. Filters combine with AND. `today` is a valid `YYYY-MM-DD` date: the browser sends the date in Italy; if omitted, the API uses the UTC day and returns it in `reference_date`. `due_soon` includes both endpoints from `today` to `today + 7 days`.

`sort` accepts `updated` (default, newest first), `priority` (urgent, high, normal, low, then due date) and `due` (nearest due dates, then priority). Missing due dates follow present ones; descending update time and ID break ties. `limit` ranges from 1 to 100, `offset` from 0 to 1,000,000. Unknown, duplicate or invalid parameters return `400`.

The additional `operations_queue` filter accepts `all`, `needs_reply`, `waiting_customer`, `sla_today`, `unscheduled` and `follow_up_due`. It combines with other filters and is stored in shared views. Operational queues include active tickets only; **SLA today** uses each ticket agreement's timezone. Internal notes and drafts do not count as customer replies.

## Verification and backup

```sh
make test-tickets
```

Tests compile the servers and use temporary databases, synthetic credentials and loopback ports, without writing to Punto's data. They cover workflows, permissions, comments, history, revisions, persistence, operational queues, due dates, sorting, HTTP checks, schema migration and catalog connections with permission revocation and source unavailability. RAW tests cover claiming, concurrent conversion, linking, attachment selection and current ACLs. Receiver tests use real synthetic IMAPS TLS servers, including interrupted fetches, UIDs, MIME, authentication, password/XOAUTH2 and CONNECT tunnels. No real mailbox is required.

The `tests/test_tickets_oidc.py` suite uses a local OIDC provider with real RSA signatures; it requires no external Keycloak. It checks explicit account linking, local roles, CSRF, session duration and revocation, callback reuse, `oidc` mode, disabled accounts, unknown subjects and invalid signatures.

The operational suites cover the priority matrix and override reasons, waits and follow-ups, business hours and daylight-saving changes, SLA snapshots, permission-filtered rules, drafts and presence, bulk previews and revisions, reports, templates and solutions. Correspondence tests use a synthetic HTTPS service to verify threading, revisions, uncertain outcomes and deduplication. The time bridge is also tested against real Punto with a temporary database, revoked permissions, separate calendar consent and idempotent recovery; no test needs a real mailbox. To verify the addon's SMTP headers too, run `python3 -m unittest discover -s tests -p 'test_mail_addon.py' -v`.

For the simplest backup, stop `tickets` and `tickets-receiver` if enabled, copy the entire data directory or dedicated volume, then restart the services. For hot backups, use the SQLite Backup API or SQLite's `.backup` command: copying only the `.db` file while the process writes can lose data in the WAL. Retain backups in a protected location because they contain tickets, comments, accounts, original emails and attachments.
