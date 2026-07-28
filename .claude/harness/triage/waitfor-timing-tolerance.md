---
slug: waitfor-timing-tolerance
label: LATER
conditions: []
source: .claude/audit-meaningless-code.csv (2026-07-25 whole-tree audit, finding M34; verified by adversarial verifier, verdict PLAUSIBLE)
task: none
date: 2026-07-28
---

# Timing tolerance band in wait_for/assert_capture tests is too loose

## Measurement

Verified by reading test files:

- `tools/playspectra_waitfor_test.py:147` — `check("timeout deadline honoured (250-1500ms)", 250 <= el <= 1500, "elapsed=%dms" % el)`
- `tools/playspectra_capture_assert_test.py:123` — `check("assert_capture retry times out to FAIL (no hang)", r is False and 250 <= el <= 1500, "rc=%s el=%dms" % (r, el))`

Both tests configure `timeout_ms=350` but accept elapsed times up to 1500ms (4.3x the configured timeout).

Actual implementation (playspectra_server.py:374-386, `_poll_until`) re-reads `time.monotonic()` on each iteration after `probe()`, so true expected elapsed is ~350-400ms. The upper bound leaves ~1.1s of unmonitored slack.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NOT APPLICABLE | No current task running; no N condition can be YES |
| N2 | NOT APPLICABLE | No current task |
| N3 | NOT APPLICABLE | No current task |
| N4 | NOT APPLICABLE | No current task |
| B1 | NO | No named planned work depends on a premise this makes false; next work is measuring Monado with real HMD and G3 (SteamVR Adapter), neither of which specifies requirements on timing assertion tightness |
| B2 | NO | No planned design depends on verifying tight timing assertions; planned work does not require this unverified capability |
| L1 | NO | These are executable Python test files with assertions, not prose |
| L2 | NO | No workaround is applied; the loose band is the only implementation of this check |
| L3 | NO | No current task; L3 only applies to remainders from an active task's defect population |
| D1 | NO | Not recorded in prior triage; audit CSV is the source input to this triage, not a separate triage record |
| D2 | NO | Problem reproduces; assertions exist in code at cited lines and enforce the loose bounds (250-1500ms) |

## Why this label

No condition tripped. The problem is real (loose tolerance), verifiable, and bounded to test code with low regression-detection risk (a 2-4x overshoot or poll-interval inflation would pass). It is appropriate for the backlog because it is a test-quality issue, not blocking current or planned work.

## What the caller must do

Add to `.claude/harness/backlog.md` under "Open — 全域監査「無意味なコード」から起票" section if not already listed, or verify it is captured as M34 in the source audit and add a reference. The fix itself (tighten tolerance or make it relative to configured values) is low priority.
