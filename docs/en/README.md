# Punto documentation

[Italiano](../README.md) | **English**

This documentation describes the web app for projects and time tracking, the services that run it, and its workflows. It is organized into **`tehnical/`** for developers and system administrators and **`prod/`** for people configuring and using the product. The `tehnical` directory name is retained as required by the project.

**Reference:** workspace code as of 11 October 2026, application schema **14**. Native mode uses SQLite and local documents; Docker mode uses MySQL and MongoDB GridFS. Current schema and policy are described in the [API catalog](tehnical/api-app.md) and [operational workflows](prod/flussi-operativi.md); previous test counts remain historical evidence. The [main README](../../README.md) contains the general startup instructions.

## Reading paths

| Audience | Recommended path |
| --- | --- |
| Operator | [Overview](prod/panoramica.md) → [User guide](prod/guida-utente.md) → [Frequently asked questions](prod/faq.md) |
| Manager / project manager | [Manager guide](prod/guida-gestore.md) → [Processes and rules](prod/processi-e-regole.md) → [Status and roadmap](prod/roadmap.md) |
| System administrator | [Architecture](tehnical/architettura.md) → [Installation and configuration](tehnical/installazione-configurazione.md) → [Networking and security](tehnical/rete-sicurezza.md) → [Operations and recovery](tehnical/operazioni-backup-ripristino.md) |
| Developer / integrator | [Data model](tehnical/modello-dati.md) → [App API](tehnical/api-app.md) → [Addon APIs](tehnical/api-addon.md) → [Development and testing](tehnical/sviluppo-test.md) |
| Product owner | [Overview](prod/panoramica.md) → [Updated roadmap](prod/roadmap.md) → [20-feature plan](prod/roadmap-20-funzionalita.md) → [Financial module plan](prod/piano-modulo-finanziario.md) |

The [technical index](tehnical/README.md) and [product index](prod/README.md) list every document and its purpose.

## Guides by task

| Task | Dedicated documents |
| --- | --- |
| Manage inventory and asset assignments | [Inventory guide](prod/asset-inventory.md), [service and API](tehnical/asset-inventory.md) |
| Organize tasks and blockers | [Operational guide](prod/attivita-kanban-e-blocchi.md), [implementation and API](tehnical/attivita-kanban-e-blocchi.md), [11 October verification](tehnical/verifica-attivita-2026-10-11.md) |
| Prepare for first use | [Operational startup](prod/avvio-operativo.md), [access scenarios](prod/scenari-accessi.md) |
| Interpret hours and amounts | [Analysis and cost examples](prod/esempi-analisi-e-costi.md), [product rules](prod/processi-e-regole.md) |
| Understand part of the code | [C server](tehnical/interni-server-c.md), [frontend](tehnical/frontend-e-interfaccia.md), [alert engine](tehnical/motore-alert.md) |
| Change data or integrations | [Migrations and consistency](tehnical/migrazioni-e-consistenza.md), [email and Calendar flows](tehnical/flussi-integrazioni.md) |
| Standardize Docker services and images | [Naming and migration](tehnical/nomi-servizi-e-immagini.md) |
| Prepare a release | [Releases and compatibility](tehnical/rilascio-e-compatibilita.md), [acceptance testing](prod/collaudo-accettazione.md) |
| Diagnose a problem | [Diagnostics and observability](tehnical/diagnostica-e-osservabilita.md), [certificates and secrets](tehnical/gestione-certificati-e-segreti.md) |

Examples use fictional data. Test procedures describe what to check and the expected result; they do not attest to a new test run. Technical guides identify their repository sources and distinguish current behavior from guidance for future development.

## Feature availability

| Status | Meaning | Examples |
| --- | --- | --- |
| Implemented and usable | Code, APIs, and interface are available, subject to normal installation configuration | Projects, tasks/subtasks, Kanban, blockers, hours, users, groups, contracts/PDFs, analysis, rates, internal notifications, asset inventory |
| Implemented; provider configuration required | Service and APIs exist; real integration requires credentials and external configuration | SMTP email delivery and Google OAuth connection |
| Available through the API | Functionality is accessible through the application bridge; no dedicated screen exists yet | Google Calendar synchronization and event management |
| Planned / partial | Requirement or release plan; any existing parts are identified in the roadmap | General timesheet approval, full finance, multiple companies/currencies, event–project–hours links |
| Historical evidence | Results of checks performed at an earlier stage, with their scope | Schema 3 UI testing |

Plans also describe future tables and APIs: their presence in a planning document does not mean they exist in the software. The [implemented schema](tehnical/modello-dati.md) and [current API catalog](tehnical/api-app.md) identify the actual resources.

## File organization

English pages mirror the Italian documentation under `docs/en/`, retaining the same filenames. The Italian documentation and shared images remain in their original locations:

```text
docs/
├── README.md
├── en/               # English documentation with the same structure
│   ├── README.md
│   ├── tehnical/
│   └── prod/
├── tehnical/
│   ├── README.md
│   ├── architettura.md
│   ├── nomi-servizi-e-immagini.md
│   ├── interni-server-c.md
│   ├── frontend-e-interfaccia.md
│   ├── installazione-configurazione.md
│   ├── keycloak.md
│   ├── ticket-reverse-proxy.md
│   ├── rete-sicurezza.md
│   ├── gestione-certificati-e-segreti.md
│   ├── modello-dati.md
│   ├── migrazioni-e-consistenza.md
│   ├── api-app.md
│   ├── api-addon.md
│   ├── asset-inventory.md
│   ├── flussi-integrazioni.md
│   ├── attivita-kanban-e-blocchi.md
│   ├── motore-alert.md
│   ├── operazioni-backup-ripristino.md
│   ├── diagnostica-e-osservabilita.md
│   ├── rilascio-e-compatibilita.md
│   ├── sviluppo-test.md
│   ├── verifica-attivita-2026-10-11.md
│   └── verifica-ui-2026-10-10.md
├── prod/
│   ├── README.md
│   ├── panoramica.md
│   ├── avvio-operativo.md
│   ├── guida-utente.md
│   ├── guida-gestore.md
│   ├── asset-inventory.md
│   ├── attivita-kanban-e-blocchi.md
│   ├── processi-e-regole.md
│   ├── scenari-accessi.md
│   ├── esempi-analisi-e-costi.md
│   ├── collaudo-accettazione.md
│   ├── faq.md
│   ├── glossario.md
│   ├── roadmap.md
│   ├── roadmap-20-funzionalita.md
│   └── piano-modulo-finanziario.md
├── verifica-ui/       # Historical test screenshots
├── analisi/           # Analysis view evidence
├── alert/             # Notification evidence
├── attivita/          # Task and blocker module evidence
└── commesse/          # Shared catalog evidence
```

Existing images retain their paths. Local service manuals remain available in [addon/README.md](../../addon/README.md), [calendar-addon/README.md](../../calendar-addon/README.md), and [inventory-addon/README.md](../../inventory-addon/README.md).

## Maintenance criteria

1. Update product guides when a workflow, permission, label, or validation rule changes.
2. Update the data model and API catalog alongside migrations, endpoints, and formats. Schema changes also require an updated version reference.
3. Update architecture, configuration, and operational procedures alongside services, networks, volumes, certificates, or dependencies.
4. Report tests as dated results, stating what ran, what is optional, and what uses simulated providers. A historical result does not certify a new environment.
5. Use synthetic examples and placeholder names. Credentials, real cookies, `.env.docker` files, private keys, and user data do not belong in documentation.
6. Check relative links after moving files. Source references are relative to the repository root, without personal computer paths.
7. Keep the Italian and English versions aligned when changing documented behavior.

Current guides are based on versionable code and configuration. Decisions still pending remain in the roadmap; they are not presented as features or guarantees already available.
