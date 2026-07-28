# Investigation summary: unit-tests-registry-capture
Updated: 2026-07-17T10:45:00Z

R19 完了後の再評価結果。

## テスト可能な関数と抽出戦略

### HandTopFromBindingPath (action_registry.cpp:80)
- 純粋文字列ヘルパー、ステートなし、ロックなし
- action_registry.cpp 全体をテストターゲットにリンクすると PathToStr(ランタイム依存)が必要 → リンク不可
- **抽出戦略**: action_registry.h で `inline` 化（R12 xr_math.h と同パターン — 純粋関数の header-only inline）
- テストケース: "/user/hand/left/input/grip/pose" → "/user/hand/left", "/user/hand/right" → "/user/hand/right", "/other/path" → "", "" → ""

### EyeToIndex + DominantEyeIndex (capture.cpp:122-137)
- capture.cpp の anonymous namespace 内 — 外部からテスト不可
- 純粋関数（getenv のみ、グラフィックス API 依存なし）
- **抽出戦略**: capture_common.{h,cpp} に移動（テストターゲットが既に capture_common.cpp をリンク済み）
- テストケース: "left"→0, "right"→1, "dominant"→1(default), viewCount=0→0, idx>=viewCount→clamp

### InferHandTops / AggregateFallback — テスト不可（スコープ外）
- g_actions / 内部状態依存、テストターゲットへのリンクが実質不可能
- スコープ外のまま維持

## Impact scope
- layer/src/action_registry.h — HandTopFromBindingPath を inline 化
- layer/src/action_registry.cpp — HandTopFromBindingPath の定義を削除
- layer/src/capture.cpp — EyeToIndex + DominantEyeIndex を削除（capture_common に移動）
- layer/src/capture_common.h — EyeToIndex + DominantEyeIndex の宣言追加
- layer/src/capture_common.cpp — EyeToIndex + DominantEyeIndex の実装追加
- layer/tests/test_action_helpers.cpp (新規) — HandTopFromBindingPath テスト
- layer/tests/test_capture_common.cpp (既存) — EyeToIndex テスト追加
- layer/CMakeLists.txt — test_action_helpers.cpp 追加

## Constraints
- move-only 抽出 + テスト追加。関数の挙動は不変
- vr_agent_test の既存 64 テストに回帰なし
- capture.cpp の anonymous namespace から関数を出すので、namespace vr_agent に入れる

## Open questions
なし
