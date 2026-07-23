# v1.3.2 実装記録 — MARA 捕獲ブラックリスト(敵対的レビュー用)

> ステータス: **r3 = 再レビュー全指摘対応済み・再パッケージ済み(2026-07-23)**。
> ブランチ: `mara-guard-v1.3.2`(base = `main` @ 6877530 = v1.3.1)。
> コミット列: `bf8883b` docs → `f09bf40` L1 → `fae79b2` L2 → `0e00f85` L3 →
> `a8ca5c5` release cut → `d1bf735` レビュー r2 修正(§R)→ `49f91a4` r2 docs →
> **`cac79ca` 再レビュー r3 修正**(§R2)。
> 敵対的レビュー = [MARA_GUARD_ADVERSARIAL_REVIEW.md](MARA_GUARD_ADVERSARIAL_REVIEW.md)
> (P1×5+P2×1 → r2 で全件修正)/ 再レビュー =
> [MARA_GUARD_ADVERSARIAL_REREVIEW.md](MARA_GUARD_ADVERSARIAL_REREVIEW.md)
> (初回修正は全て確認済み・残 P1×2+P2×2+P3×1 → **r3 で全件修正**)。
> §1〜§4 は r1 時点の記録として保存し、以後の修正で無効になった記述には
> (r2 修正)/(r3 修正)を付す。
> 設計根拠: [MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md) §3 / 検証根拠:
> [MARA_CRASH_CLASS_AUDIT.md](MARA_CRASH_CLASS_AUDIT.md)。

---

## §R2. 敵対的再レビュー対応(r3・コミット cac79ca)

再レビューの判定「初回 8 findings は修正確認・残 5 件」を受理し、**残 5 件を全て修正**。

| Finding | 対応(cac79ca) |
|---|---|
| **P1-1 最終選択 ARMA 未検査** | `ResolveArmaModels` が **ARMA 確定直後・bipedModels 読取り前**に hard+plugin 層(`IsDynamicForm`/defining-file/`PluginDenied`)を選択 ARMA 自体へ適用。**判定と使用が同一関数・同一ポインタ**なので check/use ギャップ無し(レビュー提案の AdmissionResult 持ち回りより強い一点化)。ケース A(許可 ARMO→deny plugin の ARMA)/ケース B(runtime ARMA 差し替え)とも遮断。resolver の全呼び出し元(picker gate・登録境界・注入・shape 列挙)に自動継承。carrier manifest 側は独自 resolve だが AdmittedContents フィルタ(下記)が同じ結論を先に適用する |
| **P1-2 quarantine が登録抑止どまり** | ① `IsContentAdmissible` に quiet モード(`a_log=false`)を追加し、`AdmittedContents()` スナップショットフィルタを**全派生読者**へ: `SetTokenStats`・`ApplyKeywordsToToken`・`BuildEnchantSpell`(box/persist アビリティの単一チョーク)・`WriteCarrierManifest`(boxes+persist actives)・`BoxStatsSummary`(SMF 表示)。② policy 変更(entry 追加/削除・スイッチ)後に `ReevaluateContentAdmissions()` = `ReloadSettingsFromDisk`(実戦済みプリミティブ: 全 active detach → gated 再登録 → persist active 復元 → Reconcile → アビリティ/manifest 再構築)。**blocked-active は即 detach、解除時は自動再 admit、設定と co-save は常に保持**。ログは gate のみ loud・派生フィルタは無音(per-frame スパム無し) |
| **P2-1 公開 `InjectArma` 迂回** | 匿名 ns の `InjectArmaUnchecked` プリミティブ + gated 公開ラッパへ分離。`cef inject`/self-test は **実際に注入する local/plugin から合成した colon-id** で admission(bare label "test" では素通りしない)。`InjectArmaById` は一度 admit して直接プリミティブ呼び(二重ログ回避) |
| **P2-2 公開 `CaptureEnchant` native 未ゲート** | 関数境界に `IsContentAdmissible`(上位の pre-gate は UX、関数自身が強制 — 二重化) |
| **P3-1 CTest 未登録** | `include(CTest)` + `add_test(capture_policy)`。検証: `ctest --test-dir build/release` → **1/1 Passed(43 checks)** |

**挙動ノート(r3)**:
- blocked-**ARMA** 経由の content は picker/gate では「mesh could not be resolved」側の
  文言で拒否される(ResolveArmaModels が false を返すため)。ブロック理由そのものは
  `ResolveArma: ... refused` の warn ログに出る — UI 文言の層別化は次版課題。
- `ReevaluateContentAdmissions` = フル reload なので、blacklist 編集時に表示中衣装が
  一瞬 re-attach される(既存の「Reload settings from disk」と同一挙動・ユーザー操作
  時のみ)。
- 再レビュー §4 の `git diff --check` 警告(初回レビュー md の hard-break 末尾空白)は
  **レビュー文書を原文保存する方針**のため未修正(実装コードに trailing whitespace は無い)。

再検証(r3): `/W4` ビルド緑・`ctest` 1/1(43 checks)・再パッケージ差分 =
1.3.1 + `CostumeFW_NoCapture_KID.ini` のみ(552→553 files)。
**未了のまま残るのは実機系のみ**(再レビュー §5 の MARA 実機 5 手順・runtime fixture・
VR smoke — 静的には閉じたが、リリース最終判定はこの実機確認後)。

---

## §R. 敵対的レビュー対応(r2・コミット d1bf735)

**全 6 findings を実ソース照合の上 CONFIRMED と判定**(反証できた指摘はゼロ)。
CommonLibSSE-NG 裏取りの決定打: `TESForm.h:292-300` — `GetLocalFormID()` は
`GetFile(0)` の戻り値を**無条件デリファレンス**する。つまり **v1.3.1 の
「+ Add worn item 押下で即 CTD」の機械的正体 = 旧 `WornArmors()` が全外部装備に
`MakeColonId` → 動的フォームで null deref**(レビュー P1-4 の指摘 5 が root cause を
言い当てていた。クラッシュログとの突合は §5 チェックリストに残る)。

| Finding | 判定 | 対応(d1bf735) |
|---|---|---|
| P1-1 判定前の entry コピー | CONFIRMED(`GetInventory` は filter 通過 entry の `InventoryEntryData`/extraLists をコピーしてから返す) | 構造/deny 判定を **`GetInventory` の filter 内**へ移動(`WornArmors`/`InventoryArmors`)。blocked フォームは **entry がコピーすらされない**。filter が安全境界であることをコメントで明文化 |
| P1-2 CaptureEnchant の全 Armor コピー | CONFIRMED(安全なアイテム捕獲でも敵性 entry をコピー) | filter を **対象 FormID 一致のみ**に変更 — 捕獲対象以外の entry には一切触れない |
| P1-3 最深部ゲート不在 / hand-JSON 未保護 | CONFIRMED(**r1 の「hand-JSON も store 前線で保護」は誤り** — LoadJson は AddBox を経由しない) | `RegisterBoxById` / `RegisterArmaById` / `InjectArmaById` 先頭に **hard admission**(`IsContentAdmissible`)。settings 読込・co-save 復元・ReapplyBoxes・persist 再有効化・`RegisterPersist`/`DefineBox` native・console `cef box` を全カバー。**拒否時も設定は保持**(quarantine-lite: 登録だけしない+理由ログ。co-save 側は既存 ROOT-H の未解決保全に自然合流) |
| P1-4 動的判定の初手が不適切 / allowDynamic | CONFIRMED(上記 root cause) | `IsDynamicForm()`(formID 読みのみ)を**初手のハード判定**に、no-defining-file を第 2 ハード判定(`kNoDefiningFile` 新設)に。`MakeColonId` を no-file 安全化(file 無しは生 FormID 8 桁+空プラグインを**デリファレンスなしで**整形)。**`allowDynamic` は UI/json/policy から完全撤去** — ハード不変条件に通常経路の解除口を残さない |
| P1-5 policy のデータ競合(UB) | CONFIRMED(「g_boxes と同類の許容」は安全性の証明にならない — 撤回) | `std::atomic<std::shared_ptr<const policy::CapturePolicy>>` の **immutable snapshot 方式**。読者は操作単位で 1 世代を取得(列挙は filter〜loop まで同一 snapshot)、変更は copy-and-publish。公開済み vector の in-place 変更は消滅 |
| P2-1 直接 ARMA id の迂回 | CONFIRMED | `IsContentAdmissible` を層構造化: ①canonical id の**文字列 deny**(解決不能でも効く)→ ②**汎用 TESForm** 判定(dynamic/no-file/plugin deny — ARMA にも効く)→ ③ARMO 固有判定。`CanCaptureContent` = admission + resolvability |
| 検証不足: unit test 不在 | CONFIRMED | 純粋層を `src/CapturePolicy.{h,cpp}`(std-only・colon-id parse/format の単一実装に統合、SkinRebind は委譲)へ分離し、**`tests/policy_tests.cpp`(host 実行・43 checks 緑)** を追加(`policy_tests.exe`・dev 専用ターゲット)。form-level 層(IsDynamicForm/flags/keyword)は RE 依存のため実機チェックリスト側(監査 §5) |
| 成果物件数の不一致 | CONFIRMED(表記揺れ) | 正: **552→553 files + 9 folders**(7-Zip)。r1 の「562→563 entries」は files+folders+アーカイブ名行を含む Path 行数 — 以後 files/folders 表記に統一 |

**r2 で維持した設計**(レビュー §2 の「良かった点」+ §5 提言との整合):
hard/soft 分離(hard = dynamic/no-file、soft = non-playable/deny-list/keyword)、
quarantine-lite(拒否は登録抑止のみ・設定とco-saveを破壊しない)、
`AdmissionPurpose` の完全実装は見送り(現状は capture / registration の 2 面 —
理由コード付きログで代替。フル版は beta 線での拡張候補)。

**C1(不正 NIF)の主張の狭め**(レビュー §7): v1.3.2 の主張は
「**MARA 型ランタイムフォーム(および deny 対象)を CEF の列挙・捕獲・登録経路から遮断する**」
まで。正規 plugin レコードが参照する破損 NIF(legacy `NiTriShape`/0 頂点)の安全性は
**保証しない**(監査 §4.1 の残余のまま。VFS 経由 bytes+隔離 validator 案はレビュー §4 を
そのまま次版検討課題として引き継ぐ)。

再検証(r2): `/W4` ビルド緑・`policy_tests` 43/43・再パッケージ後の 7z 差分 =
1.3.1 + `CostumeFW_NoCapture_KID.ini` のみ(552→553 files)・DLL のみ更新。

---

---

## 0. 何を解決するか(1 段落)

MARA(Nexus 173949・ESP を持たない DLL 単体 mod)は実行時生成(0xFF)の不可視装備
「CORE Carrier」をインベントリに常駐させる。CEF の捕獲ピッカーは生インベントリを
無フィルタで列挙し、**フォーム検査より先に `InventoryEntryData`(IsWorn/xList)へ触れて
いた**ため、これを選択(または列挙)した瞬間の即 CTD が報告された(CEF Nexus posts
2026-07-22・安定版 v1.3.1)。同型事故は MARA バグ #1059563(別のインベントリ UI での
hover-CTD)で独立に再現している。v1.3.2 は捕獲面(ピッカー2種 × box/persist × MCM/SMF/
preset/native)に 3 層のブラックリストを敷き、**ブロック判定をフォームレベル読みだけで
完結させ、判定前にエントリデータへ一切触れない**よう列挙順序を組み替えた。

## 1. 変更一覧(file:line)

### L1 構造ガード + ゲート(f09bf40)

| 箇所 | 内容 |
|---|---|
| src/BoxStore.cpp:2012-2063 | `CaptureBlockReason` — **form-level 読みのみ**の契約。順序: null → L1a 動的フォーム(`GetFile(0)==nullptr`、:2025-2028)→ L2a プラグイン(:2030)→ L1b non-playable(`formFlags & kNonPlayable`、:2036)→ L3 キーワード(:2041)→ L2b 名前(:2049)→ L2c colon-id(:2054)。**動的フォームは名前/キーワード読みに到達しない** |
| src/BoxStore.cpp:2228 (`WornArmors`) / :2267 (`InventoryArmors`) | ループ順序組替え: `As<ARMO>` → `IsCaptureBlocked` → `IsTokenPluginFile` → **その後で初めて** count/`entry->IsWorn()`/名前読み。旧順序(v1.3.1)は IsWorn が先(v1.3.1 の :1872)。**(r2 修正)** この loop 側 skip は `GetInventory` の entry コピー後に走るため不十分(レビュー P1-1)→ 判定は **filter 内**へ移動(§R) |
| src/BoxStore.cpp:2086 | `CanCaptureContent(id, why*)` = blacklist → 既存 `CanResolveContent`。未解決 id は blacklist 層をスキップし従来どおり resolve 拒否(挙動互換) |
| ゲート敷設(全入口) | SMF: SmfUI.cpp:114 / :150。MCM: Papyrus.cpp の `CanResolveContentNative`(native 名は**据え置き** → .psc 再コンパイル不要。呼び元 .psc:1229/:1286 検証済み)。preset: Preset.cpp:235。store 直行系(native/hand-JSON): `AddBox` BoxStore.cpp:3284 周辺・`AddPersistContent` :1573 周辺 |
| src/BoxStore.cpp:324 / src/SkinRebind.cpp:1518 | `MakeColonId`/`CanonicalizeColonId` の `char[8]` → `char[16]`。`%06X` は**最小幅**なので 0xFF ローカル id(8桁)が旧バッファで切り詰められ破損 id を生んでいた(実害経路は L1 で閉鎖、id 生成自体も修正)。正規 6 桁 id はバイト同一 |
| plugin.cpp:22, :139-147 | kDataLoaded で `GetModuleHandleW(L"MARA.dll")` 1 回 → info ログ 1 行。**挙動分岐なし**(triage 用) |
| CEF_settings.json | `captureBlacklist.allowNonPlayable / allowDynamic`(既定 false = skip 有効)。WriteJson :274-284 / LoadJson :1240-1269 / 初期化リセット :1054 / catch リセット :1310。**(r2 修正)** `allowDynamic` は撤去(ハード不変条件・§R P1-4。旧 json のキーは黙って無視) |

### L2 名指しリスト + SMF Blocked ページ(fae79b2)

| 箇所 | 内容 |
|---|---|
| src/BoxStore.cpp:1936-2010 | 出荷既定 `kDefaultBlockNames = {"CORE Carrier"}` / `kDefaultBlockPlugins = {"MARA"}`(:1944-1945)+ CI 比較ヘルパ(`EqualsCI`/`PrefixCI`/`NameMatches`(末尾 `*` = 前方一致)/`PluginDenied`/`NameDenied`) |
| src/BoxStore.cpp:2112(flags)/:2172(view)/:2190(add)/:2211(remove) | `disableDefaults` フラグ + `GetCaptureBlacklist`(UI 用スナップショット copy)+ `Add/RemoveCaptureBlacklistEntry`(kind = name/plugin/id、trim・CI dedup・id は `CanonicalizeColonId`) |
| json | `captureBlacklist.names/plugins/ids/disableDefaults`(ユーザー拡張のみ永続化。既定はコード内) |
| src/SmfUI.cpp:785-879, :885 | SMF「Blocked」ページ(6 ページ目): スイッチ 3 種(allowDynamic ON 時は再現専用の警告文)、出荷既定の表示("(off)" 表示対応)、ユーザーエントリの一覧+Remove、kind コンボ+入力+Add。読みは毎フレームスナップショット、変更は AddTask 経由(他ミューテータと同規約) |

### L3 CEF_NoCapture キーワード(0e00f85)

| 箇所 | 内容 |
|---|---|
| src/BoxStore.cpp:1946-1951, :2042-2044 | `HasKeywordString("CEF_NoCapture")`(静的キーワード配列読み)で拒否 |
| CostumeFW_NoCapture_KID.ini(新規) | KID 雛形(コメントのみ・formid/editorid/名前ワイルドカード/プラグイン全体の 4 例)。**静的フォーム限定**(動的フォームに KID は載らない — その класс は L1 が塞ぐ)を明記 |
| tools/package.ps1:24-27 | 雛形をリポジトリから staging(実行時に書き換わらないファイルのため) |

### リリース構成(release cut)

- CMakeLists.txt:4 `VERSION 1.3.2`。CHANGELOG v1.3.2 節 + 過去節の修復(v1.3.1 に
  VR ブロックを帰属・watchdog 1 行・v1.3.0 スタブ追記 — Unreleased 常置の解消)。
- NEXUS_CHANGELOG_v1.3.2.txt(EN)・README「Mod compatibility」節(新規)。
- tools/package.ps1 / package_vr_patch.ps1 に `-ModFolder` パラメータ(既定 = 従来のライブ
  MO2 パス)。**v1.3.2 は「1.3.1 アーカイブ展開 + 新ビルド DLL」のクリーン staging から梱包**
  (ライブフォルダは v1.4.0-beta.1 配備中のため、beta 資材の混入を構造的に排除)。
- 成果物: `dist/CostumeExpansionFW-1.3.2.7z`(**1.3.1 との差分 = DLL 更新 +
  CostumeFW_NoCapture_KID.ini 追加のみ** — 7z マニフェスト比較で機械検証済み)/
  `dist/CostumeExpansionFW-VR-Patch-1.3.2.7z`(DLL + README_VR.txt の 2 ファイル)。
  esp/pex/SEQ/meshes は 1.3.1 とバイト同一。

## 2. 設計判断とその理由(攻撃想定 Q&A)

1. **なぜ列挙フィルタとゲートの二重化?** ピッカー非表示(UX)と、ピッカーを経ない入口
   (Papyrus native 直呼び・preset・手編集 JSON)の防御は別問題。ゲートは
   `AddBox`/`AddPersistContent` という**最深部の store 前線**にも敷いた(v1.3.1 まで
   native 経路はモデル解決以外ノーチェック — 監査 C7)。
2. **なぜ `CanResolveContent` native の名前を変えない?** 意味論変更(gate 化)を
   .psc/.pex 再出荷なしで MCM に波及させるため。Papyrus API としては「捕獲可否」を
   返す関数に**強化**であり、既存呼び元の期待(false = 捕獲しない)と互換。
3. **なぜ動的フォームを一律拒否できる?** CEF の永続 id は `local:plugin` で、
   plugin を持たない動的フォームは**そもそもロード後に復元不能**。防御と意味論が一致
   する(監査 C7)。~~逃げ道 `allowDynamic` は再現実験専用として残し、UI に警告を付けた。~~
   **(r2 修正)** `allowDynamic` は撤去 — ON にすると `MakeColonId`→`GetLocalFormID` の
   null deref で**リスト構築時に即死**するため「再現専用」としてすら機能しない
   footgun だった(§R P1-4)。再現手段は v1.3.1 導入で代替(監査 §5)。
4. **なぜ名前既定が「CORE Carrier」だけ?** MARA のリネーム複製("Silver Ring (Left)
   Misc" 等)も全て動的フォーム = L1 で消えるため、L2 名前は「静的化された将来版への
   保険 + ユーザー速報対応」の層。過剰な既定名は誤爆リスク(実在装備名との衝突)を増やす。
5. **non-playable 既定 skip の誤爆リスクは?** バニラ UI が隠すものをピッカーも隠す、
   という対称性が原則。正当な non-playable 衣装(稀)には `allowNonPlayable` を用意。
6. **スレッド安全性は?** ~~既存規約に相乗り・新しい競合クラスは導入していない。~~
   **(r2 修正)** レビュー P1-5 のとおり vector の並行 read/mutate は UB であり
   「既存と同類」は安全性の証明にならない — 撤回。r2 で policy は
   `std::atomic<std::shared_ptr<const CapturePolicy>>` の immutable snapshot に置換
   (§R)。既存 `g_boxes` 等の同類競合は本リリースのスコープ外課題として残る(§5)。
7. **`GetInventory` 自体が踏む面は?** ~~CEF 固有の追加接触だけを判定の後ろへ移した。~~
   **(r2 修正)** それでは entry コピー(filter 通過分)が判定より先に走る(P1-1)。
   r2 で判定を filter 内へ移し、**blocked フォームは entry コピー自体が発生しない**。
   残余は「filter を呼ぶために `entry->object` を読む」engine 共有面のみ
   (これはバニラ/全 mod と同一水準で、これ以上は GetInventory 再実装になる)。

## 3. 既知の限界(レビューで再確認してほしい点)

- **C1 の legacy-NIF 面は未対処のまま**(意図的スコープ外・レビュー §7 の狭め主張を採用):
  正規 ESP アイテムが legacy `NiTriShape`/0 頂点スキンの NIF を持つ場合、捕獲後の
  注入ロードでエンジン側 divide-by-zero の余地が残る(直接注入経路に
  `ValidateNifSkinnable` 相当は無い)。事前検証は BSA 不可視(std::filesystem)のため
  偽陰性を作る — 監査 §4.1 の判断。恒久策候補 = レビュー §4(VFS bytes+隔離 validator)。
- ~~`GetInventory` のマップ構築はブロック判定の外。~~ **(r2 修正)** filter 境界化で
  blocked フォームの entry コピーは消滅(§R P1-1)。残余は filter 呼出しのための
  `entry->object` 読みのみ(engine 共有面)。
- **SMF Blocked ページの Add は成功/失敗を同期表示しない**(AddTask 後の実結果はログ。
  s_status は「queued」の楽観表示 — 既存 s_pendingWear と同じ思想)。
- **`help "CORE Carrier"` 型の実機確認は未実施**(MARA 非導入環境)。再現・確認手順は
  監査 §5(テスター向け)。**(r2 修正)** allowDynamic 撤去に伴い、旧挙動の再現は
  **v1.3.1 を一時導入**して行う(その方が「修正前バイナリでの再現」として証跡も正しい)。
- **既存ストア状態(`g_boxes`/`g_persist` 等)の read/mutate 競合クラスは残存** —
  レビュー P1-5 は blacklist を snapshot 化したが、既存状態の同型競合は v1.3.2 の
  スコープ外(beta 線での一般化候補。SmfUI.cpp:18-25 の従来規約のまま)。
- CHANGELOG の過去節修復(v1.3.0/v1.3.1)は**遡及記載** — 出荷済み Nexus 文面
  (NEXUS_CHANGELOG_*.txt)を正とし、要約のみ。

## 4. 検証済み事項(証跡)

- ビルド: `build.cmd release` 3 回(Phase 毎)+ release cut ビルド、全て警告レベル
  /W4 で新規警告なし・リンク成功(DLL 2,589,696 bytes・build stamp 2026-07-23 19:45)。
- パッケージ差分検証: 7z マニフェスト比較 — 1.3.2 = 1.3.1 + `CostumeFW_NoCapture_KID.ini`
  のみ追加(562 → 563 エントリ)・esp/pex/meshes バイト同一・DLL のみ更新。
- MCM ゲート経路: .psc:1229/:1286 が `CFW_Native.CanResolveContent` を捕獲前に呼ぶこと
  を release ブランチの実ファイルで確認(.psc は main と beta で差分があるが、当該
  ガード行は main 版に存在)。
- KID 雛形の文法: kid-authoring リファレンス(v3.5.0 準拠)で構文確認(型文字列
  `Armor`・`_KID` ファイル名・formid `0x800~Mod.esp`・`*substring` ワイルドカード)。
- 動的フォーム refuse 経路の互換: id が解決しない場合は従来文言のまま refuse
  (CanCaptureContent :2090-2093 のフォールスルー設計)。

## 5. レビュー後の残タスク(本リリースに含めない)

- beta 線(nifcarrier-inproc)への本ブランチ merge + NPC 側 `IsDead` ゲート
  (PublishStore.cpp:839 相当 — 監査 §4.2)。
- VR smoke(vfunc 0x6A)チェックリスト 1 行(監査 §5.1)・タグ付け/GitHub リリース/
  Nexus 掲載(ユーザー実施)。
- 報告者への返信 + MARA 作者への連絡(下書き: MARA_COMPAT_PLAN.md §7 — **未送付**)。
