---
paths:
  - "**/CMakeLists.txt"
---

# CMake ビルド設定の規約 (全 CMakeLists)

出典は実測。推測由来の項目はない。

1. **新規 MSVC ビルドターゲットには `/utf-8` を必ず付ける。**
   このリポジトリのソースは UTF-8 の日本語コメントを含む。`/utf-8` なしの MSVC はソースをシステム ANSI
   コードページ(CI ランナー=CP1252、日本語開発機=CP932)として復号し、C4819 を出し、後続バイトの誤読で
   幻のカスケード parse エラーに化けることがある。`if(MSVC) add_compile_options(/utf-8) endif()`(プロジェクト
   全体)か、対象ターゲットへ `target_compile_options(<t> PRIVATE /utf-8)` で locale 非依存にする。
   MinGW/GCC は UTF-8 をネイティブ処理するので不要(MSVC 固有)。機械的 lint では捕まえにくいので規約で担保。
   — 出典: **2回発生** — Monado submodule cdd429f88 (drivers/targets)、layer/CMakeLists.txt 2026e1d7a
   (CI の windows-latest ランナー=非CP932 で C4819 を予防)
