# Plan: R10 d3d-hdr — R16G16B16A16_FLOAT の half→sRGB decode を D3D11/D3D12 に追加
Updated: 2026-07-16T17:15:00Z

## Definition of Done
- [x] `cmake --build layer/build --target vr_agent_layer` がクリーンに通る(manifest 同期確認)
- [x] HDR E2E: **全4組合せ(metasim/monado × D3D11/D3D12)で 18/18 PASS**(fmt=10、tonemapped:true、colorConversion Vulkan 同文言、非フラット)。16F 非提供 SKIP 分岐は防御として実装(現ランタイムでは踏めない — 全ランタイムが 16F 提供のため。mjs 内にその旨をコメント記載)
- [x] 回帰: 既定 env D3D11/D3D12 × {metasim, monado} = 17/17 ×4本。MSAA 再確認(metasim D3D11/D3D12) = 18/18 ×2本
- [x] 差分が capture_common.{h,cpp} / capture_d3d11.cpp / capture_d3d12.cpp / setup_helloxr_msvc.sh / integration_hello_xr.mjs(+ハーネス文書)のみ。**capture_vulkan.cpp 差分ゼロ**(git diff で実測確認)

## Approach
approach gate: skipped (obvious) — Vulkan 実証済み decode(HalfToFloat→QuantizeSrgb/QuantizeLinearUnit)を RowPitch 対応の共有関数 `DecodeHdrRowsToSrgb`(capture_common)にして D3D 2バックエンドから呼ぶ。RepackRows は 4byte/px 前提のため流用不可(inv 記載)。JSON は Vulkan と同形・同文言(tonemapped 常設、hdr 時 sourceHdrFormat/colorConversion)。TYPELESS 16bit 族は明示エラー維持(推測 decode 禁止)。

## Rollback policy
WIP コミット単位で `git revert`。hello_xr は setup 再実行で再現(パッチ既定=挙動不変)。

## Steps
- [x] 1. **hello_xr HDR パッチ + 16F enumerate probe** — target: `scripts/setup_helloxr_msvc.sh` — 内容: d3d11/d3d12 両ブロックへ `HELLO_XR_HDR=1` で選好リスト先頭に `DXGI_FORMAT_R16G16B16A16_FLOAT` を差すサブパッチ(マーカー式冪等、未設定=挙動不変。非提供時は既存リストへ自然フォールバック)。 — done when: 再ビルド+デプロイ後、metasim/monado × D3D11/D3D12 で `HELLO_XR_HDR=1` 実行時のレイヤログ `fmt=` を実測記録(10=16F 選択 / 29等=フォールバック)。
- [x] 2. **capture_common に DecodeHdrRowsToSrgb 追加 + D3D11/D3D12 へ HDR 経路実装** — target: `layer/src/capture_common.{h,cpp}`, `capture_d3d11.cpp`, `capture_d3d12.cpp` — 内容: 共有 decode(RowPitch 対応、8byte/texel、RGB=QuantizeSrgb/A=QuantizeLinearUnit)。両バックエンドの guard へ FLOAT16(=10) 追加、Map 後の分岐(hdr→共有 decode / それ以外→RepackRows)、成功 JSON へ tonemapped/colorConversion(+sourceHdrFormat)を Vulkan 同文言で追加(非 hdr 時も colorConversion:"direct 8-bit (no tonemap)" — Vulkan 同形)。MSAA+HDR 複合は既存 resolve がそのまま効く(フォーマット汎化の確認のみ)。 — done when: `cmake --build layer/build --target vr_agent_layer` 成功+manifest 同期。
- [x] 3. **mjs に HDR アサーション+16F 非提供 SKIP** → 4組合せ全てで HDR 行 PASS(SKIP 分岐は防御実装、現ランタイムでは発火せず)。
- [x] 4. **回帰+複合確認** → 17/17×4本、MSAA 18/18×2本、差分5ファイル+ハーネス文書のみ、capture_vulkan.cpp 差分ゼロ。

Metacognition self-check: pass — R03 の共有化意図(pixel_convert)の完遂であり、decode 重複を capture_common に一本化(D3D11/12 で同じループを2度書かない)。Vulkan 不変で回帰面を最小化。16F 非提供の可能性は mjs 側 SKIP で正直に落とす(素通しなし)。

## Resume pack
- **全 Step(1-4)+修正 F1-F3+全 DoD 完了** → 再レビューへ(判定対象は vr_agent_test の green のみ、とレビュアー指定済み)。単体テスト 14/14 PASS。
- **全 Step(1-4)+全 DoD 完了**(初回レビュー時点)。
- 実測サマリ(再導出禁止): HDR E2E 18/18 × 全4組合せ(monado も HDR は受理 — MSAA と対照的)。回帰 17/17×4 + MSAA 18/18×2。capture_vulkan.cpp 差分ゼロ。R10 差分= capture_common.{h,cpp} / capture_d3d11.cpp / capture_d3d12.cpp / setup_helloxr_msvc.sh(+27) / integration_hello_xr.mjs(+20)。
- レビュー対象: R10 の wip 3件(step 1, 2, 3-4)。contract=r10-d3d-hdr(profile=runtime)。
- Metacognition self-check: pass — decode は capture_common に一本化(D3D 2箇所で重複させない=R03 共有意図の完遂)。JSON 文言は Vulkan と一字一句同一(将来の R15 統一を阻害しない)。16F 非提供 SKIP は防御実装でその旨コメント明記(無言素通しなし)。

## Review result (iteration 3)
**Verdict: needs-fix**(h-reviewer 委譲、runtime profile)
- 検証は全て green(ビルド/HDR 18/18/回帰 17/17/PNG 目視/数値等価性/JSON 文言一致/Vulkan 差分ゼロ/numstat 一致)だが、**vr_agent_test がリンク不能**: capture_common.cpp が pixel_convert に依存するようになったのにテストターゲットのソース列に pixel_convert.cpp 未追加(undefined reference)。lens 1(依存宣言の追従)の実例。
- minor: DecodeHdrRowsToSrgb の単体テスト不在(RowPitch パディング1ケース推奨)。nit: capture_d3d11.cpp:279 の「RGBA8/BGRA8 guard」コメントが stale。
- 16F TYPELESS 非受理・setup パッチ・mjs SKIP は問題なしと判定(needs-human 不要)。

## 修正 Steps (review iteration 3)
- [x] F1. layer/CMakeLists.txt の vr_agent_test ソース列へ `src/pixel_convert.cpp` を追加 → ビルド成功。
- [x] F2. test_capture_common.cpp へ DecodeHdrRowsToSrgb の単体テスト追加(sRGB 丸め非依存の端点値 0/1 + RowPitch パディング+sentinel) → **14/14 PASS**(新テスト含む)。
- [x] F3. capture_d3d11.cpp:279 の stale コメント修正 → 「Step 2 format guard(受理フォーマットは全て typed)」表現に更新、レイヤービルド成功。

## Review result (iteration 5 — 再レビュー)
**Verdict: pass**
- 初回レビュアーが「再レビューは vr_agent_test のビルド/実行結果のみで判定可能」と受入基準を事前指定していたため、その基準を main loop が直接検証: `cmake --build layer/build --target vr_agent_test` 成功(pixel_convert.cpp のコンパイルをビルドログで確認) → `vr_agent_test.exe` **14/14 PASS**(新設 DecodeHdrRowsToSrgb_HonorsRowPitchAndEndpoints 含む)。レイヤー本体も再ビルド green。F2/F3 は指摘どおりの追加/修正で新規リスクなし(テスト追加+コメント1行)。

## Findings (h-work appends unplanned discoveries here)
- hello_xr の SelectColorSwapchainFormat は find_first_of(runtimeFormats 順)のため「選好リストへの追加」では16Fを強制できない — env 時の早期 return 方式に変更(plan の「リスト先頭へ差す」から実装方式を修正、意図は同一)。
- metasim は稀に起動直後 xrEnumerateEnvironmentBlendModes が SIZE_INSUFFICIENT で落ちるフレークあり(連続 probe 中に1回観測、再試行で解消)。統合テストは1回実行のためフレーク時は再実行で判断する。
