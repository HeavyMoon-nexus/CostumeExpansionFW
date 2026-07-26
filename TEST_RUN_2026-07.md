# 一括実機テストラン(2026-07) — v1.3.2 / NPC beta / VR の未実行分を全消化

> 目的: 現時点で残っている実機テストを 1 スイープで消化する。green の範囲が
> そのまま「報告者返信の投稿可否(§A/§M)」「基準 A リリースゲート(§M/§F)」
> 「beta.2 修正リスト確定(§B)」になる。
> 出典: [MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md) §7.1-r5 /
> [MARA_GUARD_IMPL.md](MARA_GUARD_IMPL.md) §R3 実機手順 /
> [MARA_CRASH_CLASS_AUDIT.md](MARA_CRASH_CLASS_AUDIT.md) §5 /
> NPC_SUPPORT_IMPL.md §11-§12(beta ブランチ)/ 記憶(stat-passthrough 2026-07-21)。
> 記録: 各項目のチェックボックスに ✓ + 気付きは行末へ追記。NG は ID ごと切り出して
> issue 化。

---

## §0 プロファイル構成と共通注意

| プロファイル | 内容 | 用途 |
|---|---|---|
| **P-A** MARA 検証(新規作成) | CEF core **1.3.2**(dist 7z)+ 依存(SKSE/AddressLib/RaceMenu/SkyUI/SMF/FSMP 3.5.0)+ **MARA** + **Crash Logger** + テスト fixture ESP 2 種(§F 準備)。**新規ゲーム** | §A(1.3.1 に一時差替)・§M・§F・§R |
| **P-B** NPC beta(既存ライブ) | v1.4.0-beta.1 + NPC addon(現状のまま) | §B |
| **P-C** VR | VR runtime + core 1.3.2 + VR-Patch-1.3.2 | §V(VR 環境がある場合。無ければコミュニティ依頼) |

⚠ **共通の罠**:
1. **betaセーブ保護**: 1.3.2 DLL は PUBB/NPRS を登録しない。**P-A/P-C で beta セーブを
   絶対にロードしない**(ロード+セーブで NPC publish 状態消失)。P-A は新規ゲーム限定。
2. **stale DLL**: プロファイル切替・mod 版差替のたびに **MO2 を完全再起動**し、CEF ログ
   1 行目の build 刻印(`loaded (file ...)`)が期待版か確認してから測る。
3. P-B に MARA を入れない(ガード未 merge のため旧 CTD が出る)。
4. 開始前に `CEF_settings.json` と P-B のセーブをバックアップ。

---

## §A P-A + **v1.3.1** に一時差替: root cause 実証(任意だが推奨・2 項)

- [x] **A1** ジュエリー数点を装備し MARA に CORE Carrier を湧かせる →
      `help "CORE Carrier" 4` → **FormID が FF 始まり**であることを記録
      (計画書 §8-1 を自前で閉じる)。
      → ✅ 2026-07-25: クラッシュログの RDX/RBX で確定 — **FormID 0xFF001260**、
      Flags **kPlayable**|kInitialized(= L1b non-playable 層では捕まらない個体。
      動的フォームhard層の必然性を裏付け)。§8-1 の playable 問いも同時に回答。
- [x] **A2** `+ Add worn item` を開く → **従来 CTD の再現**。Crash Logger の
      ログを保存し、faulting が CostumeExpansionFW.dll の列挙経路
      (WornArmors/MakeColonId/GetLocalFormID 相当番地)かを後で照合
      (= IMPL §R の機序の実地確定)。
      ※ A2 完了後、CEF を **1.3.2 に戻して** MO2 再起動+刻印確認。
      → ✅ 2026-07-25 再現・**機序 100% 一致**(`crash-2026-07-25-12-17-26.log`):
      faulting = `CostumeExpansionFW.dll+0x8241C` **`movzx r9d, byte ptr
      [rax+0x478]`, RAX=0** → AV read 0x478 = **`TESFile::compileIndex`
      (TESFile.h // 478)を null file から読む = `GetLocalFormID()` の
      unchecked `GetFile(0)` deref そのもの**。RDX/RBX = "CORE Carrier"
      (0xFF001260)、R8 に名前バイト列 "CORE Car"、スタックは Papyrus VM
      (MCM native)→ CEF 列挙 5 フレーム。r2 レビューの静的特定と完全一致。
      → ✅ 追加データ(同日 21:35, `crash-2026-07-25-12-35-35.log`): **beta.1**
      (ガード未 merge)+ MARA でも同一機序・同一被写体で再現(オフセットのみ
      beta バイナリ相当にシフト)。ガード無しビルドは確実に落ちる対照群として記録。
      1.3.2 での §M 実施は M0 の差替実体確認が前提。

## §M P-A(1.3.2): MARA 本丸 — 返信 §7.1-r5 の裏付け

> ⚠ **§M0 を必ず先に**(2026-07-25 の事故記録): 「1.3.2 へ戻した」つもりの 2 回目
> クラッシュ(`crash-2026-07-25-12-35-35.log`)は、実際には **beta.1 が動いていた**
> (本体 mod = beta 配備 2,762,240 bytes・compile Jul 18・Character フック行あり・
> compat 行なし)。**v1.3.2 は MO2 に mod として存在していなかった**。beta.1 は
> ガード未 merge なので同機序で落ちるのが正常(=対照群のデータとして §A に追加)。

- [ ] **M0** 差替の実体確認:
      1. `dist\CostumeExpansionFW-1.3.2.7z` を **新規 MO2 mod**("CostumeExpansionFW
         test 1.3.2")としてインストール。
      2. テスト中は **本体 CostumeExpansionFW(beta 配備)と CostumeFW_NPC を無効化**、
         test 1.3.2 + MARA + Crash Logger を有効化。MO2 完全再起動。
      3. 起動後、**`Documents\my games\Skyrim.INI\SKSE\CostumeExpansionFW.log`**
         (この環境の実出力先。標準の My Games\Skyrim Special Edition ではない)の
         1 行目が **`compile Jul 24 2026`** 系であること + `compat: MARA.dll
         detected` 行があることを確認してから §M1 へ。
         **(M4-J ガード版に更新後)**: test 1.3.2 mod を再梱包版 7z で**入れ直し**、
         刻印 = **file 2026-07-25 22:47 / compile Jul 25 2026** 系、compat 行が
         「... and capturing WORN jewelry is refused ...」の新文言であること。
- [ ] **M1** 起動ログに `compat: MARA.dll detected` 1 行。想定外 warn 無し。
- [ ] **M2** CORE Carrier 装備中: box の `+ Add worn item` / `+ Add from inventory`
      → **クラッシュせず、CORE Carrier がリストに出ない**。
- [ ] **M3** 同を persist 側でも(2 ピッカー)。
- [x] **M4** 同状態で**通常アイテムを 1 点捕獲** → 正常(登録・表示・返却まで)。
      (再レビュー必須の CaptureEnchant/P1-2 面)
      → ✅ 2026-07-25 22:03: DLL 実体 = 真正 1.3.2(M0 クリア・file 2026-07-24
      20:00:50)。1 点目(金のアミュレット 0FC055)捕獲・enchant snapshot・
      in-proc sync 完走まで正常。
      → ⚠ **新規発見 M4-J**: 2 点目に**装着中のエンチャント付きアミュレット**
      (10DF51)を捕獲した直後、**MARA.dll 内部で CTD**
      (`crash-2026-07-25-13-03-03.log`: 全 8 フレーム MARA.dll・std::format
      整形中の壊れた引数・SKSE タスク起点。CEF ログは warn ゼロで完走)。
      = 計画書 §2 旧 H2「装着中の MARA 管理装備を RemoveItem で剥ぐと MARA 側で
      死ぬ」のフィールド実証。**ジュエリー(slot35/36)の worn 捕獲に限定した事象**
      で、ピッカー安全化・非ジュエリー捕獲(M4 本体)は green のまま。
      対応方針は MARA_COMPAT_PLAN §7.3 追記参照(A: 文書化のみ / B: MARA 検出時の
      worn ジュエリー捕獲ガード)。
- [ ] **M4-B** (M4-J ガードの検証・再梱包版で) MARA 稼働中:
      ① **装着中**のアミュレット/指輪を `+ Add worn item` で選択 → **拒否**
      (SMF: "MARA manages worn jewelry - unequip it first, or capture it from
      inventory" / MCM: 汎用文言+ログに refused 行)。クラッシュしないこと。
      ② 同じジュエリーを**外してから**インベントリ捕獲 → 成功。
      ③ 非ジュエリーの worn 捕獲 → 従来どおり成功。
      ④ MARA 無効化 → worn ジュエリー捕獲が従来どおり成功(ガードは MARA 検出時のみ)。
- [ ] **M5** save → load → 表示/捕獲状態維持・二重表示無し。
      ※ M4-J の後始末: クラッシュ前のセーブへ戻ると **json(グローバル)には
      アミュレット 2 件が box 登録済み・アイテム custody は未セーブ**の不整合。
      再開時は box からアミュレット 2 content を削除(store 空なので二重付与なし)
      してから、**非ジュエリーのアイテムで** M5 を続行。
- [ ] **M6** MARA を無効化して回帰 1 周: 捕獲(worn/inv × box/persist)・表示切替・
      preset 取込・`cef list` — 従来どおり(= 返信の "with and without")。
- [ ] **M7** (S2 観察・任意) ジュエリー入り slot-35/36 box を装備 → MARA が
      不可視トークンに干渉するか(リネーム/複製/unequip)を記録 → README 注意文の
      文言検証。
- [x] **M8 green(証跡取得済み 2026-07-26 11:05-11:07)** `disableDefaults` を
      **4 往復**トグル(11:05:51/11:06:13/11:06:15/11:06:18/11:06:20/11:06:23/
      11:07:05/11:07:21、最終 OFF に復帰)。ON 中もピッカーに CORE Carrier は
      出ないことをユーザー確認 = **動的フォームのハード層のみで遮断できている**
      (名前既定リストに依存しない)。同ログに `[error]` ゼロ。
      **副産物(ストレス試験)**: 90 秒で 8 回のポリシー変更 = `DoReset3D` ちょうど
      8 回(1 操作 1 回・多重なし)。teeth-drop watchdog が clean rebuild #1 →
      #2 で `ChangeHeadPart` 昇格まで進み**そこで収束**(以降の再発ログ無し)
      = 29fda68 の 3 段エスカレーションが高頻度 facegen 再構築下でも機能。
      content 不変のため `carrier manifest updated` は 0 回(変化検知ログの正常動作)。
- [ ] **M8 原文** (任意) `disableDefaults` ON にしても CORE Carrier が**出ないまま**
      であること(動的フォームはハード層で遮断 = 名前既定に依存しない)。OFF に戻す。

## §F P-A(1.3.2): fixture 系 — IMPL §R3 手順 1-5(基準 A ゲート)

> 準備: fixture ESP 2 種(**AllowedWrapper.esp** = playable な ARMO
> "Fixture Wrapper"、armorAddons が **DeniedAddon.esp** 内 ARMA を参照。NIF は
> 既存バニラ資産を流用)。**依頼があれば houseCARL で生成可能**。
> runtime/no-file **ARMA** 差替 fixture(R3 手順 2)は実行手段(実行時 armorAddons
> 書換ハーネス)が無いため**本ランではスコープ外** — 当該層は AdmitArmaSource の
> コード検査+MARA 実物(動的 ARMO)で部分カバー済みと記録する。
>
> **生成済み(2026-07-26, houseCARL)** — mod フォルダ `houseCARL - DeniedAddon` /
> `houseCARL - AllowedWrapper`。全 fixture は **slot FX01(=61)** / race DefaultRace /
> ArmorType Clothing / モデルはバニラ `Armor\Iron\{Male,F}\CuirassLight_1.nif`
> (weight slider 有効。値はバニラ `IronCuirassAA` から実読して転写)。
>
> | FormID | 型 | EditorID | 用途 |
> |---|---|---|---|
> | `000800:DeniedAddon.esp` | ARMA | `CEF_FixtureAddonDenied` | F1/F2 の deny 対象 |
> | `000800:AllowedWrapper.esp` | ARMA | `CEF_FixtureAddonAllowed` | F6 用(deny 操作の影響を受けない) |
> | `000801:AllowedWrapper.esp` | ARMO | `CEF_FixtureWrapper` "Fixture Wrapper" | playable。armature は **DeniedAddon 側 1 件のみ**(複数持たせると許可 addon にフォールバックしてテストが空振りする) |
> | `000802:AllowedWrapper.esp` | ARMO | `CEF_FixtureNonPlayable` "Fixture NonPlayable" | F6 用(MajorFlags=NonPlayable) |
>
> ロードオーダーは **DeniedAddon.esp → AllowedWrapper.esp** の順(マスター順)。
> 入手は `help "Fixture" 0 ARMO` → `player.additem <RuntimeFormID> 1`。
> 両 ARMO とも world model 無しなので**地面に落とさない**こと(表示されない)。
> deny 入力の意味論: **plugin は前方一致(大小無視)** =`DeniedAddon` でよい /
> **id は完全一致の正準形** =`000800:DeniedAddon.esp` / name は完全一致(末尾 `*` で前方一致)。

- [x] **F1 green(機能)/ 所見 X-UI2** wrapper を捕獲しようとする → ピッカーには出る
      (ARMO 自体は許可)が、選択で拒否。ログ実測(10:38:10 / 10:40:50 / 10:45:18):
      `ResolveArma: 801:AllowedWrapper.esp skips ARMA B8000800 from deny-listed
      plugin 'DeniedAddon.esp'` + `... has no admitted ARMA`。
      ⚠ **SMF ピッカーでは拒否理由が画面に出ない(無言)** → X-UI2。
- [x] **F2 green(r4 本命層の実証)** id `000800:DeniedAddon.esp` を ids へ →
      ログ実測(10:29:01 / 10:29:53): `ResolveArma: 801:AllowedWrapper.esp selects
      deny-listed ARMA '000800:DeniedAddon.esp' - refused`。鉄鎧メッシュ非表示化を
      確認。エントリ削除(10:32:51)で捕獲可能へ復帰。
- [x] **F3 green(v1.5.0 で再走・2026-07-26 23:57 → 07-27 00:22)** fixture wrapper を
      **box 57**(token `000813:CostumeFW.esp`、content 1 件だけの純粋な観測条件)へ
      捕獲し、SMF の Add から plugin `deniedaddon` を追加:
      - 画面: 鉄の鎧が**即時消滅**、Remove で**即時復活**(ユーザー確認)。
      - manifest: 追加時 23:57:41.695 と削除時 00:22:59.353 の**両方で
        `carrier manifest updated`**(= 変化検知ログが両遷移で発火)。最終状態の
        現物にも `000801:AllowedWrapper.esp -> Armor\Iron\F\CuirassLight_1.nif` が
        復帰済み → deny 中は当該 content が manifest から抜けていた。
      - settings: `Costume Box 57` の contents は**終始 `['000801:AllowedWrapper.esp']`**
        = quarantine-lite(隠すが捨てない)。
      ※ 入力は小文字 `deniedaddon` でも一致(plugin は前方一致・大小無視)。
- [x] **F4 green** エントリ削除で鉄鎧が即時復活。
      ※ deny 有効中に MCM から再捕獲を試みて出た拒否ウィンドウは**正しい挙動**
      (捕獲ゲートの拒否。MCM は理由を出す = SMF との差が X-UI2 の裏付け)。
- [x] **F5 部分 green** ログ実測: `carrier manifest updated` は全 5 回、いずれも
      別々の box 操作に 1 対 1(重複ゼロ)。persist head rebuild も
      `- / + / DoReset3D` が 1 操作 1 組(多重なし)。
      **deny 追加/削除方向も v1.5.0 で実測して green**: 追加(23:57:41.515)→
      manifest 1 回(.695)→ reload → `DoReset3D` 1 回(23:57:42.550)、
      削除(00:22:59.155)→ manifest 1 回(.353)→ reload → `DoReset3D` 1 回。
      **どちらも 1 操作 1 回**。対照として、box content に無関係な plugin
      (`mystique lingerie`, 22:46:28)の追加では reload は走るが
      `carrier manifest updated` は出ない = 変化検知が正しく黙っている。
      ※ 10:38:14 / 10:38:52 の mouth clean rebuild #1/#2 は既知の
      persist head-carrier teeth-drop watchdog の正常動作(自己修復済み)。
- [x] **F6 green** ログ実測 10:48:50 `allowNonPlayable = true` / 10:49:06 `= false`。

## §R P-A(1.3.2): 汎用回帰(監査 §5-4 + Blocked UI)

> 準備: §F の fixture(`houseCARL - DeniedAddon` / `houseCARL - AllowedWrapper`)は
> **R4 まで有効のままにしておく**。deny エントリの *追加* は X-UI1 のため
> `CEF_settings.json` の `captureBlacklist` 手編集 +「ディスクから再読込」で代用
> (R4 は manifest を見ないので X-MAN の影響を受けない)。

- [x] **R1 部分 green(2026-07-26 12:32-12:42)** 削除がログ+json へ反映:
      `capture: blacklist name entry removed 'testtesttest'`(12:32:44)/
      `... id entry removed '000801:AllowedWrapper.esp'`(12:42:09)。
      `disableDefaults` トグルは M8 で実証済み。
      **追加も v1.5.0 で green**: `capture: blacklist plugin entry added
      'mystique lingerie'`(22:46:28)/`... 'deniedaddon'`(23:57:41)= この
      ログ群で初めて出た "entry added"。`CEF_settings.json` の
      `captureBlacklist.plugins` にも永続化を現物確認。
- [x] **R2 green(2026-07-26)** hide-when-worn / body-morph opt-in /
      show-real-body の既存挙動が 1.3.2 でも不変(ユーザー目視)。
- [-] **R3 スキップ(経過観察へ)** RMSS(Selector of Skins)は互換性確認目的で
      入れてあるだけで**細かい設定を詰めていない**ため、肌一致の判定に足る条件が
      作れず未実施。RMSS 互換機能(2026-07-12 の base-skin-first + ApplySkinTextures)
      は**依頼元の Nexus ユーザーから以降クレームが無い**ことをもって暫定「動作中」
      と扱い、**経過観察**とする。→ 苦情が来た時点で再開(§R3 は v1.5.0 の
      ブロッカーにしない)。
- [x] **R4 green(2 層とも実証・2026-07-26)** ログ実測:
      - **R4-a(P2-1 登録境界)** 12:41:20 `register: inject
        '000801:AllowedWrapper.esp' not admitted - blocked: id on the deny-list
        (capture blacklist) (not registered)` → 注入されず。
      - **R4-b(ARMA 層)** id エントリ削除後の 12:42:33、reload バーストから
        独立した単発で `ResolveArma: 801:AllowedWrapper.esp skips ARMA B8000800
        from deny-listed plugin 'DeniedAddon.esp'` + `has no admitted ARMA`
        (成功時の `InjectArma ... 3p=` 行は無し)= admission を通ってから
        モデル解決側で拒否された署名。
      - **副産物(登録境界のもう 1 面)** 12:40:56 の reload で
        `register: box content '000801:AllowedWrapper.esp' not admitted - ...
        **(config kept, not registered)**` = quarantine-lite(設定は保持)の実証。
      - **手順 4 陽性対照 green(2026-07-27 00:33:54)**: deny を全削除した直後の
        `cef inject 000801:AllowedWrapper.esp` で
        `InjectArma 801:AllowedWrapper.esp 3p='Armor\Iron\F\CuirassLight_1.nif'
        1p='...'` = **deny が無ければ通る**side も実証。
      - 後始末は 2026-07-26 22:42 に完了(`disableDefaults = false`)。
      具体手順(fixture 利用・2 層を撃ち分ける):
      1. `cef list` / `cef shapes 000801:AllowedWrapper.esp` / `cef persist` が
         例外なく応答すること。
      2. **R4-a(P2-1 = 登録境界)** ids に `000801:AllowedWrapper.esp` を入れて
         reload → `cef inject 000801:AllowedWrapper.esp` →
         ログ `register: inject '000801:AllowedWrapper.esp' not admitted - ...
         (not registered)`([SkinRebind.cpp:2311](src/SkinRebind.cpp:2311))で
         **注入されない**こと。
      3. **R4-b(ARMA 層)** ids を外し plugins に `DeniedAddon` を入れて reload →
         同じ inject → 今度は admission を通り、`ResolveArma: 801:...
         skips ARMA ... deny-listed plugin` + `has no admitted ARMA` で
         **モデル解決側が拒否**すること(F1 と同じ経路をコンソールから踏む)。
      4. エントリを全削除して `cef inject` が通常どおり成功することを確認。
- [ ] **R5 未実施(意図的に最後)** 新規ゲームでの初期化・`Prepare for uninstall`
      → 再有効化の往復。セーブとログを畳むため、**§F/§R の再走(X-UI1 修正後)を
      終えてから**に回す。

## §B P-B(beta.1): NPC ゲート+スパイク+beta 既知バグ実証

> §12.1 回帰ゲート(NPC_SUPPORT_IMPL.md・未実施分):

- [ ] **B1** box: トークン装備→内容表示/解除→非表示。`cef list` 従来同等。
- [ ] **B2** persist: head-carrier セットで save/load → 歯が揃っている(watchdog)。
- [ ] **B3** hide-when-worn: ブーツ装備でネイル非表示→復帰。
- [ ] **B4** RaceMenu 開閉+性別変更→正しい性別モデルで再注入。
- [ ] **B5** セル移動・fast travel・save/load/別セーブ revert → 注入復元・二重無し。
- [ ] **B6** carrier 再装備で揺れ反映。`cef persist` の診断カウンタが動く。
- [ ] **B7** body-morph opt-in の形状追従・real-body content の肌一致。
- [ ] **B8** box 装備で合成 spell 付与/解除。
- [ ] **B9** 街 1 周 CTD/ログ異常無し(NPC binding ゼロなら Character フック no-op)。

> スパイク(§11・shipped 実装の事後実証):

- [ ] **B10 (S2)** publish トークンのスロット restamp: 発行トークンのスロットが
      ソース box と同じ表示スロットになり、競合装備が退避されること。
- [ ] **B11 (S4)** 同一 pub トークンを NPC 2 体へ → 両者表示+FSMP 揺れ・片側
      unequip の独立性。
- [ ] **B12 (S9)** NPC-persist トークン(non-playable)強制装備: UI 非表示・
      outfit リセット後の残留/自動復元ループの有無。
- [ ] **B13 (既知リスク観察)** publish トークン着用 NPC を殺害 → セル再訪で
      死体ロード → CTD/挙動を記録(**IsDead ゲート未実装の現状記録** —
      beta.2 で修正予定の監査 §4.2 項)。

> beta 既知バグの実機実証(2026-07-21 静的調査 → 修正は beta.2 予定):

- [ ] **B14** **PublishBox spell-leak**: エンチャント content 入り box を
      **着用したまま** publish → プレイヤーに "Costume Stats" が残留するか。
      残留した場合、pub トークン装備で効果が**二重**になるか。
- [ ] **B15** **UnpublishToBox 不可視**: unpublish で box 復帰 → stats/spell は
      正常だが**コスチュームが再表示されない**(settings reload/再起動で復活)か。

## §V P-C(VR): smoke(VR 環境があれば。無ければコミュニティ依頼文を別途)

- [ ] **V1** 起動: ログ `runtime: Skyrim VR` + build 刻印 + skee v4。
- [ ] **V2** box 着脱で表示/非表示。
- [ ] **V3** セル移動+RaceMenu 適用後に衣装が**再アタッチ**される
      (= vfunc 0x6A が VR でも Load3D — 監査 §5-1 の残項)。
- [ ] **V4** ピッカー2種が開く・捕獲 1 点・save/load。
- [ ] **V5** (MARA VR 0.0.2 を入れる場合のみ・任意) M1-M4 相当の VR 再現。

## §X 未解決の新規発見(次セッション引き継ぎ)

- [x] **X-SMP 解決(2026-07-26)= CEF の退行ではなく MO2 優先度事故**。M0 ゲートで
      新規導入した mod「CostumeExpansionFW test 1.3.2」が modlist 2 行目(最高位、
      live "CostumeExpansionFW" は 18 行目)にあり、配布 7z 同梱の**まっさら
      carrier NIF(234 バイト)と physics XML(42 バイト)が実 carrier を上書き
      マスク**していた(live: Box44_r1=1,028,906 / Box46_r4=3,556,017 /
      Box48_r2=516,428 / Persist_r1=1,897,317、XML Box44_r1=28,491)。
      整合の決め手: remap 警告が出るボーンは box44/46/48 の**骨入り carrier が
      実在する content のみ**。`CEF_carrier_manifest.json` は完全・正常 =
      **H-A(r4 seam)は無実**。自己修復しないのは sync が hash 一致で
      `built=0 skipped(unchanged)=9` を返すため。**対処 = test mod の `meshes` を
      削除/hide**(ユーザー実施済み)→ 再装備で復旧。
      **恒久策候補は X-DIAG(下)**。
      **2026-07-26 12:42 のログで復旧を実機確認**: `bound N bone(s) to FSMP
      physics-driven node(s)` が 111 行、remap 111 行、しかも**両者のボーン分布が
      完全一致**(XLS1/XLS2・VDFSA_bobr・Ellxe* とも bound と remap の両方に出る)
      = 3p は FSMP 駆動へバインド、1p は仕様どおり静的 remap
      ([SkinRebind.cpp:590](src/SkinRebind.cpp:590))という正常形。
      <details><summary>当時の triage 計画(記録)</summary>

      初動(次セッション):
      1. まず**トークン再装備**(carrier apply はユーザー駆動が設計原則)で直るか
         → 直るなら「テスト中の連続 sync で carrier リビジョンが回った」だけの
         仕様挙動の可能性。直らないなら実バグ。
      2. 証拠採取: CEF ログ(remap warn 行・auto-sync 結果・manifest updated 回数)、
         `CEF_carrier_manifest.json` と `carriers.json` の現物(**box contents が
         manifest から消えていないか**)、発生直前の操作(Blocked ページの
         スイッチ/エントリ操作? F テスト? reload?)、build 刻印、MARA 有無。
      3. 仮説ランキング:
         **H-A(r4 退行疑い・最有力查点)**: manifest の resolveContent が
         `ResolveAdmittedModelPath(..., a_log=false)` で**静かに失敗**し content が
         manifest から**無音で脱落** → carrier が骨なしで再生成 → 全 SMP 静的化・
         自己修復不能(レビューの「no silent caps」違反類型)。manifest 現物で即判定可。
         **H-B**: blacklist 編集の ReevaluateContentAdmissions(フル reload)と
         FSMP 世代/carrier 再装備原則の相互作用(detach→再注入が新 carrier の
         育骨前に走る等)。
         **H-C**: P-A プロファイルの FSMP セットアップ/defaultBBPs 差異(CEF 外)。
         **H-D**: 仕様どおり(revision 回転後の再装備待ち)を「バグ」と誤認。
      4. H-A が黒なら: seam 失敗理由の特定(EffectiveSexFor/policy/ResolveFormId の
         どれが false か)→ manifest 側にだけ **脱落ログ(1 行/content)** を追加する
         修正が最小(quiet 原則は列挙側のみに限定する)。
      </details>

### v1.5.0 で直す(2026-07-26 §F ランで確定)

- [x] **X-DIAG 修正・実機で発火→追跡→収束まで実証(v1.5.0)** 2026-07-26 22:41:39 に
      box46 へ `0 of 18 custom bone(s) ... carrier = 'CostumeFW/Box46_carrier_r6.nif'`
      を出力 → **トークン再装備**で 00:35:45 に
      `bound 13 bone(s) ... VDFSA_bobr00->hdtSSEPhysics_AutoRename_Armor_...` へ
      回復し、以後この診断行は出ていない = 「リビジョンが回って再装備待ちだった」
      という**正しい診断**で、回復後は黙る(再武装ロジックも意図どおり)。
      carrier 診断 warn: manifest が content を宣言しているのに carrier
      側で期待ボーンが **1 本も見つからない**場合、`carrier NIF の実パス + 期待/実際の
      ボーン数`を warn 出力する。現状は「アタッチ済みで骨 0」と「まだアタッチ中」を
      区別できず、retry 予算 4([SkinRebind.cpp:309](src/SkinRebind.cpp:309))を
      使い切って**黙って諦める** — X-SMP が 10 秒で判るはずが一晩かかった直接原因。
- [x] **X-UI1 修正・実機実証済み(v1.5.0, 798fe6f)** SMF「Blocked」ページの **Add が事実上機能しない**。
      コンボ+InputText+Add を幅指定なしで `SameLine` 連結しているため
      ([SmfUI.cpp:838-856](src/SmfUI.cpp:838))狭い窓では入力欄/ボタンが画面外へ
      押し出される。加えて `Button(...) && s_blkValue[0] != '\0'` で**空入力時は
      無言 no-op**。ログ実測でも `blacklist ... entry added` は 1 行も出ず、
      remove/switch のみ出ている。修正 = `SetNextItemWidth` で幅を確定 +
      `ImGuiInputTextFlags_EnterReturnsTrue` で Enter 追加 + 空入力時のフィードバック。
      **F3/F5 の残りはこの修正後に再走**。
- [x] **X-UI2 修正・実機確認済み(v1.5.0, 2026-07-27)** 拒否時に画面上部へ通知が出ることを確認。SMF ピッカーの**捕獲拒否が無言**。`QueueCapture` は理由を
      `s_status` に入れる([SmfUI.cpp:115](src/SmfUI.cpp:115))が、Boxes ページの
      表示位置はページ最上部([SmfUI.cpp:386](src/SmfUI.cpp:386))で、ピッカーは
      box の TreeNode 内部の深い位置 — 選択直後に目に入らない。MCM は拒否
      ウィンドウを出すので体感差が大きい。修正 = 拒否時に `DebugNotification`、
      またはピッカー直下へステータス行を出す。
- [x] **X-LOG1 修正・実機確認済み(v1.5.0)** — 2026-07-26/27 のセッションで
      `ResolveArma: 801:AllowedWrapper.esp has no admitted ARMA (every candidate
      refused by the capture policy)` が **[warning]**、かつセッション全体の
      **`[error]` は 0 件**(修正前は同一行が 7 件の error)。(小) 明示 deny による `ResolveArma: ... has no admitted ARMA` が
      **[error]** レベル。2026-07-26 のランでは**ログ中の error 7 件が全てこれ**で、
      設計どおりの拒否が「障害」に見える。修正 = 拒否理由が policy 由来のときは
      warn へ落とす(真の解決失敗のみ error に残す)。
- [x] **X-MAN 修正・実機実証済み(v1.5.0)** `ReloadSettingsFromDisk`(MCM/SMF の「ディスクから再読込」・
      [Papyrus.cpp:584](src/Papyrus.cpp:584) / [SmfUI.cpp:308](src/SmfUI.cpp:308))は
      **manifest を書き直さない**。`WriteCarrierManifest` は
      `ReevaluateContentAdmissions`([BoxStore.cpp:2178](src/BoxStore.cpp:2178))
      にしか無いため、json を手編集して reload した場合ポリシー変更は効くのに
      manifest だけ stale で残る(実測: 10:45:18 に deny 追加 → manifest の mtime は
      10:42 のまま)。修正 = reload 末尾でも manifest を書く(または reload を
      quarantine トランザクションへ一本化)。

## §W ラン後の処理

- [ ] **W1** 本ファイルへ結果記入 → commit。NG は ID ごとに切り出し。
- [ ] **W2** §A+§M green → **報告者返信(§7.1-r5)を投稿**。
- [ ] **W3** §M+§F green → **基準 A のリリースゲート通過**と判定(基準 A/B の
      最終判断はオーナー)。→ タグ v1.3.2・GitHub/Nexus 公開へ。
- [ ] **W4** §B の結果 → beta.2 修正リスト確定(IsDead ゲート・B14/B15 の
      publish/unpublish 修正・mara-guard merge)。
- [ ] **W5** 動作確認完了をもって `K:\dev\CEF_video_config_backup_2026-07-22`
      を削除(2026-07-23 復元済みの後始末)。
