# User guide

[Italiano](../../prod/guida-utente.md) | **English**

This guide describes the **Operator** journey in the current interface. Administrative actions are in the [manager guide](guida-gestore.md). Bold names correspond to command labels.

## Sign in and out

1. Open the app address provided by your organization's contact.
2. Enter the assigned account's **Email** and **Password**.
3. If needed, use **Show password** to check what you typed and **Hide password** to hide it again.
4. Select **Remember me for 30 days** only if you want a longer session on that device.
5. Press **Sign in to your workspace**.

The page reports missing fields, invalid email addresses and incorrect credentials. Remember me also stores the email in the browser; it does not store the password there. Normal session duration is 8 hours. A service restart or account deactivation can require signing in again before expiration.

Your name, email and profile appear at the top. Use **Sign out** when finished. The interface does not include self-registration, password recovery or password changes: contact the installation's administrator for access problems. The login demonstration section helps in the demo environment and does not replace credentials assigned by your organization.

## Find your way around the workspace

| Section | Purpose |
| --- | --- |
| **Projects** | View projects you can access and their status |
| **Tasks and blockers** | View the project board, update assigned tasks and report obstacles when authorized |
| **Time log** | Enter and view your time by day |
| **Analysis** | Summarize personal hours over an interval, by project or by user |
| **Notifications** | Read alerts addressed to your account |

Available actions depend on your profile and current grants. Historical hours on a project do not automatically grant the right to log more.

On small screens, tables may scroll horizontally within their containers. Windows can be closed with **Cancel**, the cross or Escape when no save is in progress. Closing a form does not save work: always check the confirmation message after pressing the save button.

## Find a project

1. Open **Projects**.
2. Use **Search by name, code, or client…** to narrow the list.
3. If useful, choose a **Status**: Planned, In progress, Paused, Completed or Cancelled.
4. Check code and name before logging work, especially when projects have similar names.
5. Use **Reset filters** when search does not return what you expect.

The list shows only currently authorized projects and those shared by everyone. A project assigned with **Viewer** role can be viewed but not used for new entries. **Contributor**, or an equivalent manager-configured role, also allows logging hours.

Hours shown in the project list are **your total hours** across all dates. For a period total, use Analysis. Project status alone does not determine whether hours can be logged: if company procedure requires a block, the manager must change grants.

## Follow tasks and obstacles

1. Open **Tasks and blockers** and select the **Project**, or press **Tasks** in its Projects row.
2. View the **To do**, **In progress**, **Blocked**, **To review** and **Completed** columns. Search, status and priority narrow the tasks shown.
3. Choose **List and subtasks** in **View** to read the hierarchy. Board columns can scroll horizontally.
4. For a task assigned to you, if your role permits collaboration, choose the new **Status** and press **Apply**. Title, owner, due date and structure are defined by the manager.
5. Use **Report blocker** to describe an obstacle affecting the project or a specific task. Enter title, description, impact, next action and, if needed, due date. The new blocker is assigned to you.
6. In the **Blocker log**, press **Manage** to update blockers you created or own. Set **Resolved** when the obstacle is overcome. The initial filter hides resolved blockers; choose **All** to find them.

Viewer allows viewing only. Losing authorization prevents updating a task even if you are still named as owner. The manager can reassign it. Task and blocker descriptions are shared with anyone who can read that project.

Task, child-task and blocker statuses do not update one another: resolving a blocker does not automatically move the task. Hours are still logged against the project; **Work done / note** is free text and does not link the entry to a board card.

If someone else has already saved a change, the app reports a conflict. Do not repeat saving the old version: use **Close and reload the updated version**, check current content and reapply still-needed edits. The closed draft is not retained. **Refresh** reloads the project's tasks and blockers.

The [module guide](attivita-kanban-e-blocchi.md) describes statuses, limits and deletions.

## Log the day's work

1. Press **Log hours** at the top of the page, or open **Time log** and press **Add hours**.
2. In **Project**, choose a list entry. Manually entering a new code is not supported.
3. Set **Workday**. Check the date even when catching up on past entries.
4. Enter **Hours worked** in your preferred format.
5. If useful, fill in **Work done / note** to describe the work.
6. Press **Save hours** and wait for confirmation.
7. In that day's Time log, verify the new row and **Daily total**.

Duration examples:

| Entered text | Logged time |
| --- | --- |
| `1,5` or `1.5` | 1 hour and 30 minutes |
| `1:30` | 1 hour and 30 minutes |
| `0:15` or `0,25` | 15 minutes |
| `8` | 8 hours |

Colon format means hours and minutes: `1:30` is not equivalent to `1,30`, which means 1.3 hours, or 1 hour and 18 minutes. After the colon, minutes must be between 00 and 59. Decimal durations must correspond to whole minutes; for short durations, `0:MM` avoids ambiguity.

Each row must last at least one minute. The total across all projects on the same day cannot exceed 24 hours. Multiple entries on the same project with different notes are allowed, with at most 1000 rows per user per day. The note can contain up to 1000 characters.

Hours are always attributed to your account. You can enter past or future days when your grant permits it. A single grant must authorize both today and the day being logged: a project visible today may not be usable for a date before you joined it.

## Use shared projects

If the manager configured them, **Generale**, **Formazione**, **Riunioni** and **Attività interne** appear among available projects. Use the entry consistent with organizational rules. For example, a meeting about a specific project can be logged directly against that project if that is the adopted convention.

Shared projects do not require a personal grant and remain available to users added later. The manager can change their names, status and visibility as with other projects.

## View saved hours

1. Open **Time log**.
2. Set **Workday**, or use previous/next-day commands.
3. Press **Today** to return to the browser's current day.
4. Read entries and the daily total.

The log remains personal. If you lose authorization for a project, your saved entries remain viewable while the project exists. Permanent project deletion by the manager instead also removes all linked hours, tasks and blockers.

As Operator, you cannot edit or delete a saved entry. If there is an error, contact the manager with date, project, duration and requested correction. Do not add a second row to simulate a correction: it would increase the total. Correction requests and approval are still planned; the manager currently has no generic editor for other people's hours.

## Analyze your work

1. Open **Analysis**.
2. Choose **By period**, **By project** or **By user**.
3. Set **From** and **To**. For a single day, use the same date in both fields.
4. Optionally select a **Project**.
5. Press **Apply**. **Today**, **This week** and **This month** prepare quick intervals using the date returned by the server.
6. Read the summary and table rows.

The interval includes both endpoints and cannot exceed 366 days. **By period** groups by day; **By project** groups work by project. **By user** remains limited to your account when you are an Operator. Neither the filter for other users nor the two financial views reserved for managers is available.

You can move from a day row to its projects and from a project row to its trend by period. View and filters appear in the page address: save the link to reopen the same selection. Whoever opens it sees only data permitted to their account.

Analyses include your history on subsequently revoked projects. They contain neither individual entry notes nor Kanban card aggregates. An empty list means no entries match the filters, not that a project or account was deleted.

## Read notifications

1. Open **Notifications** when a counter appears in navigation or when you want to check alerts.
2. Read title, message, alert date and any affected **Day**.
3. Press **Mark as read** after reviewing it.
4. Use **Refresh** to reload immediately. The interface also refreshes automatically every minute.

The latest 100 accessible notifications are shown. The counter indicates unread notifications and can include alerts older than the displayed list. Notifications are personal: the operator receives only anomalies concerning their own work.

Available checks are:

- **Project does not exist**: an attempt to save hours against a nonexistent or deleted project was rejected; those hours were not saved.
- **Missing daily hours**: a Monday–Friday day has fewer than 8 hours after the specified grace period. The rule does not yet consider public holidays, leave or part-time work.

Reading does not automatically correct an entry or delete a notification. The message preserves the context detected when the check ran. If you later added hours, compare the notification with the updated log.

## Handle a problem

| Situation | What to do |
| --- | --- |
| Project absent from list | Reset filters; if still missing, ask the manager to check its grant and dates |
| Project visible but not selectable for hours | Check the operational role with the manager: it may be read-only |
| Grant revoked while logging hours | The request is rejected; choose a still-authorized project after checking the correct destination. The UI retains date, duration and note when updating choices |
| Access denied or item unavailable in Tasks and blockers | The app clears the board and closes drafts; select a still-authorized project. Unsaved changes are not retained |
| Cannot change a task's status | Check that you own it and still have Contributor project access; a Viewer can only view |
| Resolved blocker does not appear | In the blocker register, choose **All** or **Resolved** |
| Conflict editing tasks or blockers | Reload the updated version and compare it with your edits; the previous version is not overwritten |
| Duration rejected | Use whole minutes, check format and the total already present that day |
| Session ended | Sign in again; an unconfirmed draft is not a saved entry |
| Error or delay while saving | Before repeating submission, check whether the row already appeared in the log; time entry does not yet have the general deduplication planned on the roadmap |
| 8-hour alert during leave or public holidays | The current check uses a fixed threshold: report the context to the manager without entering work not performed |
| Missing Calendar screen | Synchronization is provided through APIs, but the screen and event–project link remain to be implemented |

Complete rules are in [Processes and rules](processi-e-regole.md). The [roadmap](roadmap.md) lists planned developments for a weekly grid, copying, corrections and calendar.
