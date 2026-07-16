# Backlog — cross-task ledger (advisory)

旧 `refactor-plan.csv`(R01〜R21) は 2026-07-16 に削除済み。未完了項目の要点は本ファイルに畳み込み済み
（レビュー時の実装注意は旧 refactor-plan-review.md 由来、同じく削除済み）。
**auto実行のスコープは move-only リファクタのみ**。挙動変更(下記 Deferred)は human approval まで自動着手しない。

次フェーズ: **Monado PoC**（Meta XR Simulator 代替検証）→ 計画は `.claude/monado-poc-plan.md`。

## Open — move-only refactors (auto-pickable / 優先度・依存順)
- **[P2] R12 xr-math-header** — pose_override.cpp の QMul/QConj/QRot/VAdd/VSub + control_channel.cpp の NormalizeQuat を xr_math.h(header-only inline)へ verbatim 集約。数式不変。注意: NormalizeQuat は float&×4、pose_override 側は XrQuaternionf — シグネチャ統一時は呼び出し側の書き換えが入る(数式不変なら move-only 扱い可)。
- **[P2] R16 dxgi-formats-header** — capture_d3d11/d3d12 に重複する DxgiIsRGBA8/BGRA8 を dxgi_formats.h へ共有。TYPELESS 受理差(D3D12=可/D3D11=否)は allowTypeless 引数で可視化し現挙動維持(D3D11=false/D3D12=true)。差異の解消自体は R17。
- **[P2] R11 unit-tests** — ホスト実行ユニットテスト拡充。対象: pixel_convert 全関数(HalfFloatSelfTest の昇格)、LinearizeViewDepth(reversed-Z/inf 境界)、NormalizeQuat、HandTopFromBindingPath/InferHandTops、AggregateFallback(bool OR/float絶対値max/vec2大きさ勝ち)、EyeToIndex。**depends R03(済), R12**。※googletest 基盤(VR_AGENT_BUILD_TESTS, 単体13件)は導入済み — 残対象の棚卸しから。
- **[P2] R13 lodepng-pin** — FetchContent の lodepng を GIT_TAG master→コミットハッシュ固定+非推奨 FetchContent_Populate(単独形)是正。注意: lodepng.cpp を add_library で直接ソースに入れているため、MakeAvailable 化の際は lodepng 側 CMakeLists を走らせない取り込み方(SOURCE_SUBDIR ダミー等)にする。
- **[P3] R19 layer-state-split** — control_channel の公開状態ストア(poses/head/views/haptics 5系統の mutex+globals)を layer_state.{h,cpp}(Set/Get API)へ、ソケット transport を control_channel_transport へ分離。プロトコル(コマンドテーブル)専任に。注意: Handle_reset が g_pose_mutex を直接ロックして g_poses.clear() するバックドアあり — API 経由に揃える(挙動不変で可能)。**depends R06(済)**。
- **[P3] R20 server-ts-split** — mcp/src/server.ts(730行、registerTool 19個)を client.ts(ControlClient)/math.ts(quat/vec)/tools/*.ts(input/pose/head/observe)へ分割。server.ts は登録と起動のみに。
- **[P3] R21 stale-comments** — openxr_agent_layer.cpp:13-19 冒頭コメント是正(doc-only): (1)「D3D11/D3D12 still return an explicit not implemented error」は虚偽(実装済み。残る非対応は MSAA/HDR/TYPELESS のみ=R08-R10/R17)。(2) per-instance dispatch TODO(18-19行)は GAP-07 で解消済み+R18削除で方針否定済み — 削除し「単一 XrInstance 前提は意図的設計判断」を1行明記(layer_dispatch.cpp:15-22 と整合)。R04 分割後の hooks_*.cpp 移動言及の是正(capture_backends.h:8-9 含む)も同範疇。
- **[P3] layer-log-unused-string** — layer_log.h の未使用 `#include <string>`(R14 で顕在化)を掃除。transitive include 依存の可能性 — 除去前に他 TU のビルド影響を要検証。 — src: journal 2026-07-14 (R14 review)

## Open — OpenVRマイルストーン (設計= .claude/openvr-milestone-plan.md、M0 は Done)
- **[P3] setup-curl-fsl** — setup_monado.sh / setup_opencomposite.sh の curl を -fSL に統一(HTTPエラー早期失敗+診断。PE検査バックストップは維持)。 — src: journal 2026-07-16 (M1 review nit)
- **[P3] setup-hellovr-nits** — setup_hellovr.sh: MSBuild パスの BuildTools ハードコードに preflight 追加(VS Community等で親切に失敗)、部分クローン残留時の [ -d ] 判定の脆さ。 — src: journal 2026-07-16 (M2 review nits)
- **[P3] openvr-test-graceful-promote** — integration_openvr_test.sh の graceful ゲートを SKIP→FAIL 昇格(PASS 達成可能と実証済みのため退行検知を効かせる)。寸法チェックの scs[1] 追加も同時に。 — src: journal 2026-07-16 (M4 review 観察)
- **[P2] openvr-real-game-injection** — 実ゲーム(OpenXR or OpenVRレガシー直読み系)での入力注入到達検証。M4 の SKIP 事項(OC の IVRInput マニフェスト・ルーティングは legacy 合成で未到達)。ゲーム選定はユーザー判断。 — src: journal 2026-07-16 (M4/M5)

## Deferred — behavior-changing (⚠ 2026-07-16 ユーザー着手指示あり: 「push(A)の次にこれ(C)をやる」— push 完了後に R08→R09→R10→R17 の順で着手可)
- **[P1] R09 d3d12-msaa** — DIRECT リストで RENDER_TARGET→RESOLVE_SOURCE 遷移+ResolveSubresource→単一サンプル中間リソース(→COPY_SOURCE)→readback。中間リソースは再利用キャッシュ。effort M。
- **[P1] R10 d3d-hdr** — R16G16B16A16_FLOAT を pixel_convert(HalfToFloat/QuantizeSrgb)共有で D3D11/D3D12 に half→sRGB decode 追加。結果 JSON の tonemapped/colorConversion フィールドも Vulkan と同形に。**depends R03(済)**。
- **[P2] R17 d3d11-typeless** — D3D11 でも TYPELESS 受理。staging を desc.Format(=TYPELESS)のまま作って生バイトを読めば済む(D3D12 の ResolveFootprintFormat と同規則の同族 UNORM 解釈でも可)。R16 の allowTypeless=true に切り替えるだけの局所変更。**depends R16**。effort S。
- **[P2] R15 error-json-unify** — エラー応答の api/eye/viewIndex を共通 fail ヘルパー(capture_common)で3バックエンド統一(Vulkan にも追加、D3D11 の10箇所手書きを置換)。エラー文言は不変だが出力が変わるため挙動変更扱い。成功 JSON の sampleCount/msaaResolved 等を統一するかは実施時に1行決める。追記(R08 review nit): capture_d3d11 の EnsureResolveTexture 失敗 JSON に hr 併記も(兄弟エラーは全て hr 付き)。
> これらは出力が変わるため、ユーザー承認まで自動着手しない。~~この環境で E2E 不可~~ → **2026-07-16 解消**:
> MSVC 版 hello_xr(third_party/hello_xr_msvc、`HELLO_XR_EXE` で指定)により D3D11/D3D12 の E2E が可能になった。

## Doing

## Done
- **[P1] R08 d3d11-msaa** — D3D11 MSAA を ResolveSubresource 2段構成+再利用キャッシュで対応。hello_xr へ HELLO_XR_SAMPLE_COUNT パッチ(RTV/DSV MSAA 追従込み)、MSAA アサーション+ランタイム能力 SKIP(monado は VALIDATION_FAILURE で拒否=SKIP)。metasim 18/18・回帰17/17×2・レビュー pass。 → resolved(feat コミット、squash済)。 — src: journal 2026-07-17 (L60-L63)
- **[P3] m5-openvr-docs** — README+メモリ反映(事実照合レビュー pass)。**OpenVRマイルストーン M0〜M5 完了**。 → resolved c4c3f33。 — src: journal 2026-07-16 (M5)
- **[P2] m4-openvr-integration-test** — OpenVR統合テスト新設(15 PASS/1 SKIP明示+graceful PASS、回帰2本17/17)。 → resolved 5efccce。 — src: journal 2026-07-16 (M4)
- **[P2] m3-openvr-layer-smoke** — v1.8.19 タグ固定(OC版数上限適合)+スモーク完遂。framesObserved=550、OpenXR側=D3D12、actions=legacy-*42件(M4前提)。M2の「到達」誤認を訂正。 → resolved(タグ固定コミット)。 — src: journal 2026-07-16 (M3改)
- **[P1] m2-hellovr-dx12** — setup_hellovr.sh(dx11不存在→dx12 x64変換採用)。Monado 実証15s生存、OpenXR側=D3D11 client(OpenComposite選択)。レビュー needs-fix(doc)→pass。 → resolved(WIPコミット)。 — src: journal 2026-07-16 (M2)
- **[P1] m1-opencomposite-setup** — setup_opencomposite.sh(直DL+PE x64検査+VERSION、per-app方式のみ)。レビュー pass。 → resolved(WIPコミット、squash判断は最終出荷時)。 — src: journal 2026-07-16 (M1)
- **[P1] d3d11-flat-capture** — D3D11 キャプチャ単色バグ。根因= KEYEDMUTEX 共有画像の未同期読み(MiscFlags=0x100 実測)。AcquireSync(0)/ReleaseSync(0) で修正、失敗2経路は明示エラー。D3D11 17/17 ×{metasim,monado}、回帰なし、レビュー pass。 → resolved 3dbb431。 — src: journal 2026-07-16 (M0)
- **[P1] R04 hooks-split** — openxr_agent_layer.cpp を hooks_capture/locate/action の3TUへ move-only 分割(926→~290行)。correctness pass(27フック全数バイト等価) / readability needs-fix(`#include <string>` dead)→修正→pass、E2E 15/15。 → resolved a7fcd7b。 — src: journal 2026-07-14
- **[P1] R05 registry-erase-actionset** — Hook_xrDestroyActionSet の erase を action_registry::RegistryEraseActionSet(callback注入で逆依存回避)へ移動(move-only、handle-reuse安全性保存)。 → resolved db4334c。correctness pass / readability needs-fix(dead using 5本)→修正→pass、E2E 15/15。 — src: journal 2026-07-14
- **[P2] R14 buildactionsjson-cleanup** — LayerBuildActionsJson 転送を除去、control_channel が action_registry::BuildActionsJson を直接呼ぶ(move-only、phase4 TODO完遂)。 → resolved 97b7f88。レビュー2観点pass、E2E 15/15。 — src: journal 2026-07-14
- **[P1] R07 capture-dispatch-table** — CaptureOnEndFrame の snapshot構築を ParseEndFrameSnapshot へ抽出 + 3バックエンド分岐を Backend テーブルへ畳込(move-only、depth非対称保存)。 → resolved 58a48a8。レビュー2観点pass、E2E 15/15。 — src: journal 2026-07-14
- **[P1] R06 control-channel-split** — HandleRequest 14コマンドを Handle_<cmd>+テーブルディスパッチへ分離(move-only、応答JSON不変)。 → resolved aabcfd1。correctness pass / readability needs-fix(コメント孤立)→修正→pass、E2E 15/15。 — src: journal 2026-07-14
- **[P1] R03 pixel-convert-extract** — 純変換5関数を pixel_convert.{h,cpp}(vr_agent::)へ verbatim 抽出(move-only、R11/R10の前提)。ClassifyDepthFormat等VK依存は残置。 → resolved 4954eb1。レビュー2観点pass、E2E 15/15。 — src: journal 2026-07-14
- **[P1] R02 vulkan-dedup** — capture_vulkan の copy→submit→fence→map 重複を RecordAndSubmitCopy/MapStaging へ単一source化(move-only、F1二重アロケ解消) → resolved 7abb76f。レビュー2観点pass、E2E 15/15。 — src: journal 2026-07-14
- **[P1] R01 capture-common** — capture 3バックエンド共通部(RepackRows/EncodeRgbaPng/BuildCaptureSuccessJson)を capture_common.{h,cpp} へ抽出(move-only) → resolved(master R01 merge)。 — src: journal 2026-07-14
- **R18** — per-instance dispatch 化は実施しない決定(能力要求なし・単一インスタンス前提は意図的設計判断)。CSV からも削除済み。 — src: refactor-capability-vs-cleanup
