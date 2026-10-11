# Product documentation

[Improved operational workflows (schema 13)](flussi-operativi.md)

[Italiano](../../prod/README.md) | **English**

[General index](../README.md) · [Technical documentation](../tehnical/README.md) · [Project README](../../../README.md)

These guides explain how to use punto, organize access to projects, tasks and blockers, and interpret hours, analyses and notifications. They refer to the product implemented as of **11 October 2026**, schema **13**. Development plans are clearly identified as such.

## Operational guides

| Document | Audience | Contents |
| --- | --- | --- |
| [Overview](panoramica.md) | Everyone | Purpose, people involved, available modules and integration status |
| [User guide](guida-utente.md) | Operators | Sign-in, visible projects, daily entries, personal history, analyses and notifications |
| [Asset inventory](asset-inventory.md) | Everyone and Managers | Asset catalog, statuses, assignments, warranties and history; writing restricted to Managers |
| [Tasks, Kanban and blockers](attivita-kanban-e-blocchi.md) | Managers and collaborators | Board, subtasks, owners, obstacles, revisions and permissions |
| [Manager guide](guida-gestore.md) | Managers and project managers | Users, groups, roles, contracts, PDFs, projects, rates, checks and audit |
| [Processes and rules](processi-e-regole.md) | Managers, users and support | Complete workflows, access criteria, dates, statuses, durations, amounts and alert behavior |
| [Frequently asked questions](faq.md) | Everyone | Common problems and explanations of less obvious behavior |
| [Glossary](glossario.md) | Everyone | Terms used in the app and documentation |

## Scenarios and examples

| Document | Audience | Contents |
| --- | --- | --- |
| [Getting started operationally](avvio-operativo.md) | Managers and people responsible for configuration | First-use path, from organization to initial hours and checks |
| [Access scenarios](scenari-accessi.md) | Managers and support | Examples of groups, roles, dates, revocations, shared projects and visibility |
| [Analysis and cost examples](esempi-analisi-e-costi.md) | Project managers | Fictional dataset, five views, verifiable calculations and interpretation of results |
| [Acceptance testing](collaudo-accettazione.md) | People preparing and verifying a release | Manual cases, preconditions, actions and expected results; not a record of tests already performed |

## Planning

| Document | Status and use |
| --- | --- |
| [Status and roadmap](roadmap.md) | Updated comparison of current capabilities, completed parts and upcoming releases |
| [20-feature plan](roadmap-20-funzionalita.md) | Detailed project originally based on schema 3, retained with dependencies and acceptance criteria |
| [Financial module plan](piano-modulo-finanziario.md) | Plan for multiple companies/countries/currencies, invoicing and receipts; the full module remains to be implemented |

Use the operational guides for daily work. Use the updated roadmap to decide priorities; the two detailed plans also contain proposed screen, entity and API names.

## Boundaries of the current product

The **Manager** profile administers the workspace; the **Operator** profile uses projects for which access has been granted. Roles on individual projects govern viewing and logging hours, updating assigned tasks and managing personal blockers. The Manager organizes tasks; the Viewer can view the board. The detailed log remains personal even for managers; analyses allow managers to view aggregates for multiple users.

The available financial pages cover contract values and estimated labor costs in **EUR**. Internal notifications are operational. Email delivery requires SMTP configuration; Google synchronization is exposed through APIs and requires OAuth. The Calendar screen and linking events to projects and hours are later developments.
