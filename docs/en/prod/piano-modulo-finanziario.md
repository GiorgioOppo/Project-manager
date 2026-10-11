# Financial module plan for punto

[Italiano](../../prod/piano-modulo-finanziario.md) | **English**

> **Design document; the full financial module remains to be implemented.** The initial baseline was schema 3; the current code uses schema 10 and provides contract values and estimated labor costs in EUR. Invoices, receipts, multiple companies, multiple currencies and tax channels described here remain planned. Consult [status and roadmap](roadmap.md) for the distinction between current availability and objectives.

The financial module will cover the path from project financial forecasting through invoicing, open items and their settlement. The first financial release will already include **invoicing and receipts**, **multiple companies**, **multiple currencies** and **companies with tax domiciles in different countries**. These requirements are part of the model's foundations and acceptance testing for the first delivery.

The plan complements the [20-feature roadmap](roadmap-20-funzionalita.md). F1 is a complete release, developed through internal increments; F2, F3 and F4 add automation, procurement and group reporting. The baseline for the first draft was schema 3: the entities and APIs below are the design to implement.

## Planned financial features

| Code | Feature | Contents | Delivery |
| --- | --- | --- | --- |
| FIN01 | Companies and countries | Owning company, tax profile, financial year, time zone, separate users and authority | F1 |
| FIN02 | Currencies and exchange rates | Document and functional currency, historical rates and settlement differences | F1; revaluations F4 |
| FIN03 | Customers and suppliers | Registered counterparties, commercial roles and versioned billing data | F1 |
| FIN04 | Commercial contracts | Sales distinct from purchases, time-based, fixed-price or milestone model | Base F1; quotations and recurring fees F2 |
| FIN05 | Project budgets | Approved versions of revenue, labor, purchases and expenses, with variances | F1 |
| FIN06 | Rates and resource costs | Cost and sales price with validity, currency and explicit precedence | F1 |
| FIN07 | Time valuation | Snapshots of hours and rates, billable sources and adjustments | F1 |
| FIN08 | Revenue and progress | Management revenue accrued under an approved rule, distinct from invoice and receipt | F1; forecasts to completion F2 |
| FIN09 | Purchases and direct costs | Project costs, linked invoices and remaining commitments | Costs and invoices F1; orders F2 |
| FIN10 | Expense reports and reimbursements | Documented expenses, request, approval and reimbursement without double costs | Manual expenses F1; contributor workflow F2 |
| FIN11 | Customer invoices | Drafts, lines, taxes, approval, documents and country-specific channel | F1 |
| FIN12 | Supplier invoices | Recording, verification, project classification and payables | F1 |
| FIN13 | Credit notes and advances | Adjustments, original documents and amounts to allocate | F1; advanced progress billing and retentions F2 |
| FIN14 | Tax rules and channels | Country profiles, checks, exchange and documented outcomes | Operational channel for each company activated in F1 |
| FIN15 | Due-date schedule and open items | Installments, balances, overdue amounts and aging of receivables and payables | F1 |
| FIN16 | Receipts and payments | Manual transactions, partial allocations, advance payments and reversals | F1 |
| FIN17 | Bank reconciliation | Statement import, duplicates and proposed matches | F3 |
| FIN18 | Margins and cash flows | Results by project, company, customer and period; forecasts | Current position F1; scenarios F2 |
| FIN19 | Journal and closing | Immutable events, balancing, locked periods and accounting export | Base F1; advanced closing F4 |
| FIN20 | Group and intercompany | Relations between companies, converted summaries and eliminations | Summary by currency F1; consolidation F4 |

## F1 First complete delivery

F1 is complete when the interface allows configuring a company and its profile, associating a customer and financial terms with a project, valuing costs and revenue, preparing an invoice, completing its required operational channel, recording a receipt and checking its allocation, balance and cash impact. The same release handles supplier invoices and manual payments.

| Internal increment | Work to complete | Completion evidence |
| --- | --- | --- |
| F1.0 | Company context, migration and isolation of existing APIs | No cross-access; previous data preserved |
| F1.1 | Exact money, countries, profiles, exchange rates, capabilities and numbering | Reproducible amounts and verified roles |
| F1.2 | Counterparties, commercial contracts, accounts and payment terms | Records usable for complete documents |
| F1.3 | Budgets, rates, costs, time valuation and management revenue | Financial view with traceable origins |
| F1.4 | Customer/supplier invoices, credit notes, files and country-specific channels | Usable flow for the companies at first startup |
| F1.5 | Opening balances, installments, receipts, payments, allocations, reversals and exchange differences | Open items and available balances reconciled from the start date |
| F1.6 | Overview, due-date schedule, journal, periods and export | Explainable totals and respected access rights |
| F1.7 | SQLite/MySQL, migration, cluster and browser acceptance testing | Complete scenarios passed before go-live |

F1 has high complexity because it also includes the company model. The increments build and verify the release; they do not move invoices and receipts to a later delivery. Duration will be estimated based on the actual countries, channels and team capacity.

## Companies and data isolation

Every project, contract, group and document belongs to a company. User identities remain global, while membership, operational profile and financial capabilities are assigned per company. The same user may be a manager in A, an operator in B and have no access to C.

Introduce `companies`, `company_memberships`, company administrative roles, capabilities and assignments. Add `company_id` to existing entities and composite relationships `(company_id,id)` to prevent cross-references. Codes are unique within their company scope.

New APIs carry the company in the path, for example `/api/companies/{company_id}/invoices`. The server checks company and resource on every request. The UI selector does not modify a global session variable: two tabs may operate on different companies. Frontend cache and state are separated by company, and responses for a previous context are ignored.

Previous APIs may automatically resolve the company only when the user has a single accessible scope; otherwise, they require explicit context. Search, counts, matrix, audit, exports, errors and downloads apply the same filter. Knowing a GridFS blob key does not grant the right to read it.

Operational managers do not gain authority over all companies. Technical platform administration does not automatically grant financial access; company authority is explicitly assigned and tracked. A project with existing transactions cannot change company through a simple field update.

The 24-hour limit remains per person and declared ISO date, summing entries even if work is split across companies. The company's time zone determines today and access validity without converting durations into time slots. Internal checks can calculate the limit, while reviewers see only their own scope. Revoking a project preserves personal history while company membership remains valid; revoking membership blocks that company's business data. Any personal access after leaving requires an explicit policy.

## Financial permissions

Viewer, Contributor and custom project roles continue to govern viewing and hours. Financial permissions are separate and can be limited to a company or projects within the same company.

| Proposed profile | Main capabilities | Scope |
| --- | --- | --- |
| Project controlling | Budgets, aggregate costs, management revenue and margins | Assigned projects |
| Billing | Preparing invoices, credit notes and documents | Assigned company; separate approval |
| Treasury | Receipts, payments, accounts and allocations | Authorized company and accounts |
| Financial administration | Financial configuration, approvals and periods | Assigned company |
| Financial viewing | Reports and due-date schedule | Authorized resources |
| Company administration | Members and assignment of allowed capabilities | Assigned company |

Indicative capabilities: `finance.read`, `budgets.manage`, `costs.aggregate`, `rates.read`, `rates.manage`, `invoices.draft`, `invoices.approve`, `invoices.finalize`, `invoices.credit`, `payments.record`, `payments.allocate`, `payments.reverse`, `balances.prepare`, `balances.approve`, `fx.manage`, `periods.lock`, `periods.reopen`, `finance.export` and `finance.consolidate`.

Individual costs, rates and bank details require specific capabilities. Financial aggregation of hours does not expose other users' personal notes. Sensitive operations distinguish preparation from approval; where approval by the same author is allowed, the rule is explicit and tracked. Revocations and expirations are rechecked between preview and confirmation.

## Currencies and exact calculations

Every company has a functional currency; documents and transactions retain their original currency. Group reports may have an additional presentation currency. These remain distinct and visible.

After the first financial posting, the functional currency cannot be changed through ordinary configuration; any change requires a later dedicated procedure. Exponent versions snapshotted in documents remain the original ones even after the catalog is updated.

Final amounts use 64-bit integers in minor currency units, with currency code and exponent version. `cents` remains only for compatibility with current EUR values. The currency catalog will be versioned and updatable from ISO 4217 authority data. [SIX and ISO 4217](https://www.six-group.com/en/products-services/financial-information/market-reference-data/data-standards.html)

Prices, quantities, tax rates and exchange rates use integer coefficients and explicit scales or exact fractions. A common C module applies limits, overflow checks and defined rounding; no `float` or `double`. In JSON, amounts and coefficients are strings, formatted without floating-point conversions. SQLite and MySQL must produce the same outcomes.

For an exchange rate defined as one source major unit equal to `num/den` destination major units:

```text
destination_minor = round(
  source_minor × num × 10^destination_exponent
  / (den × 10^source_exponent)
)
```

The rounding policy is versioned by profile, with rules for lines, taxes, total and differences. All intermediate values have verified limits. A missing exchange rate produces explicit incomplete data, never zero or an assumed rate of one.

A confirmed document retains original and functional amounts, date, source and exchange-rate snapshot. Settlement retains the actual rate and difference from the item's value. Updating the price list does not rewrite the past. ECB exchange rates may feed reports or proposals with provenance: the ECB publishes them for information and discourages their use as transaction rates. [ECB reference exchange rates](https://www.ecb.europa.eu/stats/exchange_rates/html/index.en.html)

Summaries show amounts per currency or an explicit conversion. A converted total across companies is not presented as consolidation with intercompany eliminations.

## Customers and commercial contracts

Current contracts require `supplier_id`, while `jobs.client` is a free-text description. Their value is therefore neither an automatic revenue source nor sufficient master data for invoicing.

Introduce counterparties per company with customer, supplier or both roles and versioned tax/commercial data. Preserve existing suppliers with explicit mapping. Distinguish purchases from sales; a project may have a commercial contract and multiple procurement references without reinterpreting previous ones.

The sales contract defines customer, currency, validity, payment terms and a time-based, fixed-price or milestone model. Verify master data and profile before finalizing an invoice. The document saves a snapshot: changing an address or terms does not modify already confirmed invoices.

## Project cost and revenue budgets

Budgets have draft, submitted, approved and superseded versions. The approved baseline contains expected revenue, labor minutes/cost, purchases and expenses; each new approval retains the previous version.

Cost and sales rates have validity, currency and scope. Precedence among contract, project, role and person is explicit; ambiguous overlaps are rejected. Personnel cost may include the company component defined by the controller without exposing salaries to operators.

Time valuation saves user, project, date, minutes, entry revision, rate, exchange rate and result. Missing data remains to be completed. Changing a rate does not alter approved actuals. Adjusting hours produces a new proposal and, if necessary, reversal and replacement of the financial effect.

F1 also intercepts updates and deletions through existing time APIs. Valued or invoiced sources retain revisions and use logical deletion: no change destroys the financial snapshot or automatically releases quantities already invoiced. Commercial adjustments require the applicable document workflow, even in an open period.

| Indicator | Origin | Rule |
| --- | --- | --- |
| Budget | Approved version | Forecast, not an actual transaction |
| Commitment | Approved order/obligation | Outstanding commitment, distinct from accrued cost |
| Accrued management cost | Valued labor, expense or recognized purchase | One source produces only one active cost |
| Accrued management revenue | T&M rule, milestone or approved manual event | Does not automatically equal hours × price |
| Net invoiced amount | Valid active documents, net of taxes and adjustments | Does not add recognized revenue again |
| Receipts and payments | Recorded treasury transactions | Flows distinct from revenue and costs |
| Outstanding receivables and payables | Open items and allocations | Amount to settle in the item's currency |
| Management margin | Management revenue minus management cost | Declared period, currency and criterion |

A supplier invoice linked to an already recognized expense links it to the payable or replaces recognition through an explicit relationship, without double costs. Billable sources retain remaining minutes, quantities and amounts to prevent double invoicing.

## Invoicing and country profiles

The common engine manages header, lines, quantities, prices, discounts, tax components, due dates and billable sources. An invoice may allocate lines across multiple projects of the same company. Tax rates, exemptions, reverse charge, withholdings and other components are versioned configurations of the applicable profile, not deductions from currency alone.

Every company records its country and any additional required tax registrations. A document profile identifies transaction type, format, calculations, numbering, checks and channel. The European Commission maintains national factsheets distinguishing platforms, specifications and reporting: the plan provides for country adapters. [European Commission eInvoicing factsheets](https://ec.europa.eu/digital-building-blocks/sites/spaces/DIGITAL/pages/467108874/eInvoicing%2BCountry%2BFactsheets%2Bfor%2Beach%2BMember%2BState%2Band%2Bother%2Bcountries)

States are separate:

- **Internal document:** draft, submitted, approved, finalized; finalization freezes content and number.
- **Tax/delivery channel:** to prepare, ready, transmitted, rejected, accepted or delivered according to the protocol. Do not force every country into the same events.
- **Management posting:** to post, posted or reversed according to the profile.
- **Settlement:** unsettled, partial or settled, derived from allocations; overdue status is a property of open installments.

First startup includes a validated operational channel for each activated company. Planned modes: generating/exchanging files with the existing tax or accounting system and acquiring outcomes, or a tested connector to the chosen provider. File exchange keeps the app without Internet egress; a connector requires a dedicated component, authorized destinations, an outbox and persistent receipts.

For Italy, the adapter provides XML and SdI outcomes according to applicable specifications. The PDF is a readable representation and does not replace the structured flow required by the profile. [Italian Revenue Agency electronic invoicing](https://agenziaentrate.gov.it/portale/web/english/electronic-invoicing)

General support for foreign companies does not automatically enable every tax regime. The named list of countries and channels becomes part of the F1 definition: format, transactions, outcomes and retention are validated for each company before issuance in the relevant country. Where the external system remains the issuing system, punto acquires its final documents and references without presenting them as internally generated or transmitted.

Finalized invoices are corrected with credit notes or profile procedures, without rewriting. Rejections follow the channel's correction workflow, retaining attempts without duplicating financial effects. Numbering uses a transactional sequence per company, series, type and financial year, with a unique constraint; not `MAX(numero)+1` or the automatic ID. External document numbers retain their originals.

Supplier invoices retain external reference, supplier, and separate document and receipt dates. Duplicate checking considers company, counterparty, references and fingerprint: the same number from different suppliers is not automatically a duplicate.

## Receipts, payments and open items

Every document produces one or more due installments with currency, amount due and date. A receipt or payment identifies company, account, counterparty, date, currency, amount, source and reference. Recording a payment in the management system is distinct from executing a bank transfer.

Every treasury account has a currency. Cash transactions are expressed in that currency; a settled document may use another through allocation and an explicit exchange rate. Transfers or exchanges between accounts use separate linked transactions, with any fees separate, without implicit balance conversion. Allocation requires the same company, counterparty and compatible direction: a customer receipt does not settle a supplier payable. Third-party payments and offsets between items require a dedicated workflow before being allowed.

A transaction may be partial, cover several installments or remain an unallocated advance. Every allocation retains the portion in transaction and item currencies, the functional equivalent and exchange difference. EUR is not directly subtracted from a USD item.

Remaining balances derive from allocations, credits and reversals; no command arbitrarily marks an invoice as paid. Excess remains an advance. A credit note is allocated once, without reducing both the original total and remaining balance through two paths.

Recording, allocation and reversal require revision, UUID, current authorization and a transaction with audit. Allocations cannot exceed an item's remaining balance or a transaction's available amount. Reversing a payment neutralizes its allocations in the same transaction and reopens items; an actual refund of money is a separate transaction.

## Start date and opening balances

F1 defines a start date per company. An opening batch collects account balances, outstanding receivables and payables, unallocated advances and their source documents. Each item retains counterparty, due date, original currency and functional equivalent, with documented exchange rate and criterion.

Preparation and approval are separate. Opening produces idempotent balanced entries against opening accounts, without generating previously accrued revenue or costs again. Verification reconciles accounts and items with sources at the chosen date; differences or missing data prevent presenting the balance as complete. Later importing a historical invoice links it to the opening item, without creating a second payable or receivable.

Previous hours retain their operational value. They are not automatically valued by migration: any financial catch-up requires explicit rates, interval, sources and approval, avoiding overlap with openings.

## Management journal and periods

Introduce an immutable journal of financial effects, with unique source and version. Each effect indicates company, management accrual date, period, accounts/categories, project and amounts. Entries adopt debit/credit balancing in functional currency and make balances, rounding and exchange differences reconstructable. Management double-entry bookkeeping does not automatically confer compliance with countries' statutory accounting; integration formats and rules are validated per company.

A period may be open or locked. Locking also covers indirect changes: already valued hours, reallocations or rates do not rewrite historical effects. Adjustments go into an open period; reopening requires a specific capability, reason and audit.

Invoice entries separate net amount, taxes, open item and links to previously recognized revenue/costs. Cash settles the item without creating revenue or cost again. Posting rules are versioned, with balancing examples for every event before development.

## Interface and reports

The **Finance** section includes Overview, Projects, Customers and suppliers, Budget, Invoices, Receipts and payments, Due-date schedule, Journal and Configuration. Every page indicates company, period and currency; sections and actions depend on the actor's capabilities.

The project's financial view shows budget, accrued cost, management revenue, margin, invoices, allocated receipts and balances. Each total opens its sources within permissions; individual costs are hidden when only aggregation is allowed.

The invoice shows internal state, channel, installments, allocations, adjustments and history. Treasury distinguishes recorded transactions, unallocated amounts and forecasts. The balance indicates the last reconciliation; it does not appear as a real-time bank balance without connection and verification.

F1 includes a summary of authorized companies by currency; every conversion declares exchange rates and coverage. F4 adds management consolidation, group relationships and eliminations. Access to one company does not grant details of the others.

## Proposed data model

The final table count will be established in the detailed schema.

| Module | Main entities |
| --- | --- |
| Companies and access | `companies`, `company_memberships`, `company_roles`, `company_role_capabilities`, `company_role_bindings`, project scopes |
| Counterparties | `counterparties`, roles, versioned records/addresses, supplier mapping |
| Money and profiles | `currencies`, `fx_rates`, `company_finance_settings`, `tax_rule_versions`, `invoice_profiles`, `number_sequences` |
| Commercial contracts | Sales and terms, purchase references, `job_finance_settings` |
| Project finances | `budget_versions`, `budget_lines`, `rate_versions`, `time_valuations`, `revenue_events`, `expense_items` |
| Documents | `invoices`, `invoice_lines`, `invoice_line_job_allocations`, `invoice_source_allocations`, `invoice_installments`, `invoice_document_links`, `invoice_transmissions` |
| Treasury | `treasury_accounts`, `payments`, `payment_allocations`, `credit_allocations`, opening batches and lines |
| Journal | `financial_periods`, `financial_events`, `financial_event_lines`; reuse audit and `mutation_receipts` |
| Extensions | Orders/commitments, expense reports, statements, matching, scenarios and intercompany relationships |

Every company entity uses `company_id`, revision, author and technical dates. Amounts include currency and exponent; posted effects also include functional equivalent, exchange rate and rule. Invoice, project, account, counterparty and allocation require the same company, except for explicit intercompany links.

PDFs, XML, any signed envelopes, receipts and supporting documents are in a separate financial GridFS store, with SQL metadata and hashes. The validator is separate from the current one for contract PDFs only. XML import has no external entities or DTD, with limits on structure, size and archives. Blob backup and integrity remain distinct from country-profile retention procedures.

SQL and MongoDB do not share a distributed transaction: use persistent to-prepare, ready or failed states, snapshot-based generation, idempotent retries and orphan reconciliation. The external channel has an outbox and uniquely identifies receipts and attempts.

## Proposed APIs

Financial routes are in the `/api/companies/{company_id}` context. The following paths are relative to this prefix.

| Path | Function |
| --- | --- |
| `/finance-settings`, `/memberships`, `/finance-roles` | Configuration and capabilities |
| `/counterparties`, `/sales-contracts` | Counterparties and commercial contracts |
| `/currencies`, `/fx-rates`, `/invoice-profiles`, `/tax-rules` | Currencies, exchange rates and profiles |
| `/jobs/{job_id}/finance` and `/budgets`, `/rates`, `/valuations`, `/revenue-events` subresources | Project finances |
| `/invoices` | Customer/supplier documents, invoices/credit notes |
| `/invoices/{id}/submit`, `/approve`, `/finalize` | Authorized transitions |
| `/invoices/{id}/installments`, `/documents`, `/transmissions` | Installments, files and outcomes |
| `/payments`, `/payments/{id}/post`, `/allocations`, `/reverse` | Transactions and allocations |
| `/opening-balances`, `/opening-balances/{id}/approve` | Opening preparation and approval |
| `/dashboard`, `/aging`, `/cash-flow`, `/journal` | Reports and journal |
| `/periods/{id}/lock`, `/reopen` | Closing and reopening |

Writes specify revision and UUID; amounts are canonical strings. The server performs authoritative calculations. Previews produce no financial effects; confirmation repeats checks and authorization in the transaction.

## F2, F3 and F4 developments

| Release | Contents | Dependency |
| --- | --- | --- |
| F2 Procurement and planning | Quotations, orders, commitments, order/receipt/invoice comparison, expense reports, recurring fees, progress billing, retentions, recurring proposals, projected cash flow and margin at completion | F1 and approvals |
| F3 Banks and automation | Statements, deduplication, assisted reconciliation, matching, other connectors and accounting exchange | F1 open items; R3 imports |
| F4 Group and controlling | Intercompany with independent documents, matching, group conversion, eliminations, revaluations and advanced closing | F1; verified reconciliations and profiles |

Connectors required by first go-live countries already belong to F1. F3 adds automation and channels, not the first ability to invoice. Banking starts with data acquisition and reconciliation; any transaction-initiation flows are separate processes with explicit authorization.

## Integration with the existing roadmap

- **R0 mandatory:** revisions, idempotency, validators, resource authorization and transactions with audit. F1.0 extends context to the company before activating the second scope.
- **R1 reusable:** accessible components, views, commands and matrix. Completing all visual improvements is not necessary to begin the financial domain.
- **R2 feeds actuals:** the grid and adjustment requests interact with valuations and periods. F1 must bring forward revisions, logical deletion and protection of valued or invoiced sources on all existing paths; only the full request workflow may wait for R2.
- **R3 provides import and batch:** add financial validators without importing implicit postings or duplicate transactions.
- **R4 expands delegation:** financial capabilities only if assigned; F1 already has financial capabilities per company.
- **R5 revisions and obligations:** F1 invoices have their own snapshots regardless; new revisions do not modify already finalized documents.
- **R6 and R7:** offline initially for hours. Invoice finalization, posting and payments online; OCR proposes fields without approving transactions.

## Existing data migration

1. SQL/GridFS backup, counts and fingerprints; test on a schema 3 copy.
2. Create an initial company for current data, preserving IDs, credential hashes, storage keys, hours and grants. Complete tax data during onboarding.
3. Transfer operational authority to the initial company without granting implicit financial read access. Explicitly appoint financial administrators in setup.
4. Make groups, roles and grants company-scoped. Resolve Viewer/Contributor by code within the company, removing fixed references to IDs 1 and 2 before creating other catalogs.
5. Preserve EUR values as minor units with exponent 2; migrate `value_cents` and the EUR constraint without financial conversions. Legacy compatibility only in routes declaring EUR.
6. Preserve `jobs.client` until explicit counterparty mapping, and retain the supplier side of old contracts. No invoice or revenue generation during migration.
7. Add company to audit without changing historical payloads or immutability, through controlled metadata migration and no domain-event rewriting.
8. DDL once on the chosen node, writes suspended and alignment checked. Access and integrity tests before the second company.

Equivalent SQLite/MySQL migration. After financial transactions or transmissions, recovery must respect the documents, receipts and external systems involved: no automatic destructive reverse migration.

## Acceptance testing and release conditions

F1 must pass at least these scenarios:

1. A and B with the same codes and different permissions; no leaks through lists, audit, search, export, PDFs or errors. Two tabs with independent contexts.
2. Operational manager without finance; aggregate costs accessible only with a dedicated capability, without individual notes.
3. Currencies with different exponents, rounding, fractional quantities and overflow; identical C/SQLite/MySQL results. Missing exchange rates reported.
4. Invoice across several projects, installments and currency different from functional; unique numbering with requests on all three nodes.
5. Partial receipts, advance, credit note and reversals; balances and exchange differences reconciled.
6. Adjustments to valued hours and a supplier invoice on an existing expense; no duplicate cost/revenue or excessive consumption of billable sources.
7. Duplicate submission and timeout after commit/failover; only one invoice, posting and allocation.
8. Period locked even against indirect changes; adjustments in an open period and tracked reopening.
9. Go-live country profiles: consistent files, checks, outcomes, rejections and external documents. No tax issuance inferred from the PDF alone.
10. Crash between SQL, documents and channel: recovery, unchanged hashes and no double accounting.
11. Previous EUR data, operational permissions and documents preserved; schema and data agree across all three nodes.
12. Approved and reconciled openings, repeated import without further effects and historical document linked without duplicating the item.
13. Allocations between incompatible companies, counterparties or directions rejected; transfers between accounts in different currencies with reconciled balances and fees.
14. Functional currency locked after the first posting; catalog updates without changes to historical documents.

Numerical example without tax components: a 1,000 USD invoice valued at 900 EUR; a 368 EUR receipt allocated to 400 USD, a 285 EUR receipt allocated to 300 USD, a 100 USD credit note valued at 90 EUR and allocated to the invoice. Expected balance: 200 USD, original remaining value 180 EUR and realized exchange difference on the two receipts 23 EUR. Cash: 653 EUR, without a second revenue. Actual posting follows the approved profile.

## Data to define before go-live

Collect the list of companies and countries, functional currencies, tax registrations, financial years, existing accounting/tax systems and exchange methods. Define administrators, approvers, resource costs, commercial models, rounding, start date and opening balance sources.

Collection occurs at the beginning of F1 in parallel with company context and the monetary module. The design does not assume all companies are Italian or all currencies have two decimal places.
