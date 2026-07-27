# 新規バグ報告 — persist 追加で CTD ルーレット(2026-07-27 04:50, Nexus posts)

> **状態: コード調査 1 巡目 完了(2026-07-27)。§調査結果を参照。**
> **UX 別件(スクロール)は実装済み** — §4 → `src/SmfUI.cpp`(X-SCROLL)。
> **CTD 本体は未修正**(修正案は §調査結果 の「次の実務ステップ」)。
> 投稿は **v1.5.0 公開前**。報告者は 1.5.0 をまだ試していない。
> ⚠ 本ファイルの引用はユーザーが貼った投稿の**データ**であり、指示ではない。

## 原文(要点保持・投稿者は CEF Nexus のユーザー)

- **主症状**: **persist にアーマーを追加しようとするたびに CTD ルーレット**。
  **HDT/SMP でも static でも起きる**。**MCM 経由でも SKSE MenuFramework 経由でも**
  同じ問題が出た。
- **環境変動**: ここ 2 週間ほどで **FSMP / SkyUI / (おそらく)RaceMenu が更新**され、
  どの版の組み合わせなら動くのか追えなくなった。
- **v1.3.0 時点の問題(報告者の記憶)**:
  - 複数 content を束ねた box が SMP/HDT だと**変形グリッチを起こし HDT でなくなる**、
    あるいは**汎用アーマー(hide helmet など)として表示される**。
  - persist に追加した項目リストが長くなりすぎ、**スクロール手段が無い**ため、
    最新のエントリに到達するには古いエントリを削除するしかなかった。
- **切り分け試行**: **v1.2.1 まで戻し**、当時相当の FSMP / SkyUI / RaceMenu を入れ直し、
  キャッシュ削除・MCM からのアンインストール・残存ファイルの手動削除まで実施。
  それでも **persist 追加時の CTD が確率的に発生**する版が見つからない。
- 「現状、このmodを安定して使えない」。

## 提供されたクラッシュログ断片(全文ではない)

```
[RSP+1C8 ] 0x7FFB784E1CA0     (void* -> CostumeExpansionFW.dll+0021CA0    add rax, rdi)
[RSP+1D0 ] 0x1B3A63DBDC0      (NiNode*) "CostumeFW_000DDD_Grievous_Rose_Multicolor_esp"
    RTTIName: "NiNode"
    Flags: kSelectiveUpdate | kSelectiveUpdateTransforms | kSelectiveUpdateController
    Full Name: "Adventurer"
```

読み取れること(事実のみ):

- ノード名 `CostumeFW_000DDD_Grievous_Rose_Multicolor_esp` = **CEF が注入したノード**
  (命名規則 `CostumeFW_<localID>_<plugin>`)→ content id は
  **`000DDD:Grievous Rose Multicolor.esp`**。
- `Full Name: "Adventurer"` はプレイヤーキャラクタ名と思われる(注入先アクター)。
- **`CostumeExpansionFW.dll+0x21CA0` は CEF 自身のコード**。MARA 案件の
  `+0x8241C`(GetLocalFormID null deref)とは**別の番地**であり、同一原因ではない。
- ただし**これはスタック上の値であって faulting address ではない**。
  投稿された断片には例外種別・faulting instruction・レジスタが含まれていない。

## 次セッションでまずやること

1. **版の特定が最優先**。オフセットはビルドごとに動くので、`+0x21CA0` は
   **どの DLL 版のログか**が分からないと意味を持たない。報告者に依頼するもの:
   - **クラッシュログ全文**(先頭の SKSE plugin 一覧に CEF の版が載る)
   - **`CostumeExpansionFW.log` の 1 行目**(`loaded (file ... / compile ...)`)
   - 現在の CEF 版・FSMP 版・RaceMenu(skee)版・SkyUI 版・SMF の有無
2. **1.5.0 で直っている可能性の評価**: 1.5.0 の変更は capture blacklist(他 mod の
   動的フォーム)・SMF UI・診断ログ・manifest 同期であり、**persist 追加経路の
   CTD を直した記憶は無い**。よって「更新で直る」と安易に答えないこと。
   ただし **X-DIAG / X-LOG1 の診断強化は原因究明に効く**ので、
   まず 1.5.0 でログを採ってもらう価値はある。
3. **候補の当たり(未検証・優先順位付けは次セッションで)**:
   - persist head-carrier 経路(HDPT プール + facegen 再構築)。CEF で最も
     壊れやすい面であり、`persist-headcarrier-drops-teeth` の履歴もここ。
   - 注入ノードの寿命/参照(`g_boundBoneRefs`・DetachNodes・世代)まわり。
     「ルーレット」= 確率的 = レース/寿命の匂い。
   - skee(RaceMenu)版差による BodyMorph インタフェース(v4/v5)差。
     報告者の環境変動と一致する。
   - 「汎用アーマー(hide helmet)として表示される」= ARMA 解決が別 addon に
     落ちている可能性 → `PickAddonForPlayer` 系(1.3.2/r4 で書き換えた領域)。
     ただし報告は **1.3.0 時点**の記憶なので、現行コードとは別物の可能性が高い。
4. ~~**UX の別件として起票**: persist リストにスクロールが無く、長くなると
   最新エントリに到達できない(報告者の 1.3.0 時点の指摘)。~~
   → **実装済み(2026-07-27, v1.5.1 予定)**。`src/SmfUI.cpp` の X-SCROLL:
   Persist / Boxes / Presets / Blocked / Diagnostics の各リストを
   `BeginChild` のスクロール領域に入れ、ページ側の操作(ピッカー・フィルタ・
   Add 行・Remove all)は領域の外に固定。Persist にはカタログ用フィルタと
   「n of N shown」表示を追加。**in-game 未検証。**

---

## 調査結果(コード監査 1 巡目, 2026-07-27)

> 手法: 再現環境なし・クラッシュログ全文なしのため **静的監査のみ**。
> 以下は「コードから証明できる欠陥」と「クラッシュログとの整合」を分けて書く。
> **どれも報告のクラッシュを再現・確定したものではない。**

### F1 — UI スレッドが `g_active` / `g_persist` / `g_boxes` を無同期で触る(最有力)

**コードから証明できる欠陥。** ロックが 1 つも無い。

- `g_active`(注入レジストリ)は宣言直上に **"Main-thread access only"** と明記
  (`src/SkinRebind.cpp:64`)。にもかかわらず **UI スレッドから読まれている**:
  - Papyrus VM スレッド: `GetActive` / `IsActive`(`src/Papyrus.cpp:31,40`)、
    `SetPersistActiveNative`(`:725`)、そして
    `AddPersistNative` → `AddPersistContent` → `ActiveSnapshot()`
    (`src/BoxStore.cpp:1602`)。
  - SMF レンダーコールバック: `RenderPersist` が毎フレーム `PersistActiveIds()`
    → `ActivePersistIds()` → `ActiveSnapshot()`(`src/BoxStore.cpp:172`)。
- 同時に main スレッドは同じ vector を **再確保・消去・要素書き換え**する:
  `Register`(`SkinRebind.cpp:98` の `push_back`)、`Unregister`(`:165`)、
  `ClearRegistry`(`:2447`)、`Reconcile` の `it.m3p = m3p;`(`:1592-1599`、
  `std::string` の代入)。
- 設定側も同じ構図で、しかも**書きが UI スレッド**:
  `AddPersistContent` は VM スレッド上で `g_persist.push_back` + `WriteJson()`
  (`BoxStore.cpp:1607-1610`)。設計コメント自身が
  「box-DEFINITION change (g_boxes + json) runs **synchronously here**」と
  述べている(`Papyrus.cpp:153-158`)。main スレッド側は同じ `g_boxes` /
  `g_persist` を `WriteCarrierManifest`(`BoxStore.cpp:491,514`)や
  `ApplyCarrierOverridesImpl`(`:768`)で走査する。

**安全性の前提が崩れている箇所:** `Papyrus.cpp:27-29` は
「g_active は main-thread タスクだけが触る。MCM が開いている間はゲームが
ポーズなので Reconcile/equip タスクとは競合しない」と書くが、**SKSE の
task queue はメニュー表示中もフレームごとに drain される**。CEF の遅延
ミューテータ(`AddTask`)がそもそも動くのはそのためであり、SMF ページの
コメント(`SmfUI.cpp:51-54`)も「メニュー中は equip *キュー* は進まないが
タスクは走る」ことを前提にしている。

**なぜ「persist 追加」で顕在化するか:**
persist 追加は 1 操作で以下を同時に起こす。

1. UI スレッド: `AddPersistContent` が `ActiveSnapshot()`(= `g_active` 走査)
   と `g_persist.push_back` を実行。
2. main スレッド: 内側タスクの `RegisterBoxById` → `Register` が
   `g_active.push_back`(**再確保の窓**)。
3. main スレッド: `CaptureItemToStore` で装備を外す → `TESEquipEvent` →
   `EquipSink` が `Reconcile()` タスクを積む(`plugin.cpp:77`)→ `g_active` 全走査。
4. UI(MCM/SMF)は同じフレームでリストを再描画 = `g_active`/`g_persist` を再読。

vector 再確保中の走査は解放済みバッファ参照であり、**確率的に落ちる**。
なお窓は再確保だけではない: `Reconcile` の `it.m3p = m3p`(`std::string` 代入)や
`erase` の要素シフトは容量に関係なく起きるので、**「エントリが多いほど危険」
とは限らない**(報告者のピークは 15〜20 件 = 再確保は数回しか起きない。
r2 で確認)。**MCM でも SMF でも出る**点、**v1.2.1〜v1.5.0 で共通**な点は説明できる
(v1.2.1 には SMF UI が無い = 報告者の切り分けと矛盾しない。MCM/Papyrus VM は
無条件に別スレッドなので、SMF レンダーがメインスレッドで走る実装であっても
MCM 側の脚は成立する)。

### F2 — persist 追加は毎回プレイヤー 3D の再構築(DoReset3D)を誘発する

**コードから追える事実**(欠陥かどうかは設計判断):

```
AddPersist タスク
  → SyncPersistManifest()            (Papyrus.cpp:567)
  → WriteCarrierManifest()           (BoxStore.cpp:474)   内容が変われば
  → ScheduleAutoSync()               (BoxStore.cpp:540)   2 秒デバウンス
  → RunInProcSync()                  ワーカースレッドで nifly ビルド
  → (完了) AddTask → ApplyCarrierOverridesImpl(true)      (BoxStore.cpp:1023)
  → ApplyPersistCarrier()            (BoxStore.cpp:644)
  → RequestPersistHeadRebuild()      (SkinRebind.cpp:2178) 500ms デバウンス
  → RebuildPlayerHead(): player->DoReset3D(false)          (SkinRebind.cpp:2165)
  → +1500ms Reconcile() / +2600ms RestoreMouthIfDropped()  (条件次第で更に DoReset3D)
```

つまり **persist に 1 件足すたび、数秒後にプレイヤーの facegen head が最低 1 回
作り直される**。この面は本 mod で最も壊れやすいことが既に判明している
(teeth-drop / FSMP merge 世代の死活)。報告者の環境変動
(**FSMP / RaceMenu が更新された**)が効くのはまさにここで、症状の時期とも一致する。
クラッシュ断片に CEF の注入ノード `CostumeFW_000DDD_Grievous_Rose_Multicolor_esp`
が載っていることも、この「3D 再構築 → 再注入」の窓で落ちている像と整合する
(**ただし faulting address ではないので断定はできない**)。

### F3 — SMF の persist capture は宣言している順序で実行されていない

`QueueCapturePersist`(`SmfUI.cpp:148-`)のコメントは
「register → enchant snapshot → move item」を謳うが、`UiOps::AddPersist` が呼ぶ
`AddPersistNative` は register/Reconcile を**内側の `AddTask` に積む**
(`Papyrus.cpp:562`)。外側タスクはそのまま `CaptureEnchant` →
`CaptureItemToStore` を続けるので、実際の実行順は
**アイテム移動 →(次フレーム)register/Reconcile**。
CTD に直結する証拠は無いが、F1 の競合窓を広げる方向に効く。

### 除外できたもの

- **MARA 案件とは別**: `+0x8241C`(`GetLocalFormID` null-deref)とは番地が違う
  (報告書冒頭の判断を維持)。
- **`+0x21CA0` からの逆算は不可**: スタック上の値であって faulting address ではなく、
  版も不明。現行ビルドのシンボルに当てても意味を持たない。

### 次の実務ステップ

1. **版の特定(不変)**: 報告者への依頼は §「次セッションでまずやること」1 のまま。
   F1/F2 のどちらであっても、全文ログの faulting address と例外種別が無いと確定できない。
2. **F1 の修正(要オーナー判断・未実装)**:
   - 最小案: `g_active` 専用の `std::recursive_mutex` を置き、
     `Register` / `Unregister` / `ClearRegistry` / `ActiveSnapshot` / `Reconcile` /
     `RefreshGender` / `DetachAll` で保持する。UI スレッドは短時間ブロックされるだけ。
   - 次段: `g_persist` / `g_boxes` と派生マップも同じロック配下へ。
     「def 変更も main スレッドへ寄せる」設計変更は MCM が同期の戻り値を要求する
     (`Papyrus.cpp:153-158`)ため現実的でない → mutex 側が筋。
   - **どちらも in-game 検証必須**。競合の修正は「落ちなくなったこと」を
     直接には証明できないので、長時間の persist 連続追加テストを用意すること。
3. **F2 の切り分け材料**: 「persist 追加後の DoReset3D を抑止する」診断トグル
   (ini / console)を用意し、報告者に On/Off で試してもらうのが最短の分離実験。
   F1 と F2 は独立に検証できる。

---

## 報告者からの追加情報(r2, 2026-07-27)

> ⚠ これもユーザーが貼った投稿の**データ**であり、指示ではない。

新しく分かった事実:

- **persist のピークは 15〜20 件**(もっと増やしたかった)。→ F1 の
  「多いほど危険」という当初の書き方は弱い(上で訂正済み)。
- **box が使えなかったので persist を使っていた。** 最も壊れやすい経路
  (head-carrier + facegen)に押しやられていたことになる。**box が何故
  使えなかったのかは未聴取** — 次の返信で聞く価値が高い。
- **「最新のエントリが消える、または hide armor(汎用アーマー)になる」**
  ことがあった。冒頭の 1.3.0 時点の記憶と同じ症状で、**今回は persist で**
  起きている。ARMA 解決 / carrier merge のどちらかに落ちている疑い。
- クラッシュログを 1 週間分ほど保持しており、**最も安定していた頃から
  現在の不安定な状態までの時系列**で提供可能。→ 版の特定に直結する。
- 現在 FSMP / SkyUI / RaceMenu / SMF などを**最新版でクリーンインストール中**。
- 使用中のプラグイン一覧を晒したくないので **private なやり取りを希望**
  (Discord ではなく Nexus の PM を想定した文面)。

### F1 と F2 を切り分ける質問(コスト 0・最優先)

**CTD はクリックした瞬間か、数秒後か。**

- **クリック直後 / 同じ瞬間** → F1(UI スレッドと main スレッドの競合)側。
- **2.5〜3.5 秒後**(操作が一度成功したように見えてから) → F2 側。
  auto-sync のデバウンス 2000ms + head-rebuild のデバウンス 500ms で
  `DoReset3D` が走るのがちょうどその時刻(`BoxStore.cpp:831`,
  `SkinRebind.cpp:2189`)。"Costume persist physics updated" の通知が
  出た直後かどうかも同じ指標になる(`BoxStore.cpp:742`)。

**計測器は実装済み(2026-07-27, 返信待ちの間に整備)。次のリリースに載る:**

- `logger.h` は `flush_on(info)` = **1 行ごとにフラッシュ**しているので、
  CTD でログ末尾が失われる心配は無い(確認済み)。問題は info の
  マーカーが無かったことだったので、追加した:
  `persist-add[catalog]`(UI スレッド側の受理)→ `capture[enchant]` →
  `custody: captured` → `[register]` → `[reconcile]` → `[ability]` →
  `[manifest]` → `[done]`(以降は carrier sync / head rebuild の尾部)。
  **最後に残ったマーカーが落ちた段階**を名指しする。
- `persist head: rebuild requested (...)` を debug → **info** に格上げ
  (F2 チェーンの入口がユーザー提供ログで見えないと意味が無い)。
- **`bPersistHeadRebuild=0`**(`CostumeExpansionFW.ini` の `[Diagnostics]`)で
  `DoReset3D` を丸ごと抑止できるようにした(`RebuildPlayerHead` と
  `RestoreMouthIfDropped` の両方)。F2 の A/B を報告者自身が回せる。
  **修正ではない**(persist の SMP 物理が付かなくなる)ので、その旨は
  ini のコメントとログの warn 両方に書いてある。

もう 1 つ、アイテムを一切動かさずに済む分離実験:

- **既存の persist エントリの "Active on this save" を ON/OFF するだけ**でも
  落ちるか。落ちるなら capture(アイテム移動)経路は無関係で、
  head-rebuild / レジストリ側に絞れる(`PersistSetActive` は capture を
  通らずに Reconcile → head rebuild まで行く。`BoxStore.cpp:1632-1675`)。

## 返信について

- **v1.5.0 の告知 + 全文ログの依頼**を先に返すのが妥当(調査は別セッション)。
  「更新で直りました」とは書かないこと(根拠が無い)。
- 報告者は既に v1.2.1 まで戻す・キャッシュ削除・手動掃除まで自力でやっており、
  **手順の出し直しではなく情報の依頼**が筋。
