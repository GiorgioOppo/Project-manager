# Practical project access scenarios

[Italiano](../../prod/scenari-accessi.md) | **English**

These examples illustrate the product's actual rules. They are reasoning and testing cases, not assignments to apply automatically. All people and projects are fictional. Their creation procedure is in the [manager guide](guida-gestore.md).

## 1. Distinguish person, profile and operational role

A person signs in with a **Manager** or **Operator** account. When assigning a project to a user or group, an operational role is chosen instead: **Viewer**, **Contributor** or a custom role.

An Operator account with Contributor role can view the assigned project and log personal work against it. They cannot administer that project, even if the custom role name were “Owner”. A Manager account has global powers over workspace projects without needing a grant.

In the following matrices:

| Symbol | Meaning |
| --- | --- |
| **L+O** | Can read the project and log personal hours on the case's date |
| **L** | Can read but not log new hours |
| **—** | No current project access |

L means viewing the project, not access to underlying contracts/PDFs or other people's hours. L+O does not include editing another user's entries.

## 2. A tree with different assignments

Example groups and direct memberships:

```text
Area tecnica                       Paolo is a direct member
├── Sviluppo                        Elena is a direct member
└── Supporto                        Lucia is a direct member

Luca                               no group
Marta                              Manager profile
```

Paolo, Elena, Lucia and Luca are active Operator accounts. Intervals are unlimited in this first scenario. Create these grants:

| Project | Configuration |
| --- | --- |
| ACC-A, Portale | Area tecnica: Contributor |
| ACC-B, Manuale | Sviluppo: Viewer |
| ACC-C, Affiancamento | Luca: direct Contributor |
| ACC-G, Generale | Shared project |
| ACC-Z, Riservata | No grants and not shared |

Expected outcome for the current work date:

| Person | ACC-A | ACC-B | ACC-C | ACC-G | ACC-Z |
| --- | --- | --- | --- | --- | --- |
| Elena, Sviluppo | L+O | L | — | L+O | — |
| Lucia, Supporto | L+O | — | — | L+O | — |
| Paolo, Area tecnica | L+O | — | — | L+O | — |
| Luca, no group | — | — | L+O | L+O | — |
| Marta, Manager | L+O | L+O | L+O | L+O | L+O |

The grant to parent Area tecnica reaches direct members and descendants. The Sviluppo grant does not travel upward to give ACC-B to Paolo or across to sibling group Supporto. Marta also retains administrative powers; the matrix shows only reading and personal time entry.

If a new Operator is created without memberships or grants, they can use ACC-G but do not acquire the four restricted projects. Adding them to Sviluppo gives them inherited ACC-A and ACC-B access from the next request.

## 3. Multiple access paths do not cancel one another

Start from the previous example, focusing on Elena and ACC-A. Listed changes are sequential; all dates remain unlimited.

| Step | Elena's situation | Outcome on ACC-A |
| --- | --- | --- |
| 1 | Area tecnica assigns Contributor | L+O |
| 2 | Add a direct Viewer grant to Elena | L+O: Viewer does not cancel the group's Contributor |
| 3 | Revoke only the direct grant | L+O: the group path remains |
| 4 | Also add Elena to a second group with Contributor on ACC-A | L+O through two paths |
| 5 | Revoke ACC-A from Area tecnica | L+O: the second group remains |
| 6 | Revoke the second group too | —, if no other grants or shared property exist |

The app has no “deny” role taking precedence over other grants. A custom role with **View only** is equivalent to Viewer for this capability; a role with **View and log your own hours** can enable writing.

Changing a custom role affects every assignment using it. Changing it from writing to read-only can remove logging capability, but only for users without another valid writing path. Predefined roles cannot be edited.

## 4. A single grant must cover both dates

For this example, the server considers **today = 15 October 2026**, and the user wants to log work from **10 October 2026**. These values describe a case; to repeat it without changing the server clock, use today and a date five days earlier, adjusting intervals.

Start/end dates are inclusive. “Unlimited” means an empty field. The project is not shared and the user is an active Operator.

| Available grant | Can read on 15 October? | Can log 10 October? | Reason |
| --- | --- | --- | --- |
| Contributor from 1 to 31 October | Yes | Yes | The same grant covers both dates |
| Contributor from 15 to 31 October | Yes | No | Covers today, but not the work day |
| Contributor from 1 to 14 October | No | No | Expired today, even though it covered the work |
| Contributor from 16 October without end | No | No | Access has not started yet |
| Viewer from 1 to 31 October | Yes | No | Lacks time-entry capability |
| Contributor without limits | Yes | Yes | Neither endpoint excludes either date |

Writing rights cannot be constructed by combining different periods:

| Grant 1 | Grant 2 | Outcome for 10 October |
| --- | --- | --- |
| Group A, Contributor 1–10 October | Group B, Contributor 15–31 October | Denied: neither covers both today and the work |
| Direct, Viewer 1–31 October | Group B, Contributor 15–31 October | Denied: the grant covering both dates is Viewer only |
| Group A, Contributor 1–10 October | Group B, Contributor 10–31 October | Allowed: the second grant includes both 10 and 15 October |

The same reasoning applies to a future entry. A Contributor valid from 1 to 31 October, on the 15th, can log 20 October but not 1 November. The software allows future dates within authorization limits; it does not interpret them as approval of work already performed.

## 5. Shared projects and project status

Enabling **Shared project** allows every active account to view the project and log personal hours, without a personal grant and without date limits derived from an assignment.

| Configuration for an active Operator | Reading | New hours on a valid date |
| --- | --- | --- |
| Shared, no grant | Yes | Yes |
| Shared, direct Viewer grant | Yes | Yes: shared access permits time entry |
| Shared, expired grant | Yes | Yes: expiration does not cancel the shared property |
| Not shared, sole grant is Viewer | Yes | No |
| Not shared, no grant valid today | No | No |

Disabling the shared property removes only that path. Anyone retaining a direct or group grant continues to have the rights it grants.

**Not started**, **In progress**, **Paused**, **Completed** and **Cancelled** statuses do not change this decision. A Cancelled shared project remains technically usable by active accounts. For an operational closure, the manager must also check access and shared property; there is no separate “close time entry” switch today.

## 6. Revocations, moves and personal history

Suppose Elena has already logged two hours on ACC-A, and her only access comes from Area tecnica through Sviluppo.

| Change | Current access to ACC-A | Elena's previous hours |
| --- | --- | --- |
| Remove Elena from Sviluppo | Revoked, absent other paths | Remain in her log and analyses |
| Move Sviluppo under a parent without the grant | Area tecnica's grant is no longer inherited | Retained |
| Restore membership or parent | Access recalculated with current grants | Retained without duplication |
| Disable only writing through a custom role | Reading may remain | Retained |
| Delete ACC-A | The project no longer exists | All ACC-A hours are deleted, not only Elena's |

Revocation is checked even if the user already opened the form. Saving does not use a permission “frozen” when the page opened. The UI updates choices and can retain date, duration and note while requesting a still-authorized project.

In a personal report, a revoked project can continue to appear because it contains personal hours. That historical presence does not make it available again for new hours. After permanent deletion, those rows no longer remain in reports; administrative History retains events and the deletion summary.

## 7. Deactivation and global profile changes

Account deactivation prevents access and revokes existing sessions, even if the person belongs to groups or the project is shared. Later reactivation requires a new login. Historical hours remain in managers' reports.

Changing Manager to Operator removes administrative powers from the next request and restricts reports to personal history. Valid operational grants are required to continue using restricted projects; having created them as a Manager does not confer permanent rights.

The last active Manager cannot be deactivated or demoted. Before testing a profile change on your own account in the lab, prepare a second active Manager. Testing does not require performing the same operation on the last responsible person in a real workspace.

## 8. Detail access and confidentiality limits

| Resource | Operator | Manager |
| --- | --- | --- |
| Current project list | Authorized and shared only | All |
| Time log | Personal | Personal, with editing/deletion of personal rows |
| Work reports | Personal hours, including after revocation | Aggregates for all users |
| Notes in report responses | Not present | Not present |
| Change history | Inaccessible | Accessible; before/after time details can include others' notes |
| Suppliers, contracts and PDFs | Inaccessible | Accessible |
| Contract value and labor cost | Inaccessible | Accessible |
| Notifications | Personal and about themselves only | Personal notifications, also about other people |
| Calendar via API | Personal connection only | Personal connection only, without extension to other users |

Time log ownership does not guarantee that notes are invisible to administrative managers: History can retain them in snapshots. Put the necessary work description in the note according to organizational rules.

Contract permissions are not inherited from project access. A project Viewer can therefore view its permitted information without being able to open the contract PDF.

## 9. References for verifying the cases

These decisions are consistent with the access engine in `src/organization.c`, time-entry checks in `src/workspace.c` and cases in `tests/test_groups.py`, `tests/test_assignments.py` and `tests/test_job_defaults.py`. To repeat them through the interface, see [acceptance testing](collaudo-accettazione.md). APIs and error codes are in the [app's technical documentation](../tehnical/api-app.md).
