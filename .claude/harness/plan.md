# Plan: unit-tests-registry-capture — HandTopFromBindingPath + EyeToIndex テスト追加
Updated: 2026-07-17T10:55:00Z

Approach gate: skipped (obvious)

## Definition of Done
- [x] `cmake --build layer/build --target vr_agent_layer` ビルド green
- [x] `cmake --build layer/build --target vr_agent_test` ビルド green
- [x] `layer/build/vr_agent_test` 全テスト PASS — 78/78 (既存64 + 新規14)
- [x] HandTopFromBindingPath のテスト 6件
- [x] EyeToIndex のテスト 7件 + DominantEyeIndex 1件

## Rollback policy
`git revert HEAD`

## Steps
- [x] 1. HandTopFromBindingPath inline 化 (action_registry.h)
- [x] 2. EyeToIndex/DominantEyeIndex 抽出 (capture_common.{h,cpp})
- [x] 3. テスト14件追加 (test_action_helpers.cpp + test_capture_common.cpp)

Metacognition self-check: pass — R12 パターンの inline 化 + 純粋関数の抽出+テスト。

## Resume pack
全 Step + 全 DoD 完了。WIP 1コミット。78/78 PASS。次: h-review。

## Review result (iteration 4)
**Verdict: pass** (h-reviewer 委譲、runtime profile)

78/78 テスト PASS。全5検証項目 green。HandTopFromBindingPath inline が verbatim (ODR なし)。
EyeToIndex 抽出が正しく namespace 解決。テスト網羅性 OK。Lens 1 (CMakeLists 追従) OK。

## Findings
なし
