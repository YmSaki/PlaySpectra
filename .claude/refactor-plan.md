# VR-Playwright レイヤ リファクタリング計画書

作成: 2026-07-14 / 対象: v1.0.0 機能一式コミット済み時点 (HEAD = a751805)
方針の絶対制約: CLAUDE.md の North Star（VR版Playwright / コアループ最優先 / over-scope・under-scope 双方の回避）

---

## 0. 結論サマリ

- **是非**: リファクタは**正当化される。ただし「振る舞いを一切変えない、ファイル分割（move-only）」に厳格に限定する**。
  クラス階層・仮想インターフェース・ロック設計変更・multi-instance 対応などのアーキテクチャ変更は**行わない**（§6）。
- **形**: `capture_backends.h` の平坦分離（GAP-01/02 の成功例）を唯一の手本とし、
  (a) capture.cpp から Vulkan バックエンドを同じパターンで抽出、
  (b) openxr_agent_layer.cpp を責務クラスタ 5 つ（log / dispatch / action registry / pose override / input inject）に分割。
  **すべて既存機構の整理であり、並行システムの新設は 0 件**（各項目で個別に示す — §3）。
- **フェーズ数: 6**（+ 事前ベースライン確認）。各フェーズは単独で「`mingw32-make -C layer/build vr_agent_layer` 緑 + `scripts/integration_test.sh` 14/14 緑」を保ち、1フェーズ = 1コミット、ロールバック = そのコミットの revert。
- **触らないもの**: `control_channel.{h,cpp}`（558+96行、責務は既に単一=transport）、`mcp/src/server.ts`（730行/19ツール、分割は金メッキ）、`capture_d3d11/12.cpp`（既に手本の形）。

---

## 1. 現状アーキテクチャの棚卸し（コード根拠付き）

### 1.1 ファイル別の責務と規模

| ファイル | 行数 | 責務 | 判定 |
|---|---|---|---|
| `layer/src/openxr_agent_layer.cpp` | 1782 | **6+ 責務が同居**（下記 1.2） | 分割対象（本丸） |
| `layer/src/capture.cpp` | 1386 | OpenXR側追跡＋要求ハンドオフ **と** Vulkanバックエンド（readback/MSAA/HDR/深度）が同居 | 分割対象 |
| `layer/src/capture_d3d11.cpp` | 225 | D3D11 readback バックエンド | **手本。触らない** |
| `layer/src/capture_d3d12.cpp` | 306 | D3D12 readback バックエンド | **手本。触らない** |
| `layer/src/capture_backends.h` | 52 | 平坦（プリミティブ引数のみ）バックエンド interface | Vulkan 分を追記するのみ |
| `layer/src/control_channel.{h,cpp}` | 96+558 | Winsock NDJSON transport + コマンド解釈 + 注入状態ストア | **触らない**（§2.3） |
| `mcp/src/server.ts` | 730 | MCP ツール19本 → NDJSON クライアント | **触らない** |

### 1.2 openxr_agent_layer.cpp の内部責務クラスタ（file:line 根拠）

```
openxr_agent_layer.cpp (1782行)
│
├─ [A] ロギング ......................................... 42-68
│     LayerLog + ログストリーム。capture.cpp:45-46 と
│     control_channel.cpp:27-32 が extern 前方宣言を「重複」して参照。
│
├─ [B] ディスパッチ基盤（GAP-07） ....................... 83-210, 1552-1631, 1660-1733, 1743-1781
│     g_mutex / g_instance / g_session / g_ca_enabled (83-87)
│     LayerDispatch 構造体 + g_dispatch (94-133)
│     ResolveNext / RebuildLayerDispatch / ClearLayerDispatch (140-210)
│     VrAgentGetInstanceProcAddr のフックテーブル (1552-1631)
│     RuntimeSupportsExtension / CreateApiLayerInstance / negotiate (1634-1781)
│     + パスユーティリティ ToPath (212-218) / PathToStr (477-488)
│
├─ [C] CA入力適用（buttons/analog/active/pose push） .... 240-301
│     ApplyPendingInputs: キュー drain → xrSetInputDeviceState* /
│     xrSetInputDeviceLocationEXT。EnsureLocalSpace (222-236) を使用。
│
├─ [D] 頭部/VIEW上書き + ポーズ数学 ..................... 303-419, 1143-1204, 1247-1359
│     g_view_spaces / g_ref_space_types + mutex (309-313)
│     クォータニオン/ベクトル数学 QMul/QRot/... (333-346)
│     RebaseViewsToHead (351-366)  IPD保存の再基底化
│     TransformLocalPoseToSpace / TransformHeadToSpace (378-419)
│     Hook_xrLocateViews の上書き+view公開 (1143-1204)
│     ApplyHeadToLocation / ZeroVelocity(GAP-05) / FindInNextChain (1255-1281)
│     Hook_xrLocateSpace / Hook_xrLocateSpaces (1284-1359)
│
├─ [E] コントローラー grip/aim ポーズ上書き ............. 421-682
│     g_action_mutex + g_action_spaces / g_grip_pose_actions /
│     g_aim_pose_actions / g_grip_to_aim(+valid) (429-441)
│     GAP-06 null-subaction 解決 HandTopFromBindingPath/InferHandTops (490-522)
│     ResolveGripToAimOffset(GAP-04, 2相ロック規律) (532-581)
│     ApplyPoseOverride（xrLocateSpace/Spaces 双方から呼ばれる中核） (589-682)
│
├─ [F] アクション発見レジストリ（観測専用, WU5） ........ 443-475, 1362-1503, 1509-1547
│     ActionSetReg/BindingReg/ActionReg + g_action_sets/g_actions/
│     g_attached_action_sets (452-464)
│     記録フック: CreateActionSpace(1362) / SuggestBindings(1382) /
│     CreateActionSet(1412) / CreateAction(1430) / DestroyActionSet(1456) /
│     AttachSessionActionSets(1485)
│     BuildActionsJson（socketスレッドから呼ばれる dump） (1509-1547)
│
├─ [G] GAP-08 非CAフォールバック（action system エミュレーション） 684-827, 975-1080
│     FallbackActionState + g_fallback_states / g_synced_active_sets /
│     g_emulated_profile / g_pending_ip_event (698-716)
│     ActionsBoundTo / ApplyFallbackSync / AggregateFallback (723-827)
│     読み取りフック: GetActionState{Boolean,Float,Vector2f}(975-1030) /
│     GetCurrentInteractionProfile(1034) / PollEvent(1055)
│
└─ [H] capture / control_channel への薄い転送フック ..... 832-958, 1082-1140
      CreateSession/DestroySession/swapchain系/EndFrame → vr_agent::CaptureOn*
      ApplyHapticFeedback → ControlChannelRecordHaptic (1082-1102)
      DestroyInstance のクラスタ横断クリーンアップ (1104-1140)
```

### 1.3 capture.cpp の内部責務（file:line 根拠）

```
capture.cpp (1386行)
├─ [K] OpenXR側: 追跡 + スナップショット + ハンドオフ + ディスパッチ
│     SwapchainInfo / g_swapchains (54-72)
│     EndFrameSnapshot (405-424)
│     capture要求ハンドオフ g_req_* + cv (426-433)
│     CaptureOnCreateSession のバインディング検出 (1089-1113)
│       ※ D3D11/D3D12 は既にここから D3D11SetDevice/D3D12SetDevice へ
│         「平坦に」手渡している (1102-1110) — 手本パターンの起点
│     CaptureOn{Swapchain,Images,Acquire,Release} (1127-1193)
│     CaptureOnEndFrame: パース + per-API 分岐ディスパッチ (1195-1347)
│     CaptureRequestScreenshot / CaptureStatusJson (1349-1383)
│
└─ [V] Vulkanバックエンド（本来 capture_vulkan.cpp にあるべき部分）
      vulkan-1.dll 動的ロード VkFns/LoadVulkan (77-201)
      リソース管理 Ensure{VulkanResources,Staging,ResolveImage}/Free (229-402)
      フォーマット/HDR/半精度float/リニア→sRGB (470-574)
      VulkanReadbackToPng（色, MSAA resolve, HDR tonemap） (603-802)
      深度: ClassifyDepthFormat/LinearizeViewDepth/
      VulkanReadbackDepthToPng/ResolveDepth (812-1069)
```

### 1.4 具体的な重複・結合点

1. **extern 前方宣言の重複**: `LayerLog` を capture.cpp:45-46 と control_channel.cpp:27-31 が各自宣言。
   `LayerBuildActionsJson` ブリッジも同様（openxr_agent_layer.cpp:1737-1741 ⇔ control_channel.cpp:29-32）。
   ヘッダが無いことによる「暗黙の ABI」であり、シグネチャ変更がコンパイラで検出されない。
2. **Vulkan バックエンドだけ interface の外**: D3D11/12 は `capture_backends.h` の平坦関数だが、
   Vulkan は capture.cpp 内部型 `EndFrameSnapshot::View` を直接受ける（capture.cpp:603-604, 852-853）。
   同一 dispatch 分岐（1294-1316）内で「D3D は平坦 / Vulkan は内部型」と非対称。
3. **出力パス生成の三重化**: `NextColorCapturePath()`（1085-1087, D3D 用）と、Vulkan 色 (770-771)・
   深度 (1013-1014) のインライン組み立てが同じ counter/dir を別々に書いている。
4. **g_action_mutex が 3 クラスタ（E/F/G）を横断**: レジストリ読み（F）・pose解決（E）・
   フォールバック状態（G）が同一 mutex（GAP-08 コミットで「別ロックは順序逆転になる」と明記済み、
   openxr_agent_layer.cpp:695-697）。分割時に**この共有をヘッダで明示化する必要がある**（§3.3）。
5. **クラスタ横断クリーンアップ**: Hook_xrDestroySession(850-882) / Hook_xrDestroyInstance(1104-1140) が
   D/E/F/G の状態を直接クリアしている。分割時は各モジュールの Clear 関数呼び出しに置き換える
   （capture が既に `CaptureOnDestroySession` でやっている形と同じ）。
6. **ロック規律（不変条件）がコメントに散在**:
   - 「g_action_mutex 保持中にランタイムを呼ばない」(532-556, 745-747)
   - 「g_action_mutex 保持中に ControlChannel* を呼ばない」(612)
   - 「ホットフックでの g_dispatch 読みは無ロック（単一インスタンスモデルで安全）」(89-93)
   これらは分割後、所有モジュールのヘッダに集約して明文化する。

---

## 2. リファクタの是非 — 正直な評価

### 2.1 「やらない」選択肢の検討

やらない場合のコスト/ベネフィット:
- コードは動いており、テストは緑、コメントは充実している。1782行は大きいが読めなくはない。
- 分割自体はユーザー価値を 1 も生まない（スクリーンショットが良くなるわけでも入力が増えるわけでもない）。
- **リファクタのためのリファクタなら、やらないのが正しい。**

しかし、やらない場合に確実に起きること:
1. **直後に実装予定の durationMs 補間（§5）は、クラスタ C/D/E（CA適用・頭部・コントローラーポーズ）の
   全てに触る機能**である。補間エンジン＋時刻キャッシュという新しい状態族を 1782 行のファイルに
   追加すると、既に 3 つの mutex と 6 つの状態族があるファイルが 2000 行超・4 mutex になる。
2. 実 velocity（GAP-05 の一般化）も同様に D/E の ZeroVelocity 経路（1305-1311, 1349-1352）に挿さる。
3. g_action_mutex の 3 クラスタ横断や「コメントでしか守られていないロック規律」は、
   機能追加のたびに壊すリスクが累積する（GAP-08 のメタ認知レビュー是正 71a9a16 は正にロック順序の修正だった）。

### 2.2 判定

**分割は「近未来機能の受け入れ準備」として正当化される。金メッキとの境界線は次で引く:**

- ✅ やる: 責務クラスタをファイルに切り出し、共有状態と不変条件をヘッダで明示する。**動作・ロック設計・
  データ構造・アルゴリズムは 1 バイトも変えない**（診断: 変更後の diff が「移動 + include/宣言の配管」のみで
  説明できること）。
- ❌ やらない: 「きれいな設計」のための抽象化（仮想関数、クラス化、レイヤ化）、ロック粒度の見直し、
  multi-instance 対応（openxr_agent_layer.cpp:74-82 に「やらない」と明記済みの設計判断を尊重）、
  control_channel/MCP の再構成。これらはコアループに 1 mm も寄与しない。

### 2.3 触らない判断の根拠

- **control_channel.{h,cpp}**: 責務は「socket transport + コマンド→状態/キュー変換」で既に単一。
  sticky pose / head の生ストア（control_channel.cpp:49-54）は「コマンドされた目標値の置き場」として
  transport 側にあるのが正しく、durationMs でも動かす必要がない（§5.2 参照 — 補間はレイヤ側の
  評価器がやる。ストア移設は不要と判断した）。
- **mcp/src/server.ts**: 730 行 / 19 ツールは 1 ツール平均 ~35 行の平坦な列挙で、責務混在がない。
  分割は純粋な金メッキ。
- **capture_d3d11/12.cpp**: 手本そのもの。

---

## 3. 提案する再構成

### 3.1 目標ファイルマップ

```
layer/src/
  openxr_agent_layer.cpp   ~600行  negotiate / GIPAフックテーブル / instanceライフサイクル /
                                   全 Hook_xr*（薄い: 記録・上書きはモジュール関数を呼ぶ）
  layer_log.{h,cpp}        ~50行   [A] LayerLog（extern重複の解消）
  layer_dispatch.{h,cpp}   ~280行  [B] LayerDispatch + g_instance/g_session/g_ca_enabled +
                                   Rebuild/Clear + ToPath/PathToStr
  action_registry.{h,cpp}  ~330行  [F] 観測レジストリ + grip/aim分類 + GAP-06推論 +
                                   BuildActionsJson + ActionMutex()公開
  pose_override.{h,cpp}    ~450行  [D+E] ポーズ数学 + VIEW空間追跡 + 頭部再基底 +
                                   grip/aim上書き + GAP-05 velocityゼロ化 + LOCAL空間
  input_inject.{h,cpp}     ~280行  [C+G] CA適用 と 非CAフォールバック（同一責務の2経路）
  capture.h                        （現状のまま）
  capture.cpp              ~500行  [K] OpenXR側追跡 + snapshot + ハンドオフ + per-API分岐
  capture_vulkan.cpp       ~750行  [V] Vulkanバックエンド（新規TU、手本パターンに合流）
  capture_backends.h       ~80行   Vulkanエントリを追記（下記 3.2）
  capture_d3d11.cpp / capture_d3d12.cpp / control_channel.{h,cpp}  （無変更）
```

CMakeLists.txt はソース列挙に 4 ファイル追加するだけ（layer/CMakeLists.txt:37-44）。

### 3.2 capture: Vulkan を手本パターンに合流させる（並行システムではない証明）

これは新機構ではなく、**capture_backends.h 自身が「あるべき」と宣言している形の完成**である
（capture_backends.h:4-9 「Vulkan stays inline in capture.cpp because ...」の理由は MSAA/HDR を
共有 readback 経路が持つためだったが、その経路自体が Vulkan 専用コードなので TU ごと動かせる）。

`capture_backends.h` に追記（D3D11/12 と同じ流儀: 平坦・プリミティブ引数・JSON返し）:

```cpp
// ---- Vulkan ----
// binding は XrGraphicsBindingVulkanKHR から。アプリ所有・非ref-hold（D3D と同じ契約）。
void VulkanSetBinding(void* vkInstance, void* vkPhysicalDevice, void* vkDevice,
                      uint32_t queueFamilyIndex, uint32_t queueIndex);
void VulkanFree();  // セッション破棄時。in-flight fence 待ち(GAP-07(c))を内包
nlohmann::json VulkanReadbackToPng(uint64_t imageHandle, int64_t vkFormat, uint32_t sampleCount,
                                   int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                   const std::string& eye, int viewIndex);
// 深度は Vulkan のみ実装（nice-to-have, CLAUDE.md）。平坦引数に EndFrameSnapshot::View の
// 深度フィールド(minDepth/maxDepth/nearZ/farZ)を展開して渡す。
nlohmann::json VulkanReadbackDepthToPng(uint64_t imageHandle, int64_t vkFormat, uint32_t sampleCount,
                                        int32_t x, int32_t y, int32_t w, int32_t h,
                                        uint32_t arrayIndex, float minDepth, float maxDepth,
                                        float nearZ, float farZ);
```

- capture.cpp 側: `CaptureOnCreateSession` の Vulkan 分岐（1095-1101）が `VulkanSetBinding(...)` を
  呼ぶ形になり、D3D11/12 分岐（1102-1110）と**完全に対称**になる。`CaptureOnEndFrame` の
  ディスパッチ（1294-1316）も 3 API とも同形になる。
- 深度パス生成は `NextColorCapturePath()` と対で `NextDepthCapturePath()` を capture.cpp に置き、
  三重化（§1.4-3）を解消する（counter/dir の所有は capture.cpp のまま）。
- 移動のみ: LoadVulkan / VkFns / Ensure* / Free / HalfToFloat / LinearToSrgb / 深度線形化は
  そのまま capture_vulkan.cpp の匿名 namespace へ。**アルゴリズム・バリア・フォーマット対応は不変**。

### 3.3 input系の分割 — 各モジュールの中身と「既存機構の整理」であることの根拠

**共通原則（capture_backends の手本から引き継ぐもの）**:
フックは openxr_agent_layer.cpp に残り「記録/上書きモジュール関数を呼んでから forward」する薄い形
（現在の capture フック群 832-958 が既にこの形）。モジュールは平坦な自由関数 + TU内状態で、
クラスも仮想関数も導入しない。

#### (1) `layer_log.{h,cpp}` — [A]
- 中身: `vr_agent::LayerLog` と log stream（42-68 の移動）。
- 根拠: 3 TU が extern 重複宣言している既存関数のヘッダ化。新機構ゼロ。

#### (2) `layer_dispatch.{h,cpp}` — [B]
- 中身: `LayerDispatch` 構造体、`g_dispatch`（公開は `const LayerDispatch& Dispatch()` アクセサ）、
  `g_instance/g_session/g_ca_enabled` の getter/setter、`RebuildLayerDispatch/ClearLayerDispatch`、
  `ToPath/PathToStr`。
- ヘッダに明文化する不変条件: 「ホットフックからの読みは無ロック。単一インスタンスモデル
  （create/destroy とフックは同時に走らない）が前提」（現コメント 89-93 の移設）。
- 根拠: GAP-07 で導入済みの機構そのもの。negotiate/GIPA/CreateApiLayerInstance は
  openxr_agent_layer.cpp に残す（レイヤの「顔」であり dispatch 表とは別責務）。

#### (3) `action_registry.{h,cpp}` — [F]
- 中身: `ActionSetReg/BindingReg/ActionReg/ActionSpaceInfo` 型、
  `g_action_sets/g_actions/g_attached_action_sets/g_action_spaces/g_grip_pose_actions/
  g_aim_pose_actions/g_grip_to_aim(+valid)`、記録関数（フックから呼ばれる
  `RegistryRecordActionSet/Action/Bindings/ActionSpace/Attach`、削除系 `RegistryEraseSpace/ActionSet`、
  クリア系 `RegistryClearSessionScoped/RegistryClearInstanceScoped`）、GAP-06 の
  `HandTopFromBindingPath/InferHandTops`、`BuildActionsJson`。
- **`std::mutex& ActionMutex()` を公開する**。pose_override と input_inject のフォールバックが
  同じ mutex を要求するのは GAP-08 で確立済みの設計（695-697「別ロックは順序逆転」）であり、
  **ロック設計の変更はスコープ外**。ヘッダに 3 つの規律（§1.4-6）を集約記載し、
  「PRECONDITION: caller holds ActionMutex()」系の既存コメント規約をそのまま維持する。
- 根拠: 記録フック 6 本（1362-1503）と dump（1509-1547）は既に「観測して転送するだけ」の
  独立機構。移動のみ。

#### (4) `pose_override.{h,cpp}` — [D+E]
- 中身: クォータニオン/ベクトル数学（333-346）、`g_view_spaces/g_ref_space_types` + mutex と
  `IsViewSpace/DescribeRefSpace/RecordRefSpace/EraseSpace`、`EnsureLocalSpace`（g_local_space 所有）、
  `RebaseViewsToHead`、`TransformLocalPoseToSpace/TransformHeadToSpace`、
  `ResolveGripToAimOffset`（GAP-04）、`ApplyPoseOverride`（GAP-06 含む）、
  `ApplyHeadToLocation/ZeroVelocity/FindInNextChain`（GAP-05）、warn-once フラグ群 +
  `PoseOverrideResetWarnings()`（xrDestroyInstance から呼ぶ — 現在の 1129-1133 の置き換え）。
- D と E を 1 モジュールにする理由: 両者は `TransformLocalPoseToSpace`・数学ヘルパ・
  `EnsureLocalSpace`・ZeroVelocity を共有しており（378-419 ⇔ 661-681、1255-1311）、分けると
  ヘッダ経由の相互依存が増えるだけ。**「LOCAL 空間で注入されたポーズを、要求された空間の
  locate 結果に上書きする」という単一の責務**である。
- 根拠: xrLocateViews/Space/Spaces フックは残し、その中の「上書きロジック」だけが移る。
  capture フックが `CaptureOnEndFrame` を呼ぶのと同型。

#### (5) `input_inject.{h,cpp}` — [C+G]
- 中身: `ApplyPendingInputs`（CA 経路, 240-301）と GAP-08 フォールバック一式
  （`FallbackActionState/g_fallback_states/g_synced_active_sets/g_emulated_profile/
  g_pending_ip_event`、`ApplyFallbackSync/AggregateFallback/ActionsBoundTo`、
  `FallbackOverride{Bool,Float,Vector2f}State`（GetActionState フック本体 981-1026 の中身）、
  `FallbackCurrentProfile/FallbackTakePendingIpEvent/FallbackArmIpEvent`、
  クリア系 `FallbackClearSessionScoped/InstanceScoped`）。
- C と G を 1 モジュールにする理由: 両者は**同一責務「button/analog 注入」の CA/非CA という
  2 経路**であり、Hook_xrSyncActions（960-971）が既に `if (g_ca_enabled) ApplyPendingInputs else
  ApplyFallbackSync` と対で分岐している。分けると「入力注入とは何か」が 2 ファイルに割れる。
- フォールバック状態は `ActionMutex()` 下（現状維持）。ヘッダに明記。
- 根拠: GAP-08 で追加済みの機構の移動のみ。

#### (6) openxr_agent_layer.cpp（残り）
- negotiate（1743-1781）、GIPA テーブル（1552-1631）、CreateApiLayerInstance / CA 有効化判定
  （1660-1733）、全 Hook_xr*（各フックは try/catch + 記録/上書き関数呼び出し + forward の薄い形）、
  DestroySession/DestroyInstance のクリーンアップは各モジュールの Clear 関数の列挙になる。
- 例外境界（「例外は ABI を越えない」規約, CMakeLists.txt:61-62）は**フック側に残る**ので、
  モジュール関数は現在の内部関数と同じく「フックの try/catch に守られて呼ばれる」契約のまま。

### 3.4 明示的に「しない」設計判断（金メッキ防止線, §6 と対）

- モジュール間を interface クラスで抽象化しない（バックエンド差し替えのような可変性は存在しない）。
- `g_action_mutex` の分割・細粒度化をしない（71a9a16 の教訓: ロック順序はレビュー済みの現状が正）。
- handle→state を per-instance 化しない（74-82 の設計判断を維持）。
- 匿名 namespace の TU 内状態はそのまま TU 内に置き、ヘッダには関数と必要な型だけ出す。

---

## 4. 段階的実行計画

各フェーズ共通:
- **完了条件**: `mingw32-make -C layer/build vr_agent_layer` 成功 + `scripts/integration_test.sh` 14/14 PASS。
- **コミット粒度**: 1 フェーズ = 1 コミット（`refactor(...)`）。
- **ロールバック**: 該当コミットを `git revert`（各フェーズは後続に依存されるが先行には依存されないため、
  末尾から順に revert 可能。途中フェーズ単独の revert が必要になった場合は後続フェーズも同時に revert）。
- **diff 検査**: 各フェーズのレビュー観点は「移動 + 配管（include/宣言/呼び出し置換）以外の変更が
  含まれていないか」。ロジック行の変更はゼロであること。

| # | フェーズ | 内容 | 新規/変更ファイル | リスク |
|---|---|---|---|---|
| 0 | ベースライン | 現状の build + 14/14 を実測し記録（着手前提条件） | なし | なし |
| 1 | log 抽出 | [A]→`layer_log.{h,cpp}`。capture.cpp:45-46 / control_channel.cpp:27-32 の extern 宣言を include に置換。`LayerBuildActionsJson` ブリッジ宣言も暫定でここに同居させ重複解消（Phase 4 で action_registry.h へ移す） | +2, 変更3 | 極小（ウォームアップ） |
| 2 | Vulkanバックエンド抽出 | [V]→`capture_vulkan.cpp`、capture_backends.h に §3.2 のエントリ追記、capture.cpp の Vulkan 分岐を平坦呼び出しに置換、`NextDepthCapturePath()` 追加 | +1, 変更3 | 中（最大の移動量だが独立性最高） |
| 3 | dispatch 抽出 | [B]→`layer_dispatch.{h,cpp}`。全フックの `g_dispatch.xxx` 読みを `Dispatch().xxx` に置換 | +2, 変更1 | 小（機械的置換） |
| 4 | レジストリ抽出 | [F]→`action_registry.{h,cpp}`。記録フック 6 本の中身を Registry* 呼び出しに置換、`ActionMutex()` 公開、Destroy 系クリーンアップを Clear 関数化 | +2, 変更2 | 中（mutex 共有の明示化） |
| 5 | pose 抽出 | [D+E]→`pose_override.{h,cpp}`。locate 系フック 3 本 + CreateReferenceSpace/DestroySpace の中身を置換、warn フラグを ResetWarnings() 化 | +2, 変更1 | 中（最重要経路。integration の view-override テストが直接検証） |
| 6 | inject 抽出 | [C+G]→`input_inject.{h,cpp}`。SyncActions/GetActionState*/PollEvent/GetCurrentInteractionProfile/AttachSessionActionSets の中身を置換 | +2, 変更1 | 中（非CA経路は §7-3 の通り静的検証のみ） |

フェーズ順の根拠: 依存の葉（log, Vulkan バックエンド）→ 基盤（dispatch）→ 状態所有者（registry）→
その消費者（pose, inject）。Phase 4 が 5・6 の前提（ActionMutex() と registry query を提供）。

---

## 5. 近未来要件の受け皿（設計のみ・今回は実装しない）

### 5.1 durationMs 補間の載り方

新設は小さな評価器モジュール 1 つ（`pose_animator.{h,cpp}`）＋既存構造体へのフィールド追加のみ:

```cpp
// pose_animator.h（形のみ; リファクタ後に実装）
namespace vr_agent {
struct EvaluatedPose {
  XrPosef pose;                  // 現在時刻で評価済みのポーズ (LOCAL)
  XrVector3f linVel, angVel;     // 補間中は有限差分/解析微分、静止時はゼロ (GAP-05 の一般化)
  bool animating = false;        // 補間中か（velocity の意味付けに使用）
};
// 時間源: フックがキャッシュした最新の XrTime。
//   - Hook_xrEndFrame: frameEndInfo->displayTime（既に傍受済み, openxr_agent_layer.cpp:949）
//   - Hook_xrLocateViews: viewLocateInfo->displayTime（同 1162-1163）
// を NoteDisplayTime(XrTime) で渡す。xrWaitFrame の新規フックは不要（実装時に不足なら追加検討）。
void AnimatorNoteDisplayTime(XrTime displayTime);
// StickyPose/HeadPose（control_channel.h）に optional な durationMs が付いた時、
// 「前回評価値 → 目標」の補間状態を animator が所有し、評価して返す。
EvaluatedPose AnimatorEvalController(const StickyPose& target, XrTime now);
EvaluatedPose AnimatorEvalHead(const HeadPose& target, XrTime now);
}
```

呼び出し点（すべて既存経路への差し込みで、並行システムなし）:
- `input_inject.cpp / ApplyPendingInputs` の sticky 再適用ループ（現 286-297）:
  `sp` を直接使う代わりに `AnimatorEvalController(sp, now).pose` を注入。
- `pose_override.cpp / ApplyPoseOverride`（現 626-639 の sticky 読み）: 同上。
- `pose_override.cpp` の頭部経路（`ControlChannelGetHead` の 3 呼び出し点）: `AnimatorEvalHead`。
- MCP 側: `vr_set_controller` / `vr_set_hmd` に `durationMs` パラメータ追加 →
  control channel の `pose` / `head` コマンドにフィールド追加（transport は素通しするだけ）。
  sticky ストアが control_channel に残っていて良い理由: ストアは「目標値」を持ち、
  補間状態（開始ポーズ・開始時刻）はレイヤ側 animator が所有する、という分担が自然なため。

### 5.2 実 velocity（任意拡張）の載り方

GAP-05 の `ZeroVelocity`（pose_override.cpp に移動済み）呼び出し点
（Hook_xrLocateSpace: 1305-1311 / Hook_xrLocateSpaces: 1349-1352 相当）で、
`EvaluatedPose.animating` なら `linVel/angVel` を書き、静止時は従来どおりゼロ。
lerp/slerp は閉形式で微分できるため有限差分すら不要（実装時の選択肢）。
**「velocity ゼロ化」という既存機構の一般化**であり、新経路を作らない。

---

## 6. スコープ外（このリファクタでやらないこと）

1. **エンジン固有コード一切**（Unity/Unreal/Godot プラグイン、エンジン統合テスト） — CLAUDE.md の第2軸。
2. **multi-instance / handle→state レジストリ化** — openxr_agent_layer.cpp:74-82 で「やらない」と
   明記済みの設計判断。単一被験アプリが north star。
3. **ロック設計の変更**（ActionMutex の分割、lock-free 化、dispatch 読みのロック化）。
4. **クラス/仮想インターフェースによる抽象化**（capture backend も input モジュールも平坦関数のまま）。
5. **control_channel / MCP サーバの再構成**（§2.3）。プロトコル・ツール仕様も不変。
6. **動作変更を伴う一切の「ついで修正」**: フォーマット対応の追加、深度 MSAA、トーンマップ演算子、
   タイムアウト値変更、ログ文言変更を含む。気づいた改善点は本計画書末尾に追記して据え置く。
7. **リネーム/スタイルだけの churn**（既存の命名・コメント規約をそのまま移す）。
8. **ユニットテスト基盤の新設** — 検証契約は integration_test.sh 14/14（§7）。テスト追加は
   リファクタと独立に判断すべき別案件。
9. **capture_vulkan.cpp 内の色/深度 submit-wait-map 系の重複統合**（§1.4-3 のような機械的重複は
   解消するが、GPU コマンド列の共通化は挙動リスクがあるため移動のみ。将来の候補として記録）。

---

## 7. リスクと検証

### 7.1 フェーズ共通の機械的検証

1. `mingw32-make -C layer/build vr_agent_layer`（DLL リンクまで成功。MinGW static link オプション
   CMakeLists.txt:63-65 は不変）。
2. `scripts/integration_test.sh` → **14/14 PASS**（profile/binding 傍受・xrLocateViews 上書き・
   sync 往復・非退化キャプチャ）。Meta XR Sim の SES 競合による flake（メモリ参照）に注意:
   fail 時はまず再実行して flake と regression を区別する。
3. `mcp/` は無変更のため `npm run build` は Phase 0 と最終確認時のみ。
4. diff レビュー: 「移動 + 配管」以外の行が無いこと（§4 冒頭）。

### 7.2 フェーズ別の固有リスク

| フェーズ | リスク | 緩和 |
|---|---|---|
| 2 (Vulkan) | `EndFrameSnapshot::View` → 平坦引数への展開ミス（特に深度の minDepth/maxDepth/nearZ/farZ と MSAA の x/y/arrayIndex 非対称性, capture.cpp:658-703 の注意書き） | integration の非退化キャプチャテストが直接検証。深度は手動 E2E（`vr_screenshot withDepth:true`）を Phase 2 完了時に 1 回実施 |
| 3 (dispatch) | 関数 static → アクセサ化の置換漏れ | ビルドエラーで機械的に検出（g_dispatch を TU 外から見えなくする） |
| 4 (registry) | mutex 共有の暗黙依存の見落とし（PRECONDITION コメント付き関数の呼び出し規約） | 既存コメントをヘッダへ全数移設し、呼び出し側の lock_guard 位置を diff で照合 |
| 5 (pose) | head/controller 上書きはコアループの心臓部 | integration に xrLocateViews 上書き・sync 往復の直接アサーションあり。加えて手動で `vr_set_controller`→`vr_screenshot` の目視 1 回 |
| 6 (inject) | **非CA フォールバック経路は runtime-exercised でない**（Meta sim は CA あり; openxr_agent_layer.cpp:695-697 に明記） | 現状と同じ制約。移動のみ・ロジック不変を diff で担保。`VR_AGENT_NO_CA=1` での起動スモーク（クラッシュしないこと）を Phase 6 完了時に 1 回実施 |

### 7.3 全体リスク

- **ODR / リンケージ**: 匿名 namespace の状態を TU 間で共有しない（共有するものだけアクセサ関数化）。
  同名グローバル（`g_mutex` は capture.cpp / control_channel.cpp / openxr_agent_layer.cpp に各自存在）は
  各 TU の匿名 namespace に留まるため衝突しない。
- **例外境界**: モジュール関数はフックの try/catch 内から呼ばれる契約を維持（§3.3-6）。
  モジュール側に新しい catch を足さない（二重化は挙動変更）。
- **スケジュールリスク**: 各フェーズ独立のため、途中で止めても「そこまでの分割が済んだ緑の master」が
  残る。全フェーズ完了を durationMs 実装の前提にはするが、Phase 2（capture）だけは
  durationMs と無関係なので、時間都合で最後に回してもよい（依存なし）。

---

## 付録: 検証コマンド一覧

```bash
# ビルド
mingw32-make -C layer/build vr_agent_layer
# 統合テスト（14/14 必須）
scripts/integration_test.sh          # 既定 Vulkan
scripts/integration_test.sh D3D11    # Phase 2 完了時に追加確認推奨
scripts/integration_test.sh D3D12    # 同上
# MCP（Phase 0 / 最終のみ）
cd mcp && npm run build
```
