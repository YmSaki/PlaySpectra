# PlaySpectra SteamVR Adapter (`driver_playspectra`) — 📋設計・開発中

SteamVRへPlaySpectraの仮想HMDと左右コントローラーを正規のSteamVR Driver経路で供給する
Runtime Adapter。目標は、物理HMDを前提にせず、SteamVRがPlaySpectraの3デバイスを使って
XRアプリケーションを実行できる状態にすることである。

現状の`driver/`は、左右の仮想コントローラーと独自Control Channelだけを持つ以前の
Controller-only Skeletonである。仮想HMD、共有Device Coreへの接続、Windows上のRuntime・Application
検証は未完であり、このSkeleton単独をSteamVR Adapter完成とは扱わない。検証状況は
[verification.md](verification.md)のSteamVR Adapter行を正とする。

本文書は検証境界を明示する: ✅実測済みの事実 / 🟡部分検証 / 📋設計 / 🔬仮説（検証法付き）。

## 目的と完成境界

SteamVR Adapterの初期完成範囲は次のとおり。

- `TrackedDeviceClass_HMD`の仮想HMDを1台公開する。
- `TrackedDeviceClass_Controller`の左右コントローラーを1組公開する。
- HMD Pose、Controller Grip/Aim Pose、Button、Trigger、Thumbstick、Tracking/Connection状態を
  共通`VirtualDeviceState`からSteamVRのDriver APIへ写像する。
- 物理HMDなしでSteamVRが仮想HMDを認識し、XRアプリケーションを開始できることを実測する。
- PlaySpectra所有のController Input ProfileとBindingをDriver Packageへ同梱し、別ベンダーの
  Driver Resourceなしで左右Controllerを利用できるようにする。
- Go Core、CLI、MCP、JSON Scenarioの操作意味論とScenario Schemaは既存の共通Protocolを使い、
  Runtime固有の操作モデルを追加しない。接続先の選択方法は別途Runtime設定として明示する。
- SteamVR + native OpenXRアプリケーションでoperate -> render -> observeを再現可能なWindows E2Eとして検証する。
- native OpenVRアプリケーションは入力到達と描画観測を分離して判定し、OpenComposite経由の結果を
  native OpenVRの証拠として扱わない。

### Device class・identity・body role

SteamVR MVPの実体は **1 HMD / 2 Controllers / 0 Trackers** とする。このPRおよび初期実装で
Generic Trackerや可変台数Protocolを実装する必要はない。

一方、Runtime AdapterのArchitectureは次のDevice構成を表現できる方向へ保つ。

- 1 HMD（SteamVRでは`TrackedDeviceClass_HMD`）。
- 0..N Controllers（`TrackedDeviceClass_Controller`）。
- 0..N Generic Trackers（`TrackedDeviceClass_GenericTracker`）。

各Deviceでは次の3概念を分離する。

- **device identity**: 再接続やbody role変更をまたいでDeviceを識別する安定ID。SteamVRのSerialへ写像する。
- **device class**: HMD / Controller / Generic Tracker。Runtime AdapterがSteamVRのTracked Device Classへ写像する。
- **body role**: `head`、`left_hand`、`right_hand`、`waist`、`chest`、`left_foot`、`right_foot`、
  `unassigned`等の任意割当。identityの一部にせず、同じDeviceへ後から再割当できる。

したがって`left` / `right`をDeviceそのもののidentityとして固定しない。MVPでは2台のControllerへ
`left_hand` / `right_hand`を割り当てるが、将来の追加ControllerやTrackerは別の安定identityを持つ。
body roleの語彙とSteamVR Propertyへの具体的な写像は、Generic Tracker実装時に一次資料と実機で確定する。

現行Protocol Version 1の`hmd` / `left` / `right`固定SchemaはMVPでそのまま使う。将来のCore Protocolは、
安定`device_id`をkeyとするDevice Collectionとclass/role metadataへ一般化できなければならない。
そのProtocol revisionはVersion/Capability negotiationを伴い、Version 1の意味を黙って変更しない。

次は初期完成の範囲外とし、別の検証・拡張として扱う。

- 物理HMDと仮想HMDの同時利用または切り替え。
- 実コントローラーと仮想コントローラーの優先度調停、Pose Mirror、Input Mux。
- 特定ベンダーのControllerとして偽装する方式の出荷判断。
- 物理Display向けDirect ModeとPhysical-HMD Display Compositor。
- Unity・Unreal Engine固有の統合設定やPlugin。
- Application-level deterministic frame timing。

## なぜDriver経路が必要か（✅実測事実）

The Lab（OpenVRゲーム）+ OpenComposite + SteamVR/OpenXR Runtimeでの実測（2026-07-17）:

| 機能 | 結果 | 経路 |
|---|---|---|
| screenshot | ✅ D3D11 1717x2052 | Layerの`xrEndFrame` Hook |
| head注入 | ✅ 視点が動いた（画角の違和感あり） | Layerの`xrLocateViews` Hook |
| Button/Controller注入 | ❌ 不可 | `XR_EXT_conformance_automation`をSteamVRが提供しない |

この結果は、Layer経由の入力注入だけではSteamVR Runtime上の完全な操作経路を構成できないことを示す。
SteamVRの下側に正規デバイスを供給するDriver Adapterが必要になる。

## アーキテクチャ

```text
PlaySpectra Go Core / CLI / MCP / JSON Scenario
                         |
                         | VirtualDeviceState / NDJSON
                         v
driver_playspectra.dll（vrserver.exe内）
  |
  +-- Virtual Device Core（既存devicecore/を組み込み）
  |     +-- Protocol parse / validation
  |     +-- Shared VirtualDeviceState
  |     +-- TCP Control Channel
  |
  +-- SteamVR Adapter Shell
        +-- Virtual HMD        -> TrackedDeviceClass_HMD
        +-- Left Controller    -> TrackedDeviceClass_Controller
        +-- Right Controller   -> TrackedDeviceClass_Controller
        +-- Generic Trackers   -> TrackedDeviceClass_GenericTracker（将来0..N）
        +-- Core state         -> DriverPose_t / IVRDriverInput
        +-- HMD display config -> IVRDisplayComponent
        +-- SteamVR events     -> Core haptics queue
                         |
                         v
                      SteamVR
                         |
             pose/input  |  haptics
                         v     ^
                OpenVR / OpenXR application
```

### Virtual Device Coreの責務

[`devicecore/`](../devicecore/)はRuntime非依存のまま維持する。SteamVR対応のために
`DriverPose_t`、`IVRDisplayComponent`、`IVRDriverInput`などのOpenVR型をCoreへ追加してはならない。

SteamVR Driverは、Monado Adapterと同様に次のCoreソースを自身のRuntime Processへ組み込む。

- `playspectra_proto.{c,h}`: 共通NDJSON Protocol。
- `playspectra_state.{c,h}`: HMD・左右Controllerの共有状態。
- `playspectra_control.{c,h}`: `hello` / `set_state` / `get_state` / `status` / `reset`。
- `ps_os.{c,h}`: Thread・Mutex・SocketのPlatform Shim。

CoreへSteamVR対応を追加するのではなく、**SteamVR Adapterが既存Coreを利用する**依存方向とする。
Runtime固有の不足が実装中に判明した場合も、Coreへ追加できるのはRuntime非依存の状態・能力だけである。
現行CoreがHMDと左右Controllerの固定フィールドを持つことはMVPの制約であり、Adapter全体の恒久的な
Device数制約とはしない。将来の可変台数化でもAdapter -> Coreの依存方向を維持する。

### SteamVR Adapter Shellの責務

- `playspectra_state_get_head`のPoseを`DriverPose_t`へ変換する。
- 左右Controller状態を`DriverPose_t`と`IVRDriverInput`のBool/Scalar Componentへ変換する。
- Device identity、device class、body roleを混同せず、SteamVRのSerial/Class/Role Propertyへ写像する。
- SteamVRのHapticsをCoreのHaptics Eventへ戻す。
- STAGE座標、Quaternion順序、Tracking/Connection状態をSteamVR表現へ変換する。
- HMDのRender Target Size、Eye Viewport、Projection、Distortion、Refresh Rateなどを
  `IVRDisplayComponent`とDevice Propertyとして公開する。
- Driver LifecycleとCore Control Channelの開始・停止順序を所有する。
- SteamVR API呼び出しの例外をDriver境界で封じ、`vrserver.exe`へ伝播させない。

### Control Channel

SteamVR Adapterは[device-core-spec.md](device-core-spec.md)のProtocol Version 1を使用する。
計画上の既定Portは`52701`とし、Monado Adapterの`52702`とは分ける。Go側の操作APIとScenario Schemaは
変更しないが、現行のMonado向け既定値や`PLAYSPECTRA_MONADO_PORT`だけではSteamVRを選択できない。
実装時に接続Endpointを明示指定できるRuntime設定（CLI option、共通環境変数、またはSession設定）を追加し、
CLI・MCP・Scenarioが同じ選択結果を使うことを必須とする。Runtime固有の操作コマンドは追加しない。

現行`driver/src/driver_playspectra.cpp`にある旧Layer Protocol互換のTCP/JSON実装は置き換える。
Driver固有の並行Protocolを維持しない。

## 仮想HMD

仮想HMDは将来拡張ではなくSteamVR Adapterの必須デバイスである。

SteamVR Shellは少なくとも次のContractを担当する。

- `ITrackedDeviceServerDriver`としてHMDを登録・更新する。
- `GetComponent`から`IVRDisplayComponent`を公開する。
- `GetWindowBounds`、`GetRecommendedRenderTargetSize`、`GetEyeOutputViewport`、
  `GetProjectionRaw`、`ComputeDistortion`を仮想Display設定から返す。
- Display Frequency、IPD、Universeなど必要なHMD Propertyを設定する。
- CoreのHead PoseをSteamVR Compositorへ継続して通知する。
- `driver.vrdrivermanifest`にHMD discovery情報を持たせ、SteamVRがHMD未接続状態でも
  PackageをHMD Driver候補として発見できるようにする。現行の空`hmd_presence`は完成条件を満たさない。

Driver Manifest、HMDの`Activate`、`IVRDisplayComponent`は一つのContractとして検証する。
`hmd_presence`の具体値（たとえばSimpleHMD系Driverで使われるwildcard）は、対象SteamVR Versionでの
headless discovery実測をもって確定し、単にManifestへ文字列を追加しただけでは完了としない。

解像度、Refresh Rate、IPD、左右Viewport、Projection/FOV、Distortionは、CoreのRuntime非依存Descriptor
または明示的なAdapter設定から導出する。現行Descriptorで不足するFOV等を追加する場合もOpenVR型は使わず、
MonadoとSteamVRの両Adapterが解釈できるRuntime非依存の値として定義する。

`openvr_driver.h`に`IVRDisplayComponent`が存在すること、およびリポジトリ内のMonado SteamVR Driverに
HMD実装例が存在することは確認済み。ただし、PlaySpectra Driverで物理HMDなしにSteamVR Sessionを開始できるかは
未実測であり、実装完了のWindows E2Eで検証する。

物理DisplayのDirect Modeは初期範囲に含めない。仮想HMDがSteamVR Compositorへ必要なDisplay Contractを提供する
最小経路を先に成立させる。

## 仮想コントローラー

左右ControllerはCoreの`grip` / `aim` / `inputs`をSteamVRへ写像する。現行Skeletonの
`VirtualController`とInput Component生成は参考にできるが、状態・通信の所有権はCoreへ移す。

特定ベンダーProfileへの偽装は完成条件ではない。初期実装では`Prop_InputProfilePath_String`を
Driver Package内の`{playspectra}/input/playspectra_profile.json`へ向け、PlaySpectra所有のInput Profileと
Bindingだけで自己完結させる。現行Skeletonが指定する`oculus_touch`と
`{oculus}/input/touch_profile.json`は実Oculus Driver Resourceへ依存するため置き換える。
既存ゲームの自動Bindingを目的としたベンダー偽装は実測後に別途判断する。

## Hapticsの逆方向経路

Pose、Button、Axisは`Core -> Driver -> SteamVR -> Application`へ流れる。一方Hapticsは
`Application -> SteamVR -> Driver -> Core -> Observer`へ流れる別方向のContractである。

DriverはHaptic Componentを作るだけではなく、`RunFrame`で`PollNextEvent`を処理し、
`VREvent_Input_HapticVibration`を対象の左右Device/Componentへ関連付け、振幅・周波数・継続時間を
Runtime非依存のCore Haptics Eventへ変換する。Coreの既存Observer配信まで到達するE2Eを完成条件に含める。

## 既存Instrumentation Layerとの関係

Runtime AdapterはHMD・Controller入力をSteamVRの正規Device Pathへ供給する。
OpenXR Instrumentation Layerは、対象アプリケーションがOpenXR Loader経路を通る場合のScreenshot、Recording、
Action Discovery、Diagnosticsを引き続き担当する。DriverがCaptureを実装することは本設計の範囲外。

描画観測はApplication APIごとに分ける。

- **SteamVR + native OpenXR**: 既存OpenXR Instrumentation Layerでoperate -> render -> observeを構成する。
  これを初期Adapterの完全E2E対象とする。
- **SteamVR + native OpenVR**: Driver経由の操作到達は検証対象だが、純粋なOpenVR Compositor経路には
  現行OpenXR Layerを挿入できない。描画観測方式は別設計事項であり、未設計の間は「操作のみ」または
  「観測未対応」と明記する。
- **OpenVR + OpenComposite**: OpenXRへ変換される別経路であり、native OpenVRの証拠として扱わない。

## 実装順序

1. 本文書の目的、範囲、Application API別の完了条件を固定する。
2. HMD Class、Properties、`IVRDisplayComponent`、Manifest discoveryを一体で実装する。
3. Controllerを加える前に、物理HMDなしのSteamVR + 仮想HMDだけでRuntime/Application起動を実測する。
4. `devicecore/`を組み込み、Core Head Poseを仮想HMDへ接続する。
5. PlaySpectra所有Controller Input ProfileとBindingをPackageへ組み込む。
6. 左右Controller Pose/Button/Axisを接続する。
7. SteamVR Event PollingとCore Haptics Observerへの逆方向経路を接続する。
8. SteamVR + native OpenXRでoperate -> render -> observe E2Eを実測する。
9. SteamVR + native OpenVRで操作到達を実測し、描画観測を独立した状態として記録する。

Controllerを先に増築せず、HMD-only headless startupを最初のRuntime checkpointとする。
Generic Trackerと可変台数Protocolはこの順序へ含めず、MVP後の独立した実装・検証項目とする。

## 検証と完了条件

SteamVR Adapterを「Verified」へ変更するには、少なくとも次の一次証拠を保存する。

1. Clean Buildで`driver_playspectra.dll`とDriver Packageを生成できる。
2. `vrpathreg`で登録したDriverがSteamVRにロードされる。
3. HMD discoveryを含むManifestから、物理HMDなしで仮想HMDが発見・起動される。
4. HMD-only checkpointの後、仮想HMDと左右Controllerが列挙される。
5. Go Coreから送ったHead Poseがアプリケーションの視点へ到達する。
6. 左右ControllerのPoseと少なくともBool・Scalar入力がアプリケーションへ到達する。
7. Applicationから送ったHapticsがDriverのEvent Pollingを通り、Core Observerへ到達する。
8. `get_state` / `reset` / Writer排他を含む共通ProtocolがSteamVR Adapterでも通る。
9. SteamVR + native OpenXRアプリで入力に応じた描画変化をCapture/assertし、
   operate -> render -> observeを一周させる。
10. SteamVR + native OpenVRアプリで入力到達を確認し、描画観測は可否と経路を別欄で記録する。
11. 再現コマンド、対象SteamVR Version、対象アプリ、Application API、結果を
    [verification.md](verification.md)へ記録する。

`SteamVR Adapter: Verified`は1〜9を必須とする。10のnative OpenVR操作到達はSteamVR互換性の
独立した受け入れ項目であり、native OpenVR描画観測が未設計である間は完全な
operate -> render -> observe対応を主張しない。

物理HMDとの共存、全ゲームへの自動Binding、録画、完全なFrame Determinismはこの判定に含めない。
Generic Trackerの列挙、body role割当、3台以上のControllerも初期`Verified`判定には含めない。

## 現在の既知リスク

- **仮想DisplayでのSteamVR起動**: 必要Interfaceはソース上確認済みだが、PlaySpectra構成では未実測。
- **HMD Discovery**: 現行Manifestの`hmd_presence`は空であり、物理HMDなしのDriver discoveryを満たさない。
- **OpenVR Header Version**: 現行Skeletonはv1.8.19を前提とする。現行SteamVRとの互換性は実測が必要。
- **Controller Binding**: 同梱Profileでのアプリ側Binding手順と自動化範囲は未確定。Oculus Resource依存は除去する。
- **native OpenVR Observation**: 現行OpenXR Layerでは純粋なOpenVR描画を観測できず、別経路の設計が必要。
- **Identity/Role Mapping**: 可変台数化では安定identityと再割当可能なbody roleを分離し、
  SteamVR固有PropertyをCore Protocolへ漏らさない設計が必要。
- **Driver Process Safety**: Driverは`vrserver.exe`内で動くため、例外・Thread終了・Socket停止の不備は
  SteamVR Session全体へ影響する。
- **Display Parameters**: 初期値の解像度、FOV、IPD、Refresh Rateは決定ではなく、検証可能な設定値として
  明示してから実装する必要がある。

## 後続候補: 物理デバイスとの共存

実機Controllerの役割割当、`GetRawTrackedDevicePoses`によるPose Mirror、Input Mux、特定Controllerへの偽装は
後続テーマである。過去のH1〜H3仮説は、共存機能に着手する場合に改めて実測し、CoreのSteamVR Adapter完成条件へ
混ぜない。
