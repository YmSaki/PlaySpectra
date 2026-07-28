---
slug: swapchain-array-unvalidated
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 audit, findings M02 and M03; verified by adversarial verifier)
task: none
date: 2026-07-28
---

# Swapchain array index validation missing in capture backends

## Measurement
Not run; audit source is static analysis verified by independent review.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; no task completion criteria to evaluate |
| N2 | NO | No current task shipping/modifying the capture mechanism |
| N3 | NO | No current task using capture for its own verification |
| N4 | NO | No current work blocked; next work (Monado+HMD measurement, G3 SteamVR) is planned but not yet active |
| B1 | NO | No written premise in a named plan entry that this bug would invalidate. Next work not yet detailed. |
| B2 | NO | No design document yet exists for next work that describes what capture must verify. |
| L1 | NO | Problem is executable code (capture.cpp, capture_d3d11.cpp), not documentation. |
| L2 | NO | No workaround applied. D3D12 has the check (159-162) but other backends lack it. |
| L3 | NO | No current task to compare defect population against. |
| D1 | YES | Already recorded in `.claude/harness/backlog.md` line 32-34 as "[P2] M02/M03 swapchain-arrayindex-unchecked" with identical scope: capture.cpp:293 stores arraySize unvalidated; capture_d3d11.cpp:178 bounds check incomplete. |
| D2 | — | Not evaluated; D1 is YES |

## Why this label
Finding M02 and M03 from the 2026-07-25 audit are already tracked in the active backlog as a single issue entry `[P2] M02/M03 swapchain-arrayindex-unchecked` with complete description of both code sites and the fix. Duplicate tracking would fragment the single logical defect.

## What the caller must do
This triage record documents that the audit finding is a duplicate of existing backlog entry `.claude/harness/backlog.md:32-34`. No new file needed; work continues against the backlog entry.
