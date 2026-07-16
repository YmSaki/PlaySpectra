---
paths:
  - "scripts/**"
---

# setup / テストスクリプトの規約 (scripts/)

出典は 2026-07-16 の M1〜M3 実測とレビュー。third_party/ は「スクリプトで再現するもの」(.gitignore の思想)。

1. **ダウンロード物には機械検査のバックストップを必ず置く。**
   `curl -sL` は HTTP 404 でも rc=0 で HTML をファイルに書く(文書化仕様)ため `set -eu` では捕まらない。
   PE ヘッダ検査(MZ/PE\0\0/machine)、サイズ、フォーマット検査などを DL 直後に入れる。
   新規スクリプトでは `curl -fSL` を使う(既存の -sL 統一は backlog setup-curl-fsl)。
   — 出典: setup_opencomposite.sh の PE 検査 + M1 review

2. **MSVC 系のビルド木は短パス(%TEMP% 直下)に置く。**
   深い階層(scratchpad 等)は MSBuild FileTracker が FTK1011 で死ぬ(MAX_PATH)。
   — 出典: hello_xr MSVC ビルド(M0 前段)・hellovr ビルド(M2)

3. **サードパーティの clone はタグ固定し、タグをパスに含める**(例: `%TEMP%/vr_agent_openvr_v1.8.19`)。
   「最新 main」は互換性の時限爆弾(openvr master → OpenComposite 未実装版数で無音ブロック)。
   互換版数は推量でなく**一次ソース**(実装側の版数宣言 × ヘッダの Version 文字列)で決め、根拠を
   スクリプトのコメントに書く。VERSION.txt に取得元+タグ+日時を記録。
   — 出典: setup_hellovr.sh の v1.8.19 固定 (M3改, journal L56)

4. **実アプリを起動するテストの成立判定は機械値**(framesObserved>0、制御チャネル LISTEN 等)。
   プロセス生存時間・初期化ログ1行を判定に使わない。検証不能な項目は明示 SKIP(rc は FAIL のみで決める)。
   — 出典: journal L55/L58 (integration_openvr_test.sh が実装例)
