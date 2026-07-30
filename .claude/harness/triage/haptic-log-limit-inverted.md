---
slug: haptic-log-limit-inverted
label: LATER
conditions: [L2]
source: audit finding M15 from `.claude/audit-meaningless-code.csv` (2026-07-25)
task: none
date: 2026-07-28
---

# Haptic log limit condition inverted — limit ≤ 0 returns all entries instead of none

## Measurement
Not run — condition is in layer/src/layer_state.cpp:149, verified in code review.

Line 149: `if (limit > 0 && g_haptic_log.size() > static_cast<size_t>(limit))`
When `limit ≤ 0`, the guard evaluates false, `start` remains 0, loop returns all entries (up to 64) instead of none or empty.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running |
| N2 | NO | No current task running |
| N3 | NO | No current task running |
| N4 | NO | No current task running |
| B1 | NO | Next planned work (Monado measurement + G3 SteamVR Adapter) does not depend on precise haptic log limit semantics; impact summary says "現状ほぼゼロ" |
| B2 | NO | No planned design depends on this unverified behaviour |
| L1 | NO | It is live code in layer_state.cpp:149, not prose |
| L2 | YES | Workaround applied: all callers intentionally pass positive values. observe.ts default=20, scripts/integration_hello_xr.mjs:207,210 limit=200. Problem text confirms "非正値を渡す経路は無く" — no code path passes ≤0 |
| L3 | NO | No current task to compare against |

## Why this label
A workaround is in place via caller discipline: all code paths pass positive values (20 or 200), never ≤0, so the inverted semantics are never exercised. The boundary case itself is undefended (no test covers it), but the impact is zero in current use. Backlog item.

## What the caller must do
Add to backlog.md: either add guard `if (limit <= 0) return out;` at function start (making limit=0 → empty array, matching documented semantics), or document that non-positive values mean "all entries" in both layer_state.h:105 and observe.ts. No fix required before G3 SteamVR Adapter work.
