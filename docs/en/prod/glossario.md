# Glossary

[Italiano](../../prod/glossario.md) | **English**

[Product index](README.md) · [Processes and rules](processi-e-regole.md)

The following terms describe the current product. Entries referring to future development say so explicitly.

## People and access

| Term | Meaning in punto |
| --- | --- |
| User / account | Personal identity with a name, email, derived password and active or deactivated status. Entries belong to its stable identifier. |
| Manager | Account profile that administers the workspace: master data, projects, organization, rates and personal alert rules. Also views aggregate analyses for multiple users. |
| Operator | Profile that views authorized projects, logs personal hours, updates assigned tasks and manages personal blockers when the project role permits it. |
| Account role | Global Manager or Operator profile. Separate from the role granted on a project. |
| Project role | Set of operational permissions on a project: viewing and, if enabled, logging hours and collaborating on assigned tasks and personal blockers. Does not grant global administration. |
| Viewer | Predefined project role allowing viewing without logging hours. It is not a third account profile. |
| Contributor | Predefined project role allowing viewing, logging personal hours, updating assigned task statuses and managing blockers they created or own. |
| Group | Set of users that can be organized in a hierarchy. A user can belong to multiple groups. |
| Subgroup | Child group that benefits from access granted to its ancestor groups. |
| Membership | Direct link between a user and a group. Separate from granting that group access to a project. |
| Access grant / grant | Link between a project and a user or group, with a role and validity interval. |
| Effective access | Result of direct links, groups, temporal validity, roles and shared-project rules. |
| Revocation | Removal of a grant. Other valid paths may continue to provide access. The history of previously logged hours remains personal. |
| Deactivation | Blocking an account's access while retaining its historical data. Different from deleting a project. |

## Work and documents

| Term | Meaning in punto |
| --- | --- |
| Project | Unit to which work is attributed: unique code, name, client, description, status and optional contract. Requests referring to a “project” mean this entity. |
| Shared project | Project the manager makes viewable and usable by all active accounts without individual assignments. |
| Project status | Work-cycle label: Planned, In progress, Paused, Completed or Cancelled. Does not, by itself, prevent logging hours. |
| Task | Project work item with a title, description, status and priority; owner and due date are optional. Hours are not yet linked to individual tasks. |
| Subtask | Task linked to a parent in the same project. The hierarchy reaches eight levels and retains independent statuses. |
| Kanban / board | Five-column task view: To do, In progress, Blocked, To review and Completed. |
| Task owner | Person responsible for following it. Assignment does not automatically grant project access. |
| Blocker | Obstacle affecting a project or one of its tasks, with impact, owner, status, next action and due date. |
| Impact | A blocker's operational severity: Low, Medium, High or Critical. Does not automatically calculate financial losses. |
| Next action | Described step for addressing a blocker; not a new automatically created task. |
| Task/blocker revision | Number changing with each update that prevents overwriting a newer version with a stale edit. |
| Dependency | Temporal relationship between tasks planned for later development; not the same as subtask parentage. |
| Time entry | Personal row consisting of project, work date, duration in minutes and optional note. Does not contain a start/end time range. |
| Work day | Date to which the hours refer, separate from the moment they are saved. |
| Minutes | Whole-number storage unit for durations. The UI allows entry and display in hours, but saving does not allow fractions of a minute. |
| Personal history | User entries, viewable even when a related grant is revoked; permanent project deletion instead removes linked hours. |
| Supplier | Master record of the party to which contracts are linked. The catalog is not yet a full financial customer/supplier master database. |
| Contract | Registered agreement with supplier, code, title, status, dates, optional EUR value and PDF documents. Can be linked to multiple projects. |
| Contract document | Original PDF uploaded to a contract, retained with metadata and an integrity fingerprint. Uploading another PDF adds a document. |
| Document integrity | Check that the downloaded PDF's size and fingerprint match the recorded values. Not equivalent to legal validation, antivirus scanning or a digital signature. |
| Permanent deletion | Removal without an application recycle bin. For a project, includes tasks, blockers, every user's hours and linked grants. |
| Change history / audit | Record of who made changes, when and with which before/after values. Separate from the daily time log and not a substitute for a backup. |

## Analysis and finances

| Term | Meaning in punto |
| --- | --- |
| Period | Date interval selected in analyses; both endpoints are inclusive. For a single day, From and To are equal. |
| Aggregate | Summary of multiple entries by date, project or user. Does not automatically expose detailed notes on other people's hours. |
| Contract value | Full contract amount, counted once in the view when linked to the selected work. Not allocated by day or project. |
| Hourly rate | EUR amount per hour assigned to a user and date interval, used to estimate the cost of their work. |
| Estimated labor cost | Sum of costs of individual entries covered by a rate, rounded to the cent per entry. |
| Unpriced hours | Hours without an applicable rate. Do not represent work with zero cost. |
| Undefined amount | Missing value. Separate from an amount explicitly equal to zero. |
| EUR / cents | Currently supported currency and the whole-number unit used for financial values. |
| Invoice, receipt, payment | Entities in the planned financial module. Current analysis pages do not issue invoices or record financial movements. |
| Multi-company / multi-currency | Isolation and calculations for different companies and currencies planned in the financial plan; unavailable in the current schema. |

## Checks and integrations

| Term | Meaning in punto |
| --- | --- |
| Alert rule | Manager's personal check, with type, name, daily time, enabled status and recipients. |
| Trigger | Condition producing a report: an attempt on a nonexistent project or an incomplete day after the grace period. |
| Grace period | For missing hours, 48 actual hours from the end of the Monday–Friday day. The alert arrives at the rule's next check. |
| Recipient | App account or email address selected for a rule. The recipient's role limits the information they can receive in the app. |
| Internal notification | Account alert, viewable in Notifications and markable as read. |
| Email queue / outbox | Persistent storage of alerts to be sent and their outcomes. Presence in the queue does not mean the email has been sent. |
| SMTP acceptance | Confirmation that the mail server accepted a message. Does not certify delivery to the final mailbox. |
| Addon | Separate service called by the app through private APIs: one for email and one for Google Calendar. |
| OAuth | Consent by which the user authorizes linking their Google account. The technical administrator must configure the service first. |
| Bidirectional synchronization | Importing changes from Google Calendar and sending operations requested through app APIs to Google. |
| Calendar conflict | Remote event changed since the version on which a request is based: reported without automatic overwrite. |
| Idempotency | Repeating the same operation preserves its already recorded result, avoiding a new effect. |

For field types, identifiers and API statuses, see the [data model](../tehnical/modello-dati.md) and [API catalog](../tehnical/api-app.md).
