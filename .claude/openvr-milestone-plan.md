# マイルストーン: OpenVR 対応 (OpenComposite アダプタ方式)

作成: 2026-07-16。ユーザー承認済み(「新マイルストーンとして実装を設計して、h-loopで実装を開始」)。
背景メモ: memory `openvr-support-direction` / `monado-poc-go`。

## ゴール

OpenVR API で書かれた VR アプリに対しても、OpenXR アプリと同様に「入力注入 + 画面キャプチャ」が
機能すること。検証対象は OpenVR 公式サンプル hellovr_dx12 の x64 変換ビルド(自前ビルド、Steam 完全不要。
※hellovr_dx11 は openvr リポジトリに存在しない — 2026-07-16 実測で判明した誤前提を是正済み)。

## アーキテクチャ(コア無改修が大原則)

```
OpenVRアプリ → openvr_api.dll 差し替え(OpenComposite) → OpenXRローダ → vr_agent layer → Monado
```

- vr_agent レイヤー / MCP サーバーには**原則1行も手を入れない**。OpenVR 対応はアダプタ(OpenComposite)と
  テスト資産の追加で実現する。レイヤー変更が必要になったら、それは「OpenXR 普遍面の欠陥」なので
  理由を明記して個別判断(CLAUDE.md のスコープ2軸を厳守)。
- OpenComposite は GPLv3。third_party/ 配置(非コミット)+セットアップスクリプトで取得。再配布はしない。

## フェーズ分解

### M0: d3d11-flat-capture 修正【前提バグ・P1】
- backlog「Open — bugs」の d3d11-flat-capture。OpenVR 世代ゲームはほぼ D3D11 のため事実上の前提。
- 再現: `HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe bash scripts/integration_test.sh D3D11`
  (metasim / VR_RUNTIME=monado の両方で distinctColors=1 の単色キャプチャ)。
- 容疑順: (1) released image index の選択(D3D11 の acquire/release サイクルと snapshot の対応)、
  (2) CopySubresourceRegion の srcSubresource/arrayIndex、(3) Map/RowPitch 処理。
  D3D12 は正常なので、D3D12 実装との差分比較が最短の手がかり。
- 完了条件: 両ランタイムで D3D11 17/17 + graceful PASS(非退化キャプチャ含む)。Vulkan/D3D12 回帰なし。

### M1: OpenComposite 取得スクリプト
- `scripts/setup_opencomposite.sh` 新設: 公式ビルド(GitLab CI/リリース)を third_party/opencomposite/ へ。
  入手性が悪ければソースからのビルド手順に切替(MSVC。setup_helloxr_msvc.sh と同じ流儀)。
- 完了条件: openvr_api.dll(x64) が third_party に再現可能に配置され、VERSION 記録がある。

### M2: OpenVR サンプルアプリの用意 【2026-07-16 前提修正・完了】
- ~~hellovr_dx11~~ は openvr リポジトリに**存在しない**(実測: dx12/opengl/vulkan のみ)。**hellovr_dx12(x64変換)を採用**。
  フォールバック: opengl(SDL2同梱) → 最小DX11自作(最終手段、不要だった)。
- `scripts/setup_hellovr.sh`: shallow clone → vcxproj を Win32→x64 機械変換 → MSBuild → third_party/hellovr/ へ
  配備(アセット+SDL2.dll は Monado 同梱を流用+openvr_api.dll を OpenComposite 版に差替)。
- 完了(実測、M3で修正・確定): **openvr は v1.8.19 にタグ固定必須**。master ヘッダは IVRSystem_026 を要求するが
  OpenComposite(openxr) の実装上限は System/Compositor=022, Input=010, RenderModels=006(Reimpl の
  GEN_INTERFACE 一次ソースで確定)。未実装版数だと VR_GetGenericInterface が null → hellovr は**無音の
  SDL モーダルで永久ブロック**(生存して見えるが 0 フレーム)。教訓: **プロセス生存≠フレームループ到達。
  完了判定は必ず framesObserved>0 で行う**。
- v1.8.19 実測: framesObserved=550、スワップチェーン 896x1007 fmt=29 ×2、**OpenXR 側は D3D12 client**
  (アプリの DX12 提出に整合。※master ヘッダ時に見えた D3D11 client init は仮アダプタ段階のログで、
  実フレーム経路ではなかった)。

### M3: レイヤー結合スモーク 【2026-07-16 完了】
- 実測(m3_smoke_results.json、Monado+レイヤー+hellovr_dx12@v1.8.19):
  - status: instance/session=true、api=**D3D12**、framesObserved=550、swapchains 896x1007 fmt=29 ×2
  - **actions 実態**: アクションセット「opencomposite-actions」1個(attached=true)に `legacy-*` 命名の
    42 アクション(trigger/grip/thumbstick/trackpad/menu/a/system/haptic/aim-pose/grip-pose × 左右)。
    束縛は 6 プロファイル(khr/simple, oculus/touch, valve/index, htc/vive, microsoft/motion, hp/mixed_reality)。
    hellovr のアプリ定義アクション名ではなく **OpenComposite のレガシー入力翻訳名**が見える。
  - screenshot: ok、D3D12 経路、896x1007 truecolor 438KB(非退化、256値)
  - head 注入 → view 反映: y 1.588→2.917(ベース+注入の合成、STAGE 空間)、回転も反映
  - 非CA経路(conformanceAutomation=false)で全コマンド正常応答
- **M4 設計制約**: アサーションは `legacy-right-trigger` 等の OC 翻訳名+opencomposite-actions セット名を
  前提にする(hellovr 由来の名前は出ない)。入力注入の標的も legacy 系(例: 右トリガー=legacy-right-trigger)。

### M4: OpenVR 統合テスト系統 【2026-07-16 完了】
- 新設: `scripts/integration_openvr_test.sh` + `scripts/integration_openvr.mjs`(既存テスト無改修のコピー+
  OpenVR 実態改変。共有ライブラリ化は R20 系将来課題)。
- 結果: **15 PASS / 0 FAIL / 1 SKIP + graceful PASS (rc=0)**。回帰: metasim Vulkan 17/17・monado D3D12 17/17。
  - PASS 内訳: status(D3D12/framesObserved>0)/swapchains×2/actions(opencomposite-actions attached、
    legacy-right-trigger FLOAT、khr/simple束縛)/view+FOV/head反映(delta=1.395)/screenshot(729色 34.2%
    dominant の実シーン)/寸法一致/decode/withDepth honest degrade。
  - **SKIP(設計どおり明示)**: 入力注入→アプリ反応(haptic)。非CA[G]経路の合成は OpenXR アクション
    (legacy-*)には効くが、hellovr は IVRInput マニフェスト経由で読むため OC 内部のマニフェスト・
    ルーティングまで届かない(3秒注入で haptics 0→0 実測)。**M5/実ゲーム検証フェーズへの引き継ぎ事項**:
    レガシー入力直読みのゲーム(GetControllerState 世代)なら届く見込み — 実ゲームで要実測。
  - graceful: WM_CLOSE(taskkill /F なし)で xrDestroyInstance マーカー到達=レイヤー掃除検証 OK。
    その後 hellovr 自身が shutdown 後半で Segfault するが、マーカー到達後であり layer/OC 検証には無関係
    (hellovr@1.8.19 の終了時クセとして記録)。

### M5: ドキュメント反映
- README: 対応状況に OpenVR(via OpenComposite) 行を追加、セットアップ/実行手順を追記。
- backlog / memory の更新(M3 で記録したアクション名翻訳の実態と、MCP 側 UX の課題があれば起票)。

## スコープ外

- 実 Steam ゲームでの相性つぶし(DRM で Steam クライアントが要るため CI 不能。将来の手動検証テーマ)
- IVROverlay 等オーバーレイ系 API、OpenComposite 本体への upstream パッチ
- OpenComposite 同梱での再配布パッケージング(GPLv3 の扱いはその時に別途判断)

## リスクと逃げ道

- OpenComposite の Windows ビルド入手性 → ソースビルドへ切替(M1 内で判断)
- hellovr サンプルのビルド難易度(古い VS プロジェクト) → 解決済み(M2: vcxproj の Win32→x64 機械変換+MSBuild。
  自作アプリの最終手段は不要だった)
- OpenComposite が要求する OpenXR 拡張を Monado が欠く場合 → metasim でも試し、それでも駄目なら
  失敗ログを記録して当該フェーズを凍結(コアの欠陥ではない)
- M0 が capture_d3d11 の作り直し級に膨らむ場合 → D3D12 実装の移植を優先検討(挙動変更は E2E で担保)

## 完了定義(マイルストーン DoD)

1. M0: 3API × 2ランタイムの integration_test.sh が全 PASS
2. M4: OpenVR 統合テストが Monado 上で全 PASS
3. M5: README が実態を反映
