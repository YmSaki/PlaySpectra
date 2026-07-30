# PlaySpectra アーキテクチャ設計 (正典)

決定日: 2026-07-19。ユーザー提示の「大改革」資料＋同日の4点補正を反映。
本文書は PlaySpectra 体系の**正典 (canonical)** であり、既存の
`steamvr-driver-plan.md` / `monado-poc-plan.md` / `openvr-milestone-plan.md` を
本文書の下に位置づけ直す(§9)。CLAUDE.md の north star (「VR版Playwright」) と
検証規律 (「検証済みと未検証を混ぜない」) はそのまま上位に効く。

---

## 0. この文書の位置づけ

- 実装の詳細ステップは各サブ計画/セッション plan に置く。ここは**構造と方針**を定義する。
- 主張には検証境界ラベルを付ける: ✅実装+自動テスト済み / 🟡実機で部分検証済み /
  📋設計・開発中 / 🔬仮説(検証法付き)。ラベルなしの断定を置かない。

---

## 1. PlaySpectra とは (再定義)

PlaySpectra = **XRアプリを操作・観察・記録・再生・検証する自動化基盤**。
旧名 `VR-MCP` は「VRをMCPで触る実装都合の名前」であり本質を表さないため廃止する
(内部識別子の改名は §7、リポジトリ名は当面据え置き=ユーザー決定 2026-07-19)。

MCP は製品本体ではなく、**複数ある操作インターフェースの1つ**。同じ基盤の上に
CLI・JSON シナリオ実行・録画・再生・自動テストが乗る。

目標(Playwright 相当): 操作する / 観察する / シナリオを記録する / 同じ操作を再生する /
自動テストする / 失敗原因を追跡する。ただし Playwright の名前や内部構造の模倣が目的ではない。

---

## 2. アーキテクチャ層

```
  ┌─ 操作インターフェース ────────────────────────────┐
  │   MCP  /  CLI  /  JSON Scenario Runner            │
  └───────────────────────┬──────────────────────────┘
                          ▼
                 PlaySpectra Server
        (高水準命令 → デバイス状態へ解釈・補間)
                          ▼
              Virtual Device Core
          (Runtime非依存の VirtualDeviceState)
                          ▼
        ┌──────── Device Backend (Runtime Adapter) ────────┐
        │   Monado Adapter        SteamVR Adapter          │
        │   (OpenXR/headless/CI)  (SteamVR実ゲーム)         │
        └───────────┬───────────────────┬──────────────────┘
                    ▼                   ▼
                 Monado             SteamVR Runtime
                    ▼                   ▼
             OpenXR アプリ        OpenVR/OpenXR アプリ

  ── Instrumentation (別軸・Device Backend ではない) ──
     OpenXR API Layer:  screenshot / recording / trace /
                        action discovery / diagnostics /
                        (自動テスト用の override のみ)
```

**層の責務**:

- **操作インターフェース**: 人間/AI/シナリオからの入口。互いに対等で、どれも Server を叩く。
- **PlaySpectra Server**: `walk_forward` → 左スティック `[0,1]` 列、`look_at` → 四元数列、の
  ような高水準命令の解釈・補間をここで行う。Driver は高水準命令を理解しない(§6)。
- **Virtual Device Core**: Runtime 非依存の仮想デバイス状態 `VirtualDeviceState`。
  MCP/シナリオは Monado 固有型や SteamVR 固有型を直接触らない。Adapter が各 Runtime の型
  (`xrt_space_relation` / `DriverPose_t` / `XrPosef`) へ変換する。上位機能が増えても
  Adapter を頻繁に触らずに済む、が設計意図。
- **Device Backend (Runtime Adapter)**: 仮想デバイス状態を各 Runtime の正規デバイス経路へ公開する。
  Monado Adapter と SteamVR Adapter。**「どれか一方」ではなく複数並立**。
- **Instrumentation (OpenXR API Layer)**: 【補正 2026-07-19】Layer は Monado/SteamVR と
  **同種の Device Backend として扱わない**。観測・キャプチャ・診断・テスト補助を担う**別軸**。
  入力注入は自動テスト用 override に限定し、本番の入力経路にはしない。

### 【重要な補正】共通汎用 Driver ABI は存在しない

OpenXR が標準化しているのは「アプリ ⇔ XR Runtime」の間だけで、その下の
「Runtime ⇔ デバイス Driver」は Runtime ごとに固有。よって**単一の Driver DLL が
SteamVR/Monado/Meta/PICO すべてに刺さる構造にはならない**。必要なのは
「共通デバイスモデル (Virtual Device Core) + Runtime 別 Adapter」。この分割が §2 の骨格。

---

## 3. 検証境界の規律 (CLAUDE.md「検証済みと未検証を混ぜない」の本タスク適用)

### 3.1 事実 (このリポジトリ内の実測ログ/一次ソースを指せる)

- ✅ 現行 `driver/src/driver_playspectra.cpp`(旧 driver_vragent.cpp・M0.5 改名済) は**キャプチャに触れていない**(仮想HMD無し。
  コメント「Capture/head-override stay in the layer」)。→ ドライバ経路単独では映像取得が未達。
- 🟡 `{oculus}` 偽装パスは**実 oculus ドライバが同居している時だけ解決**する(vrserver ログで確認)。
  → 実機同居前提であり、完全 headless では別解決手段が要る。
- ✅ metasim/CA 経路: VRDevApp で session確立・D3D12キャプチャ・左スティック移動・右スティック
  回転・head override を実測(memory `vrdevapp-test-target`)。
- ✅ metasim では Layer の xrLocateViews override 後もジオメトリは**歪まない**(実測)。
- ✅ Monado PoC は **Go**(G1〜G7、`monado-poc-plan.md` 結果節)。Windows artifact 入手可・
  レイヤー結合で 3API パリティ・非CAフォールバック動作・並列セッション・xcopy デプロイ可。

### 3.2 🔬 仮説 (未検証。事実として書かない)

- 🔬 **「SteamVR での表示異常 (『画角が変』) の原因はコンポジタが物理HMDポーズで
  リプロジェクションを続けること」**。
  観測事実は「The Lab (SteamVR) で head override 時に画角の違和感があった」までで、
  **その機序 (コンポジタ再投影が原因) は未実測**。metasim では歪まないので SteamVR 固有の
  可能性はあるが、原因の同定はできていない。
  【補正 2026-07-19】この因果は**未検証として扱い、Monado 採用理由からは分離する**。
  検証法: 実 SteamVR で (a) 物理HMDポーズ固定 (b) 仮想HMDポーズ注入 の両条件で歪みの有無を
  撮って比較する。実測して初めて「歪みの原因」を決定へ昇格させる。
  ※`steamvr-driver-plan.md` の VD4 節は是正済み(🔬 ラベル付与、2026-07 時点で確認)。

### 3.3 Monado を採用する理由は「事実」だけで足りる

Monado 方向は §3.2 の未検証仮説に依存しない。根拠は次の事実のみ:

- ソースを改変できる → 仮想HMD/仮想コントローラを Runtime の**正規経路**に載せられる。
- headless / CI / 再現性の高い実行に向く(PoC で xcopy デプロイ・並列セッション実証済み)。
- 非CA環境でもフォールバック経路が全 PASS(PoC G5)。

したがって「仮想HMDでキャプチャが正確になる」という**キャプチャ精度の主張は 🔬 のまま**にし、
Monado 採用の意思決定はそれと切り離して事実の上に置く。仮に §3.2 が外れても Monado 判断は揺らがない。

---

## 4. 実行モード

- **Realtime Mode**: 通常の Runtime 挙動。実ゲーム寄り。
- **Frame-synchronized Mode**: フレーム単位で入力を適用し、リプレイ/テスト寄り。
  【補正 2026-07-19】**現時点では "Deterministic" とは呼ばない**。delta time/GPU scheduling/
  physics/async loading/predicted display time/dropped frames 等が揺れるため、決定性は
  実測で検証できてから "Deterministic Mode" へ昇格させる。それまでは「再現性の高いXR自動化」
  と表現する(過度に決定論を約束しない)。

将来管理する時間軸(📋): frame number / predicted display time / input application time /
pose timestamp / submitted frame / screenshot correlation。

---

## 5. Virtual Device Core (次ステップ M1 の対象・骨子のみ)

Runtime 非依存の中心状態。詳細な型・単位・座標系・プロトコルは M1「VirtualDeviceState/通信仕様」で確定する
(ここでは骨子だけ・実装ではない)。

```
VirtualDeviceState
├── protocol_version, sequence, timestamp
├── HMD          : pose / velocities / display params / tracking flags
├── LeftController  : grip pose / aim pose / buttons / axes / tracking flags
└── RightController : (同上)
将来: Trackers / Hand / Eye / Face
```

流れ: `PlaySpectra Command → VirtualDeviceState →(Adapter)→ xrt_space_relation / DriverPose_t / XrPosef`。

---

## 6. Server と Driver の責務分離

Driver は高水準命令を理解しない「状態配信装置」に保つ。

- 悪い構造: Driver に `walk_forward` / `look_at` / `click_object` / `run_scenario` を持たせる。
- 正しい構造: Server が `walk_forward` を解釈し `left.thumbstick=[0,1]` を生成、Driver は
  受け取った状態を Runtime へ公開するだけ。補間列の生成も Server 側。

---

## 7. 命名移行 (内部一括・ユーザー決定 2026-07-19「今すぐ一括・内部のみ」)

### 影響範囲 (2026-07-19 grep 実測)

- `vr_agent` / `vragent`(大小文字含む): 約 **340 箇所 / 64 ファイル**。
- `vragent` 単体: 34 箇所 / 7 ファイル。
- `vr-mcp` / `VR_MCP`: 46 箇所 / 22 ファイル。
- `taskfile.yml` と `.gitmodules` は既に PlaySpectra 命名(submodule 展開済み)。

### 対応表 (実行時に全数置換し、完了条件=grep 0件〈許容形除く〉)

| 旧 | 新 |
|---|---|
| `vr_agent` / `vragent` / `vr-agent` (識別子) | `playspectra` |
| `openxr_agent_layer.cpp` | `playspectra_layer.cpp`(層のエントリ) |
| `XrApiLayer_vr_agent.json` / レイヤー名 | `XrApiLayer_playspectra.json` / `XR_APILAYER_..._playspectra` |
| `driver/vragent/` ディレクトリ | `driver/playspectra/` |
| `driver_vragent.cpp` | `driver_playspectra.cpp` |
| `vragent_profile.json` / `legacy_bindings_vragent.json` | `playspectra_profile.json` / `legacy_bindings_playspectra.json` |
| env `VR_AGENT_*`(PORT/NO_CA 等) | `PLAYSPECTRA_*` |
| VR-MCP(文書中の製品名) | PlaySpectra |

### 実行時の注意 (review-checklist レンズ1/3)

- **DLL 名・driver 登録名・manifest・env 名が変わる** → 検証スクリプト(`scripts/*.sh`,
  `*_client.mjs`)・`.claude/settings.json` の allow ルール・taskfile を**全数追従**させる
  (本体ビルドだけの DoD はテスト/スクリプトのリンク切れ・パス切れをすり抜ける)。
- 完了条件は機械値: 旧識別子の `grep` 0件(意図的に残す許容形があれば列挙)。
- リポジトリ名 (`YmSaki/VR-MCP`) と PR #6 は据え置き(外部向けはユーザー判断)。

### コンポーネント名 (§14 資料の整理)

`PlaySpectra Server / Device Core / Scenario Runner / Recorder / Monado Driver /
SteamVR Driver / OpenXR Layer(Instrumentation)`。

---

## 8. マイルストーン (着手順・ユーザー確定 2026-07-19)

- **M0 設計文書ドラフト完成・レビュー承認済み** ← 本文書。✅
- **M0.5 内部一括改名** ✅(2026-07-19 実施・独立コミット `refactor/playspectra-rename-m05` 15cca62):
  vr_agent/vragent→playspectra、VR_AGENT_*→PLAYSPECTRA_*、VR-MCP→PlaySpectra。実測決定:
  内部エントリ `openxr_agent_layer.cpp`→`layer_entry.cpp`、`VR_AGENT_NO_CA`→`PLAYSPECTRA_DISABLE_CA`
  (用途=CA自動有効化を無効化/診断用)。検証: layer build+unit 78 PASS / driver build / mcp tsc /
  manifest OK / 残存0(許容: .claude履歴・feature-inventory.csv・submodule)。
  README(ドキュメント更新と混在)と driver/(当時未追跡、現在は追跡済み)は作業ツリー反映済み・別コミット。
  ※残存0の反例1件が後日判明: `driver/src/driver_playspectra.cpp:241-242` の SteamVR デバイスシリアル
  `VRAGENT_LEFT`/`VRAGENT_RIGHT`(改革前スケルトン。G3 での作り直し時に新名へ)。
- **M1 VirtualDeviceState / 通信仕様の確定** ✅(spec = `playspectra-device-core-spec.md`。M2 実装・E2E まで完了):
  型・単位・座標系・NDJSON プロトコル・protocol_version/sequence/timestamp・HMD/L/R の最小フィールド。
  Server↔Adapter の境界 API。
- **M2 最小 Monado Virtual HMD** ✅(M2.1〜M2.5 すべて完了。WSL2＋Windows・実GPU で実ビルド・E2E 検証済み〈列挙・
  pose/入力・set_state・haptics・reset・複数 observer・実アプリ hello_xr〉。M2.5 の実エンジンアプリ検証は
  ターゲット移行先の VRAppDummyGame〈Godot 4.7〉で 24/24 達成=G1、2026-07-25。詳細と各段の実測は
  `playspectra-m2-status.md` と README 検証境界表。当初 Windows は glslang 不足で
  ビルド不可だったが vcpkg ツールチェーン＋`/utf-8` で解消済み。段階化):
  - **M2.1** Monado が PlaySpectra Virtual HMD を列挙する(デバイスとして認識)
  - **M2.2** 固定 HMD pose を OpenXR アプリが取得できる(xrLocateViews/Space が値を返す)
  - **M2.3** NDJSON `set_state` で HMD pose が変わる(§spec の主経路が Driver に届く)
  - **M2.4** hello_xr を headless で起動して継続動作(framesObserved>0 で機械判定)
  - **M2.5** 実エンジンアプリで E2E 確認 ✅(当初表記は VRDevApp。実体は VRAppDummyGame で 24/24 達成)
  まず HMD のみ。「通信+Driver+headless compositor+アプリ」を一本の DoD にせず段階分割。
- 以降: **Scenario Runner ✅**(`tools/playspectra_server.py`) / **Recorder+Replay ✅**(`tools/playspectra_record.py`)
  は完了済み。残るのは **SteamVR Adapter を新 Core に接続(=G3、次の本丸)** と Layer の instrumentation
  への責務移行(注入をテスト用 override へ降格)。

改名(§7)の実行位置: M0 完了後・M1 着手前に**内部一括改名**を1パスで行い、以降の新規実装
(Core/Adapter)は最初から新名で書く(クリーンなベース=ユーザー Q1 の意図)。

---

## 9. 既存文書との関係 (再位置づけ)

- `steamvr-driver-plan.md`: → PlaySpectra の **SteamVR Adapter** の詳細計画。VD1〜VD3 の
  実測知見は有効。**VD4 節の「歪みの原因=コンポジタ再投影」は 🔬 仮説へ相対化**(§3.2)。
  H1〜H3 は従来どおり未検証ラベルのまま。
- `monado-poc-plan.md`: → **Monado Adapter** の基盤検証(Go 判定済み)。M2 はこの上に立つ。
- `openvr-milestone-plan.md`: → OpenComposite 経由の OpenVR 対応。SteamVR Adapter 系の履歴として保持。

---

## 10. スコープ規律 (CLAUDE.md 2軸を維持)

- **コア必須(全部やる)**: 入力注入の全経路(全アクション種別・両手・頭部)、キャプチャの
  **D3D11 / D3D12 / Vulkan** 全て。これらは OpenXR/各Runtime が定義する普遍的な話。
  Vulkan を先行(ユーザー B7)、他は release までに揃える。DI(依存性注入)思考は保つ。
- **スコープ外(やらない/後回し)**: 特定エンジン専用プラグイン・特定エンジン統合テストを
  必須マイルストーンに置くこと。
- nice-to-have に落としてよいのはユーザーが明示的に許容表現で言ったもの(例: 深度マップ)のみ。
  API 完全性を勝手に nice-to-have に格下げしない。
