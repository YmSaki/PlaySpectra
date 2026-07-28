---
slug: e2e-loose-assertions
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 whole-tree audit, findings M16/M17/M18/M19/M20/M30/M31; verified by an adversarial verifier)
task: none
date: 2026-07-28
---

# Loose assertions in E2E test suites (integration_hello_xr.mjs, integration_openvr.mjs)

## Measurement
Not run — this is a code-reading audit, not a reproducible command.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; no completion criteria to evaluate. |
| N2 | NO | No current task running. |
| N3 | NO | No current task shipping or modifying this code. |
| N4 | NO | This does not block other work from proceeding right now. |
| B1 | NO | No premise in unwritten planned work. Next work (Monado real-HMD measurement, SteamVR Adapter) is informally stated, not in a contract requiring these assertions. |
| B2 | NO | No explicitly named planned design depends on this code. |
| D1 | YES | Already recorded in `.claude/harness/backlog.md:47-48` as `[P3] M16〜M20 weak-assertions-in-e2e`, with description matching all seven findings: "注入した3軸のうち1軸しか照合しない、`typeof x === "boolean"` だけで値を見ない、`if (path) {...}` に else が無く欠落が素通り、swapchain 情報が無いと寸法チェックが緩む等。" M30/M31 (PLAUSIBLE) are also acknowledged at backlog line 51-52 as part of the M25-M34 PLAUSIBLE batch delegated to "CSV 参照". |
| D2 | N/A | Condition not evaluated (D1 already YES). |
| L1 | N/A | Condition not evaluated (D1 already YES). |
| L2 | N/A | Condition not evaluated (D1 already YES). |
| L3 | N/A | Condition not evaluated (D1 already YES). |

## Why this label
These seven audit findings (M16-M20 CONFIRMED, M30-M31 PLAUSIBLE) are already tracked in the backlog at priority P3, with detailed problem descriptions matching the audit output verbatim. Separate triage would duplicate the existing tracking.

## What the caller must do
No action. This problem is already in `.claude/harness/backlog.md` pending priority review and assignment. The audit CSV provides implementation details for any future work that picks it up.
