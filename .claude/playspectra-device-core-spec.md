# PlaySpectra Virtual Device Core — VirtualDeviceState / 通信仕様 (M1 rev2 ドラフト)

作成: 2026-07-19 / 改訂: 2026-07-19 rev2(ユーザーレビュー反映)。正典 `playspectra-architecture.md` の M1。
**✅ 実装・実測済みの現行プロトコル仕様**(2026-07-30 ステータス是正: 旧「ドラフト・再レビュー待ち」のまま
M2 実装→E2E 検証まで完了し、本仕様どおりの実装が Windows/WSL2 で稼働中。fov/解像度/refresh の実値も
確定済み — 1080x1200 / 90Hz。実装は `devicecore/playspectra_control.c` の add_descriptor、
値の注入元は Monado 殻 `playspectra_interface.h` の PLAYSPECTRA_HMD_* 定数)。
基盤決定(ユーザー確定): 基準空間=**STAGE**、メッセージ=**完全スナップショット/フレーム**、
クロック=**Server が sequence 所有・時刻はモード別**、座標規約=OpenXR 既定。

rev2 で厳密化した契約: (1) set_state=完全スナップショット(継承なし)、(2) tracking を valid/tracked/connected に分離、
(3) 入力を semantic-path 集合へ、(4) grip/aim は独立状態、(5) frame_synchronized=論理ステップ、
(6) プロトコル運用規則(request_id/writer排他/エラー分類)、(7) M2 段階化(→ architecture §8)。

---

## 1. 規約 (conventions)

- **座標系**: 右手系・**+Y 上 / +X 右 / −Z 前**・単位 **メートル**。OpenXR/Monado と同一。
- **四元数**: `[x, y, z, w]`(正規化)。OpenVR `DriverPose_t` は `[w,x,y,z]` 順なので SteamVR Adapter が並べ替える(§6)。
- **基準空間 (canonical)**: **STAGE** = 床が y=0、原点は床上プレイエリア中心、+Y 上。
  各 Runtime の原点差([[local-space-origin-per-runtime]])は **Adapter が吸収**する。Core は常に STAGE 表現。
- **軸**: thumbstick x/y ∈ `[-1,1]`(x=右+, y=前+)。`/value` 系(trigger/squeeze)∈ `[0,1]`。
- **速度**: 線速度 m/s、角速度 rad/s。
- **時刻とモード**(§4): `sequence` は Server 所有の単調増加。`clock.mode` がモードを表す。

---

## 2. データモデル

### 2.1 PoseState (再利用単位)

pose ごとに追跡状態を持つ(HMD head / controller grip / controller aim で別々に成立し得る)。

```jsonc
{
  "position":        [0.0, 1.6, 0.0],     // STAGE, m。必須
  "orientation":     [0, 0, 0, 1],        // quat xyzw。必須
  "linear_velocity": [0, 0, 0],           // m/s。必須キー・値なしは null 可(§3)
  "angular_velocity":[0, 0, 0],           // rad/s。必須キー・値なしは null 可
  "relation_flags": {                     // 必須(4つとも bool)
    "position_valid":     true,           // 現在の位置値が意味を持つか
    "orientation_valid":  true,           // 現在の向き値が意味を持つか
    "position_tracked":   true,           // 実追跡中か(false=推定/外挿で valid のこともある)
    "orientation_tracked":true
  }
}
```

`valid` と `tracked` を分離する: 「値は有効だが光学追跡を失って推定中」= valid:true / tracked:false。
OpenXR の `XR_SPACE_LOCATION_*_VALID_BIT` / `*_TRACKED_BIT` に対応(§6)。

### 2.2 VirtualDeviceState (完全スナップショット・冪等)

各メッセージは**完全**: connected な device の**全可変フィールドを毎回含む**(省略による前回値継承は無い)。
省略=不足はエラー(§5 validation_error)。部分更新は将来の `patch_state`(未実装)で分離する。

```jsonc
{
  "protocol_version": 1,
  "sequence": 12345,                    // Server 所有・単調増加
  "clock": { "mode": "realtime", "t_ns": 1732000000000 },   // §4
  "hmd": {
    "connected": true,                  // device 単位
    "head": { /* PoseState (2.1) */ }   // HMD の頭部 pose(view 基準)
  },
  "left":  { /* Controller (2.3) */ },
  "right": { /* Controller (2.3) */ }
}
```

- `connected:false` の device は他フィールド不要(その時点で非接続)。connected な device は完全必須。
- `hmd`/`left`/`right` の3キーは常に present(未接続は `{"connected":false}`)。

### 2.3 Controller

```jsonc
{
  "connected": true,
  "grip": { /* PoseState */ },   // 物体を握る基準。独立状態
  "aim":  { /* PoseState */ },   // レイ/ポインティング/射線の基準。独立状態
  "inputs": {                    // §2.4。Descriptor が宣言した path を全て必須で含む
    "/input/trigger/value": 0.0,
    "/input/trigger/touch": false,
    "/input/squeeze/value": 0.0,
    "/input/thumbstick/x": 0.0,
    "/input/thumbstick/y": 0.0,
    "/input/thumbstick/click": false,
    "/input/thumbstick/touch": false,
    "/input/thumbrest/touch": false,
    "/button/a/click": false,
    "/button/a/touch": false,
    "/button/b/click": false,
    "/button/b/touch": false,
    "/input/menu/click": false
  }
}
```

- **grip/aim は独立状態として Core に保持**(手首固定で照準だけ変える等を表現できる)。
  片方から他方を自動生成する補助は **Core ではなく上位 Server** に置く(Core は与えられた両 pose をそのまま扱う)。

### 2.4 入力モデル (profile 非依存・semantic path)

固定 struct に物理ボタン名を焼き込まず、**well-known semantic path をキーとする集合**にする。
これにより trackpad 等の将来コントローラや左右非対称(左 x/y・右 a/b)を列挙型に押し込まずに済む。

- path 命名: `/button/<name>/click|touch`、`/input/<name>/value|x|y|click|touch`。
- 値型は suffix で決まる: `/value` → float[0,1]、`/x`・`/y` → float[-1,1]、`/click`・`/touch` → bool。
- **存在有無は Descriptor(§3)が宣言**。未対応入力(path 非存在)と「現在 false の入力」を区別する。
- **完全性**: set_state は、その hand の Descriptor が宣言した path を**すべて**含む(欠落=validation_error)。
- **system 等 runtime 予約入力は application 入力から分離**する。M1 の正典 application 入力に
  `system` を無条件で含めない。必要時は別名前空間 `runtime_reserved`(任意・完全性対象外)に置く:
  ```jsonc
  "runtime_reserved": { "/input/system/click": false }   // 任意。Runtime が公開できない場合あり
  ```
- 初期実装が内部で固定 struct を使うのは可。ただし**ワイヤ形式は semantic path + Descriptor 宣言**とする。

---

## 3. Device Descriptor (静的・接続時に1回ネゴ)

毎フレーム送らない静的能力。存在する入力 path と表示諸元を宣言する。

```jsonc
{
  "protocol_version": 1,
  "hmd": {
    "recommended_eye_width": 1680, "recommended_eye_height": 1760, "refresh_hz": 90,
    "fov": { "left":  { "angle_left": -0.9, "angle_right": 0.9, "angle_up": 0.9, "angle_down": -0.9 },
             "right": { "angle_left": -0.9, "angle_right": 0.9, "angle_up": 0.9, "angle_down": -0.9 } }
  },
  "controllers": {
    "profile": "playspectra_generic",     // 正典 profile 名。Adapter が Runtime profile へ写像
    "left":  { "inputs": ["/input/trigger/value","/input/trigger/touch","/input/squeeze/value",
                          "/input/thumbstick/x","/input/thumbstick/y","/input/thumbstick/click",
                          "/input/thumbstick/touch","/input/thumbrest/touch",
                          "/button/x/click","/button/x/touch","/button/y/click","/button/y/touch",
                          "/input/menu/click"] },
    "right": { "inputs": ["/input/trigger/value","/input/trigger/touch","/input/squeeze/value",
                          "/input/thumbstick/x","/input/thumbstick/y","/input/thumbstick/click",
                          "/input/thumbstick/touch","/input/thumbrest/touch",
                          "/button/a/click","/button/a/touch","/button/b/click","/button/b/touch"] }
  }
}
```

fov/解像度/refresh の実値は Runtime 実測が正。M2 で Monado から取得して確定する(ここは形のみ)。

---

## 4. モードとクロック

- **sequence**: Server 所有の単調増加 uint。全メッセージに付き、トランスポート順序を与える。
- **realtime**: `clock = { "mode":"realtime", "t_ns": <int|null> }`。t_ns は参考。**latest-wins**
  (最大 sequence を現在状態に採用、古い sequence は破棄)。決定性は保証しない。
- **frame_synchronized**: `clock = { "mode":"frame_synchronized", "logical_frame": <int> }`。
  - `logical_frame` は **PlaySpectra Server が発行する論理ステップ番号**。
    **OpenXR アプリ frame や compositor frame との一致は保証しない**(名は実質 logical-frame-synchronized)。
    実 Runtime frame との相関は **M2 で計測して確定**する。
  - Adapter は各 logical_frame で「対応する最新 state を一度適用」する。
  - **同一 logical_frame の重複**: 内容が同一なら**冪等成功**、内容が異なれば **conflict_error**(§5)。
  - 遅れて届いた state(既に次の frame を適用済み): 破棄しつつ `applied:false, reason:"stale_frame"` を返す。
- `predicted_display_time` 等の**物理**時刻は Runtime/Adapter 由来。Core には持たせず、M2 で
  sequence/logical_frame と相関づける(pose timestamp / submitted frame / screenshot correlation)。

---

## 5. 通信仕様 (Server ↔ Adapter 境界)

### 5.1 トランスポートとフレーミング

- NDJSON over TCP。**UTF-8**、**1 JSON object = 1 行**(`\n` 区切り)、**空行は無視**。
- **最大メッセージサイズ = 1 MiB**。超過は protocol_error + 接続クローズ(状態=キャプチャ等の大物はこの経路に載せない)。
- **未知フィールドは許容(tolerant reader)**: 認識できない追加キーは無視(マイナー拡張の前方互換)。
  ただし set_state の**必須フィールド欠落は validation_error**(未知フィールド許容とは別)。

### 5.2 リクエスト/レスポンス対応

- すべての**リクエストに `request_id`**(client 採番)を付け、レスポンスは同じ `request_id` を返す
  (set_state/get_state/status が同一接続を流れても対応が曖昧にならない)。
- **エラー分類**を明示: `error_type` ∈
  - `protocol_error` — フレーミング/バージョン/未知 cmd/writer 競合/サイズ超過
  - `validation_error` — スキーマ/範囲/必須欠落
  - `conflict_error` — frame_synchronized の同一 frame 異内容

### 5.3 ロール(書き込み排他)

- `hello` で `role` を宣言: `"writer"` | `"observer"`。
- **writer は同時に1接続のみ**(最初に writer で hello を完了した接続が排他書き込み権を持つ)。
  以降の writer hello は `protocol_error: "writer_taken"`。**observer は複数可**(get_state/status/イベント購読)。
- **writer 切断時**: Adapter は最後に適用した state を**保持**(device は消さない)し、status に
  `writer_connected:false` を立てる。新しい writer が hello で引き継ぎ可能。明示 `reset` で初期化。

### 5.4 メッセージ

```jsonc
// ハンドシェイク
→ { "cmd":"hello", "request_id":"h1", "protocol_version":1, "role":"writer" }
← { "request_id":"h1", "ok":true, "protocol_version":1, "role_granted":"writer", "descriptor": { …§3… } }
← { "request_id":"h1", "ok":false, "error_type":"protocol_error", "error":"writer_taken" }
← { "request_id":"h1", "ok":false, "error_type":"protocol_error", "error":"protocol_version", "supported":[1] }

// 状態適用(主経路・writer のみ)
→ { "cmd":"set_state", "request_id":"s42", "state": { …§2.2… } }
← { "request_id":"s42", "ok":true, "applied":true, "sequence":12345, "logical_frame":120 }
← { "request_id":"s42", "ok":true, "applied":false, "reason":"stale_frame" }          // 遅延/latest-wins 破棄
← { "request_id":"s42", "ok":false, "error_type":"validation_error", "error":"missing:/input/trigger/value", "hand":"left" }
← { "request_id":"s42", "ok":false, "error_type":"conflict_error", "error":"frame_content_mismatch", "logical_frame":120 }

// 問い合わせ(observer/writer)
→ { "cmd":"get_state", "request_id":"g1" }   ← { "request_id":"g1", "ok":true, "state": { …現在… } }
→ { "cmd":"status",    "request_id":"q1" }   ← { "request_id":"q1", "ok":true, "writer_connected":true, "devices": { … }, "runtime": "…" }
→ { "cmd":"reset",     "request_id":"r1" }   ← { "request_id":"r1", "ok":true }        // writer のみ

// イベント(Adapter→Server, 非要求・request_id なし)
← { "event":"haptics", "hand":"left", "amplitude":0.5, "duration_ms":100, "frequency_hz":160 }  // 詳細は M2 以降
```

---

## 6. Runtime 型への写像 (informative — Adapter 実装の指針)

| Core | OpenXR / Monado | OpenVR / SteamVR |
|---|---|---|
| PoseState.position/orientation | `XrPosef`(quat xyzw) | `DriverPose_t`(quat **wxyz** へ並べ替え・`vecPosition`/`qRotation`) |
| relation_flags valid/tracked | `XR_SPACE_LOCATION_*_VALID_BIT` / `*_TRACKED_BIT` | `poseIsValid` / `result`(Running_OK 等) / `deviceIsConnected` |
| STAGE 基準 | `XR_REFERENCE_SPACE_TYPE_STAGE` | driver world 原点(床)へ写像 |
| linear/angular_velocity | `xrt_space_relation` の linear/angular velocity | `vecVelocity` / `vecAngularVelocity` |
| /input/*/value・x・y | action `.../input/*/value|x|y` | `IVRDriverInput` scalar component |
| /button/*/click・touch | action `.../click|touch` | `IVRDriverInput` bool component |
| grip/aim pose | `/user/hand/*/input/grip|aim/pose` | controller pose(aim はレイ用オフセット) |
| device.connected | session/interaction profile の有無 | `deviceIsConnected` / `TrackedDeviceAdded` |

---

## 7. 未確定 / 後続で確定 (deferred)

- fov/recommended 解像度/refresh の実値: M2 で Monado から実測して Descriptor を確定。
- velocity 供給元: Server 供給 or Adapter が pose 差分から推定 — 実測して選ぶ(protocol は null 許容で両対応)。
- haptics 逆方向イベントの詳細スキーマ(振幅/周波数/持続の単位・複数チャンネル)。
- logical_frame と実 Runtime frame の相関計測(M2)。
- Trackers / Hand / Eye / Face(将来。Descriptor と State を後方互換拡張)。
- `patch_state`(部分更新)の要否と設計。
- protocol_version 昇格規則(破壊的変更で +1、Descriptor で能力ネゴ)。

---

## 8. M1 完了条件 (DoD)

- [x] 座標規約・基準空間・単位(§1)
- [x] PoseState(valid/tracked/connected 分離)・VirtualDeviceState 完全スナップショット(§2)
- [x] semantic-path 入力モデル + system 分離 + Descriptor 宣言(§2.4, §3)
- [x] grip/aim 独立状態(§2.3)
- [x] frame_synchronized=論理ステップの限定定義・realtime latest-wins / frame 競合規則(§4)
- [x] プロトコル運用規則(request_id / writer 排他 / エラー分類 / フレーミング / 未知フィールド)(§5)
- [ ] **ユーザー再レビュー承認** → 承認後 M2(段階化: architecture §8 M2.1〜M2.5)着手
