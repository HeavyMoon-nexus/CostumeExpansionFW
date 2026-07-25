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
- [ ] **M8** (任意) `disableDefaults` ON にしても CORE Carrier が**出ないまま**
      であること(動的フォームはハード層で遮断 = 名前既定に依存しない)。OFF に戻す。

## §F P-A(1.3.2): fixture 系 — IMPL §R3 手順 1-5(基準 A ゲート)

> 準備: fixture ESP 2 種(**AllowedWrapper.esp** = playable な ARMO
> "Fixture Wrapper"、armorAddons が **DeniedAddon.esp** 内 ARMA を参照。NIF は
> 既存バニラ資産を流用)。**依頼があれば houseCARL で生成可能**。
> runtime/no-file **ARMA** 差替 fixture(R3 手順 2)は実行手段(実行時 armorAddons
> 書換ハーネス)が無いため**本ランではスコープ外** — 当該層は AdmitArmaSource の
> コード検査+MARA 実物(動的 ARMO)で部分カバー済みと記録する。

- [ ] **F1** wrapper を捕獲しようとする → ピッカーには出る(ARMO 自体は許可)が、
      選択で拒否: UI 文言は "mesh could not be resolved" 系、**ログに
      `ResolveArma: ... skips ARMA ... deny-listed plugin 'DeniedAddon.esp'`**。
      (deny 対象 plugin の既定化のため、事前に Blocked ページで plugin
      `DeniedAddon` を追加してから)
- [ ] **F2** F1 の plugin エントリを外し、代わりに **ARMA の colon-id**
      (例 `000800:DeniedAddon.esp`)を ids へ追加 → 同様に拒否(final-ARMA ID deny)。
      エントリ削除 → 捕獲可能に戻る。
- [ ] **F3** (エントリ無しで)wrapper を box へ捕獲・表示させ、armor/weight/enchant
      付き content と同居 → Blocked ページで plugin `DeniedAddon` を追加 →
      **即時**: 表示消滅・token stats/keywords 縮小・player の合成 ability から
      効果消滅・`CEF_carrier_manifest.json` から該当 content 消滅。
      **設定(box 内容)からは消えない**こと。
- [ ] **F4** F3 のエントリを削除 → 表示・stats・ability・manifest が**自動復元**。
- [ ] **F5** F3/F4 の各操作で: manifest 更新が**操作あたり最大 1 回**・
      persist head rebuild が多重実行されない(ログで確認)。
- [ ] **F6** non-playable ARMO(`player.additem` で投入)がピッカー非表示 →
      Blocked ページ `allowNonPlayable` ON で出現 → OFF に戻す。

## §R P-A(1.3.2): 汎用回帰(監査 §5-4 + Blocked UI)

- [ ] **R1** Blocked ページ: 既定表示("CORE Carrier"/"MARA*")・name/plugin/id の
      追加/削除・`disableDefaults` トグルが json(`captureBlacklist`)へ永続化。
- [ ] **R2** hide-when-worn / body-morph opt-in / show-real-body の既存挙動不変。
- [ ] **R3** RMSS(Selector of Skins)を P-A に入れている場合: show-real-body の
      肌一致(base-skin-first 修正の実機確認 — 2026-07-12 以来 pending)。
- [ ] **R4** `cef` コンソール一式(list/shapes/persist)無事。`cef inject` に
      deny 対象 id を渡すと**拒否ログ**が出て注入されない(P2-1 ゲート)。
- [ ] **R5** 新規ゲームでの初期化・`Prepare for uninstall` → 再有効化の往復。

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

## §W ラン後の処理

- [ ] **W1** 本ファイルへ結果記入 → commit。NG は ID ごとに切り出し。
- [ ] **W2** §A+§M green → **報告者返信(§7.1-r5)を投稿**。
- [ ] **W3** §M+§F green → **基準 A のリリースゲート通過**と判定(基準 A/B の
      最終判断はオーナー)。→ タグ v1.3.2・GitHub/Nexus 公開へ。
- [ ] **W4** §B の結果 → beta.2 修正リスト確定(IsDead ゲート・B14/B15 の
      publish/unpublish 修正・mara-guard merge)。
- [ ] **W5** 動作確認完了をもって `K:\dev\CEF_video_config_backup_2026-07-22`
      を削除(2026-07-23 復元済みの後始末)。
