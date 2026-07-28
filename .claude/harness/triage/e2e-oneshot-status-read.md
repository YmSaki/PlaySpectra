---
slug: e2e-oneshot-status-read
label: DROP
conditions: [D1]
source: .claude/audit-meaningless-code.csv (2026-07-25 audit, findings M01 and M09)
task: none — no h-loop task running
date: 2026-07-28
---

# E2E integration tests do one-shot status/actions read with no retry, causing false-red flakes

## Measurement
Not run; problem already verified in backlog at `.claude/harness/backlog.md` lines 23–28 with evidence of reproduction: M01 observed as 6 FAILs + rc=2 on actual test run (2026-07-25), clears to 20/20 on re-run.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | N/A | No h-loop task running; no current task to block |
| N2 | N/A | No h-loop task running; no current task to block |
| N3 | N/A | No h-loop task running; no current task to block |
| N4 | N/A | No h-loop task running; no current task to block |
| B1 | NO | Next planned work (measure Monado + G3 SteamVR Adapter) does not depend on E2E test premises being false; flakiness is a known issue already in backlog |
| B2 | NO | No unverified design premise |
| L1 | NO | Mechanisms exist and are recorded (the tests themselves + backlog entry) |
| L2 | NO | No applied workaround recorded |
| L3 | NO | Same defect class as current audit population |
| D1 | YES | `.claude/harness/backlog.md` lines 23–28 record both findings: "[P1] M01 hello-xr-e2e-status-race" and "[P2] M09 openvr-e2e-status-race" with identical scope (one-shot reads, no retry, false-red on slow app init, bounded retry fix, cite same files and line numbers) |
| D2 | N/A | Problem already recorded; no need to verify reproduction again |

## Why this label
Both M01 and M09 are already tracked in the backlog as duplicate entries sourced from the same audit file and time (2026-07-25). No new information is added by reporting them again.

## What the caller must do
No action required; resolve from backlog.md, not as a new triage issue. Both entries are already assigned (P1 for M01, P2 for M09) and have fix guidance specified.
