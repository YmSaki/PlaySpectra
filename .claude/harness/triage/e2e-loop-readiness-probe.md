---
slug: e2e-loop-readiness-probe
label: LATER
conditions: []
source: `.claude/audit-meaningless-code.csv` (2026-07-25 audit, finding M29, verdict PLAUSIBLE; verified by adversarial audit)
task: none — no h-loop task running; next planned work is informal (measure Monado with real HMD, then G3 SteamVR Adapter) with no written contract yet
date: 2026-07-28
---

# Control-channel readiness probe exit status is discarded, confusing diagnostics on timeout

## Measurement
Confirmed by code review: scripts/e2e_playwright_loop.sh lines 48–55 run `python3 - <<'PY' ... sys.exit(0|1) PY` to poll :52702 readiness and print CTRL_UP/CTRL_TIMEOUT, but line 56 immediately tests app liveness without capturing the heredoc's exit status. No `RC=$?`, no `||`, no enclosing `if`. Script runs `set -u` only; no `-e` or `pipefail`.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; cannot block completion criteria of a nonexistent task |
| N2 | NO | No current task; no enforcement mechanism associated with it |
| N3 | NO | No current task; script failure is not silent (hard-gated by PNG-hash assertion at lines 74–91) |
| N4 | NO | Script does not block other work: it fails loudly via hash mismatch when channel is down (audit text: "the terminal assertion ... is a hard gate, and with the channel down playspectra_server.py cannot change the pose, so frames stay identical and the run fails") |
| B1 | NO | Next planned work (measure Monado readiness; G3 SteamVR Adapter) is informal only — no written plan, contract, or backlog entry yet. No named premise going false. |
| B2 | NO | No unverified design premise in written planned work (none exists) |
| L1 | NO | Mechanisms exist: this is executable code, not prose-only |
| L2 | NO | No applied workaround. The `kill -0 "$APP"` check at line 56 is a separate liveness test (and itself broken per audit: APP=$! captures timeout's PID, not hello_xr's), not a workaround for the readiness probe |
| L3 | NO | No current task to define a defect population; issue is isolated to this script's diagnostics layer |
| D1 | NO | M29 exists in audit file as PLAUSIBLE but is not recorded in backlog.md or any triage file yet |
| D2 | NO | Problem still reproduces in current code at lines 48–56 |

## Why this label
No condition is tripped. The issue is genuine (11s+ diagnosability cost, misattributed failure message on CTRL_TIMEOUT) but non-blocking: the script's verdict is hard-gated by an independent PNG-hash assertion, so a timeout is caught, just not at the readiness point. No written planned work yet depends on fixing this to proceed.

## What the caller must do
Add to `.claude/harness/backlog.md` under Open or Done section as a P3 (diagnosability, safe to defer): capture the heredoc rc and bail with explicit "control channel timeout" message on nonzero, mirroring the APP_DIED check. While there, audit notes a secondary issue: line 56's liveness check is also broken (APP=$! captures timeout's PID, not app's), but that is a separate find—see verify_note in audit.
