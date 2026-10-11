# Frequently asked questions

[Italiano](../../prod/faq.md) | **English**

[Product index](README.md) · [User guide](guida-utente.md) · [Manager guide](guida-gestore.md)

These answers describe the current product as of 11 October 2026. For complete rules, see [Processes and rules](processi-e-regole.md).

## Access and projects

### Why can I see a project but not log hours against it?

Your project role may allow viewing only. Ask the manager to check the grant's role and dates. For an operator, the same grant allowing time entry must be valid both today and on the work date; two different grants, each valid on only one of the dates, are insufficient.

### The manager revoked a grant, but I can still see the project. Is this an error?

There may be another direct grant, group access or an enabled Shared project checkbox. The manager must check every applicable path. A single revocation removes only that link.

### Can I type a new project when logging hours?

The log uses the catalog of existing projects available to the user. Adding a new project requires a manager. A nonexistent project is rejected even when its identifier is sent directly to APIs.

### Does a “Completed” or “Cancelled” project prevent further hours?

Status describes the project, but does not by itself block entries. Current checks concern existence and authorization. To limit operators, the manager must manage roles, expiration or revocation, also considering groups and shared-project status. Managers retain global project access.

### Do General, Training, Meetings and Internal activities appear on every new installation?

They are the shared catalog adopted for the project, configurable by managers. They are not a mandatory automatic software seed: a new installation must create the desired projects and enable their Shared project checkbox. Active accounts created later can use existing shared projects.

## Tasks and blockers

### Does being a task owner give me project access?

No. Assignment indicates who follows the work; authorization determines what they can view or change. To update status, you must own the task and have Contributor access valid today, or be a Manager. Revocation prevents updates even if your name remains associated.

### Why can I see the board but not create tasks?

Creating, organizing and deleting tasks is the Manager's responsibility. The Contributor updates their own task statuses and reports blockers; the Viewer only views. These are the same project permissions, without a new global role.

### If I complete the parent, are subtasks completed too?

No. Each task retains its own status, owner and due date. Subtasks form a tree of up to eight levels; they are not yet temporal dependencies. To delete the parent, first move or delete its children.

### Does resolving a blocker put the task back In progress?

No. The obstacle register and Kanban status are independent. Update both when needed. A blocker may also affect the entire project and coexist with other obstacles on the same task.

### Who can edit a blocker?

The Manager can manage all blockers. An operator can update one if they created or own it and retain Contributor access. The manager can change the owner without changing the creator. Only the Manager can delete or reassign the blocker.

### Where have resolved blockers gone?

The register initially shows **Open and being handled**. In **Show blockers**, choose **All** or **Resolved**. Resolved items still count toward the 500-blocker limit per project; completed tasks also count toward their corresponding 500 limit.

### Why does saving report a changed version?

Another change updated the same item. The app prevents overwriting it with a draft based on older data. Note your edits, use **Close and reload the updated version**, and compare content before retrying. Automatic merging of changes is not provided.

### Can I log hours against a specific task or receive alerts about its due date?

These links remain to be developed. Hours belong to the project, and the **Work done / note** field is descriptive. Board deadlines, assignments and statuses do not produce automatic emails or notifications. Dependencies, Gantt and workload planning remain on the roadmap.

### What happens to blockers if I delete a task?

Blockers stay in the project register and lose their link to the deleted task; their revision changes. Deleting the entire project instead also removes tasks, blockers and hours. The [module guide](attivita-kanban-e-blocchi.md) summarizes every case.

## Entries and analyses

### How do I enter half an hour or an hour and a half?

In the hours field you can use `0:30` or `1:30`, or the corresponding decimal format. `1,30` in decimal format means 1.3 hours, or 78 minutes: it does not mean an hour and thirty minutes. Saving must produce a whole number of minutes.

### Can I correct hours already entered?

A manager can edit or delete their own entries. An operator can add and view entries; a correction request and approval workflow is planned. The manager does not have an ordinary way to edit rows belonging to another user. For operational anomalies, contact the manager without duplicating an entry in an attempt to correct it.

### Can the manager see my hours?

They can view aggregates in analyses by period, project and user. The detailed Time log remains personal, and analyses do not show activity notes. The manager also has access to Change history: audit events can contain before/after operation details. Analysis and audit permissions are therefore separate.

### If I lose project access, do I lose my personal history?

Revoking a grant preserves previous hours and personal access to them. Permanent project deletion instead removes linked tasks, blockers and hours for all users. Before deletion, the manager must consider the effect on historical data; the app displays a dedicated confirmation.

### Can permanent deletion be undone?

There is no recycle bin. Recovery, if possible, requires a backup and coordinated technical intervention. History retains the deletion event, but does not offer a button restoring all removed rows.

### Why do Contract value and Labor cost show different numbers?

They measure different things. Contract value shows the entire value of contracts linked to selected work, counting each contract once. Labor cost applies rates to individual entries. The first value is not allocated across days, projects or people and does not represent revenue accrued in the period.

### Why do some hours have no cost?

No valid rate exists for the user and entry date. Hours remain marked as unpriced; an explicit zero rate is a different case. Changing a rate can also change estimates for past work, because these are not frozen accounting snapshots.

### Does an analysis link grant access to data?

The URL preserves view and filters. Whoever opens it must still sign in and have the necessary permissions. An operator remains limited to personal entries; financial views are restricted to managers.

## Alerts, documents and integrations

### When does an alert appear for a day with fewer than eight hours?

The day must be Monday–Friday, and 48 actual hours must have elapsed from its end in the Europe/Rome time zone. The anomaly is detected at the rule's next daily check, or with Run now once the grace period has elapsed. For example, Monday ends on Tuesday at 00:00 and becomes eligible for checking on Thursday at 00:00.

### Does the check consider leave, public holidays and part-time work?

The current criterion is fixed: eight hours for each Monday–Friday day. There is no calendar of holidays, absences or individual schedules yet. Those exceptions are planned on the roadmap and must not be assumed when reading alerts.

### Does “Nonexistent project” mean hours were saved without a project?

The check reports rejected attempts using nonexistent or deleted identifiers. The invalid entry is not saved. Normal project deletion also removes related hours in the same transaction, avoiding orphaned rows.

### Why is a notification present in the app but the email does not arrive?

The two channels have separate outcomes. SMTP needs configuration: alerts intended for email can remain awaiting configuration. When the channel is active, acceptance by the SMTP server will not by itself guarantee mailbox delivery. A send with an uncertain outcome requires investigation and is not automatically repeated.

### Why does my rule not produce alerts from before its creation?

Monitoring begins when the rule is created and when the user enters its monitoring scope. It is not an indiscriminate retrospective check of all history. Disabled rules do not run automatically; Run now allows a manual check. Previously notified anomalies are deduplicated.

### If I disable a rule, do prepared emails stop too?

Disabling suspends automatic checks but does not delete the existing email queue. When SMTP is active, pending sends can be processed if the owner is still an active manager and the address remains among recipients. Delivery already started is not cancelled. Before enabling the provider, the technical contact must therefore also check the preexisting queue.

### Why does an operator recipient not receive alerts about other people?

Internal notifications respect the recipient's scope. An operator receives only anomalies about their own work; a manager recipient can receive those of monitored users. Selecting someone as a recipient does not grant the Manager profile.

### Does uploading a PDF replace the previous one?

Contract documents are retained as separate originals. A new upload adds a document; it is not yet a system of approved contract versions. Checking the `%PDF-` signature, size and fingerprint verifies some technical requirements but does not perform OCR, antivirus scanning or legal validation of the contract.

### Where do I connect Google Calendar and turn an event into hours?

The service and synchronization APIs are implemented. Google OAuth needs configuration and the Calendar screen still needs to be built. Linking events to projects and converting events to hours are also on the roadmap; appointments do not automatically generate work entries.

### Can I already issue invoices or manage multiple companies and currencies?

The current product offers contracts and financial estimates in EUR. Invoicing, receipts, payments, company context and multiple currencies are described in the [financial plan](piano-modulo-finanziario.md) and remain to be implemented.

## Useful support information

When reporting a problem, specify the section, action, work date, displayed message and, if needed, project code. Avoid including passwords, OAuth codes, session cookies or confidential contract content. For availability or configuration problems, the manager can involve the system administrator using the [operations guide](../tehnical/operazioni-backup-ripristino.md).
