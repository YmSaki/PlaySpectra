# Investigation summary: R08 d3d11-msaa — sampleCount>1 を ResolveSubresource で対応
Updated: 2026-07-16T15:25:00Z

## Impact scope
- `layer/src/capture_d3d11.cpp:62-70` — 現行の MSAA 拒否ガード（Step 1）。ここを「拒否」から「解決→矩形コピー」へ置換。
- `layer/src/capture_d3d11.cpp:180-221` — keyed mutex 取得〜コピー〜解放ブロック。MSAA 時は mutex 保持中に ResolveSubresource（共有テクスチャ読み）を発行し、解決先(プロセスローカル)からの矩形コピーは mutex 解放後で可。
- `layer/src/capture_d3d11.cpp:50-54` — `D3D11Free()`。解決先テクスチャキャッシュの解放を追加。
- `layer/src/capture_vulkan.cpp:277-345` — 参考パターン GAP-03 `EnsureResolveImage`（w/h/format 不一致で再作成する再利用キャッシュ。D3D11 版は「サブリソース全体解決」の制約上 **desc.Width×desc.Height（テクスチャ全体寸法）× desc.Format** でキャッシュ）。
- `layer/src/capture_vulkan.cpp:701-702` — Vulkan 成功 JSON は `sampleCount`/`msaaResolved` を付加。D3D11 も同形フィールドを付けるか（R15 統一の先行断片）は plan で1行決める。
- `layer/build/_deps/openxr_sdk-src/src/tests/hello_xr/graphicsplugin_d3d11.cpp:250` — `GetSupportedSwapchainSampleCount` が **1 固定** → 既定 E2E では MSAA 経路を踏まない。
- `scripts/setup_helloxr_msvc.sh:20-27` — 既存の chrono パッチ実績。同じ流儀で sampleCount を env 可変化するパッチを足せる（graphicsplugin_d3d12.cpp も同型 → R09 でも再利用可）。

## Constraints and assumptions
- **ResolveSubresource の仕様**: サブリソース全体を解決（矩形指定不可）→「①全体解決 → ②矩形 CopySubresourceRegion」の2段構成が必須（backlog 記載どおり）。解決先は同一フォーマット・single-sample・USAGE_DEFAULT。
- **keyed mutex 規律** (`.claude/rules/native-win-interop.md`): AcquireSync は `hr == S_OK` 厳密比較。共有テクスチャを読む GPU コマンド（Resolve/Copy）は mutex 保持中に発行。取得失敗した mutex に ReleaseSync しない。D3D12/Vulkan へ同種同期を予防的に足すのは誤り。
- **CLAUDE.md**: 失敗経路は明示エラー JSON、無言の壊れた画像は禁止。D3D11/D3D12/Vulkan はコア必須（今回はその MSAA 欠落の解消）。
- **検証規約** (`.claude/rules/setup-scripts.md` 4): 成立判定は機械値。検証不能項目は明示 SKIP、rc は FAIL のみで決める。
- テスト経路: `HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe` + `VR_GFX_API=d3d11` で metasim/monado 両ランタイム E2E 可（integration_test.sh / integration_hello_xr.mjs）。
- ビルド: `cmake --build layer/build --target vr_agent_layer`（ゾンビ hello_xr.exe の write-lock 回避）。実行 DLL は layer/manifest/ 同期コピー。

## Assumptions (minor ambiguities — state them and proceed)
- MSAA スワップチェーン画像が keyed mutex 共有か否かは実測未詳（D3D11 の共有 MSAA テクスチャは通常不可のため「mutex なし」の可能性が高い）。現行コードは MiscFlags 条件分岐なので**どちらでも正しく動く**設計を維持すれば前提を置く必要なし。
- 解決先キャッシュはグローバル1枚（Vulkan GAP-03 と同型）。両眼で寸法/フォーマットが同一なら再利用され、異なれば作り直し（正しさ優先、性能は十分）。

## Open questions (unresolved)
- [ ] **E2E 検証戦略**（criticalだが empirical に解ける — plan で決めて work 冒頭で probe）: hello_xr は sampleCount=1 固定のため、MSAA 経路の実機検証には (a) setup_helloxr_msvc.sh に env 可変パッチ（`HELLO_XR_SAMPLE_COUNT`、既定1で現行挙動不変）を追加し、ランタイムが sampleCount>1 の D3D11 スワップチェーンを受理するか実測する、のが推奨。ランタイム側が拒否した場合は (b) 明示 SKIP + 単体レベル検証（WARP デバイスでの resolve 経路テスト）へフォールバック。
- [ ] 成功 JSON に `sampleCount`/`msaaResolved` を付加するか（Vulkan 同形）。→ plan で1行決定（付加推奨: R15 の先行断片として自然、既存フィールドは不変なので後方互換）。
