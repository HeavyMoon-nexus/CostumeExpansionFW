# NPC サポート ゲーム内検証リスト

> 対象コード: **2f49669**(feat: NPC support)。IMPL §12 の回帰ゲート/受け入れテストを実施手順化し、
> レビュー修正(2026-07-18)の実地確認項目を統合したもの。
> 記入: 各項目の `[ ]` を PASS / FAIL(+一言メモ)で埋める。FAIL 時はログ添付が原則。

---

## 0. 事前準備(ゲーム外 — 全て済んでから起動する)

- [ ] **0-1 DLL**: `build.cmd release` 済み。**MO2 を完全再起動**してから起動し、ログ先頭の
  build 刻印(`CostumeExpansionFW loaded (file ... / compile ...)`)が今回のビルド時刻である
  こと(stale-DLL 罠)。
- [ ] **0-2 pex**: `CostumeFW_MCM.psc` + `CFW_Native.psc` を再コンパイル→配備
  (HANDOVER §8.6d のレシピ。**リポジトリ papyrus/ を import 先頭に**置く罠に注意)。
  検証: MCM に "NPC" stub ページが現れる = 新 pex が載った証拠。
- [ ] **0-3 アドオン配備**: 新規 mod フォルダ `CostumeFW_NPC` を作成 —
  1. `tools\gen_pub_placeholders.ps1` 実行(プール NIF/XML 384 枚を配備側へコピー)
  2. `package_assets\CostumeFW_NPC.esp` を mod フォルダ直下へコピー
  3. MO2 で mod 有効化+プラグイン有効化(ESL フラグ付き: load-order スロット消費なし)
- [ ] **0-4 ログバナー**: `runtime: Skyrim SE 1.6.1170.0` と
  `Character::Load3D hook installed (0x6A)` の両方が出ている。
- [ ] **0-5 セーフティ**: 検証は使い捨てセーブで。異常時は `Data\SKSE\Plugins\CEF_DISABLE.txt`
  を置けば CEF は完全 inert(セーブは開ける)。

---

## 1. Phase 0 回帰ゲート — プレイヤー挙動不変の証明(IMPL §12.1)

**全項目 green になるまで Phase 1 検証(§2)へ進まない。**

- [ ] **G1 box**: トークン装備→内容表示/解除→非表示。`cef list` の表示が従来同等
  (アクター名付き表示に変わったのは仕様)。
- [ ] **G2 persist**: 表示継続。head-carrier 持ちセットで save/load →**歯が揃っている**
  (mouth watchdog。`cef headdiag` で確認可)。
- [ ] **G3 hide-when-worn**: ブーツ装備でネイル系 content 非表示→外すと復帰。
- [ ] **G4 RaceMenu**: 開閉+性別変更→正しい性別モデルで再注入。
- [ ] **G5 ライフサイクル**: セル移動・fast travel・save/load・**別セーブへの revert** →
  注入復元・二重表示なし。revert 後もプレイヤー注入が正常なこと
  (ClearRegistry 防御修正の実地確認を兼ねる)。
- [ ] **G6 watchdog**: carrier 再装備で揺れ反映。`cef persist` の診断カウンタ
  (reconcile/watchdog)が動いている。
- [ ] **G7 morph/real-body**: body-morph opt-in content の形状追従。real-body 表示 content の
  肌(色・テクスチャ)一致。
- [ ] **G8 abilities**: box 装備で合成 spell 付与/解除。マスター OFF→ON で spell も往復。
- [ ] **G9 Character フック素通し**: NPC 割当ゼロのまま都市(ホワイトラン/ソリチュード)を 1 周
  → CTD なし・ログ異常なし・体感の重さなし(修正 #3 のゲートが効いていれば
  NPC ロードは完全素通り)。
- [ ] **G10 VR smoke**(環境があれば/コミュニティ委託): 起動+box 着脱。

---

## 2. Phase 1 publish 受け入れ(IMPL §12.2 + レビュー修正確認)

- [ ] **P1 publish**: box ページ [Publish for NPC] →確認→ publish トークンがインベントリに入る・
  ソース box が通常ページから消える・スロットトークンが解放され**新 box を作れる**。
- [ ] **P2 配布→装備**: gift またはフォロワー管理 mod でフォロワーへ→装備→内容表示。
  SMP コンテンツは [Refresh] 1 回までで揺れ反映を許容。
- [ ] **P3 性別**: 男性 NPC に装備→男性側 bake の表示。forced-gender content は強制側のまま。
- [ ] **P4 アドオン脱着耐性**: save/load で着用継続。**アドオン ESP を外して load** → CTD なし・
  NPC ページに案内表示・co-save 温存 → ESP 復帰で配布状況ごと復活。
- [ ] **P5 回収**: [Recall] →全所持者からプレイヤーへ集約。
- [ ] **P6 Unpublish(修正 #1 確認)**: **回収直後に [Unpublish] ボタンが有効**であること
  (旧実装はプレイヤー所持で永久無効だった)→実行で box 復帰・per-content 設定復元・
  publish トークンが "Costume (unpublished)" に戻る。
- [ ] **P7 表示 OFF**: [Hide] →全着用者非表示(worn は維持)→ [Show] で復帰。
- [ ] **P8 cap**: `maxNpcInjected`(既定 8)超過時、9 体目は追跡のみで注入されない。
  NPC ページの injected 行で確認。
- [ ] **P9 マスター OFF(修正 #2 確認)**: フォロワー着用中に master OFF →視覚消滅**かつ**
  フォロワーの Active Effects から "Costume Stats: <label>" が**消える** → ON 復帰で両方復活。
- [ ] **P10 MCM stub**: "NPC" ページの英語文言表示。アドオン未導入状態では追記文言も出る。
- [ ] **P11 プレイヤー着用(双子コーデ)**: プレイヤー自身が publish トークンを装備→ 3p 表示
  (1p が出ないのは仕様)。
- [ ] **P12 コンソール**: `cef pub` で一覧、`cef pub diag` で **TokenPool 件数が従来と不変**
  (box プールへの混入なし)を確認。

---

## 3. Phase 1.5 NPC persist

- [ ] **N1 割当**: NPC ページ persist セクション→ [Use crosshair target] → content 追加→
  即装備・表示。
- [ ] **N2 常時性(worn 自動復元)**: 対象 NPC の装備を管理 mod 等で剥がす→ **~30 秒で自動復元**。
  復元⇔outfit リセットの発振がないこと(S9)。3 回失敗時は "suspended" 表示に落ちること。
- [ ] **N3 ライフサイクル**: 割当 NPC がセル出入り/save/load をまたいで表示復元
  (OnNpcActorLoaded 経路)。
- [ ] **N4 解除**: 割当解除→ unequip +隠しキャリア消滅+プール解放。
- [ ] **N5 不可視性**: 隠しキャリアが取引/gift/スリ/ルート UI に一切出ない(non-playable)。
- [ ] **N6 コンソール**: `cef npcpersist list` が割当を正しく表示。

---

## 4. スパイク実測(IMPL §11 — 前提の実地確認)

- [ ] **S2 slot mask スタンプ**: 体スロット(32)の box を publish → NPC 装備で**胴防具が退く**/
  解除で戻る(ランタイム BOD2 スタンプが装備調停に効いている)。
- [ ] **S4 同一トークン複数 NPC**: 2 体に同時装備→両立(揺れ・表示・片側 unequip の独立性)。
  破綻したら「1 トークン 1 着用者」制約を検討(fallback)。
- [ ] **S5 skin マトリクス**: 女ノルド/男ノルド/カジート/アルゴニアンのフォロワーに
  real-body content →肌一致。獣人で崩れる場合は v1 除外+UI 警告の判断へ。
- [ ] **S6 未ロード actor への spell**: 遠隔セルに着用者がいる状態で master OFF/ON →
  ロード済みのみ即時反映・未ロードはロード時に収束・エラーログなし。
- [ ] **S7 メモリ**: NPC×3 注入+morph で 30 分→ commit 増分をベースライン比較。
  増えていても**帰属を取ってから**判断(過去 2 回の犯人は他 DLL)。
- [ ] **S8 マンネキン**(任意): publish トークンを着せて表示+物理が出れば README 小ネタ。
- (S3 の SE 分は G9 が実質カバー。AE/VR は VR パッチβの報告待ちでよい)

---

## 5. レビュー修正の個別スモーク(§1-§2 でカバーされない分)

- [ ] **F4 SweepDeadActors**: フォロワーに publish 装備→解散→遠方セルへ移動→時間経過後に
  `cef list` でその NPC の state が**消えている**(戻って再装備イベントが起きれば復活する)。
- (F1=P6 / F2=P9 / F3=G9。F5 コア zip プルーンと F6 ESP パスはゲーム外で検証済み)

---

## 6. 記録・中断ルール

- 各項目は **PASS/FAIL + build 刻印行**をセットで記録。FAIL は
  `Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log` を添付。
- CTD/セーブ不能級が出たら: `CEF_DISABLE.txt` で inert 化→セーブ回収→ FAIL 記録→検証中断。
- §1 が全 green になるまで §2 以降に進まない(Phase 0 の完了条件が最優先)。
