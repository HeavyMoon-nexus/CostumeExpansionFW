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

### なぜ配列が壊れるのか — 容疑者 1: `Create(0)`(**実測で否認**)

> **結論を先に: 2026-07-28 の in-game 実測(`cef arraytest`)で否認された。**
> 以下は経緯として残す。

**実測結果**(オーナー環境, AE 1.6.1170):

```
Create(0) -> size=0 cap=0 data=0x0
  +child0 -> size=1 cap=1 data=0x27d9085e5a8 walkable=yes
  +child1 -> size=2 cap=2 data=0x27d0187f1c8 walkable=yes
  +child2 -> size=3 cap=3 data=0x27d0187e6a8 walkable=yes
  +child3 -> size=4 cap=4 data=0x27d9438c738 walkable=yes
```

`Create(0)` + `AttachChild` は**毎回正しく伸長する**。壊れた状態は作られない。
→ **この仮説は死んだ。**

ただし同じ実測から、`Create(0)` は **attach のたびに再確保+コピー**している
ことも分かった(capacity が size にぴったり追従している)。20 シェイプの
コスチュームなら 20 回の再確保。`Create(geoms.size())` への変更は
**効率改善としては妥当**なので残すが、**修正ではない**。

### なぜ配列が壊れるのか — 容疑者(以下は当時の推論。上記で否認済み)

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

### ローカル再現の試み(2026-07-28, オーナー環境)

`cef arraytest` → `cef nodediag` → 報告者手順、の 3 段で実施。

| 測定 | 結果 |
|---|---|
| `cef arraytest` | 全行 `walkable=yes` → **`Create(0)` 仮説を否認** |
| persist 追加 3 回(うち 2 回は同一プラグインの 2 アイテムを 1.3 秒間隔) | 全ステージ通過、CTD なし |
| persist off → head part 解除 → `DoReset3D` | 正常完了(F2 チェーンも通した) |
| ガードのヒット (`unwalkable`) | **0 件** |
| `[error]` 行 | **0 件** |
| active items | 32 |

**→ ローカルでは再現しなかった。**

⚠ **これは「修正が効いた」証拠にはならない。** オーナー環境ではそもそも
一度も再現していない(数週間のテストで一度も出ていない)。非再現は
**期待どおりの結果**であって、修正の有効性については何も語らない。
言えるのは **リグレッションが無いこと**だけ。

**`cef nodediag` 2 回目(07:24:36, 実データあり):**

```
[3p] CostumeFW_000EC1__Caenarvon__Cosplay_Basics_esp   size=2 cap=2 data=0x27d018c9288
[3p] CostumeFW_000800_Aves_Dibella_Jewelry_esp         size=3 cap=3 data=0x27d018c8cc8
[3p] CostumeFW_000875__Witchy__The_Horniest_Mod_Ever_esp size=1 cap=1 data=0x27d9085df68
...
-- 10 CEF node(s), 0 unwalkable array(s)
```

**10 ノード全部健全**(全て `size == cap`、有効なヒープポインタ)。3p / 1p に
5 コンテンツずつ。走査器が実データで動くことも確認。**健全時のベースライン**として
今後の比較基準になる(壊れた側は `size` はあるのに `data=0x1` だった)。

### FSMP 4.0.1 での再走(2026-07-28 07:41-07:52)— 陰性、ただしカバレッジ不足

**環境の妥当性は確認済み**(MO2 プロファイル `test CEF bug`):

- `+Faster HDT-SMP 4.0.1`(優先度 621)が `+Faster HDT-SMP 3.5.0`(622)より**上**
  → 実際にロードされたのは **4.0.1**(dll 実測 `ver=4.0.1.0`)
- `-CostumeExpansionFW test 1.3.2` **無効**(既知の汚染源を回避)
- `-SMP-Fixes-0.0.3` **無効**

**やったこと / 結果:**

| | |
|---|---|
| persist 追加 **14 回を約 30 秒**(catalog 7 → 20、全て同一プラグイン) | 全ステージ通過 |
| persist on/off トグル | 正常 |
| `cef nodediag`(最終) | **30 ノード / unwalkable 0** — 全て `size == cap` |
| `cef arraytest` | 3.5.0 と同一(全 `walkable=yes`) |
| ガードヒット / `[error]` | **0 / 0** |
| CTD | **なし** |

**しかし、この回は肝心の経路を一度も通っていない:**

| 経路 | 証拠 | 状態 |
|---|---|---|
| FSMP 物理ノードへのバインド | `bound N bone(s) to FSMP` = **0 行** | **未実行** |
| 祖先リマップ(カスタムボーン有り) | `remapped N unresolved bone(s)` = **0 行** | **未実行** |
| skee body morph | `bodymorph gate` **120 回すべて skip**(apply 0) | **未実行** |
| head-carrier 再構築 | `persist head: DoReset3D` **0 行** | **未実行** |

使ったコンテンツ(Witchy)は**カスタム SMP ボーンを持たない静的アセット**で、
全ボーンが生スケルトンで解決している。つまり **FSMP 依存コードは一行も走っていない**。
FSMP 4.0.1 仮説の検証としては、**ほぼ何も試していないに等しい**。
(参考: 直前の 3.5.0 セッションでは `remapped ...` 警告が 45 行出ていた =
そちらはリマップ経路を通っていた。)

→ **陰性結果だが、仮説を弱める材料にはならない。**

### 次のテスト設計(4.0.1 のまま)

1. **カスタム SMP ボーンを持つコンテンツ**を使う(直前セッションで
   `remapped ...` を出していた Caenarvon / COCO / ELLE / Aves 系)
2. **box トークンを装備し直す** — carrier override は "re-equip to apply" なので、
   装備しないと FSMP はキャリアのボーンをマージしない
3. **`cef headdiag` を打つ。これが最優先。**
   4.0.x で `hdtSSEPhysics_AutoRename_(Armor|Head)_<8hex>` の命名が変わっていれば、
   `ParseRenamedBone` が全て失敗し、CEF の物理バインドは**黙って静的に劣化**する。
   headdiag が 0 本を返したら、それ自体が 4.0.1 互換性の欠陥として確定する
4. **Body morph を数件 ON** にして skee 経路を通す(現在 apply 0 件)
5. persist を **有効化して head-carrier を登録** → `DoReset3D` を走らせる
6. 各ステップ直後に **`cef nodediag`**

### `cef headdiag` の結果 — FSMP 4.0.1 は無罪、別の欠陥が出た(2026-07-28)

**FSMP 4.0.1 互換性は問題なし:**

- 命名は `hdtSSEPhysics_AutoRename_(Armor|Head)_<8hex>` のまま →
  `ParseRenamedBone` は正常に動く。**懸念していた命名変更は起きていない**
- CEF 自身のキャリアも実際にマージされている:
  `bound 56 bone(s) to FSMP physics-driven node(s)
   (e.g. GLDressH_A 1->hdtSSEPhysics_AutoRename_Head_00000008 C5A73E8E3_GLDressH_A 1)`
  ← nifcarrier の名前空間プレフィックス付き = **persist head-carrier 経路は生きている**
- headdiag 3p: `8 Armor + 94 Head physics bone(s), 2 merge group(s)`

**では「remap されたまま戻らない」のは何か — `CEF_sync.log` に答えがあった:**

```
[sync] box46: content '…Gala_DressMAscot_1.nif' skipped for the carrier
       - no inline HDT xml (not SMP, or defaultBBPs-driven which is not detected)
[sync] box38: all 5 declared content(s) unresolved/excluded - keeping previous carrier
[sync] box34: all 7 declared content(s) unresolved/excluded - keeping previous carrier
[sync] box58: all 4 declared content(s) unresolved/excluded - keeping previous carrier
[sync] box59: all 1 declared content(s) unresolved/excluded - keeping previous carrier
[persist] 5 SMP content(s)
```

**21 コンテンツがキャリア構築時に除外されている**(inline HDT xml が無い =
非 SMP、あるいは defaultBBPs 駆動で検出できない)。除外されたコンテンツの
ボーンはキャリアに入っていないので、**原理的に永久にバインドできない**。

### 派生して見つかった欠陥 2 件(CTD とは別。ただし環境的に関係しうる)

**D1: 永久に成功しないリバインドを無限に再試行していた。**

`RequestRebindRetry` は静的フォールバックのたびに再試行を積み、
`Reconcile()` は外部起因のたびに `g_rebindRetryBudget` を**再武装**する。
除外済みコンテンツは毎回必ず静的に落ちるので、**ループが終わらない**。
実測: **35 ラウンド / 2.5 分、対象は 7 件まで増加、ログ末尾でもまだ継続中**。
1 ラウンドごとに detach + NIF ロード + clone + リバインド + 再アタッチ。
つまり **holder ノードを毎秒作っては壊し続けていた**。

→ 修正: `g_staticDiagReported` に入っている(= 既に「恒久的に静的」と
診断済みの)id は再試行を積まない。このセットは物理バインドに成功した
瞬間に `InjectInternal` が消すので、**キャリア再生成や 3D 再構築があれば
自動的に再武装される**。望みのない case だけを parking する。

> ⚠ **これが CTD の原因だとは主張しない。** ただし報告者の環境
> (persist 15-20 件、多くが非 SMP)では同じループが常時回っていたはずで、
> **FSMP が head merge 世代を作り直している最中(本ログでも
> `Head_00000002 → 5 → 8` と進んでいる)に、毎秒ノードを作り壊す**という
> 状態は、children 配列を中途半端な瞬間に観測する条件そのものではある。

**D2: 実行時の診断メッセージが嘘をついていた。**

`carrier diagnostic ... 0 of N bound` は「**ファイルが違う。再装備しろ**」と
案内していたが、実際の最頻原因は「**そのコンテンツはキャリア構築時に
除外されている**」で、再装備では絶対に直らない。
→ 修正: 最初に `CEF_sync.log` の `skipped for the carrier` を見ろ、と明示。

**その他、記録しておくべき sync 側の警告:**

- `[persist] WARNING proxy pool exhausted (8) - collision mesh '…' goes inert` ×2
  → persist の proxy プールは **8 個上限**で、既に使い切っている。
  persist を増やすほど衝突が黙って無効化される。**要スケール対策(別件)**
- `[persist] WARNING collision mesh '…' has no skinned shape in the merge` ×6

### まだ分かっていないこと / 次の一手

- **配列が壊れる原因は未特定**(`Create(0)` は否認された)。ガードのログ
  (`scene: node '...' has an unwalkable children array (size=.. cap=.. data=..)`)
  が報告者の環境で出れば、**どのノードがいつ壊れるか**が一発で分かる。
### 版の事実確認 — 変わったのは FSMP だけ(2026-07-28, オーナー調べ)

報告者は「FSMP / SkyUI /(おそらく)RaceMenu が更新された」と書いていたが、
**実際に更新されたのは FSMP だけ**だった:

| | 事実 |
|---|---|
| RaceMenu | 最終更新 **今年 4/20** — 報告者の 2 週間の窓の外 |
| SkyUI / SKSE | 2 週間以内の更新なし |
| **FSMP** | **3.5.0 が 7/4、4.0.1 が 7/6 公開** — 窓のど真ん中 |
| 報告者の FSMP | **4.0.1**(本人のメモに明記) |
| CEF が想定する FSMP | **3.5.0**(README「3.5.0 tested」)。**4.0.1 との対応は取っていない** |
| FSMP 4.0.0 系 | バグ・クラッシュ報告が相次いでいる(オーナー観測) |

→ **一つ前の「skee(RaceMenu)が次の容疑者」は前提が誤りだったので取り下げる。**
CEF がホルダーを外部に渡すのは `BodyMorph::ApplyToNode` だけ、という構造の話は
事実だが、**RaceMenu は当該期間に変わっていない**ので時期が合わない。

### ローカル再現が失敗した理由が判明した

ローカル MO2 に入っている FSMP は **3.5.0 が最新**(4.0.x は一つも無い)。
つまり第 3 段の再現試行は、**唯一変わった変数を欠いたまま**回していた。
非再現は当然で、これで「修正が効いた」とはますます言えない。

**次の実験は明確:**

1. ローカルに **FSMP 4.0.1** を入れる(3.5.0 と切り替えられるようにする)
2. 同じ手順(同一プラグインの同スロット 2 アイテム、装備中+インベントリ)を再走
3. 各操作の直後に `cef nodediag` — 壊れた瞬間を CTD 前に捕まえる
4. 3.5.0 に戻して同じ手順 → **差が出れば FSMP 4.0.x が原因側で確定**

`fsmp_patches/` に v4.0.0 のソースツリーから起こした patch があるので、
4.0.x が children 配列に何をしているかはソースで追える。

補足: 報告者は `SMP Fixes 0.0.3`(FSMP 3.0.0-beta 向け)を 4.0.1 と併用しており、
13 本中 3 本は `SMPFixes.dll` 内で落ちている。ただし**本人が「最新のクラッシュの
前に削除した」と明記**しており、削除後の 7/27 の 2 本も落ちているので、
SMP Fixes 単独が原因ではない。**ローカルにも同じ mod が入っている**ので、
再現テストでは有効/無効をはっきりさせること。
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
