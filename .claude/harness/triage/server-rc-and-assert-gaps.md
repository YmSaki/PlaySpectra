---
slug: server-rc-and-assert-gaps
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 whole-tree audit, findings M22 and M33 listed)
task: none
date: 2026-07-28
---

# Two gaps in playspectra_server.py: rc judgment and assertion validation

## Measurement

**M22 verification — rc not gating capture**: Line 606 of `tools/playspectra_server.py`:
```python
return 1 if (cmd in ("assert", "wait_for", "assert_capture") and result is False) else 0
```
Confirmed: tuple does NOT include `"capture"`, so `--cmd capture` failing returns rc=0 instead of non-zero.

**M33 verification — eq/ne without None guard**: Lines 364-365 of `tools/playspectra_server.py`:
```python
if op == "eq":    return actual == value
if op == "ne":    return actual != value
```
Confirmed: these operations lack the `actual is not None` guard present on lines 363, 366, 367. If path typo causes `_resolve→None` AND value is omitted (=None), then `None == None` → True (PASS).

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; cannot block a task's completion criteria |
| N2 | NO | No current task |
| N3 | NO | No current task |
| N4 | NO | No current task |
| B1 | NO | Next planned work (Monado real-HMD measurement + G3 SteamVR Adapter) does not depend on these specific server.py rc/assertion details. tools/README.md contracts exist but not actively exercised in current plans |
| B2 | NO | Both issues are existing code with measured behaviour, not unverified design claims |
| L1 | NO | Both are code defects, not documentation-only |
| L2 | NO | No applied workaround; M22 unused in repo (no `--cmd capture` calls), M33 avoided by scenario constraints (only use "near"/"true") |
| L3 | NO | Both found in whole-tree audit, not part of a specific task's defect population |
| D1 | YES | M22 already recorded at `.claude/harness/backlog.md:45` as `[P3] M22 capture-rc-not-gated`; M33 referenced at backlog.md:52 within "PLAUSIBLE 10件(M25〜M34)は CSV 参照" and recorded in problem source as audit findings |

## Why this label

M22 has a dedicated entry in backlog.md. M33, though not yet with its own backlog entry, is already documented in the audit source (the problem input file itself cites it as verified finding). Both conditions and their fixes have been captured in the audit/backlog system.

## What the caller must do

Both findings are already in the backlog/audit ledger. If work is planned to address audit findings, pick these up from backlog.md in priority order (M22 is P3; M33 part of P3 batch). No new tracking file needed.
