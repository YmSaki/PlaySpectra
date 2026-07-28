---
slug: spec-approval-stale
label: DROP
conditions: [D1]
source: .claude/audit-meaningless-code.csv (2026-07-25, finding M12)
task: none
date: 2026-07-28
---

# M1 spec approval checkbox unchecked despite M2+ shipped

## Measurement
Not run — this is a documentation/process issue with no executable condition.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task is running (h-loop is idle); no N condition can be YES per triage instructions |
| N2 | NO | Not applicable — no current task |
| N3 | NO | Not applicable — documentation only, no mechanism depends on the checkbox |
| N4 | NO | Not applicable — no current task |
| B1 | NO | Next planned work (Monado path real-HMD measurement, G3 SteamVR Adapter) contains no stated premise that M1 approval is a blocker. The risk is confusion/clarity, not a false premise in written planned work. |
| B2 | NO | No unverified capability claimed; problem explicitly states "Documentation/process only; no test or gate in code depends on this checkbox." |
| L1 | YES | Prose documentation only: `.claude/playspectra-device-core-spec.md:254` DoD checkbox has no automated enforcement or consumer. |
| L2 | YES | Workaround already applied: M2.1–M2.4 and downstream milestones (Windows unified stack, real-app E2E, Python MCP) shipped despite unchecked M1 approval. This gate-bypass is documented in `.claude/harness/backlog.md:105–107` (M1 DoD item marked 「未達」for approval, but M2 marked 「Done」). |
| L3 | NO | Not applicable; no current task root-cause population to compare. |
| D1 | YES | Already recorded: `.claude/harness/backlog.md:49` lists this under "[P3] M12/M14/M23 doc-drift" — "M1 spec が「承認待ち」のまま M2 完了済み" (exact same problem from same 2026-07-25 audit). |
| D2 | NO | Not run (documentation-only issue with no reproducibility check). |

## Why this label
D1 is YES: the problem is already recorded in `.claude/harness/backlog.md:49` as part of the M12/M14/M23 doc-drift group from the same 2026-07-25 audit-meaningless-code verification run. Duplicate recording.

## What the caller must do
Close this as a duplicate of `.claude/harness/backlog.md:49`. If fixing the M1 doc-drift (M12), address all three items in that line (M1 approval checkbox, capture.cpp line reference, playspectra_server.py docstring) together.
