# Plan: R08 d3d11-msaa — sampleCount>1 を ResolveSubresource(解決→矩形コピー2段)で対応
Updated: 2026-07-16T15:30:00Z

## Definition of Done
- [x] `cmake --build layer/build --target vr_agent_layer` がクリーンに通る(manifest 同期ログ確認)
- [x] MSAA E2E: metasim=18/18 PASS(`sampleCount:4, msaaResolved:true`、distinctColors=6 非フラット)。monado=明示 SKIP(xrCreateSwapchain→XR_ERROR_VALIDATION_FAILURE 実測、rc=0、理由出力)
- [x] 回帰: 既定(sampleCount=1)の D3D11 統合テスト 17/17 × {metasim, monado}
- [x] 差分が capture_d3d11.cpp / setup_helloxr_msvc.sh / integration_hello_xr.mjs / integration_test.sh(+ハーネス文書)のみ(git diff --stat 実測4ファイル)。D3D12/Vulkan へ予防的同期なし。※integration_test.sh は Step 3 の SKIP 実装で追加(Findings 参照)

## Approach
approach gate: skipped (obvious) — backlog 記載の設計(Vulkan GAP-03 同型の再利用キャッシュ + 「全体解決→矩形コピー」2段)が唯一の妥当解。
1行決定: (1) E2E は hello_xr への env 可変パッチ(`HELLO_XR_SAMPLE_COUNT`、既定1=挙動不変)で実現し、ランタイム拒否時は明示 SKIP。(2) 成功 JSON に `sampleCount`/`msaaResolved` を Vulkan 同形で付加する(既存フィールド不変・R15 の先行断片)。

## Rollback policy
コード変更は WIP コミット単位で `git revert`。third_party/hello_xr_msvc は setup スクリプト再実行で再現(パッチは既定値1で現行挙動不変)。

## Steps
- [x] 1. **hello_xr sampleCount 可変化 + 受理 probe** — target: `scripts/setup_helloxr_msvc.sh`(graphicsplugin_d3d11.cpp への冪等パッチ: `GetSupportedSwapchainSampleCount` を env `HELLO_XR_SAMPLE_COUNT`(既定1)読みに) — done when: 再ビルド後、(a) env 未設定で従来どおり起動(samples=1 のスワップチェーンログ)、(b) `HELLO_XR_SAMPLE_COUNT=4` で layer ログの swapchain 作成行が `samples=4` を示す(受理) or xrCreateSwapchain 失敗が記録される(拒否→SKIP 方針確定)。両ランタイムで probe。
- [x] 2. **capture_d3d11.cpp に MSAA resolve 実装** — target: `layer/src/capture_d3d11.cpp` — 内容: 拒否ガード撤去、`EnsureResolveTexture`(グローバル1枚キャッシュ: desc.Width×Height×Format 一致で再利用、不一致で作り直し、`D3D11Free()` で解放)、MSAA 時は keyed mutex 保持中に `ResolveSubresource(resolveTex, 0, tex, srcSub, desc.Format)` 発行→mutex 解放→resolveTex から矩形 `CopySubresourceRegion`、非 MSAA 経路は不変。成功 JSON に `sampleCount`/`msaaResolved` 付加。失敗経路は明示エラー JSON。 — done when: `cmake --build layer/build --target vr_agent_layer` 成功 + manifest 同期ログ確認。
- [x] 3. **MSAA アサーションをテストへ追加** — target: `scripts/integration_hello_xr.mjs` — 内容: env `HELLO_XR_SAMPLE_COUNT`>1 のとき capture 結果 JSON の `sampleCount`(=env値)/`msaaResolved:true` を PASS 項目に追加(未設定時は従来どおり=項目自体出さない)。 — done when: `HELLO_XR_SAMPLE_COUNT=4 VR_GFX_API=d3d11 HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe bash scripts/integration_test.sh` が両ランタイムで全 PASS(非フラット判定含む)。Step 1 で拒否だったランタイムは明示 SKIP 実装+理由出力。
- [x] 4. **回帰確認** — done when: 既定 env(サンプル数指定なし)で D3D11 統合テスト 17/17 × {metasim, monado}、`git diff --stat` が対象3ファイル+ハーネス文書のみ。→ 実測: 17/17×2、差分4ファイル(SKIP実装分含む)。

Metacognition self-check: pass — コア必須(API完全性)の欠落解消であり CLAUDE.md の north star に直結。設計は実証済みパターン(GAP-03/M0)の移植で、短期ハックなし。E2E 不能時も「無言の素通し」でなく明示 SKIP に落とす設計。

## Resume pack
- **全 Step(1-4)+全 DoD 完了** → h-review へ。
- 実測サマリ(再導出禁止): metasim MSAA=18/18 PASS(sampleCount:4/msaaResolved:true/distinctColors=6)。monado MSAA=明示 SKIP(VALIDATION_FAILURE 実測、rc=0)。回帰 17/17×{metasim,monado}。差分4ファイル(capture_d3d11.cpp +104/-17, integration_hello_xr.mjs +11, integration_test.sh +17/-2, setup_helloxr_msvc.sh +53 — numstat 実測、レビュー指摘で訂正)。
- レビュー対象コミット: step 1-4 の wip 4件(4fd7310〜)。contract=r08-d3d11-msaa(profile=runtime)。
- Metacognition self-check: pass — GAP-03/M0 実証パターンの移植。失敗経路も mutex 解放+明示エラー JSON。SKIP は観測された拒否のみ変換し退行検知を保存。

## Findings (h-work appends unplanned discoveries here)
- hello_xr の D3D11 プラグインは RTV 次元 TEXTURE2D 固定+深度 SampleDesc.Count=1 固定で、MSAA スワップチェーンだと CreateRenderTargetView が E_INVALIDARG。env パッチだけでは不足 → 同一パッチ内で MSAA 条件分岐化(Step 1 の必要拡張として実施)。
- ゾンビ hello_xr.exe(kill 不能、既知)が third_party/hello_xr_msvc/hello_xr.exe の上書きをロック → **実行中 exe のリネームは可能**なので hello_xr.exe.zombie へ退避して新 exe を配置(新回避策)。.zombie はプロセス消滅後に削除可。
- monado の D3D11 コンポジタは MSAA スワップチェーンを受理しない(XR_ERROR_VALIDATION_FAILURE、v25.1.0-646)。Vulkan ベースの compositor への D3D11 MSAA 共有インポート非対応が濃厚。
- SKIP 判定は .mjs では実装不能(アプリが xrCreateSwapchain で即死し client が接続不能) → integration_test.sh 側に実装(対象ファイル1つ追加、DoD 修正済み)。SKIP は「実際に観測された拒否」(HELLO_XR_SAMPLE_COUNT>1 かつログに xrCreateSwapchain+VALIDATION_FAILURE)のみ変換 — 将来ランタイムが受理すればフルアサーションが走る設計(lens 6: 退行検知を殺さない)。
- 制御チャネルは瞬間 LISTEN する(即死前) → SKIP 判定は LISTEN 有無でなく rc!=0 + ログ grep で行う(monado 実測で ok=1→connect timeout→SKIP 変換を確認)。

## Review result (iteration 3)
**Verdict: pass**(h-reviewer 委譲、runtime profile)
- 再実行証跡: ビルド(touch 後の実コンパイル+manifest 同期) / MSAA metasim 18/18(MSAA 行 PASS、graceful PASS) / 回帰 metasim 17/17 / MSAA monado 明示 SKIP rc=0 / 回帰 monado 17/17 / PNG 目視正常 / レイヤログに resolve intermediate 実走行+回帰時は同行なし(非MSAA経路不変の機械確認)。
- 判定点: keyed mutex 規律維持(S_OK 厳密比較、新失敗点の解放順・二重解放なし)。ResolveSubresource 引数合法(実測: metasim テクスチャは TYPELESS(27)+typed 引数29で clean 画像)。SKIP は偽発火経路なし・退行検知保存。setup パッチは stock で4マーカー不在+アンカー assert=冪等/初回適用とも保証。dod_checks 4件充足、non_goals 侵犯なし。
- 非ブロッキング nit: (1) resume pack の numstat 転記ずれ +19/-2→+17/-2(本コミットで修正済み)。(2) EnsureResolveTexture 失敗 JSON に hr なし → R15(error-JSON統一)で拾う(backlog 追記)。
