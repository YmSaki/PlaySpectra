---
slug: harness-fixed-sleep-readiness
label: LATER
conditions: [L2]
source: .claude/audit-meaningless-code.csv (findings M25 and M26; 2026-07-25 whole-tree audit, verified by adversarial verifier)
task: none
date: 2026-07-28
---

# Fixed sleep times as race-condition workarounds in readiness polls

## Measurement
Not run: The race condition is observable in code without reproduction. The `taskkill //F` → fixed sleep → netstat poll pattern in both scripts creates a window where the poll can latch onto a dying process's socket instead of the new one, potentially causing false "ready" signals and skipping the 30-second readiness budget. The fixed sleeps (5s and 6s) mitigate but do not eliminate this hazard. Proper fix is documented and partially implemented in lib_monado_stack.sh:57-62 but missing from the two affected scripts.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; no completion criteria to invalidate |
| N2 | NO | No enforcement mechanism involved; workaround sleeps are applied |
| N3 | NO | Not a silent failure; race condition is time-sensitive but detection would be nondeterministic anyway |
| N4 | NO | Workaround is in place; tests can proceed; no immediate block |
| B1 | NO | Next planned work (Monado path measurement) is informal; no formal contract with explicit premises yet written. B1 requires naming a plan entry and quoting a premise it would invalidate — neither exists. |
| B2 | NO | No unverified capability; this is a defect in test infrastructure, not a capability gap |
| L1 | NO | This is shell script code affecting test execution, not prose |
| L2 | YES | Workarounds are applied: `sleep 6` at scripts/integration_test.sh:70 and `sleep 5` at scripts/integration_openvr_test.sh:43 mitigate the race by providing extra time before the control-channel client connects. These sleeps are holding and prevent immediate failure in normal operation. |
| L3 | NO | No current task; this is test-infrastructure defect, not same-population remainder of any task |
| D1 | NO | Not recorded in triage directory; audit CSV (source) is the origin of this finding |
| D2 | NO | Defect is verifiable by code inspection; race window exists whether or not a specific run reproduced it |

## Why this label
L2 condition is met: the race condition is mitigated by fixed-sleep workarounds already applied at the two affected script locations. The proper fix (reuse lib_monado_stack.sh's wait-for-port-free loop, documented in audit M25/M26) belongs on a backlog for future refactoring to improve test reliability without urgency.

## What the caller must do
This belongs in `.claude/harness/backlog.md` under a test-infrastructure section (or LATER section if one exists), with the audit findings M25 and M26 as reference. The proper fix is spelled out in the audit CSV: port scripts/integration_test.sh:40-64 and integration_openvr_test.sh:25-44 to wait for 52700/52702 to actually free (per lib_monado_stack.sh:57-62) before launching the new service/app, and add readiness checks for monado-service instead of flat sleeps (per lib_monado_stack.sh:65).
