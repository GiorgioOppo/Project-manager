# Acceptance test plan

[Italiano](../../prod/collaudo-accettazione.md) | **English**

This document defines reproducible manual tests and expected results for the current product. **It does not attest that these new tests have been performed.** Execution reports must be completed separately with version, environment, date, observations and evidence.

Criteria derive from the interface, code and existing tests. The [historical UI verification](../tehnical/verifica-ui-2026-10-10.md) documents only the scope examined in that session; it does not replace executing this plan on the version to deliver.

## 1. Environment and test data

Run this plan on an isolated lab installation, with synthetic data and a destination clearly distinguishable from the operational environment. Deletion, revocation, deactivation and role-change tests genuinely change data when performed. Do not use production projects, hours, documents or people.

Before starting, record:

| Information | What to note |
| --- | --- |
| Version | Commit or build identifier and application schema |
| Environment | Native SQLite or Docker MySQL/GridFS; lab URL |
| Client | Browser/version, window size and device time zone |
| Server date and time zone | Current day and Europe/Rome for alert checks |
| Integrations | SMTP and Google OAuth status, which can remain unconfigured |
| Responsible people | Person executing and person assessing outcomes |
| Recovery baseline | Snapshot/backup of the lab alone, if reusing the same environment |

Prepare at least four fictional accounts:

- **Marta**, Manager performing registration.
- **Bruno**, second active Manager to retain administrative access during Marta's profile tests.
- **Elena**, Operator logging hours.
- **Luca**, Operator with Viewer role on the test project.

Use separate browser profiles, not just tabs sharing the same cookie. Do not include passwords, cookies, tokens or secrets in evidence. Synthetic identities, commands, results and messages suffice for screenshots.

Prepare a valid synthetic PDF under 10 MiB, a text file renamed with `.pdf` extension and, for the size limit, a test document over 10 MiB. Do not use real contracts to test uploads.

## 2. Record each case's outcome

For each identifier, use **Passed**, **Failed**, **Blocked** or **Not run**. A case blocked by missing configuration or waiting for time to elapse is not passed.

Note preconditions, steps actually performed, observed result, expected result and synthetic identifiers useful for reconstructing the test. When a case changes the baseline, restore the lab or explicitly declare the new precondition before continuing.

API tests below complement the UI: an absent button demonstrates only graphical behavior, not server rejection. The test client must use the intended profile's session and an allowed origin, avoiding confusion between a permissions rejection and a session or origin-protection error.

## 3. Access and identity

### UAT-01 — Login, errors and logout

**Profile:** Elena. **Precondition:** active account, signed out.

1. Try submitting login with empty fields, then with a formally invalid email.
2. Try invalid credentials.
3. Enter assigned credentials, check Show/Hide password and press Sign in to your space.
4. Verify name, email and profile in the header, then press Sign out.

**Expected:** invalid fields are reported; incorrect credentials do not open the workspace; valid login displays the correct identity; after logout, authenticated APIs are no longer accessible through that session. Error messages do not expose other accounts' information.

### UAT-02 — Difference between Manager, Operator and Viewer

**Precondition:** a restricted project, Elena Contributor and Luca Viewer.

Open the workspace with all three profiles. Elena must see Projects, Time log, Analysis and Notifications. Luca has the same Operator profile, but can view the test project without logging new hours against it. Marta also sees Users, Groups and roles, Suppliers, Contracts, Alerts and triggers and History, plus Analysis's two financial views.

**Expected:** the project role changes capabilities on that resource, not the account's administrative features. Marta still has no form for assigning her own new hours to Elena.

### UAT-03 — Deactivation and an already-open session

**Profile:** Elena signed in; Marta administrator.

Marta deactivates Elena in Users. Elena tries refreshing a section or saving a new entry. Marta reactivates the account; Elena logs in again.

**Expected:** requests from the revoked session produce no new changes. Reactivation does not restore the old cookie; a new sign-in is required. Existing hours remain in administrative reports.

### UAT-04 — Demotion and the last Manager

**Precondition:** Marta and Bruno are the lab's only two active Managers. Also count any initial bootstrap account: if it is a third active Manager, this case does not reach the condition being checked. Prepare the isolated baseline so one account is the initial manager, or explicitly account for every active Manager before testing.

Marta changes her profile to Operator. In the already-open session, verify removal of restricted modules and restriction of analyses to personal hours. Sign in as Bruno and, in the lab, try demoting or deactivating this last active Manager too.

**Expected:** the first change applies; the second is rejected and Bruno retains the profile needed to administer the lab. Bruno can restore Marta for subsequent cases. Administrative data loaded by the previous session must not reappear through late UI responses.

## 4. Master records and documents

### UAT-05 — Supplier and contract

**Profile:** Marta.

Create an active supplier, then a contract with unique code and title, that supplier and value `1250,50`. Try another contract with the same code, a value with three decimals and an end before the start. Finally save a valid status and note edit.

**Expected:** the first contract is readable with the exact amount 1,250.50 EUR. Duplicate, excessive precision and reversed interval are rejected without new valid rows. The permitted edit appears in History. The end date alone does not automatically change status.

### UAT-06 — Supplier deactivation and contract statuses

Link a project to the contract, then deactivate the supplier. Verify the existing contract remains. Try selecting the supplier for a new link. Set the contract to Closed or Cancelled and try linking it to a new project.

**Expected:** existing references can be retained; new links require an active supplier and do not allow a Closed/Cancelled contract. The change does not automatically delete previous projects or hours.

### UAT-07 — PDF upload and download

Open PDF on the test contract, upload the valid synthetic PDF and download it. Compare size and SHA-256 fingerprint of original and downloaded file with a local tool. Upload a file with the same name a second time.

**Expected:** bytes and fingerprint match; download is an attachment. The second upload adds a distinct document instead of overwriting the first. No document editing or deletion commands appear.

### UAT-08 — Rejected PDFs and prohibited access

Try the text file renamed `.pdf`, the file over 10 MiB and a request without a valid file. As Elena and Luca, try the authenticated lab document download URL through the [app's document APIs](../tehnical/api-app.md).

**Expected:** invalid uploads add no documents; neither Operator receives PDF bytes. Authorization on the linked project does not grant contract-archive access. Deep content inspection and antivirus scanning are not features to claim as passed by these checks.

## 5. Projects and access

### UAT-09 — Creation, search and statuses

Create a project with code, name, client, description and contract. Search by code, name and client; apply a status filter excluding it, then Reset filters. Edit it through all five available statuses.

**Expected:** search and filters are consistent, a duplicate code is rejected and changed data persists on reload. Statuses do not impose a transition order. With an authorized Operator, a valid entry remains technically allowed on Paused, Completed or Cancelled projects too.

### UAT-10 — Parent group and subgroup

Create Area tecnica with children Sviluppo and Supporto. Add Elena only to Sviluppo and Luca only to Supporto. Assign the parent Contributor on project A; assign Sviluppo Viewer on project B.

**Expected:** both can log on A; only Elena views B and cannot log there. Sviluppo's permission does not pass to sibling Supporto. Removing Elena from Sviluppo removes inherited permissions from the next request, unless other grants exist.

### UAT-11 — Alternative paths and custom role

Assign Elena Contributor through a group and direct Viewer on the same project. Revoke the direct grant, then add a second group with writing and revoke the first. Also create a custom role initially read-only and verify how changing it affects logging capability on a separate project.

**Expected:** Viewer does not cancel writing obtained from the group. An incomplete revocation leaves the alternative path valid. Custom-role changes apply to grants using it; the two predefined roles cannot be edited.

### UAT-12 — Inclusive dates and the same grant

Use `T` as the server's current date and `W` as five days earlier. On the non-shared project, prepare a Contributor grant covering W and T: save one minute on W. Then replace it with two grants from different groups, one covering only W and one only T, with no single interval containing both. Try another minute on W.

**Expected:** the first save succeeds; the second is rejected even though the project is visible today. Repeat with Viewer covering W and T plus Contributor valid only today: entry on W remains prohibited. An endpoint exactly equal to the day is included.

### UAT-13 — Shared project

Create a shared project without assignments. Verify personal viewing and time entry as Elena and Luca. Create a new lab Operator and verify the same behavior. Add a direct Viewer grant for Elena and retry.

**Expected:** all active accounts can log through the shared property, even with direct Viewer. Disabling Shared project leaves only valid explicit access; Elena remains read-only if her sole remaining grant is Viewer.

### UAT-14 — Revocation while filling the form

Elena opens Log hours on a restricted project, prepares date, duration and note without submitting. Marta revokes her sole access. Elena tries saving or refreshes choices.

**Expected:** no new row is saved on the revoked project. The UI does not retain a usable selection based on old permission; when reloading options, it retains date, duration and note to allow a valid choice. Previous hours remain in personal log and analyses.

## 6. Hours and analyses

### UAT-15 — Formats and ownership of entries

As Elena, save three rows on an authorized project using `1,5`, `1:30` and `0:15`. Reload that day's Time log. As Marta, check the personal log for the same date.

**Expected:** Elena has 90 + 90 + 15 = **195 minutes**, three rows, total 3 h 15 min. Rows do not appear as Marta's personal hours. Editing/deletion commands are unavailable to Elena; her name is not a field replaceable with another user's.

### UAT-16 — Invalid durations and daily limit

On a separate day, try zero, a negative duration, `1:60` and a decimal fraction requiring fractions of a minute, such as `0,01`. Then save 24 valid hours and try adding another minute on the same day, including on a different project.

**Expected:** invalid values are rejected. The 24 hours can be saved; the next minute cannot, and the total remains 1440. The limit concerns person and day, not individual project.

### UAT-17 — Editing and deleting personal hours only

As Marta, enter a personal entry, edit it and use Delete entry. As Elena, keep a personal row on the same project. Verify that normal actions in Marta's log do not permit editing Elena's row; the API check in UAT-26 completes this test.

**Expected:** commands affect Marta's row and update her totals. Elena's row remains. History retains before/after details for permitted operations and can include notes: a personal log does not mean notes are always invisible to the Manager.

### UAT-18 — Five views with verifiable data

Load the [Analysis and cost examples](esempi-analisi-e-costi.md) dataset in a dedicated lab. Set the stated period and compare all tables with the document's expected results.

**Common expected result:** **540 minutes, 10 entries, 5 projects and 4 people**. Known contract value **10,000 EUR**, one contract without value and 60 minutes without a contract. Known cost **447 EUR** on 480 priced minutes, with 60 minutes without a rate and a partial-estimate indication. The shared contract's value is counted only once.

### UAT-19 — Filters, links and intervals

In the same dataset, combine date, project and user. For Elena and A across the full period, expect 210 minutes and two rows. Select the same From and To date; then try end before start and a 367-day period. Copy an analysis URL and open it in an Operator session.

**Expected:** combined filters and inclusive dates; invalid intervals rejected. The link does not grant Manager rights: the Operator remains limited to personal data and cannot access financial views. An empty period shows zero results without invented rows. Quick periods use the server-provided date.

### UAT-20 — Rates, zero, missing and overlap

Try a zero rate, no rate and two consecutive rates for the same person. Then try creating an interval sharing at least one day with an existing one. Edit a past rate as in the guide's numeric scenario.

**Expected:** zero and absence are distinct; endpoints are included and overlap is rejected. The rate valid on the work date determines cost. Editing the past recalculates the estimate without rewriting minutes. No rate-deletion command exists.

### UAT-21 — Per-entry rounding

On separate data, use a rate of 1 EUR/hour. Enter three one-minute rows and compare their cost with one three-minute row on another project. Repeat one minute at 0.30 EUR/hour.

**Expected:** three one-minute rows cost 0.06 EUR; the three-minute row costs 0.05 EUR. The minute at 0.30 EUR/hour costs 0.01 EUR. Rounding occurs before summing.

## 7. Alerts and notifications

### UAT-22 — Personal rule, recipients and time

As Marta, create a rule with name, type, time and at least one active user. Then try a rule without recipients, an invalid email address and a deactivated user. As Bruno, open Your rules.

**Expected:** only valid configurations are saved; up to 50 users and 50 emails. Bruno does not automatically see Marta's rules. The card shows Europe/Rome time, status, last and next run. If created after today's time, the next run is tomorrow. Automatic change between standard and daylight-saving time requires a dedicated temporal case in the technical suite; a single test today does not certify it.

### UAT-23 — Attempt on a nonexistent project

In the lab, create a temporary project and note its ID. Delete it according to UAT-27, then create the Nonexistent project rule before generating the observed attempt. With a test client authenticated as Elena, submit an entry with valid date and duration to that now-nonexistent ID. Use the `POST /api/time-entries` contract in the [API guide](../tehnical/api-app.md). Set Marta and Elena as recipients and press Run now.

**Expected:** saving hours is rejected; no invalid row appears in the log. Marta and Elena receive the relevant alert, while a different Operator recipient does not receive Elena's. A second run does not duplicate the anomaly. Simple access revocation on an existing project must not replace a nonexistent ID in this test.

### UAT-24 — Missing hours after 48 hours

This manual case requires time to elapse: it cannot be declared passed by pressing Run now immediately after rule creation.

1. On Monday `D`, create the Missing daily hours rule before the day's end, with Marta and Elena as recipients.
2. On D, log **479 minutes** for Elena and **480 minutes** for another active test Operator.
3. Run a check before 48 hours have elapsed from the midnight ending D.
4. Run it again after the deadline; in a week without a daylight-saving transition, D is eligible from Thursday at 00:00.
5. Run the check again and verify deduplication.

**Expected:** before the threshold, no alert concerning D; after it, Elena is incomplete and the person with 480 minutes is not. Other active users can produce additional alerts if also incomplete: monitoring is not limited to recipients. Saturday and Sunday are not days requiring completion. D's notification retains the detected total and is not continuously updated when hours are corrected.

For quick verification of exact thresholds, daylight-saving time and recovery after restart, use the dedicated technical time tests in `tests/test_alerts.py` on temporary data. Do not change a shared environment's clock to shorten this manual plan.

### UAT-25 — Reading, suspension and unconfigured email

Open an unread Elena notification and press Mark as read; compare the counter and Time log. Suspend the rule and still use Run now on a new anomaly. Prepare a synthetic address in the email channel while keeping SMTP unconfigured.

**Expected:** marking updates only acknowledgment. A suspended rule does not run automatically, but manual checks remain available. A successful manual check also records today's run: the automatic run for the same day does not repeat. Suspending the rule does not cancel already queued emails. The interface states emails are pending; no actual send must be presented as complete. Verifying SMTP acceptance and delivery requires dedicated configuration/fixtures and is not covered by this test.

## 8. Server checks and destructive operations

### UAT-26 — API rejection by profile and ownership

Use valid lab sessions and requests with correct origin. For fields, methods and complete format, see [App APIs](../tehnical/api-app.md). Perform at least:

| Request | Profile | Expected result |
| --- | --- | --- |
| `GET /api/users` or `/api/audit` | Operator | Administrative access rejected |
| `POST /api/jobs` with otherwise valid data | Operator | No creation |
| `GET /api/labor-rates` | Operator | No rate exposure |
| Financial report or another-person filter | Operator | Rejection, without colleague data |
| Edit/delete Elena's time-entry ID | Manager Marta | No modification of another person's entry |
| Add `user_id` to normal time entry | Operator | Field not permitted; no ownership transfer |
| Read or edit Marta's rule | Manager Bruno | Rule inaccessible as their own |
| Mark Elena's notification read | Luca | No change to another person's notification |

Codes can distinguish 403, 404 or 400 depending on the check described by APIs. The principal check is that no confidential data leaves and no modification occurs. A 401 caused by an absent session does not demonstrate profile checking.

### UAT-27 — Permanent project deletion

**Lab only.** Prepare a dedicated project with at least one Marta row and one Elena row, a direct grant and a group grant. Open Edit → Delete project; verify the text and cancel the first time. Reopen and confirm Delete project and hours.

**Expected:** cancelling retains data. Confirmation removes the project, all its hours and grants in one operation; totals decrease in both logs and analyses. Users, groups, roles, contract and PDFs remain. History retains the summary but offers no restore. Do not perform this case on a project needed by unfinished tests.

### UAT-28 — History and confidentiality of evidence

As Marta, filter History by projects, hours, grants, contracts, rates and rules. Open before/after details, reload and use Load earlier when available. As Elena, try accessing the same API.

**Expected:** actor, entity and change can be reconstructed; the Operator cannot access administrative history. Passwords, tokens and secrets are not in audit payloads. Time-entry notes may appear in details instead: evidence must use synthetic text. This case does not simulate an audit-write failure; atomicity on error is covered by dedicated technical tests.

## 9. Interface and integrations still to activate

### UAT-29 — Small screens and keyboard use

Repeat sign-in, opening the hours form, project selection, entry, cancellation and section navigation at indicative widths of 1440, 768, 390 and 320 px. Use Tab, arrows where supported for tabs, and Escape. Check labels, errors, focus and table scrolling.

**Expected:** essential commands remain reachable and no action requires the mouse exclusively. Wide tables can scroll within their own containers. Record observed contrast or accessibility issues: this test does not confer WCAG certification on the entire application.

### UAT-30 — Integration status and release limits

Verify that Alerts and triggers correctly states SMTP is absent. Through Calendar APIs, check configuration status expected for the environment and technical service availability. In the current installation, Google OAuth needs configuration.

**Expected:** missing configuration is not presented as a connected Google account or sent email. There is no Calendar UI section yet, nor event–project/hours conversion. Invoicing, receipts, multiple companies and multiple currencies are absent from the current flow. An absent screen for a planned feature is not a test defect in this version.

Actual tests of Google consent, reading/writing an authorized calendar, revocation, remote conflicts and SMTP sending must be recorded in a separate integration test when the provider is configured. Local-fixture tests do not attest connection to a real account.

## 10. Close the test session

The session owner completes a summary with all 30 identifiers, outcomes, cases not run and outstanding anomalies. Do not claim “all features verified” if temporal cases, server checks or a backend required for the release were missing.

A native SQLite session does not automatically demonstrate MySQL/GridFS behavior. The Docker lab also requires relevant technical checks for cluster, documents, TLS and networking. This plan does not require infrastructure failure tests or restarts of the operational stack.

Retain synthetic evidence and build state. For product criteria, see [Processes and rules](processi-e-regole.md); for automated tests and release, see [Development and testing](../tehnical/sviluppo-test.md).
