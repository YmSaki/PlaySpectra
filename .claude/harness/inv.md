# Investigation summary: R09 d3d12-msaa — sampleCount>1 を ResolveSubresource で対応
Updated: 2026-07-16T16:35:00Z

## Impact scope
- `layer/src/capture_d3d12.cpp:85-87` — 現行 MSAA 拒否ガード。撤去し resolve 経路へ。
- `layer/src/capture_d3d12.cpp:192-227` — barrier〜copy 区間。MSAA 時は: src を RENDER_TARGET→**RESOLVE_SOURCE** 遷移 → `ResolveSubresource(intermediate, 0, tex, subresource, typedFmt)` → intermediate を RESOLVE_DEST→COPY_SOURCE 遷移 → intermediate から CopyTextureRegion(srcBox) → src を RENDER_TARGET へ戻し、intermediate も RESOLVE_DEST へ戻す(キャッシュ状態を不変に保つ)。全て同一 DIRECT リストに記録可(既存の単一 ExecuteCommandLists+fence 構造は不変)。
- `layer/src/capture_d3d12.cpp:53-57` — `ResolveFootprintFormat`: resolve の typed format 引数にそのまま使える(D3D12 は dxgiFormat 自体が TYPELESS の場合がある — D3D11 と違い guard が TYPELESS を受理する点に注意)。
- `layer/src/capture_d3d12.cpp:61-69` — D3D12Free。中間リソースキャッシュ(ComPtr)の解放追加。
- `layer/build/_deps/openxr_sdk-src/src/tests/hello_xr/graphicsplugin_d3d12.cpp` — hello_xr パッチ対象3点:
  (1) `GetSupportedSwapchainSampleCount` **override 不在**(graphicsplugin.h:38 の既定=recommended を継承、実測1) → env override をクラスへ追加。
  (2) `:118 depthDesc.SampleDesc.Count = 1;` → colorDesc.SampleDesc.Count に追従(anchor 一意。:62 の buffDesc は頂点バッファなので触らない)。
  (3) `:397 pipelineStateDesc.SampleDesc = {1, 0};` → env の N に追従(**PSO の SampleDesc は RT と一致必須** — D3D12 固有の盲点、L63 の同族)。
  ※RTV/DSV の MSAA 次元(:450-472)は **stock で対応済み**(D3D11 と違い追加パッチ不要)。
- `scripts/setup_helloxr_msvc.sh` — 上記3点を既存 python パッチブロックへ追記(マーカー式冪等、既定 env 未設定=挙動不変)。
- `scripts/integration_hello_xr.mjs` / `scripts/integration_test.sh` — **変更不要**(R08 の MSAA アサーションは env ガード式・SKIP はランタイム非依存 grep 式で D3D12 にそのまま効く)。

## Constraints and assumptions
- keyed mutex は **D3D11 固有** — D3D12 に予防的同期を足さない(native-win-interop.md、レビューで false finding 判定済み)。D3D12 の同期は既存の fence 待ちで完結。
- CLAUDE.md: 失敗経路は明示エラー JSON。成功 JSON へ sampleCount/msaaResolved(Vulkan/D3D11 同形)。
- probe 先行(L60): metasim/monado とも D3D12 MSAA スワップチェーン受理は未実測。monado は D3D11 で拒否実績 → D3D12 も拒否なら R08 の SKIP がそのまま発火するはず(その動作確認も probe で取る)。
- ビルド: レイヤーは `cmake --build layer/build --target vr_agent_layer`、hello_xr は setup_helloxr_msvc.sh 再実行(ゾンビが exe をロックしたら mv 退避 → L62)。
- E2E: `HELLO_XR_SAMPLE_COUNT=4 VR_GFX_API=d3d12 HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe bash scripts/integration_test.sh D3D12`。回帰は既定 env の D3D12 17/17 × 両ランタイム(M0 時代の実績コマンド)。

## Assumptions (minor ambiguities — state them and proceed)
- 中間リソースの常在状態は RESOLVE_DEST に統一(使用後に戻す遷移を同一リストに積む)。初回作成時の InitialState も RESOLVE_DEST。
- RasterizerState.MultisampleEnable は触らない(D3D12 では三角形の MSAA ラスタライズは RT の SampleDesc で決まり、当該フラグは線の AA 挙動のみ)。cube 描画は三角形なので不要。

## Open questions (unresolved)
- [ ] metasim が D3D12 の sampleCount=4 スワップチェーンを受理するか(empirical、work 冒頭 probe で解決。拒否なら R08 と同じ SKIP 方針だが、その場合 MSAA 経路の実機検証が両ランタイムで不能になる → その時は plan へ戻り検証戦略を再検討: 明示 SKIP + レビュー重点)。critical だが probe で即断できる。
