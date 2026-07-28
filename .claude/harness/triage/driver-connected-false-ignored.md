---
slug: driver-connected-false-ignored
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 audit, finding M07; verified by adversarial verifier)
task: none — no h-loop task running
date: 2026-07-28
---

# Controller disconnection state cannot be set; apply_ctrl ignores connected:false request

## Measurement
Not run — condition is verified in code review of `runtime/monado-playspectra/src/xrt/drivers/playspectra/playspectra_control.c:347-394`.

**Code flow verified:**
- Line 347–348: `if (!u->present) { return; }` — early exit if controller absent
- Line 351–352: `U_ZERO(&st); st.connected = true;` — always sets `connected=true` unconditionally
- Line 393: `playspectra_state_set_ctrl(c->state, hand, &st);` — applies state
- Result: No code path exists to set `st.connected = false`. Downstream code at control.c:269 (branch on `!st.connected`) and playspectra_controller.c:96–99 (OpenXR-visible active=st.connected + zero-clear) are unreachable.

Response always returns `ok:true / applied:true` even when the state is unchanged — a silent failure (applied response, no effect).

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; next work is "measure Monado path + G3 SteamVR Adapter" |
| N2 | NO | No current task running |
| N3 | NO | No current task running |
| N4 | NO | No test exercises controller disconnection (problem text: "切断を試すテストが1件も無い"); existing test suite stays green |
| B1 | NO | Next planned work (Monado measurement + SteamVR Adapter G3) does not depend on controller disconnection being functional |
| B2 | NO | No planned design currently depends on this being verifiable |
| L1 | NO | Live code in playspectra_control.c:347–394, not prose |
| L2 | NO | No workaround applied; the problem is precisely the absence of a code path to set `connected=false` |
| L3 | NO | No current task to compare against |
| D1 | YES | Already recorded in `.claude/harness/backlog.md` lines 29–31 as "[P2] M07 driver-connected-false-ignored" with identical source and finding (playspectra_control.c:347, set_state ignored, response false, cut disconnection scenario) |
| D2 | N/A | Problem already recorded elsewhere; audit verification complete |

## Why this label
The problem is already tracked in the project backlog (backlog.md line 29–31) as the same audit finding M07, with identical code location and problem description. Reporting it again as a triage issue would duplicate an existing record.

## What the caller must do
Resolve from `.claude/harness/backlog.md` line 29–31, not as a new triage entry. If action is needed, address it there in priority order (currently marked [P2]). No new triage record required.
