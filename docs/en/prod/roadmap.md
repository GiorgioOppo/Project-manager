# Product roadmap and progress

[Italiano](../../prod/roadmap.md) | **English**

This page updates how the plans should be read against the application baseline documented on **11 October 2026**, schema **14**. The [twenty-feature plan](roadmap-20-funzionalita.md) and [financial module plan](piano-modulo-finanziario.md) retain the original detailed design, written when the baseline was schema 3. Schema numbers, counts, and descriptions of the “current baseline” in those plans are historical references, not the current state.

The statuses used here are:

- **Implemented**: capability available in the code and intended UI.
- **Partial**: a useful part of the feature or infrastructure exists, but a component of the described workflow is missing.
- **Planned**: the requested behavior is not yet available as a complete feature.

A table, similar component, or addon alone does not complete a product feature. For current operational availability, see the [overview](panoramica.md). The **twenty improvements from the workflow review**, delivered in schema 14 and described in the [improved operational workflows](flussi-operativi.md), use separate numbering from the **twenty features in the historical plan** below. Delivering the review improvements does not automatically complete every requirement in the original plans; a global access matrix and assisted duplicate detection remain future extensions.

## Baseline already available

Implemented features include login, workspace administration and delegated Area Managers, Sales/Finance functional profiles, projects and states, direct and hierarchical-group access, operational roles, shared projects, personal time logs, suppliers, contracts/PDFs, and history. Five Analysis views, EUR cost rates, daily alerts, and notifications are available. Tasks includes tasks and subtasks, Kanban, a blocker register, estimates, and personal capacity comparison. Closure and archiving retain data; separate permanent deletion is blocked when a project contains consolidated data or protected economic references.

The email and Google Calendar addons are implemented as separate C services with available UI workflows. Email needs a configured SMTP provider to send: authorized administrators and managers can inspect the queue, cancel pending deliveries, and review uncertain outcomes. Calendar provides connection, viewing, and synchronization; operational use requires provider OAuth configuration and individual consent. Personal event time proposals pass through a draft and explicit confirmation.

## New track: tasks and collaboration

The first delivery includes the three agreed priorities: **tasks and subtasks, Kanban, and the blocker register**. Managers organize work and choose responsible people, priorities, and due dates; contributors update their assigned tasks' status and report or manage their own impediments. The [module guide](attivita-kanban-e-blocchi.md) describes behavior and permissions.

This track adds to the original twenty improvements without replacing their numbering.

| Proposed feature | Current status | Development still needed |
| --- | --- | --- |
| Tasks and subtasks | Implemented for organization, progress and linked hours | Separate checklists |
| Kanban board | Implemented | Further view preferences and automation only if defined in a later release |
| Risk and blocker register | Partial | Blockers implemented; future risks with probability and preventive measures still to develop |
| Dependencies and schedule | Planned | Time relationships, delay impacts, and Gantt |
| Team workload | Partial | Task estimates and personal capacity comparison implemented; aggregate team planning, dependencies, and workload leveling still to develop |
| Discussions and decisions | Planned | Comments, mentions, and reasoned decisions with project-scoped visibility |
| Change requests | Planned | Assessment and approval of impacts on work, deadlines, and budget |
| Deliverables and acceptance | Planned | Versions, review, observations, and approval of results |
| Customer portal | Planned | External access restricted to explicitly shared content and requests |
| Overview of all projects | Partial | Personal cross-project view and project overview implemented; overall portfolio still to develop |
| Personal timer | Planned | Start/pause, proposal requiring confirmation, and overlap checking |
| Guided project closure | Implemented | Open-work summary, checklist with manually verified ticket/equipment references, blocking new hours, archiving and reopening with a reason |

The current board uses status selections with confirmation, accessible by keyboard. States of tasks, parents, children, and blockers remain independent; calculated percentage progress and formal task approvals are absent. Operational assignment and due-date notices are available. Revisions protect concurrent task and blocker edits without automatically extending that mechanism to every historical API.

Recommended next steps are dependencies, scheduling, and aggregate team planning. Estimates, personal capacity, individual schedules, and approved absence requests are already available. Hour–task linking is available as an optional reference in the time log and project overview; the note remains descriptive.

## The twenty features in the historical plan

This table retains the original plan numbering so priorities and requirements can be compared. For the twenty improvements from the later review, use the separate [operational workflows](flussi-operativi.md) table.

| No. | Feature | Current status | What remains to complete it |
| --- | --- | --- | --- |
| 1 | Weekly grid timesheet | Implemented | Seven days, entry details, atomic multiple insertions and consistent totals |
| 2 | Copy previous day or week | Implemented | Previous-week and selective entry/day copying with explicit destinations, editable preview, and atomic confirmation; no automatic saving |
| 3 | Favorite and recent projects | Planned | Persistent personal preferences and selector ordering, always subject to current access |
| 4 | Time correction request | Implemented | Reasoned request, approval/rejection/cancellation, history and conflicts |
| 5 | Time-log anomaly checks | Partial | Versioned individual schedules, absences and holidays implemented; possible-duplicate warnings still to develop |
| 6 | Personal saved views | Partial | Analysis filters are already in the URL; named views, column/sort preferences, and account persistence are missing |
| 7 | Global search | Planned | A single search across authorized resources; local Projects, Suppliers, and Contracts searches already exist |
| 8 | Keyboard quick commands | Planned | Ctrl/Cmd+K bar with action catalog and subsequent record search; ordinary UI keyboard controls remain available |
| 9 | Customizable and accessible interface | Partial | Responsive UI, labels, and keyboard navigation present; theme/density, contrast review, and complete workflow checks remain |
| 10 | Installable app with offline drafts | Partial | Personal on-device drafts for hours, batch entry, and guided creation implemented with seven-day expiry, logout/account-change cleanup, and explicit recovery; PWA installation and complete offline availability still to develop |
| 11 | Guided CSV import | Planned | Column mapping, preview, row errors, confirmation, and subsequent user onboarding without passwords in CSV |
| 12 | Bulk operations | Planned | Multiple selection, preview of states/grants/revocations, and confirmation with revision checks |
| 13 | Guided project creation and templates | Implemented | Data, contract, team, tasks and summary; atomic confirmation, versioned templates with archiving and explicit restoration |
| 14 | Effective-access matrix | Partial | Per-person, per-project explanation implemented with direct/inherited sources and dates; global matrix still to develop |
| 15 | Area managers with limited delegation | Implemented | Workspace Administrator distinct from Area Manager; group/subgroup delegations, explicit capabilities, inclusive dates, and revocation effective on subsequent requests; hours retain team scope |
| 16 | Duplicate detection | Partial | Unique codes and PDF hashes already exist; assisted comparison, VAT-number/content warnings, and reasoned decisions are missing |
| 17 | Contract versions and amendments | Implemented | Sales prepares and publishes immutable data/PDF-reference revisions; explicit project link to a published version of its own contract |
| 18 | Assisted PDF reading and form filling | Planned | Local extraction/OCR, proposals with source pages, and manager confirmation, without modifying originals |
| 19 | Contractual obligation register | Planned | Obligations, responsibilities, deadlines, evidence, and internal agenda; existing time alerts do not already monitor contract deadlines |
| 20 | Supplier qualification | Planned | Versioned checklists, requirements, evidence, deadlines, and informational qualification status |

“Partial” for features 6 and 16 means a reusable baseline, not an already-complete saved view or duplicate check. Extended acceptance criteria remain in the [operational plan](roadmap-20-funzionalita.md).

## Operational release sequence

| Release | Scope | Expected result |
| --- | --- | --- |
| R0 | Shared foundations | Shared validators, resource authorization, revisions, idempotency receipts, and consistent time context |
| R1 | Navigation and access: 3, 6, 8, 9, 14 | Faster selection, personal preferences, and access explanations |
| R2 | Hours: 1, 2, 4, 5 | Weekly entry, controlled copies, approved corrections, and anomalies better aligned with actual schedules |
| R3 | Search and data entry: 7, 11, 12, 13, 16 | Less repeated entry, verifiable imports, and bulk operations |
| R4 | Delegation: 15 | Managers with powers limited to their own scope |
| R5 | Contracts and suppliers: 17, 19, 20 | Revisions, obligations, and qualification evidence |
| R6 | Mobility: 10 | Personal offline drafts and confirmed saving when back online |
| R7 | Documents: 18 | Assisted PDF reading with human review |

This is the historical dependency sequence, not a committed calendar or a list of features all still to build. Delivered parts are identified in the status tables. R6 and R7 can proceed separately when their foundations are ready. Existing alert and addon checks must be integrated into the design, not duplicated with a second equivalent engine.

R0's practical priority is preventing grid, copy, CSV, offline, and bulk operations from producing different behavior for the same data. Repeated submission of the same operation must have only one effect; an edit against a revision that has changed must return a visible conflict.

## Financial track

The agreed requirement for the **first financial release F1** already includes invoicing and receipts, multiple companies, multiple currencies, and companies with tax residence in different countries. Delivering only an estimated labor cost does not complete F1.

EUR reports and internal economic control are available to Finance: project budgets, forecast remaining cost, open-period actuals, immutable approved snapshots, and separate reasoned economic corrections. Approval of a global period blocks underlying time edits; changing rates never recalculates old snapshots. These capabilities **do not complete F1**: company isolation, customer tax records, sales rates, invoices, money movements, open items, and multi-company accounting closes remain absent.

| Code | Planned feature | Current status / target |
| --- | --- | --- |
| FIN01 | Companies and countries | Planned F1: separate company contexts and authorization |
| FIN02 | Currencies and exchange rates | Planned F1: document currency, functional currency, historical rates, and settlement differences |
| FIN03 | Customers and suppliers | Partial: operational suppliers exist; counterparties and versioned tax data in F1 |
| FIN04 | Commercial contracts | Partial: supplier contracts exist; purchase/sale distinction and time-based, fixed-price, or milestone models in F1 |
| FIN05 | Project budgets | Partial: internal EUR budget, remaining cost, and estimate at completion available; approved budget versions, expected revenue, and multi-company context remain for F1 |
| FIN06 | Rates and resource costs | Partial: per-user EUR cost and validity already available; sales, currency, scopes, and precedence in F1 |
| FIN07 | Time valuation | Partial: live estimates, immutable approved cost snapshots, frozen hours, and explicit economic corrections available; billable sources, sales rates, and company/tax context remain for F1 |
| FIN08 | Revenue and progress | Planned F1 for management revenue and progress, distinct from invoices and receipts; existing internal cost forecasts do not recognize revenue or cash flows |
| FIN09 | Purchases and direct costs | Planned F1 for costs and invoices; orders and commitments in F2 |
| FIN10 | Expense claims and reimbursements | Planned: manual expenses in F1, contributor workflow and structured reimbursements in F2 |
| FIN11 | Customer invoices | Planned F1: draft, lines, taxes, approval, and documents |
| FIN12 | Supplier invoices | Planned F1: classification and cost/project links |
| FIN13 | Credit notes and advances | Planned F1: adjustments and amounts to allocate; advanced progress billing and retentions in F2 |
| FIN14 | Tax rules and channels | Planned F1: a verified operational workflow for each activated company |
| FIN15 | Due dates and open items | Planned F1: installments, balances, overdue amounts, and aging |
| FIN16 | Receipts and payments | Planned F1: manual recording, partial allocations, advances, and reversals |
| FIN17 | Bank reconciliation | Planned F3: statement imports, deduplication, and proposed matches |
| FIN18 | Margins and cash flows | Planned F1 for current position, scenarios in F2 |
| FIN19 | Ledger and closes | Partial: approved internal cost periods and corrections available; multi-company management ledger, balancing, accounting closes, and export remain for F1, extensions in F4 |
| FIN20 | Group and intercompany | Planned: summaries by currency in F1; intercompany, conversion, and eliminations in F4 |

Current application History is not the financial ledger planned by FIN19. A total of contract values is not recognized revenue; an estimated hourly cost is not a payment; a converted company total is not already consolidation with eliminations.

### Internal F1 increments

1. **F1.0 — Companies and isolation:** assign companies to existing data, documents, and permissions, preserving identifiers and checking the absence of cross-company access.
2. **F1.1 — Amounts and capabilities:** exact monetary models, currency catalogs, country profiles, numbering, and financial roles.
3. **F1.2 — Counterparties and terms:** customers/suppliers, commercial contracts, accounts, and payment terms.
4. **F1.3 — Economic control:** budgets, costs, rates, valuations, and management revenue with traceable origins.
5. **F1.4 — Documents:** customer/supplier invoices, credit notes, attachments, and channels for initial-launch countries.
6. **F1.5 — Settlements:** opening balances, due dates, receipts, payments, allocations, and exchange differences.
7. **F1.6 — Management verification:** overview, due-date schedule, ledger, periods, and export.
8. **F1.7 — Testing:** migrations, permissions, numeric results, duplicates, interface, and recovery from uncertain outcomes.

F2 adds purchasing and planning, F3 banks and automation, F4 group management and advanced closing. Channels needed to issue invoices for initial-launch companies are already part of F1.

### Decisions required for F1

Before activation, identify companies, countries, functional currencies, tax data, existing accounting systems, and exchange channels. Also define financial administrators, approvers, commercial models, start date, and opening-balance sources. The initial-launch list of supported countries has not yet been selected.

Company isolation precedes activation of the second company. The operational profile must not become unrestricted access to financial data or other companies. Already-valued or invoiced sources must also be protected from current edit/delete routes: financial retention cannot depend solely on a future approval page.

## Calendar and email: completing the user workflow

Email still requires SMTP provider configuration in the deployment. The queue UI supports outcome inspection, cancellation of still-pending deliveries, and recording provider verification for uncertain outcomes. Recording a review does not resend the message; a delivery already in progress cannot be cancelled. The addon does not require a new user identity in the interface.

Google Calendar already has a screen for connection/disconnection, calendar selection, status viewing, and operations requiring review. Provider OAuth configuration and individual consent remain operational prerequisites. Google transport and tokens stay in the addon.

The event–project workflow is available through time drafts: it verifies the event in the personal calendar, normalizes dates and duration, checks current access, and requires confirmation before recording hours. All-day or cancelled events are not converted into hours; review the proposal again if the event has changed. A Ticket reference may be entered as a proposal to check and never grants permissions. Confirming the same draft is idempotent. Planned time remains separate from confirmed work: there is no automatic conversion or subsequent synchronization of saved hours with the event.

## Cross-cutting completion conditions

Every release must include interface, server behavior, data and migrations, authorization checks, error-case tests, and updated documentation. A feature appearing in the menu without permissions, saving, and error handling is incomplete.

Common criteria are: preservation of existing data; reproducible outcomes in SQLite and MySQL when both are supported; no exposure through filters, counts, or downloads; handling repeated submissions and conflicts; keyboard-usable workflows and small-screen support. Historical tests are evidence for the version examined and do not replace tests for the next release.

Implementation and acceptance details are in the [twenty-feature plan](roadmap-20-funzionalita.md) and [financial module plan](piano-modulo-finanziario.md); current capabilities remain described in [Processes and rules](processi-e-regole.md).
