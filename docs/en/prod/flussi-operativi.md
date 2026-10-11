# Improved operational workflows

**English** | [Italiano](../../prod/flussi-operativi.md)

Workspace implementation as of 11 October 2026, schema **14**. Building or testing this version does not update an existing Docker deployment.

## Profiles and time scope

Every active account is an Operator; Manager derives from the identity role. In **Users → Profiles**, administrators request Sales and Finance changes, including for Keycloak accounts. With two or more active administrators a different administrator must approve. A single administrator can apply changes immediately in the explicit, audited bootstrap mode. Sales edits suppliers, contracts and PDFs; Finance views them and reads values, rates and labor costs aggregated by project, without third-party personal hours or a User filter. Editing rates requires Manager + Finance. The Manager role alone does not grant these rights.

For hours, managers see themselves and members of their own organizational groups and subgroups. Without groups, or if company groups are unavailable, they see only themselves. The Time log’s person selector provides read-only team day/week viewing. Work reports, summaries, corrections, History and alerts apply the same team scope; revocations and profiles are checked on every request.

## My day

The My day view brings together assigned tasks, blockers, today's hours, equipment and personal notices, with contextual navigation. Every request checks live job access. Archived jobs remain available through the job catalogue without occupying the current work queue. Task and blocker lists return up to 200 items each and disclose additional result counts. My tickets opens the personal ticket queue. Ticket remains a separate application with its own session and explicit Punto connection.

## Weekly timesheets

Choose Day or Week in the timesheet. Weeks run Monday to Sunday across month and year boundaries. Cells retain individual entries and notes. Multi-row entry and copying the previous week prepare an editable preview; neither saves automatically or replaces existing entries. Copying retains task links, which can be removed in the preview. Changing a row's job clears its previous task link.

A batch contains at most 100 new entries. All rows commit together or none do. The server checks current and historical grants, closed jobs, daily totals of at most 24 hours and the 1000-entry daily limit. Retrying the same request returns its previous result without inserting duplicates. Changed contents require a new operation.

## Corrections

Request a correction from one of your entries, propose job, date, duration and note changes or request deletion, and provide a reason. The original remains unchanged until approval. Operators see their own requests; managers review other members’ requests within their own team and cannot decide their own. Authors can cancel pending requests. Rejections require a reason.

Only one request may be pending per entry. Approval atomically rechecks the original revision and contents, owner permissions and daily limits. A changed or deleted original produces a retained conflict, including the original snapshot and proposal. Repeated decisions do not modify hours twice. Reducing or deleting the original on the same job and day remains possible after revocation or closure; increases and moves require current destination access. Moving to another job clears the previous task reference.

## Task time

Choose an optional task belonging to the selected job or start logging hours directly from its card. Job summaries show task actuals, personal for operators and aggregated within their own team for managers. Deleting a task retains its hours on the job and clears only the task reference.

## Schedules and exceptions

Managers configure expected minutes for each weekday with an effective start date. Later schedules preserve earlier periods. Personal exceptions represent leave or individual shifts; company holidays apply to everyone. Precedence is **personal exception → company holiday → latest effective schedule → default**. The default remains eight hours Monday–Friday and zero at weekends.

Missing-hours checks retain the 48-hour grace period after the end of a day in Europe/Rome and include weekends when work is expected. Existing historical notifications are retained. Configuration changes do not automatically rerun dates already checked.

## Effective access and job summaries

Managers can inspect a person's direct, inherited group, common-job and global-manager access sources, including roles, dates and inactive sources. Effective logging rights also account for closure and archive. Removing one source does not revoke other valid grants.

The job summary brings together progress, blockers, recorded time and links to related work. Managers can inspect collaborators; operators retain their personal timesheet scope. Job access does not grant access to contract PDFs.

## Actionable notifications

Task assignments produce personal notices. The operational feed also identifies tasks and blockers due within three days or overdue. Notices are deduplicated and filtered against live access and current responsibility. Reading a notice does not complete work. Inventory notices can open the asset and explicitly start or inspect contextual tickets when Ticket is configured. Internal notifications do not require SMTP.

## Guided setup and templates

Managers review job details, an optional contract when they also have Sales, team grants, initial tasks and a final summary. Job, grants and tasks commit atomically. Invalid references cancel the entire creation. Task assignees require existing operational access; assignment alone grants none.

Templates store explicit setup data and require review before use. They do not copy job codes, hours or documents. All references are revalidated on commit. Limits are 100 initial tasks, 200 direct grants, 200 group grants and 200 named templates.

## Close, archive and reopen

The lifecycle dialog shows open work and preserved data. Closing blocks new hours; archiving additionally hides the job from the current catalogue. Both retain authorized access to hours, tasks and blockers. Show archived retrieves archived jobs. Reopening requires an explicit reason; lifecycle revisions prevent stale overwrites and decisions enter the audit log.

Descriptive job status remains separate from logging closure. Permanent deletion requires a space administrator and remains destructive. It returns a conflict when consolidated hours, economic adjustments, budget or published contract revision references require retention; use closure and archive to preserve history.


## Schema 14 improvements

The twenty improvements following the workflow review are separate from the original twenty-feature roadmap:

1. Space administrators and dated, organizationally scoped manager delegations. Existing managers retain administration once during migration; new managers require explicit appointment. Administration never expands time visibility.
2. Sales/Finance profile requests approved by another administrator, with an audited single-administrator bootstrap exception.
3. Group membership and move impact previews, followed by live checks at confirmation.
4. Correction queues with status, person and date filters and cursor pagination.
5. UI and documentation aligned with schedules, team scope and current schemas.
6. Explicit personal device drafts for time, batch time and guided setup, expiring after seven days and cleared on logout/account change.
7. Daily batch previews including existing time, additions and final totals, with row-specific errors and server revalidation.
8. Selective copying of days and entries across up to seven days, with at most 100 entries per confirmed batch.
9. Scoped, expiring correction-approver delegation and immediate revocation checks.
10. Task estimates and personal capacity compared with scheduled and actual work.
11. Interval absence requests applied only after approval by another authorized manager.
12. Schedule revisions preventing stale updates and retaining before/after audit records.
13. Template editing, archived versions and explicit restoration, with revalidation before reuse.
14. Immutable published contract revisions and explicit job references to a published revision of the same contract.
15. Finance previews and approves immutable global economic periods; time in those periods is locked and later economic adjustments are separate, reasoned entries.
16. Job budgets combining closed snapshots, open-period actuals, explicit remaining cost and final forecast. Earlier reports are labelled live estimates.
17. Calendar and Ticket time proposals require personal review and explicit, idempotent confirmation.
18. Email outbox visibility, cancellation before sending and explicit review of uncertain results, without automatic resend.
19. Verified inventory person-to-Punto account mapping for My assets; shared identities do not expand personal results.
20. Closure checklists include corrections and explicit ticket/equipment references. Open items require a reasoned confirmation. External references are maintained manually, without automatic reconciliation.

Real Google OAuth and SMTP still require provider configuration. Local tests use isolated databases and simulated services; building files does not update a running deployment.
