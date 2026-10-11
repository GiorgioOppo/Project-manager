# Product processes and rules

[Italiano](../../prod/processi-e-regole.md) | **English**

This document collects current behavior affecting work organization and data interpretation. Procedures with interface commands are in the [user](guida-utente.md) and [manager](guida-gestore.md) guides.

## 1. Functional profiles, teams and project roles

Every active account has implicit Operator access; Manager derives from the identity role. Sales and Finance are explicit profiles assigned locally in **Users → Profiles**, including to Keycloak accounts. No manager receives them automatically. Project roles remain separate and govern the operator’s viewing and time entry on that resource.

A manager’s team includes themselves, members of their organizational groups and their subgroups. Without groups, or when company groups are unavailable, they see only their own hours. This scope applies to detailed day/week logs, work reports, time summaries, corrections, audit and notifications. Finance does not enlarge this team.

| Action | Manager | Operator |
| --- | --- | --- |
| View projects | All | Shared or with a grant valid today |
| Create, edit and delete projects | Yes | No |
| Read tasks and blockers | All projects | Shared or with a grant valid today, including Viewer |
| Create, organize and delete tasks | Yes | No |
| Change a task's status | All | Only assigned to themselves, with Contributor access valid today |
| Report a blocker | Yes | With Contributor access valid today |
| Edit a blocker | All, including owner | Only if creator or owner and still Contributor; cannot change owner |
| Delete a blocker | Yes | No |
| Log personal hours | On all existing projects that are neither closed nor archived | On projects that are neither closed nor archived, shared or with time-entry authorization valid today and on the work day |
| Read personal log | Yes | Yes, even after grant revocation |
| Edit or delete personal hours | Yes | No |
| Edit others' individual hours | Only by approving a correction requested by another team member; no generic editor | No |
| Read detailed hours and work Analysis | Own team, including read-only day/week logs | Personal entries only |
| Administer users, groups, roles and grants | Yes | No |
| Create and edit alert rules | Personal rules only | No |
| Read notifications | Addressed to their account; time details limited to their own team | Addressed to their account and about themselves |
| View History | Yes, with team time scope and profile-based financial details | No |
| Use Calendar APIs | Personal connection only | Personal connection only |

The server repeats verification. Hiding a button is a consequence of permissions, not the primary protection. Changes to profile, grants, groups and operational roles also apply to open sessions. Account deactivation revokes sessions; reactivation requires a new login.

| Financial data | Required profile |
| --- | --- |
| View suppliers, contracts, PDF metadata and download originals | Sales or Finance |
| Create/edit suppliers and contracts, upload PDFs | Sales |
| Change a project’s contract link | Manager + Sales |
| Read Contract value, Labor cost and hourly rates | Finance |
| Create/edit hourly rates | Manager + Finance |

Finance is read-only and receives financial reports aggregated by project, without third-party personal entries or person filters. The Manager role alone does not open contracts, PDFs or costs. Organizational and functional assignments are separate; revocation is checked even for known resources and open sessions.

## 2. How project access is determined

For an operator, the system considers:

1. Direct project authorization.
2. Grants assigned to membership groups and their parent groups.
3. **Shared project** option.

Grants combine without an explicit denial mechanism. Viewer does not take precedence over Contributor obtained through another path. A grant to a parent group reaches subgroup members; a grant to a subgroup does not authorize sibling groups or all parent members.

Start and end dates are inclusive. An absent date removes only that side's limit. Viewing requires a grant valid today. Writing hours requires **the same authorization link**, with time-entry capability, valid both today and on the work date.

Example: a Contributor authorized from 1 to 31 October can log work from 10 October on 15 October. On 1 November, once the period expires, they can no longer use that link for new entries for 10 October. Another grant valid only in November does not combine the two periods retrospectively.

Revoking one link can leave other paths active. Making a project non-shared does not remove direct or group assignments. The Manager profile does not depend on these grants to work on projects.

## 3. Statuses, closure and deletion

Project statuses are Planned, In progress, Paused, Completed and Cancelled. These are manually managed classifications; no mandatory transitions or approvals exist. Status labels alone do not prevent new hours. Closing or archiving with a reason blocks new entries for everyone, including managers, without deleting data. Reopening requires a reason; the summary and revision protect the decision.

Contract statuses are Draft, Active, Expired, Closed and Cancelled. A user with Sales also edits these; the end date does not generate an automatic transition. Closed or Cancelled contracts cannot be selected for a new project link, while previous links can remain.

| Operation | Existing hours | Access | Other data |
| --- | --- | --- | --- |
| Change project status | Retained | Unchanged | Update in History |
| Close or archive a project | Retained | Viewing retained; new hours blocked for everyone | Data retained, reason and revision in History |
| Revoke a grant | Retained and viewable by their creator | That path removed; others can remain | Project remains |
| Deactivate a user | Retained in aggregates and history | Login blocked | Account retained |
| Delete a personal entry as Manager | That row removed | Unchanged | Event in History |
| Delete a project | All linked hours for all users removed | Project's direct and group grants removed | Tasks and blockers removed; users, groups, roles and contracts retained; summary in History |

Project deletion is permanent in the app and performed in one transaction with removal of links, hours, tasks and blockers. History is not a recycle bin. Retention required by the future financial module will introduce further rules for priced or invoiced sources; those rules are not already active.

### Tasks, hierarchy and obstacles

A task belongs to a single project, immutable after creation. Any parent must belong to that same project: cycles and hierarchies beyond eight levels, counting the root as level one, are forbidden. Moving a branch must respect the limit for all descendants too. Parentage organizes work but is not a temporal dependency.

Task statuses are To do, In progress, Blocked, To review and Completed; priorities are Low, Normal, High and Urgent. Parent, child and project statuses remain independent. The manager can change all details; an assigned Contributor can update only status. Tasks check access valid **today**; a due date is neither a work date nor an additional grant condition.

A blocker affects a project and, optionally, one of its tasks. Its impact is Low, Medium, High or Critical; status Open, Being handled or Resolved; owner, next action and due date are optional. An operator initially becomes its owner. Creator or owner can update it while Contributors; only a manager can change owner or delete it. Changing owner does not change creator.

Assigning a person requires them to be active and a Manager or Contributor on that project. A preexisting reference can remain after deactivation or revocation but does not grant access. Changing owner must select a currently authorized person. The board does not expose contracts, PDFs or other users' personal hours.

Every edit or deletion of an existing task or blocker checks a revision. A stale revision produces a conflict without automatic overwrite. Detaching blockers from a deleted task also changes their revision. Changes and History events are atomic.

Task deletion is rejected if it has children; linked blockers are retained and detached. Project deletion instead removes all tasks and blockers. Reporting or resolving a blocker does not change task status; completing a task does not log hours or generate automatic notifications. The [module guide](attivita-kanban-e-blocchi.md) describes interface commands.

## 4. Time log rules

Each entry contains person, project, day, minutes, an optional note and an optional link to a single task from the same project. The note remains descriptive. The person is taken from the session and stably identified; it is not a browser-editable field.

- Duration per row: 1 to 1440 minutes.
- Total per person and day: maximum 1440 minutes across all projects.
- Multiple rows on the same project and day: allowed.
- Maximum 1000 entries per person per day.
- Notes: up to 1000 characters.
- Past and future dates: allowed if valid and compatible with grants.
- Start and end times: not recorded; the system retains a duration, not time slots.

Durations are whole minutes. Decimal format is converted exactly: `1,5` = 90 minutes; `1:30` = 90 minutes; `1,30` = 78 minutes. A value requiring fractions of a minute is rejected.

The 24-hour limit is an integrity constraint. The alert threshold, defaulting to 8 hours on weekdays and configurable through schedules and exceptions, is a separate informational check: fewer or more than 8 hours can be saved, within 24 hours, without automatic timesheet approval.

The weekly grid, copying the previous week with a preview, atomic multiple insertions and operator-requested corrections are available. Corrections preserve the original and proposal and require a decision by a team manager other than the author. Multiple-entry saving uses an idempotency receipt; for a single entry with an uncertain outcome, check the log before entering it again. Copying a single day, general timesheet approval and offline saving remain to be developed.

## 5. Relationship between suppliers, contracts, documents and projects

A contract requires a supplier; a project can link a contract or remain without one. A commercial sales relationship must not be inferred solely from the project's Client field or a supplier contract's value.

Supplier, contract and project codes are unique in their respective catalogs. Deactivating a supplier retains historical references but excludes it from new links. Contract values are optional and expressed in EUR; zero and missing value are distinct.

PDFs are additional originals: each upload creates a new document, even when name or content match. The system saves the file fingerprint and checks it on download, but has no duplicate recognition or contract versioning workflow yet. Document editing and deletion are unavailable through current APIs.

The per-file limit is 10 MiB, with up to 1000 documents per contract. Name, initial signature, size and integrity are checked; accepting a file does not certify completeness, legal validity or absence of malicious content. Content is not automatically extracted.

## 6. Interpret Analysis correctly

Date filters include first and last day and allow at most 366 days. A single date uses the same From and To value. Counts derive from all filtered entries; result groups are limited to 1000 and exceeding this produces an explicit error.

The manager can aggregate people in their own team, including deactivated ones. The operator sees only themselves, including personal history on revoked projects. Entry notes are not included in reports; Kanban tasks are not aggregated in time analyses. Saving or sharing a filtered URL does not grant additional permissions.

Financial views require Finance, do not accept the User filter and do not return other people’s personal entries. Labor cost groups by project. Finance can read rates; editing requires Manager + Finance.

### Contract value

The report identifies contracts **currently linked** to projects with hours in the period and counts each contract once. It does not allocate a share of value to hours, the person or the interval. A contract without an amount remains unknown; a zero-euro contract is explicitly valued at zero.

If two projects with hours in the period share a 10,000 EUR contract, known total is 10,000 EUR. Changing a project's linked contract also changes analysis of its past hours. It is not an automatic revenue or margin calculation.

### Labor cost

For each entry, the person's rate valid on the work date is found. Rate dates are inclusive; two intervals for the same person cannot overlap. An individual row's cost is:

```text
cost in cents = round to the nearest cent
                (minutes × hourly rate in cents / 60)
```

Half values round upward, and calculation happens **before summing**, for each row. At 1 EUR/hour, three one-minute entries are worth 0.02 EUR each, or 0.06 EUR; a single three-minute entry is worth 0.05 EUR. The difference follows from per-entry rounding.

A zero rate assigns a known zero cost. Without a rate, duration is reported as unpriced and does not contribute as zero cost. If all hours lack rates, the estimate is unavailable; if only part is covered, the estimate is partial.

Rates are in EUR, with a maximum of 999,999.99 EUR/hour. The report is recalculated: rate changes alter historical estimates without changing minutes. Sales rates, accounting snapshots, currency exchange and locked financial periods do not yet exist.

## 7. Alerts: timing, recipients and meaning

A rule contains owner, type, name, daily time, enabled status and recipients. Each manager manages personal rules. If the owner is no longer active or a Manager, automatic execution is suspended.

### Nonexistent project

This check collects rejected attempts to log hours against a nonexistent or deleted project identifier. Hours were never saved. Normal project deletion removes its hours: the alert therefore neither reconstructs deleted hours nor performs a generic database integrity scan.

Events predating rule creation are not notified as new anomalies. Lack of authorization on an existing project does not by itself produce this event type.

### Missing daily hours

Days of monitored active users with expected minutes greater than zero are examined. A total below the expected minutes produces an alert after at least **48 actual hours from the day’s end**, interpreted in Europe/Rome. Personal exceptions take precedence over holidays, followed by the latest effective schedule; without configuration, the default is 480 minutes Monday–Friday and zero Saturday–Sunday.

Example without a daylight-saving transition: Monday ends Tuesday at 00:00. The grace period expires Thursday at 00:00; a 09:00 rule evaluates it Thursday at 09:00. The default calendar expects no weekend hours, but an individual schedule can require them. Chronological weekend hours count toward the grace period. A rule can also run Saturday or Sunday to detect a weekday that has become eligible.

The change between standard and daylight-saving time does not turn the grace period into “two calendar dates”: it remains 48 actual hours. Managers configure holidays, leave and individual schedules in **Schedules and absences**. Schedule effective dates preserve the expectations for earlier days; personal exceptions define expected minutes for a particular date. Active users in the owner’s team are monitored, including managers; selected recipients receive only time details they are authorized to read.

Monitoring begins at rule creation. A user added later is included from the first check registering them as a subject, without recovering periods before their monitoring began. A previously examined day is not continuously rechecked: the notification preserves the detected total, even if hours are later added or deleted. It does not represent an always-current approval status for the day.

### Scheduling and channels

The chosen time's zone is Europe/Rome. Automation saves the last day executed and does not repeat that run after a normal restart. A new rule created after today's scheduled time waits until the next day. **Run now** allows a manual check, including with a disabled rule; deduplication avoids notifying anomalies already processed by that same rule again.

A manual run updates the processed day and prevents a second automatic run on the same day. Suspending the rule does not delete pending emails; delivery checks that owner and recipient remain permitted, separately from enabling the daily run. The [alert engine](../tehnical/motore-alert.md) describes cursors and restart cases.

Up to 50 user recipients and 50 email addresses per rule are allowed, with at least one recipient overall. Time alerts respect both owner and recipient scope: an Operator receives only personal events, a Manager those of their own team. Email addresses must also correspond to authorized active accounts; external addresses do not receive time details. Delivery rechecks current permissions. Inventory rules retain their shared-catalog scope.

The email channel retains queued sends while SMTP is absent. Outcomes can distinguish awaiting configuration, retry, SMTP acceptance, uncertain delivery and error. Uncertain delivery requires investigation and is not automatically resent. Technical states do not yet form an outbox administration page in the interface.

## 8. Notifications and history

A notification belongs to a recipient. **Mark as read** updates acknowledgment, not the cause of the alert. The list shows the latest 100 accessible notifications, while the counter can include older unread ones.

Time alerts that are no longer authorized are excluded from the list, unread count and read actions even when produced before revocation. The same personal or team scope protects correction originals and proposals and time details in History.

History is a record of authorized changes. Application save and event are atomic; a history error also prevents the change. Events preserve actor, date and before/after details. Passwords, password derivations and salts are not stored in audit payloads. Rejected errors do not become successful-change events; attempts on nonexistent projects instead have the dedicated alert-event channel.

## 9. Displayed dates and time zones

Work days are dates, without clock-in and clock-out times. Analyses use the server-returned date for quick periods. The Time log and some UI indicators use the browser's local date; the server remains authoritative for access validity. Near midnight, devices in different time zones can display different indications: check the entry's explicit date.

Notifications and alert times are shown in Europe/Rome; History displays instants in the browser's local zone. Full unification of company time context is planned in the roadmap's technical foundation.

## 10. Operational capacity

| Object | Current limit |
| --- | --- |
| Users | 200 |
| Groups | 200, maximum 16 levels |
| Project roles | 100 overall |
| Projects | 1000 |
| Tasks | 500 per project, including completed; maximum 8 levels |
| Blockers | 500 per project, including resolved |
| Task and blocker text | Title up to 160 characters, description up to 2000, next action up to 1000 |
| Suppliers | 1000 |
| Contracts | 1000 |
| PDFs | 10 MiB per file, 1000 per contract |
| Hourly rates | 1000 |
| Time entries | 1000 per user per day; maximum total 24 hours |
| Analyses | Maximum interval 366 days; maximum 1000 result groups |
| Alert rules | 100 per owner |
| Alert recipients | 50 users and 50 emails per rule |
| Notification list | Latest 100 accessible; unread count calculated separately |

Do not bypass a reached limit by creating duplicate identities or fictional time records. Modules with a registration limit return an explicit error; expansion requires a verified product change.

## 11. Integrations and data protection

Google Calendar has a service and APIs for personal connection, calendar selection, event reading and bidirectional changes. OAuth still needs configuration and the graphical path is unavailable. An event does not automatically generate hours, and the manager does not gain access to other users' private calendars.

The service detects conflicts on remote changes and does not promise automatic overwrite of the latest version. Local storage also retains recurring series and exceptions, without currently expanding them into every occurrence in a graphical view. The event–project link and proposed duration will need to be designed with user confirmation.

Application permissions do not mean every datum is encrypted. Calendar storage encrypts tokens and content at rest but decrypts them for processing. Login passwords are retained as irreversible derivations; application fields and PDFs do not yet have the vault required in the longer term. Database replication protects availability against some failures, but does not automatically recover propagated deletions: [backups](../tehnical/operazioni-backup-ripristino.md) are needed for that.
