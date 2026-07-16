# Investigation summary: R10 d3d-hdr — R16G16B16A16_FLOAT の half→sRGB decode を D3D11/D3D12 に追加
Updated: 2026-07-16T17:10:00Z

## Impact scope
- `layer/src/capture_vulkan.cpp:655-680` — **参考実装**: HDR は staging の 8byte/texel を HalfToFloat×4 → RGB=QuantizeSrgb / A=QuantizeLinearUnit で 8-bit RGBA 化。`HalfFloatSelfTest()`(1回限り、:405-425)。結果 JSON(:700-711): `tonemapped`(常設 bool)、hdr 時 `sourceHdrFormat` + `colorConversion`(固定文言)、非 hdr 時 `colorConversion:"direct 8-bit (no tonemap)"`。
- `layer/src/pixel_convert.h` — HalfToFloat/LinearToSrgb/QuantizeSrgb/QuantizeLinearUnit は R03 で共有済み(vr_agent::)。R10 はこれを D3D 側から使う。
- `layer/src/capture_common.{h,cpp}` — 共有 decode の置き場: `DecodeHdrRowsToSrgb(src, rowPitch, w, h)`(RepackRows の HDR 版、RowPitch 対応)を新設し D3D11/D3D12 で共用(Vulkan は staging がタイト詰めで独自ループ既存 — 触らない=回帰ゼロ)。
- `layer/src/capture_d3d11.cpp:41-46, 73-82` — format guard に `DXGI_FORMAT_R16G16B16A16_FLOAT`(=10) を追加。staging は desc.Format のまま(16F でも CopySubresourceRegion/Map は同じ)。Map 後、hdr なら RepackRows でなく DecodeHdrRowsToSrgb(RowPitch 対応必須 — **RepackRows は 4byte/px 前提なので HDR に流用不可**)。MSAA+HDR 複合は R08 の resolve が 16F でもそのまま働く(ResolveSubresource は FLOAT 対応)。成功 JSON へ tonemapped/colorConversion(+sourceHdrFormat) を Vulkan 同形で追加。
- `layer/src/capture_d3d12.cpp:40-57, 88-93` — guard に FLOAT16 追加(**TYPELESS の 16bit 族は追加しない** — UNORM か FLOAT か外形から断定できず、推測 decode は「無言の壊れた画像」class。8bit 族の TYPELESS 受理と非対称になるが明示エラーが正直)。footprint format は ResolveFootprintFormat(16F はそのまま返る)。RowPitch は footprint 由来 — decode は同じ共有関数。texel 8byte 前提の totalBytes は GetCopyableFootprints が自動計算。JSON 同上。
- `layer/build/_deps/openxr_sdk-src/src/tests/hello_xr/graphicsplugin_d3d11.cpp:120-127` / `graphicsplugin_d3d12.cpp:289-296` — 選好リストは 8bit 4種のみ(16F なし) → **E2E には env パッチ第3弾**: `HELLO_XR_HDR=1` で `DXGI_FORMAT_R16G16B16A16_FLOAT` をリスト先頭へ(未設定=挙動不変)。ランタイムが 16F を enumerate しなければ hello_xr は既存リストへフォールバック(SelectColorSwapchainFormat の仕様上、無害に 8bit で走る — この場合 HDR E2E は不能でその旨の SKIP 相当判定が必要)。
- `scripts/setup_helloxr_msvc.sh` — 上記パッチ追記(d3d11/d3d12 両ブロック)。
- `scripts/integration_hello_xr.mjs` — env `HELLO_XR_HDR=1` のとき `shot.tonemapped===true` + `shot.format===10` を追加検証(MSAA 行と同型の env ガード)。**16F 非提供ランタイムでは fmt が 8bit のままなので、その場合は「ランタイムが 16F を提供しない」ことを明示して SKIP 扱いにする設計が要る**(アプリは正常動作するため integration_test.sh の既存 SKIP とは別問題 — mjs 内で shot.format を見て判定)。

## Constraints and assumptions
- CLAUDE.md: HDR「decode 出力」自体はコア必須(フォーマット完全性)。深度とは違い許容表現ではない(R10 は Deferred でユーザー着手承認済み)。
- 観測の正直さ(Vulkan 前例): PNG は常に 8-bit。hdr 時は tonemapped/sourceHdrFormat/colorConversion で変換を明示。文言は Vulkan と一字一句同じにする(R15 の統一を先取りしない範囲で同形)。
- hello_xr は線形値を描く: 16F RTV では sRGB 自動エンコードが効かないため、decode の LinearToSrgb がちょうど正しい(PNG は 8bit 時とほぼ同じ見た目になるはず — 非フラット判定は既存のまま効く)。
- Vulkan バックエンドは触らない(回帰ゼロ)。共有 decode は D3D 2バックエンドのみ。
- ビルド/テスト手順は R08/R09 と同一。probe 先行: metasim/monado が D3D11/D3D12 で 16F を enumerate するか。

## Assumptions (minor ambiguities — state them and proceed)
- HalfFloatSelfTest は capture_vulkan.cpp 内 static のまま(移動は move-only 別件)。D3D 側 decode は同じ HalfToFloat(R03 共有・単体テスト対象)を使うため自己テストの重複配置はしない。
- D3D11 の guard は typed のみ(現状踏襲)。16F の TYPELESS(=9) は D3D11/D3D12 とも明示エラー維持。

## Open questions (unresolved)
- [ ] metasim/monado が 16F カラーフォーマットを enumerate するか(empirical、work 冒頭 probe。両方非提供なら E2E 不能 → mjs の SKIP 設計+レビュー重点で出荷し、その旨を明記)。critical だが probe で即断可。
