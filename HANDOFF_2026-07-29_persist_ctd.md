# 引継ぎ — persist CTD 対応セッション(2026-07-27 〜 07-29)

> このファイルだけで次セッションが再開できるように書いてある。
> **深い技術記録は [BUGREPORT_2026-07-27_persist_ctd.md](BUGREPORT_2026-07-27_persist_ctd.md)**
> (全経緯・逆アセンブル結果・否認した仮説)。プロジェクト全体の
> コールドスタートは [HANDOFF.md](HANDOFF.md)。

---

## 0. 一行で

Nexus 報告「persist にアーマーを追加するたびに CTD」を追い、**クラッシュ地点を特定して
走査全廃の設計に作り替えた**。ローカル検証は緑。**報告者環境では未検証**で、
テストビルド `1.5.1-test.2`(DLL-only)を送付して**返答待ち**。

---

## 1. いま何がどうなっているか

| | 状態 |
|---|---|
| ブランチ | `main`、作業ツリーはクリーン |
| 最終コミット | `6bf212c` diag: log the attachment census automatically |
| バージョン | CMakeLists `1.5.1`(未リリース。CHANGELOG に Unreleased 節あり) |
| 報告者へ送付済み | `dist/CostumeExpansionFW-1.5.1-test.2-DLLONLY.7z` + `返信.txt` の本文 |
| 報告者の状態 | 返答待ち。**オーナーは繁忙期のため返信が遅れる旨を伝達済み** |
| ローカル環境 | MO2 プロファイル `test CEF bug`、FSMP 4.0.1、SoftBody 無効、MARA 無効、`CostumeExpansionFW test 1.3.2` 無効 |

**注意: `返信.txt` は削除済み**(未追跡ファイルだった)。本文は Nexus PM 送信済み。

---

## 2. 確定した事実(再調査するな)

### 2-1. クラッシュ地点

`Reconcile()` 末尾の `DetachRealBody`。報告者の 13 本のログのうち **CEF 起因は 6 本**で、
全部この 1 操作。出荷 DLL を `dumpbin /disasm` して特定した(手順は BUGREPORT に記載)。

### 2-2. 設計上の禁止事項 ★最重要★

**アクターのスケルトンを手書きで走査してはならない。**
`NPC Root [Root]` は `BSFlattenedBoneTree` で、EXE の RTTI から確認したところ
vtable `+0x150`(`GetObjectByName`)が **NiNode とは別実装**:

| 型 | GetObjectByName |
|---|---|
| `NiNode` / `BSFadeNode` | `SkyrimSE.exe+0xD1D9A0` |
| **`BSFlattenedBoneTree`** | **`+0xD30380`** |

エンジンはここを children 走査で探さない → **children スロットの中身は無保証**。
現行 CEF は **自分が付けたホルダーと貼り付け先を `NiPointer` で記録**して detach する
(`ActiveItem::holder3p/parent3p/holder1p/parent1p`、real body はファイルスコープ、
`DetachRecorded()`)。**名前検索も走査も `->parent` 参照もしない。**

**`DetachSkinned` の順序は load-bearing**: 記録はレジストリ項目に載っているので
**detach → unregister** の順でなければ孤児化して二重化する(実測 40→42)。
`ClearRegistry` も同様。順序ミスは `DetachNodes ... no registry entry` の warn で出る。

### 2-3. 否認済みの仮説(実測・報告者の証言で否定)

- `NiNode::Create(0)` — `cef arraytest` で健全と実証
- F1 スレッド競合 — 落ちるのはメインスレッド(ただし F1 自体は実在の欠陥で**修正済み**)
- F2 head rebuild — 報告者が「"Costume persist physics updated" の通知は出ない」と明言
- FSMP 4.0.1 の命名変更 — `hdtSSEPhysics_AutoRename_` のまま
- 80 ボーン上限 — 報告者は BLE 導入済み。**80 はシェイプ単位、3450 はアクター合計で別軸**
- MARA — 報告者は未導入(ローカルのみのノイズ)
- **ESL / zEdit マージ** — 報告者が非マージ・作者 ESL・自作 ESL の全部で同様に落ちると報告
- **SoftBody** — 切っても落ちた(ただしキャリア非マージの相互作用は実在)

---

## 3. このセッションで入れた変更(全部 in-game 未検証は明記済み)

| コミット | 内容 |
|---|---|
| `48af5d3` | SMF スクロール領域 + persist カタログフィルタ |
| `1a67a62` | `persist-add[...]` ステージマーカー、`bPersistHeadRebuild` レバー |
| `53ddc55` | `->parent` 参照の全廃(**この時点では走査に置換 = 後で悪化と判明**) |
| `e011f82` | `ChildrenWalkable` ガード、`Create(geoms.size())` |
| `d14b9f9` | `cef arraytest` / `cef nodediag` |
| `564b17a` | D1 望みのない rebind 再試行の parking、D2 誤診断メッセージ修正 |
| `2116ea6` / `8103ed8` | Diagnostics「Physics bones」(BLE 検出、最重量シェイプ/80、要求 vs 実際) |
| `3c7f0de` | **F1 修正** — 全 store 関数に共有再帰ミューテックス(`src/StoreLock.h`) |
| `5541a77` | addon picker がスロットを見るように(「角が Hide helmet」対策・未検証) |
| `1486191` | **走査全廃・記録方式**(本命の修正) |
| `89e1364` | **detach → unregister の順序修正**(二重化) |
| `6bf212c` | 自動 census ログ(コンソール不要化) |

---

## 4. 返答が来たら見るもの(優先順)

1. **`attached: N content(s) registered, N on 3p, N on 1p, real body ...`**
   変化時のみ出る自動点呼。CEF が何を付けていたかが分かる
2. **`persist-add[catalog|register|reconcile|ability|manifest|done]`** — どこで切れたか
3. **`unwalkable children array`** — 出れば一次破損が再発(未特定のまま)
4. **キャリア masking が外れた後の物理** — 良くなっているはず。隠れていた症状が出る可能性も
5. crash log の faulting module と RSI(シンボル名が載ることがある)

---

## 5. 未解決 / 次にやること

| 優先 | 項目 |
|---|---|
| 高 | **一次破損の原因**(CEF ホルダーの `children._data = 0x1`)。現行コードは触れないので落ちないはずだが**直った証拠は無い** |
| 高 | 報告者環境での検証(返答待ち) |
| 中 | **BACKLOG 5-2** キャリアのスケール設計。CEF 単体で 1 アクター約 3450 ボーン要求 → 他 SMP mod と同居すると**丸ごとマージされない**(実測: SoftBody 有効で CEF 分 0 / 無効で 3453)。`proxy pool exhausted (8)` も同根 |
| 中 | 「最新エントリが消える」— 未着手 |
| 低 | v1.5.1 リリース(CHANGELOG は書けている。報告者の検証後が望ましい) |

---

## 6. このセッションで自分がやらかしたこと(繰り返さないため)

1. **engine 実装を手書きに置き換えて悪化させた**。走査に変えた結果 New Game まで落ちるようになった。
   → **型ディスパッチを疑え。** vtable を実測してから置き換える
2. **`DetachSkinned` の順序ミス**で二重化。ローカルテストで捕捉できたのは幸運
3. **テストビルドに full package を渡した** → 234B プレースホルダが実キャリアを masking し、
   報告者の「物理が歪んで止まる」を誘発。**テストは DLL のみ**
4. **実行不可能な依頼**(「落ちた直後にコンソールで nodediag」)。メニュー中はコンソールを
   開けず、CTD には反応できない → 自動 census ログで解決
5. **仮説を 3 回外した**(F1 → F2 → SoftBody)。いずれも「コードから証明できる」と
   言えるものと、そうでないものを混ぜて語ったのが原因。**測定してから言え**

---

## 7. 便利なもの

- `cef nodediag` — CEF ノードの点呼(走査するのでユーザー起動のみ)
- `cef arraytest` — mod 不要の合成テスト。`NiNode::Create` の健全性を 30 秒で確認
- `cef headdiag` — FSMP マージボーンの列挙。グループ名と `C<8hex>_` プレフィックスで
  CEF 分と他 mod 分を切り分けられる
- `bPersistHeadRebuild=0`(ini `[Diagnostics]`)— head rebuild の抑止。切り分け用
- ローカルに **SkyrimSE.exe 1.6.1170 の同一バイナリ**あり
  (`K:\SteamLibrary\...`、MD5 が報告者と一致)。RTTI から vtable を引ける
- 出荷 DLL は `dist/*.7z` に全部ある。`dumpbin /disasm` でオフセットを引ける
  (PDB は出荷していないので再ビルドでは合わない)
