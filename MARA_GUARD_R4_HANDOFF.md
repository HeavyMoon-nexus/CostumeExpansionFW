# r4 修正ハンドオフ — R3 レビュー残存 P1 の修正指示書

> 宛先: 修正実装者(Codex 想定)。レビュー3巡の文脈を持たない実装者が、
> **プロジェクト固有の不変条件を壊さずに** R3 残存指摘を修正できることを目的とする。
> 入力: [MARA_GUARD_ADVERSARIAL_REREVIEW_R3.md](MARA_GUARD_ADVERSARIAL_REREVIEW_R3.md)(全指摘**受理**)
> / 実装記録 [MARA_GUARD_IMPL.md](MARA_GUARD_IMPL.md)(§R/§R2 — §R2 の manifest 主張 2 点は
> R3 により**誤りと確定**、本文に撤回注記済み)。
> ベース: branch `mara-guard-v1.3.2` @ 本ハンドオフのコミット。
> 発行者(コンテキスト保持側)による検証済み判定: R3 の P1-1/P1-2 の事実主張は
> **全て実コードで追認済み**(独自検証: g_boxSpells は policy 変更で失効しない/
> mutator は PublishPolicy→WriteJson(=manifest 同期生成)→再評価の順/manifest の
> 独自 resolver は選択 ARMA を無検査で読む/PickAddonForPlayer は guard 前に
> race/additionalRaces を読む)。

---

## 0. スコープ判定(先に読むこと)

**本ラウンドのスコープ = R3 の P1-1・P1-2・P3-1 のみ。**

R3 §3 の P2-1(fake form graph テストハーネス)と P2-2(NIF 構造 validator)は
**本ラウンド対象外**とする。理由: これらは「他 mod 起因の類型 CTD をほぼ完全に防ぐ」
という基準(基準B)でのみリリース阻害であり、リリース基準の選択(下記)は
プロジェクトオーナーの未決事項。v1.3.2 の原初目的(基準A = MARA 型 runtime-form
事故の遮断)では R3 自身が「既知の picker 即 CTD 経路は大きく縮小・r2 防御は有効」
と認定している。P2 系を実装したくなっても本ラウンドでは着手しないこと
(必要なら TODO として文書に書くに留める)。

- 基準A: MARA 型(runtime/no-file form + deny 対象)事故の遮断 = **P1 修正+実機で出荷可**
- 基準B: 類型 CTD の広範防御 = P2-2 まで必要 → beta 線プロジェクト化が妥当

最小テストは P2-1 の全面ハーネスではなく「§3 の完了条件」の範囲で追加する。

## 1. 修正対象(必須・R3 §6 の 1-6 に対応)

### 1-A. safe resolver への一元化(R3 P1-1)

1. `PickAddonForPlayer()`(src/SkinRebind.cpp)の候補ループで、**race/additionalRaces を
   読む前に**候補 ARMA ごとの hard 判定を行う:
   `IsDynamicForm()`(formID 読みのみ)→ `GetFile(0)` null → `policy::PluginDenied`。
   不合格候補は race マッチング対象から**除外**(= admitted 候補だけで従来どおりの
   優先順位選択)。読み順が本質: 判定前に許されるのは formID と sourceFiles だけ。
2. 選択確定 ARMA へ **final-ARMA colon-id deny** も適用(`MakeColonId(arma)` →
   `policy::IdDenied`)。既存の dynamic/no-file/plugin 判定(ResolveArmaModels 内)と
   合流させ、判定箇所を 1 箇所にすること(候補ループで hard、確定後に id deny、で可)。
3. `WriteCarrierManifest()` の**独自 ARMO→ARMA resolver を削除**し、注入側と同じ
   safe resolver を使う。推奨 seam: 「content id + sex → NIF path(admitted のみ)」を
   返す関数を SkinRebind に公開し、manifest はそれだけを呼ぶ
   (`ResolveAdmittedModelPath(id, sex)` 的な最小公開で良い。R3 の
   `AdmittedModels` struct 全面導入は任意 — 不変条件は R3 §3 P1-1「必須修正」の
   6 箇条で、型名ではない)。
4. `AdmittedContents()` の判定を「base-form のみ」から「**base + 選択 ARMA が
   resolve 可能**」へ強化するか、または派生読者を「registry に実在する(=登録を
   通過した)content」基準に切り替える。どちらでも「base admission と model
   admission の判定不一致」(R3 の反例 A/B)が消えること。

### 1-B. quarantine のトランザクション化(R3 P1-2)

1. **box ability cache の失効**: policy 変更の再評価では、全 box について
   `RebuildBoxAbility(token)` 相当(= player から旧 SpellItem を remove →
   cache erase)を行ってから `ApplyBoxAbilities()`。
   ⚠ `ClearBoxSpellCache()` 単独は不可 — それはロード経路専用
   (セーブ再ロードが旧 spell を殺す前提)。ミッドセッションは
   **remove-then-erase**(src/BoxStore.cpp `RebuildBoxAbility` :2973-2983 の既存
   パターン)を全 box に適用すること。
2. **順序の是正**: mutator 経路を
   `PublishPolicy → settings JSON のみ書く → 再評価(detach/re-register/派生再構築)
   → manifest 生成(admitted 状態から)` に。
   実装案: `WriteJson()` に manifest 生成を抑止する形(引数 or 分割関数)を作る。
   ⚠ **他の全 WriteJson 呼び出し面の挙動は不変であること**(捕獲/削除フローは
   WriteJson→manifest→auto-sync に依存)。既定値で従来挙動、mutator だけが
   settings-only を使う形にする。

### 1-C. P3-1

`git diff --check` の EOF 空行(MARA_GUARD_ADVERSARIAL_REREVIEW.md:494)は
本ハンドオフのコミットで**修正済み**。以後 `git diff --check <base>..HEAD` を
クリーンに保つこと(レビュー md の本文内容は原文保存 — hard-break 空白は対象外)。

## 2. 壊してはならないプロジェクト不変条件(コンテキスト供与)

1. **M2 セマンティクス**: カタログ外の persist active は**正当な状態**。
   `ReloadSettingsFromDisk` が snapshot した active を catalog でフィルタせず
   再登録するのは意図(review 2026-07-07 P1-a)。quarantine 化してもこの性質を
   維持(admission でのみ絞る。catalog 有無で絞らない)。
2. **設定と co-save は削らない**: blocked は「登録しない+理由ログ」のみ。
   co-save 側の未解決保全(ROOT H)への合流を壊さない。
3. **head rebuild の合流**: `ReloadSettingsFromDisk` の carrier パスは
   「空 active → 再登録」の 2 要求が**1 回の debounced head rebuild に合流**する
   設計(コメント参照)。再評価トランザクションでもこの合流を維持
   (persist head の多重 rebuild は過去にメモリ膨張事故の温床)。
4. **manifest の等値 short-circuit**(内容不変なら書かない)と
   `ScheduleAutoSync` の 2s debounce を保つ — manifest を 1 トランザクションで
   1 回だけ生成する形にすれば自然に満たされる。
5. **スレッド規約**: 変更系は main thread(AddTask 済みの文脈で呼ばれる)。
   join/blocking dialog 禁止。policy は immutable snapshot(atomic shared_ptr)
   のまま — 「1 操作 = 1 世代」を resolver 側にも貫くこと(列挙/再評価の途中で
   snapshot を取り直さない)。
6. **正当コンテンツの race マッチング挙動を変えない**: PickAddonForPlayer の
   候補優先順位・フォールバック(該当 sex モデル無し→他 sex)は現状維持。
   admitted 候補内での選択結果が従来と一致すること。
7. **パッケージ不変**: バージョンは 1.3.2 のまま。アーカイブへの新規ファイル追加
   なし(esp/pex/SEQ/meshes はバイト同一を維持)。`tools/package.ps1` の staging
   リストを変えるファイル追加をしない。
8. **テストの純度**: `tests/policy_tests.cpp` は RE 非依存を維持。本ラウンドで
   追加するテストも「純粋層で表現できる範囲+ビルド/ctest 緑」まで
   (RE 依存 fake form graph は本ラウンド対象外 — §0)。
9. **レビュー文書は編集しない**(EOF 空行修正済みの分を除く)。対応記録は
   MARA_GUARD_IMPL.md に **§R3** として追記し、実装・テスト・文書主張を
   同一コミットで更新(R3 §6-11)。
10. **ビルド/検証手順**: `build.cmd release`(SKYRIM_MODS_FOLDER を scratch に
    向けて良い)+ `ctest --test-dir build/release --output-on-failure`。
    /W4 新規警告ゼロ。コミットは `-F <file>` 方式(PowerShell 5.1 は -m 内の
    二重引用符を壊す)。メッセージ末尾に
    `Co-Authored-By: <実装者名>` を 1 行。

## 3. 完了条件(このラウンドの green)

1. R3 反例 A(許可 ARMO→deny plugin ARMA)と B(許可 ARMO→runtime/no-file ARMA)が
   **resolver・登録・manifest・派生処理の全てで**拒否される(deep read なし)。
2. 選択 ARMA の colon-id deny が効く。
3. blacklist 追加→既存 active の detach+stats/keywords/**ability**/manifest から
   即時消滅、解除→自動復元、の往復がコード上一貫(手動確認手順を §R3 に記載)。
4. mutator 経路で manifest 生成が quarantine 完了後に 1 回だけ走る。
5. `/W4` ビルド緑・ctest 緑(既存 43 checks + 追加分)。
6. MARA_GUARD_IMPL.md §R3 に「何を・どこに・なぜ」+ 残余(P2-1/P2-2 の deferral と
   その理由 = 基準A/B の未決)を明記。過大主張の禁止:
   「静的には閉じた」等の表現は**基準A の範囲に限定**して書くこと。

## 4. リリース基準の未決事項(実装者は決めないこと)

基準A で出荷するか基準B まで引き上げるかは**プロジェクトオーナーの判断**。
本ラウンドは P1 修正までを行い、判定文書(§R3)では両基準での現在地を
それぞれ 1 行で述べるに留める。
