# Asset inventory

[Italiano](../../prod/asset-inventory.md) | **English**

[Product index](README.md) · [Technical guide](../tehnical/asset-inventory.md)

Inventory collects IT devices, network equipment, physical assets, software and licenses. It retains location, assignment, status, purchase and warranty information, with type-specific fields and links between records. It is available to authenticated users: everyone can view the catalog, relationships and history; only the **Manager** can add or edit records and create or remove relationships.

## View assets

Open the **Asset inventory** tab. Search and filters for status, type, site and department narrow the catalog; search includes type-specific details, such as hostname, publisher and license reference. Department choices narrow according to the selected site; deactivated master records remain selectable to find previously registered assets. Pagination lets you read additional assets without loading the whole archive. The **Entire inventory** summary shows global counts even when the list is filtered. Open an asset to read its record, relationships and revision history.

The **Operational dashboard** follows every filter, including **My assets**, and counts all results beyond the loaded page. It shows open handovers, overdue returns, assets never checked, checks to repeat and locations to review. Selecting a dashboard card filters the list; press the same card again or **Clear operational filter** to remove it. **Reset filters** restores the full catalog. The expandable distribution groups assets by site, department and user, showing at most the 20 largest groups.

Handover and check counts cover IT devices, network equipment and physical assets, excluding retired and lost assets. A return becomes overdue on the day after its expected date. **Repeat verification after** offers 30, 90, 180 or 365 days: a check is due again when the last recorded date plus the interval reaches the current day; assets never checked have a separate count. Locations to review correspond to the latest check showing an observed place different from the expected one. Changing the dashboard interval changes viewing; alert-rule intervals are set in each rule.

| Status | Operational meaning |
| --- | --- |
| Available | Asset ready to be assigned. |
| Assigned | Asset entrusted to the person or function indicated as assignee. |
| Under maintenance | Asset temporarily unavailable for maintenance. |
| Retired | Asset withdrawn from use, retained in the archive. |
| Lost | Asset that can no longer be found. |

Statuses are updated by the Manager. Assignment, handover and return also update status; merely passing a date does not change it.

## Create and update a record

With **New asset**, the Manager enters a unique inventory tag and a name. The tag distinguishes the asset even when other assets have the same model. Category is free-form and defaults to `hardware`; manufacturer, model, serial number and notes describe the asset. Text location and assignee fields remain compatible with older records; to link an asset to a person in the app, use structured assignment from the record.

The type selects the record's fields, while category remains a free-form classification:

| Type | Specific information |
| --- | --- |
| IT device | Hostname, IPv4 or IPv6 address, MAC, operating system. |
| Network equipment | IT device fields, network role, VLAN and number of ports. |
| Physical asset | Asset kind, department, next maintenance. |
| Software | Publisher, catalog version, edition, software type. |
| License | Referenced software, model, purchased seats, expiration, renewal date and method, administrative reference. |

For a license, first register its software and specify at least one purchased seat. The administrative reference can be an order or contract number: do not enter activation keys. Records existing before types were introduced initially become IT devices with empty details; the Manager can reclassify them.

Purchase and warranty expiration are optional. When present, they must be valid dates; warranty cannot precede purchase. Limits are 64 bytes for the tag, 160 for name, location and assignee, 80 for category, 120 for serial number, manufacturer and model, and 4000 for notes. Accented characters can occupy multiple bytes.

The **Asset record** displays details, relationships and history with revision copies; **Edit** opens the form for Managers. A type with active relationships cannot change until those relationships are removed; software referenced by a license record must remain software.

Every creation and edit preserves actor, date, version and a copy of the resulting record. If another Manager saves first, the app reports a conflict: the draft remains visible and the updated record must be explicitly reloaded before repeating changes. The operation does not silently overwrite someone else's work.

To withdraw an asset, set **Retired**. No delete command is available: identifier and history remain viewable.

## People and identities

The **People** and **Identities** commands open two separate directories, viewable by users and editable by Managers. A person represents a real individual, including someone without access to Punto; an identity represents an account in a system or domain. For example, Mario can have an email account and a VPN account; Mario and Anna can use the same departmental account. The relationship is many-to-many, regardless of whether the identity is classified as personal, shared, or service.

A person has a unique code, name, email, organization, job title, and notes. An identity has a system/domain and account name, unique as a pair, a display name, type, and notes. Directory entries retain their active status, actor, version, and history. Do not enter credentials in these fields: the records catalog accounts and do not enable access.

From an identity record, the Manager links one or more people as owner or user. A person record shows all associated identities. Search and pagination allow browsing and selection in large directories. Removal preserves history; deactivating an entry retains existing references and prevents new links.

In an asset record, **Linked people and identities** lets the Manager record people and accounts as owner, user, or administrator. Directory records show directly linked assets and offer **View assets** to filter the catalog, dashboard, and export. Linking a shared account to an asset does not automatically assign that asset to everyone using the account. Links do not change asset status, handovers, license seats, or access permissions. Existing handovers continue to use Punto users.

## People, sites and locations

The Manager can manage master records for **sites**, **departments** and **locations**, then select them in IT device, network equipment or physical asset records. Each location belongs to a site; a department can be linked to a site. The record checks that selected places are consistent. To change the site of a department or location already in use, first remove it from assets' current contexts. Master records can be deactivated; previous entries retain the names saved in history.

Structured assignment links the asset to an active app user and sets its status to **Assigned**. Removing it returns an assigned asset to **Available**, retaining its location. The **My assets** filter shows assets linked to the current user; the full catalog remains viewable by all authenticated users.

Software and licenses continue to use their own details and relationships: personal license assignments remain text-based. The general editor and CSV import do not replace the assignment, handover and return flow for assets already managed in a structured way.

## Hand over, return and check an asset

From the record, the Manager can register a **handover** to an active user, specifying date, expected return, initial condition and notes. The asset must be available or already structurally assigned to the same user, with no open handover. Handover assigns it to the user; an overdue expected return appears as late in the record.

A **return** records the date, final condition and notes, closes the handover and removes personal assignment. The Manager chooses whether to make the asset available or put it under maintenance. Site, department and location remain recorded. Actual dates cannot be in the future, and expected return cannot precede handover.

**Physical check** records date, condition, optional observed location and notes. The record retains the most recent check and allows previous checks to be viewed. An observed location differing from the expected one is recorded as a discrepancy: it does not automatically move the asset. Checks are manual operations; receiving periodic alerts requires configuring an **Inventory verifications** rule.

Handover, return and checks retain actor, version and history. A version conflict requires reloading the record before confirming again.

## QR labels

**QR label** opens a preview with inventory code, name and a QR code for the record. You can download the SVG file or print only the label in 90 × 50 mm format. Keep the white border and good contrast when resizing or printing the code.

Scanning opens the record in the app and requires authentication. The QR contains the installation address and asset identifier; it does not contain credentials or record details. Generate it from the final address reachable by phones: changing domain or port requires reprinting labels. The code is generated in the browser with a local library, without sending data to an external service.

## Installations, networking and licenses

Relationships are manually recorded by the Manager and appear on both source and destination:

- **Software** can be installed on an IT device or network equipment. The relationship can record an installed version different from the software record's version.
- An **IT device or network equipment** can be connected to other network equipment, specifying ports on both sides.
- A **license** can be assigned to a device or person according to the selected model. The person is free text, not an app account.

The **per-device** model requires an IT device or network equipment record. **By user** requires a text assignee. **Concurrent** and **site** allow recording a device or assignee, one at a time. The relationship's quantity consumes seats specified in the license: counts show assigned and remaining seats based on these records.

Active relationships cannot be duplicated, purchased seats cannot be exceeded, expired licenses cannot be assigned, and links cannot be made to retired or lost records. Capacity cannot fall below already assigned seats. To change a license's software or model, first remove its active assignments.

Removal deactivates the link, releases assigned seats and preserves the event in history. Relationships also check the source record's version: a concurrent change requires reloading the record. Adding or removing a relationship does not change the destination's revision.

## Import and export CSV

**Export CSV** downloads all assets matching selected filters, including beyond the visible page and respecting **My assets**. The file includes common fields, type, JSON details, identifier and version; relationships, master records, structured assignments, handovers and checks are not imported from CSV. The interface limits each export to 20 MiB: narrow the filters for larger archives.

The Manager can use **Import CSV**, download the template and upload a UTF-8 comma-separated file, up to 256 KiB and 200 rows. Required fields are `asset_tag` and `name`; `details` contains a JSON object of type-specific fields. Unknown or duplicate columns, invalid data and repeated codes within the file are reported in the preview.

The initial mode creates new assets only. **Create and update by inventory code** can update existing ones: start with a recent export and retain `version`. An omitted column leaves that field unchanged; an empty cell in a present column clears it, if allowed. Do not remove the version to force an update. To import licenses, first register the referenced software: `software_id` must point to an existing software record in the same inventory.

**Check file** shows operations and errors without modifying records. Confirmation is available only when every row is valid and applies the whole file together, preserving history. The preview expires after 15 minutes and can be used only once by the same Manager. A conflict cancels the entire batch and requires a new preview.

Values that spreadsheet applications might interpret as formulas are exported with a protective apostrophe. Reimporting removes that prefix; for a literal apostrophe before `=`, `+`, `-` or `@`, or before another apostrophe, write two in a manual CSV.

## Alerts for deadlines, returns and checks

In **Alert rules**, the Manager can create the **Inventory due dates** check, choose a lead time of 1 to 365 days, daily time and recipients. It covers warranty end, next maintenance for physical assets, license expiration and renewal, including overdue dates; retired and lost assets are excluded.

Every selected recipient receives alerts about the shared catalog. The notification opens the asset directly. The same combination of asset, deadline type, date and lead time is not notified again by the same rule; changing the date or lead time can generate a new alert. No rule or recipient is created automatically. The email channel uses the configuration already provided for other alerts.

**Overdue inventory returns** reports handovers still open from the day after the expected return. **Inventory verifications** reports physical assets never checked and those whose configured interval has elapsed, from 1 to 365 days, initially 90. Both checks exclude retired and lost assets. Repeating a check does not duplicate previously sent alerts; a new handover or check can produce new alerts. Closing the handover or recording a recent check removes the asset from subsequent results of the respective check without deleting previous notifications. These rules use the full company catalog, not temporary dashboard filters.

If inventory is unavailable, the check is not marked complete: the scheduler can retry it. Alerts do not perform renewals or maintenance.

## Scope and availability

The catalog is shared within the workspace and does not apply individual project roles. When the Ticket module is configured, the asset record allows opening linked tickets or creating one. It does not include autodiscovery, network scanning, imports from agents or links to projects, contracts and time entries. Installations, connections, handovers and checks are not detected from devices. Available license numbers are administrative counts of registered assignments; for concurrent licenses they do not measure sessions in use or verify compliance with supplier terms. Rules configured by Managers can generate deadline, return and check alerts; renewals and maintenance remain manual operations.

Inventory uses its own C service and storage. If the module is not configured or the service is unavailable, inventory requests display an error; the administrator can check the service using the [technical guide](../tehnical/asset-inventory.md).

## Data protection

In the Compose installation, inventory automatically creates a verified copy every 24 hours, keeping the latest 14 in a volume separate from storage. The first backup occurs at initial startup; later restarts respect the next scheduled time. The administrator can change frequency and retention and check the latest success and errors in service status. A backup error does not interrupt viewing or saving records.

The general MySQL backup does not include inventory. Local copies also need external retention to remain recoverable after host loss. The [technical procedure](../tehnical/asset-inventory.md#backup-recovery-and-checks) allows checking a copy and restoring it to a new file without overwriting active storage.
