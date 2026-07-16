# Plan: R09 d3d12-msaa — sampleCount>1 を RESOLVE 遷移+ResolveSubresource+中間キャッシュで対応
Updated: 2026-07-16T16:40:00Z

## Definition of Done
- [x] `cmake --build layer/build --target vr_agent_layer` がクリーンに通る(manifest 同期ログ確認)
- [x] MSAA E2E: metasim=18/18 PASS(`sampleCount:4, msaaResolved:true`、distinctColors=6 非フラット、graceful PASS)。monado=SKIP 行出力+rc=0 を実測(R08 機構が D3D12 で発火確認)
- [x] 回帰: 既定(sampleCount=1)の D3D12 統合テスト 17/17 × {metasim, monado}
- [x] 差分が capture_d3d12.cpp(+144/-27) / setup_helloxr_msvc.sh(+46)のみ(numstat 実測)。D3D11/Vulkan・テスト2ファイルに変更なし、keyed mutex 追加なし

## Approach
approach gate: skipped (obvious) — backlog 記載設計(RENDER_TARGET→RESOLVE_SOURCE 遷移+ResolveSubresource→単一サンプル中間(RESOLVE_DEST 常在、使用後に戻す)→COPY_SOURCE→既存 readback)が唯一の妥当解。R08 と同じく成功 JSON へ sampleCount/msaaResolved 付加。resolve の typed format は既存 ResolveFootprintFormat を流用(D3D12 は dxgiFormat 自体が TYPELESS の場合がある)。

## Rollback policy
コード変更は WIP コミット単位で `git revert`。hello_xr は setup スクリプト再実行で再現(パッチ既定値=挙動不変)。

## Steps
- [x] 1. **hello_xr D3D12 パッチ + 受理 probe** — target: `scripts/setup_helloxr_msvc.sh` — 内容: 既存 python ブロックへ D3D12 の3サブパッチ追記((1) GetSupportedSwapchainSampleCount env override 追加(クラス内、d3d11 と同式)、(2) depthDesc.SampleDesc.Count=1 → colorDesc 追従、(3) PSO SampleDesc={1,0} → env N。RTV/DSV 次元は stock 対応済みで触らない)。 — done when: 再ビルド+デプロイ後、`HELLO_XR_SAMPLE_COUNT=4` で metasim/monado の受理/拒否をレイヤログ(samples=4)+hello_xr ログで実測記録。**両方拒否なら plan へ戻る**(検証戦略再検討)。
- [x] 2. **capture_d3d12.cpp に MSAA resolve 実装** — target: `layer/src/capture_d3d12.cpp` — 内容: 拒否ガード撤去、中間リソースキャッシュ(ComPtr グローバル+w/h/fmt キー、RESOLVE_DEST 常在、D3D12Free で解放)、MSAA 時のリスト記録を「src→RESOLVE_SOURCE / Resolve / 中間→COPY_SOURCE / 中間から CopyTextureRegion / src→RENDER_TARGET / 中間→RESOLVE_DEST」へ分岐(非 MSAA 経路・fence 構造は不変)、成功 JSON へ sampleCount/msaaResolved。失敗経路は既存 fail ヘルパー(hr 併記)。 — done when: `cmake --build layer/build --target vr_agent_layer` 成功+manifest 同期ログ。
- [x] 3. **MSAA E2E + SKIP 動作確認** → metasim 18/18 PASS、monado SKIP 行+rc=0 実測。
- [x] 4. **回帰確認** → 17/17 × {metasim, monado}、numstat=対象2ファイルのみ。

Metacognition self-check: pass — R08 で実証した設計・検証枠組みの D3D12 移植で、確立パターンの適用。probe 先行で「両ランタイム拒否」の場合に plan へ戻る脱出条件を明示(検証不能のまま出荷しない)。

## Resume pack
- **全 Step(1-4)+全 DoD 完了** → h-review へ。
- 実測サマリ(再導出禁止): probe=metasim 受理/monado 拒否(VALIDATION_FAILURE)。metasim MSAA 18/18(sampleCount:4/msaaResolved:true)。monado SKIP 行+rc=0(R08 機構が D3D12 で発火)。回帰 17/17×{metasim,monado}。R09 差分= capture_d3d12.cpp(+144/-27)+setup_helloxr_msvc.sh(+46)。
- レビュー対象: R09 の wip 2件(step 1, step 2)+plan 文書。contract=r09-d3d12-msaa(profile=runtime)。
- Metacognition self-check: pass — R08 実証設計の同型移植。中間リソースの常在状態(RESOLVE_DEST)を遷移で復元しキャッシュ前提を保存。D3D12 に keyed mutex 等の予防的同期は足していない。

## Findings (h-work appends unplanned discoveries here)
(なし — probe 想定どおり、plan からの逸脱なし)

## Review result (iteration 3)
**Verdict: pass**(h-reviewer 委譲、runtime profile、findings なし)
- 再実行証跡: 実コンパイル+manifest 同期 / MSAA metasim 18/18×2回(sampleCount:4/msaaResolved:true、非フラット) / 回帰 metasim 17/17 / monado SKIP 行+rc=0 実測 / PNG 目視正常 / numstat 完全一致(+144/-27, +46) / 予防的同期の混入なし(grep) / パッチ3点の適用実体確認。
- 状態遷移: 全経路で対称性+キャッシュ常在(RESOLVE_DEST)不変条件成立(EnsureResolveResource 失敗は barrier 記録前=安全、fence タイムアウトでも復元バリアは同一 submit 内)。
- 参考メモ(対応不要判定): D3D12 env 既定のハード1は D3D11 stock と整合・テスト決定性で妥当。hr の 0x+10進表記はファイル既存慣習(R15 系の別件)。
