# Manager guide

[Italiano](../../prod/guida-gestore.md) | **English**

The **Manager** administers the workspace and also uses their personal Time log. Personal sign-in, time-entry and notification features are described in the [user guide](guida-utente.md). This document follows the recommended order for preparing people, projects and checks.

## Prepare the workspace

1. Register people in **Users**.
2. Organize departments and teams in **Groups and roles**, when group management reduces individual assignments.
3. Register **Suppliers** and **Contracts** for projects with a reference agreement.
4. Create **Projects** and define which are shared and which require specific grants.
5. Assign users or groups with appropriate roles and dates.
6. Organize **Tasks and blockers** for each project, with owners, due dates and next actions.
7. Enter **Hourly rates** to estimate labor cost.
8. Configure **Alerts and triggers** with time and recipients.
9. Use **Analysis**, **Notifications** and **History** for subsequent checks.

Settings do not automatically assign hours. Reporting remains an action by the person who performed the work.

## Create and manage users

Open **Users** and press **New user**. Enter Name, Email, Initial password and Role, then press **Create user**. The initial password must have at least 8 characters; it is not shown in the list or history. Deliver credentials through your organization's chosen channel.

Choose **Operator** for people who must view authorized projects and log personal work. Choose **Manager** for people who must administer the workspace: this profile has global powers in the current workspace, not limited to a department.

With **Edit**, change name, role and **Active / Deactivated** status. Email identifies the account and cannot be edited. This form has no password reset.

| Operation | Effect |
| --- | --- |
| Change name | Updates displayed identity without transferring hours to another account |
| Change Manager to Operator | Reduces powers from the next request, even with an open session |
| Deactivate | Revokes sessions and prevents access; history is retained |
| Reactivate | Allows a new login; does not restore revoked sessions |
| Deactivate or demote the last active Manager | Rejected to retain someone responsible for the workspace |

Current management allows up to 200 users. Deactivated accounts can still appear in managers' historical analyses.

## Build the group tree

1. Open **Groups and roles**.
2. Press **+ Group** in **Group tree**.
3. Enter **Name** and choose **Parent group**, or leave **Top level**.
4. Press **Create group**.
5. Select the group in the tree.
6. In **Add an active user**, choose the person and press **Add to group**.

The record shows the selected group's direct members. A user can belong to multiple groups. Grants assigned to a group apply to its members and subgroup members, even when the latter do not appear in the direct-member list.

Example: if “Area tecnica” contains “Sviluppo” and “Supporto”, granting Area tecnica access to a project includes both subgroups. Granting only Sviluppo does not grant Supporto or all parent-group members access.

With **Edit**, rename or move a group. Moving can immediately change access inherited by members and subgroups. The system prevents cycles and hierarchies beyond 16 levels; at most 200 groups are allowed. **Delete** is permitted only when the group has no members, subgroups or project grants: remove or relocate relevant references first.

## Define project roles

Predefined roles are:

| Role | Viewing | Logging personal hours |
| --- | --- | --- |
| Viewer | Yes | No |
| Contributor | Yes | Yes |

In **Project roles**, press **+ Role** to add a variant. Enter Code, Name and choose **View only** or **View and log your own hours**. The code begins with a lowercase letter and may contain lowercase letters, numbers, hyphens and underscores, up to 40 characters.

Roles allowing time entry also enable board collaboration: the operator updates assigned task statuses and creates or edits personal blockers. Viewers only view. Choosing a task or blocker owner does not create project authorization.

Predefined roles cannot be edited. Custom-role changes apply to every grant using them. The overall limit is 100 roles. An operational role does not permit creating projects, managing accounts or accessing contract PDFs.

## Register a supplier

1. Open **Suppliers** and press **New supplier**.
2. Enter a unique **Code** and **Company name / name**.
3. Complete VAT number, Email, Phone, Address and Notes when available.
4. Keep **Active** status to allow selection in new contracts.
5. Press **Create supplier**.

Search works by name, code or VAT number; the filter distinguishes Active and Deactivated. Editing data and deactivating do not delete linked contracts. A new contract can select only an active supplier; an existing link can be retained. This section has no supplier qualification process or documentary certification check.

## Register a contract and its PDFs

1. Open **Contracts** and press **New contract**.
2. Enter a unique **Code** and **Title**.
3. Select **Supplier**. If missing, register it in Suppliers first.
4. Choose **Status**.
5. Optionally fill **Start date**, **End date**, **Value in euros** and Notes.
6. Press **Create contract**.

| Status | Suggested organizational meaning |
| --- | --- |
| Draft | Agreement in preparation |
| Active | Ongoing agreement |
| Expired | Term elapsed, to check or renew |
| Closed | Concluded agreement |
| Cancelled | Agreement no longer used |

The manager selects these labels: passing the end date does not automatically change status. End cannot precede start. Value is optional and uses EUR; an empty field means **undefined value**, while `0` is an explicit amount. Write amounts without thousands separators and with at most two decimals, for example `12500,00`.

For documents:

1. In the contract row, press **PDF · number**.
2. In **Add the original PDF**, choose the file.
3. Press **Upload PDF** and wait for it to appear in the list.
4. Use the document download command to obtain the original.

PDFs up to 10 MiB each are accepted. Each upload adds a document: it does not replace one with the same name. Deleting, editing and inline document preview are unavailable. Metadata includes a content fingerprint, and downloading checks byte integrity. Uploading does not perform OCR, automatic classification or antivirus scanning.

The archive is restricted to managers. Granting an operator access to a linked project does not grant access to contracts or PDFs.

## Create, update and close a project

1. Open **Projects** and press **New project**.
2. Enter a unique **Project code** and **Project name**.
3. Set **Status**.
4. Optionally fill Client and Description.
5. If needed, select a registered **Contract**.
6. Decide whether to enable **Shared project**.
7. Press **Create project**.

Available statuses are **Not started**, **In progress**, **Paused**, **Completed** and **Cancelled**. There is no mandatory transition order: the manager can update status according to work. **Status does not block time entry.** To prevent new hours, manage project permissions and, where relevant, disable the shared option.

A project can refer to only one contract, while the same contract can be used by multiple projects. A new link cannot use a Closed or Cancelled contract. If an already-linked contract changes status, the reference can remain. Selecting **No contract** removes the link.

Client is a descriptive field. It does not automatically create a financial customer, sales contract or invoice.

To prepare shared activities, check or create **Generale**, **Formazione**, **Riunioni** and **Attività interne**, with distinct codes and the **Shared project** checkbox. All active accounts can view them and use them for personal hours. Creating the four entries is a manager task, not a mandatory seed for a new installation.

## Grant access to users and groups

1. In the project row, press **Permissions**.
2. For a person, use **Grant user access**, select User and **Role on the project**.
3. If needed, enter **From date** and **To date**, then press **Grant user access**.
4. For a team, use **Group permissions**, choose Group, Role and dates, then press **Enable group**.

Endpoints are inclusive; an empty date field leaves that side unlimited. One direct assignment is configured per user–project pair and one per group–project pair: editing updates the link's role and period.

Operator viewing requires a grant valid today. Logging requires **the same grant** to allow writing and cover both today and the work date. Two separate grants, each valid on only one of the dates, do not combine for this check.

A person can receive permissions through multiple paths. Viewer does not cancel Contributor; if at least one valid path permits logging, the action remains allowed. **Confirm revocation** removes the selected link. To remove access entirely, also check other groups, parent groups, direct assignments and any Shared project checkbox. The Manager profile retains global powers independently of these grants.

Changes also apply to open sessions. Previous personal entries remain viewable after a simple revocation.

## Organize tasks, subtasks and blockers

1. Open **Tasks and blockers** and choose a project, or use **Tasks** in its row.
2. Press **New task**, enter **Title** and, if useful, **Description**.
3. Choose **Owner**, **Priority**, **Status** and optional **Due date**. The owner must be active and have Contributor project access, or be a Manager.
4. For a subtask, choose **Parent task**, or start from **+ Subtask** on the card. You can reach eight overall levels; circular references are rejected.
5. Press **Create task**. Use **Edit** to revise details or **Status → Apply** to move the card quickly.
6. View **Kanban board** or **List and subtasks**, and use search, status and priority to focus work.

The five statuses are To do, In progress, Blocked, To review and Completed. There is no mandatory sequence; completing a parent does not complete its children. Each task retains its own owner and due date. The module does not yet contain duration estimates, dependencies or Gantt charts.

For obstacles, use **Report blocker**, enter impact and next action, and choose an owner or leave **Unassigned**. The blocker may refer to the whole project or a task. In the register, use **Manage** to change status to Open, Being handled or Resolved. Blocker status does not move the Kanban card. The initial filter includes only open items and those being handled.

The operator can report blockers and update those they created or own while retaining Contributor access. Only the Manager changes owners, organizes tasks or deletes items. If a person loses access, the historical reference remains: consider reassigning work to an authorized person.

Deleting a task first requires moving or deleting children. Any blockers remain in the project without a link to the deleted task. Confirmation is specific to the item and does not change logged hours. Up to 500 tasks and 500 blockers are allowed per project, including completed or resolved items.

Changes use a revision: a save based on an earlier version is rejected. On conflict, note your edits before closing the draft, reload data and compare it. History records tasks, blockers and blocker detachment when a task is deleted.

Hours remain associated with the project. Task deadlines and assignments do not automatically generate alerts or email. Details and permission matrix: [Tasks, Kanban and blockers](attivita-kanban-e-blocchi.md).

## Permanently delete a project

Open **Edit** on the project and use **Delete project**. Read the summary in the dedicated window; final confirmation is **Delete project and hours**.

The operation deletes the project, **tasks, subtasks, blockers and all linked hours for every user**, plus direct and group grants. Data disappears from logs and analyses. Users, groups, roles, contracts and change history remain; a deletion summary is added to history.

Deletion has no recycle bin or undo in the app. To retain historical work, keep the project and manage status and access. Recovery after deletion requires assessing a backup with the technical contact.

## Manage personal hours

The manager can log time against projects without an individual assignment. Their personal **Time log** also provides **Edit** and deletion with **Delete entry** confirmation. These commands affect personal rows.

Access to overall analyses does not permit editing others' hours. An operator correction-request and approval workflow is planned. Do not delete a project to correct one error: doing so deletes every linked entry, task and blocker.

## View the five Analysis views

**From**, **To**, **Project** and **User** filters combine. Dates are inclusive; the maximum interval is 366 days. Use **Apply**, **Reset** and quick periods. The selection appears in the URL, but the link does not extend the opener's permissions.

| View | Question answered | Correct interpretation |
| --- | --- | --- |
| **By period** | How much work was logged each day? | Sums filtered entries; allows navigating to a day's projects |
| **By project** | Which projects take the time? | Hours, entries and people involved, with current project status |
| **By user** | How is work distributed among people? | Also includes deactivated accounts with hours in the period |
| **Contract value** | Which agreements are linked to logged work? | Counts each distinct contract's full value once; does not allocate it by hours |
| **Cost of hours** | What is the work worth at entered rates? | Estimate by person and project, with coverage and hours without a rate |

The first three views show all users' aggregates to the manager. They are not lists of individual notes. The last two are restricted to managers.

In Contract value, a 10,000 EUR contract linked to two projects is counted as 10,000 EUR overall, not 20,000. Even if the period contains only one hour, the displayed value is the whole agreement value. Changing the project's link also changes analysis results for historical hours.

## Enter hourly rates

1. Open **Analysis → Labor cost**.
2. In **Hourly rates**, press **New rate**.
3. Choose **User** and enter **Rate in EUR per hour**.
4. Set **Valid from** and, if needed, **Valid until**.
5. Press **Create rate**.

Amounts from `0` to `999999,99` EUR are allowed, with at most two decimals. Zero is a valid rate; missing rates are instead shown as missing data. Dates are inclusive and one user's intervals cannot overlap. To change rates on 1 November, end the previous rate on 31 October.

A rate can be edited, but cannot be deleted through the current interface/API or moved to another user. Rates can also be defined for deactivated accounts to price their history. **Editing a rate recalculates estimates for past hours** in its interval; frozen financial actuals do not yet exist.

Each entry is priced separately and rounded to the cent before summing. Check coverage: if rates are missing, displayed cost is partial, not the work's overall cost. The section does not yet include expenses, purchases, invoices, revenue or margins.

## Configure Alerts and triggers

Rules belong to the manager creating them. Another manager does not automatically see or edit those rules, even if they are an alert recipient.

1. Open **Alerts and triggers**.
2. Press **New rule**, or **Create this check** under the desired type.
3. Enter **Rule name**.
4. Choose **Check type**: Nonexistent project or Missing daily hours.
5. Set **Run every day at** in hours/minutes format. The time zone is **Europe/Rome**.
6. Keep **Active rule** selected for automatic runs.
7. Choose **In-app recipient users** and optional **Email recipients**.
8. Press **Create rule**.

At least one recipient across the two channels is required. Each rule allows up to 50 active users and 50 email addresses. Separate emails with commas or lines; the UI removes duplicates. Up to 100 personal rules are allowed.

**Project does not exist** reports rejected attempts on deleted or nonexistent projects. It does not report an already-accepted invalid entry: saving is blocked. A normal authorization error on an existing project is not equivalent to this check.

**Missing daily hours** checks fewer than 480 minutes per person, Monday–Friday, after 48 actual hours from the day's end. Monitoring begins with the rule; later users are monitored from the first check including them. Active users are considered, not just selected recipients. A manager recipient can therefore receive everyone's alerts; an operator recipient receives only personal ones. Email addresses are explicit channel recipients, not accounts subject to the personal filter applied to in-app notifications.

An automatic rule runs once per local day; last and next run appear on its card. If created after today's time has passed, its first scheduled run is the next day. The service must be active: the time is a processing deadline, not a guarantee of delivery to the second.

Press **Run now** for a manual check, including on a suspended rule. Results report new notifications and any queued emails. Previously processed anomalies do not continuously generate duplicates. Disabling **Active rule** suspends automatic runs and preserves configuration; deleting rules is not provided in the current interface.

A manual check also consumes the same day's automatic run. Disabling the rule does not delete queued emails: with SMTP active, these can still be sent to recipients remaining configured. The UI provides no command to cancel the whole queue.

The 8 hours are not a mandatory entry limit: they are an alert threshold. Public holidays, leave and part-time work cannot yet be configured. Changing check type restarts its monitoring; changing name, time or recipients must not be interpreted as a complete historical rerun.

The email channel is implemented, but **SMTP still needs configuration**. Alerts intended for email remain pending and are not sent. In-app notifications are available without SMTP. Any future SMTP acceptance will attest that the server accepted the message, not that it was read or finally delivered.

If the owner is deactivated or loses Manager profile, their rules no longer run automatically. Before changing responsibilities, check which checks must be recreated by the new manager.

## View History

Open **History** to see changes from newest to oldest. Use **Show**, expand before/after details and press **Load earlier** to continue. **Refresh** reloads events.

History includes projects, tasks, blockers, hours, grants, accounts, groups, roles, suppliers, contracts, document uploads, rates and rules. Passwords are not included. A change and its event are saved together: if the app cannot record history, it rolls back the change too.

History explains what changed but is not a restore command. After deletion, historical information does not automatically recreate removed records.

## Calendars and upcoming modules

The Google Calendar synchronization service and APIs for each user's calendar exist. OAuth configuration will be completed later. The current interface has no Calendar section, account connection button or conversion of events into project hours yet.

Invoices, receipts, payments, multiple companies and multiple currencies are in the [financial plan](piano-modulo-finanziario.md). Daily-work improvements are in the [roadmap](roadmap.md). For limits and edge cases, see [Processes and rules](processi-e-regole.md).
