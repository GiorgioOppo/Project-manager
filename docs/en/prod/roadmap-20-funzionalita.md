# Implementation plan for Punto's 20 features

[Italiano](../../prod/roadmap-20-funzionalita.md) | **English**


> **Planning document.** The first draft assumed schema 3; the code documented as of 10 October 2026 uses schema 7. The entities, APIs and releases proposed here do not attest to already available features. For an updated comparison, see [status and roadmap](roadmap.md) and the [product overview](panoramica.md). Analytics, rates, alerts and addons were added after this plan's initial baseline.

The 20 additions will be implemented in seven releases, preceded by a shared technical foundation. The first release simplifies navigation and explains access; subsequent releases improve hours, record creation and delegation, then introduce contract versions, offline drafts and assisted PDF reading.

Each release includes backend, interface, migrations, tests and documentation. Feature numbers correspond to the discussed proposal. The choices described are the implementation design; the plan's initial baseline was version 3; implemented status is summarized in the updated roadmap.

The [financial module plan](piano-modulo-finanziario.md) extends this roadmap to multiple companies, countries and currencies, including invoicing and collections in the first financial release. Operational features share company context and access controls; financial permissions are separate from project permissions.

After F1.0, references to the global manager mean the individual company's manager within assigned capabilities. No operational role retains a cross-company bypass; delegations remain within the same company scope.

## Initial plan baseline and constraints

The starting point is commit `2b16fe3`: C server, framework-free JavaScript interface, MySQL 8.4 with three Group Replication nodes, HAProxy, MongoDB GridFS and Nginx Proxy Manager. Native mode uses SQLite and a local document store. MySQL retains 14 application and service tables in schema 3.

Project permissions derive from users and hierarchical groups, with Viewer, Contributor and custom roles. A single permission must allow time tracking and cover both today and the worked day. Operational project roles are distinct from administrative powers.

Hours always belong to a stable user and can currently be viewed only by their author, including for managers. Operators can add them; managers can edit or delete their own. Future correction approval therefore requires a specific permission.

Durations remain whole minutes; existing EUR amounts remain cents. The new financial module uses minor currency units and a per-currency exponent. Days and intervals remain ISO dates; technical and audit instants are UTC. The company time zone becomes explicit, initially `Europe/Rome` for the legacy company and configurable for others, to determine today and weeks without nodes or browsers making decisions about different days.

The app calculates one company date per operation after acquiring write-transaction protection and passes it to all checks. The date and time zone are returned to the UI; the decision does not depend on the browser clock or `CURRENT_DATE` on individual nodes.

Original PDFs remain immutable in GridFS or the local store. The relational schema retains references, metadata and auditing. Segmentation keeps NPM as the only public service; new features do not require app Internet egress. Google Calendar and the previous roadmap remain separate developments, integrable through shared APIs and deadlines.

Any automatic financial connectors have a dedicated component with egress restricted to configured destinations. Manual file exchange keeps the app isolated from the Internet. Companies, master data, hours, groups, search and documents require consistent isolation before activating a second company.

## Release sequence

| Release | Features | Verifiable result | Dependencies |
| --- | --- | --- | --- |
| R0 | Shared technical foundation | Reusable writes, revisions, idempotency and shared access controls | Schema 3 |
| R1 | 3, 6, 8, 9, 14 | Favorites, views, quick commands, accessible interface and access explanations | R0 |
| R2 | 1, 2, 4, 5 | Weekly entry, controlled copying, corrections and time-entry warnings | R0 and R1 UI components |
| R3 | 7, 11, 12, 13, 16 | Unified search, import, bulk operations and guided creation | R0, R1; duplicates before import |
| R4 | 15 | Area managers with scoped administration | Matrix 14 and per-resource checks |
| R5 | 17, 19, 20 | Contract revisions, obligations and supplier qualification | R3; authorization consistent with R4 |
| R6 | 10 | Installable app and offline drafts to confirm online | R1, R2, idempotency and local data policy |
| R7 | 18 | Local text extraction and suggested completion from PDFs | R5 and persistent processing queue |

R6 and R7 are independent: draft work can proceed while document processing is prepared. R5 is not a prerequisite for time-entry drafts. In R1, quick commands search actions and sections; R3 adds record search to the same bar.

The financial path starts with R0 and F1.0 company separation. It reuses R1 and progressively integrates R2, R3, R4 and R5 without waiting for all seven releases to provide invoices and collections. It brings revisions, soft deletion and protection of costed or invoiced hours forward into F1, even though correction requests arrive in R2. R6 initially limits offline drafts to hours.

Complexity is relative: **low** for a local change with available data, **medium** for a new flow with APIs and persistence, **high** for scoped authorization, concurrency or new processing processes. Day estimates will be made for each release after defining its details and team capacity.

## R0 Shared technical foundation

1. Gradually extract domain operations and project, hour, assignment and contract validators from HTTP handlers. Forms, imports, wizards and batches will call the same functions. Keep existing APIs compatible.
2. Introduce an authorization context for actor, action and resource. Reuse `organization_job_access` and `organization_access_list`; add grant explanations to their decisions, avoiding a second permission engine for search and the matrix.
3. Add numeric revisions to mutable records involved in previews and corrections. Every update checks the expected revision within the same transaction; a conflict returns 409 and preserves the client draft.
4. Introduce `mutation_receipts` with actor, operation UUID, content fingerprint and minimal outcome. Write, receipt and audit are atomic. The same UUID with the same content does not cause a second effect; different content returns 409. Receipt lifetime must cover draft lifetime and allowed attempts. After detailed outcomes are cleaned up, a durable trace of consumed keys remains so an old draft cannot be applied again. Retrieving the minimal outcome of an already completed write is distinct from a new change, which always requires current authorization.
5. Add structured JSON bodies to new bulk APIs, with a shared parser and limits on size, depth, rows and fields. Retain current forms; validate UTF-8, integers and unknown fields. New routes retain sessions and origin checks.
6. Progressively split `public/app.js` into modules for APIs, actions, preferences, hours and organization. Introduce CSS tokens for colors, spacing, focus and density without completely rewriting the interface.
7. Use server-side pagination and filters for new searches. Retain explicit limits: no silently truncated lists or implicit increases in current limits.
8. Prepare the company context shared with the financial plan. Preferences, views, authorization, imports and queues carry the company when handling business data; the operational profile does not become a bypass for other company scopes.

MySQL transactions retain `cluster_write_guard` protection. Migrations will run once with application writes suspended and a connection to a selected node, waiting for group alignment before resumption. In multi-primary mode, MySQL requires particular care with concurrent DDL and DML; new relationships will continue to avoid `CASCADE` operations. [MySQL 8.4 documentation](https://dev.mysql.com/doc/refman/8.4/en/group-replication-multi-primary-mode.html)

**R0 completion:** upgrade from a schema 3 copy without data loss; repeated submission produces one domain event; stale updates are rejected; existing endpoints retain previous behavior.

## R1 Navigation and understanding access

### Feature 3 Favorite and recent projects

**Low complexity.** Add `user_job_preferences(user_id,job_id,is_favorite,last_used_at)` with a composite key. `/api/me/job-preferences` APIs always operate on the authenticated user. Update recent use only after successful time-entry saving; sort favorites and recent projects before others.

**Acceptance:** preferences separate between users; revoked projects disappear from choices even if favorited; a recent item grants no access and failed saving does not update it.

### Feature 6 Saved personal views

**Medium complexity.** Add `saved_views` with owner, section, name, versioned configuration, default view and revision. `/api/me/views` saves filters, sorting and columns from an allowed set. First delivery provides private views for projects, contracts and suppliers in accessible sections.

**Acceptance:** the same view is available on another device; no access to others' views; incompatible filters are reported; the server always refilters data according to current permissions.

### Feature 8 Keyboard quick commands

**Low complexity.** Create a single action catalog used by buttons and the `Ctrl/Cmd + K` bar. R1 includes navigation, new time entries and opening permitted screens; R3 adds global search results. Actions reassess availability when executed.

**Acceptance:** opening, selection and closing fully usable by keyboard; Escape restores focus; no interference with typing or text composition; revocation while the bar is open is respected.

### Feature 9 Customizable and accessible interface

**Medium complexity.** Add `user_preferences` and `/api/me/preferences` for light, dark or system themes and normal or compact density. Complete CSS tokens, loading states, field-linked errors and assistive-technology announcements. Visual preferences may have a local fallback without business data.

**Acceptance:** main flows usable by keyboard and smartphone; visible focus, contrast and zoom verified; status and role distinguishable without color; consistent dialogs and messages. The design reference is WCAG 2.2 level AA, to be verified on real flows. [WCAG 2.2](https://www.w3.org/TR/WCAG22/)

### Feature 14 Effective access matrix

**Medium complexity.** Expose `/api/jobs/{id}/access-matrix` and `/api/users/{id}/effective-access`, initially only to managers. Show effective access, role, direct and group grants, inherited path and dates. Browsing is paginated; a simulated date is only for preview, while the actor's right to view it is always checked in the present.

**Acceptance:** the matrix agrees with operational APIs for alternative roles, inclusive dates and multiple memberships. Revoking a grant highlights any remaining access. Simulation changes neither data nor authorization.

## R2 Faster, more controllable time tracking

### Feature 1 Weekly time-entry grid

**High complexity.** Extend reading of personal hours to a seven-day interval, row pagination and totals. Every cell shows its total and allows opening individual entries with notes and IDs; grid insertion adds rows without replacing existing ones. First delivery supports reading and insertion, retaining edits in already permitted flows.

The new batch API saves at most 100 new entries per operation, all in the same transaction. It checks each day's final total, the entry limit and the grant valid both today and on the worked date.

**Acceptance:** totals equal the daily log, multiple notes preserved in one cell, revoked history readable but unusable for new hours. One invalid row cancels the entire batch; double submission does not duplicate hours.

### Feature 2 Copy the previous day or week

**Medium complexity.** Reuse weekly reading and batches to prepare a draft with an explicit destination. Offer copying durations alone or notes too. Show excluded or invalid rows and allow removal or editing before confirmation.

**Acceptance:** source unchanged; correct checks at month end, year change and leap year; no automatic saving; server rechecks revocations or role changes between preview and submission.

### Feature 4 Time-entry correction requests

**High complexity.** Add `time_entry_correction_requests` with author, entry, starting revision, proposed values, reason, status and reviewer. One pending request per entry; pending, approved, rejected, canceled and conflict statuses. Dedicated APIs support personal requests, personal lists, reviewer queues and reasoned decisions.

Time-entry deletion becomes soft deletion through `deleted_at` and revision increments: the row remains available for requests and audit but is excluded from the current log and totals. This prevents historical requests from blocking deletion or deletion leaving invalid references.

Introduce the specific capability to approve corrections: first for global managers, then authorized area managers in R4. The queue shows only decision-relevant data and does not generically extend APIs for others' hours. Prevent self-approval; managers retain their existing personal editing flow.

An approved correction checks revision, daily limits and reviewer permissions in the same transaction as hours, request and audit. After revocation, historical reductions or deletions can be proposed; increases and moves require current permission valid for the destination. The entry author's name does not change.

**Acceptance:** no change before approval; a second decision has no further effect; changed revisions or deleted entries produce conflicts; rejection includes a reason; no viewing of out-of-scope requests.

### Feature 5 Log anomaly checks

**Medium complexity.** Start with warnings about possible duplicates and inconsistent totals. Add expected user schedules, validity periods and calendar exceptions to make incomplete-day detection reliable. Proposed tables: `work_schedules`, `work_schedule_assignments`, `work_schedule_exceptions` and `time_policy_rules`.

Warnings are initially informational; the hard 24-hour daily limit remains. The schema stores duration and day, so overlapping time slots cannot be inferred. Operators see their own warnings; reviewers see those relevant to their queue and, after R4, their delegated scope.

**Acceptance:** part-time work, holidays and exceptions cause no false blocks; absent configured schedules reported; changing data updates warnings; no exposure of others' hours through counts or messages.

## R3 Search and record creation with fewer steps

### Feature 16 Duplicate detection

**Medium complexity.** Implement before import: reuse unique codes and PDF SHA-256 hashes, index hashes and add country and normalized VAT number to suppliers. Show accessible candidates and reasons for matching. Initially, equal VAT numbers and hashes generate warnings, while unique codes remain mandatory constraints.

**Acceptance:** renamed PDFs recognized by hash; same name with different content remains distinct; no automatic merge or deletion; any reasoned continuation is tracked; check repeated on saving.

### Feature 7 Global search

**Medium complexity.** Add `/api/search` for codes and text of projects, suppliers, contracts and document metadata. Apply permissions before results, counts and previews. First delivery uses parameterized SQL and paginated results; PDF text indexing arrives with R7. User search remains a scoped administrative feature.

**Acceptance:** operators find only authorized projects; contracts and suppliers respect their permissions; Unicode and wildcards are handled; searches with no results do not reveal forbidden objects.

### Feature 11 Guided CSV import

**High complexity.** Implement upload, column mapping, validation, preview and confirmation. Use `operation_batches` and `operation_rows` for author, type, content, expiry, errors and result, reusing them for bulk operations. Proposed APIs: `/api/imports`, `/{id}/preview` and `/{id}/commit`.

First increment for suppliers and projects; at most 1000 preview rows and 100 per atomic confirmation. Larger files require explicit partitions with an outcome for each part. Second increment for users: import master data and the Operator profile, without passwords in CSV. Add activation with a single-use token, expiry and fingerprint stored in `account_enrollments`; managers may deliver the link without introducing automatic emails. Manager privileges remain assigned through the dedicated administrative flow.

Master data stays in enrollment until activation, without creating accounts able to authenticate with temporary credentials. Confirmation validates an unexpired, uncanceled enrollment, creates the account with the chosen password derived using the existing method, and consumes the token in the same transaction. An already registered email causes conflict and neither reactivates nor replaces an existing account. The full token is shown to the manager once; it appears in neither CSV, audit nor logs.

**Acceptance:** quotes, separators, newlines and UTF-8 validated; errors per row and field; repeated confirmation without duplicates; data and permissions rechecked; expired temporary materials removed; credentials excluded from files and auditing.

### Feature 12 Bulk operations

**High complexity.** Add project selection and preview for status changes, user or group assignment and revocation. `/api/jobs/bulk/preview` produces a persistent operation with expected revisions; `/api/jobs/bulk/commit` checks scope and intervening changes. Initial limit: 100 projects per transaction.

**Acceptance:** all results applied or none; stale previews return 409; audit for every object with a shared operation identifier; no revocation declared complete if other valid grants remain.

### Feature 13 Guided creation and project templates

**Medium complexity.** Create a wizard for contract, project data, permissions and summary. `job_templates` and `job_template_grants` retain only explicit, versioned configurations. Managers can prepare a template per service type, then choose a new code and confirm proposed data.

**Acceptance:** atomic creation of project and grants; contract, supplier, groups and roles rechecked on confirmation; disabled references reported; hours and documents not copied from the template; no implicit individual access.

## R4 Administrative responsibilities by area

### Feature 15 Area managers with limited delegation

**High complexity.** Add `area_delegations` with recipient, root group, capabilities, interval and assignable roles, and `job_management_scopes` to explicitly associate projects with an administrative scope. Group membership, operational project access and authority to administer it remain distinct concepts.

First delivery: the manager handles project permissions within their scope and memberships within the branch; corrections can be approved only with an explicit capability. New permission recipients must belong to the managed branch; initial inclusion of people outside the branch remains with the global manager. Users in multiple branches retain external memberships and permissions intact.

Creating or promoting managers, changing the role catalog, delegations, group moves and subdelegation remain with the global manager. Delegation grants no access to the contract archive. The UI receives per-resource capabilities: having a delegation does not turn `manage_jobs` into a global permission.

**Acceptance:** no operations outside the branch or on merely visible projects; no self-extension of powers; revocation effective on the next request; moving a branch recalculates scopes; changes and decisions always audited. Search, matrix and bulk operations show only authorized scope.

## R5 Structured contracts and suppliers

### Feature 17 Contract versions and amendments

**High complexity.** Add `contract_revisions`, `contract_revision_documents` and `jobs.contract_revision_id`. A draft revision can be prepared and then published as an immutable snapshot of data and document references. Contracts and revisions have distinct responsibilities; substantive changes to a published contract create a new revision.

Migration creates an initial revision from the current situation and links already associated projects; this snapshot does not retroactively reconstruct versions never recorded. Original documents remain reusable between revisions without changing protected metadata.

**Acceptance:** new revisions do not change existing projects' references; project version changes explicit and tracked; concurrent publishing controlled; existing codes, values, dates and PDFs retained.

### Feature 19 Contract obligation register

**Medium complexity.** Add `contract_obligations` and `obligation_evidence` with revision, type, description, authorized owner, due date, warning lead time, status and evidence. First delivery includes an internal agenda and UI deadline indicator; external reminders and Google Calendar will connect to the separately planned integration module.

**Acceptance:** responsibility and completion tracked; renewals and notice periods entered or confirmed by managers; evidence linked to the correct revision; passing a deadline does not automatically renew or close a contract. Assignment to an obligation does not automatically grant access to the whole archive.

### Feature 20 Supplier qualification

**High complexity.** Add versioned checklists with requirements, mandatory status, validity, verification and evidence. Proposed tables: `qualification_templates`, `qualification_requirements`, `supplier_qualifications` and `supplier_requirement_checks`. Introduce `supplier_documents` metadata and a dedicated bucket while keeping the contract bucket compatible.

First delivery computes an informational status: incomplete, qualified or expired. Any blocking of new assignments will be a second configurable increment with reasoned exceptions. Criteria are defined by the organization and not automatically inferred from regulatory obligations.

**Acceptance:** expired documentation updates status; checklist revisions do not rewrite historical checks; evidence readable only by authorized users; existing contracts, projects and hours retained.

## R6 Installable app and offline drafts

### Feature 10 Installable app with offline drafts

**High complexity.** Introduce a manifest, service worker and versioned cache for static interface files only. The PWA requires HTTPS in public environments; `localhost` enables local development. [Service workers on MDN](https://developer.mozilla.org/en-US/docs/Web/API/Service_Worker_API/Using_Service_Workers)

Personal drafts in IndexedDB contain owner ID, UUID, date, project, minutes, note and status. Local retention is optional per device, with expiry and a delete command. Passwords, session tokens, authenticated APIs and PDFs do not enter the cache. The design must clarify which note content may be retained and how to protect it on the device before enabling the feature.

Returning online requires a valid session, summary and confirmation. The server applies current permissions and returns confirmation, rejection or conflict for the operation. Switching accounts cannot submit the previous account's drafts; sign-out and account switching delete local data after warning about unsent drafts. A disconnected device may retain downloaded data until its next connection: server revocation prevents new writes but does not remotely delete an offline copy.

Provide synchronization from the open app and a manual button; Background Sync is only an optional enhancement because browser support is not uniform. [Background Synchronization API on MDN](https://developer.mozilla.org/en-US/docs/Web/API/Background_Synchronization_API)

**Acceptance:** network loss after commit causes no duplicates; permissions revoked while offline respected on return; no drafts transferred between accounts; exhausted storage and expired sessions handled; service worker updates without draft loss.

## R7 Assisted document reading

### Feature 18 Assisted reading and completion from PDFs

**High complexity.** Add `document_processing_jobs` with status, document, hash, processor version, attempts and job expiry, plus versioned results containing page, excerpt, proposed value and manager confirmation. First extract existing text; for scanned pages, convert the PDF to images and use local OCR. Tesseract requires this conversion because it does not read PDFs directly. [Tesseract input formats](https://tesseract-ocr.github.io/tessdoc/InputFormats.html)

The C app continues handling authentication, queues and persistence. A separate local worker performs extraction and OCR with CPU, memory, page, file and duration limits. The initial solution uses `network_mode: none`, no database credentials and a dedicated exchange volume: the app exports a verified PDF and collects a validated result. The worker joins none of the three application networks. Generated internal names, no commands built from user-supplied paths and temporary-file cleanup are part of the flow.

Every attempt has an identifier and claim deadline. The result must match the job, current attempt, PDF hash and processor version; the app rejects late results belonging to an attempt superseded after a restart.

Originals in GridFS; derived text and extended results in a separate bucket; status, proposals and references in SQL. Initial text search uses a controlled, rebuildable index queried only after filtering authorized documents. There is no SQL–MongoDB distributed transaction: use processing states, compensation and reconciliation for incomplete or orphaned results.

**Acceptance:** text PDFs and scans processed without changing originals; protected, malformed or over-limit PDFs produce explicit outcomes; restarting the worker recovers jobs without double application; amounts and dates are proposals to confirm, with source page; no contract text in logs or unauthorized users' results.

## Schema and API evolution

Final versions will be assigned to migrations as each release is implemented. The following breakdown describes content without assuming everything must be applied in a single update.

| Increment | Data to add or extend |
| --- | --- |
| R0 | Revisions on affected mutable records; `mutation_receipts`; explicit company time zone |
| R1 | `user_preferences`, `user_job_preferences`, `saved_views`; calculated matrix without permanent permission copies |
| R2 | `time_entry_correction_requests`, `time_entries.deleted_at` (brought forward to F1 for financial sources); schedules, assignments, exceptions and warning rules |
| R3 | `operation_batches`, `operation_rows`, `account_enrollments`, `job_templates`, `job_template_grants`; normalized fields and duplicate indexes |
| R4 | `area_delegations`, associated capabilities and `job_management_scopes` |
| R5 | Revisions and document references; obligations and evidence; checklists and supplier documents |
| R6 | Versioned IndexedDB schema; reuse of server receipts without duplicating the time log in the browser |
| R7 | Queue, result metadata, proposals and rebuildable index; separate bucket for derived text |

All new SQL tables will have primary keys, explicit relationships and equivalent SQLite/MySQL migrations. Existing IDs and references will be preserved. New responses will use additional fields or dedicated endpoints; errors will distinguish validation, prohibition, expired previews and conflicts.

## Verification and release

For each increment, apply these criteria to the parts actually changed:

- Domain and HTTP tests for the main flow, errors and previous API behavior.
- Permission tests with manager, operator, viewer and, from R4, area manager; revocations with an already open session and between preview and confirmation.
- Migration from a schema 3 copy and intermediate versions, comparing historical data, relationships and document hashes.
- Live MySQL checks for transactions, auditing and concurrency. For idempotency and queues, simulate network interruptions and commits with uncertain outcomes in an isolated test environment.
- Browser verification of changed paths, keyboard, mobile and recoverable errors. For R6 include browsers with and without Background Sync; for R7 text, scanned and unprocessable PDFs.
- Coordinated SQL and document backup before migrations; verification of three-node agreement and affected services after release. The restore plan specifies whether returning to previous code is sufficient or backup recovery is needed; no automatic destructive reverse migration.
- Separate commits for coherent increments, including migrations, tests and instructions. A module is complete when its flow is usable from the interface and verified on the intended backend.

## First increment to develop

Start R0 with validator extraction, revisions and the idempotency protocol, then R1 in this order: **theme and accessible components → favorites → saved views → access matrix → quick commands**. Favorites are the first small functional delivery; the matrix prepares future delegation.

Company schedules, content retainable offline, checklist requirements and administrative boundaries are settings to define when their release is addressed. The plan's initial values allow early increments to proceed without blocking on later-stage decisions.
