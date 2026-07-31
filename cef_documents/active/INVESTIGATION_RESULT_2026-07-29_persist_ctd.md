# 調査結果 — CostumeExpansionFW / persist 追加時 CTD

> 調査日: 2026-07-29  
> 入力資料: `INVESTIGATION_BRIEF_2026-07-29_persist_ctd.md`  
> 対象ビルド: v1.5.1-test.2（未リリース）

---

## 結論

1. `NiNode::children._data == 0x1` の**直接原因**は確定しているが、これを書いたモジュール・命令までは現資料から特定できない。次に必要なのは仮説追加ではなく、`holder + 0x118` への data breakpoint による writer の捕捉である。
2. 記録方式の detach は旧 `DetachRealBody` の検索クラッシュを閉じたが、**名前検索、ライブツリーの手書き走査、破損 holder の破棄**という同じクラッシュクラスは現行コードにも残っている。
3. persist キャリアは単一巨大キャリアをやめ、**需要駆動・入場制御・実マージ確認・安定シャーディング**へ移行するべきである。固定の FSMP 上限や item 単位 skip を仮定せず、実際にマージされたかを ACK として扱う。

---

## A. `_data = 0x1` の一次原因

### 仮説

現在の証拠が支持する最も狭い仮説は、次のものである。

> 正常に構築・充填された holder の `_data` が、構築完了後に別の書き込みによって `0x1` へ置換された。

ただし、書き手のモジュールまでは絞れない。

考えられる書き込み形式は以下の 3 種類である。

1. `mov qword ptr [holder+118h], 1` に相当する 8 バイトの定数書き込み
2. `_data` をゼロにする処理と、その後の bool/refcount の `1` 書き込みという二段階破損
3. holder が所有権破損・部分破棄状態に入り、別コードがその内部を他の型として再利用

`holder + 0x110` を `NiRefObject*` と誤認して `IncRefCount()` した、という案は**単独原因としては成立しない**。子を持つ holder の `_data` は有効なヒープアドレスなので、その下位 DWORD を 1 増やしても結果は `0x1` ではなく「元アドレス + 1」になる。この案には、その直前に `_data == 0` へ変える別処理が必要である。

### コードから証明できること

- 現行 `InjectOnRoot` は `geoms.size()` を capacity として `NiNode::Create()` を呼び、子を attach してからライブツリーへ接続する。外部コードへ到達可能になる前には有効な child buffer が存在する。
- 旧 `Create(0)` も `cef arraytest` で正常伸長が確認されている。したがって初期生成だけでは観測状態を説明できない。
- CommonLib の `NiTArray` デストラクタは `_data` を `Deallocate()` するだけであり、`_data` に `1` を書く処理ではない。
- ただし `_data == 1` のまま holder が破棄されると `Deallocate(1)` になり、走査とは別の CTD が発生し得る。
- skee は `bodymorph gate == apply` の場合だけ CEF から直接 holder ポインタを受け取る。
- holder をライブ scene graph に attach した後は、CEF が明示的に渡さなくてもエンジン、FSMP、その他の scene-graph visitor から到達可能になる。
- SoftBody の NIF/XML や ESP は実行コードではないため、直接の書き手にはなれない。エンジンや FSMP の処理経路を変える入力・発火条件にはなり得る。

参照:

- `src/SkinRebind.cpp:874-1045`
- `src/BodyMorph.cpp:54-72`
- `build/release/vcpkg_installed/x64-windows-static-md/include/RE/N/NiTArray.h:32-35`
- `build/release/vcpkg_installed/x64-windows-static-md/include/RE/N/NiRefObject.h`

### 状況証拠にとどまること

- `0x1` がセッションや ASLR をまたいで決定論的であることだけでは、UAF や部分破棄を否定できない。解放処理や allocator が決定論的な値を残す可能性がある。
- faulting thread がメインスレッドだったことは、それ以前に別スレッドが破損を書いた可能性を否定しない。否認済みなのは具体的な UI/main 間の `g_active` 競合であって、すべての非同期書き込みではない。
- Clothing Loot2 由来 holder の反復は、その ESP がメモリを書いた証拠ではない。形状数、extra data、BodyMorph、物理構成などの発火条件である可能性がある。
- RaceMenu が直近で更新されていないことは、RaceMenu 経路を否認しない。以前から存在する欠陥が新しい scene graph 構成で発火することはあり得る。
- FSMP が holder を変更しているという直接証拠はまだ無い。

### 反証する測定

#### 1. 非走査の stage snapshot

holder ごとに次の値を直接読み、1 行ずつフラッシュする。

```text
id / holder / parent / refcount / data / capacity / freeIdx / size / growthSize
```

採取点:

1. `NiNode::Create` 直後
2. 各 `AttachChild` 後
3. ライブ attach 後
4. `Update` 後
5. `ApplyAltTextures` 後
6. `ApplyVertexDiff` の直前・直後
7. `InjectOnRoot` 終端
8. `Reconcile` 終端
9. bind watchdog tick
10. detach の直前

これにより「注入関数内で壊れた」「正常に戻った後、遅れて壊れた」を分離できる。

#### 2. hardware data breakpoint

再発実績のある holder ID を対象に、最後の正常な `AttachChild` 後で次を設定する。

```text
ba w8 <holder+0x118>
```

取得すべき情報:

- writer の RIP と module
- 書き込み命令
- call stack
- thread ID
- 書き込み前後の 8 バイト
- holder の refcount と parent

この 1 回の捕捉が、一次原因を確定する最短経路である。

#### 3. 破棄・所有権監視

対象 holder の refcount、`NiNode` デストラクタ、child buffer の `Deallocate` に breakpoint を置く。親の child array から除かれる前にデストラクタへ入っていれば、所有権または refcount 破損が確定する。

#### 4. 差分試験

以下を別々に試す。

| 条件 | 分離できるもの |
|---|---|
| BodyMorph 呼び出しを強制 OFF | skee の直接 holder 操作 |
| `hdtSMP64.dll` 自体を外す | FSMP コード全体 |
| holder をライブ attach せず、私有状態で BodyMorph だけ呼ぶ | skee 単独 |
| BodyMorph OFF + FSMP OFF でライブ attach | CEF/エンジン側 |
| 専用 sibling branch に attach | `BSFlattenedBoneTree` 内部との相互作用 |

SoftBody OFF だけでは FSMP 自体を除外できない。

#### 5. 意図的破損テストの注意

合成 holder の `_data` を意図的に `0x1` にする場合、通常のスコープ終了で holder を破棄してはいけない。読者を一度も通さなくてもデストラクタの `Deallocate(1)` で落ち、どの経路を検証したのか分からなくなる。

実施するなら次のいずれかが必要である。

- debugger 下の別プロセスで 1 経路ずつ実行
- テスト後に元の `_data` を必ず復元
- 破損 holder を意図的に quarantine して破棄しない

---

## B. 現行修正の妥当性 / 残存する経路

### B-1 の評価

「存在しない `CEF_RealBody` の検索が最も広く scene graph を触るため、旧クラッシュ地点に集中した」という説明は妥当である。

また、以下の帰結も妥当である。

> 破損は CTD 操作の瞬間に作られたとは限らず、以前に作られた破損を全域検索が初めて発見した可能性がある。

ただし、「存在しない検索は必ず全 CEF holder を訪れる」は、`BSFlattenedBoneTree::GetObjectByName` の実装全体を逆アセンブルしていない限り証明ではない。クラッシュログが証明するのは「少なくとも問題の holder までは到達した」である。

### B-2 の評価

B-2 の指摘は正しい。ただし残存範囲は本文の 2 箇所より広い。

#### 1. `FindFsmpRenamedBone`

`src/SkinRebind.cpp:309-345`

- 無ガードのライブツリー全走査
- 最大世代選択のため早期終了なし
- `BSFlattenedBoneTree` に対する generic `GetChildren()` 反復を含む
- 未解決ボーンごとに最大 2 回

#### 2. `HasDeadPhysicsBind` 冒頭の名前検索

`src/SkinRebind.cpp:537-542`

未注入、3D 再構築後、dead-bind detach 後は holder が存在しないため、見つからない検索になる。

#### 3. `HasDeadPhysicsBind` のライブツリー全走査

`src/SkinRebind.cpp:544-578`

holder が見つかった場合も、FSMP 世代集合を作るために毎回ライブツリー全体を手書き走査する。`Reconcile` だけでなく 2.5 秒間隔の bind watchdog からも到達する。

#### 4. `HasDeadPhysicsBind` の holder 再走査

`src/SkinRebind.cpp:581-615`

root 走査中に `ChildrenWalkable(holder) == false` となって holder を skip しても、その直後に `BSVisit::TraverseScenegraphGeometries(holder)` を呼ぶ。観測済みの `_data == 1` を持つ holder をここで再度踏み得る。

#### 5. `RebindGeometry` の名前検索

`src/SkinRebind.cpp:650-705`

- 各ボーンに対する `a_root->GetObjectByName(src->name)`
- 未解決時の FSMP renamed bone 全走査
- 祖先ごとの `a_root->GetObjectByName(anc->name)`

特に未解決ボーンの多い環境では、見つからない検索を大量に発生させる。

#### 6. `ApplySkinTextures`

`src/SkinRebind.cpp:1231-1245`

すでに `g_realBodyHolder3p/1p` を記録しているにもかかわらず、holder を名前検索してから走査している。

#### 7. 診断系

- `ChildArrayScan`
- bone budget 診断
- `cef headdiag`

いずれも user-initiated ではあるが、`BSFlattenedBoneTree` を generic に反復するため安全ではない。診断機能が CTD の発火点になる可能性がある。

### B-3 の評価

`ChildrenWalkable` は観測済みの失敗モード全体を防げない。加えて、実装上の確定バグが 1 件ある。

現在のコード:

```cpp
if (kids.size() == 0) {
    return true;  // nothing to iterate (end() == begin())
}
```

しかし CommonLib の `NiTArray::end()` は次の実装である。

```cpp
return _data + _capacity;
```

つまり range-for は `_size` ではなく `_capacity` 個のスロットを反復する。

そのため:

```text
size=0, capacity>0, data=0x1
```

は現在のガードを通過し、反復時に落ちる。

最低限の metadata 判定順は次のようになる。

```cpp
if (kids.size() > kids.capacity()) {
    return false;
}
if (kids.capacity() == 0) {
    return true;
}
return reinterpret_cast<std::uintptr_t>(kids.begin()) >= 0x10000;
```

ただし、これでも以下は防げない。

- 有効アドレスに見える dangling pointer
- 配列スロット中の不正な `NiAVObject*`
- `BSFlattenedBoneTree` 固有の非 generic なスロット表現
- check 後に別処理が変更する TOCTOU

したがって本質的な対策は guard 強化ではなく、**ライブ actor tree の generic 走査廃止**である。

### 他に残っている地雷

#### 1. 破損 holder のデストラクタ

`DetachRecorded` は holder の children を読まずに親から外せるが、続けて `a_holder.reset()` する。

```cpp
if (a_parent && a_holder) {
    a_parent->DetachChild(a_holder.get());
}
a_parent.reset();
a_holder.reset();
```

最後の参照なら、`_data == 1` の child array が破棄されて `Deallocate(1)` になる。したがって記録方式だけでは既存破損を安全に処理できない。

封じ込め策:

1. detach 前に記録 holder の metadata を直接検査
2. 破損していれば親からだけ外す
3. `NiPointer` をプロセス終了まで quarantine し、デストラクタへ渡さない
4. 致命的診断をログへ出す

破損 object の意図的リークは、ここでは安全性のための妥当な選択である。

#### 2. bound-bone pin の解放順

`src/SkinRebind.cpp:183-199`

現在は:

```cpp
g_boundBoneRefs.erase(a_id);
DetachRecorded(...);
```

skin の `bones[]` は raw pointer なので、pin を先に解放すると、geometry がまだ attach されている短い区間に dead bone が解放され得る。

正しい順序は:

```text
holder detach/破棄
→ bound-bone pin 解放
```

#### 3. 過去の孤児 holder

記録方式は新たな孤児化を防げるが、以前のバグですでに registry から失われた holder を発見できない。「記録に無い holder は存在しない」は、新規 3D またはクリーンな新規セッションの invariant としてのみ成立する。

### 推奨する修正順

1. `HasDeadPhysicsBind` に `ActiveItem::holder3p` を直接渡し、冒頭の名前検索を削除
2. 注入時に geometry と bound-bone の世代情報を記録し、holder の再走査を削除
3. `FindFsmpRenamedBone` の毎ボーン全走査を、一世代一回の FSMP bone index または FSMP API に置換
4. `ApplySkinTextures` に記録済み real-body holder を直接渡す
5. CEF holder を `NPC Root [Root]` 内ではなく専用 sibling branch に付け、ボーン検索範囲と表示 branch を分離
6. 破損 holder の quarantine を実装
7. bound-bone pin の解放を holder detach 後へ移動
8. 診断系を記録ベースまたは engine の型別 API ベースへ変更

---

## C. キャリアのスケール設計

### 推奨する設計変更

単一巨大キャリアから、次の 4 段階方式へ変更する。

```text
需要集合の確定
→ コスト計算と入場制御
→ 安定 shard の投入
→ 実マージ確認後に採用
```

### 1. 需要駆動

キャリアへ含めるのは次をすべて満たす content だけにする。

- 現在表示中
- inline HDT XML または確実に検出可能な SMP 定義を持つ
- static fallback ではなく SMP が必要
- 永久 static と診断されていない

非表示、inactive、非 SMP、永久 static の content を carrier から除く。

### 2. content 単位のコスト manifest

ビルド時に次を記録する。

```text
content ID
unique custom bone 数
constraint 数
collision shape 数
proxy 要求数
最大 chain 長
namespace prefix
```

スロット番号ではなく、実コストを admission の単位にする。

### 3. 優先度付き admission

候補 content を優先度順に少数ずつ carrier へ投入する。

優先度の例:

1. 現在ユーザーが明示的に選択したもの
2. 現在 visible な box content
3. persist content
4. static fallback の見栄えが悪いもの
5. コストの小さいもの

FSMP の固定上限は仮定しない。投入後、grace 期間を置いて `C<8hex>_` の expected/observed を比較する。

### 4. 実マージを ACK として扱う

各 shard について:

```text
expected CEF bones
observed CEF bones
owner merge group
generation
```

を確認する。

- expected と observed が一致: admission 成功
- observed が一部: 部分成功として不足 content を特定
- observed が 0: shard 不採用

不採用 shard は最低優先 content から静的フォールバックし、再構築回数を制限して再試行する。

ユーザー向け診断例:

```text
CEF SMP budget: 812/1094 requested bone(s) admitted
Static fallback:
  0017E9:Clothing Loot2.esp  152 bone(s)
  000DDD:Grievous Rose Multicolor.esp  130 bone(s)
Competing live groups:
  Head_00000005  Steammist
  Armor_00000003 HDTS TailBone
```

### 5. 安定シャーディング

carrier 分割は総ボーン数を減らさない。価値は「容量を増やす」ことではなく、失敗・退役・再構築の単位を小さくすることである。

推奨:

- スロット単位ではなく予測コストによる bin packing
- content ID に基づく安定した shard 割当
- 1 content の追加で全 shard を組み替えない
- shard 数そのものにも上限を設ける
- 失敗した shard だけ分割または退役

注意:

- FSMP が global total 制限なら分割だけでは改善しない
- item 数や merge overhead の制限なら過剰分割は悪化する
- したがって分割方式は controlled experiment 後に確定する

### 6. proxy pool は別予算

`proxy pool exhausted (8)` と FSMP merge 不採用は、どちらも無制限な需要という設計問題ではあるが、同じ資源・機序とは証明されていない。

proxy は別に:

- visible content のみ確保
- LRU/reuse
- 要求数と割当数を表示
- 枯渇時にどの content の collision が inert になったか明示

するべきである。

### FSMP の制限機序を特定する実験

既知のボーン数を持つ合成 carrier A/B/C を作る。

| 試験 | 判定 |
|---|---|
| A 単独のボーン数を二分探索 | per-item または global 閾値 |
| A → B と B → A で順序逆転 | greedy admission / 装備順依存 |
| Armor と Head で同じ試験 | class 別予算 |
| 1 carrier と同総量の 2 carrier | item atomicity / item overhead |
| 他 SMP mod の前後で投入 | 競合時の勝者規則 |
| 1p/3p を個別計測 | actor/root 別予算 |

結果の読み方:

- 勝者が投入順で変わる: greedy admission
- 常に同じ class/slot が勝つ: 固定優先度
- 合計値だけで変わる: global budget
- 単独では通るが 2 item 化で落ちる: item/group 数または overhead
- 巨大 item だけ丸ごと落ちる: item atomic skip 仮説を支持

長期的には FSMP 上流へ次の API を提案する価値がある。

- actor ごとの merged bone 列挙
- merge generation 通知
- item/shard の採否
- 拒否理由
- 使用量または残予算

上流: <https://github.com/DaymareOn/hdtSMP64>

---

## この文書で修正すべき認識

### 1. 「決定論的だから dangling ではない」

証明になっていない。決定論的な部分破棄、allocator metadata、再利用パターンはあり得る。

### 2. 「faulting thread が main だからスレッド競合ではない」

faulting read のスレッドしか示していない。以前の書き込みスレッドは未確定である。

### 3. 「size だけ正しい」

観測されたのは `_size >= 1` だけであり、その値が現実の子数と一致しているとは証明されていない。stale metadata の可能性がある。

### 4. 「ApplyVertexDiff が唯一の外部アクセス」

CEF から holder ポインタを直接渡す唯一の場所ではあるが、ライブツリーへ attach 後の到達経路はそれだけではない。

### 5. 「ChildrenWalkable の size==0 は無反復」

CommonLib の range は capacity 個のスロットを反復するため誤りである。

### 6. 「FSMP は item 単位で残予算を判定して丸ごと skip」

観測と整合する有力仮説だが、まだ確定事実ではない。controlled experiment または FSMP コード追跡が必要である。

### 7. 「proxy pool 枯渇と FSMP merge 不採用は同根」

設計上のスケール問題という共通点はあるが、同一資源・同一機序とは証明されていない。

---

## 優先アクション

### P0 — 次のテスト DLL

1. holder stage snapshot
2. 対象 ID の watchpoint 用アドレスログ
3. `ChildrenWalkable` の capacity ベース修正
4. `HasDeadPhysicsBind` の holder 名前検索削除
5. 破損 holder quarantine
6. bound-bone pin の解放順修正

### P1 — ホットパス安全化

1. `FindFsmpRenamedBone` 全走査の廃止
2. `HasDeadPhysicsBind` ライブツリー全走査の廃止
3. geometry/bound-bone 記録方式
4. `ApplySkinTextures` の記録方式
5. CEF 専用 sibling branch の実験

### P2 — キャリア再設計

1. content cost manifest
2. demand-driven working set
3. expected/observed admission ACK
4. controlled FSMP budget experiment
5. stable sharding と明示的 static fallback

