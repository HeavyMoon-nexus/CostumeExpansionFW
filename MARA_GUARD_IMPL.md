# v1.3.2 実装記録 — MARA 捕獲ブラックリスト(敵対的レビュー用)

> ステータス: **実装完了・パッケージ済み(2026-07-23)**。本書は「何を・どこに・なぜ」を
> レビュー(Codex 敵対的レビュー想定)が攻撃できる粒度で記録する。
> ブランチ: `mara-guard-v1.3.2`(base = `main` @ 6877530 = v1.3.1)。
> コミット列: `bf8883b` docs(計画+監査) → `f09bf40` Phase 1 (L1) → `fae79b2` Phase 2 (L2)
> → `0e00f85` Phase 3 (L3) → release cut(本書を含む)。
> 設計根拠: [MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md) §3 / 検証根拠:
> [MARA_CRASH_CLASS_AUDIT.md](MARA_CRASH_CLASS_AUDIT.md)(C1/C7 が開いていた面)。
> 本文の file:line は release cut 時点の実ファイル。

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
| src/BoxStore.cpp:2228 (`WornArmors`) / :2267 (`InventoryArmors`) | ループ順序組替え: `As<ARMO>` → `IsCaptureBlocked` → `IsTokenPluginFile` → **その後で初めて** count/`entry->IsWorn()`/名前読み。旧順序(v1.3.1)は IsWorn が先(v1.3.1 の :1872) |
| src/BoxStore.cpp:2086 | `CanCaptureContent(id, why*)` = blacklist → 既存 `CanResolveContent`。未解決 id は blacklist 層をスキップし従来どおり resolve 拒否(挙動互換) |
| ゲート敷設(全入口) | SMF: SmfUI.cpp:114 / :150。MCM: Papyrus.cpp の `CanResolveContentNative`(native 名は**据え置き** → .psc 再コンパイル不要。呼び元 .psc:1229/:1286 検証済み)。preset: Preset.cpp:235。store 直行系(native/hand-JSON): `AddBox` BoxStore.cpp:3284 周辺・`AddPersistContent` :1573 周辺 |
| src/BoxStore.cpp:324 / src/SkinRebind.cpp:1518 | `MakeColonId`/`CanonicalizeColonId` の `char[8]` → `char[16]`。`%06X` は**最小幅**なので 0xFF ローカル id(8桁)が旧バッファで切り詰められ破損 id を生んでいた(実害経路は L1 で閉鎖、id 生成自体も修正)。正規 6 桁 id はバイト同一 |
| plugin.cpp:22, :139-147 | kDataLoaded で `GetModuleHandleW(L"MARA.dll")` 1 回 → info ログ 1 行。**挙動分岐なし**(triage 用) |
| CEF_settings.json | `captureBlacklist.allowNonPlayable / allowDynamic`(既定 false = skip 有効)。WriteJson :274-284 / LoadJson :1240-1269 / 初期化リセット :1054 / catch リセット :1310 |

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
   する(監査 C7)。逃げ道 `allowDynamic` は再現実験専用として残し、UI に警告を付けた。
4. **なぜ名前既定が「CORE Carrier」だけ?** MARA のリネーム複製("Silver Ring (Left)
   Misc" 等)も全て動的フォーム = L1 で消えるため、L2 名前は「静的化された将来版への
   保険 + ユーザー速報対応」の層。過剰な既定名は誤爆リスク(実在装備名との衝突)を増やす。
5. **non-playable 既定 skip の誤爆リスクは?** バニラ UI が隠すものをピッカーも隠す、
   という対称性が原則。正当な non-playable 衣装(稀)には `allowNonPlayable` を用意。
6. **スレッド安全性は?** 読み(列挙・判定・UI スナップショット)はレンダ/VM スレッド、
   変更はメインスレッド(AddTask)— SmfUI.cpp:18-25 に既存明文化された
   「read-only snapshot tolerated + 変更は AddTask」規約に**完全に相乗り**。
   `g_captureBlacklist` の vector 読み書き競合は既存 `g_boxes` 等と同じ許容クラス
   (変更頻度 = ユーザー操作時のみ)。**新しい競合クラスは導入していない**。
7. **`GetInventory` 自体が踏む面は?** マップ構築時のエントリ走査はエンジン共有面
   (バニラ UI・全 mod が通る)で、CEF 固有の追加接触(IsWorn/名前/colon-id)だけを
   ブロック判定の後ろへ移した。ここは**残余**として明記(§3)。

## 3. 既知の限界(レビューで再確認してほしい点)

- **C1 の legacy-NIF 面は未対処のまま**(意図的スコープ外): 正規 ESP アイテムが
  legacy `NiTriShape`/0 頂点スキンの NIF を持つ場合、捕獲後の注入ロードで
  エンジン側 divide-by-zero の余地が残る(直接注入経路に `ValidateNifSkinnable` 相当は
  無い)。事前検証は BSA 不可視(std::filesystem)のため偽陰性を作る — 監査 §4.1 の判断。
- **`GetInventory` のマップ構築**はブロック判定の外(上記 Q7)。
- **SMF Blocked ページの Add は成功/失敗を同期表示しない**(AddTask 後の実結果はログ。
  s_status は「queued」の楽観表示 — 既存 s_pendingWear と同じ思想)。
- **`help "CORE Carrier"` 型の実機確認は未実施**(MARA 非導入環境)。再現・確認手順は
  監査 §5(テスター向け)に依存。allowDynamic+disableDefaults で意図的に v1.3.1 相当へ
  戻せる設計にしてある。
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
