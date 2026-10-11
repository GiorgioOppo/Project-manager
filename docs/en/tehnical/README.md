# Technical documentation

[Italiano](../../tehnical/README.md) | **English**

[General index](../README.md) · [Product documentation](../prod/README.md) · [Project README](../../../README.md)

These guides describe the system implemented as of **11 October 2026**: C server, HTML/CSS/JavaScript interface, application schema 10, and Docker deployment with three MySQL members and three C addons. Native and Docker configurations are separate environments.

## Guides

| Document | Contents |
| --- | --- |
| [Architecture](architettura.md) | Components, responsibilities, flows, persistence, transactions, availability, and limitations |
| [Docker naming](nomi-servizi-e-immagini.md) | Service and image names, TLS aliases, local tags, and migration from previous containers |
| [Container migration of 11 October 2026](migrazione-container-2026-10-11.md) | Operational report: backup, data preservation, cluster recovery, and deployment checks |
| [Installation and configuration](installazione-configurazione.md) | Requirements, first startup, native modes, variables, certificates, and provider activation |
| [Keycloak and multi-app authentication](keycloak.md) | Shared realm, Punto and Ticket clients, login modes, user linking, secrets, and deployment |
| [Ticket reverse proxy](ticket-reverse-proxy.md) | Publishing Ticket through Nginx Proxy Manager, hostname, forwarding, and checks |
| [Networking and security](rete-sicurezza.md) | Eight networks, addresses and ports, Squid, TLS, authentication, authorization, secrets, and actual encryption |
| [Data model](modello-dati.md) | Databases, table dictionary, types, relationships, constraints, indexes, migrations, and addon stores |
| [App API](api-app.md) | Public endpoints, sessions, origin checks, permissions, parameters, responses, errors, and limits |
| [Asset inventory](asset-inventory.md) | Separate C service, API, permissions, inventory network, persistence, startup, and backup |
| [Addon APIs](api-addon.md) | Private HTTPS contracts for email and Calendar, authentication, queues, idempotency, OAuth, and synchronization |
| [Operations, backup, and recovery](operazioni-backup-ripristino.md) | Monitoring, updates, cluster recovery, consistent backups, data recovery, and diagnosis |
| [Development and testing](sviluppo-test.md) | Code structure, build, native checks, optional suites, change criteria, and recorded results |
| [Task verification of 11 October 2026](verifica-attivita-2026-10-11.md) | Suites, MySQL integrity, desktop/mobile browser, screenshots, and schema 8 module deployment status |
| [UI verification of 10 October 2026](verifica-ui-2026-10-10.md) | Historical schema 3 report, synthetic data, screenshots, and test limitations |

## Implementation details

| Document | Contents |
| --- | --- |
| [C server internals](interni-server-c.md) | Startup, modules, HTTP, resource management, errors, and concurrency boundaries |
| [Frontend and interface](frontend-e-interfaccia.md) | Page state, navigation, concurrent requests, forms, rendering, and accessibility |
| [Migrations and consistency](migrazioni-e-consistenza.md) | Schema evolution, transactions, backend differences, and uncertain outcomes |
| [Tasks, Kanban, and blockers](attivita-kanban-e-blocchi.md) | Schema 8, API, hierarchies, revisions, permissions, and deletion |
| [Alert engine](motore-alert.md) | Schedules, cursors, deduplication, subjects, recipients, restart, and outbox semantics |
| [Integration flows](flussi-integrazioni.md) | Email and OAuth sequences, synchronization, conflicts, and provider consistency |

## Administration and maintenance

| Document | Contents |
| --- | --- |
| [Diagnostics and observability](diagnostica-e-osservabilita.md) | Interpreting available signals and diagnosis paths by symptom |
| [Releases and compatibility](rilascio-e-compatibilita.md) | Release preparation, components, schema, checks, and rollback limitations |
| [Certificates and secrets](gestione-certificati-e-segreti.md) | Material inventory, consumers, checks, expiration, and rotation constraints |

## How to use these guides

For a new installation, start with architecture and configuration. Before changing the software, read the data model and APIs, then the development instructions. When working on an existing installation, use the operational procedures and verify the action's scope: **initial seeding, updating, quorum recovery, and restoring from backup are different operations**.

Pages identify the source files to consult to confirm details. The specific [email](../../../addon/README.md), [Calendar](../../../calendar-addon/README.md), and [Asset Inventory](../../../inventory-addon/README.md) manuals remain alongside their services.

The [product roadmap](../prod/roadmap.md) collects future requirements. In particular, the plans for full finance, company isolation, and business-field encryption do not describe components already implemented.
