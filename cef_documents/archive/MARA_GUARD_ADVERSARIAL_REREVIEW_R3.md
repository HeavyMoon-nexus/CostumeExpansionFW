# MARA Guard 敵対的再レビュー r3

> 対象ブランチ: `mara-guard-v1.3.2`
> 対象HEAD: `b171a0d`
> 実装修正コミット: `cac79ca`
> 文書更新コミット: `b171a0d`
> 比較基点: `49f91a4`
> 実施日: 2026-07-23
> 対象文書: [MARA_GUARD_IMPL.md](MARA_GUARD_IMPL.md)
> 前回レポート: [MARA_GUARD_ADVERSARIAL_REREVIEW.md](MARA_GUARD_ADVERSARIAL_REREVIEW.md)

---

## 1. 結論

`MARA_GUARD_IMPL.md` の次の宣言は、現時点では承認できない。

> r3 = 再レビュー全指摘対応済み

今回の変更で、以下は実装上確認できた。

- 公開 `InjectArma()` に admission が追加された
- 公開 Papyrus native から到達可能な `CaptureEnchant()` 自身に admission が追加された
- policy 変更後に active の再評価を試みる経路が追加された
- stats、keyword、ability、manifest、SMF summary に `AdmittedContents()` が配置された
- `policy_tests` が CTest に登録された
- `/W4` の既存 release build は成功状態
- `ctest --test-dir build/release --output-on-failure` は 1/1 成功

しかし、最重要の P1 は閉じていない。

1. 最終選択ARMAの検査より先に、そのARMAの内部を読む経路が残っている
2. carrier manifest は最終ARMA admissionを完全に迂回する
3. `AdmittedContents()` はARMO本体しか判定せず、参照先ARMAの拒否結果と一致しない
4. policy変更時にbox ability cacheが失効せず、blocked content由来の能力が残留する
5. policy更新の途中で、quarantine完了前にmanifest生成が走る

したがって判定は次のとおり。

**r3の「残5件を全て修正」は否認する。P1が残存しており、同類CTDを広く防ぐ目的ではリリース停止を継続すべきである。**

### 重大度サマリ

| 重大度 | 件数 | 概要 |
|---|---:|---|
| P1 / リリース阻害 | 2 | 最終ARMA境界が未閉鎖、quarantineが非原子的かつcache残留 |
| P2 / 保証不足 | 2 | r3実装の自動テスト不在、破損NIF面が無防備 |
| P3 / 品質 | 1 | 差分検査に文書末尾空行 |

---

## 2. 修正確認済み項目

### 2.1 公開 `InjectArma()` のgate

`src/SkinRebind.cpp:2213-2240` では、unchecked primitiveを匿名namespaceへ閉じ、
公開 `InjectArma()` が実際のlocal IDとplugin名からcolon-idを生成して
`IsContentAdmissible()` を通すようになった。

これにより、`cef inject` とself-testがbare label `"test"` を理由に
admissionを素通りする問題は修正されている。

### 2.2 `CaptureEnchant()` の関数境界gate

`src/BoxStore.cpp:2849-2861` で、inventoryへ触る前に
`IsContentAdmissible()` が実行される。

上位UIだけでなく公開native自身が強制するため、前回P2-2は修正確認とする。

### 2.3 CTest登録

`CMakeLists.txt:52-56` に以下が追加された。

```cmake
include(CTest)
add_test(NAME capture_policy COMMAND policy_tests)
```

実行結果:

```text
Test project K:/dev/CostumeExpansionFW/build/release
    Start 1: capture_policy
1/1 Test #1: capture_policy ... Passed

100% tests passed, 0 tests failed out of 1
```

CTest未登録という前回P3-1自体は修正されている。

---

## 3. 残存findings

### P1-1: 最終ARMA admissionは「使用前」になっていない

### 文書主張

`MARA_GUARD_IMPL.md:25` は次を主張している。

- ARMA確定直後、`bipedModels`読取り前にhard+plugin判定
- 判定と使用が同一関数・同一ポインタ
- check/use gapなし
- runtime ARMA差し替えを遮断
- carrier manifestも`AdmittedContents()`が同じ結論を先に適用

最後の2点は実コードと一致しない。

### 経路A: `PickAddonForPlayer()` がguard前にARMAを読む

`ResolveArmaModels()` の実際の順序は次のとおり。

```text
SkinRebind.cpp:1222  direct ARMA lookup
SkinRebind.cpp:1224  ARMO lookup
SkinRebind.cpp:1225  PickAddonForPlayer(armo)
SkinRebind.cpp:1240  選択ARMAの IsDynamicForm()
SkinRebind.cpp:1245  選択ARMAの GetFile(0)
SkinRebind.cpp:1251  選択ARMAの PluginDenied()
SkinRebind.cpp:1259  bipedModels読取り
```

ところが `PickAddonForPlayer()` は `src/SkinRebind.cpp:1497-1521` で、
guard前の候補ARMAに対して次を読む。

```cpp
aa->race
aa->additionalRaces
```

つまり、runtime/no-file/半構築ARMAへの差し替えを想定するなら、
そのポインタを選択するための深いreadがhard判定より前に発生する。

`bipedModels`より前に判定があることは事実だが、
「ARMA内部を一切読む前に判定する」境界にはなっていない。
半構築ARMAが `race` または `additionalRaces` の読取りで破綻する場合、
追加されたguardへ到達する前にCEF側でCTDし得る。

### 経路B: carrier manifestは最終ARMA guardを通らない

`WriteCarrierManifest()` は `src/BoxStore.cpp:475-509` に独自resolverを持つ。

```text
LookupForm<ARMA>
  または LookupForm<ARMO> → PickAddonForPlayer
    ↓
選択ARMAのhard/plugin判定なし
    ↓
arma->bipedModels[sex].model
```

`ResolveArmaModels()` は呼ばれないため、今回追加した
`IsDynamicForm()`、`GetFile(0)`、`PluginDenied()` は一切適用されない。

これは前回のケースA・Bをそのまま再現する。

#### 反例A: 許可ARMOからdeny pluginのARMA

```text
AllowedWrapper.esp の静的ARMO
  → armorAddons
    → MARA.esp のARMA
```

`AdmittedContents()` はAllowedWrapper.espのARMOを判定するため通過する。
manifest側はMARA.espのARMAを選び、そのまま `bipedModels` を読む。

#### 反例B: 許可ARMOからruntime/no-file ARMA

```text
AllowedWrapper.esp の静的ARMO
  → 他DLLがarmorAddonsをruntime ARMAへ差し替え
```

ARMO本体はadmissionを通過する。
manifest側では選択ARMAのdynamic/no-file判定がない。

### `AdmittedContents()` は「同じ結論」を返さない

`src/BoxStore.cpp:452-461` の `AdmittedContents()` は、
各content idに `IsContentAdmissible()` を適用するだけである。

`IsContentAdmissible()` が検査するgeneric formはcolon-idが直接示すフォームであり、
ARMOから最終選択されるARMAではない。

したがって次の2結果が同時に成立する。

```text
IsContentAdmissible(ARMO) == true
ResolveArmaModels(ARMO)   == false  // selected ARMAがdeny/runtime/no-file
```

この時点で、base admissionとmodel admissionは別の判定系になっている。
`MARA_GUARD_IMPL.md:25` の
「carrier manifest側はAdmittedContentsが同じ結論を先に適用する」
という記述は明確に誤りである。

### explicit ID denyも最終ARMAへ適用されない

追加された最終ARMA判定はdynamic/no-file/pluginだけである。
許可ARMOが参照するARMA自身のcolon-idをuser deny-listへ追加しても、
ARMO本体のID判定は通過し、最終ARMAでは `IdDenied()` が実行されない。

「最終ARMAへのgeneric admission」と呼ぶなら、少なくとも次が必要である。

- dynamic
- no defining file
- plugin deny
- canonical final-ARMA ID deny

### policy更新中にもmanifest経路が先に走る

policy mutatorの順序は概ね次のとおり。

```text
PublishPolicy(next)
  → WriteJson()
      → WriteCarrierManifest()
  → ReevaluateContentAdmissions()
      → ReloadSettingsFromDisk()
```

`WriteJson()` は `src/BoxStore.cpp:297-302` でmanifestを同期生成する。
つまり、新policyをpublishした直後、active detachやregistry再構築より前に、
問題の独自ARMA resolverが動く。

deny entry追加そのものが、許可ARMO→危険ARMAのmanifest読取りを
誘発する可能性がある。

### 必須修正

ARMO admissionとARMA/model解決を別々の関数で再判定してはならない。
単一のsafe resolverへ統合する。

推奨形:

```cpp
struct AdmittedModels
{
    RE::TESObjectARMA* addon;
    ModelRef model3p;
    ModelRef model1p;
};

std::optional<AdmittedModels> ResolveAdmittedModels(
    const ContentId& content,
    RE::SEX sex,
    const policy::CapturePolicy& snapshot);
```

重要なのは型名ではなく、次の不変条件である。

1. addon候補ごとにdynamic/no-file/plugin/final-IDを先に検査する
2. 検査通過前に `race`、`additionalRaces`、`bipedModels` を読まない
3. race matchingはadmitted候補だけで行う
4. inject、shape scan、CanResolve、carrier manifestが同じresolverを使う
5. resolverが返した同じaddon/modelを再選択せず使用する
6. raw `PickAddonForPlayer()` を公開しない

manifestにはmodel pathだけを返す安全な公開seamを用意し、
独自のARMO→ARMA解決を削除するべきである。

---

### P1-2: quarantineが非原子的で、box ability cacheが残留する

### 改善された点

次の派生読者に `AdmittedContents()` を追加した方向性は正しい。

- token armor/weight
- passthrough keyword
- synthesized enchant abilityのbuild
- carrier manifest
- SMF stats summary

policy変更後に全activeをdetachし、gated registrationをやり直す方針も妥当である。

### 問題1: `g_boxSpells` がpolicy変更時に失効しない

boxのsynthesized abilityは `g_boxSpells` にcacheされる。

```text
BoxAbilityFor()
  → token keyがcacheにあれば既存SpellItem*を返す
```

policy再評価経路 `ReloadSettingsFromDisk()` は、

- active detach
- registry clear
- LoadBoxes
- persist active復元
- Reconcile
- `RebuildPersistAbility()`
- `ApplyBoxAbilities()`

を行うが、全boxに対する `RebuildBoxAbility()` または
既存spellのremove+cache invalidationを行わない。

そのため、blacklist追加前に構築済みだったbox abilityは、
blocked content由来のeffectを保持したままplayerへ残る。

逆にblacklist解除時も、以前のnullptrまたは縮小済みcacheが再利用され、
自動re-admit後のeffectが戻らない場合がある。

`BuildEnchantSpell()` 内で `AdmittedContents()` を使っていても、
build自体が再実行されなければquarantineにはならない。

これは `MARA_GUARD_IMPL.md:26` の
「アビリティ再構築」「全派生読者」の主張を満たさない。

### 問題2: quarantine完了前に派生処理が走る

前述のとおり、policy変更は `WriteJson()` 内のmanifest生成を先に実行する。

安全なquarantine transactionの順序は少なくとも次であるべきである。

```text
1. immutable policyをpublish
2. 1世代のadmission結果を構築
3. newly-blocked activeをdetach/unregister
4. playerから旧box/persist synthesized spellをremove
5. stats/keywords/spell cacheを再構築
6. admitted snapshotだけからmanifestを生成
7. re-admitted contentをregister/reconcile
8. settingsを永続化
```

実装都合で永続化を先にする場合でも、
`WriteSettingsJson()` と `WriteCarrierManifest()` を分離し、
quarantine完了前にmanifestを生成してはならない。

### 問題3: selected ARMA拒否が派生filterへ伝播しない

ARMO本体がadmittedでも、selected ARMAで拒否される場合がある。
しかし `AdmittedContents()` はARMO本体の結果しか見ない。

そのためselected ARMA拒否時にも、次はcontentをadmitted扱いする。

- token stats
- passthrough keyword
- synthesized ability
- carrier manifest
- SMF summary

必要なのは文字列IDの再判定ではなく、
policy世代ごとに構築した単一の `AdmissionResult` / quarantine setである。

推奨状態:

```text
Configured
  ├─ Admitted(resolved addon + models)
  ├─ Quarantined(reason)
  └─ Unresolved(reason)
```

全registrationと全派生処理が同じ状態スナップショットを参照すべきである。

---

### P2-1: CTestは動くが、r3修正を1件も検証していない

CTest登録そのものは成功している。
しかし `tests/policy_tests.cpp` の43 checksはr2で追加されたpure string policy testであり、
`cac79ca` ではテストコードが変更されていない。

現在のCTestが検証するもの:

- case-insensitive比較
- name/plugin/id matcher
- defaults on/off
- colon-id parse/format/canonicalize

現在のCTestが検証しないもの:

- ARMOからdeny plugin ARMAを選ぶケース
- ARMOからruntime/no-file ARMAを選ぶケース
- selected ARMAのexplicit ID deny
- guard前のrace/additionalRaces read
- carrier manifestのresolver
- active contentのblacklist追加時detach
- box ability cacheの削除と再構築
- token stats/keywordsの除去
- blacklist解除時の再admit
- 公開 `InjectArma()` のgate
- 公開 `CaptureEnchant()` のgate

したがって「CTest 1/1 green」は、
r3 safety fixが正しいことの証拠としては使えない。
現状は単に既存43 checksをtest runnerへ接続しただけである。

### 必須テスト

RE型へ直接依存する部分を薄いadapterの背後へ分離し、
fake form graphで少なくとも次を自動化する。

| Case | 期待 |
|---|---|
| allowed ARMO → denied-plugin ARMA | resolver/manifest/register全拒否 |
| allowed ARMO → dynamic ARMA | ARMAのdeep readなしで拒否 |
| allowed ARMO → no-file ARMA | ARMAのdeep readなしで拒否 |
| allowed ARMO → explicit-ID-denied ARMA | 全経路拒否 |
| blocked addonの後ろにsafe race-match addon | safe候補だけから選択 |
| active box contentをblacklistへ追加 | detach、registry除去、stats/keywords/spell/manifest除去 |
| blacklistから削除 | register、派生状態、manifest復元 |
| public InjectArma blocked input | unchecked primitive未到達 |
| CaptureEnchant blocked input | GetInventory未到達 |

fakeだけでなく、CommonLib/実機fixtureでruntime/no-file ARMAを用いたsmokeも必要である。

---

### P2-2: 「同類CTDをほぼ完全に防ぐ」にはNIF境界が未完成

`MARA_GUARD_IMPL.md:175-179` 自身が認めているとおり、
正規pluginレコードが破損NIFを参照するケースは未対処である。

`src/SkinRebind.cpp:185-195` の `LoadNif()` は
`RE::BSModelDB::Demand()` を直接呼ぶ。
この前に `ValidateNifSkinnable()` 相当の判定はなく、
load成功後も `Clone()` やscenegraph traversalへ進む。

このため保護できる範囲は、

> deny可能な既知plugin、dynamic/no-file form、既知name/id/keyword

までであり、

> 許可された静的ARMO/ARMAが参照する破損・敵対的NIF

は依然としてCEFの注入経路へ入る。

MARA固有事故だけを対象にするなら明示的なrisk acceptanceでよい。
しかし「他mod起因の類型クラッシュをほぼ完全に防ぐ」がリリース目標なら、
この項目はスコープ外ではなく安全境界の未実装である。

推奨:

1. BSResource/VFS経由でNIF bytesを取得する
2. hash単位でvalidator結果をcacheする
3. 可能なら別processのvalidatorでparseし、validator自身のcrashをCEFから隔離する
4. 0 vertex、invalid skin/bone refs、unsupported legacy shape等を理由付きquarantine
5. manifest/build/injectの全経路で同じNIF verdictを使う
6. validatorで保証不能なengine固有面は文書で明示し、deny追加へ誘導する

完全保証は困難でも、known-bad structural classesをCEF到達前に落とすことで
「他modの壊れたassetをCEFが能動的にロードしてCTDさせる」範囲を大きく縮小できる。

---

### P3-1: 差分検査に文書末尾空行

`git diff --check 49f91a4..HEAD` は次を報告する。

```text
MARA_GUARD_ADVERSARIAL_REREVIEW.md:494: new blank line at EOF.
```

実装上の問題ではないが、
`MARA_GUARD_IMPL.md:38-39` が説明しているhard-break末尾空白とは別の警告である。
release evidenceとして `git diff --check` を使うなら除去すべきである。

---

## 4. 再検証結果

### 成功

- branch: `mara-guard-v1.3.2`
- HEAD: `b171a0d`
- implementation commit: `cac79ca`
- generated build systemで `cmake --build build/release --config Release`: 成功
- CTest: 1/1 passed
- `capture_policy`: 43 checks相当の既存test executableが成功
- public `InjectArma()` gate: 静的確認
- `CaptureEnchant()` function-boundary gate: 静的確認
- CTest wiring: 静的・実行確認

### 失敗または未達

- `git diff --check 49f91a4..HEAD`: 文書末尾空行1件
- selected ARMAを使う全経路の一元化: 未達
- guard前にselected ARMAの内部を読まない保証: 未達
- carrier manifestのfinal-ARMA admission: 未達
- selected ARMA拒否と`AdmittedContents()`の判定一致: 未達
- policy変更時のbox ability cache invalidation: 未達
- r3で変更した安全経路の自動テスト: 未達

### 実機未検証

- MARA導入環境でのpicker 5手順
- allowed ARMO → denied-plugin ARMA fixture
- allowed ARMO → runtime/no-file ARMA fixture
- active contentをblacklistへ追加・解除する往復
- box ability/stats/keywords/manifestの即時除去と復元
- AE実機smoke
- VR実機smoke
- crash log上の旧CEF frame消失確認

---

## 5. リリース判定

### MARAの既知dynamic inventory ARMOだけを対象にする場合

r2で入った次の防御は有効である。

- `GetInventory` filter内でのdynamic/no-file判定
- blocked entryのコピー抑止
- `CaptureEnchant`のtarget-only filter
- dynamicを解除不能なhard invariant化

そのため、既知のpicker即CTD経路は大きく縮小している。

ただし実機証跡は未取得であり、最終承認はできない。

### 他mod起因の同類CTDを広く対象にする場合

**リリース不可。**

理由:

- 許可ARMOから危険ARMAへ到達できる
- danger checkより先にARMAのrace情報を読む
- manifestがfinal-ARMA guardを迂回する
- policy変更がmanifest生成を先に実行する
- quarantine後もcached box abilityが残り得る
- 変更部分を検証する自動testがない
- 破損NIFのCEF能動loadは未保護

少なくともP1-1とP1-2を修正し、
runtime/no-file/deny ARMA fixtureとpolicy往復testが緑になるまで
「全件修正」「静的には閉じた」という表現は使用すべきではない。

---

## 6. 推奨修正順序

1. `PickAddonForPlayer()` をsafe resolverへ統合し、guard前のARMA deep readをなくす
2. carrier manifestの独自resolverを削除し、injectと同じadmitted model結果を使う
3. selected ARMAへdynamic/no-file/plugin/final-ID policyを適用する
4. `AdmittedContents()` を単なるbase-form再判定から、世代付きadmission snapshot参照へ変更する
5. policy変更をquarantine transaction化し、manifest生成をdetach/rebuild後へ移す
6. 全box abilityをplayerからremoveした上でcacheを再構築する
7. r3反例matrixをfake form graphでCTest化する
8. runtime/no-file/deny ARMAの実機fixtureを追加する
9. MARA実機5手順、AE、VR smokeを実施する
10. NIF structural validationを別processまたは隔離可能な境界で導入する
11. 実装・test・文書の主張を同じcommitで更新する

最終的な安全性の鍵は、各所へboolean guardを追加することではない。

**「設定されたcontent」「選択された最終ARMA」「使用するmodel」「派生状態」が、同一policy世代の単一AdmissionResultからしか生成されない構造へ変えること**である。
