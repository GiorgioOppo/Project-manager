# Getting the workspace operational

[Italiano](../../prod/avvio-operativo.md) | **English**

This path takes an organization from first sign-in to a usable project, with hours, analyses and checks. It is a procedure to perform in the chosen installation, not a report of new operations performed while writing. Example names, codes and data are fictional.

For individual commands, also see the [manager guide](guida-gestore.md); for daily work, the [user guide](guida-utente.md).

## 1. Prerequisites and responsibilities

| Responsible person | Preparation | Evidence before starting |
| --- | --- | --- |
| Technical contact | Installation, URL, backups, initial account and service availability | Reachable login and working Manager account; known recovery procedure |
| Operations lead | Conventions for codes, shared projects, work attribution and statuses | List of people, teams and projects to register |
| Workspace manager | Master records, access, rates and check rules | Data verified by whoever is responsible for it |
| Operator | Logging their own work | Active account and correct selectable project |
| Review contact | Reading aggregates and assessing anomalies | Understanding the difference between hours, estimated cost and contract value |

In the current app, the Manager has global workspace powers. The organizational title “operations lead” is not a third application role. Administering only one area with limited powers will require the delegation planned in the [roadmap](roadmap.md).

This path does not require SMTP or Google Calendar: checks produce internal notifications and hours are entered manually. These providers will be configured separately. A second company with legal/operational isolation requires the future company module, not simply a new group.

## 2. Prepare a consistent example

The “Avvio portale Aurora” example uses these entities:

| Object | Fictional value | Function |
| --- | --- | --- |
| Manager | Marta, `marta.avvio@example.test` | Registration and checks |
| Operator | Elena, `elena.avvio@example.test` | Implementation and logging hours |
| Read-only operator | Luca, `luca.avvio@example.test` | Viewing the project |
| Parent group | Area tecnica | Organizational scope |
| Subgroup | Sviluppo | Elena's team |
| Supplier | `AVV-F01`, Servizi Aurora | Contract counterparty |
| Contract | `AVV-C01`, Accordo servizi portale | Project's contractual reference |
| Project | `AVV-J01`, Avvio portale Aurora | Destination for project hours |
| Shared project | `AVV-GEN`, Generale | General work in the example |

The `.test` addresses are for the lab only and must not be used as real contact addresses. The supplier contract is a reference in the current master data: its value does not generate revenue or invoices.

If repeating this path in a workspace already in use, adopt distinct codes and first check for existing records. Do not duplicate real people to work around an access error.

## 3. Register people and organization

**Marta, Manager:** open **Users → New user** and create Elena and Luca with the **Operator** profile. Set an initial password according to organizational procedures; this guide does not prescribe shared credentials. A newly created Operator account does not automatically receive every restricted project.

Open **Groups and roles → + Group** and create Area tecnica at Top level, then Sviluppo with Area tecnica as parent. Select Sviluppo and add Elena with **Add to group**. Luca can remain without a group in this example, because he will receive a direct grant.

Do not create a role named “Manager” in the project-role list to assign administrative powers. The two predefined roles suffice for this example: Contributor for Elena and Viewer for Luca.

**Check before continuing:** Elena appears among Sviluppo's direct members; Sviluppo is a child of Area tecnica; both accounts are active. Effective access changes if the group moves or a person's membership changes.

## 4. Register supplier, contract and original PDF

**Marta:** in **Suppliers → New supplier**, enter code AVV-F01 and name Servizi Aurora, retaining Active status. Add contacts and descriptive data only if available.

In **Contracts → New contract**, create AVV-C01 with title Accordo servizi portale and supplier AVV-F01. Set status Active and, in this example, value `10000,00` EUR. Dates are optional; when entered, the end date cannot precede the start date.

Open **PDF · number**, choose a synthetic lab PDF under 10 MiB and press **Upload PDF**. Check that it appears in the list and download it to verify the expected document. In operational use, the file must be the original authorized by your organization.

Every upload is additive. Do not repeatedly upload the same file to “update” it: the app does not replace the previous file and currently offers neither deletion nor formal document versioning. A contract's end date does not automatically make it Expired.

## 5. Create the project and shared activities

**Marta:** in **Projects → New project**, enter AVV-J01, Avvio portale Aurora and status In progress. Choose AVV-C01 in the Contract field. Leave **Shared project** disabled: this project has assigned access.

Also create AVV-GEN, Generale, with **Shared project** enabled and no contract. For the real workspace, check or prepare the agreed catalog of Generale, Formazione, Riunioni and Attività interne. Names are not reserved and the software does not mandatorily generate them.

Define a convention before starting: in the example, activities directly related to the portal go to AVV-J01, while general work not attributable to the project goes to AVV-GEN. This classification is an organizational choice, not an automatic system suggestion.

**Check:** both projects exist, but only Generale is shared. The contract is associated with AVV-J01, and its code avoids confusion with similarly named projects.

## 6. Grant and verify access

**Marta:** in the AVV-J01 row, press **Permissions**.

1. In **Group permissions**, assign Area tecnica with Contributor role and no date limits for this lab.
2. In **Grant user access**, assign Luca with Viewer role, also without date limits.

Elena receives time-entry capability through Area tecnica → Sviluppo. Luca can view the project but not log hours against it. Both can use AVV-GEN thanks to its shared property.

**Elena:** sign in with your account and check that AVV-J01 appears in Projects and the **Log hours** selector.

**Luca:** sign in through another session and check that AVV-J01 is viewable but unavailable for a new entry. His account remains Operator: Viewer is the role on this project alone.

Use separate browser profiles to avoid confusing identities during verification. The manager sees every project, so their list alone does not prove the operator's access is correct. The [access cases](scenari-accessi.md) explore dates, alternative roles and revocations.

## 7. Log an example day

**Elena:** choose a lab day `D` and enter two rows:

| Project | Duration | Fictional note |
| --- | --- | --- |
| AVV-J01 | `6:30` | Portal preparation |
| AVV-GEN | `1:30` | Organizational tasks |
| **Total** | **8:00** | **480 minutes, 2 entries** |

For each row, use **Log hours**, select the project, check Work day and press **Save hours**. In D's Time log, verify two rows and 8 hours overall.

The hours belong to Elena and must be entered from her session. Marta cannot assign Elena hours from her own form. If only exploring the app, perform this example in a test environment rather than a person's actual time records.

The alert threshold is 8 hours, but saving does not enforce 8-hour days: the technical constraint is between 1 minute per row and 24 total hours per day. In real use, logged activity must always match work performed.

## 8. Read results and estimated cost

**Marta:** open **Analysis**, set From = D, To = D and User = Elena, then press Apply. Work views must show 480 minutes, 2 entries, 2 projects and 1 user.

In **Contract value**, contract AVV-C01 appears with its full 10,000 EUR value and 6 hours 30 minutes linked. The remaining 1 hour 30 minutes are reported as work without a contract. The value is not prorated to 6 hours 30 minutes.

In **Labor cost → Hourly rates → New rate**, enter Elena's rate of 40 EUR/hour valid on D, without overlapping other rates for the same person. The day's estimated cost will be:

| Project | Calculation | Estimate |
| --- | --- | --- |
| AVV-J01 | 6.5 hours × 40 EUR | 260.00 EUR |
| AVV-GEN | 1.5 hours × 40 EUR | 60.00 EUR |
| **Total** | **8 hours × 40 EUR** | **320.00 EUR** |

A missing rate instead leaves hours unpriced. Do not use zero to mean the amount is unknown. The report is dynamic: a later change to the rate applicable on D recalculates this estimate. Do not automatically compare 10,000 EUR and 320 EUR as the day's margin: the first is the whole contract value, the second only the estimated cost of selected hours.

For a wider example with missing amounts, zero and multiple people, see [Analysis and cost examples](esempi-analisi-e-costi.md).

## 9. Enable daily checks

**Marta:** open **Alerts and triggers → New rule** and configure a Missing daily hours check at 09:00 Europe/Rome. Select Marta and Elena as user recipients and leave the email channel empty until needed and configured. You can prepare a second rule for Nonexistent project.

The hours check monitors active users, not just selected recipients. Marta can therefore receive alerts about all monitored people; Elena only about herself. In this example Luca and Marta, if active and without 8 hours, can be detected as incomplete days: the Viewer role on the project does not exempt Luca from checks.

The rule does not immediately evaluate old days predating its creation. For a representative check, set it up before the day to observe. A weekday becomes eligible after 48 actual hours from its end; **Run now** does not bring that time forward. Rules do not yet account for leave, public holidays, part-time work or personal exemptions.

After a run, check the rule card and **Notifications**. A rule with no anomalies can correctly create zero alerts. A successful **Run now** records today's run and prevents another automatic run that day; do not use it expecting two separate checks on the same day. Pressing **Mark as read** does not modify the time log.

## 10. Move into ordinary use

Before making the workspace operational, verify these results with each responsible party:

| Check | Who confirms it |
| --- | --- |
| Codes and names make projects recognizable | Operations lead |
| People see only intended projects and roles are correct | Manager testing as Operator/Viewer |
| Shared projects and reporting conventions are known | Manager and operators |
| Suppliers, contracts and PDFs are linked to correct resources | Archive owner |
| Missing values, zero and rates are interpreted correctly | Review contact |
| Rules have an appropriate owner, time and recipients | Manager owning the rules |
| Backups and recovery are documented | Technical contact |

Do not delete a lab project if real hours were accidentally entered against it: deletion removes all linked hours for every user. Destructive tests must be performed in an isolated workspace according to [acceptance testing](collaudo-accettazione.md).
