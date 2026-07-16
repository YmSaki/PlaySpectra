# Monado PoC 計画 — Meta XR Simulator 代替検証

作成: 2026-07-16。調査結果の詳細はメモリ `custom-runtime-future` を参照。

## 目的

テスト基盤の OpenXR ランタイムを Meta XR Simulator から Monado(Windows) へ差し替えられるか検証する。
これは**テスト基盤側のインフラ課題**であり、vr_agent レイヤー/MCP のコアループには一切手を入れない
(ランタイムは `XR_RUNTIME_JSON` の差し替えのみ。Playwright がブラウザを差し替えるのと同型)。

動機(4点): SES 単一インスタンス競合の根絶 / 決定性 / Meta シムの癖(CA の向き無視)排除 / 配布・ライセンス制約の解消。
PoC が Go なら自作小型ランタイムは不要になる。No-Go ならその失敗ログ自体が自作ランタイムの要件定義になる。

## Go/No-Go 判定基準

| # | 条件 | 必須? |
| --- | --- | --- |
| G1 | Monado の Windows ビルド(公式 CI artifact またはローカルビルド)が入手できる | 必須 |
| G2 | 素の Monado + hello_xr -g Vulkan がセッション RUNNING まで到達(表示窓の有無は不問) | 必須 |
| G3 | vr_agent レイヤー有効で `integration_test.sh Vulkan` 全項目 PASS | 必須 |
| G4 | 同 `D3D11` / `D3D12` 全項目 PASS | 必須(片方欠けは条件付き Go) |
| G5 | 非 CA フォールバック経路の動作確認(Monado は XR_EXT_conformance_automation 非対応の可能性大 → [G] 経路が既定になる想定。それ自体は許容) | 必須 |
| G6 | hello_xr 2プロセス同時起動で SES 相当の競合が起きない(並列テストの本丸動機) | 強く推奨 |
| G7 | **xcopy デプロイ可能**: インストーラ・レジストリ登録・管理者権限・常駐サービス導入なしで、ディレクトリ配置+`XR_RUNTIME_JSON` のみで動く(CI/配布の要。ユーザー指定 2026-07-16) | 必須 |

判定: G1〜G5+G7 全て PASS → **Go**(移行タスク化へ)。G4 が部分 PASS → **条件付き Go**(欠けた API は Meta シム併用)。
G1/G2/G3/G7 のいずれか不能 → **No-Go**(自作小型ランタイム設計へ。失敗理由を要件として記録)。
※G7 は「別途インストールが必要なら自作に舵を切る」というユーザー方針の反映。要インストールの時点で減点でなく失格。

## フェーズ

### Phase 0: ベースライン固定(~30分)
- Meta XR Sim で `integration_test.sh Vulkan`(+可能なら D3D11/D3D12)を再実行し、現在の PASS 数を比較基準として記録。
- `VR_AGENT_NO_CA=1` 経路も1回回して [G] 経路のベースラインを取る(Monado では常時この経路になる想定のため)。

### Phase 1: Monado 入手(0.5〜1日)
- 優先: GitLab (gitlab.freedesktop.org/monado/monado) の CI が出す Windows artifact。
- 代替: ローカルビルド(MSVC 推奨。本リポジトリは MinGW だが、ランタイムは別プロセスなのでツールチェーン混在は無問題)。
- 成果物: monado の runtime JSON のパス確認、`third_party/monado/` に配置(非コミット、third_party 慣習に従う)。
- 確認事項: Windows での service モデル(monado-service 別プロセスか in-process か)、simulated ドライバ/null compositor の有効化方法(XRT_* / MND_* 環境変数)。

### Phase 2: 素の動作確認(0.5日)
- `XR_RUNTIME_JSON` を Monado に向け、レイヤー**無し**で `hello_xr -g Vulkan`。
- 見る点: instance/session 生成、フレームループ到達、simulated HMD のビュー供給。表示ウィンドウが出ても可(観察はレイヤーが行うため表示品質は不問)。
- つまずいたら: Monado のログ(XRT_LOG=debug 相当)で原因を分類 → ビルド問題なら Phase 1 へ戻る、機能欠落なら No-Go 材料。

### Phase 3: レイヤー結合スモーク(0.5日)
- レイヤー有効化(`XR_API_LAYER_PATH` / `XR_ENABLE_API_LAYERS` は既存のまま)。
- `vr_status` でランタイム名・CA 拡張有無を確認 → CA 無しなら非 CA フォールバックが自動選択されることを確認。
- head/pose/input/screenshot の手動スモーク(scripts/ の *_client.mjs)。

### Phase 4: 統合テスト 3API(1日)
- `scripts/env.sh` にランタイム切替を追加(例: `VR_RUNTIME=metasim|monado` で `XR_RUNTIME_JSON` を分岐)。**既存既定は metasim のまま**。
- `integration_test.sh` の Meta 固有箇所(taskkill の MetaXRSimulator/synth_env_server、SES 前提のプロセス衛生)を切替対応。
- Vulkan → D3D11 → D3D12 の順に全項目実行。失敗は「Monado の欠落」「レイヤーの Meta 依存前提」「テストの Meta 依存前提」の3分類で記録。

### Phase 5: 差分・癖の記録と判定(0.5日)
- 記録: CA 有無 / displayTime の性質(決定性) / swapchain 画像数・フォーマット列挙差 / G6 並列起動結果。
- 判定基準表に沿って Go / 条件付き Go / No-Go を決定し、メモリ `custom-runtime-future` と backlog を更新。
- Go の場合の次タスク種: CI での既定ランタイム切替、Meta シム依存記述の整理(README 含む)。
- No-Go の場合: 失敗ログを要件化して自作小型ランタイム(土管仕様: swapchain 3API 割当+フレームペーシング+スタブ入力)の設計へ。

## リスクと逃げ道

- **Windows ビルドの成熟度**(最大リスク): D3D11/D3D12 client 対応は「initial support」段階。→ G4 部分 PASS の条件付き Go を許容し、欠け分は Meta シム併用で塞ぐ。
- **service 構成の複雑さ**: 別プロセス service が必要な場合、integration_test.sh のプロセス管理(起動/終了/ゾンビ掃除)を1系統追加。
- **hello_xr の stdin EOF 問題は Monado でも同じ**(アプリ側の性質)。feeder パイプ方式はそのまま流用。
- **manifest 隣 DLL の stale 事故**(対策済み)と同種の「どのランタイムを掴んだか」事故 → Phase 3 以降、テストログに必ずランタイム名を出す(vr_status で機械確認)。

## スコープ外(このPoCではやらない)

- Meta XR Sim の完全廃止・third_party からの削除
- CI への組み込み、配布物の変更
- 自作ランタイムの実装着手(No-Go 判定後の別フェーズ)
- Monado 本体への upstream パッチ作成(回避可能な癖は記録に留める)

## 見積もり

合計 3〜4 日規模(Phase 1 のビルド難航時 +1〜2 日)。トークン面では Phase 1-2 が調査寄り、Phase 3-5 は既存テスト資産の再実行が主で軽い。

---

## 結果 (2026-07-16 実施) — 判定: **Go**

| # | 結果 | 証拠 |
| --- | --- | --- |
| G1 | ✅ | GitLab CI artifact (main@2e7d9ea, v25.1.0-646, job=windows)。third_party/monado/ に配置、VERSION.txt に記録 |
| G2 | ✅ | 素の hello_xr -g Vulkan がセッション VISIBLE+ 到達(Simulated HMD 自動選択) |
| G3 | ✅ | Vulkan 17/17 + graceful PASS(ベースラインの Metaシム 17/17 と完全一致) |
| G4 | ✅ | D3D12 17/17 + graceful PASS。D3D11 は 16/17 だが、唯一の FAIL(単色キャプチャ)は **Metaシムでも同一再現するレイヤー側の潜在バグ**(下記)であり Monado 起因ではない — 3API とも Metaシムと完全パリティ |
| G5 | ✅ | CA 拡張なし → 非CAフォールバック [G] 経路が自動選択され全 PASS |
| G6 | ✅ | 1つの monado-service に hello_xr 2プロセス同時セッション、両制御チャネル(52710/52711)同時 LISTEN |
| G7 | ✅ | CI zip 展開のみ・runtime JSON は相対パス・monado-service は同梱の素のユーザープロセス(インストーラ/レジストリ/管理者権限/常駐サービス登録すべて不要) |

### PoC の副産物(重要)
1. **D3D の E2E テストが初めて可能になった**: MSVC 版 hello_xr(D3D11/D3D12 プラグイン入り)を third_party/hello_xr_msvc/ に配備し、`HELLO_XR_EXE` で差し替え可能に。backlog の Deferred(R08/R09/R10/R17)の「この環境で E2E 不可」前提が解消。
   ※MSVC 版は Vulkan SDK 不在で Vulkan プラグインなし — Vulkan は従来の MinGW 版、D3D は MSVC 版と使い分ける。
2. **レイヤーの潜在バグ発見**: D3D11 キャプチャが単色画像(distinctColors=1)を返す。Metaシム/Monado 両方で同一再現 → ランタイム非依存のレイヤーバグ。backlog に P1 登録(d3d11-flat-capture)。
3. テストハーネス2修正: capture.api 期待値のパラメータ化(VR_GFX_API)、PNG デコーダの greyscale(colorType 0)対応。

### 記録した差分・癖
- Monado の Simulated HMD は 896x1007/eye(Metaシムは 1680x1760)。テストは寸法非依存なので影響なし。
- swapchain フォーマットは両ランタイムとも fmt=29(R8G8B8A8_UNORM_SRGB)を hello_xr が選択(Vulkan は 43=SRGB)。
- monado-service の名前付きパイプ名は %TEMP% パス由来 → TEMP を変えれば多重サービスも原理上可能(未検証)。
- 要確認(CI 化の際): monado-service がコンポジタウィンドウを開く挙動のヘッドレス CI ランナーでの可否。XR_MND_headless は DLL に存在。

### 移行タスク候補(次フェーズ)
- d3d11-flat-capture バグ修正(P1・コア必須。E2E 手段が揃った今なら再現→修正→検証まで回せる)
- CI/既定ランタイムの Monado 切替、README の実行手順更新
- Deferred R08/R09/R10/R17 の E2E 検証つき実施(承認待ち解除の条件が整った)
