---
slug: probe-return-values-discarded
label: LATER
conditions: [L2]
source: .claude/audit-meaningless-code.csv (2026-07-25 whole-tree audit, M10/M11/M24)
task: none
date: 2026-07-25
---

# Weak return-value assertions in three probes discard failure signals

## Measurement

Not run (triage of audit findings; see source file for verification details).

## Conditions

| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running — classified against B and L only per instructions |
| N2 | NO | No current task |
| N3 | NO | No current task |
| N4 | NO | No current task |
| B1 | NO | `.claude/playspectra-m2-status.md:193` cites these probes as "入力E2E検証済み" (input E2E verified), but no named future task contract explicitly depends on this premise being true. Next planned work (real HMD testing, G3 SteamVR Adapter) is described informally and would rely on independent verification methods. |
| B2 | NO | The problem is not unverified design but weak assertions in existing code. |
| L1 | NO | All three are executable code (C probes, Python script), not prose-only. |
| L2 | YES | M24 (tools/playspectra_headless_probe.c): workaround already applied — `tools/playspectra_coupling_probe.py` provides the load-bearing gate (asserts actual pose values), and is wired into `scripts/run_hello_xr_monado.sh:52-56` with rc folded into run verdict. M24 is documented as redundant manual diagnostic. M10/M11 have no workaround (no script gates on their rc; M11 is manual E2E only, not in run_all_tests.sh). |
| L3 | NO | No current task to compare defect population against. |
| D1 | NO | No prior triage record cited. |
| D2 | NO | Audit findings are already verified; no reproduction step needed. |

## Why this label

M24 has a workaround (coupling_probe is the real gate), tripping L2. M10 and M11 lack workarounds but are not NOW (no active task) or BEFORE-NEXT (no named planned work depends on them), so they default to LATER. The entire problem set is not blocking current work.

## What the caller must do

Backlog entry in `.claude/harness/backlog.md` under "Input verification / probe gateway" or similar. M24 is a cleanup task (document the workaround, optionally delete redundant probe). M10 and M11 are genuine bugs (weak exit codes) but low-priority until the MCP wrapper or action probe is wired into a gate or used for actual integration testing (e.g., real HMD runs, SteamVR Adapter testing).
