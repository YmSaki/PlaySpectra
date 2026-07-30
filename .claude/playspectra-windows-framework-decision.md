# 決定お膳立て: PlaySpectra フレームワークを Windows で動かす

> **✅ 解決済(2026-07-22)**: ユーザー判断で **案 A を実行→達成**。Monado(PlaySpectra driver)を Windows ビルド成功、
> `monado-service` の :52702 に対し Server/wait_for/CLI が **9/9 動作**（実GPU）。真因=MSVC の UTF-8/CP932 誤読、
> 修正=`/utf-8`。詳細と再現手順は memory [[windows-monado-unified-stack]]。以下は当時の判断材料（履歴）。
>
> **✅ 実アプリ結合フロンティアも解決(2026-07-22)**: `openxr_monado` ターゲットを追加ビルド→hello_xr が Windows
> Monado(:52702) に IPC 接続→D3D11 実描画→capture(:52700) が 20/20 観測（`scripts/run_hello_xr_monado.sh`）。
> Q1(Windows IPC 機能)/Q2(実アプリ capture) とも YES。null compositor headless。表示 compositor 経路のみ未検証。

作成: 2026-07-22（autonomous loop 中・**実装せず判断材料のみ**）。approach-gate 文書。
正典 [.claude/playspectra-architecture.md] と規律（検証済み/未検証を混ぜない）に従う。

## ギャップ（事実）

- ✅ **観察(capture)は Windows・実 GPU で完結**: `integration_test.sh {D3D11,D3D12,Vulkan}` 各 20/20 = 60/60（2026-07-22）。Layer :52700。
- ✅ **改革後の上位スタック（Server / Scenario / MCP / wait_for）は Monado アダプタ `:52702` にしか喋らない**（`tools/playspectra_*.py`）。
- ✅ **`:52702` は WSL2 でのみ検証**（Monado ビルドに glslang/Vulkan SDK 必要）。Windows には未展開。
- ⇒ **Windows で Playwright テスト FW（wait_for/シナリオ/CLI/MCP）を実アプリに使う手段が無い**。これが最大の全体最適ギャップ。

## 2チャネルの能力（grep 実測 2026-07-22）

| | `:52702` Monado アダプタ | `:52700` Layer CA |
|---|---|---|
| operate | `set_state`（**全デバイス状態の完全スナップショット**：head+L/R grip/aim+inputs） | `active`(プロファイル) / `input`(アクション値注入) / `pose`,`head`(姿勢override) / `haptics` / `reset` |
| observe | `get_state`（**権威的 VirtualDeviceState 全体**） | `status` / `view`(実レンダリング視点pose+FOV) / `actions`(登録アクション) / `screenshot` / `pose_get`,`head_get`(注入echo) |
| 注入モデル | Server がモデルを所有し完全スナップショットを流す | アプリ登録アクションへ末尾一致で per-action 注入（CA=conformance_automation） |
| 検証環境 | WSL2 のみ | **Windows で実測済み（今日 60/60）** |

**観測パリティの核心的差**: `:52700` は `:52702` の `get_state`（全デバイス状態）に相当するものを持たない。
Server の assert（`["hmd","head","position",2]` 等の状態パス比較）は CA 経路に**そのまま写らない**。
CA での assert 対象は `view`（実レンダリング視点）/ `screenshot`(pixels) / 注入 echo になる
— これは「注入した状態」でなく「アプリが描いた結果」を検証する形で、**実は本物の Playwright に近い**
（Playwright も DOM/pixels を見る）が、assert の**種類が変わる**。

## 選択肢

### A. Monado を Windows ビルド（`:52702` を Windows に）★ユーザー選好・de-risk 済み(2026-07-22)
- ✅ 上位スタックが無改造でそのまま動く（統一スタック・get_state 完全）。実 GPU・実 D3D アプリも可能（Monado の XRT_HAVE_D3D11/D3D12）。
- ✅ **PlaySpectra ドライバー自体は Windows ビルド可能を静的検証済み**（2026-07-22）:
  - socket = `#ifdef _WIN32`→winsock2/ws2tcpip・`ps_socket_t`/`ps_close_socket` 抽象・WSAStartup ガード（`playspectra_control.c:34-47,822`）
  - `drivers/CMakeLists.txt:94-96` `if(WIN32) target_link_libraries(drv_playspectra PRIVATE ws2_32)`（drv_remote と同型の実績パターン）
  - スレッドは Monado 標準 `os_thread_helper`（クロスプラットフォーム）
- ✅ **stock Windows Monado が現存**（`third_party/monado/bin/monado-service.exe`・PoC CI artifact）＝Monado は Windows でビルド実績あり。
- ⚠️ **唯一のゲート = Vulkan SDK 未インストール**（glslang + Vulkan dev files。glslang はシステム全滅を確認）。要システム変更（ユーザーがインストーラ実行）。
- 🔬 残存リスクは「Monado *本体* の Windows/MSVC フルビルドが glslang 以外で詰まらないか」のみ（driver は上記で潰した）。stock CI artifact の存在で見込み高いが、実ビルドで答え合わせ。
- **submodule ソースは Windows 作業ツリーに在り**（`runtime/monado-playspectra/`）・**Windows では未 configure**（build/ に CMakeCache 無し）。
- **実行手順（SDK 導入後）**:
  ```
  # VULKAN_SDK 設定 & glslangValidator が PATH に入った新シェルで
  cmake -S runtime/monado-playspectra -B runtime/monado-playspectra/build-win \
        -G "Visual Studio 17 2022" -DXRT_BUILD_DRIVER_PLAYSPECTRA=ON
  cmake --build runtime/monado-playspectra/build-win --config Release
  # 機械判定: PLAYSPECTRA_ENABLE=1 <build>/monado-service.exe で 'PlaySpectra HMD' (head role) 列挙・:52702 listen
  ```

### B. Server に CA アダプタを足す（`:52700` でも operate/observe）
- 実装: Server の Runtime 抽象に「Layer-CA バックエンド」を追加。高水準命令 → `active`+`input`+`head`/`pose`。観測 → `view`/`screenshot`/`actions`。
- ✅ **システム変更不要**。今日 Windows で検証済みの CA 経路を再利用。memory [[layer-observe-driver-inject]]「layer は**自動テスト以外**観測専用」の**自動テスト例外に該当**（＝新規アーキ決定でなく既存パターンの集約）。`integration_hello_xr.mjs` が既にこのパターンで動いている。
- ⚠️ **観測パリティ不整合**（上記）: `assert_state` は全デバイス状態でなく `view`/`screenshot` ベースへ再マップが要る。assert 語彙の一部が CA では別意味/未対応になる（設計判断）。
- △ 中規模実装。get_state 互換の「疑似 state」を view+注入echo から組むか、assert 語彙を capture 主体へ寄せるかの設計判断込み。

### C. 最小: 既存 Windows CA クライアントに auto-wait だけ足す
- `integration_hello_xr.mjs`（または新規小 client）に wait_for/retry-assert 相当を追加。統一せず Windows テストに auto-wait の恩恵だけ与える。
- ✅ 小さく verifiable。⚠️ ロジック二重化（Server と別実装）。統一はしない。

## 推奨（要ユーザー決定）

- **中長期の統一を重視するなら B**（CA アダプタ）。システム変更不要・検証済み経路の再利用・自動テスト例外に合致。ただし**「Windows での assert は全デバイス状態でなく view/screenshot ベースになる」トレードオフを受け入れるか**が判断点。
- **上位スタックを無改造で使いたい & Monado を Windows でも持ちたいなら A**。ただし要システム変更＋ビルド成否未検証。
- **今すぐ Windows テストに auto-wait だけ欲しいなら C**（つなぎ）。

**独断で着手しない理由**: B/A とも「観測パリティの扱い」「システム変更」という**破壊的でない一意には決まらない設計判断**を含む。
CLAUDE.md の over-scope 戒め（局所最適が全体を侵す）に照らし、方向決定は user に留保。実装はどれも本環境で verifiable。

## 実装せずに確定済みの事実（この doc の根拠）
- CA コマンド面: `layer/src/control_channel.cpp` ディスパッチ表（grep 実測）。
- Windows capture 60/60: `scripts/integration_test.sh`（2026-07-22 実行ログ）。
- 上位スタックの接続先: `tools/playspectra_mcp.py`(operate→:52702/capture→:52700)、`tools/playspectra_server.py`。
