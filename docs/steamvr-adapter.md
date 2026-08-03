# PlaySpectra SteamVR Adapter (driver_playspectra) — 📋設計・開発中

SteamVR ランタイムへ仮想コントローラー(将来は仮想 HMD)を正規のドライバー経路で供給する
Runtime Adapter。**現状は設計と、旧レイヤープロトコル(:52700 と同形式)のままのスケルトン実装**であり、
共有 Device Core([`devicecore/`](../devicecore/))への接続と Windows 検証は未完
(検証状況は [verification.md](verification.md) の SteamVR Adapter 行が正)。

本文書は検証境界を明示する: ✅実測済みの事実 / 🟡部分検証 / 📋設計 / 🔬仮説(検証法付き)。

## なぜドライバー経路が必要か (✅ 実測事実)

The Lab (OpenVR ゲーム) + OpenComposite + SteamVR/OpenXR ランタイムでの実測(2026-07-17):

| 機能 | 結果 | 経路 |
|---|---|---|
| screenshot | ✅ D3D11 1717x2052 撮れた | レイヤー xrEndFrame フック (拡張不要) |
| head 注入 | ✅ 視点が動いた (画角の違和感あり) | レイヤー xrLocateViews フック (拡張不要) |
| ボタン/コントローラー注入 | ❌ 不可 | XR_EXT_conformance_automation — **SteamVR が非対応** |

`conformanceAutomation:false` は SteamVR ランタイムの制限であり、レイヤー側では回避不能。
Meta XR Sim / Monado では CA 経路が動く(統合テストで実証済み)。したがって SteamVR で入力を
注入するには、ランタイムの下側 = OpenVR ドライバー API でデバイスそのものを供給する。

## アーキテクチャ

```
PlaySpectra Server / Virtual Device Core
  ├─ Monado Adapter   :52702 (稼働中 — VirtualDeviceState/NDJSON)
  └─ SteamVR Adapter  :52701 → driver_playspectra.dll (vrserver.exe 内)
                                 ├─ 仮想コントローラー L/R (TrackedDeviceClass_Controller)
                                 ├─ DriverPose_t 更新 (RunFrame)
                                 └─ IVRDriverInput (bool/scalar コンポーネント)
```

- **driver_playspectra.dll** (`driver/src/`): OpenVR ドライバー API (`openvr_driver.h`) を実装する
  server tracked device provider。`HmdDriverFactory` → `IServerTrackedDeviceProvider`、Init で
  仮想コントローラー2本 (`ITrackedDeviceServerDriver`) を `TrackedDeviceAdded`。
- **ワイヤプロトコル (📋)**: Monado Adapter(:52702)と同一の VirtualDeviceState / NDJSON
  ([device-core-spec.md](device-core-spec.md) — `hello`/`set_state`/`get_state`/`status`/`reset` +
  `request_id` + writer 排他)へ揃え、共有 Device Core(`devicecore/`)を vrserver プロセスへ静的リンクする。
  Adapter 側がプロトコルを揃えるため **Server/MCP は無改修で両対応**になる。
  **現行 `driver/src/driver_playspectra.cpp` は旧レイヤープロトコル(:52700 と同形式)のままの
  スケルトンであり要改修。**
- **SteamVR 登録**: `vrpathreg adddriver <repo>/driver/playspectra` + steamvr.vrsettings
  `activateMultipleDrivers: true`。実機ドライバーとの同時ロードは 🟡 実測済み(「コントローラー偽装」節の
  vrserver ログ観測)。その場合にコントローラーが4本列挙される、は 📋 想定(未実測)。
- **openvr_driver.h**: ValveSoftware/openvr **v1.8.19 タグ固定**(プロジェクト全体の openvr ピンと統一)。
  📋 ドライバー API は後方互換とされるため現行 SteamVR でも動く見込み(未実測。
  検証法: vrpathreg 登録後、vrserver.txt にデバイス追加行が出ることを確認)。
- **既存レイヤーとの関係**: capture はレイヤーのまま(✅ SteamVR 上で screenshot 実測済み — 上の表)。
  recording もレイヤー側だが SteamVR 上では未実測(他ランタイムで実証済み)。入力だけドライバー経路が加わる。

## 実機との共存 / 優先度調停 — 方向性のみ決定、方式は仮説段階

要求: 実機コントローラー入力をバイパスしつつ、注入があればそちらを優先(またはその逆)。

**✅ 検証済みの事実**:
- `IVRServerDriverHost::GetRawTrackedDevicePoses()` は v1.8.19 ヘッダに実在
  (third_party/openvr_driver_sdk/openvr_driver.h)。**挙動・呼び出し可否は未検証**。

**🔬 仮説(この環境で未検証。設計に採用する前に実測すること)**:
- **H1 (役割の活動追従)**: SteamVR は同役割コントローラーが複数あるとき「最近入力があった
  デバイス」へ手の役割を割り当てる … 学習知識由来。バージョン依存・条件の詳細不明。
  **検証法**: 実機 Touch + 仮想ペア同時接続で、(a) 注入時 (b) 実機操作時に
  役割がどちらに付くかを SteamVR デバイスパネル / GetControllerRoleForTrackedDeviceIndex で実測。
- **H2 (pose ミラーの実現性)**: GetRawTrackedDevicePoses で他デバイスのポーズを RunFrame から
  読める … API 存在のみ確認済み。**検証法**: pose 注入実装時にログ出力で実機ポーズが取れるか確認。
- **H3 (oculus_touch 偽装でバインディング自動解決)**: ControllerType=oculus_touch を名乗れば
  既存ゲームの Touch バインディングが仮想ペアに当たる … 学習知識由来。
  **検証法**: The Lab で仮想ペアに役割を付けた状態でスタートボタン到達を実測。

**進め方**: pose 注入の実装段階で H1〜H3 の実測をスコープに含める。実測結果を本節に追記してから、
方式1(活動ベース切替に乗る) / 方式2(ミラー+チャンネル単位 mux)のどちらを出荷形にするか決める。
**実測前にどちらの方式もコミットしない。**

## コントローラー偽装の方向決定 — 効果は仮説 H3、実測待ち

偽装先は `oculus_touch`(独自タイプ playspectra_controller ではなく)。選定理由のうち**事実**は
(1) 対象実機が Rift CV1 + Touch で、steamvr.vrsettings に oculus_touch_250820_* キーが実在する
こと(実測)。(2) knuckles はスケルタル入力エミュレーションが必要で自動化に不要、(3) Vive wands は
ジョイスティックなし、は学習知識由来の比較。
「偽装すれば既存ゲームの Touch バインディングが仮想ペアに当たる」は**仮説 H3**(上節参照)。
現行スケルトンは H3 の検証装置として ControllerType=oculus_touch +
InputProfilePath={oculus}/input/touch_profile.json + Touch 実配置コンポーネント(左 x/y・右 a/b
非対称、thumbrest touch 含む)を実装済み — {oculus} パス解決可否・バインディング解決可否とも
The Lab で実測してから確定する。
🟡 判明済みの境界: `{oculus}` 偽装パスは**実 oculus ドライバが同居している時だけ解決**する
(vrserver ログで確認)。完全 headless では別の解決手段が要る。

## 仮想 HMD (スコープ: 実 SteamVR 共存ケースのみ)

完全 headless は Monado Adapter が担うため、本 Adapter の仮想 HMD は**実 SteamVR 共存ケース**
(実機観戦 ⇔ 仮想HMD切替)を対象とする。
**🔬 動機は仮説**: 観測事実は「The Lab (SteamVR) で head override 時に画角の違和感があった」まで。
「原因はコンポジターが物理 HMD ポーズでリプロジェクションを続けること」「ドライバーで HMD を
仮想化すれば根本解消」は**未実測の仮説**であり、事実として扱わない(Meta XR Sim では歪まない)。
検証法: 実 SteamVR で (a) 物理HMDポーズ固定 (b) 仮想HMDポーズ注入 の両条件で歪みの有無を撮って比較。
実現時の切替は vrsettings フラグ (driver_playspectra.virtualHmd) + SteamVR 再起動を想定
(SteamVR は HMD を1つしか採用しない)。

## リスク / 未知

- 実機コントローラーとの優先順位: H1 のとおり仮説段階。仮想側に入力を流せば勝てる想定だが実測が要る。
- MinGW での openvr_driver.h ビルド: クラス ABI は COM ライクな純仮想なので MinGW で問題ない見込み。
  レイヤー DLL 側の MinGW 実績はあるが、driver DLL としての実績はまだない。
- ドライバーのプロセスは vrserver.exe — クラッシュすると SteamVR ごと落ちる。例外は全部飲む
  (レイヤーの HandleRequest "never throws" と同じ流儀)。
