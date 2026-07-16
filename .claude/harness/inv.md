# Investigation summary: R17 d3d11-typeless — D3D11 で 8bit TYPELESS スワップチェーンを受理
Updated: 2026-07-16T18:15:00Z

## Impact scope
- `layer/src/capture_d3d11.cpp` guard(Step 2) — DxgiIsRGBA8 へ `DXGI_FORMAT_R8G8B8A8_TYPELESS`(27)、DxgiIsBGRA8 へ `DXGI_FORMAT_B8G8R8A8_TYPELESS`(90) を追加(D3D12 の guard と同形に)。8bit TYPELESS は族内でバイト配置同一のため生バイト読みで正しい(L69 の原理どおり、16bit TYPELESS とは違い曖昧性なし)。
- `layer/src/capture_d3d11.cpp` staging — `sdesc.Format = desc.Format` のまま変更不要(TYPELESS staging の Map は合法、生バイトが読める — backlog 記載どおり)。
- `layer/src/capture_d3d11.cpp` MSAA resolve — **要修正**: 現在 `ResolveSubresource(..., static_cast<DXGI_FORMAT>(dxgiFormat))` で、F3 修正後のコメントが「guard の受理フォーマットは全て typed」を根拠にしている。R17 で TYPELESS が guard を通ると **resolve の format 引数が typeless になり不正**。D3D12 の `ResolveFootprintFormat` と同規則の同族 UNORM 写像(R8G8B8A8_TYPELESS→UNORM / B8G8R8A8_TYPELESS→UNORM)をローカル関数 `ResolveTypedFormat` として追加し resolve 引数に使う。コメントも追従。
  ※R16(dxgi_formats.h 共有ヘッダ)未実施のため写像は capture_d3d12.cpp と一時重複 — R16(Open, move-only)実施時に統合される。R16 を先にやる案は、ユーザー承認順(R08→R09→R10→R17)の外に別タスクを差し込むことになるため不採用。
- `scripts/setup_helloxr_msvc.sh` — E2E 用パッチ第4弾(D3D11 ブロックのみ): `HELLO_XR_TYPELESS=1` で SelectColorSwapchainFormat が R8G8B8A8_TYPELESS(27) をランタイム列挙から探して早期 return(HDR パッチと同型)。**加えて hello_xr の RTV は swapchainFormat 直渡しのため typeless だと CreateRenderTargetView が失敗** → RenderView の RTV format を「typeless なら同族 UNORM」に写像するサブパッチも必要(R08 で追加済みの rtvColorDesc ブロックに手を入れる形)。
- `scripts/integration_hello_xr.mjs` — env `HELLO_XR_TYPELESS=1` のとき `shot.format===27` を PASS 項目に(MSAA/HDR 行と同型の env ガード)。ランタイムが TYPELESS を列挙しない場合は mjs 内で明示 SKIP(HDR の防御 SKIP と同型 — hello_xr は列挙になければ typed へフォールバックするため)。
- `scripts/integration_test.sh` — 変更不要見込み(swapchain 作成拒否経路は既存 SKIP grep が VALIDATION_FAILURE 前提。TYPELESS では「列挙にない→hello_xr が typed へフォールバック」が先に起きるので xrCreateSwapchain 失敗経路は通らない見込み)。

## Constraints and assumptions
- 8bit TYPELESS→UNORM 解釈は D3D12 バックエンドの既存実装(ResolveFootprintFormat + guard の TYPELESS 受理)と同じ規則 — バックエンド間の一貫性がそのまま設計根拠。
- PNG 出力は 8bit 生バイトコピーのため UNORM/UNORM_SRGB のどちらの解釈でもバイト列は同一(変換なし) — 「無言の壊れた画像」リスクなし。成功 JSON の format には dxgiFormat(27) がそのまま載る(観測の正直さ)。
- probe 先行: metasim/monado の D3D11 が TYPELESS をスワップチェーンフォーマットとして**列挙するか**は未知。列挙しなければ E2E 不能 → mjs の明示 SKIP+レビュー重点(HDR と同じ扱い)。レイヤー側の受理コード自体はどのみちコア必須(実アプリが TYPELESS を要求し得る)。
- ビルド/回帰手順は R08-R10 と同一。単体テスト(vr_agent_test 14件)も green 維持(L67: DoD に含める)。

## Assumptions (minor ambiguities — state them and proceed)
- R16 は未実施のまま R17 を単独実装(写像関数の一時重複を許容、R16 で統合)。理由: ユーザー承認済みの着手順を崩さない+重複は2関数×数行で管理可能。
- hello_xr の D3D12 側 TYPELESS パッチは作らない(R17 は D3D11 タスク。D3D12 レイヤーは既に TYPELESS 受理済みで、その E2E 検証残はスコープ外)。

## Open questions (unresolved)
- [ ] metasim/monado の D3D11 が TYPELESS(27) を列挙するか(empirical、work 冒頭 probe。両方非列挙なら E2E は SKIP+コード審査で出荷 — HDR の防御 SKIP 前例に従う)。critical だが probe で即断可。
