---
paths:
  - "layer/src/**"
---

# Windows ネイティブ interop の実測済み罠 (layer/src)

出典は 2026-07-16 M0 (d3d11-flat-capture) の実測と独立レビュー。推測由来の項目はない。

## D3D11 共有スワップチェーン画像の keyed mutex

- OpenXR ランタイムの D3D11 スワップチェーン画像は `MiscFlags=0x100`
  (**D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX** — 列挙名にアンダースコアなし。`..._KEYED_MUTEX` はコンパイルエラー)
  の共有テクスチャ(metasim / Monado の両方で実測)。
- mutex 未取得の読み(CopySubresourceRegion 等)は**未定義=全ゼロ**を返す。エラーにならないので
  「無言の壊れた画像」になる — 必ず QI IDXGIKeyedMutex → `AcquireSync(0, timeout)` → コピー発行 →
  `ReleaseSync(0)` → `Release()` の順で挟む(Map はコピー後なら mutex 解放後でよい)。
- **`AcquireSync` の戻り値は `hr == S_OK` の厳密比較のみ正しい。** WAIT_TIMEOUT(0x102) と WAIT_ABANDONED は
  SUCCEEDED() 扱いの HRESULT。`SUCCEEDED(hr)` ガードはバグ。
- 取得に失敗した mutex に ReleaseSync を呼ばない。失敗経路は明示エラー JSON(CLAUDE.md)。
- この機構は **D3D11 固有**。capture_d3d12(フェンス共有)/capture_vulkan(external memory)に同種の同期を
  「予防的に」足すのは誤り(レビューでも false finding と判定済み)。

## MinGW ビルドの流儀

- COM の IID は `IID_Xxx` シンボル直接参照(CMake で dxguid リンク済み)。`__uuidof` は使わない。
- レイヤーの単独リビルドは `cmake --build layer/build --target vr_agent_layer`(ゾンビ hello_xr.exe が
  exe の書込ロックを持ち全体ビルドの relink を塞ぐことがある。実行は可能・再起動で解消)。
- 実行時に読まれる DLL は `layer/manifest/` のコピー(POST_BUILD で同期)。ビルドログの
  「Syncing vr_agent_layer.dll next to the loader manifest」で同期を確認できる。
- **kill 不能ゾンビが exe をロックしていても、実行中 exe のリネームは通る**(上書き cp は
  "Device or resource busy"、mv は成功 — NTFS は open 中でも rename 可)。デプロイは
  `mv old.exe old.exe.z && cp new old.exe`、退避ファイルはプロセス消滅後に削除
  (third_party は gitignore 済みなので残っても無害)。 — 出典: journal L62 (R08〜R17 で4回実証)
