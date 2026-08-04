# CEF登録アイテムへの透明化エフェクト伝播 調査報告

## 決着 2026-08-04: 不具合は再現せず — 伝播は既に機能している（実測で確定・CLOSED）

`cef invisdiag`（56a6e68 + fadeNode/effectData 拡張）による 2 巡の実測と視覚確認の結果:

1. **第1巡（4段階）**: 透明化の全段階で propAlpha / matAlpha / kRefraction / flags は
   通常装備・CEF holder とも完全に不変 → バニラ透明化は per-geometry の
   シェーダプロパティを書き換えない。本書 §4 が想定した方式②（手動 alpha 同期）は
   複製対象が存在せず原理的に不適。
2. **第2巡（完全透明時, invisibilityAV=1.00）**: エンジンの実チャネルは
   `BSShaderProperty` の **kTempRefraction フラグ + effectData リンク**
   （InvisFXShader EFSH 0002DF92 の ShaderReferenceEffect 由来）で、
   **CEF 注入ジオメトリにも通常装備と同一ポインタ（0x24d525c4f40）で張られていた**。
   解除後は両者とも tmpRefr=0・flags 復元で対称。オーナー視覚確認でも
   CEF アイテムは体と同様に透明化。
3. 機構: effect は actor ルート配下の全ジオメトリへ per-geometry リンクを適用する。
   CEF holder は同じルート配下に attach されるため自動的に包含される —
   §2 の「自動的に参加していないと考えられる」という推定が誤りだった。

**残余の未検証ケース1件（低優先・30秒で確認可能）**: 「完全透明の最中に」box token を
装備 / persist を新規表示した場合に、後から attach されたジオメトリへリンクが張られるか。
再現した場合のみ、§4 の設計（記録済みジオメトリへの effectData 同期）を最小実装する。
実装するときの答えは本決着メモに揃っている: 対象 = ActiveItem の記録済みジオメトリ、
チャネル = `SetEffectShaderData` + kTempRefraction、参照元 =
`ProcessLists::ForEachShaderEffect` で target==player の effect。

backlog 6-1 は CLOSED。`cef invisdiag` は診断コマンドとして恒久保持。

---

（以下は 2026-08-02 時点の調査原文。§2 の結論は上記の実測により覆っている）

- 調査日: 2026-08-02
- 対象: Costume Expansion Framework（CEF）
- 対象ソース: `K:\dev\CostumeExpansionFW`
- 対象MO2環境: `K:\Mo2_SkyrimSE1170`
- 対象ランタイム: Skyrim SE/AE 1.6.1170
- 調査範囲: CEFソース、導入済みDLL、`CostumeFW.esp`
- 実装状態: **調査のみ。コード・ESP・アセットは変更していない**

## 1. 報告された現象

CEFへ登録したアイテムを表示・装備している状態で、プレイヤーが自身へ透明化魔法を使用しても、CEF登録アイテムへ透明化表現が伝播しない。

対象となる表示方式は次の両方である。

- Box登録アイテム
- Persist登録アイテム

## 2. 結論

修正先は `CostumeFW.esp` ではなく、CEFのSKSE DLL側である。

CEFが表示する衣装は、通常のARMO/ARMA装備としてゲームエンジンに描画されているのではなく、DLLが実行時にNIFを複製してプレイヤーのscene graphへ注入したNiNodeである。このため、通常装備へ行われる透明化時のfade、effect shader、refractionなどの処理へ自動的に参加していないと考えられる。

BoxとPersistの違いは表示判定だけであり、表示時はどちらも次の共通経路を通る。

```text
Reconcile
  -> InjectInternal
       -> InjectOnRoot (3P)
       -> InjectOnRoot (1P)
```

したがって、Box用とPersist用の個別修正は不要である。共通の注入・可視状態同期処理を修正すれば両方へ適用できる。

## 3. 導入物とソースの対応確認

調査時点で、MO2に導入されている `CostumeExpansionFW.dll` と `K:\dev\CostumeExpansionFW` のビルドDLLは、SHA-256が一致した。

```text
F32151E311B189A5019637DDD17FB7859BCD5212ECB88E9B8B9B81AC7810DD45
```

そのため、本報告で確認したソースは実際の導入DLLと対応している。

ソース側の状態は次のとおり。

- ブランチ: `main`
- 最新コミット: `d74321e docs: v1.5.2 published + PM sent - standing-watch phase`
- バージョン: 1.5.2
- 調査時のworktree: clean

## 4. 変更が必要なソース箇所

### 4.1 `ActiveItem`へ可視状態管理情報を追加

対象:

- `K:\dev\CostumeExpansionFW\src\SkinRebind.cpp`
- `ActiveItem`（おおむね78～99行）

現在の `ActiveItem` は、3P/1Pモデル、トークン情報、性別、注入したholderとparentを保持している。一方、透明化の同期に必要となる次の情報は保持していない。

- 同期対象となる可視ジオメトリ
- 各ジオメトリの元のshader/material alpha
- fade/effect contextの関連情報
- refractionの状態
- 前回同期したActor側の可視状態

実装時には、3P/1Pそれぞれについて安全に同期対象を参照できる構造を追加する必要がある。

手動でmaterial alphaを変更する方式を採る場合は、各形状の元のalphaを保存し、Actorの透明度を相対的に適用して、解除時に正確に復元しなければならない。

### 4.2 `InjectOnRoot`で同期対象を記録し、初期状態を反映

対象:

- `K:\dev\CostumeExpansionFW\src\SkinRebind.cpp`
- `InjectOnRoot`（おおむね1106～1300行）

現在は次の処理を行っている。

1. 注入先rootを取得する。
2. NIFをロードしてcloneする。
3. skinned geometryを収集・rebindする。
4. 新しいholderを作成する。
5. holderをActorへ接続する。
6. Alternate Textureを適用する。
7. Body Morphを適用する。
8. holderのbaselineを記録する。

ここにはActorのalpha、fade、effect shader、refractionを注入ノードへ反映する処理がない。

必要な変更は次のとおり。

- 注入した可視ジオメトリを記録する。
- Alternate TextureとBody Morphの適用完了後にbaselineを取得する。
- Actorがすでに透明化中であれば、その時点の状態を新しい注入ノードへ初期反映する。

これにより、「CEFアイテムを表示してから透明化」だけでなく、「透明化してからBoxを装備」「透明化中にPersistを表示」というケースにも対応する。

### 4.3 既存holderの早期returnとは独立した同期処理が必要

対象:

- `K:\dev\CostumeExpansionFW\src\SkinRebind.cpp`
- `InjectOnRoot` 内の既存holder判定（おおむね1139～1145行）

記録済みholderが同じrootに存在する場合、`InjectOnRoot` は早期returnする。

そのため、新規注入時のコードへ透明化処理を追加するだけでは、すでに表示されているCEFアイテムへ後から透明化を掛けるケースは修正されない。

透明化の追従処理は、注入処理および `Reconcile()` の再注入判定とは独立して実行する必要がある。

### 4.4 Real Body置換も同期対象に含める

対象:

- `K:\dev\CostumeExpansionFW\src\SkinRebind.cpp`
- Real Body用holder/parent（おおむね1406～1415行）
- `InjectRealBody`（おおむね1661～1671行）

CEFのReal Body置換も、通常装備ではなくDLLが注入するscene nodeである。登録コンテンツがReal Body置換を使用している場合、衣装だけを透明化すると置換bodyが表示されたままになる可能性がある。

Real Body用の3P/1P holderについても、通常の `g_active` と同じ可視状態同期へ含める必要がある。

Real Bodyの初期状態記録と反映は、`ApplySkinTextures` 完了後に行うのが適切である。

### 4.5 記録済み対象だけを同期する関数を追加

対象候補:

- `K:\dev\CostumeExpansionFW\src\SkinRebind.cpp`
- `ContainmentSweepFrame` 周辺（おおむね2257～2272行）

`SyncInjectedVisualState` 相当の処理を追加し、次だけを反復する構成が適切である。

- `g_active` に記録された3P/1Pの可視ジオメトリ
- Real Body用の3P/1P可視ジオメトリ

Actorのrootまたはskeleton全体を毎フレーム探索してはならない。現在進行中のchild-array破損・CTD対策では、記録済みholderを先に検査する構成が採られているため、その安全性を崩さないことが重要である。

同期処理は次の性質を持たせる。

- holderの健全性確認後にのみ対象へアクセスする。
- Actor側の状態が変化した場合だけmaterial/shaderを更新する。
- 毎フレームのscene-tree検索を行わない。
- 毎フレームのログ出力を行わない。
- Box/Persistを同じ処理で扱う。

### 4.6 `PlayerUpdateHook`でActor更新後に同期する

対象:

- `K:\dev\CostumeExpansionFW\plugin.cpp`
- `PlayerUpdateHook::thunk`（おおむね65～80行）

現在のhookは、originalのPlayer Updateより前に `ContainmentSweepFrame()` を呼んでいる。

推奨する実行順は次のとおり。

```text
ContainmentSweepFrame()
original Player Update
SyncInjectedVisualState()
```

Actor側のfadeや透明化状態はPlayer Update中に進行する可能性がある。同期処理をoriginal Update後に置くことで、そのフレームの新しい状態を読み取り、1フレームの遅延を避けられる。

独立したpublic関数として呼ぶ場合は、次にも宣言を追加する。

- `K:\dev\CostumeExpansionFW\src\SkinRebind.h`
- `ContainmentSweepFrame` 宣言付近（おおむね129～134行）

### 4.7 detach・quarantine・registry clear時に参照を破棄

新しい可視ジオメトリ参照やbaseline cacheを追加した場合、少なくとも次の経路で確実に破棄する。

- `QuarantineIfBroken`（おおむね330～371行）
- `DetachNodes`（おおむね386～418行）
- Real Body detach処理
- `DetachAllInjected`（おおむね2439～2464行）
- `ClearRegistry`（おおむね3481～3501行）
- 3D再構築およびroot差し替え時

detach済みNiAVObjectへの参照を残すと、dangling pointer、不要なノード保持、または現在対応中のCTD問題へつながる可能性がある。

## 5. ESP調査結果

ロードされている `CostumeFW.esp` はACTIVEで、定義レコードは74件だった。

| レコード種別 | 件数 |
|---|---:|
| ArmorAddon | 31 |
| Armor | 27 |
| HeadPart | 9 |
| Spell | 5 |
| Container | 1 |
| Quest | 1 |

次のレコードは定義されていない。

- MagicEffect
- EffectShader

VMADがあるのは次のQuestのみである。

- `000802:CostumeFW.esp` `CFW_MCMQuest`
- Script: `CostumeFW_MCM`

Boxトークン、carrier ARMA、Persist carrier HeadPartには透明化を制御するVMAD、ObjectEffect、Art Objectなどは存在しない。

### 5.1 CEFのSpellレコード

CEFが定義する5個のSpellは能力値補正用Abilityであり、透明化処理とは無関係である。

| FormID | EditorID/用途 | 参照MGEF |
|---|---|---|
| `000808` | Armor Rating | `017120:Skyrim.esm` |
| `000809` | Health | `0493AA:Skyrim.esm` |
| `00080A` | Magicka | `049504:Skyrim.esm` |
| `00080B` | Stamina | `049507:Skyrim.esm` |
| `00080C` | Carry Weight | `07A0F4:Skyrim.esm` |

すべてAbility / Constant Effect / Selfであり、VMADはない。

### 5.2 BoxとPersistのcarrier

代表的なBoxトークン `CFW_BoxToken_44 [ARMO:000801]` はslot 44を使用し、carrier ARMA `000900` を参照する。このARMAのモデルは次である。

```text
CostumeFW/Box44_carrier.nif
```

Persist用 `CFW_PersistCarrier [HDPT:000909]` は次を参照する。

```text
CostumeFW\boxtoken.nif
```

これらはCEFの注入処理を成立させるcarrierであり、実際に表示される衣装そのものではない。

### 5.3 ESP側で変更しないもの

本件について、次の変更は不要または不適切である。

- `CostumeFW.esp`へのMGEF/EFSH追加
- Box用ARMO/ARMAへの透明化効果追加
- Persist用HDPTへの効果追加
- `CostumeFW_MCM.psc`による透明化監視
- carrier NIFの再生成
- 透明化Magic Effect適用イベントだけを監視する処理

Magic Effectの適用イベントだけでは、fadeの進行、効果終了、解除、スクリプトによるalpha変更を継続的に反映できない。

## 6. 実装前に必要なランタイム診断

バニラの透明化はActorのalphaを変更するだけとは限らない。

代表的なバニラ透明化MGEFは次の構成を持つ。

- `InvisibillityFFSelf [MGEF:0001EA6A]`
- Archetype: Invisibility
- Actor Value: Invisibility
- Hit Shader: `InvisFXShader [EFSH:0002DF92]`
- Hit Effect Art: `InvisFXBody01 [ARTO:000339C8]`
- Keyword: `MagicInvisibility`

したがって、実装方式を決める前に、通常装備のジオメトリとCEF注入ジオメトリを比較する診断が必要である。

透明化開始、fade中、完全透明、解除開始、解除完了の各段階で次を確認する。

- `Actor::GetAlpha()`
- `BSShaderProperty::alpha`
- `BSShaderProperty::fadeNode`
- effect data
- `kTempRefraction`
- `kRefraction`
- materialのrefraction値
- 通常装備とCEF注入形状の差

診断結果により、実装方式は次のいずれかになる。

1. CEFジオメトリを通常装備と同じfade/effect contextへ関連付ける。
2. Actorのalpha/refractionを記録済みCEFジオメトリへ明示的に同期する。
3. fade/effect contextの関連付けと、必要なmaterial状態の同期を併用する。

診断ログを実装する場合は状態遷移時だけ出力し、常時のフレームログは避ける。

## 7. alpha処理上の注意

`K:\dev\CostumeExpansionFW\src\nifcarrier\NifCarrierCore.cpp` のおおむね600～636行では、carrier NIFを不可視にするため、shader alphaを0にし、NiAlphaPropertyを設定している。

そのため、次のような広範囲の処理は危険である。

```cpp
player->Get3D()->UpdateMaterialAlpha(...);
```

Actor root全体へ絶対値のalphaを適用すると、次の問題が発生し得る。

- Box/Persist carrierが可視化される。
- SMP collision proxyが可視化される。
- コンテンツ作者が設定した半透明形状のalphaが失われる。
- 本来alpha 0であるshapeが表示される。

同期範囲は、注入時に明示的に記録した「表示対象ジオメトリ」に限定する必要がある。

手動alpha同期を採用する場合は、絶対値を上書きせず、元のmaterial alphaをbaselineとしてActor側の係数を乗算し、解除時に元の値を復元する。

## 8. VRについて

現在の `PlayerUpdateHook` はSE/AE用vfunc `0xAD` を使用し、VRでは意図的に導入されない。

CommonLib側ではActor UpdateのVRインデックスは `0xAF` である。VRまで正式対応する場合は、VR用hookまたは同等の安全な更新経路が必要になる。

ただし、現在進行中の大きなCTD関連修正へ混ぜず、SE/AEの挙動を確定した後の独立した対応とするのが望ましい。既存の2.5秒watchdogは滑らかなfade同期には遅すぎる。

## 9. 回帰試験項目

### 9.1 基本ケース

- Persist表示中に透明化を開始し、効果終了時に復元される。
- Box装備中に透明化を開始し、効果終了時に復元される。
- 透明化してからPersistを登録・表示する。
- 透明化してからBoxを装備する。
- 透明化中にBoxを装備・解除する。

### 9.2 視点・3D再構築

- 1P/3Pを切り替える。
- セル移動する。
- セーブ・ロードする。
- RaceMenuを開閉する。
- プレイヤーの3Dが再生成される。
- `Load3DHook` 後の再注入時に現在の透明状態が初期反映される。

### 9.3 CEF固有機能

- Real Body置換を有効にする。
- BoxとPersistを同時に使用する。
- 複数の登録アイテムを同時表示する。
- CEFを無効化した状態、および登録アイテムがない状態で余分な処理が走らない。

### 9.4 マテリアル・物理

- 作者設定の半透明shapeが元の透明度を維持する。
- alpha 0のshapeが可視化されない。
- SMP collision proxyが可視化されない。
- Box/Persist carrierが可視化されない。
- Alternate Texture適用後のmaterialが正しく復元される。

### 9.5 透明化の種類

- バニラ透明化魔法
- `AlchInvisibility`を使用する透明化ポーション
- mod追加のInvisibility archetype効果
- `SetAlpha`を使用する別の効果やスクリプト
- refractionを使用する視覚効果
- 通常装備とCEF表示物の見え方が一致すること

### 9.6 安全性

- holder child-arrayの検査より前にジオメトリへ触れない。
- detach/quarantine後の参照へアクセスしない。
- Actor scene graphの毎フレーム探索を行わない。
- 現在対応中のCTDを再発させない。
- フレームごとの不要なmaterial書き換えを行わない。
- Papyrusを周期実行させない。

## 10. 推奨する実装順序

現在の大きなバグへの対応完了後、次の順序で進める。

1. 通常装備とCEF注入ジオメトリのalpha/fade/effect/refractionを比較する診断を追加する。
2. engine側で通常装備へ透明化が伝播する条件を確定する。
3. `ActiveItem` とReal Body管理領域へ、必要最小限の可視対象・baseline情報を追加する。
4. `InjectOnRoot` と `InjectRealBody` で対象記録と初期同期を行う。
5. `PlayerUpdateHook` のoriginal Update後に、記録済み対象だけを同期する。
6. detach/quarantine/clear経路へcache破棄を追加する。
7. SE/AEで回帰試験する。
8. VR対応を別変更として検討する。

## 11. houseCARL関連設定

調査に使用する設定は次のとおり。

```text
MO2 instance:
K:\Mo2_SkyrimSE1170

Papyrus compiler:
K:\SteamLibrary\steamapps\common\Skyrim Special Edition\Papyrus Compiler\PapyrusCompiler.exe

BSArch:
K:\BSArch\BSArch.exe
```

調査時のプロファイルは `Default`、`CostumeFW.esp` はACTIVEだった。

