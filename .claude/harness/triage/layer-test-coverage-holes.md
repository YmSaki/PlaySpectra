---
slug: layer-test-coverage-holes
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 audit findings M04 and M05)
task: none
date: 2026-07-28
---

# Test coverage gaps in layer capture and pixel conversion functions

## Measurement
Two specific test coverage defects confirmed in code:
- `layer/src/capture_common.cpp:67-72` — DominantEyeIndex() function has an untested `PLAYSPECTRA_DOMINANT_EYE=left` branch (line 69) that returns 0; test at `layer/tests/test_capture_common.cpp:123` only exercises the default case (returns 1).
- `layer/src/pixel_convert.cpp:52-57` — LinearToSrgb() power function branch (line 56) is tested only by `layer/tests/test_pixel_convert.cpp:86` with weak assertions (range 0.5 < v < 1.0) that do not verify coefficients or gamma value.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running — no completion criteria to trip |
| N2 | NO | No current task running |
| N3 | NO | No current task running |
| N4 | NO | Not blocking other work right now |
| B1 | NO | No planned work premise is made false by these gaps; the gaps are architectural (not preventing planned continuation) |
| B2 | NO | These are test deficiencies, not unverified design facts; both functions exist and work correctly in production |
| L1 | NO | These are code defects with measurable fixes, not prose only |
| L2 | NO | No workaround is applied; the code paths simply remain untested |
| L3 | NO | Not a different defect population — these are gaps within the layer test suite, same as any future test work would address |
| D1 | YES | Already recorded: `.claude/harness/backlog.md:40-42` records `[P3] M04/M05 test-coverage-holes` with identical scope (DominantEyeIndex env path coverage zero + LinearToSrgb weak assertions) |
| D2 | NO | Not tested; the gaps remain in the current code |

## Why this label
This is an exact duplicate of backlog.md's M04/M05 entry from the same audit date (2026-07-25). The problem was already captured in the project's prioritized backlog when the audit findings were triaged.

## What the caller must do
This record is recorded in `.claude/harness/backlog.md:40-42` under the audit findings section ("Open — 全域監査「無意味なコード」から起票"). No further action needed — the finding is already tracked and prioritized at P3.
