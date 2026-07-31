# MARA Guard 敵対的再レビュー

> 対象ブランチ: `mara-guard-v1.3.2`
> 初回レビュー対象: `a8ca5c5`
> 再レビュー対象: `49f91a4`
> 修正コミット: `d1bf735`
> 実施日: 2026-07-23
> 初回レポート: [MARA_GUARD_ADVERSARIAL_REVIEW.md](MARA_GUARD_ADVERSARIAL_REVIEW.md)

---

## 1. 結論

初回レビュー時点から大幅に改善され、**元のMARA picker CTD経路はコード上ほぼ閉じている**。

特に以下は適切に修正されている。

- `GetInventory()` のfilter内でblocked formを除外
- `CaptureEnchant()` を対象FormID限定に変更
- `IsDynamicForm()` を初手にした解除不能hard guard
- `allowDynamic` の完全撤去
- immutable policy snapshot
- 直接ARMA IDへのgeneric admission
- settings/co-save/主要native登録境界へのgate
- host-side policy testの追加

ただし、「他modに起因する同類CTDをほぼ完全に防ぐ」という基準では、
**P1が2件残るため、リリース停止勧告を維持する**。

残る主要問題は次の2点である。

1. ARMOから最終的に選択されるARMAへhard/plugin admissionが適用されない
2. block済みcontentを既存active・stats・keyword・ability・carrier manifestから隔離しない

### 重大度サマリ

| 重大度 | 件数 | 概要 |
|---|---:|---|
| P1 / リリース阻害 | 2 | 最終ARMA未検査、quarantine不完全 |
| P2 | 2 | raw injection迂回、公開CaptureEnchant native未ゲート |
| P3 / 検証基盤 | 1 | policy testがCTestへ未登録 |

---

## 2. 初回findingの修正確認

### 修正確認済み

| 初回finding | 再レビュー結果 |
|---|---|
| P1-1 `GetInventory` が判定前にentryをコピー | **修正確認**。`WornArmors` / `InventoryArmors` ともpolicy判定がfilter内へ移動 |
| P1-2 `CaptureEnchant` が全Armorをコピー | **修正確認**。対象 `baseId` 一致だけをfilter通過 |
| P1-3 最深部admission不在 | **主要経路は修正**。settings/co-save/Reapply/native/`cef box` は登録境界へ収束。ただしraw API残余あり |
| P1-4 動的判定と `allowDynamic` | **修正確認**。`IsDynamicForm()` 初手、no-file第2層、`allowDynamic` 撤去 |
| P1-5 blacklist policyのデータ競合 | **修正確認**。`atomic<shared_ptr<const CapturePolicy>>` のcopy-and-publish方式 |
| P2-1 直接ARMA IDがdenyを迂回 | **直接ARMAについて修正確認**。generic `TESForm` admissionを適用 |
| unit test不在 | **部分修正**。pure policy 43 checks追加。RE依存層は未自動化 |
| package件数表記 | **修正確認**。552→553 files + 9 foldersへ統一 |

### 評価

初回指摘に対する修正方針は概ね正しい。

特に、次の変更は今回のMARA事故に直接効く。

```text
GetInventory filter
  → IsDynamicForm
  → no defining file
  → plugin/non-playable/keyword/name/id
  → 通過したフォームだけ InventoryEntryData copy
```

これにより、MARAのruntime ARMOは `InventoryEntryData` のコピー前に除外される。

---

## 3. 残存findings

### P1-1: ARMOが参照する最終ARMAを検査していない

対象:

- `src/BoxStore.cpp:2044-2088` (`IsContentAdmissible`)
- `src/SkinRebind.cpp:1215-1260` (`ResolveArmaModels`)
- `src/SkinRebind.cpp:1460-1485` 周辺 (`PickAddonForPlayer`)

`IsContentAdmissible()` は入力IDがARMOを指す場合、ARMO自身について次を検査する。

- dynamic
- defining file
- plugin deny
- non-playable
- `CEF_NoCapture`
- name deny
- ID deny

しかし、実際にNIFを提供するフォームは `ResolveArmaModels()` が
`PickAddonForPlayer()` で選ぶARMAである。

現在の処理は概念上、次の順になる。

```text
ARMO admission
  → ARMOは安全
  → ResolveArmaModels
  → PickAddonForPlayer
  → 選択ARMAのbipedModelsを直接読む
```

このため、次の構造がpolicyを迂回する。

#### ケースA: deny対象pluginのARMA

```text
AllowedWrapper.esp の静的ARMO
  → MARA.esp / BadMod.esp のARMAを参照
```

ARMOのpluginは許可されるためadmissionを通過し、deny対象pluginのARMAが使用される。

#### ケースB: runtime/no-file ARMA

他modのDLLが静的ARMOの `armorAddons` をruntime ARMAへ差し替えた場合、
ARMO側のhard guardは通過するが、選択ARMAには次が適用されない。

- `IsDynamicForm()`
- defining file存在
- plugin deny

その後 `ResolveArmaModels()` は選択ARMAの `bipedModels` を読み始める。
半構築ARMAなら、この時点でCEF側CTDになり得る。

#### 必須修正

最終選択ARMAにもgeneric hard policyを適用する。

```cpp
AdmissionResult CheckGenericForm(
    RE::TESForm* form,
    const policy::CapturePolicy& policy);
```

ARMOの場合は次の順にする。

```text
canonical ID deny
  → ARMO generic check
  → ARMO-specific check
  → PickAddonForPlayer
  → selected ARMA generic check
  → model checks
```

理想的にはadmission結果が選択済みARMAを返す。

```cpp
struct AdmissionResult
{
    CaptureBlock reason;
    RE::TESForm* sourceForm;
    RE::TESObjectARMO* armor;
    RE::TESObjectARMA* addon;
};
```

`ResolveArmaModels()` はadmission後にARMAを再選択せず、同じ `addon` を使用する。
これにより判定と使用の不一致、将来的なTOCTOUを縮小できる。

---

### P1-2: quarantineが登録だけで、既存active・派生処理を止めない

対象:

- `src/BoxStore.cpp:2185-2226` (blacklist Add/Remove)
- `src/BoxStore.cpp:447-545` (`WriteCarrierManifest`)
- `src/BoxStore.cpp:2545-2589` (`BuildEnchantSpell`)
- `src/BoxStore.cpp:2646-2676` (`ApplyKeywordsToToken`)
- `src/BoxStore.cpp:2700-2720` (`SetTokenStats`)
- `src/BoxStore.cpp:2948-3005` (`BoxStatsSummary`)

blacklist追加後も、既に登録・注入されているcontentはdetachされない。

また、settingsに保持されたblocked contentは、登録を拒否された後も次の処理から参照される。

- armor/weight集計
- enchantment ability構築
- keyword passthrough
- stats表示
- carrier manifest生成
- carrier sync入力

現在のAdd処理は概念上次のとおり。

```text
copy current policy
  → add deny entry
  → PublishPolicy
  → WriteJson
  → WriteCarrierManifest
  → g_boxes内のcontentを再解決
```

つまり「危険だからblockした」対象を、そのblock操作後の `WriteJson()` /
carrier manifest生成が再び読む可能性がある。

#### 具体的な残存経路

`LoadBoxes()` でも次の順になる。

```text
RegisterBoxById(content)
  → admission拒否
  → 設定は保持
SetTokenStats(box)
  → 同じblocked contentをResolveArmo
  → armor/weight/keywordsを読む
```

これはquarantineとは呼べない。登録だけを止めても、CEF内部の派生処理が対象を読み続ける。

#### 必須修正

policy変更時にruntime stateを再評価する。

```text
PublishPolicy
  → ReevaluateContentAdmissions
      ├─ blocked active → detach/unregister
      ├─ admitted       → keep
      └─ config ID      → preserve
  → rebuild stats/keywords/abilities
  → rebuild carrier manifest from admitted contents only
```

さらに全派生処理は、raw `box.contents` ではなく共通のadmitted snapshotを使う。

```cpp
std::vector<std::string> AdmittedContents(
    const BoxDefInfo& box,
    const policy::CapturePolicy& policy);
```

ただし、毎フレームのUI表示や各処理でadmissionを繰り返し、同じ拒否ログを大量出力する実装は避ける。
policy世代ごとのquarantine setを一度構築し、参照側はそのsetを見る方式が望ましい。

状態は少なくとも次に分離する。

- configured
- admitted
- active
- quarantined
- unresolved

---

### P2-1: 公開 `InjectArma()` が中央admissionを迂回する

対象:

- `src/SkinRebind.cpp:2184-2194` (`InjectArma`)
- `src/Commands.cpp:126-143` (`cef inject`)
- `src/SkinRebind.h:28-34`

`InjectArma()` はadmissionなしで次を行う。

```text
ResolveArmaModels
  → Register
  → InjectInternal
```

公開コンソールの `cef inject` はこの関数を直接呼ぶ。

そのため次を迂回できる。

- explicit ID deny
- plugin deny
- `CEF_NoCapture`
- non-playable
- ARMO name deny
- hard generic check

通常UI経路ではないが、release-facingの公開コマンドであり、
「全登録経路が中央admissionへ収束する」という主張には反する。

#### 必須修正

未検査primitiveと公開wrapperを分離する。

```cpp
namespace
{
    bool InjectArmaUnchecked(...);
}

bool InjectArmaById(const std::string& id)
{
    auto admission = AdmitContent(id);
    if (!admission.allowed()) {
        return false;
    }
    return InjectArmaUnchecked(admission);
}
```

raw NIFや故意の危険assetを試す診断機能が必要なら、次のいずれかに限定する。

- debug build限定
- `cef unsafe-inject`
- 起動時設定で明示的に診断モードを有効化

通常の `cef inject` は必ずadmissionを通すべきである。

---

### P2-2: `CaptureEnchant()` 自身にはadmissionがない

対象:

- `src/BoxStore.cpp:2803-2840`
- `src/Papyrus.cpp:864-870`
- `papyrus/CFW_Native.psc:249-254`

対象FormID限定filterに変更されたため、**無関係なMARA entryをコピーする問題は修正済み**である。

しかし `CaptureEnchant()` はPapyrus nativeとして直接公開されており、関数自身には
`IsContentAdmissible()` がない。

CEFのMCM/SMFは先にcapture gateを通すため通常利用では安全だが、外部modは
blocked static armorへnativeを直接呼べる。

その場合、対象entryの次を読む。

- `InventoryEntryData`
- `extraLists`
- `IsWorn()`
- `GetEnchantment()`

#### 修正

関数境界でadmissionを適用する。

```cpp
bool CaptureEnchant(const std::string& content)
{
    std::string why;
    if (!IsContentAdmissible(content, &why)) {
        return false;
    }
    // target-only GetInventory
}
```

上位callerの事前確認はUX用、関数自身の確認は安全性強制用として二重化する。

---

### P3-1: policy testが自動実行されない

対象: `CMakeLists.txt:46-54`

`policy_tests` は `add_executable()` されているが、次がない。

- `enable_testing()`
- `add_test()`
- build/release workflowからの `ctest`

したがって `build.cmd release` はtest binaryをコンパイルするだけで、失敗結果を検出しない。

今回の再レビューでは手動実行し、次を確認した。

```text
policy_tests: 43 checks, 0 failure(s)
```

#### 推奨修正

```cmake
include(CTest)

if(CEF_BUILD_POLICY_TESTS)
    add_executable(policy_tests
        tests/policy_tests.cpp
        src/CapturePolicy.cpp
    )
    target_include_directories(policy_tests PRIVATE src)
    target_compile_features(policy_tests PRIVATE cxx_std_23)
    add_test(NAME capture_policy COMMAND policy_tests)
endif()
```

release validationでは次を実行する。

```powershell
ctest --test-dir build/release --output-on-failure
```

---

## 4. 再検証結果

### 成功

- Release `/W4` configure/build成功
- `CostumeExpansionFW.dll` link成功
- `policy_tests`: 43 checks / 0 failures
- 作業ツリーclean
- 通常パッケージ: 553 files / 9 folders
- VR patch: 2 files / 2 folders
- `allowDynamic` がUI/json/policyから撤去されている
- 旧settings内の `allowDynamic` は無視される
- blacklist vectorのin-place mutationが消滅
- `WornArmors` / `InventoryArmors` が操作単位の同一policy snapshotを使用
- `CaptureEnchant` が対象FormID以外を `GetInventory` filterで除外
- direct ARMA IDにgeneric form/plugin checkが適用される
- settings/co-save/`RegisterPersist`/`DefineBox`/`cef box` の主要登録経路がgateされる

### 未検証

- MARA実機
- runtime/no-file ARMO fixture
- runtime/no-file ARMAを参照する静的ARMO
- deny対象ARMAを参照する許可ARMO
- blacklist追加時のactive content detach
- blacklist追加後のstats/keyword/manifest隔離
- RE依存form-level test
- policy更新とpicker/renderのstress
- malformed/legacy NIF
- VR実機smoke

### `git diff --check`

現在の `git diff --check a8ca5c5..HEAD` は、初回レポート
`MARA_GUARD_ADVERSARIAL_REVIEW.md` のMarkdown hard-break用末尾空白とEOF空行を警告する。

該当は文書整形のみであり、ランタイム実装には影響しない。
ただし機械的なdiff hygieneをgreenにするなら、hard-breakを通常改行へ直すべきである。

---

## 5. リリース判定

### 現在のMARA事故だけを対象にした場合

次の主経路はコード上閉じている。

```text
MARA runtime CORE Carrier
  → GetInventory filter
  → IsDynamicForm
  → falseを返す
  → InventoryEntryDataをコピーしない
  → pickerに出ない
```

したがって、**元報告に対する修正としては妥当性が高い**。

ただしMARA実機確認は未実施であり、最終確定には次が必要である。

1. CORE Carrier装備状態でworn pickerを開く
2. inventory pickerを開く
3. 通常の安全な別アイテムを捕獲する
4. box/persist双方で反復する
5. save/load後に再実施する

### 同類CTDを広く防ぐ場合

以下が残るため未達である。

- static ARMOからruntime/no-file ARMAへ到達可能
- block追加後も既存active・派生処理が対象へ触れる
- raw console injectionがadmissionを迂回
- 公開enchant nativeがblocked targetへ触れられる

よって、ユーザーが求める「他modに起因する類型CTDをほぼ完全に防ぐ」という基準では、
**P1-1とP1-2を解消するまでリリース停止を継続する**。

P2はadvanced/raw入口だが、中央admissionを安全性境界として明文化するなら同時修正を推奨する。

---

## 6. 推奨修正順序

1. 最終選択ARMAへhard/plugin admissionを適用
2. policy変更時のactive/quarantine再評価を実装
3. stats/keyword/ability/manifestをadmitted contents限定にする
4. `InjectArma()` をinternal unchecked primitive化
5. `cef inject` をgated wrapperへ接続
6. `CaptureEnchant()` 自身へadmissionを追加
7. policy testsをCTest登録
8. MARA実機とruntime ARMA fixtureでsmoke
