# VR-MCP

> **VRアプリ版の Playwright。** AIエージェントが VRアプリを *操作*（入力注入）し *観察*（画面キャプチャ）するための、MCP サーバー + OpenXR API レイヤー。

Playwright がブラウザに合成入力を注入して DOM / スクリーンショットを観察するのと同じことを、VRアプリに対して行う。VRコントローラーと HMD を「触ったのと同じ入力」をVRアプリに注入し、レンダリング結果を画像として取得できる。

実機 HMD なしの完全ヘッドレスで動作し（Meta XR Simulator）、**エンジン非依存**（OpenXR API レイヤー方式なので Unity / Unreal / Godot / ネイティブ OpenXR アプリを問わない）。

---

## 特徴

- 🎮 **入力注入** — コントローラー（トリガー / グリップ / スティック / ボタン）と HMD・コントローラーの姿勢をプログラム的に注入
- 📷 **画面観察** — レンダリング結果を PNG で取得（既定は利き目カラー。深度マップ・左右ステレオも選択可）
- 🧩 **エンジン非依存** — OpenXR の「入力キュー + フレームバッファ」抽象に介入するため、エンジン固有プラグインを書かない
- 🖥 **ヘッドレス** — 実機不要。Meta XR Simulator 上でシミュレーション動作
- 🛠 **19 の MCP ツール** — `vr_status` / `vr_input` / `vr_screenshot` / `vr_set_hmd` / `vr_look_at` など

## 仕組み

```
AIエージェント ──(MCP/stdio)──> MCPサーバー(mcp/) ──(localhost TCP)──> 制御チャネル
                                                                          │
対象VRアプリ ──OpenXR呼び出し──> vr_agent_layer.dll(layer/) ─────────────┘
                                     │ xrSyncActions / xrLocateViews / xrEndFrame をフック
                                     ▼
                                OpenXR ランタイム（Meta XR Simulator）
```

レイヤーは OpenXR ローダによって対象VRアプリのプロセスに注入され、入力注入と画面キャプチャを行う。MCP サーバーはエージェントと stdio で、レイヤーとは localhost TCP（NDJSON）で接続する。

## 構成

```
mcp/          TypeScript MCP サーバー（19 個の vr_* ツールを提供）
layer/        C++ OpenXR API レイヤー vr_agent_layer.dll（CMake + MinGW）
  src/          レイヤー本体・制御チャネル・キャプチャ・各グラフィックスAPIバックエンド
  manifest/     XrApiLayer_vr_agent.json（ローダが読むレイヤー manifest）
scripts/      セットアップ・起動補助・統合テスト
third_party/  外部ランタイム / FetchContent 取得先（非コミット）
```

## Getting Started

### 前提

- Windows 11、MinGW-w64 + CMake ≥ 3.21、Node.js
- `vulkan-1.dll`（レイヤーは動的ロード、リンクはしない）
- Meta XR Simulator v201.0（ヘッドレスの基盤 OpenXR ランタイム）

### ビルド

```bash
# レイヤー（C++）— OpenXR-SDK / lodepng / nlohmann_json は FetchContent で自動取得
cd layer
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
# build/vr_agent_layer.dll を layer/manifest/ にコピーする

# MCP サーバー（TypeScript）
cd mcp
npm install
npm run build
```

### 実行

```bash
# 環境変数をまとめて設定（Meta XR Sim ランタイム + レイヤー有効化）
source scripts/env.sh

# 1. 対象VRアプリを起動 → レイヤーが 127.0.0.1:52700 で制御チャネルを開く
# 2. MCP サーバーを起動してエージェントに接続
cd mcp && npm start
```

主要な環境変数（詳細は `scripts/env.sh`）:

| 変数 | 既定 | 説明 |
| --- | --- | --- |
| `XR_RUNTIME_JSON` | — | 使用する OpenXR ランタイム（Meta XR Simulator の JSON） |
| `XR_API_LAYER_PATH` | — | レイヤー manifest 探索ディレクトリ（`layer/manifest`） |
| `XR_ENABLE_API_LAYERS` | — | `XR_APILAYER_vr_agent` |
| `VR_AGENT_PORT` | `52700` | 制御チャネルの TCP ポート（レイヤー ↔ MCP で一致させる） |
| `VR_AGENT_CAPTURE_DIR` | `%TEMP%` | スクリーンショット / 深度 PNG の出力先 |

## 使い方

エージェントから MCP ツールを呼ぶ。基本フロー:

1. `vr_status` — セッション / CA 拡張の有無 / ランタイム名を確認
2. `vr_actions` — アプリが登録したアクション名と束縛パスを取得
3. `vr_input` / `vr_click` — コントローラー入力を注入
4. `vr_set_hmd` / `vr_set_controller` / `vr_look_at` / `vr_point_at` — 頭部・コントローラー姿勢を注入
5. `vr_wait` — 注入反映のため N フレーム待つ
6. `vr_screenshot`（`eye` / `withDepth`）— 結果を PNG で取得
7. `vr_view` — 視点姿勢 + FOV を取得（画面ピクセル ↔ ワールド座標の橋渡し）

## 対応状況

| 領域 | 状況 |
| --- | --- |
| 入力注入 | ✅ 全経路（全アクション種別・両手・頭部/コントローラー姿勢） |
| キャプチャ: Vulkan | ✅ RGBA8/BGRA8/HDR、MSAA resolve、深度 |
| キャプチャ: D3D11 / D3D12 | 🟡 8bit RGBA/BGRA 実装済み。MSAA / HDR / TYPELESS は明示エラー（コア必須の対応を進行中） |
| 深度マップ | ➖ nice-to-have・Vulkan 専用（`XrCompositionLayerDepthInfoKHR` 提出フレームのみ） |

> グラフィックスAPIの完全性（D3D11 / D3D12 / Vulkan）は「VR版Playwrightが全キーを打てる」ためのコア必須要件。いずれのバックエンドも未対応フォーマットでは「無言で壊れた画像」を返さず、必ず明示的にエラーを返す。

## テスト

```bash
scripts/integration_test.sh Vulkan   # hello_xr 実アプリ + Meta XR Sim でエンジン非依存面を検証
```
