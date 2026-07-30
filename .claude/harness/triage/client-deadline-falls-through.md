---
slug: client-deadline-falls-through
label: LATER
conditions: []
source: .claude/audit-meaningless-code.csv (2026-07-25 audit, M32, verdict PLAUSIBLE)
task: none
date: 2026-07-28
---

# vrdevapp_client.mjs SESSION_WAIT_MS readiness loop exits silently on timeout

## Measurement
Verified at scripts/vrdevapp_client.mjs:69-78. The `while (Date.now() < deadline)` loop polls for `st.session && frames > 0` but falls through without error on deadline expiry. Lines 79-82 execute unconditionally (status log, view, screenshot), and line 111's main().catch() only catches exceptions, not silent timeouts. `run_vrdevapp.sh:48-49` propagates the exit code from the mjs script, so a timeout yields exit 0 (success) and is indistinguishable from readiness achieved.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task — cannot block task's own completion criteria. |
| N2 | NO | No enforcement mechanism to test; readiness check is purely informational. |
| N3 | NO | Silent failure occurs in a manual discovery tool, not an automated verification step the task ships/modifies/uses. |
| N4 | NO | No blocking of commits/loops/suite runs — the script is not part of run_all_tests.sh (verified: grep returns 0 matches). |
| B1 | NO | Next planned work (Monado measurement + G3 SteamVR Adapter) does not depend on vrdevapp_client.mjs; README:71 classifies VRDevApp as 🟡 manually verified, outside automation. |
| B2 | NO | No planned work's design depends on this readiness-check capability. Manual testing does not require automation-level verification. |
| L1 | NO | This is executable code (JavaScript), not prose/documentation. |
| L2 | NO | No workaround is applied; the code falls through unconditionally and calls process.exit(1) only on exceptions, not on timeout. |
| L3 | NO | No current task exists; L3 condition does not apply (scoped to current task's defect population). |
| D1 | NO | Not yet recorded in backlog.md; the audit CSV reference (M32) is the source of this triage input, not a pre-existing backlog entry. |
| D2 | NO | Defect clearly reproduces — code at lines 69-78 exhibits the exact pattern: deadline loop with no post-timeout check. |

## Why this label
No condition is tripped. The problem is a real defect in manual-verification code with genuine impact (silent timeout concealment), but it does not block any planned task, does not make any active premise false, and has no applied workaround. It belongs in the backlog as nice-to-have hardening for the discovery tool.

## What the caller must do
Add to `.claude/harness/backlog.md` under "Open — full-audit findings (manual/low-priority)" or similar section. Fix is: after line 78, add `if (!(st.session && ((st.capture&&st.capture.framesObserved)||0) > 0)) { console.error('[vrdev] readiness timeout', JSON.stringify(st)); process.exit(1); }` so `run_vrdevapp.sh` distinguishes timeout from success.
