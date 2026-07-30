# SteamVR 仮想コントローラードライバー計画 (driver_playspectra)

決定日: 2026-07-17。The Lab 実測で確定した方針転換。

> **⚠️ 読み替え注意(2026-07-30 追記)**: 本計画は PlaySpectra 大改革(2026-07-19、`playspectra-architecture.md`)
> **以前**の起票。G3 として着手する際は新アーキテクチャ前提で読み替えること:
> - 接続元は TS MCP(**廃止方向確定** 2026-07-24)ではなく **Server / Virtual Device Core**(Python、
>   `tools/playspectra_server.py`)。下のアーキ図の「MCP server (TS)」は当時のもの。
> - ワイヤプロトコルは本文の「pose / input / status」ではなく **Monado Adapter(:52702)と同一の
>   VirtualDeviceState / NDJSON**(`hello`/`set_state`/`get_state`/`status`/`reset` + `request_id` +
>   writer 排他 — `playspectra-device-core-spec.md` §5)。**Adapter 側がプロトコルを揃えれば Server/MCP は
>   無改修で両対応**(backlog vd2 の要求)。現スケルトン `driver/src/driver_playspectra.cpp` は旧レイヤー
>   プロトコル(:52701)のままなので要改修。
> - **VD5(MCP ルーティング)は撤回済み**(2026-07-24。要件は vd2 へ吸収され、経路分岐は不要と判明)。
> - **VD4(仮想 HMD)は縮退済み**: 「完全 headless」目標は Monado Adapter(M2)で既達。残スコープは
>   実 SteamVR 共存ケースのみ(backlog)。

## なぜ必要か (実測事実)

The Lab (OpenVR ゲーム) + OpenComposite + SteamVR/OpenXR ランタイムで実測した結果:

| 機能 | 結果 | 経路 |
|---|---|---|
| screenshot | ✅ D3D11 1717x2052 撮れた | レイヤー xrEndFrame フック (拡張不要) |
| head 注入 | ✅ 視点が動いた (画角の違和感あり) | レイヤー xrLocateViews フック (拡張不要) |
| ボタン/コントローラー注入 | ❌ 不可 | XR_EXT_conformance_automation — **SteamVR が非対応** |

conformanceAutomation:false は SteamVR ランタイムの制限であり、レイヤー側では回避不能。
Meta XR Sim / Monado では従来どおり CA 経路が動く(15 PASS 実証済み)。

## アーキテクチャ

```
MCP server (TS)
  ├─ 既存: control channel :52700 → OpenXR レイヤー (capture / head / CA注入)
  └─ 新規: driver channel  :52701 → driver_playspectra.dll (SteamVR 内)
                                      ├─ 仮想コントローラー L/R (TrackedDeviceClass_Controller)
                                      ├─ DriverPose_t 更新 (RunFrame)
                                      └─ IVRDriverInput (bool/scalar コンポーネント)
```

- **driver_playspectra.dll**: OpenVR ドライバー API (`openvr_driver.h`) を実装する server tracked device provider。
  - `HmdDriverFactory` エクスポート → `IServerTrackedDeviceProvider`
  - Init で仮想コントローラー2本 (`ITrackedDeviceServerDriver`) を `TrackedDeviceAdded`
  - 入力プロファイル: **`oculus_touch` 偽装が現行実装**(下記「コントローラー偽装の方向決定」節が正。
    当初案の独自 JSON `resources/input/playspectra_profile.json` はディスクに残るがコードから未参照)
  - IPC: NDJSON over TCP。~~コマンド: pose / input / status~~ → **冒頭注記のとおり :52702 と同一の
    VirtualDeviceState プロトコルへ揃える**(現スケルトンは旧形式のまま=要改修)
- **SteamVR 登録**: `vrpathreg adddriver <repo>/driver/playspectra` + steamvr.vrsettings
  `activateMultipleDrivers: true`。実機 Rift ドライバーと共存(コントローラー4本になるが直近アクティブが勝つ)。
- **openvr_driver.h**: ValveSoftware/openvr v1.8.19 タグ固定 (プロジェクト全体の openvr ピンと統一)。
  ドライバー API は後方互換なので現行 SteamVR で動く。
- **既存レイヤーとの関係**: capture/recording はレイヤーのまま(動いている)。入力だけドライバー経路が加わる。
  ~~MCP が「CA が使えるランタイムか」で経路を自動選択~~ → **撤回**(旧 VD5、2026-07-24。Adapter が
  プロトコルを揃えるため MCP/Server 側の分岐は不要)。

## マイルストーン

- **VD1** driver skeleton: DLL ビルド + vrpathreg 登録 + SteamVR がコントローラー2本を認識
  (機械判定: `vrcmd` or SteamVR ステータス画面 / vrserver.txt ログに playspectra デバイス追加行)
- **VD2** pose 注入: :52701 経由で DriverPose_t 更新。The Lab 内で手が動く(スクショで機械判定可)
- **VD3** ボタン注入: IVRDriverInput コンポーネント。The Lab の「スタート」ボタン押下到達

### 実機との共存 / 優先度調停 — 方向性のみ決定、方式は仮説段階 (2026-07-18)

要求: 実機コントローラー入力をバイパスしつつ、MCP 注入があればそちらを優先（またはその逆）。

**検証済みの事実**:
- `IVRServerDriverHost::GetRawTrackedDevicePoses()` は v1.8.19 ヘッダに実在
  (third_party/openvr_driver_sdk/openvr_driver.h:3370)。**挙動・呼び出し可否は未検証**。

**仮説（この環境で未検証。設計に採用する前に VD2 で実測すること）**:
- **H1 (役割の活動追従)**: SteamVR は同役割コントローラーが複数あるとき「最近入力があった
  デバイス」へ手の役割を割り当てる … 学習知識由来。バージョン依存・条件の詳細不明。
  **検証法**: 実機 Touch + 仮想ペア同時接続で、(a) 注入時 (b) 実機操作時に
  役割がどちらに付くかを SteamVR デバイスパネル / GetControllerRoleForTrackedDeviceIndex で実測。
- **H2 (pose ミラーの実現性)**: GetRawTrackedDevicePoses で他デバイスのポーズを RunFrame から
  読める … API 存在のみ確認済み。**検証法**: VD2 実装時にログ出力で実機ポーズが取れるか確認。
- **H3 (oculus_touch 偽装でバインディング自動解決)**: ControllerType=oculus_touch を名乗れば
  既存ゲームの Touch バインディングが仮想ペアに当たる … 学習知識由来。
  **検証法**: The Lab で仮想ペアに役割を付けた状態でスタートボタン到達を実測。

**進め方**: VD2 は「pose 注入 + 上記 H1〜H3 の実測」をスコープに含める。実測結果を本節に追記
してから、方式1（活動ベース切替に乗る）/ 方式2（ミラー+チャンネル単位 mux）のどちらを
出荷形にするか決める。**実測前にどちらの方式もコミットしない。**
- **VD4** 仮想 HMD: TrackedDeviceClass_HMD + IVRDisplayComponent + IPC ポーズ注入。
  **🔬 動機は仮説 (2026-07-19 相対化。詳細は playspectra-architecture.md §3.2)**:
  観測事実は「The Lab (SteamVR) で head override 時に画角の違和感があった」まで。
  「原因はコンポジターが物理 HMD ポーズでリプロジェクションを続けること」「ドライバーで HMD を
  仮想化すれば根本解消」は**未実測の仮説**であり、事実として扱わない(metasim では歪まない)。
  検証法: 実 SteamVR で物理HMDポーズ固定 vs 仮想HMDポーズ注入の両条件で歪みを撮って比較。
  実測して初めて「歪みの原因」を決定へ昇格させる。**この仮説は Monado 採用理由からは分離する**
  (Monado の根拠はソース改変可能/正規経路/headless/CI の事実のみ)。
  なお実現時の切替(実機観戦 ⇔ 仮想HMD)は vrsettings フラグ (driver_playspectra.virtualHmd) +
  SteamVR 再起動を想定(SteamVR は HMD を1つしか採用しない)。
- ~~**VD5** MCP ルーティング: vr_input/vr_set_controller/vr_set_hmd が経路自動選択~~ **撤回**(2026-07-24。
  要件は vd2〈:52702 と同一プロトコル〉へ吸収。ツール名も廃止方向の TS 版の語彙だった)
- **VD6** docs + 統合テスト

## コントローラー偽装の方向決定 (2026-07-17) — 効果は仮説 H3、実測待ち

偽装先は `oculus_touch` を選ぶ(独自タイプ playspectra_controller ではなく)。選定理由のうち**事実**は
(1) ユーザー実機が Rift CV1 + Touch で、steamvr.vrsettings に oculus_touch_250820_* キーが実在する
こと(実測)。(2) knuckles はスケルタル入力エミュレーションが必要で自動化に不要、(3) Vive wands は
ジョイスティックなし、は学習知識由来の比較。
**「偽装すれば既存ゲームの Touch バインディングが仮想ペアに当たる」は仮説 H3**(上の共存節参照)。
VD2 実装は H3 の検証装置として ControllerType=oculus_touch +
InputProfilePath={oculus}/input/touch_profile.json + Touch 実配置コンポーネント(左 x/y・右 a/b
非対称、thumbrest touch 含む)を実装済み — {oculus} パス解決可否・バインディング解決可否とも
The Lab で実測してから確定する。

## リスク / 未知

- Rift 実機コントローラーとの優先順位 (SteamVR は「最後に入力があったデバイス」を利き手に割当てる —
  仮想側に入力を流せば勝てる想定だが VD2 で実測)
- MinGW での openvr_driver.h ビルド (クラス ABI は COM ライクな純仮想なので MinGW で問題ない見込み。
  hellovr で openvr_api 連携の MinGW 実績はないが、レイヤーDLL 側の MinGW 実績はある)
- ドライバーのプロセスは vrserver.exe — クラッシュすると SteamVR ごと落ちる。例外は全部飲む
  (レイヤーの HandleRequest "never throws" と同じ流儀)

## 関連アイデア (別タスク)

- mcp-media-return: MCP Apps / 画像・動画リターン。録画 mp4 をクライアント内で直接確認できるように。
