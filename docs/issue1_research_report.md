# VR-MCP: Meta XR Simulator 依存脱却のための調査と仮設計

Issue #1 にて提起されている、Meta XR Simulatorからの依存脱却、およびヘッドレスで動作する代替OpenXRランタイムの調査・検証結果、ならびに自作ランタイム（ダミーランタイム）の仮設計について報告します。

## 1. Meta XR Simulator依存をなくすにはどうすればいいか

現在、VR-MCPは `XR_RUNTIME_JSON` 環境変数を通じて Meta XR Simulator を直接指定しています。この依存をなくし、かつ「単独で動くアプリケーションとしての使い勝手」を向上させるためには、以下の2つのアプローチが考えられます。

*   **アプローチ A: 既存のヘッドレス/ダミーランタイムを内包・利用する**
    *   Meta XR Simulatorに代わり、OSの制約（特にLinux CI環境）を受けにくいオープンソースのランタイムをバンドルするか、利用者が容易にセットアップできる仕組みを提供します。
*   **アプローチ B: VR-MCP 独自のダミー OpenXR ランタイムを実装する**
    *   VR-MCPをAPIレイヤーとしてではなく、「OpenXRランタイムそのもの」として振る舞うように実装します（あるいは、最小限のダミーランタイムをC++で記述し、VR-MCPに内包する）。
    *   これにより、外部のランタイム（MetaやSteamVRなど）に一切依存せず、完全にポータブルな自動テスト基盤を提供できます。

## 2. 代替となるヘッドレスOpenXRランタイムの調査・検証

CI（Linux/Windows環境）での自動テスト用途に耐えうる、ヘッドレスなOpenXRランタイムの候補を調査しました。

### 候補1: Monado (オープンソース OpenXR ランタイム)
MonadoはFreedesktop.orgがホストするオープンソースのOpenXRランタイムです。
*   **実用性:** 実用に耐えうると考えられます。
*   **特徴:** Linux環境ではデファクトスタンダードであり、Wayland/X11なしで動作するヘッドレス/ダミードライバ（`dummy` driver）が利用可能です。
*   **課題点:**
    *   Windows版ビルドは可能ですが、公式のビルド済みバイナリ（インストーラ）の提供が弱く、内包するには自分たちでWindows/Linux両方のバイナリをビルド・管理する手間がかかります。
    *   機能がリッチすぎるため、単なる「画面なしの自動テスト」のためだけに同梱するにはファイルサイズや依存ライブラリ（Vulkan等）のフットプリントが大きくなる可能性があります。

### 候補2: OpenXR-SDKの "hello_xr" や "dummy runtime" の拡張
Khronos公式の `OpenXR-SDK` や `OpenXR-SDK-Source` には、最小限の実装例やダミーが用意されていることがあります。
*   **実用性:** そのままでは実用不可ですが、改造ベースとしては優秀です。
*   **特徴:** 描画処理を完全にスキップ（Nullスワップチェーン）するだけの最小構成のランタイムを作ることができます。

### 代替調査の結論
**Monadoのダミードライバ運用** が最も確実な既存の代替手段です。しかし、VR-MCP（Playwright for VR）としての最終的な理想形「依存関係ゼロで、npm install 一発でどこでも動く（ランタイム内包）」を目指すのであれば、既存の巨大なランタイムを使うのではなく、**VR-MCP専用の最小ダミーランタイム（アプローチB）を作成する** のが最適解と考えられます。

## 3. 代替（ダミーランタイム）を自作する場合の仕様と機能一覧

もしVR-MCPに内包するための独自の「ヘッドレスダミーOpenXRランタイム」を実装する場合、アプリケーション（Unity/Unreal等）をクラッシュさせずに動作させるために、最低限以下の仕様とAPIを満たす必要があります。

### 3.1 必須となるコア機能仕様
VR-MCPのAPIレイヤーがフックして状態を制御するため、ダミーランタイム自体は「何も描画せず、何もせず、常に成功（`XR_SUCCESS`）を返す」だけの薄いラッパーで構いません。

*   **インスタンス・セッション管理:**
    *   `xrCreateInstance`, `xrDestroyInstance`
    *   `xrCreateSession`, `xrBeginSession`, `xrEndSession`, `xrDestroySession`
    *   セッション状態（`XR_SESSION_STATE_READY`, `XR_SESSION_STATE_FOCUSED` など）の遷移をシミュレートするイベントループ機能（`xrPollEvent` でダミーイベントを返す）。
*   **システムプロパティとViewの設定:**
    *   `xrGetSystem`, `xrGetSystemProperties` (HMDの解像度やFoVのダミー値を返す)
    *   `xrEnumerateViewConfigurations`, `xrEnumerateViewConfigurationViews` (ステレオ2眼のダミーView情報を返す)
*   **スワップチェーン（画像バッファ）のモック:**
    *   `xrCreateSwapchain`, `xrEnumerateSwapchainImages`
    *   実際にGPUメモリ（Vulkan/D3D11/D3D12）を確保する必要はなく、ダミーのテクスチャハンドルを返すか、エラーにならない最小限のメモリバッファを割り当てる。
    *   `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage` は即座に成功を返す。
*   **フレームループ（タイミング制御）:**
    *   `xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`
    *   `xrWaitFrame` では、設定されたリフレッシュレート（例: 90Hz）に合わせて適切なスリープを挟み、フレームタイムをアプリケーションに通知する。
*   **インタラクションプロファイルと入力（アクション）:**
    *   `xrSuggestInteractionProfileBindings`, `xrCreateActionSet`, `xrCreateAction`
    *   VR-MCPレイヤー側で入力をオーバーライドするため、ランタイム側は登録を受け付けるだけでよい。
    *   `xrSyncActions`, `xrGetActionState*` はデフォルト値（またはVR-MCPレイヤーから流し込まれた値）を返す。
*   **スペース（トラッキング空間）:**
    *   `xrCreateReferenceSpace`, `xrCreateActionSpace`, `xrLocateSpace`
    *   HMDやコントローラの座標系。常に固定のアイデンティティ行列、またはVR-MCPから指定された座標を返す。

### 3.2 実装アプローチ
C++ で作成し、Windows用には `.dll` とマニフェストJSON、Linux用には `.so` とマニフェストJSONをビルドします。これを VR-MCP のパッケージ内に同梱し、実行時に `XR_RUNTIME_JSON` 環境変数をこの同梱ランタイムのJSONに動的に設定するスクリプトを記述することで、「ユーザーはMeta XR Simulatorをインストールすることなく、完全ヘッドレスでVR-MCPを利用できる」ようになります。
