# Review checklist — 再発レビュー指摘の横断レンズ

h-review が h-reviewer への委譲時に渡す。各レンズは実際に起きた指摘・事故が出典(推測由来なし)。
キャプチャ実装固有の罠は h-reviewer 自身の agent-memory(review-checklist-frame-capture)にあり、ここには
タスク種を問わない横断レンズのみを置く。

1. **コード移動を見たら、コメントと依存宣言の追従を必ず確認する。**
   関数を動かしたら説明コメントも一緒に動いたか(孤立コメントが別物を指していないか)。移動で「最後の
   使用者」が消えた using / `#include` を grep で全数確認したか。
   逆方向も同じ: **共有ソースへ新しい依存を足したら、それを自前コンパイルする全ターゲット
   (playspectra_test 等)のソース列が追従しているか**。本体ビルドだけの DoD はテストターゲットの
   リンク切れをすり抜ける — テストターゲットのビルド+実行を DoD に含める。
   **API リネームを伴う move-only リファクタ**では、sed で関数呼び出しを置換した後に
   `grep '#include "旧ヘッダ"'` で全消費者を全数確認する(コメント部分がリネーム済みで見た目 OK でも
   #include 行が旧ファイル名のまま残る — transitive include で偶然コンパイルが通る脆い状態)。
   **Write で新ファイルを作成する際は、元のコメントを含めて verbatim コピーする**(言語非依存。
   TS でも C++ でも同じ罠が再発する — コード本体だけコピーしてコメントを落とすと WHY が失われる)。
   — 出典: journal L29 (R06, コメント孤立), L39 (R05, dead using 5本), L43 (R04, dead include。**2タスク連続で再発**), L67 (R10, capture_common→pixel_convert 依存追加で playspectra_test リンク切れ), L88 (R19, API リネーム後の dead include。**4回目**), L90 (R20, TS 分割でコメント40行欠落。**5回目・言語非依存で再発**)

2. **レビュアー間・報告間で事実が食い違ったら、一次ソース(grep/実行)で裁く。**
   「他で使われている」「使われていない」の類いは主張の多数決でなく ground truth を自分で取る。
   — 出典: journal L40 (R05, correctness と readability の事実対立を main loop が grep で裁定)

3. **偽前提の是正は、その語をリポジトリ/文書全体で grep してから閉じる。**
   1箇所直して残りが古い前提のまま(特に後続タスクへの指示文)は再発源。完了条件は「grep 0件(許容形除く)」。
   **grep の完了確認は gitignore 対象(`.claude/` 等)を明示 path で別途走らせる** — ripgrep/Grep tool は
   gitignore をスキップするため、リポジトリルート grep だけでは「残存0」を false 0 で誤判定する。
   — 出典: journal L53 (M2→M3, hellovr_dx11 が M2節修正後も3箇所残存しレビュアーが検出), 2026-07-22
   (M0.5 改名の「grep 0件」が .claude/rules+agent-memory の vr_agent_* を見逃した。根因=ripgrep の
   gitignore スキップ [[review-env-sandbox-quirks]] L15)

4. **文書中の数値は、引用元の生データから機械カウントして照合する。**
   目視集計の転記は数え間違う。後続タスクがその数値を前提にするなら特に。
   — 出典: journal L57 (M3, 「45アクション」→ json.load 実カウントで 42)

5. **実アプリ結合の完了判定は機械値で行う。プロセス生存・初期化ログ1行は根拠にしない。**
   無音のモーダルやアイドル状態は「生きているが動いていない」偽陽性を作る。この repo なら
   framesObserved>0 等のカウンタを使う。
   — 出典: journal L55 (M2/M3, hellovr が SDL モーダルで 0 フレームのまま「15秒生存」を稼働と誤認)

6. **テストが検証できない項目は明示 SKIP(理由出力+サマリ計上)。無言の素通しも、rc への混入も不可。**
   rc は FAIL のみで決める。SKIP が PASS に化ける経路がないかをレビューで確認する。
   逆に、一度 PASS が実証された SKIP 許容項目は FAIL 昇格を検討する(退行検知を殺さない)。
   — 出典: journal L58 (M4, 注入到達を明示SKIP), M4 review 観察 (graceful ゲートが FAIL になり得ない設計への指摘)、
   2026-07-24 (h-loop外: scripts/run_all_tests.sh の初版が python/gcc/submodule 欠如時の SKIP を無言で
   `ALL GREEN` に含めていた。h-reviewer 委譲が無く機械チェックを素通り、ユーザーの `/codex:review` で検出。
   SKIP 明示計上 = `ALL GREEN`/`GREEN WITH SKIPS` の区別へ修正)

7. **文書・仕様・README 中の技術主張は「リポジトリ内の実測ログ/一次ソースを指せるか」で仕分ける。**
   指せない主張は学習知識由来の仮説であり、仮説ラベル+検証方法なしで事実の顔をして書かれていたら
   needs-fix。特に注意: ランタイム/外部システムの挙動主張、「〜が対応している」「〜すれば動く」系の断言。
   開発中設計と実装済み機能の混在も同罪(README は ✅実装+自動テスト済み / 🟡実機で部分検証済み /
   📋設計・開発中 の3段階で検証境界を明示する)。仕様の決定は事実の上にだけ置き、仮説上の実装は
   「検証装置」と明示する(CLAUDE.md「検証済みと未検証を混ぜない」節)。レンズ2〜4が「既にある事実の
   扱い」を見るのに対し、これは「そもそも事実か仮説かの分類」を見る。
   — 出典: journal 2026-07-18 (同一セッション3回再発: H1役割割当を仕様書に「決定」と記載 / README で
   Monado CA を実測と真逆に記載+開発中設計を実装済みと混在 / VD2 で H3 偽装を検証装置と明示せず実装。
   ユーザー指摘2回 → CLAUDE.md「検証済みと未検証を混ぜない」節新設)

   **変種(ブランチ跨ぎ)**: backlog/status 文書の「実装済み」はブランチ限定の主張である。ある機能が別ブランチ
   で実装されていても、それを今のブランチの記述にそのまま書かない——コミット時点では真でも、ブランチが
   分岐すれば今のブランチにはその実装が存在しない(挙動が変わっていなくても主張が偽になる)。ブランチを
   またぐ言及は明示的にブランチ名を書き、`git log`(最終変更コミット・分岐点)で裏取りしてから書く。
   — 出典: 2026-07-24 (backlog `mcp-apps-widget` が「vr_view_recording新設…実装済み」と記載していたが、
   実装は別ブランチ `feat/mcp-recording-viewer-widget` のみに存在し本ブランチ `feat/mcp-server` には無かった
   〈`mcp/src/tools/recording.ts` は `vr_start_recording`/`vr_stop_recording` のみ〉。Codex review が指摘)

8. **git status / .gitignore の可視性を額面どおり信じない — 未追跡ツリーと ignore 範囲は明示コマンドで確かめる。**
   (a) 素のディレクトリ名 ignore パターン(`build`/`bin` 等)は**全階層マッチ**で想定外の dir を無音で隠す。
   最上位限定は `/build/` と錨を打ち、ignore 範囲は `git check-ignore -v <path>` / `git status --ignored` で確認する。
   (b) git は**未追跡のディレクトリツリーを1エントリに畳む**ため、その下の入れ子 source は ignore 済みサブdir の
   陰に隠れる。新規ツリー追加時は `git add -n <dir>` / `git ls-files --others <dir>` で全数列挙し、**最初のコミット
   後にもう一度 git status を見る**(親を追跡した瞬間に隠れていた入れ子が露出する)。レンズ3が「grep の false-0
   (gitignore スキップ)」を見るのに対し、これは「git 自身の status/ignore 表示の死角」を見る。
   — 出典: 2026-07-23 (cbeb7e9d4: 素の `build` が最上位 build/ [Monado+proto test] を隠蔽、除去して初めて露出 /
   9a35e9be9: driver 入力プロファイル driver/playspectra/resources/ が親 driver コミット後に露出 = 危うく
   スケルトンを入力プロファイル欠落のまま出荷する near-miss)
