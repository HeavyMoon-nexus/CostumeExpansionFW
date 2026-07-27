# 新規バグ報告 — persist 追加で CTD ルーレット(2026-07-27 04:50, Nexus posts)

> **状態: 原因確定(2026-07-28)。修正実装済み・in-game 未検証。**
> **→ §確定した原因(クラッシュログ解析)を読むこと。**
> 下の §調査結果(静的監査 1 巡目)の **F1 / F2 は本件の原因ではなかった**。
> 消さずに残してあるが、**先に確定結果を読むこと**。
> UX 別件(スクロール)も実装済み — §4 → `src/SmfUI.cpp`(X-SCROLL)。
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

## 確定した原因(クラッシュログ解析, 2026-07-28)

報告者から 13 本のクラッシュログ + 経緯メモが届いた。

> **重要(2 巡目で判明・1 巡目の記述を訂正):**
> CEF 起因は **3 本ではなく 6 本**。faulting module が `SkyrimSE.exe` の 3 本も
> **CEF から呼ばれたエンジン関数**の中で落ちている。
> そして原因は「`->parent` が dangling」**ではない**。詳細は
> §真の原因 — CEF ホルダーノードの children 配列が壊れている を読むこと。

### まず 3 本(CEF が faulting module)は同一の命令

| ログ | 版 | faulting |
|---|---|---|
| `First Relevant crash-2026-07-25-19-01-20` | v1.3.1 | `CostumeExpansionFW.dll+0x0BA955` |
| `crash-2026-07-27-12-14-29` | v1.5.0 | `CostumeExpansionFW.dll+0x0C1AD5` |
| `Most Recent crash-2026-07-27-12-46-18` | v1.5.0 | `CostumeExpansionFW.dll+0x0C1AD5` |

いずれも `EXCEPTION_ACCESS_VIOLATION` / `mov r8, [r8+rax*1]`、
`RAX = 0xCCCCCCCCCCC3C033`、`R8 = 0x1C0`、
**`RDI = SkyrimSE.exe+0x1AF7A0`(= `xor eax,eax` … EXE の CODE セクション)**。

### 逆アセンブルによる同定(手順は再現可能)

`dist/` の 1.5.0 / 1.3.1 の**出荷 DLL そのもの**を `dumpbin /disasm` にかけ、
RVA を直接引いた(PDB は出荷していないので再ビルドでは合わない。実際
v1.5.0 タグを再ビルドしても 2,611,200 vs 出荷 2,609,664 でサイズが違う)。

faulting 関数は CommonLibSSE-NG の **vfunc ディスパッチ thunk**:

```
mov  ecx,1C8h
mov  r8d,1C0h
cmp  byte ptr [rax+118h],4     ; REL::Module::Runtime == VR(4) ?
cmove r8d,ecx                  ; vfunc byte offset = VR ? 0x1C8 : 0x1C0
mov  rax,qword ptr [rdi]       ; rax = this->vtable      <- this = RDI
mov  r8,qword ptr [r8+rax]     ; <<< FAULT
jmp  r8
```

呼び出し元(frame[1])は 3 本とも同じ形:

```
call Get3D(fp != 0)                     ; ループ変数 ebx を 0,1 で回す
GetObjectByName("CEF_RealBody")         ; .rdata の文字列を実際に確認済み
test rdi,rdi / je next
mov  rcx,[rdi+30h]                      ; NiAVObject::parent
test rcx,rcx / je next
mov  rdx,rdi / call <vfunc thunk>       ; parent->DetachChild(node)
next: inc ebx / cmp ebx,1 / jle loop
```

= **`DetachRealBody()`**(`src/SkinRebind.cpp`)そのもの。frame[2] は
`Reconcile()`(`DetachRealBody` はその末尾にインライン化されている)、
frame[3] は `skse64…dll` の **タスクポンプ** = メインスレッド。

```cpp
if (auto* n = root->GetObjectByName(kRealBodyNode)) {
    if (auto* p = n->parent) {   // ← p が解放済み
        p->DetachChild(n);       // ← p の vtable を読んで死ぬ
    }
}
```

### 真の原因 — CEF ホルダーノードの children 配列が壊れている

**1 巡目の結論(「`->parent` が dangling。FSMP が親を解放した」)は誤りだった。**
`->parent` を辿るのが危険なのは事実だが、それは**症状であって原因ではない**。

決め手は、faulting module が `SkyrimSE.exe` の 3 本のスタックだった:

```
[0] SkyrimSE.exe+0x0D1D9D7  -> 70299+0x37   mov rcx,[rax+rbx*8]
[1] SkyrimSE.exe+0x0D1D9EC  -> 70299+0x4C
[2] CostumeExpansionFW.dll+0x00412AF   ← CEF から呼ばれている
[3] CostumeExpansionFW.dll+0x005ED35
[4] skse64_1_6_1170.dll+0x00189DF      ← タスクポンプ(メインスレッド)
```

`SkyrimSE.exe+0xD1D9A0` は **`NiNode::GetObjectByName`**(NiNode vtable の
`+0x150` スロット。EXE の RTTI から確認)。落ちているのはその **+0x37**、
`mov rcx,[rax+rbx*8]` = **children 配列のインデックス**。レジスタ:

| | 値 | 意味 |
|---|---|---|
| `RAX` | **`0x1`** | `children._data`(配列の実体ポインタ) |
| `RBX` | `0x0` | インデックス |
| `RCX`/`RDI` | `(NiNode*) "CostumeFW_0017E9_Clothing_Loot2_esp"` | **CEF が注入したホルダー** |
| `RDX` | `(char*) "CEF_RealBody"` | 探索中の名前 |

つまり **CEF のホルダーノードの `children._data` が `0x1`**、しかも
`size >= 1`。**実体の無い配列を size だけ持っている状態**。3 本とも同じ。

そして CEF が faulting module の 3 本は、**同じ壊れ方のもう一つの顔**:

- `GetObjectByName` はこの配列を踏み外し、**`*(node + 0x110)` を返した**。
  `+0x110` は `NiNode::children` のオフセット(CommonLibSSE
  `RelocateMember(this, 0x110, 0x138)`)なので、返ったのは
  **children 配列自身の vtable ポインタ**。
- EXE の `.rdata` を実測して裏取り済み: 返り値 `0x19AB140` は
  `NiNode` の vtable(`0x19AB150`)の **0x10 手前** =
  `NiTObjectArray<NiPointer<NiAVObject>>` の vtable。RTTI の
  complete object locator まで一致。
- `DetachRealBody` はそれを node として `->parent`(`+0x30`)を読み、
  **NiNode の vtable の中**(`+0x20` スロット)を親ポインタとして
  `DetachChild` を呼んで死んだ。

**したがって:**

- **ダングリングポインタではない。** 値は**決定論的**で、
  ゲームセッション 2 回・EXE ベースアドレス 2 種・CEF ビルド 2 種を
  またいで完全に同一。ランダムな解放後メモリならこうはならない。
- **スレッド競合(F1)でもない。** 落ちるのは全部メインスレッドのタスク内。
- **head rebuild(F2)でもない。** 報告者が「"Costume persist physics
  updated" の通知は出ない」と明言している。

報告者の証言も全部これで揃う。「MCM でも SMF でも」「v1.2.1 まで戻しても」
「2 個目で」「Active の ON/OFF だけでも」→ **どれも `Reconcile()` を通り、
その末尾の `DetachRealBody` が全ノードを走査する**から。

### なぜ配列が壊れるのか — 最有力の容疑者

壊れているのは **CEF 自身が作ったノード**なので、作り方を疑うのが筋。

```cpp
RE::NiNode* holder = RE::NiNode::Create(0);   // ← 容量 0 で作っていた
...
holder->AttachChild(g.get(), true);           // 以降エンジンに伸長させる
```

`NiNode::Create(std::uint16_t a_arrBufLen)` は
`malloc` → `memset(0)` → **ゲームの ctor** を呼ぶ。`NiTArray` の
コンストラクタは `_capacity > 0` のときだけ `_data` を確保する
(CommonLibSSE `NiTArray.h`)。つまり `Create(0)` は
**`_data = null` / `_capacity = 0` の配列**を作り、最初の `AttachChild` で
エンジンの伸長パスに入る。観測された「`size` はあるのに `_data` が使えない」
状態は、**その伸長パスが確保に失敗した(あるいは伸長幅 0 で回らなかった)
場合にちょうど残る形**。

`InjectOnRoot` では `holder` を作る時点で `geoms.size()` が確定しているので、
**最初から実容量で作れば伸長パスを一度も通らない**。そう変更した。

> ⚠ これは**状況証拠**であり、`Create(0)` が壊す決定的証明ではない。
> 反証されうるし、その場合ガード(下)のログが次の報告で答えを出す。

### 修正(実装済み・in-game 未検証)

| 変更 | 内容 |
|---|---|
| `InjectOnRoot` | `NiNode::Create(0)` → **`Create(geoms.size())`**。容量 0 の伸長パスを踏まない(上記の容疑者への対処) |
| `ChildrenWalkable()` | **新規ガード**。`size > capacity`、または `size > 0` なのに `data` が `0x10000` 未満(観測値 `0x1`・null)なら「歩けない」と判定 |
| `DetachMatchingFrom` | 走査前にガード。引っかかったら **ノード名・size・capacity・data をログに出して skip**。CTD がログ 1 行になる |
| `HasDeadPhysicsBind` / `cef headdiag` | 同じガードを適用(どちらも children を手で走査する) |
| `DetachRealBody` / `DetachNodes` / `DetachAllInjected` / `cef headdiag` | `->parent` 参照を全廃し、**降りてきた親**経由で detach(1 巡目の修正。原因ではなかったが、壊れた木を触ったときに死に方が悪くなるのは事実なので維持) |

残る `->parent` 参照は `RebindGeometry` の祖先探索と `InjectOnRoot` の
geometry 引き剥がしの 2 箇所だけで、**どちらも読み込んだ直後の private clone**
(live tree ではない)なので対象外。

### まだ分かっていないこと / 次の一手

- **`Create(0)` が本当に原因かは未証明。** ガードのログ
  (`scene: node '...' has an unwalkable children array (size=.. cap=.. data=..)`)
  が次の報告で出れば、**どのノードがいつ壊れるか**が一発で分かる。
  出なくなれば `Create` 側で当たり。
- **1 巡目の F1(無同期のクロススレッドアクセス)は依然として実在の欠陥**。
  今回の CTD の原因ではなかったが、別課題として残す。
- 報告者の別症状 **「MHW の角が Hide helmet になる」「最新エントリが消える」**
  は本件とは別。ESL 化 + zEdit マージ環境である点(報告者メモ)から
  ARMA 解決側を疑うべきで、**別件として追う**。

---

## 調査結果(コード監査 1 巡目, 2026-07-27)

> ⚠ **後日 (2026-07-28) の追記: F1 も F2 も本件の原因ではなかった。**
> 実際の原因は上の §確定した原因(`DetachRealBody` の `->parent` 参照)。
> 以下は当時の静的監査の記録としてそのまま残す。F1(無同期の
> クロススレッドアクセス)は**欠陥としては実在する**ので別途対応する価値は
> あるが、報告された CTD の説明ではない。F3 も同様に残課題。
>
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
