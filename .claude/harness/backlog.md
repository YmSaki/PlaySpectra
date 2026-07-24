# Backlog — cross-task ledger (advisory)

旧 `refactor-plan.csv`(R01〜R21) は 2026-07-16 に削除済み。未完了項目の要点は本ファイルに畳み込み済み
（レビュー時の実装注意は旧 refactor-plan-review.md 由来、同じく削除済み）。
**auto実行のスコープは move-only リファクタのみ**。挙動変更(下記 Deferred)は human approval まで自動着手しない。

現況(2026-07-22): **Monado PoC → PlaySpectra 大改革(2026-07-19) → M0.5 改名 / M1 仕様 / M2 Monado 仮想HMD+Touch /
Windows 統一スタックまで完了**（下 Done 節）。正典 `.claude/playspectra-architecture.md`、進捗
`.claude/playspectra-m2-status.md`、memory [[windows-monado-unified-stack]]。**以下 Open は改革前(〜2026-07-18)の
起票**で、SteamVR/OpenVR 系は architecture §9 で「SteamVR Adapter」に再位置づけ済み。優先度の再付けは M2 後の
ユーザー留保事項(architecture §8「順不同・M2 後に再優先度付け」)。

## Open — move-only refactors (auto-pickable / 優先度・依存順)
(全消化済み)

## Open — harness 改善
- **[P3] worktree-join-exclude-claude** — worktree fan-out → join (cherry-pick/merge) 時に tracked `.claude/` ファイルが混入して working tree の harness state を上書きする。join フローで `.claude/` を除外する仕組みが必要。 — src: h-evolve 2026-07-17 (journal L85)

## Open — SteamVR Adapter (旧「SteamVR 仮想ドライバー」・architecture §9 で再位置づけ、設計= .claude/steamvr-driver-plan.md)
> 注(2026-07-22): 改革で **PlaySpectra の「SteamVR Adapter」**に相当。driver/ に改名済スケルトン実在
> (`driver/src/driver_playspectra.cpp`)。**VD4(headless 仮想HMD)の目標は Monado Adapter(M2)で既達**のため、VD4 は
> 実 SteamVR 共存ケースへ縮退。全 VD 群は Windows/SteamVR 実機が要る(本環境不可)。新 Core への再接続も未。
> 追記(2026-07-24・ユーザー相談): **MCP/Server 側の実装は不要と判明**——Server は VirtualDeviceState の
> NDJSON プロトコルを喋る Adapter に繋ぐだけの薄いクライアント(architecture.md §2)なので、SteamVR Adapter
> が Monado Adapter(:52702)と**同一プロトコル**を実装しさえすれば接続先ポートが変わるだけで無改修対応する。
> 下記 vd5-mcp-routing はこの分離を知らない改革前(2026-07-17)設計の名残と判明したため撤回し、要件を
> vd2-pose-injection に統合した。Monado=headless/CIレーン・SteamVR=実ゲームレーンの**並存**が設計意図で、
> どちらかを「デフォルト」に選ぶ話ではない(architecture.md §2 図に明記済み)。詳細: memory
> [[mcp-adapter-migration-decision]]。
- **[P1] vd1-driver-skeleton** — driver_playspectra.dll スケルトン(HmdDriverFactory + IServerTrackedDeviceProvider + 仮想コントローラー2本)。SteamVR に認識されるまで。 — src: The Lab 実測 2026-07-17
- **[P1] vd2-pose-injection** — IPC(NDJSON/TCP :52701) 経由で DriverPose_t 更新 + **共存仮説 H1〜H3 の実測**(plan「実機との共存」節。役割の活動追従/GetRawTrackedDevicePoses 挙動/oculus_touch 偽装は全部未検証仮説 — 実測してから方式を決める)。**Monado Adapter(:52702)と同一の VirtualDeviceState/NDJSON プロトコルで実装し、Server/MCP を無改修で両対応させる**(2026-07-24、下記vd5撤回に伴い要件明記)。 — src: 同上 + 2026-07-18、2026-07-24
- **[P1] vd3-button-input** — IVRDriverInput の bool/scalar コンポーネント駆動。The Lab の「スタート」を押せるまで。 — src: 同上
- **[P3] vd-mirror-mux** — 方式2候補: 実機ミラー+チャンネル単位 inject-wins/real-wins mux。VD2 の H1/H2 実測結果を見てから設計判断。 — src: 2026-07-18
- **[P1] vd4-virtual-hmd** — 仮想 HMD (TrackedDeviceClass_HMD + IVRDisplayComponent + IPC ポーズ注入)。head override の歪み(コンポジターが実機ポーズでリプロジェクションする不整合)を根本解消し、完全仮想 (headless) モードを実現。vrsettings フラグで実機 Rift と切替。 — src: ユーザー指摘 2026-07-17 (「見え方もおかしいし、うまく操作できないとかありえる」)
- ~~vd5-mcp-routing~~ — **撤回(2026-07-24)**。旧文言「MCPツール(vr_input/vr_set_controller/vr_set_hmd)をCA経路(層)とdriver経路(SteamVR)で自動選択」は改革後のServer/Adapter分離を前提しない設計。上記の追記のとおりMCP側の分岐は不要と判明したため削除、要件はvd2-pose-injectionへ吸収。 — src: The Lab実測2026-07-17、撤回2026-07-24
- **[P2] vd6-docs-test** — README + 統合テスト + 実測記録。 — src: 同上

## Open — MCP 移行 (legacy TS 版の扱い、2026-07-24 方針決定)
- **[P2] mcp-ts-retire** — `mcp/`(TypeScript、`mcp/src/client.ts` が今も `:52700` Layer 直結・改革前設計のまま)を
  **廃止方向で確定**。現行の実働は `tools/playspectra_mcp.py`(Server/Adapter 越し、Windows 実機で MCP 全ツール
  14/14)。`mcp/` 側は分岐点 `24b88fbe` 以降、実質的な機能追加が止まっている(recording.ts も含め検証済み)。
  手順: (1) 未マージの `feat/mcp-recording-viewer-widget`(録画ビューアwidget、1コミット `7782fa062`、
  `24b88fbe` から分岐)は**この `mcp/` へマージしない**——廃止予定の設計へ機能を足すだけになるため。
  (2) ウィジェットのアイデアを `tools/playspectra_mcp.py` 側へ移植。(3) 移植後 `mcp/` を削除(または明示
  アーカイブ)し、`feat/mcp-recording-viewer-widget` ブランチも役目を終える。README「MCP サーバー
  （レガシー・TypeScript）」行もこの決定に合わせて更新済み(2026-07-24)。 — src: ユーザー方針確認 2026-07-24、
  詳細 memory [[mcp-adapter-migration-decision]]
- **[P3] mcp-apps-widget** — MCP Apps (UI ウィジェット) での動画プレイヤー埋め込み。インライン画像リターン(vr_stop_recording サンプルフレーム添付 + vr_view_recording 新設)は別ブランチ `feat/mcp-recording-viewer-widget` で実装済みだが、本ブランチ `feat/mcp-server` には**未マージ**(`mcp/src/tools/recording.ts` は `vr_start_recording`/`vr_stop_recording` のみ、stop の返り値に画像もなし)。**上記 mcp-ts-retire の決定により、このまま `mcp/` へマージするのではなく Python 版へ移植する方針(2026-07-24)**。Claude Code がウィジェット描画に対応したら mp4 プレイヤー化を検討。 — src: ユーザー発案 2026-07-17、事実訂正+方針確定 2026-07-24

## Open — OpenVRマイルストーン (設計= .claude/openvr-milestone-plan.md、M0 は Done)
- **[P2] openvr-real-game-injection** — 実ゲーム(OpenXR or OpenVRレガシー直読み系)での入力注入到達検証。**2026-07-17 The Lab で部分達成**: OC 経由レイヤー到達・screenshot/head 注入 OK。ボタン注入は SteamVR が conformance_automation 非対応で不可 → vd1〜vd5 (SteamVR 仮想ドライバー) が後継。 — src: journal 2026-07-16 (M4/M5), The Lab 実測 2026-07-17

## Deferred — behavior-changing (R08→R09→R10→R17 は 2026-07-17 に全消化済み)
- **[P2] R15 error-json-unify** — エラー応答の api/eye/viewIndex を共通 fail ヘルパー(capture_common)で3バックエンド統一(Vulkan にも追加、D3D11 の10箇所手書きを置換)。エラー文言は不変だが出力が変わるため挙動変更扱い。成功 JSON の sampleCount/msaaResolved 等を統一するかは実施時に1行決める。追記(R08 review nit): capture_d3d11 の EnsureResolveTexture 失敗 JSON に hr 併記も(兄弟エラーは全て hr 付き)。
> これらは出力が変わるため、ユーザー承認まで自動着手しない。~~この環境で E2E 不可~~ → **2026-07-16 解消**:
> MSVC 版 hello_xr(third_party/hello_xr_msvc、`HELLO_XR_EXE` で指定)により D3D11/D3D12 の E2E が可能になった。

## Done — PlaySpectra 改革 (2026-07-19〜2026-07-22、h-loop 外で実施につき journal 未記録・現況同期のため後追い記載)
- **M0.5 内部一括改名** — vr_agent/vragent→playspectra、VR_AGENT_*→PLAYSPECTRA_*、VR-MCP→PlaySpectra。layer/driver/mcp/scripts 追従(layer target=playspectra_layer/playspectra_test)。 → 15cca62(refactor/playspectra-rename-m05)。 — src: architecture §7/§8。**残: .claude/ 配下の追従漏れを 2026-07-22 h-evolve で一掃(rules/agent-memory/backlog の driver_playspectra 等)**
- **M1 VirtualDeviceState / 通信仕様(rev2)** — STAGE/完全スナップショット/valid・tracked・connected 分離/semantic-path 入力/grip・aim 独立/frame_synchronized(論理ステップ)/エラー分類/request_id・writer 排他。DoD 未達は「ユーザー再レビュー承認」1点のみ。 — src: `.claude/playspectra-device-core-spec.md`
- **M2 最小 Monado Virtual HMD + 左右 Touch** — 列挙 / pose・入力 / set_state / haptics 逆方向 / 複数 observer / reset を WSL2 で E2E、submodule に実装(親 gitlink 49010dbdf)。 — src: `.claude/playspectra-m2-status.md`
- **Windows 統一スタック(実GPU)** — Monado(PlaySpectra driver) を Windows ビルド → Server/wait_for/CLI 9/9・record 5/5・frame 10/10・reset 20/20・capture D3D11/D3D12/Vulkan 60/60・実アプリ hello_xr×capture 各20/20・MCP 14/14・coupling 2/2。真因=MSVC の UTF-8/CP932 誤読→/utf-8。 — src: memory [[windows-monado-unified-stack]]、`.claude/playspectra-windows-framework-decision.md`(案A 実行済)
- **Server / Scenario Runner / Recorder+Replay / MCP(Python)** — 高水準命令+補間+assert+capture-assert+auto-wait(wait_for)+record/replay、FastMCP 13ツール(operate→:52702 / capture→:52700)。 — src: README 実装状況表、`tools/playspectra_*.py`
- **設計書の git 追跡 + doc 整合** — 正典 architecture 等 .claude/ 設計書5点を git add -f、README/rules の stale 数値・ツール列挙・壊れた参照・旧識別子を是正。 → 526de7d9 / 6f53669d(feat/mcp-server)。 — src: 本セッション 2026-07-22

## Doing

## Done
- **[P3] unit-tests-registry-capture** — HandTopFromBindingPath inline化+EyeToIndex抽出+テスト14件。78/78 PASS。レビュー pass。 → resolved 0728461。 — src: journal 2026-07-17 (L91)
- **[P3] R20 server-ts-split** — mcp/src/server.ts(815行、21ツール)を client.ts/math.ts/tools/5ファイルに分割。server.ts=19行。tsc green。レビュー needs-fix(コメント欠落)→復元→pass。 → resolved 63609d2。 — src: journal 2026-07-17 (L90)
- **[P3] R19 layer-state-split** — control_channel の状態ストア5グループを layer_state.{h,cpp} に分離。型5個+API 15個超移動。ClearAllStickyPoses/GetStatus/GetHapticLog 追加。消費者9ファイル追従。レビュー needs-fix(dead include)→修正→pass。 → resolved 9b8efda。 — src: journal 2026-07-17 (L88-L89)
- **[P3] doc-cleanup-batch** — R21 stale-comments + layer-log-unused-string。コメント是正3ファイル + 未使用 #include 除去。ビルド green + 64/64 PASS。レビュー pass(2 notes non-blocking)。 → resolved 99b991c。 — src: journal 2026-07-17 (L86-L87)
- **[P3] scripts-nits-batch** — setup-curl-fsl + setup-hellovr-nits + openvr-test-graceful-promote を並列実行。curl -fSL統一/MSBuild edition preflight/graceful FAIL昇格+scs[1]寸法チェック。レビュー pass(findings なし)。 → resolved 1b9aa0e。 — src: journal 2026-07-17 (L84-L85)
- **[P2] recording-mode** — 連続フレームキャプチャ録画(Layer 周期 PNG + MCP ffmpeg optional)。E2E 20/20。レビュー pass(3 LOW fixes 即時適用)。 → resolved 7416521。 — src: journal 2026-07-17 (L80-L83)
- **[P2] R13 lodepng-pin** — lodepng GIT_TAG を master→コミットハッシュ ed6fe582 に固定。FetchContent_Populate → MakeAvailable(SOURCE_SUBDIR _none)是正。レビュー pass。 → resolved 5637bf03。 — src: journal 2026-07-17 (L79)
- **[P2] R11 unit-tests** — pixel_convert 5関数 + xr_math 6関数のテスト50件追加(合計64件)。プロダクションコード変更なし。レビュー pass。 → resolved 45d2d2f。 — src: journal 2026-07-17 (L76-L78)
- **[P2] R16 dxgi-formats-header** — DxgiIsRGBA8/BGRA8/HDR16F を dxgi_formats.h へ統合、ResolveTypedFormat/FootprintFormat → DxgiResolveTyped に名前統一。レビュー pass(findings なし)。 → resolved 0c14295。 — src: journal 2026-07-17 (L74-L75)
- **[P2] R12 xr-math-header** — QMul/QConj/QRot/VAdd/VSub + NormalizeQuat を xr_math.h(header-only inline, namespace vr_agent)へ verbatim 集約。dead `<cmath>` 除去。レビュー pass(findings なし)。 → resolved fdbca93。 — src: journal 2026-07-17 (L72-L73)
- **[P2] R17 d3d11-typeless** — D3D11 guard へ 8bit TYPELESS 2種(同族 UNORM 解釈、D3D12 と規則統一)+ResolveTypedFormat(MSAA resolve の typed 化、R16 統合予定)。両ランタイム非列挙のため E2E は防御 SKIP(実測)。回帰全 green。レビュー needs-fix(冪等マーカー)→修正→pass。R16 依存は解消不要と判明(ローカル写像で成立)。 → resolved(feat コミット、squash済)。 — src: journal 2026-07-17 (L70-L71)
- **[P1] R10 d3d-hdr** — 16F half→sRGB decode を capture_common 共有(DecodeHdrRowsToSrgb+単体テスト)で D3D11/D3D12 へ。JSON は Vulkan 同形・同文言。16bit TYPELESS は明示エラー維持。HDR E2E 18/18×4組合せ・回帰17/17×4・MSAA 18/18×2・単体14/14。レビュー needs-fix(vr_agent_test 依存漏れ)→修正→pass。 → resolved(feat コミット、squash済)。 — src: journal 2026-07-17 (L67-L69)
- **[P1] R09 d3d12-msaa** — D3D12 MSAA を RESOLVE 遷移+RESOLVE_DEST 常在中間キャッシュで対応。hello_xr D3D12 パッチ3点(env override/深度 SampleDesc/PSO SampleDesc)。monado は D3D11 と同型拒否=SKIP。metasim 18/18・回帰17/17×2・レビュー pass(findings なし)。 → resolved(feat コミット、squash済)。 — src: journal 2026-07-17 (L64-L66)
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
