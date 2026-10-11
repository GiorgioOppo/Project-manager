# Task, Kanban, and blocker verification — 11 October 2026

[Italiano](../../tehnical/verifica-attivita-2026-10-11.md) | **English**

This report covers the first **Tasks and blockers** module, with application schema **8**. Tests use synthetic users, projects, and content, temporary databases, and an isolated browser. They do not attest to an update of the operational database.

## Compilation and suites

| Check performed | Result |
| --- | --- |
| Native SQLite build, C11, and warnings treated as errors | Passed |
| `make test` with the fixture environment, without enabling deployment tests | 292 cases: **263 passed, 29 skipped, 0 failed**; duration 218.558 s |
| `test_work_board.py` suite, repeated after unifying 404 responses | **20/20 passed**; these 20 cases are already included in the total above |
| Docker app build with MySQL backend | Passed; local candidate image `punto:work-board-schema8` |
| Interface JavaScript syntax | Passed |

The dedicated suite checks direct access, groups, and shared projects; revocations and disabled accounts; assignees; hierarchies and depth; concurrent changes; validation and Unicode; HTTP limits; atomic audit; deletion; and migration 7→8. It includes identical responses for nonexistent and invisible items, preventing those cases from being distinguished by ID.

The 29 skipped cases require additional options, dependencies, or environments. They are not counted as passed. The suite result describes this module's code at the time of testing; subsequent changes or other work in the same workspace require their own checks.

## MySQL integrity

A temporary **MySQL 8.4.11** container, with no network, published ports, or persistent volumes, executed all **24 CREATE TABLE statements** extracted from `src/mysql_schema.c`. The container was removed afterward.

The following were checked:

- Rejection of task–parent and blocker–task links across projects, including updates.
- User constraints, allowed states, and positive revisions.
- Unicode fields at their expected limits: title 160, description 2000, next action 1000 characters.
- Rejection of deletions incompatible with relationships; successful explicit cleanup in the expected order.
- Rollback of changes and preservation of a project unrelated to the test.

This verification covers **DDL and relational integrity**. It is not an HTTP test of the app connected to the MySQL cluster and does not repeat HA failure simulations.

## Interface and authorization

Chrome/Playwright testing used the actual `public/` files and the C server with temporary SQLite. Further tests with simulated API responses checked error states and rendering hostile content as text.

During final verification, parallel changes to the inventory and ticket modules were in progress. To avoid including incomplete code, the last HTTP test and candidate image use assets from the module's previous build, updated with the final `work*` block, verified as identical to the workspace version. The UI suite with simulated APIs passed both on this stable copy and on the workspace after the inventory definitions arrived. This does not extend functional testing to the parallel modules.

| Scenario | Observed outcome |
| --- | --- |
| Manager | Creation of an assigned task and subtask; five Kanban columns and hierarchy view |
| Contributor | Status change for their own task; reporting a blocker with author and responsible person derived by the server |
| Viewer | Viewing without write or status-change buttons |
| Concurrency | API change while the form is open: 409, saving blocked, and explicit reload |
| Revocation | Revocation during editing: 404, form closed, and board data, draft, and contextual dialog text removed |
| Focus | Return to the card after saving; accessible destination after deletion or filtering; focus on the conflict error and after reload |
| Layout | Desktop 1440 px and mobile 390/320 px; no page-wide horizontal scrolling; columns within their own scrollable container |
| JavaScript | No unhandled errors during the workflow |

Screenshots show demonstration data only:

- [Desktop board](../../attivita/desktop-2026-10-11.png).
- [Board and blockers on a smartphone](../../attivita/mobile-2026-10-11.png).

Mobile layout verification included viewport screenshots and a new session opened directly at mobile width, as well as DOM dimension checks.

## Deployment status and limitations

A separate candidate image, `punto:work-board-schema8`, was built with local ID `sha256:d1c685c8f5eb9a6a63b5a48c93e468a7e91864c240ed59aa4991b8f0e84d51d1`. It includes the Tasks module with its final fixes, without parallel modules still in progress. Existing application containers remained stopped, and the operational database was not migrated to schema 8 during this work. Migration runs when the new version starts, following the [release procedure](rilascio-e-compatibilita.md), after the required backup.

Hours remain assigned to projects. This module does not introduce hour–task links, task dependencies, Gantt charts, capacity calculations, risk probabilities, or automatic assignment notifications. Real SMTP and Google integrations are outside this test scope.

References: [product guide](../prod/attivita-kanban-e-blocchi.md), [implementation and API](attivita-kanban-e-blocchi.md), [development and testing](sviluppo-test.md).
