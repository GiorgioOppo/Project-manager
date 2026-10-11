# Application API

[Italiano](../../tehnical/api-app.md) | **English**

Contract verified against the code on **11 October 2026**, data schema 10. APIs use the `/api` prefix on the app's public origin, for example `https://progetti.tuaazienda.it`. The names and methods below are those implemented: updates use `POST`, not `PUT` or `PATCH`.

Sources: [HTTP router](../../../src/server.c), [users](../../../src/accounts.c), [projects, time, reports, rates](../../../src/workspace.c), [organization](../../../src/organization.c), [tasks and blockers](../../../src/work_board.c), [suppliers and contracts](../../../src/procurement.c), [alerts](../../../src/alerts.c), [audit](../../../src/storage.c), [calendar bridge](../../../src/calendar_client.c). Persistent types are in the [data model](modello-dati.md); internal calls are in the [addon API](api-addon.md).

## HTTP conventions

- UTF-8 JSON API responses, except PDF downloads and OAuth redirects. Not all lists contain `ok`: use the HTTP status and the specific structure.
- Login, business and calendar `POST` requests require `Content-Type: application/x-www-form-urlencoded`; values must be percent-encoded. Even a `POST` without fields must declare this format. PDF upload uses binary `application/pdf`, not multipart. Logout is a separate case: the router checks the method/origin and does not read fields from the body.
- IDs are positive integers; minutes and cents are decimal integers without separators. Dates are `YYYY-MM-DD`; incoming flags are generally `0`/`1`.
- Unknown or duplicate parameters are rejected. Queries not listed for an endpoint are rejected by business modules; do not arbitrarily add pagination or filters. Mutations using `DELETE` require an empty body.
- The HTTP parser requires well-formed requests and limits headers to 16 KiB, ordinary bodies to 32 KiB, login to 8 KiB and PDFs to 10 MiB. `POST` requests to the `/api/tasks` and `/api/blockers` collections, their `/{id}` items and `/{id}/delete` operations allow 64 KiB forms. It does not support `Transfer-Encoding`/chunked. The HTTP target has an 8192-byte buffer. One request per connection is expected (`Connection: close`).
- Response headers: `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` and a restrictive CSP.
- Business error responses normally contain `{"ok":false,"error":"messaggio"}`. Do not rely on the Italian text as a stable code: Calendar addon machine codes are a separate contract.

### Authentication, sessions and CSRF

Authentication can be local, hybrid or company-only (`AUTH_MODE=local|hybrid|oidc`). `GET /api/auth/config` returns `{"sso":true|false,"local":true|false}` without secrets. `GET /auth/login` starts the redirect to Keycloak; `GET /auth/callback` consumes single-use code/state, verifies the token and creates the opaque Punto session for an explicitly linked active local account. SSO routes return `404` if disabled and `405` for methods other than GET; in `oidc` mode, `POST /api/login` returns `403`. Callback errors return a generic message and clear the temporary cookie. No OAuth tokens are returned to the browser. Details in [Keycloak](keycloak.md).

Access uses the `punto_session` cookie, a random 32-byte token represented by 64 hexadecimal characters. Flags are `Path=/; HttpOnly; SameSite=Lax`; `Secure` when the origin is configured as HTTPS. It is neither a JWT nor a public bearer token. Its internal lifetime is 8 hours, or 30 days with `remember=on`/`true`; in the latter case `Max-Age` is added. The app retains at most 64 sessions in memory; a restart loses them and a new session may replace the one with the nearest expiration.

Every protected request rereads the account: role and state are not copied indefinitely into the cookie. Disabling an account invalidates its access; temporary database blocking causes unavailability without automatically deleting a valid token.

For login and mutations, the server checks `Origin` against `Host` and allowed origins and rejects `Sec-Fetch-Site: cross-site`. A CLI client without `Origin` is allowed if the host is valid; there is no `csrf_token` field to obtain or send. Browser calls should use the same origin and cookie. The Google callback is a cross-site return `GET` protected by single-use state and session binding.

| Status | General meaning |
| --- | --- |
| `200` | Successful read, update, deletion or idempotent operation. |
| `201` | Business entity created. |
| `202` | Calendar change queued or synchronization requested; does not mean it has already been applied on Google. |
| `303` | OAuth callback completed with redirect to the app. |
| `400` | Invalid form/query/ID/date/value or disallowed fields. |
| `401` | Incorrect login credentials or missing/expired session. |
| `403` | Unauthorized role, assignment, recipient or origin. |
| `404` | Resource absent or unavailable within the user's scope. |
| `405` | Method not implemented for the path. |
| `408`, `413`, `415`, `431` | Read timeout, body too large, unsupported format, headers too large. |
| `409` | Business conflict, uniqueness, overlap, limit reached or calendar conflict. |
| `429` | Login attempt limit; possibly a response forwarded from the calendar. |
| `500`, `503` | Internal error or temporarily unavailable dependency. Reread state before retrying a write without idempotency. |

## Access and health

| Method and path | Access and input | Outcome |
| --- | --- | --- |
| `GET /healthz`, `HEAD /healthz` | No session; path must be exact, without a query. | `200 {"ok":true,"database":"mysql"}` or `sqlite`; `503 {"ok":false}` if storage is unhealthy. `HEAD` does not send the body. |
| `POST /api/login` | No session; `email`, `password`, optional `remember`. Email max 254 bytes, password max 1024 bytes. | `200 {"ok":true}` and `Set-Cookie`; `401` for incorrect credentials, `429` after 8 failures in the global 60-second window. A success resets the count. |
| `GET /api/session` | Optional cookie. | `200 {"authenticated":false}` or identity and permissions, example below. |
| `POST /api/logout` | Origin check; cookie if present. | Invalidates the session and expires the cookie, `200 {"ok":true}`. |

Authenticated response:

```json
{
  "authenticated": true,
  "email": "utente@example.invalid",
  "name": "Utente di esempio",
  "role": "worker",
  "permissions": {
    "manage_jobs": false,
    "manage_users": false,
    "edit_time_entries": false,
    "manage_assignments": false,
    "view_audit": false,
    "manage_suppliers": false,
    "manage_contracts": false,
    "manage_groups": false,
    "manage_job_roles": false
  }
}
```

`/api/session` does not return the user ID. Permissions are descriptive for the UI; the backend still checks every operation. The login counter is per process, not per individual account or IP.

## Users

All endpoints in this section require `manager`. No queries are allowed.

| Method and path | Body | Outcome |
| --- | --- | --- |
| `GET /api/users` | — | `200 {"users":[...]}` with `id`, `name`, `email`, `role`, `active`. |
| `POST /api/users` | `name`, `email`, `password`, `role` | `201 {"ok":true,"id":...}`. Created active. |
| `POST /api/users/{id}` | `name`, `role`, `active` | `200 {"ok":true}`. Does not accept email or password changes. |

UTF-8 name of 1–120 characters; validated/canonicalized and unique email; password of 8–1024 UTF-8 bytes without tab/CR/LF; `manager` or `worker` role; maximum 200 users. `409` for duplicate email, limit reached or an attempt to disable/demote the last active manager. No password reset, user deletion or public registration endpoints are exposed.

## Projects and time entries

| Method and path | Access | Fields/query | Outcome |
| --- | --- | --- | --- |
| `GET /api/jobs` | Manager: all; operator: common and authorized projects. | No query. | `200 {"jobs":[...]}`. |
| `POST /api/jobs` | Manager. | `code`, `name`, `client`, `description`, `status`, optional `contract_id`, `is_common`. | `201 {"ok":true,"id":...}`. |
| `POST /api/jobs/{id}` | Manager. | Same fields. Code, name and status must be sent; omitted `contract_id`/`is_common` retain the previous value. | `200 {"ok":true,"id":...}`. |
| `DELETE /api/jobs/{id}` | Manager. | Empty body. | `200 {"ok":true}`; also deletes the project's tasks, blockers, time entries, assignments and grants. |
| `GET /api/time-entries?date=YYYY-MM-DD` | Authenticated user. | Mandatory `date`; no `user_id`. | `200 {"entries":[...],"total_minutes":...}` for **their own hours only**, even for managers. |
| `POST /api/time-entries` | Manager or authorized operator. | `job_id`, `date`, `minutes`, optional `note`. | `201 {"ok":true,"id":...}`; owner derived from the session. |
| `POST /api/time-entries/{id}` | Manager, own entries only. | `job_id`, `date`, `minutes`, optional `note`. | `200 {"ok":true,"id":...}`. |
| `DELETE /api/time-entries/{id}` | Manager, own entries only. | Empty body. | `200 {"ok":true}`. |

Project: code 1–40 characters, name 1–120, client up to 120, description up to 1000 including multiple lines; status `planned`, `active`, `paused`, `completed`, `cancelled`; `is_common=0/1`. `contract_id=` clears the link; a new link requires an existing contract that is neither closed nor cancelled. Limit of 1000 projects and unique code (`409`).

Each `jobs` item contains `id`, `code`, `name`, `client`, `description`, `status`, `contract_id`, `contract_code`, `contract_title`, `supplier_name`, `is_common`, `total_minutes`, `assigned_users`, `assigned_groups`, `can_log_time`. `total_minutes` is the current user's personal total, not all time tracked for the project. Assignment counts are exposed to managers; use reports for company-wide aggregates.

Time: 1–1440 minutes per row, maximum 1440 total minutes and 1000 entries per user/day, note up to 1000 characters. Each returned item includes `id`, `job_id`, `job_code`, `job_name`, `date`, `minutes`, `note`, `owner`, `user_id`. The backend does not accept `owner`/`user_id` from a time form.

For operators, time-tracking authorization must be valid both today and on the work date and allow `can_log_time`. Local roles are combined with direct assignments, ancestor groups and common projects according to [processes and rules](../prod/processi-e-regole.md). A nonexistent project ID is rejected and may produce an event for the `invalid_job` alert; no orphaned hours are saved.

## Tasks, subtasks, Kanban and blockers

The board requires a project visible today. Managers edit all tasks; current contributors change only the status of tasks assigned to them. Blockers can be created by managers and contributors, then edited by a manager or a contributor who is their author or owner. Only managers reassign or delete. Viewers only view. Responsibility for an item does not automatically grant access to the project.

| Method and path | Input | Response |
| --- | --- | --- |
| `GET /api/work-board?job_id=N` | Only query `job_id`, mandatory; empty body. | `200 {"ok":true,"job_id":N,"tasks":[],"blockers":[],"assignees":[],"permissions":{...}}`. |
| `POST /api/tasks` | `job_id`, `title`; optional `parent_id`, `assigned_user_id`, `description`, `status`, `priority`, `due_date`. | `201 {"ok":true,"id":N,"revision":1}`. |
| `POST /api/tasks/{id}` | Mandatory `revision` and changed fields, excluding `job_id`. Contributors send only `status` and `revision`. | `200` with `ok`, `id` and incremented revision. |
| `POST /api/tasks/{id}/delete` | Only `revision`; manager. | `200` with `ok`, `id` and last deleted revision; `409` if it has children. |
| `POST /api/blockers` | `job_id`, `title`; optional `task_id`, `description`, `impact`, `owner_id`, `status`, `next_action`, `due_date`. | `201` with `ok`, `id`, `revision=1`. For contributors, the owner is always themselves. |
| `POST /api/blockers/{id}` | `revision` and changed fields, excluding `job_id`; only managers can change `owner_id`. | `200` with `ok`, `id` and incremented revision. |
| `POST /api/blockers/{id}/delete` | Only `revision`; manager. | `200` with `ok`, `id` and last deleted revision. |

No query is allowed on mutations. Duplicate/unknown fields, `job_id` on update, `revision` on create, author identity or timestamps sent by the client are rejected. Updates are partial: omitted fields retain their previous values. Empty optional IDs or dates clear the reference; the response value is `null`.

| Resource | Returned fields in addition to `id` and `job_id` |
| --- | --- |
| Task | `parent_id`, `assigned_user_id`, `assigned_user_name`, `title`, `description`, `status`, `priority`, `due_date`, `revision`, `created_at`, `updated_at`, `can_edit`, `can_update_status`. |
| Blocker | `task_id`, `title`, `description`, `impact`, `owner_id`, `owner_name`, `status`, `next_action`, `due_date`, `created_by`, `revision`, `created_at`, `updated_at`, `can_edit`. |
| Selectable assignee | `id`, `name`; list for managers only, `assignees:[]` for operators. Active managers and active contributors authorized for the project today are eligible. |
| Board permissions | `manage_tasks` and `report_blockers`, booleans. Each item's effective permissions are in its respective flags. |

Title 1–160 UTF-8 characters; description up to 2000; next action up to 1000. Optional valid `YYYY-MM-DD` due date; UTC timestamps `YYYY-MM-DDTHH:MM:SSZ`. Task status: `todo` (default), `in_progress`, `blocked`, `in_review`, `done`; priority: `low`, `normal` (default), `high`, `urgent`. Blocker status: `open` (default), `in_progress`, `resolved`; impact: `low`, `medium` (default), `high`, `critical`. Blocker, task, parent and project states remain independent.

Each project allows 500 tasks and 500 blockers; creation beyond the limit produces `409`. Subtasks have at most eight levels including the root, without cycles and within the same project. A blocker may also refer only to a task in its own project. A new assignee must be eligible today; an unchanged previous assignee may be retained even after revocation, without granting permissions.

An outdated `revision` produces `409` without overwriting data; reread the board before deciding on the change. Deleting a task without children retains blockers, clears `task_id` and increments their revisions in the same transaction. The audit records `tasks.create/update/delete`, `blockers.create/update/delete` and `blockers.task_detached`. Nonexistent and invisible items return the same generic `404`. Details in [task/blocker implementation](attivita-kanban-e-blocchi.md).

## Direct assignments, groups and local roles

All endpoints below require `manager`; no queries are allowed. An empty date denotes an open endpoint.

| Method and path | Body | Response |
| --- | --- | --- |
| `GET /api/jobs/{job_id}/assignments` | — | `200 {"job_id":...,"assignments":[...]}`. User data, local role, `starts_on`, `ends_on`, `can_log_time`. |
| `POST /api/jobs/{job_id}/assignments` | `user_id`, optional `starts_on`, `ends_on`, `role_id` (default Contributor ID 2). | `201` new or `200` updated, `{"ok":true,"id":job_id}`. One row per pair. |
| `DELETE /api/jobs/{job_id}/assignments/{user_id}` | Empty. | `200`, assignment removed. |
| `GET /api/groups` | — | `200 {"groups":[...]}` with ID, name, parent and counts. |
| `POST /api/groups` | `name`, optional/empty `parent_id`. | `201 {"ok":true,"id":...}`. |
| `POST /api/groups/{id}` | `name`, `parent_id` (omitted fields retain the previous value). | `200 {"ok":true,"id":...}`. |
| `DELETE /api/groups/{id}` | Empty. | `200`; `409` if it has subgroups, members or grants. |
| `GET /api/groups/{id}/members` | — | `200 {"members":[...]}`; direct memberships. |
| `POST /api/groups/{id}/members` | `user_id` | `201` new member, `200` if already present; requires an active user. |
| `DELETE /api/groups/{id}/members/{user_id}` | Empty. | `200`. |
| `GET /api/job-roles` | — | `200 {"roles":[...]}` with `id`, `code`, `name`, `can_log_time`, `builtin`. |
| `POST /api/job-roles` | `code`, `name`, `can_log_time=0/1` | `201`. |
| `POST /api/job-roles/{id}` | Same fields; omitted values retained. | `200`; `409` for a built-in role. |
| `GET /api/jobs/{job_id}/group-grants` | — | `200 {"grants":[...]}` with group, role and validity. |
| `POST /api/jobs/{job_id}/group-grants` | `group_id`, `role_id`, optional `starts_on`, `ends_on`. | `201` new, `200` updated. |
| `DELETE /api/jobs/{job_id}/group-grants/{group_id}` | Empty. | `200`. |

Limits: 200 groups, depth 16, 100 local roles, 200 members per group, 200 direct assignments per project, names up to 120 characters and role codes up to 40. Tree cycles, reversed dates, nonexistent references and invalid roles are rejected. Removing access does not delete historical hours; deleting the project does. There is no `DELETE` for local roles.

Lists expose: group = `id`, `name`, `parent_id` (number or `null`), `members_count`, `children_count`; member = `user_id`, `name`, `email`, `role`, `active`; grant = `group_id`, `group_name`, `role_id`, `role_code`, `role_name`, `can_log_time`, `starts_on`, `ends_on`. Assignment/grant dates without an endpoint are serialized as an empty string. Direct assignment `POST` replaces dates with those sent (omission = open endpoint) and an omitted role means 2; group grant `POST`, by contrast, preserves omitted dates on an existing row. Organizational change responses include `ok` and the affected entity's `id`; for grants and assignments, this is the project ID.

## Suppliers, contracts and PDFs

All endpoints require `manager`.

| Method and path | Body/query | Outcome |
| --- | --- | --- |
| `GET /api/suppliers` | No query. | `200 {"suppliers":[...]}`. |
| `POST /api/suppliers` | `code`, `name`; optional `vat_number`, `email`, `phone`, `address`, `notes`, `active`. | `201`, `ok` and `id`. |
| `POST /api/suppliers/{id}` | Same fields; omitted values retained. | `200`, `ok` and `id`. |
| `GET /api/contracts` | No query. | `200 {"contracts":[...]}` with supplier data and associated counts. |
| `POST /api/contracts` | `code`, `title`, `supplier_id`; optional `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `notes`. | `201`, `ok` and `id`. |
| `POST /api/contracts/{id}` | Same fields; omitted values retained. | `200`, `ok` and `id`. |
| `GET /api/contracts/{id}/documents` | No query. | `200 {"documents":[...]}` with PDF manifest. |
| `POST /api/contracts/{id}/documents?filename=contratto.pdf` | Original PDF body and `Content-Type: application/pdf`; only query `filename`. | `201`, `ok` and document ID; `503` if document storage is unavailable. |
| `GET /api/contract-documents/{id}/download`, `HEAD` on the same path | No query. | `200 application/pdf`, `Content-Disposition: attachment` with UTF-8 filename. `HEAD` omits the content. |

Codes 1–40 characters; name/title 1–120; VAT number up to 32, email 254, phone 40, address 240, notes 1000. Maximum 1000 suppliers and 1000 contracts. New suppliers are active by default; new contracts use `draft` and currency `EUR`. Optional dates, end not before start, empty value = unknown (`NULL`), otherwise 0–999999999999 cents. Formats with a decimal comma are not allowed.

Contract states: `draft`, `active`, `expired`, `closed`, `cancelled`. A new project cannot be linked to a closed or cancelled contract. The server validates supplier existence and enabled state for relevant operations. Contract data does not represent issued or received invoices.

PDFs: name up to 180 bytes, `.pdf` extension, no directory separators or control characters; size 5 bytes–10 MiB; `%PDF-` signature. Download verifies size and SHA-256. PDF replacement or deletion is not exposed: every upload creates an immutable document and an audit event.

Supplier rows contain `id`, `code`, `name`, `vat_number`, `email`, `phone`, `address`, `notes`, `active`; in this API, `active` is a **number 0/1**. Contract rows contain `id`, `code`, `title`, `supplier_id`, `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `notes`, `supplier_name`, `documents_count`; absent dates and values are `null`. The document manifest contains `id`, `contract_id`, `file_name`, `byte_size`, `sha256`, `uploaded_by`, `created_at`; the internal storage key is not exposed. Limit of 1000 PDFs per contract.

## Rates and five report views

| Method and path | Access/fields | Outcome |
| --- | --- | --- |
| `GET /api/labor-rates` | Manager, no query. | `200 {"ok":true,"rates":[...]}` with ID, user, rate and validity. |
| `POST /api/labor-rates` | Manager: `user_id`, `hourly_cents`, `starts_on`, `currency=EUR`; optional `ends_on`. | `201 {"ok":true,"id":...}`. |
| `POST /api/labor-rates/{id}` | Manager, same fields, immutable `user_id`. | `200`; `409` if intervals overlap. |
| `GET /api/reports` | Mandatory `view`, `from`, `to`; optional `job_id`, `user_id`. | `200` structure described below; `409` beyond 1000 rows without partial results. |

Rate 0–99999999 cents/hour, maximum 1000 total rates, inclusive dates without overlaps for each user; `ends_on=` leaves the interval open. No rate deletion endpoint. The report takes the rate valid on each entry's date, not the current rate.

Each returned rate has `id`, `user_id`, `user_name`, `user_email`, `hourly_cents`, `currency`, `starts_on`, `ends_on` (empty string if open). The list rejects more than 1000 rates with `409`, without pagination.

| `view` | Scope | Row fields in addition to counts |
| --- | --- | --- |
| `date` | Manager: everyone or filters; operator: only themselves. | `date`, `minutes`, `entry_count`, `job_count`, `user_count`. |
| `job` | As above. | `job_id`, `code`, `name`, `client`, `status`, `minutes`, `entry_count`, `user_count`. |
| `user` | As above. | `user_id`, `name`, `email`, `active`, `minutes`, `entry_count`, `job_count`. |
| `cost` | Manager only. | One record per contract involved in the filtered hours: `contract_id`, `code`, `title`, `supplier_name`, `status`, `starts_on`, `ends_on`, `value_cents`, `currency`, `job_count`, `minutes`. |
| `labor_cost` | Manager only. | User/project pair: `user_id`, `name`, `email`, `job_id`, `code`, `job_name`, `minutes`, `entry_count`, `priced_minutes`, `unpriced_minutes`, `cost_cents`. |

The period covers 1–366 days, inclusive. An operator specifying another `user_id` receives `403`. A missing rate produces unpriced minutes; `cost_cents=null` if no entry in the row has a rate. The contract view counts the full value of each contract selected by the hours once: it does not automatically apportion value by duration, project or user and is not the cost of hours.

Common response: `ok`, `view`, `from`, `to`, `today`, `scope` (`all`/`personal`), `summary`, `options: {jobs,users}`, `rows`. `summary` contains `minutes`, `entry_count`, `job_count`, `user_count`; for `cost`, it adds `contract_count`, `known_value_cents`, `unpriced_contract_count`, `uncontracted_minutes`; for `labor_cost`, it adds `estimated_cost_cents`, `priced_minutes`, `unpriced_minutes`. Totals and rows come from the same SQL query; do not sum only rows from a truncated result, because the server rejects results exceeding the limit.

## Alerts and notifications

| Method and path | Access/fields | Response |
| --- | --- | --- |
| `GET /api/alert-rules` | Manager, own rules. | `200 {rules,users,smtp_configured,timezone,daily_target_minutes,grace_hours}`. |
| `POST /api/alert-rules` | Manager; fields below. | `201`, rule saved. |
| `POST /api/alert-rules/{id}` | Owner manager; fields below. | `200`; ownership is checked in the backend. |
| `POST /api/alert-rules/{id}/run` | Owner manager, empty body. | `200`, execution outcome and counts; does not remove duplicate protection. |
| `GET /api/notifications` | Authenticated user, own notifications. | `200 {"notifications":[...],"unread_count":...}`; latest 100, no cursor. |
| `POST /api/notifications/{id}/read` | Authorized recipient, empty body. | `200 {"ok":true}`; repeating does not change the first `read_at`. |

Rule fields: `name` (1–120 characters), `type=invalid_job|missing_hours|inventory_due|inventory_return|inventory_verification`, `run_time=HH:MM`, `enabled=0|1`, comma-separated `recipient_user_ids`, `recipient_emails` separated by commas or lines. At least one recipient, maximum 50 users and 50 emails without duplicates; maximum 100 rules per manager. Internal recipients must be existing active users; for time alerts, a worker receives only notifications concerning themselves; the three inventory rules notify all selected recipients of events in the global catalog. There is no rule deletion endpoint: disabling it retains history.

`inventory_due` accepts `days_before` 1–365 (default 30), reports warranties, maintenance, expirations and renewals including overdue dates, excludes retired/lost assets and deduplicates by rule, recipient, asset, field, date and lead time. If inventory is unavailable, it does not consume the run. Details in the [inventory contract](asset-inventory.md).

`inventory_return` reports still-open handovers with an expected return date before today. `inventory_verification` accepts `verification_days` 1–365 (default 90): it immediately reports assets never verified and those whose chronologically latest verification is at least that interval old. Both apply only to IT devices, network devices and physical assets that are neither retired nor lost. Deduplication includes the handover or verification ID, date and, for verifications, interval: a new handover or verification report may produce a new alert. The source must be fully acquired and validated before the transaction; an error creates no partial alerts and does not consume the run. `days_before` and `verification_days` remain separate fields, always validated within their respective ranges.

`missing_hours` checks 480 minutes for each Monday–Friday, with a 48-hour grace period and persistent advancement through already examined days; it does not automatically subtract leave or holidays. `invalid_job` observes rejected attempts on nonexistent IDs. Daily time uses `Europe/Rome`; the app scheduler checks approximately every 30 seconds and may run after the scheduled time if the app was busy. Daily execution is tracked to prevent duplicates. List responses include last/next run and recipients; no SMTP credentials are exposed.

Notifications have `id`, `rule_id`, `subject_user_id`, `title`, `message`, `work_date`, `recorded_minutes`, `created_at`, `read_at`. For a user currently assigned the worker role, the backend further restricts notifications to those concerning them as the subject. Emails are queued even when SMTP is not configured; `smtp_configured=false` does not prevent internal notifications. Delivery states and semantics are in the [addon API](api-addon.md).

Every listed rule includes `id`, `name`, `type`, `run_time`, `enabled`, `days_before`, `verification_days`, `created_at`, `updated_at`, `last_run_at`, `last_run_on`, `next_run_at`, `recipient_user_ids`, `recipient_emails`. Inventory notifications include `event_kind`, `asset_id`, `due_kind`, `due_on`; operational notifications also include `source_id` and, for verification, `verification_days`. Creation/update POSTs return `{"ok":true,"id":...}`; manual runs return `ok`, `notifications_created`, `emails_queued`, `next_run_at`. Some inapplicable runs return `{"ok":true,"skipped":true}`. The unread count includes all visible unread notifications, not only the latest 100 returned.

## Asset inventory

Active users view the shared catalog and dashboard; writes require a Manager. The [complete contract](asset-inventory.md) describes records, relationships, CSV, locations, handovers and verifications. All public calls pass through the authenticated bridge, with verified TLS and a private token to the separate service.

`GET /api/inventory/assets`, `/api/inventory/assets/export` and `/api/inventory/dashboard` share the filters `q`, `status`, `asset_type`, `mine=1`, `site_id`, `department_id`, `attention`, `verification_days` and `today`. Location IDs are positive; the backend transforms `mine` into the current user's ID and does not accept an arbitrary assignee. `attention` can be `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, `location_mismatch`. `verification_days` defaults to 90 (1–365); `today` is a real Gregorian date, defaulting to the business day. Only catalog/export accept `limit` 1–100 and positive `after_id`.

The dashboard returns `ok`, `today`, `verification_days`, `counts` (`total`, `open_custody`, `overdue_returns`, `never_verified`, `verification_due`, `location_mismatch`), `by_status`, `by_type`, `by_site`, `by_department`, `by_user` and `groups_truncated`. Counts cover the entire filtered set. Groups `{id,name,count}` include unassigned items with a null ID and are limited to the top 20 per section. Operational counts concern active tangible assets; `verification_due` counts only previously verified assets, while `never_verified` is separate. The reference verification is the one with the latest `verified_on`, with the highest ID breaking date ties.

`GET /api/inventory/operational-due` accepts `kind=return|verification`, `today`, `verification_days`, `limit`, `after_id`. It returns `{ok,items,next_after_id}` with one row per asset, descending IDs; each row contains `asset_id`, `asset_tag`, `name`, `asset_type`, `due_kind=return_due|verification_due`, `due_on`, `days_remaining`, `source_id`. For never-verified assets, `source_id=0` and `due_on` is the creation date. The preexisting `/api/inventory/due` endpoint retains its four expiry types and parameters. The scheduler acquires at most 100 pages/10000 events within 8 seconds, rejecting incomplete or inconsistent results.

## Audit

`GET /api/audit` requires a manager. Query: `limit` 1–100, default 30; positive `before_id` cursor; optional `entity_type` from `job`, `time_entry`, `assignment`, `user`, `system`, `supplier`, `contract`, `contract_document`, `group`, `group_member`, `job_role`, `group_grant`, `labor_rate`, `alert_rule`.

Response `200 {"events":[...],"next_before_id":...}`; `null` cursor at the end, descending ID order. Each event contains `id`, `created_at`, `actor_id`, `actor_name`, `actor_email`, `action`, `entity_type`, `entity_id`, `before`, `after`; the last two fields are JSON or `null`. Audit editing/deletion is not exposed. Retain filters when using the cursor.

## Google Calendar bridge

All endpoints concern **the authenticated user's calendar**, whether a manager or operator. They do not accept `user_id`, addon bearer or `session_binding` from the browser. Identity and OAuth binding are calculated in the app.

| Public method and path | Input | Main outcome |
| --- | --- | --- |
| `GET /api/calendar/status` | No parameters. | `200`, OAuth configuration, connection, calendar, last sync and queue counters. |
| `POST /api/calendar/oauth/start` | Empty body. | `200 {authorization_url,expires_in:600}`; the client opens Google consent. |
| `GET /api/calendar/oauth/callback` | Query `state`, `code` or `error`; Google metadata `scope`, `authuser`, `prompt` accepted and discarded. | `303 /app?calendar=connected` or `/app?calendar=error`; never the OAuth code in the redirect. |
| `GET /api/calendar/calendars` | Optional `page_token`, `limit`, effective maximum 50. | `200 {items,next_page_token}`. |
| `POST /api/calendar/selection` | `calendar_id` | `200`; requires Google writer/owner role. |
| `GET /api/calendar/events` | `from`, `to` together, optional `limit`, `after_id`. | `200`, page from the local cache. |
| `GET /api/calendar/changes` | No parameters. | `200`, latest queue outcomes. |
| `POST /api/calendar/events/create` | `idempotency_key`, JSON `event` encoded in the form. | `202` queued, `200` for an identical repeat. |
| `POST /api/calendar/events/update` | `idempotency_key`, `event_id`, `etag`, `event`. | As above; ETag protects against overwriting others' changes. |
| `POST /api/calendar/events/delete` | `idempotency_key`, `event_id`, `etag`. | As above. |
| `POST /api/calendar/sync` | Empty body. | `202`; background start requested. |
| `POST /api/calendar/disconnect` | Empty body. | `200`; user storage removed and consent revocation attempted. |

Event schema, pages, conflicts and codes are documented fully in the [addon API](api-addon.md). The bridge enforces a 32 KiB form, maximum 24576 bytes for the `event` value and addon responses up to 128 KiB. Idempotency key of 8–128 ASCII characters `[A-Za-z0-9_.:-]`. The addon response is validated as JSON and cannot contain sensitive fields recognized as OAuth tokens or secrets; transport errors and unexpected responses are sanitized.

App→addon timeouts: connection 1 second; total 25 seconds for callback, calendar list and selection, 15 for disconnect, 5 for other operations. Not all these timeouts mean the provider has not performed an action: check status/queue and reuse the same key after an ambiguous outcome.

If the native bridge is not configured, `/status` returns `enabled:false`, `oauth_configured:false`, `connected:false`, provider `google`; other operations return `503 calendar_not_configured`. If the addon runs but Google credentials are missing, it exposes `oauth_configured:false` and OAuth operations return `503 oauth_not_configured`. Google configuration has been deferred: API integration does not imply an already connected account or an existing calendar UI.

## Examples for clients and integrations

The examples use a local cookie jar already obtained through login. `COOKIE_JAR` must point to a private file; replace `APP_ORIGIN` with the configured origin. Do not put credentials or cookies in the repository.

```sh
APP_ORIGIN='https://progetti.tuaazienda.it'
COOKIE_JAR='/percorso/privato/session.cookies'

curl --fail-with-body --cookie "$COOKIE_JAR" \
  "$APP_ORIGIN/api/session"

curl --fail-with-body --cookie "$COOKIE_JAR" \
  --header "Origin: $APP_ORIGIN" \
  --header 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'job_id=123' \
  --data-urlencode 'date=2026-10-12' \
  --data-urlencode 'minutes=120' \
  --data-urlencode 'note=Analisi requisiti' \
  "$APP_ORIGIN/api/time-entries"

curl --fail-with-body --cookie "$COOKIE_JAR" --get \
  --data-urlencode 'view=labor_cost' \
  --data-urlencode 'from=2026-10-01' \
  --data-urlencode 'to=2026-10-31' \
  "$APP_ORIGIN/api/reports"

curl --fail-with-body --cookie "$COOKIE_JAR" \
  --header "Origin: $APP_ORIGIN" \
  --header 'Content-Type: application/pdf' \
  --data-binary '@/percorso/contratto.pdf' \
  "$APP_ORIGIN/api/contracts/123/documents?filename=contratto.pdf"
```

IDs are placeholders: retrieve them from lists before mutations. No example requires disabling TLS verification. In local development, the origin will be the HTTP localhost origin specified by configuration; use HTTPS in production.

## Concurrency and integration limits

The main C server handles connections sequentially and runs the scheduler in the same process. Remote Calendar synchronization has a separate worker in the addon, but synchronous provider/SMTP calls and downloads can still affect app latency. The app is not currently a process cluster with shared sessions.

Business mutations and audit are transactional, with MySQL certification checking through `cluster_write_guard`. Tasks and blockers require `revision` on update/delete, checked in the same transaction; stale editors receive `409`. There are no ETags/versions for project updates: two serialized updates may both apply, and the last may replace fields from the previous one. Public webhooks, generated OpenAPI, GraphQL, personal API tokens, invoice endpoints and automatic event→project/time linking are not provided.

Contract and regression tests: [suite](../../../tests), particularly `test_server.py`, `test_roles.py`, `test_workspace.py`, `test_groups.py`, `test_assignments.py`, `test_procurement.py`, `test_alerts.py`, `test_reports.py`, `test_labor_rates.py`, [task/blocker tests](../../../tests/test_work_board.py) and [calendar bridge tests](../../../tests/test_calendar_client.py). For running the suite, see [development and testing](sviluppo-test.md).
