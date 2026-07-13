# VR-MCP

VRアプリケーション開発時に、AIエージェントが「ブラウザに対する Playwright」のように
VRアプリを **操作（入力注入）** し **観察（画面キャプチャ）** するための MCP サーバー + OpenXR API レイヤー。

- VRコントローラー（トリガー / グリップ / スティック / ボタン）と HMD の姿勢をプログラム的に注入
- 画面の状況を画像として取得（既定は利き目カラー画像。深度マップ・左右ステレオも選択可能）
- エンジン非依存（OpenXR API レイヤー方式なので、Unity / Unreal / Godot / ネイティブ OpenXR アプリを問わない）
- 実機 HMD 不要の完全ヘッドレス / シミュレーション動作（Meta XR Simulator）

Playwright がブラウザに合成入力を注入して DOM / スクリーンショットを観察するのと同じ層に位置する。
VRアプリが共通で読む「入力キュー + フレームバッファ」に相当する OpenXR の抽象化レベルに介入するため、
個々のエンジン固有プラグインは書かない。

## 構成

```
mcp/            TypeScript MCP サーバー（stdio。19 個の vr_* ツールを提供）
layer/          C++ OpenXR API レイヤー vr_agent_layer.dll (CMake + MinGW)
  src/          レイヤー本体・制御チャネル・キャプチャ・各グラフィックスAPIバックエンド
  manifest/     XrApiLayer_vr_agent.json（ローダが読むレイヤー manifest）
scripts/        セットアップ・起動補助スクリプト
third_party/    FetchContent / 外部ランタイム（Meta XR Simulator 等）の取得先（非コミット）
```

MCP サーバー（`mcp/`）は stdio でエージェントに接続し、localhost TCP（NDJSON）で
レイヤー（`layer/`）の制御チャネルに接続する。レイヤーは対象VRアプリのプロセス内に
OpenXR API レイヤーとしてロードされ、`xrSyncActions` / `xrLocateViews` / `xrEndFrame` などを
フックして入力注入と画面キャプチャを行う。

---

## ⚠ 現状の制約 / コア必須の残タスク

VR版Playwrightとして「全キー入力できる」状態には、グラフィックスAPIの完全性
（D3D11 / D3D12 / Vulkan の3API対応）が **コア必須** である。現状は以下が未達で、対象アプリの
描画APIによっては撮影できない。いずれも「無言で壊れた画像」を返さず **明示的にエラーを返す**。

- **D3D11 / D3D12 バックエンドは未実装（コア必須の残タスク）**
  セッションの `XrGraphicsBinding{D3D11,D3D12}KHR` は検出・追跡しているが、色読み戻し本体
  （`D3D11ReadbackToPng` / `D3D12ReadbackToPng`）はスキャフォールドで、呼ぶと
  `"... capture backend not implemented yet (core-required follow-on)"` を返す。
  D3D11 / D3D12 で描画するVRアプリ（Unreal 既定など）は現状 `vr_screenshot` できない。
  （`layer/src/capture_d3d11.cpp` / `capture_d3d12.cpp` はビルド対象。ファイルが存在すること＝実装済み、ではない。）
- **Vulkan バックエンドは実装済みだが以下は未対応でエラー**
  - **MSAA（`sampleCount > 1`）の resolve 未実装** — マルチサンプルのスワップチェーンは
    `"Vulkan MSAA (sampleCount>1) resolve not implemented yet"` を返す。
  - **8bit-RGBA/BGRA 以外（HDR / `RGBA16F` 等）未対応** — 対応フォーマットは
    `R8G8B8A8_UNORM/SRGB` と `B8G8R8A8_UNORM/SRGB` のみ。それ以外は
    `"unsupported Vulkan color format ..."` を返す。
- **深度マップは nice-to-have・Vulkan 専用**
  深度は必須仕様ではない（既定は利き目カラーのみ）。アプリが `XrCompositionLayerDepthInfoKHR` を
  提出したフレームでのみ取得でき、提出が無ければ `depth.available = false` でカラーのみ動作する。
  深度の読み戻しは Vulkan バックエンドのみ実装。

> 対応の目標は D3D11 / D3D12 / Vulkan の3API（`XrGraphicsBindingD3D11KHR` /
> `...D3D12KHR` / `...VulkanKHR` はいずれも OpenXR 自身が定義する構造体であり、エンジン固有ではない）。
> Vulkan の MSAA / HDR 対応も含めて本セッションで実装を進めている。

---

## 前提

- Windows 11（本体は Win32 / WinSock を使用）
- MinGW-w64（レイヤーのビルド）+ CMake ≥ 3.21
- Node.js（MCP サーバー。`@modelcontextprotocol/sdk` / `zod`）
- 実行時 `vulkan-1.dll`（Vulkan ランタイム。レイヤーは `LoadLibrary` で動的ロードし、リンクはしない）
- Meta XR Simulator v201.0（ヘッドレスの基盤 OpenXR ランタイム）

## セットアップ

### 1. 基盤ランタイム: Meta XR Simulator の抽出

Meta XR Simulator を（`msiexec /a` などで昇格なしに）展開し、以下に配置する。

```
third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json
```

このランタイム JSON を `XR_RUNTIME_JSON` で指定する（`scripts/env.sh` が設定する値）。

```bash
export XR_RUNTIME_JSON="$PWD/third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json"
```

> `scripts/setup.ps1` は現状スタブ（案内メッセージのみ）で、抽出・登録は自動化されていない。
> `scripts/env.sh` は `XR_RUNTIME_JSON` / `XR_API_LAYER_PATH` / `XR_ENABLE_API_LAYERS` を
> まとめて設定する実働スクリプトなので、bash 環境ではこれを `source` するのが早い。

#### ヘッドレス / バッチ動作

Meta XR Simulator は既定でフロントエンド UI（`MetaXRSimulator.exe`）に接続しにいく。
完全ヘッドレス化には **バッチモード** を使う。

- `META_XRSIM_CONFIG_JSON`（環境変数）: ランタイムが読むコア設定 JSON の絶対パス。
  既定は `config/sim_core_configuration.json`。`use_batch_mode: true` が入った
  `config/sim_core_configuration_ci.json` が同梱されている（これを指すとバッチモード）。
- `use_batch_mode: true` = フロントエンド UI を起動せず gRPC スタンドアロン動作。
- 既定設定には `use_offscreen_mode: true` / `device_profile: "Meta Quest 3"` 等も含まれる。

### 2. vr_agent API レイヤーの登録

ローダは manifest（`layer/manifest/XrApiLayer_vr_agent.json`）を探索してレイヤーを見つける。
manifest の `name` は `XR_APILAYER_vr_agent`、`library_path` は `./vr_agent_layer.dll`（相対）。
ビルド済み `vr_agent_layer.dll` を **manifest と同じディレクトリ**（`layer/manifest/`）に置く。

```bash
export XR_API_LAYER_PATH="$PWD/layer/manifest"      # manifest 探索ディレクトリ
export XR_ENABLE_API_LAYERS="XR_APILAYER_vr_agent"  # 明示的に有効化するレイヤー名
```

これでローダは対象VRアプリのプロセスに `vr_agent_layer.dll` を挿入する。

## ビルド

### レイヤー（C++ / CMake + MinGW）

`OpenXR-SDK`（ヘッダのみ）・`lodepng`・`nlohmann_json` は CMake の FetchContent で自動取得する。
`Vulkan-Headers` は `third_party/Vulkan-Headers/include` を参照する（配置が必要）。

```bash
cd layer
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
# => build/vr_agent_layer.dll （PREFIX 無し）を layer/manifest/ にコピーする
```

### MCP サーバー（TypeScript）

```bash
cd mcp
npm install
npm run build        # tsc -p tsconfig.json → dist/server.js
npm start            # node dist/server.js（本番）
# または開発時: npm run dev（tsx で src/server.ts を直接実行）
```

## 実行時の環境変数

| 変数 | 情報源 | 既定 | 説明 |
| --- | --- | --- | --- |
| `VR_AGENT_PORT` | `control_channel.cpp` | `52700` | レイヤーの制御チャネル TCP ポート（`127.0.0.1`）。**MCP サーバーがレイヤーへ接続する先**。MCP 側でも `VR_AGENT_PORT` を読むので、レイヤー側と一致させること。 |
| `VR_AGENT_LOG` | `openxr_agent_layer.cpp` | `%TEMP%\vr_agent_layer.log` | レイヤーのログ出力先ファイルパス。 |
| `VR_AGENT_NO_CA` | `openxr_agent_layer.cpp` | 未設定（＝有効） | **設定されている（値は問わない）と** `XR_EXT_conformance_automation` の自動注入を無効化し、入力注入を止める。診断や、CA 有効化で挙動が変わるランタイム向けのエスケープハッチ。 |
| `VR_AGENT_CAPTURE_DIR` | `capture.cpp` | `%TEMP%`（無ければ `%TMP%`→カレント） | スクリーンショット / 深度 PNG の出力ディレクトリ。 |
| `VR_AGENT_DOMINANT_EYE` | `capture.cpp` | `right` | `eye:"dominant"` 指定時に使う利き目。`"left"` で左目、それ以外は右目。 |
| `XR_RUNTIME_JSON` | OpenXR ローダ標準 | — | 使用する OpenXR ランタイムの JSON（Meta XR Simulator の `meta_openxr_simulator.json`）。 |
| `XR_API_LAYER_PATH` | OpenXR ローダ標準 | — | API レイヤー manifest の探索ディレクトリ（`layer/manifest`）。 |
| `XR_ENABLE_API_LAYERS` | OpenXR ローダ標準 | — | 明示的に有効化するレイヤー名。`XR_APILAYER_vr_agent` を指定。 |
| `META_XRSIM_CONFIG_JSON` | Meta XR Simulator | — | Meta XR Simulator のコア設定 JSON（バッチモード等）。ヘッドレス化に使用。 |

## 使い方（基本フロー）

1. 上記の環境変数を設定（`XR_RUNTIME_JSON` / `XR_API_LAYER_PATH` /
   `XR_ENABLE_API_LAYERS=XR_APILAYER_vr_agent`、必要なら `VR_AGENT_PORT` など）。
2. **対象VRアプリを起動**する（同じ環境変数を引き継いだシェルから）。ローダが `vr_agent_layer.dll` を
   プロセスに挿入し、レイヤーが `127.0.0.1:VR_AGENT_PORT`（既定 52700）で制御チャネルを開く。
3. **MCP サーバーを起動**（`npm start`）してエージェントに接続する。サーバーは `VR_AGENT_PORT` へ
   遅延接続する（アプリ未起動なら「接続できない」旨を構造化して返す）。
4. エージェントからツールを呼ぶ:
   - `vr_status` — セッション / `XR_EXT_conformance_automation` の有無 / ランタイム名を確認。
   - `vr_actions` — アプリが登録したアクション名と束縛パスを取得し、名前で入力を駆動。
   - `vr_input` / `vr_click` — コントローラー入力を注入（`xrSyncActions` に合わせて反映）。
   - `vr_set_hmd` / `vr_set_controller` / `vr_look_at` / `vr_point_at` — 頭部・コントローラー姿勢を注入。
   - `vr_wait` — 注入を反映させるため N フレーム描画を待つ。
   - `vr_screenshot`（`eye` / `withDepth`）— レンダリング結果を PNG で取得。
   - `vr_view` — 視点姿勢 + FOV を取得（画面ピクセル ↔ ワールド座標の相互変換に使う）。

### `vr_screenshot` の `eye` / `withDepth`

- `eye`: `left` / `right` / `dominant`（既定。既定の利き目は右、`VR_AGENT_DOMINANT_EYE` で変更）/
  `both`。`both` は左右を返すが **別フレームで撮影**するため、動く場面では視差だけでなく動きの分だけ差が出る。
- `withDepth: true`: アプリが `XrCompositionLayerDepthInfoKHR` を提出していれば 16bit グレースケール
  深度マップ（近＝黒、遠＝白）+ メタデータを返す。提出が無ければ `depth.available = false`（カラーのみ）。

---

## MCP ツール一覧（19）

| ツール | 概要 | 主な入力 |
| --- | --- | --- |
| `vr_status` | vr_agent レイヤーの状態（インスタンス / セッションの有無、`XR_EXT_conformance_automation` の可否、ランタイム名、ハプティクス数）を返す | — |
| `vr_input` | コントローラー入力値を注入（`XR_EXT_conformance_automation` 経由で次の `xrSyncActions` に反映） | `hand`, `input`, `type`(float/bool/vec2), `value`/`x`/`y` |
| `vr_active` | コントローラーの接続 / 切断状態を通知（既定プロファイルは Oculus Touch） | `hand`, `active?`, `profile?` |
| `vr_screenshot` | レンダリング結果を PNG で取得（+ メタデータ。任意で深度） | `eye?`(left/right/dominant/both), `timeoutMs?`, `withDepth?` |
| `vr_set_controller` | コントローラーの姿勢（位置 + 向き）を LOCAL 空間で注入。クリアまで保持 | `hand`, `x`,`y`,`z`, `yaw/pitch/roll?` または `qx..qw?` |
| `vr_clear_controller` | コントローラー姿勢の上書きを解除（ランタイムの姿勢に戻る） | `hand` |
| `vr_set_hmd` | 視点（頭部姿勢）を LOCAL 空間で上書き（IPD / FOV は維持）。リセットまで保持 | `x?`,`y?`,`z?`, `yaw/pitch/roll?` または `qx..qw?` |
| `vr_clear_hmd` | 視点の上書きを解除（ランタイムのヘッドトラッキングに戻る） | — |
| `vr_move` | 頭部 / コントローラーを現在の注入姿勢から相対移動（向きは維持） | `target`(head/left/right), `dx`,`dy`,`dz` |
| `vr_reset` | すべての姿勢上書き（コントローラー + 頭部）を破棄。入力値には影響しない | — |
| `vr_recenter` | 視点を原点・正面に固定（＝引数なしの `vr_set_hmd`。ヘッドトラッキングは止まる） | — |
| `vr_point_at` | コントローラーのグリップ姿勢の前方(-Z)をワールド点に向ける（位置は維持） | `hand`, `tx`,`ty`,`tz`, `fx?`,`fy?`,`fz?` |
| `vr_look_at` | 視点をワールド点に向ける（頭部位置は維持。視点は固定される） | `tx`,`ty`,`tz`, `fx?`,`fy?`,`fz?` |
| `vr_click` | 入力を押下値にし `holdMs` 待ってから解放（ボタン / トリガー） | `hand`, `input`, `type`(bool/float), `value?`, `holdMs?` |
| `vr_wait` | アプリが N フレーム描画するまで（またはタイムアウトまで）待つ | `frames`, `timeoutMs?` |
| `vr_haptics` | アプリが要求した直近のハプティクスパルスを返す（入力が届いた確認信号） | `limit?` |
| `vr_capture_sequence` | `count` 枚を `intervalMs` 間隔で撮影し画像列を返す（動きの確認用） | `count`, `intervalMs`, `eye?` |
| `vr_view` | 直近 `xrLocateViews` の視点姿勢 + 投影 FOV を返す（画面ピクセル ↔ ワールド座標の橋渡し） | — |
| `vr_actions` | アプリが登録したアクションセット / アクションと束縛された相互作用プロファイルパスを列挙（名前で入力駆動） | — |

---

## エンジン別: 深度サブミッションの有効化（nice-to-have・**未検証 / best-effort**）

深度マップは nice-to-have であり、コアループ（入力注入 + カラーキャプチャ）には不要。
`vr_screenshot({withDepth:true})` は、アプリが `XrCompositionLayerDepthInfoKHR` を提出したフレームで
のみ深度を返す。多くのエンジンは既定で提出しないため、深度が欲しい場合のみ以下を有効化する。
いずれの手順も **未検証（best-effort、要検証）** であり、エンジンのバージョンで異なりうる。
（深度は Vulkan バックエンドでのみ読み戻せる点にも注意。）

- **Unity**（OpenXR Plugin）: Project Settings → XR Plug-in Management → OpenXR で
  "Depth Submission Mode"（Depth 16/24 bit）を有効化する系の設定。
- **Unreal Engine**（OpenXR）: OpenXR プラグイン / プロジェクト設定で深度レイヤー提出
  （composition layer depth）を有効化する。※ Unreal 既定は D3D11/D3D12 描画のため、
  カラーキャプチャ自体が上記「D3D11/D3D12 未実装」に該当する点に留意。
- **Godot**（OpenXR）: XR / OpenXR の設定で "Submit Depth Buffer" を有効化する。

> これらはエンジン固有の設定であり、レイヤー本体（OpenXR 抽象化レベル）の対応範囲ではない。
> レイヤーはあくまで `XrCompositionLayerDepthInfoKHR` が提出されれば読み戻す、という普遍的な挙動に留まる。
