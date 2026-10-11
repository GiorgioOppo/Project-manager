# Punto email addon

**English** | [Italiano](README.it.md)

The Compose service is named `mail`, and its local image is `punto/mail:${PUNTO_IMAGE_TAG:-local}`. To update an installation using the previous names, follow the [Docker naming guide](../docs/en/tehnical/nomi-servizi-e-immagini.md); directories, volumes and TLS identities retain their existing names.

A C microservice that receives HTTPS requests from the app and sends text messages through SMTP with TLS. It uses OpenSSL, libcurl and SQLite; `make -C addon` builds the standalone `addon/build/punto-mailer` executable. The [Dockerfile](Dockerfile) produces the `mail` Compose service, which runs as an unprivileged user with a read-only filesystem and a dedicated volume for its delivery ledger.

**SMTP is not yet configured.** The service operates as an API but does not send real emails. Internal notifications continue to work; alert emails remain in `alert_email_outbox` with `pending_configuration` status.

## Private API

The app uses `https://mail-api:8090`. The `mail-api` name is an alias present only on the internal `mail_api` network; the certificate includes this name. The client verifies the CA, server name and TLS, without following redirects or going through Squid for this internal call.

The `/v1/*` APIs require `Authorization: Bearer <token>` and a source authorized by `MAIL_ALLOWED_CLIENT`, configured in Compose as the app's `172.30.60.10` address. The shared token is read from `MAIL_API_TOKEN_FILE`; do not put it in URLs, logs, source files or visible test commands. `/healthz` requires no authentication and verifies that the process responds; it does not certify that the SMTP provider is reachable.

| Method and path | Result |
| --- | --- |
| `GET /healthz` | `200 {"status":"ok"}` |
| `GET /v1/status` | `200`, with `smtp_configured`, `transport: "smtp_tls"` and `idempotency: true` |
| `POST /v1/emails` | Sends a message or retrieves the result already recorded for the same key |

`smtp_configured` indicates that the required SMTP configuration is present; it does not make a test connection to the provider.

The POST accepts `Content-Type: application/x-www-form-urlencoded`, including with `charset=UTF-8`, and these four required fields:

| Field | Content |
| --- | --- |
| `idempotency_key` | 1 to 128 ASCII characters: letters, numbers, `_`, `.`, `:`, `-` |
| `to` | A single email address, up to 254 bytes |
| `subject` | Nonempty UTF-8 text, up to 250 bytes, without control characters |
| `body` | Nonempty UTF-8 text, up to 8192 bytes; line breaks and tabs are allowed |

Unknown or duplicate fields, multiple addresses and invalid content are rejected. The encoded HTTP body cannot exceed 32768 bytes. The service generates `text/plain` messages; attachments, HTML and multiple sends in the same request are not implemented.

| HTTP | Relevant response | App behavior |
| --- | --- | --- |
| `200` | `status: "sent"`, key and `replayed` | Stores `sent` and stops retrying |
| `503` | `error: "smtp_not_configured"`, `status: "pending_configuration"` | Keeps the request pending; no key is consumed |
| `409` | `error: "delivery_unknown"`, `status: "unknown"` | Stores `delivery_unknown`; requires manual verification |
| `502` | `error: "smtp_delivery_failed"`, `status: "failed"` | Stores `delivery_failed`; requires manual verification |
| `409` | `error: "idempotency_conflict"` | Blocks the request with `delivery_failed`; requires manual verification |
| `503` | `error: "ledger_unavailable"` | Retries later with the same key |
| `400`, `401`, `403`, `405`, `413`, `415` | Invalid request, authentication, source, method, size or format | Correct the configuration or caller |

A network error or an unrecognized response leaves the item eligible for retry (`retrying`), retaining its key and content. The app checks SMTP status about once a minute and processes at most one email per scheduler cycle, approximately every 30 seconds. Before sending, it rechecks that the manager is active and the address is still among the rule's recipients. Network calls take place outside the MySQL transaction; the outbox update and its audit event are saved together in a subsequent transaction.

`sent` means that **the SMTP server has accepted the message**. It does not confirm delivery to the mailbox, the absence of bounces or that the recipient has read it.

## Idempotency and recovery

The persistent ledger is `/var/lib/punto-mailer/delivery.sqlite3`, in the `mail_addon_data` volume. The `deliveries` table stores the key, SHA-256 content hash, status and timestamps; it does not store the recipient, subject or body in plaintext. Those contents remain in the application's outbox. The ledger uses WAL and `FULL` synchronization.

Before contacting SMTP, the service records the `sending` status and commits it to disk. A repeated request with the same key and content returns the stored result without sending again; different content returns `idempotency_conflict`. On restart, deliveries left in `sending` become `unknown`, because the provider may already have accepted the message. Both `failed` and `unknown` are also terminal for that key.

The app generates keys in the form `MAIL_API_NAMESPACE:outbox:ID`. **Keep `MAIL_API_NAMESPACE` stable** together with the application database and the addon's ledger. Do not change the namespace to force a retry or delete the ledger to resolve an error: either operation can resend emails that have already been accepted.

The MySQL backup alone does not include this volume. For a consistent backup, suspend all writes and sends, stop the app and addon, save MySQL and a consistent copy of the SQLite ledger, then restore the outbox, ledger and namespace together. For SQLite, use a consistent backup or, with the service stopped, preserve the entire volume directory, including any WAL files. Also protect tokens, certificates and recovery credentials. A new installation with a new database and ledger must use a distinct namespace; do not reuse historical keys.

The `unknown` and `failed` outcomes require checking the provider's log and making an explicit decision before any new send. There is no automatic command to resend terminal outcomes.

## Configuring SMTP later

Set the provider details in `.env.docker`:

| Variable | Purpose |
| --- | --- |
| `MAIL_SMTP_HOST` | Exact SMTP server hostname |
| `MAIL_SMTP_PORT` | `587` for STARTTLS or `465` for immediate TLS |
| `MAIL_SMTP_MODE` | `starttls` or `smtps` |
| `MAIL_SMTP_FROM` | Sender address authorized by the provider |
| `MAIL_SMTP_USER` | SMTP username, if the provider requires authentication |

Put the password only in the local `data/addon-secrets/smtp-password` file, mounted as the `/run/secrets/mail_smtp_password` secret. Do not put it in `.env.docker` or the repository. `configure_network.py prepare` creates missing secrets without printing them and preserves existing ones; the host directory has `0700` permissions, while mounted files must be readable by the containers' unprivileged users and not writable by group or others.

Replace the placeholder in [allowed-smtp-domains.txt](../docker/squid/allowed-smtp-domains.txt) with only the authorized SMTP hostname, without a scheme, port, path, wildcard or leading dot. The SMTP list is separate from the app's Google Calendar endpoints. For the addon, Squid permits only `CONNECT` tunnels to listed SMTP hosts on ports `465` and `587`; IP addresses and private or reserved destinations remain blocked. Plaintext SMTP on port 25 is not allowed. SMTP certificates are verified against the system CAs and the provider's name; transport requires TLS.

After preparing the configuration, secret and allowlist, recreate both services from the project root so they reread the environment and mounted files:

```sh
docker compose --env-file .env.docker up -d --no-deps --force-recreate --wait proxy-egress mail
```

The app does not need restarting to detect the updated SMTP status. Once SMTP is configured, the app may also process previously queued alerts: check outbox recipients and content before enabling a real provider.

The addon has only the `mail_api` and `app_egress` interfaces, both internal. The API listens only on the `mail_api` address; it publishes no host ports and is not connected to `public`, `private_app` or `backend_app`. Outbound SMTP goes through `MAIL_PROXY_URL=http://squid:3128`; only Squid has the `proxy_wan` network. The addon receives no MySQL or MongoDB credentials.

For standalone execution, the program's other settings are `MAIL_BIND`, `MAIL_PORT`, `MAIL_ALLOWED_CLIENT`, `MAIL_TLS_CERT_FILE`, `MAIL_TLS_KEY_FILE`, `MAIL_LEDGER_PATH`, `MAIL_API_TOKEN_FILE`, `MAIL_SMTP_PASSWORD_FILE`, `MAIL_PROXY_URL`, `MAIL_SMTP_TIMEOUT` (1–60 seconds, default 20) and the optional `MAIL_SMTP_CA_FILE`. The last setting allows a dedicated SMTP CA, but requires an additional mount and Compose configuration; it is not set in the current stack. Partial SMTP configuration, missing TLS files or a mismatched API TLS private key and certificate prevent startup.
