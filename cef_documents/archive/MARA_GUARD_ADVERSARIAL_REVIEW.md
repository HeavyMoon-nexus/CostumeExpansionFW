# MARA Guard 敵対的レビュー

> 対象: `mara-guard-v1.3.2` / `MARA_GUARD_IMPL.md`  
> 基点: `main` @ `6877530` (`v1.3.1`)  
> 対象HEAD: `a8ca5c5`  
> 実施日: 2026-07-23  
> 目的: MARA 固有事故の緩和だけでなく、他modが不正・半構築・競合中のフォームや
> インベントリエントリを持ち込んだ場合にも、同類CTDを可能な限りCEF側で封鎖できるかを評価する。

---

## 1. 結論

**現状の v1.3.2 はリリース停止を推奨する。**

`WornArmors()` 内の `entry->IsWorn()` をブラックリスト判定後へ移した方向性は正しい。
しかし、次の主要主張は実装事実と一致していない。

- 「危険な `InventoryEntryData` にブロック判定前は一切触れない」
- 「全捕獲入口が `CanCaptureContent()` に収束する」
- 「C7（動的フォーム/セーブ整合）を完全に閉じた」

今回報告されたMARA固有ケースを軽減する可能性は高いが、「同型CTDをほぼ完全に防ぐ」
水準には未達である。特に、CommonLibSSE-NGの `GetInventory()` がループ開始前に
`InventoryEntryData` をコピーする事実により、主防御の順序保証が成立していない。

### 重大度サマリ

| 重大度 | 件数 | 概要 |
|---|---:|---|
| P1 / リリース阻害 | 5 | 判定前のentryコピー、中央ゲート不在、動的判定、設定データ競合、不正NIF残余 |
| P2 | 1 | 直接ARMA IDによるblacklist迂回 |
| 検証不足 | 2 | 自動テスト不在、MARA実機未検証 |

---

## 2. 良かった点

以下は明確な改善であり、維持すべきである。

- `entry->IsWorn()` を `IsCaptureBlocked()` より後ろへ移した。
- picker非表示とstore側拒否を二重化しようとした。
- 動的フォームをCEFの永続IDモデルと非互換として扱った。
- non-playable、deny-list、`CEF_NoCapture` の複層ポリシーを設けた。
- `AddBox()` / `AddPersistContent()` / preset / MCM / SMFの主要経路へ
  `CanCaptureContent()` を導入した。
- `%06X` が最小幅指定であることを認識し、バッファを拡張した。
- KID雛形を静的フォーム限定と明記した。
- MARA検出を挙動分岐ではなくtriageログに留めた。

問題は個々の層の存在ではなく、**最も危険な読みより前に層が実行されていないこと**と、
**最深部の登録関数が層を強制していないこと**である。

---

## 3. Findings

### P1-1: `GetInventory()` 内でブロック判定前にentryがコピーされる

対象:

- `src/BoxStore.cpp:2235-2237` (`WornArmors`)
- `src/BoxStore.cpp:2287-2289` (`InventoryArmors`)
- CommonLibSSE-NG `TESObjectREFR.cpp:324-375`
- CommonLibSSE-NG `InventoryEntryData.cpp:19-25`

現在のpickerは次のfilterで `GetInventory()` を呼んでいる。

```cpp
auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
    return a_obj.Is(RE::FormType::Armor);
});
```

その後、返されたmapを走査して `IsCaptureBlocked(armo)` を実行する。

しかしCommonLibSSE-NGの `GetInventory(filter)` は、filterを通過したentryについて
次を実行する。

```cpp
results.emplace(
    entry->object,
    std::make_pair(
        entry->countDelta,
        std::make_unique<InventoryEntryData>(*entry)));
```

`InventoryEntryData` のコピーコンストラクタは `extraLists` をコピーする。

```cpp
if (a_rhs.extraLists) {
    extraLists = new BSSimpleList<ExtraDataList*>(*a_rhs.extraLists);
}
```

したがって、実際の順序は次のとおりである。

1. MARAのARMOがArmor filterを通過する
2. MARAの `InventoryEntryData/extraLists` がコピーされる
3. `GetInventory()` がmapを返す
4. ようやく `IsCaptureBlocked()` が実行される

`IsWorn()` 固有の障害なら偶然回避できるが、壊れた `extraLists`、リストコピー中の変更、
寿命切れポインタが原因なら元のCTD面は残る。

#### 必須修正

構造ガードを `GetInventory()` のfilter内へ移す。

```cpp
const auto policy = CapturePolicySnapshot();
auto inv = player->GetInventory([policy](RE::TESBoundObject& obj) {
    if (!obj.Is(RE::FormType::Armor)) {
        return false;
    }
    auto* armo = obj.As<RE::TESObjectARMO>();
    return armo && CaptureBlockReason(armo, *policy) == CaptureBlock::kNone;
});
```

少なくとも動的フォーム、source fileなし、non-playableのハード/構造層は
entryコピーより前に拒否しなければならない。

---

### P1-2: 安全な別アイテムの捕獲でも全Armor entryをコピーする

対象: `src/BoxStore.cpp:2798-2830` (`CaptureEnchant`)

`CaptureEnchant()` は特定contentのエンチャントだけを取得したいにもかかわらず、
全Armorをfilter通過させている。

```cpp
auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
    return a_obj.Is(RE::FormType::Armor);
});
```

これにより、ユーザーがCORE Carrierを選択していなくても、通常の安全なアイテムを1個
捕獲しただけでMARA entryのコピーが発生する。

picker側の表示フィルタが完全でも、この経路だけで同型CTDが成立し得る。

#### 必須修正

対象FormIDのみfilterを通す。

```cpp
auto inv = player->GetInventory([baseId](RE::TESBoundObject& obj) {
    return obj.GetFormID() == baseId;
});
```

同様に、全インベントリを取得してから対象を探す実装をリポジトリ全域で禁止し、
`GetInventory()` のfilterを安全境界として扱うべきである。

---

### P1-3: admission gateが最深チョークポイントにない

対象:

- `src/SkinRebind.cpp:1576-1599` (`RegisterBoxById`)
- `src/SkinRebind.cpp:2205-2227` (`RegisterArmaById`)
- `src/SkinRebind.cpp:2230-2244` (`InjectArmaById`)
- `src/BoxStore.cpp:1125-1189` (settings読込)
- `src/BoxStore.cpp:1329-1337` (settings登録)
- `src/BoxStore.cpp:1502-1509` (`ReapplyBoxes`)
- `src/BoxStore.cpp:1626-1645` (persist再有効化)
- `src/Cosave.cpp:124-143` (co-save復元)
- `src/Papyrus.cpp:78-96` (`RegisterPersist` / `DefineBox`)
- `src/Commands.cpp:163-171` (`cef box`)

次の経路は `CanCaptureContent()` を通らず直接登録・注入へ到達する。

- settings読込
- 手編集JSON
- `ReapplyBoxes`
- co-save復元
- persist再有効化
- Papyrus `RegisterPersist`
- Papyrus `DefineBox`
- コンソール `cef box`

特に手編集JSONは `AddBox()` / `AddPersistContent()` を通らず、
`g_boxes` / `g_persist` へ直接格納される。このため
`MARA_GUARD_IMPL.md` の「hand-JSONもstore前線で保護」は成立しない。

#### 必須修正

`RegisterBoxById()`、`RegisterArmaById()`、`InjectArmaById()` を真の中央境界とし、
必ずhard admission policyを適用する。

ロード済みsettingsやco-saveの禁止対象は、設定から削除してはならない。
次の状態として保持する。

```text
configured / restored
        ↓
policy check
  ├─ allowed     → register/inject
  └─ quarantined → retain ID, do not register, log reason
```

これにより、一時的なdeny-listやmod構成変更でユーザー設定を破壊せず、
危険な再登録だけを防げる。

---

### P1-4: 動的フォーム判定の最初の読みが不適切

対象:

- `src/BoxStore.cpp:2012-2028`
- `src/BoxStore.cpp:324-335` (`MakeColonId`)
- CommonLibSSE-NG `RE/T/TESForm.h:272-300,323`

現在は最初に `GetFile(0)` を呼び、nullなら動的フォームとみなしている。

```cpp
const auto* file = a_armo->GetFile(0);
if (!file && !g_captureBlacklist.allowDynamic) {
    return CaptureBlock::kDynamicForm;
}
```

問題は次のとおり。

1. `GetFile()` は `sourceFiles.array` とそのコンテナを読む。
2. CommonLibにはFormIDのみを読む `IsDynamicForm()` がある。
3. 0xFF FormIDなのにsourceFilesを持つフォームは現判定をすり抜け得る。
4. 半構築フォームのsourceFilesが壊れていればガード自身が落ち得る。
5. `GetLocalFormID()` は内部で `GetFile(0)` の戻り値をnull確認せず参照する。

`allowDynamic=true` で動的フォームがpicker行へ進むと、`MakeColonId()` が
`GetLocalFormID()` を呼び、決定的なnull dereferenceになり得る。

#### 必須修正

```cpp
if (a_form->IsDynamicForm()) {
    return CaptureBlock::kDynamicForm;
}
const auto* file = a_form->GetFile(0);
if (!file) {
    return CaptureBlock::kNoDefiningFile;
}
```

動的フォームはCEFの永続モデル上保存不能であり、正当なcapture用途がない。
そのため `allowDynamic` は次のいずれかにすべきである。

- 製品版から削除
- デバッグビルド限定
- 非永続のone-shot診断コマンドに限定

少なくとも通常UIから永続的にhard invariantを解除できる設計は避けるべきである。

`MakeColonId()` もfile存在確認前に `GetLocalFormID()` を呼んではならず、
動的/no-fileフォームには失敗値を返す契約へ変更すべきである。

---

### P1-5: blacklist設定自身にデータ競合がある

対象:

- `src/BoxStore.cpp:111-130`
- `src/BoxStore.cpp:1953-2008`
- `src/BoxStore.cpp:2112-2225`
- `src/SmfUI.cpp:785-879`

`g_captureBlacklist` のvectorとboolは、レンダ/VMスレッドから読み、
メインスレッドのAddTaskからpush/erase/代入される。

mutex、immutable snapshot、世代固定のいずれもない。特にvectorのコピー・走査中に
別スレッドでpush/eraseが起きれば、再確保後の解放済み領域を読む可能性がある。

これは標準C++上の未定義動作であり、blacklist UI自体が新しいCTD面になる。
「既存の `g_boxes` と同じ許容クラス」は安全性の証明にならない。

#### 必須修正

推奨順:

1. `std::shared_ptr<const CapturePolicy>` をatomic publishする
2. または `std::shared_mutex` で全読み書きを保護する

immutable snapshot方式では、各列挙開始時に一つの世代を取得し、
filter、loop、capture gateの全工程で同じsnapshotを使う。

```cpp
struct CapturePolicy
{
    std::vector<std::string> names;
    std::vector<std::string> plugins;
    std::vector<std::string> ids;
    bool allowNonPlayable;
    bool disableDefaults;
};

std::shared_ptr<const CapturePolicy> CapturePolicySnapshot();
```

UI操作は新しいpolicyを構築してatomicに差し替え、公開済みvectorを直接変更しない。

---

### P2-1: 直接ARMA IDはID/plugin blacklistを迂回する

対象:

- `src/BoxStore.cpp:2086-2109` (`CanCaptureContent`)
- `src/SkinRebind.cpp:1223-1267` (`ResolveArmaModels`)
- `papyrus/CFW_Native.psc:25-27`

CEFはcontentとして直接ARMA IDも受け付ける。しかし `CanCaptureContent()` は
`ResolveArmo()` が成功した場合だけ `CaptureBlockReason()` を実行する。

```cpp
if (auto* armo = ResolveArmo(a_id)) {
    const auto reason = CaptureBlockReason(armo);
    // ...
}
```

直接ARMA IDでは `ResolveArmo()` がnullになり、その後の `CanResolveContent()` は
ARMAを正常なcontentとして受け入れる。

その結果、直接ARMAでは次が無効になる。

- explicit `ids`
- plugin deny-list
- dynamic/source-file検査

#### 必須修正

判定を次の層へ分離する。

1. canonical ID文字列に対するdeny
2. 汎用 `TESForm` に対するdynamic/source-file/plugin判定
3. ARMO固有のnon-playable/name/keyword判定
4. 選択されたARMAとmodel pathに対する検査
5. model resolvability

explicit IDの照合は `ResolveArmo()` より前に実行しなければならない。

---

## 4. 既知残余C1: 不正NIF取込

`MARA_CRASH_CLASS_AUDIT.md` が認めるlegacy `NiTriShape` / 0頂点skinの問題は、
「類型CTDをほぼ完全に防ぐ」という目標に対する最大の残余である。

現在の直接注入経路は次の順で動作する。

```text
colon-id
  → ResolveArmaModels
  → BSModelDB::Demand
  → Clone
  → geometry/skin traversal
  → rebind/inject
```

engine loader内部のdivide-by-zeroやaccess violationは、`LoadNif()` の戻り値検査より前に
発生し得る。carrier生成経路の `ValidateNifSkinnable()` は直接注入には適用されない。

### 推奨恒久策

BSA内assetにも対応するには、`std::filesystem` だけでは不十分である。
推奨アーキテクチャは次のとおり。

1. Bethesda VFS経由でasset bytesを取得
2. bytesを別プロセスのvalidatorへ渡す
3. NIF parserでlegacy geometry、0頂点skin、異常partition、破損block graphを検査
4. plugin/localID、NIF path、asset hashごとに判定をキャッシュ
5. 未検証・検証失敗assetは注入せずquarantine

エンジン内 `BSModelDB::Demand` をSEHで囲み、例外後も同じゲームプロセスで継続する方式は、
エンジン内部状態が部分更新されている可能性があるため最終手段とすべきである。

---

## 5. 推奨する防御アーキテクチャ

### 5.1 Hard policyとsoft policyを分離する

#### Hard policy（解除不能）

- null form
- `IsDynamicForm()`
- defining fileなし
- malformed/empty-plugin ID
- deleted/ignored form
- content型不一致
- model解決不能
- asset validator失敗

#### Soft policy（ユーザー設定可能）

- non-playable
- shipped/user name deny-list
- plugin deny-list
- explicit ID deny-list
- `CEF_NoCapture`

安全性の根幹であるdynamic/no-file拒否を通常UIから解除させてはならない。

### 5.2 一つの中央admission関数を作る

概念上、全経路を次へ収束させる。

```cpp
AdmissionResult AdmitContent(
    std::string_view id,
    const CapturePolicy& policy,
    AdmissionPurpose purpose);
```

`AdmissionPurpose` は少なくとも次を区別する。

- picker enumeration
- physical capture
- preset assignment
- settings restore
- co-save restore
- direct registration/injection

返り値にはboolだけでなく、層・理由・対象フォーム・model pathを含める。

### 5.3 quarantineを第一級状態にする

禁止対象を設定やco-saveから即時削除すると、modの一時無効化やdeny-list誤爆で
ユーザーデータを破壊する。

そのため次を区別する。

- configured
- active
- quarantined
- unresolved

quarantinedはUIとログから理由を確認でき、policy変更後に安全に再評価できるべきである。

---

## 6. リリース前テストマトリクス

### 6.1 自動テスト

`MARA_COMPAT_PLAN.md` に記載されたunit testは現ブランチには実装されていない。
少なくとも次をhost-side testとして追加する。

#### Policy判定

- dynamic FormID
- no defining file
- non-playable
- deleted/ignored
- name exact / prefix
- plugin prefix
- explicit ID
- `CEF_NoCapture`
- default無効化
- Unicode/ASCII case
- malformed ID
- empty plugin
- direct ARMA
- ARMOから参照されたARMA

#### 順序保証

- blocked formでは `InventoryEntryData` コピーが発生しない
- blocked formでは `IsWorn()` を呼ばない
- blocked formではname/keywordを不要に読まない
- unrelated blocked armorが存在しても `CaptureEnchant(safeId)` が触れない

#### 全入口

| 入口 | Box | Persist | 期待 |
|---|---:|---:|---|
| MCM worn picker | ✓ | ✓ | blocked非表示 |
| MCM inventory picker | ✓ | ✓ | blocked非表示 |
| SMF worn picker | ✓ | ✓ | blocked非表示 |
| SMF inventory picker | ✓ | ✓ | blocked非表示 |
| AddBox/AddPersist | ✓ | ✓ | 中央拒否 |
| preset | ✓ | ✓ | quarantine/skip |
| settings reload | ✓ | ✓ | 設定保持・登録拒否 |
| co-save restore | ✓ | ✓ | 状態保持・登録拒否 |
| RegisterPersist/DefineBox | ✓ | ✓ | 中央拒否 |
| console `cef box` | ✓ | — | 中央拒否 |
| persist再有効化 | — | ✓ | 中央拒否 |

#### 競合

- policy snapshot取得とAdd/Removeの並行stress
- settings reload中のpicker描画
- picker snapshot取得直後のpolicy変更
- content mod無効化/再有効化

### 6.2 実機テスト

#### MARAあり

1. CORE Carrier装備状態でworn pickerを開く
2. inventory pickerを開く
3. 通常の安全な別アイテムを捕獲する
4. box/persistの両方を試す
5. save/loadする
6. cell移動・装備変更中にpickerを開く
7. MARAがcarrierを追加・除去・リネームしているタイミングで反復する

特に手順3は必須である。現状の `CaptureEnchant()` は、CORE Carrierを選ばなくても
全Armor entryをコピーするためである。

#### MARAなし

- runtime ARMO test fixture
- sourceFilesなしのARMO
- non-playable ARMO
- KIDで `CEF_NoCapture` を付与したARMO
- direct ARMA preset
- blacklist対象pluginのARMA
- malformed/legacy NIF

#### VR

- 同じfilter順序がVR CommonLib実装でも成立すること
- picker open/capture
- Load3D/reconcile
- save/load

---

## 7. 最低限のリリース条件

v1.3.2を出荷する前に、少なくとも次を完了すべきである。

1. 全 `GetInventory()` 呼び出しで危険フォームをfilter段階から除外する
2. `CaptureEnchant()` を対象FormIDだけの走査に変更する
3. `IsDynamicForm()` を最初のハードガードにする
4. `allowDynamic` を製品UI・永続設定から外す
5. `MakeColonId()` のno-file安全性を保証する
6. `RegisterBoxById()` / `RegisterArmaById()` / `InjectArmaById()` に中央ゲートを置く
7. blacklistをimmutable snapshot化する
8. ARMO/ARMA双方にID/plugin判定を適用する
9. settings/co-save復元対象をquarantineできるようにする
10. MARA実機で「pickerを開く」と「安全な別アイテムを捕獲する」の両方を検証する

不正NIFの完全対策を次版へ送る場合、v1.3.2の主張は次の範囲へ狭めるべきである。

> MARAのruntime carrierをCEFのinventory/capture経路から除外する。
> 正規plugin recordが参照する破損NIFの安全性は保証しない。

「C7を完全に閉じた」「同類事故を全滅させた」という表現は、上記修正とテストが
完了するまで使用すべきではない。

---

## 8. 実施した検証

- `MARA_GUARD_IMPL.md`、`MARA_COMPAT_PLAN.md`、
  `MARA_CRASH_CLASS_AUDIT.md` を実装と照合
- `v1.3.1` (`6877530`) からHEADまでの差分を確認
- picker、capture、preset、settings、co-save、Papyrus、consoleの入口を追跡
- CommonLibSSE-NG 3.7.0の `GetInventory()` と
  `InventoryEntryData` コピーコンストラクタを確認
- `build.cmd release` をworkspace内deploy先で実行
- Release `/W4` ビルド成功
- `git diff --check 6877530..HEAD` 成功
- 作業ツリーがcleanであることを確認
- `CostumeExpansionFW-1.3.2.7z` にDLL、PEX、
  `CostumeFW_NoCapture_KID.ini` が含まれることを確認

### 検証上の不足

- guardのunit test targetが存在しない
- MARA実機テスト未実施
- runtime form fixtureによるテスト未実施
- 並行stress test未実施
- malformed NIF実機/隔離テスト未実施

### 成果物記録の不一致

7-Zip実測:

- v1.3.1: 552 files, 9 folders
- v1.3.2: 553 files, 9 folders

`MARA_GUARD_IMPL.md` の「562 → 563 entries」と一致しない。
差分がKIDファイル1個である点は整合するが、監査証跡として件数表記を修正すべきである。

