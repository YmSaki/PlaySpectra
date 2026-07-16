# Plan: R17 d3d11-typeless — D3D11 で 8bit TYPELESS スワップチェーンを受理
Updated: 2026-07-16T18:20:00Z

## Definition of Done
- [x] `cmake --build layer/build --target vr_agent_layer` クリーン + `vr_agent_test` **14/14 green**
- [x] TYPELESS E2E: 両ランタイムとも非列挙(probe 実測) → mjs が明示 SKIP 行出力+rc=0+他 17/17(両ランタイムで実測)。列挙ランタイム向けアサーションは実装済み(防御)
- [x] 回帰: 既定 env D3D11 17/17 × {metasim, monado} + MSAA(metasim) 18/18 + HDR(metasim) 18/18
- [x] 差分が capture_d3d11.cpp(+23/-9) / setup_helloxr_msvc.sh(+19) / integration_hello_xr.mjs(+16)のみ(numstat 実測)。D3D12/Vulkan/capture_common 差分なし

## Approach
approach gate: skipped (obvious) — D3D12 バックエンドの既存規則(8bit TYPELESS 受理+同族 UNORM 写像)を D3D11 に揃えるだけ。R16 未実施のため写像関数(ResolveTypedFormat)は capture_d3d12 の ResolveFootprintFormat と一時重複(数行、R16 で統合予定 — ユーザー承認順を崩してまで R16 を先行させない)。非自明点は1つ: MSAA resolve の format 引数が typed 必須のため、`static_cast<DXGI_FORMAT>(dxgiFormat)` を写像経由に変更(コメントも追従)。

## Rollback policy
WIP コミット単位で `git revert`。hello_xr は setup 再実行で再現(パッチ既定=挙動不変)。

## Steps
- [x] 1. **hello_xr TYPELESS パッチ + 列挙 probe** — target: `scripts/setup_helloxr_msvc.sh`(D3D11 ブロックのみ) — 内容: (i) `HELLO_XR_TYPELESS=1` で R8G8B8A8_TYPELESS(27) をランタイム列挙から探して早期 return(HDR パッチと同型)、(ii) RenderView の RTV format を「typeless なら同族 UNORM」写像(rtvColorDesc ブロック拡張。swapchainFormat 直渡しの CreateRenderTargetView は typeless で失敗するため)。 — done when: 再ビルド+デプロイ後、metasim/monado × D3D11 で `HELLO_XR_TYPELESS=1` 時のレイヤログ fmt を実測記録(27=列挙あり / 29等=フォールバック=非列挙)。
- [x] 2. **capture_d3d11.cpp の guard 拡張 + resolve 引数の typed 化** → guard へ TYPELESS 2種、ResolveTypedFormat 新設(R16 統合予定の一時重複と明記)、エラー文言/コメント追従。ビルド+14/14。
- [x] 3. **mjs に TYPELESS アサーション+非列挙 SKIP** → 両ランタイムで SKIP 行+rc=0+17/17 実測(列挙時アサーションは防御実装)。
- [x] 4. **回帰確認** → 17/17×2、MSAA 18/18、HDR 18/18、差分3ファイルのみ。

Metacognition self-check: pass — バックエンド間の規則統一(API完全性の最後の欠落解消)で、D3D12 実証済み規則の適用。写像の一時重複は R16 が畳む前提を明記(短期しのぎでなく承認順の尊重)。E2E 不能条件は防御 SKIP で正直に落とす。

## Resume pack
- **全 Step(1-4)+全 DoD 完了** → h-review へ。
- 実測サマリ(再導出禁止): probe=両ランタイム非列挙(fmt=29 フォールバック) → SKIP 経路で出荷。SKIP 行+rc=0+17/17 を両ランタイムで実測。回帰 17/17×2+MSAA 18/18+HDR 18/18+単体 14/14。R17 差分= capture_d3d11.cpp(+23/-9) / setup_helloxr_msvc.sh(+19) / integration_hello_xr.mjs(+16)。
- レビュー対象: R17 の wip 2件。contract=r17-d3d11-typeless(profile=runtime)。
- Metacognition self-check: pass — D3D12 実証済み規則の同型適用でバックエンド規則を統一。ResolveTypedFormat の一時重複は R16 統合前提をコード内コメントに明記(短期しのぎでなく承認順尊重)。非列挙は防御 SKIP で正直。

## Review result (iteration 3)
**Verdict: needs-fix → 修正 → pass**
- レビュアー実測(全 green): numstat 一致 / ビルド+14/14 / TYPELESS SKIP 両ランタイム再現(17/17+rc=0) / 回帰 17/17×2 / MSAA 18/18 / HDR 18/18 / D3D12 規則突き合わせ一致 / MSAA×TYPELESS 静的解析 OK / lens 3 grep 残存なし。
- **Finding(修正済み)**: setup パッチ(f)の冪等マーカー(スペース1個)が挿入テキスト(整列でスペース3個)に実在せず、setup 再実行が assert で死ぬ(レビュアーが read-only probe で実証)。→ マーカーを実在文字列(3スペース)へ修正し、受入基準どおり **2連続実行で rc=0+patched 0行** を実測確認(初回 rc=1 はゾンビ exe ロック由来で無関係、mv 退避後 rc=0)。
- nit(スコープ外の既存 doc-rot): capture.cpp:6-8 の「D3D11/D3D12 は not implemented エラーを返す」記述が現状と乖離 → 次の docs タスクで拭き取り(backlog R21 に既存の同種項目あり、そこへ追記)。

## Findings (h-work appends unplanned discoveries here)
- metasim/monado とも D3D11 では TYPELESS スワップチェーンフォーマットを列挙しない(実測)。D3D12 バックエンドの TYPELESS 受理も同様に E2E 未踏の防御コードである可能性が高い(バックエンド間一貫性が受理の根拠であり、実アプリ+他ランタイムへの備え)。
