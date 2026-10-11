# Punto UI and feature verification — 10 October 2026

[Italiano](../../tehnical/verifica-ui-2026-10-10.md) | **English**

> **Historical evidence: schema 3 testing.** This report preserves the results and limitations found during that stage on 10 October 2026; service, test, section, and feature counts do not describe the current release. For the current architecture and later testing, see [architecture](architettura.md) and [development and testing](sviluppo-test.md).

The check covered features implemented in schema 3, with browser tests as a manager, operator, and viewer. The native suite includes 128 tests: 114 passed and 14 optional tests skipped. The Docker stack is available, with seven healthy services and three MySQL members ONLINE and PRIMARY.

Browser creation and editing tests use a separate C server, temporary SQLite and documents, and synthetic people and projects only. Navigation and read-only checks ran on the existing Docker stack. Native test results do not replace a new complete integration test with MySQL/GridFS.

## Functional coverage

| Area | Browser verification | Additional automated coverage |
| --- | --- | --- |
| Login and session | Required fields, incorrect credentials, show/hide password, login, logout, and disabled user | Cookies, session duration, revocation, HTTP protections, and origin |
| Projects | Creation, linked contract, editing through all five states, search, filter, and no results | Validation, uniqueness, persistence, and authorization |
| Time log | Manager and operator entry, editing own hours, decimal and HH:MM formats, rejection of fractions of a minute, daily limit, total, and date change | Limits, dates, ownership, updates, and deletion; the deletion dialog was opened and canceled in the browser |
| Suppliers | Creation, contact editing, and search | Validation, deactivation, and references |
| Contracts | Creation, supplier, dates, EUR amount, rejection of a third decimal place, status editing, and search | Relationships, validation, and data preservation |
| PDFs | Synthetic PDF upload, updated list, and download; downloaded bytes identical to the original | Signature, limits, hash, metadata, and permissions; live GridFS integration not rerun |
| Users | Name editing, group updates, demoting one's own manager account with a second active manager, and blocked disabled-account login | Creation, roles, deactivation, last-manager protection, and permissions |
| Groups and roles | Subgroup creation, member addition/removal, custom role creation/editing | Hierarchies, cycles, inheritance, predefined roles, and isolation |
| Access grants | Direct and group grants, validity, editing, revocation, inheritance, and selector restricted to usable projects | Intervals, alternative paths, and checking the same grant on today's date and the worked date |
| Revocation during form entry | Save denied; revoked choice removed; date, hours, and note preserved; personal history still available | Server checks after revocation, without implicit access through a historical entry |
| Read-only | Project viewable, no time-entry or administration buttons; other people's hours hidden | Rejection of unauthorized API calls |
| History | Loading, entity filter, before/after details, and pagination on the Docker stack | Events, immutability, and permissions |
| Responsive layout and keyboard | Desktop 1440 px, tablet 768 px, phones 390 and 320 px; dialog, Escape, and arrow-key tab navigation | Static control and label checks; no complete accessibility certification |

Managers can reach all seven sections. Operators see Projects and Time log. At 320 px, the document width equals the viewport in every section; wider tables scroll within their own container.

## Fixes applied and verified

1. **Data remaining visible after demotion.** Permission changes invalidate previous requests, clear data and metrics, close dialogs, and reload authorized resources. The test goes from eight projects to one without a manual reload and removes time-management actions.
2. **Revoked project remaining in the open selector.** Reloading also updates the time dialog's options. The revoked selection becomes empty, and a message asks the user to choose an authorized project while preserving the draft. The rejected attempt does not add hours.
3. **Group members not updated after a user change.** Saving invalidates organization and member data. Returning to the tab shows the new name without pressing Refresh.
4. **Contract states labeled as project states in history.** Formatting uses the entity type and recognizes nested snapshots. A contract's transition from Draft to Active displays correctly.
5. **Header off-screen at 320 px.** Name and email can shrink without pushing the Sign out button past the edge. The 13 px overflow found with a long name is eliminated in all seven sections.

Changes affect `public/app.js` and `public/workspace.css`. They were verified in the temporary environment. The existing application container was not rebuilt or restarted: these fixes require a later release to become available in the Docker stack.

## Live infrastructure checks

- Seven Docker services healthy.
- `/healthz` returns HTTP 200 through Nginx on loopback and on the locally resolved name `progetti.tuaazienda.it`; this does not check public DNS or Internet accessibility.
- All nodes see the same three ONLINE and PRIMARY members, with writes enabled and multi-primary mode.
- `BEFORE_AND_AFTER` consistency, update checks, and mandatory MySQL TLS confirmed.
- `no-store`, CSP, frame protection, and `nosniff` headers present.

The 14 skipped optional tests are: three HA, seven Docker networking, one TLS Connector/C, and three live MySQL/GridFS integrations. Failure tests, restarts, and application writes were not repeated on the existing stack.

## Areas still requiring improvement

- **Time zone:** the UI determines today and some validity badges using the browser's time zone, while the backend authorizes using the server's time zone. Near midnight, indications can diverge for users in other time zones. Roadmap R0 calls for a business date supplied by the server.
- **Contrast:** white text on the coral `#e96850` button has a 3.20:1 ratio; two examined secondary-text combinations are around 3.46–3.47:1. These elements require a contrast review. No complete WCAG, screen-reader, or all-browser verification was performed.
- **Future features:** finance, multiple companies/currencies, Google Calendar, offline drafts, and other roadmap additions are not implemented interfaces yet. They cannot be considered tested. Field/PDF encryption, key vaults, and encrypted calculations also remain unimplemented, as stated in the README.

## Visual evidence

Screenshots contain only synthetic test data.

![Groups and roles on desktop](../../verifica-ui/ui-desktop.jpg)

![Time log on a phone](../../verifica-ui/ui-mobile.jpg)

![Revocation during form entry with the draft preserved](../../verifica-ui/ui-revoca.jpg)
