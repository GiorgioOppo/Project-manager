# Tasks, Kanban and the blocker register

[Italiano](../../prod/attivita-kanban-e-blocchi.md) | **English**

Documented status: **11 October 2026**, data schema **8**.

The **Tasks and blockers** module organizes work within projects. A task describes a result to achieve; a blocker describes an obstacle to address. This first delivery includes tasks, subtasks, a board and a blocker register. Dependencies, workload planning, probabilistic risks and linking hours to individual tasks remain later developments.

## Prepare the project

Open **Tasks and blockers** and select a project, or use **Tasks** in the project row. The selector contains projects visible to the user, including shared projects. Tasks and blockers are shared with people authorized to view that project; do not put information intended for a different audience in descriptions.

The manager creates tasks with a title, description, optional owner, priority and due date. They can choose a parent task in the same project to create a subtask. At most eight levels are allowed, counting the main task as level one. The system rejects circular references and links between different projects.

The owner must be an active user with a Manager role or authorization to collaborate on the project. Assigning a task does not grant new permissions: access is always managed through projects, users and groups. Revocation prevents updates even if the name remains associated to preserve context; the manager can reassign the work.

## Use the board

Tasks move through five statuses:

| Status | Operational meaning |
| --- | --- |
| To do | Defined task not yet started |
| In progress | Work has started |
| Blocked | The task cannot proceed |
| To review | Work prepared, awaiting a review |
| Completed | Finished task |

Status changes use controls that are also keyboard-accessible. Following every column in sequence is not mandatory, and formal approval of the move to Completed does not yet exist. The manager can update any task; the operator can update the status of tasks assigned to them, provided they retain a role allowing collaboration.

Use **Search tasks**, **Status** and **Priority** to narrow results. Search includes title, description and owner name. The **View** selector switches between **Kanban board** and **List and subtasks**. On the board, choose the status in a card and press **Apply**; on small screens the columns scroll within their container. **Refresh** reloads the project's data.

The hierarchical view helps with reading subtasks. If the filter hides a parent, its matching subtask remains visible with a reference to the main task. Completing a parent task does not automatically complete its children. Due dates and priorities are operational information; they do not automatically change the project status or generate new email rules.

## Report a blocker

Press **Report blocker** on the page or a task card. A collaborator can report a blocker by choosing a title, description, impact, next action and optional due date. Linking to a specific task is optional: the blocker can affect the entire project.

**Manage** opens the form for an editable blocker, where description, status and next action are updated. The **Show blockers** filter initially offers **Open and being handled**, and also allows **All** or **Resolved**.

Available impacts are Low, Medium, High and Critical. Statuses are Open, Being handled and Resolved. Impact indicates operational severity and is not an automatic financial calculation.

A blocker created by an operator is initially assigned to them. The manager can assign it to another authorized person. The manager and, while authorized to collaborate on the project, the blocker's creator or owner can update it. Only the manager can change its owner or delete it.

A task's Blocked status and the blocker register are separate: reporting or resolving an obstacle does not automatically move the card. This allows multiple obstacles for the same task to be managed without accidentally completing it.

## Permissions

| Operation | Manager | Project Contributor | Viewer |
| --- | --- | --- | --- |
| Read board and blockers | Yes | Yes | Yes |
| Create and organize tasks | Yes | No | No |
| Change task status | All | Only tasks assigned to them | No |
| Report a blocker | Yes | Yes | No |
| Edit a blocker | All | Only if creator or owner | No |
| Change owner or delete a task or blocker | Yes | No | No |

Below, Contributor also includes a custom role allowing logging hours; a shared project grants this capability to active users. Board permission must be valid today, regardless of the task's due date.

Checks also apply to direct API requests. Keeping an old link does not bypass a revocation. If the server reports denied access or an unavailable item, the interface clears the board and closes open forms; an unsaved draft is not retained. Choose a currently authorized project again.

## Concurrent changes and deletion

Every save of an existing item checks its revision. If another person changed it meanwhile, the server rejects overwriting: reload the item and compare changes before trying again. No automatic merge is applied. In the form, **Close and reload the updated version** closes the draft and reloads data: first note your edits if they must be reapplied. For a quick status change, the page reloads the list and displays the conflict.

To delete a task, first move or delete its subtasks. Associated blockers remain in the project and are detached from the deleted task. Permanent project deletion instead also removes every task and blocker, as well as hours and grants already covered by deletion. History preserves events and counts; it does not replace a backup.

Title: 1 to 160 characters; description: up to 2000; a blocker's next action: up to 1000. The due date is optional. Task priorities are Low, Normal, High and Urgent.

The first delivery's limit is **500 tasks and 500 blockers per project**, including completed or resolved items. At the limit, the system rejects new creations; lists are not silently truncated.

References: [manager guide](guida-gestore.md), [access scenarios](scenari-accessi.md), [APIs and technical model](../tehnical/attivita-kanban-e-blocchi.md).
