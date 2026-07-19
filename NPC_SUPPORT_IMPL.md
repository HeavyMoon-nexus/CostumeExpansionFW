# NPC サポート実装仕様書(単独実装用)

> **本書単体で、文脈ゼロのセッション(Codex / Opus 4.8 等)が実装に着手できることを目的とする。**
> why(設計判断と経緯)は [NPC_SUPPORT_PLAN.md](NPC_SUPPORT_PLAN.md)。本書は how(ファイル/関数単位の変更指示)。
> 判断に迷ったら PLAN を読み、PLAN と本書が矛盾したら**本書を優先**(こちらが新しい)。
> 前提コード状態: **v1.3.1(commit 6877530)**。本文の file:line は同時点の実測値
> (実装中にズレたら関数名で再アンカーすること。行番号は「近傍の目印」であって契約ではない)。
> 作成: 2026-07-18。

---

## 0. 実装者が最初に読む約束事(このリポジトリの掟)

1. **メインスレッド規律**: シーングラフ(`Get3D`/attach/detach)・インベントリ・装備・spell 付与は
   **SKSE タスクキュー経由のメインスレッドのみ**。イベント sink からは必ず
   `SKSE::GetTaskInterface()->AddTask([...]{...})` で hop する(plugin.cpp:75-78 が手本)。
   SMF の描画コールバックも同様(UI から native を呼ぶ→ゲーム状態を触る処理は task 投行)。
2. **ビルドと配備**: `build.cmd`(リポジトリルート)でビルド。**ゲーム/MO2 起動中の DLL コピーは
   静かに失敗する**。検証前に必ずログの build 刻印(`CostumeExpansionFW loaded (file ... / compile ...)`
   plugin.cpp:210)で「今ロードされたのが今ビルドした DLL か」を確認。古い刻印なら MO2 を完全再起動。
3. **kill-switch**: `CostumeFW::HardDisabled()`(Config.h)が真なら何も登録しない
   (plugin.cpp:227)。新規フック/sink/スレッドも全てこのガードの内側に置く。
4. **co-save の破損耐性**: 読みは常に上限付き(`kMaxStringLen`/`kMaxItemCount` 相当)で
   「壊れたレコードは小さく失敗」(Cosave.cpp:34-53)。未解決 FormID は**捨てずに持ち越す**
   (ROOT H パターン、Cosave.cpp:42-43, 76-81, 136-140)。
5. **usvfs**: ゲーム内からの `std::ifstream`/`WriteFileAtomic` は MO2 の VFS を通る。
   既存ファイルへの上書きは mod フォルダへ write-through(実測済・NIFCARRIER_INPROC §6.10)。
   **外部プロセスが新規作成したファイルはセッション中見えない**(だから carrier は
   pre-created 回転スロット)。in-proc からの新規作成の可視性は未実測(→スパイク S1)。
6. **メモリ計測の作法**: 膨張を見たら CEF を疑う前に**割当の帰属を取る**(過去 2 回とも犯人は
   サードパーティ DLL)。FSMP rebuild は 1 回 ~2.5GB の SSE Engine Fixes アリーナを確定させるので、
   rebuild 回数を増やす変更は必ず回数カウンタ(g_persistDiag 方式)を添える。
7. **エンコーディング**: ソース/JSON/ログは UTF-8。PowerShell(5.1)スクリプトの出力は
   `-Encoding utf8` 明示。
8. **コミット規律**: 1 WP(ワークパッケージ)= 1 コミット目安。コア ESP(CostumeFW.esp)には
   **一切触れない**(本機能のレコードは全て新規アドオン ESP)。
9. **禁則(恒久)**: §14 を先に読むこと。特に「facegen head 経路を NPC に使わない」
   「FSMP 収束目的の自動再装備をしない」は設計決定であり最適化で覆さない。

## 1. 対象コードの地図(現状構造)

| ファイル | 役割 | 本実装で触る度 |
|---|---|---|
| src/SkinRebind.cpp(~2335 行) | 注入コア(rebind/reconcile/watchdog/realbody/headpart) | ★★★(Phase 0 の主戦場) |
| src/SkinRebind.h | 注入公開 API | ★★ |
| src/BoxStore.cpp(~3100 行) | box 定義・settings.json・token stats・abilities・manifest・in-proc sync 配線・隠しストア | ★★★(Phase 1 の主戦場) |
| src/BoxStore.h | box 公開 API | ★★ |
| plugin.cpp | フック/イベント sink/ライフサイクル | ★★ |
| src/Cosave.cpp | co-save('ACTV'/'STOR') | ★★ |
| src/SmfUI.cpp | SKSE Menu Framework UI | ★★ |
| src/Commands.cpp | `cef` コンソール | ★ |
| src/Config.h / CostumeExpansionFW.ini | ini 設定 | ★ |
| src/BodyMorph.cpp/.h | skee body-morph ブリッジ | ★(呼び出し 1 点) |
| src/nifcarrier/NifCarrierCore.{h,cpp} | in-proc carrier 焼き(純関数層) | ★★(published/npcPersist 焼き追加) |
| papyrus/(CostumeFW_MCM.psc) | MCM(transition-only) | ★(stub 1 ページ) |
| package_assets/ + package_*.ps1 | リリース staging | ★(アドオン zip 追加) |

### 1.1 SkinRebind.cpp の内部構造(変則に注意)

- 無名 namespace #1: :42-1290(コアヘルパー+状態)。無名 namespace #2: :1666-1808(headpart ヘルパー)。
- **watchdog 系(`kBindWatchdogMs`:1314, `g_watchdogTickPending`:1315, `BindWatchdogTick`:1317,
  `StartBindWatchdogOnce`:1340)と `g_mouthRestoreRetries`:1851 は無名 namespace の外**
  (`CostumeFW` 直下)にある。リファクタで移動する場合はリンケージに注意。

### 1.2 プレイヤー暗黙の全在庫(Phase 0 で潰す対象)

`RE::PlayerCharacter::GetSingleton()` 呼び出し 19 箇所:
SkinRebind.cpp:119, 593, 888, 899, 953, 1065, 1181, 1325, 1382, 1486, 1628, 1813, 1862, 1950,
1957, 1998, 2058, 2109(+plugin.cpp:74, 104)。
うち **headpart/mouth 系(:1813, 1862, 1950, 1957, 1998, 2058, 2109)はプレイヤー専用のまま残す**
(禁則 §14)。actor 化するのは注入/レジストリ/リアルボディ/watchdog 系のみ。

グローバル状態の在庫と処遇:

| 宣言行 | 名前 / 型 | 現状 | Phase 0 での処遇 |
|---|---|---|---|
| :82 | `g_active` `vector<ActiveItem>` | プロセス単一(=プレイヤー) | **per-actor 化**(§2.2 `g_actors`) |
| :114 | `g_boundBoneRefs` `map<string,vector<NiPointer>>` | content-id キー | ActorState 内へ移動((actor,id) 化) |
| :115 | `g_boneRefSink`(生ポインタ) | main-thread 単一値 | **InjectCtx へ移動**(§2.3) |
| :315 | `g_injectStatic3p` | 同上 | InjectCtx へ |
| :324 | `g_rebindPrefix` | 同上 | InjectCtx へ |
| :312-316 | retry budget/queued/ids/inRetry | プロセス単一 | ActorState 内へ(予算・キューとも per-actor) |
| :334-336 | head rebuild debounce 群 | player head 前提 | **触らない**(player 専用のまま) |
| :348 | `g_persistDiag` | 単一カウンタ | 残す+NPC 集計カウンタを追加(§2.7) |
| :1315 | `g_watchdogTickPending` | 単一 | 残す(tick は全 actor を回る、§2.6) |
| :1851 | `g_mouthRestoreRetries` | player mouth | 触らない |

既に actor 非依存で流用可能な層(**変更不要**):
- per-content 設定 accessor 群(BoxStore: `HideSlotsFor`/`GenderModeFor`/`BodyMorphOn`/
  `ShowRealBodyOn`/`HideShapesFor`/`ContentShapesFor` — 全て content-id キー)
- `RebindGeometry`(:504)/`HasDeadPhysicsBind`(:416)/`InjectOnRoot`(:741)— root 引数を取る
- `ResolveArmaModels`(:1223)— ただし ARMO 経路の `PickAddonForPlayer`(:1233)だけ race 引数化が必要
- `BodyMorph::ApplyToNode(refr, node)`(BodyMorph.cpp:54)— 既に refr 引数化済み。
  プレイヤー固定なのは**呼び出し側**(SkinRebind.cpp:888)の 1 点だけ

---

## 2. Phase 0 — actor 一般化リファクタ(機能追加ゼロ)

**完了条件 = プレイヤー挙動が 1bit も変わらないこと**(§12 回帰ゲート)。NPC 向けの新機能は
一切入れない(Character フックは入れるが、binding が存在しないため実質 no-op)。

### WP0.1 レジストリの per-actor 化

`SkinRebind.cpp` 無名 namespace #1 に追加(既存 `ActiveItem`:73 はそのまま流用):

```cpp
// One actor's injection universe. [0] is ALWAYS the player (created eagerly);
// NPC entries appear only when Phase-1 bindings register them.
struct ActorState
{
    RE::ActorHandle handle;             // empty for the player slot (resolve via singleton)
    bool isPlayer{ false };
    std::vector<ActiveItem> items;      // was g_active
    std::unordered_map<std::string, std::vector<RE::NiPointer<RE::NiAVObject>>> bonePins;  // was g_boundBoneRefs
    std::vector<std::string> rebindRetryIds;   // was g_rebindRetryIds
    int rebindRetryBudget{ kRebindRetryBudget };
    bool rebindRetryQueued{ false };
    bool realBodyShown{ false };        // per-actor CEF_RealBody bookkeeping
};
std::vector<ActorState> g_actors;       // main-thread only; [0]=player, appended per NPC

ActorState& PlayerState();              // g_actors[0] を返す(無ければ作る)
ActorState* FindState(RE::Actor* a);    // handle 一致 or (isPlayer && a==player)
RE::Actor* ResolveActor(ActorState& s); // player slot は singleton、他は handle.get().get()
```

- 実装ノート: 検索は線形走査でよい(要素数はプレイヤー+キャップ 8 程度)。
- **既存の全 `g_active` 参照(:82, 88, 98, 165, 1330, 1373, 1384, 1386, 1473, 1612-1615,
  1654-1658, 2244-2256, 2273)を `PlayerState().items` または「全 state 走査」に振り分ける。**
  どちらに振るかは §2.5 の関数別表に従う。
- `Register`(:84)/`Unregister`(:163)は `ActorState&` を第 1 引数に取る形へ変更し、
  既存シグネチャは `PlayerState()` を渡す薄いラッパとして残す(公開 API 非破壊)。

### WP0.2 注入コンテキスト(main-thread 単一値の除去)

現状 `g_boneRefSink`/`g_rebindPrefix`/`g_injectStatic3p` は「同時に 1 アクターしか注入できない」
構造の元凶。以下の構造体で貫通させる:

```cpp
struct InjectCtx
{
    std::vector<RE::NiPointer<RE::NiAVObject>>* boneSink{ nullptr };
    std::string rebindPrefix;           // nifcarrier::ContentNamePrefix(id)
    bool static3p{ false };             // set by RebindGeometry on ancestor-remap
    RE::NiAVObject* firstPersonRoot{ nullptr };  // player: Get3D(true); NPC: nullptr
};
```

変更点:
- `RebindGeometry(RE::NiSkinInstance*, RE::NiAVObject* root)`(:504)→ 第 3 引数 `InjectCtx&`。
  - :544-550 の prefix 参照 → `ctx.rebindPrefix`。
  - :593-596 の static 判定 `pc && a_root != pc->Get3D(true)` →
    **`a_root != ctx.firstPersonRoot`**(player 時は従来同値。NPC 時は firstPersonRoot=nullptr
    なので全 remap が static3p 扱い=リトライ対象。これが正しい意味論)。
  - :605-607 の sink append → `ctx.boneSink`。
- `InjectOnRoot`(:741)→ 第 9 引数 `InjectCtx&` を追加し `RebindGeometry` へ渡す。
  BSVisit ラムダのキャプチャに `&ctx` を足すだけ。
- `InjectInternal`(:897)→ `InjectFor(ActorState& s, const ActiveItem& it)` に改名・置換:
  ```cpp
  bool InjectFor(ActorState& s, const std::string& id, const ModelRef& m3p, const ModelRef& m1p)
  {
      RE::Actor* actor = ResolveActor(s);
      if (!actor) return false;
      InjectCtx ctx;
      ctx.rebindPrefix = nifcarrier::ContentNamePrefix(id);
      ctx.firstPersonRoot = s.isPlayer ? actor->Get3D(true) : nullptr;
      std::vector<RE::NiPointer<RE::NiAVObject>> collected;
      ctx.boneSink = &collected;
      // ... 既存 :913-933 の 3p/1p 注入を actor->Get3D で。1p は s.isPlayer のときだけ。
      // g_injectStatic3p 参照(:926)は ctx.static3p に。
      // RequestRebindRetry(id) は RequestRebindRetry(s, id) に(§2.4)。
      // :935-938 の pin 追記は s.bonePins[id] へ。
      // BodyMorph::ApplyToNode の refr は InjectOnRoot 側で actor を受ける(§2.3 注)。
  }
  ```
- **`InjectOnRoot` の morph 呼び出し(:886-889)**: `RE::PlayerCharacter::GetSingleton()` 固定を
  actor 引数化する。`InjectOnRoot` に `RE::Actor* a_morphActor` を追加(morph gate が false なら
  未使用)。呼び出し元 `InjectFor`/`InjectRealBody` が actor を渡す。
- `DetachNodes`(:117)→ `DetachNodes(ActorState& s, const std::string& id)`:
  :119 の player 取得を `ResolveActor(s)` に、:141-142 の両 root を
  `actor->Get3D(false)` / `s.isPlayer ? actor->Get3D(true) : nullptr` に。pin 解放(:123)は
  `s.bonePins.erase(id)`。

### WP0.3 Character::Load3D フック

plugin.cpp の `Load3DHook`(:28-51)の直後に追加(同型・vtable swap):

```cpp
// NPC 3D rebuilds (cell attach, outfit refresh, resurrect) — same vfunc as the
// player hook. Fires for EVERY Character; bail immediately unless CEF has a
// binding for this actor (cheap handle lookup) so city cells stay free.
struct NpcLoad3DHook
{
    static RE::NiAVObject* thunk(RE::Character* a_this, bool a_backgroundLoading)
    {
        auto* result = func(a_this, a_backgroundLoading);
        if (CostumeFW::HasActorBindings(a_this)) {        // SkinRebind 公開の O(1)〜O(n) 判定
            const auto h = a_this->GetHandle();
            SKSE::GetTaskInterface()->AddTask([h] { CostumeFW::ReconcileActorByHandle(h); });
            CostumeFW::RunAfterDelayMs(4000, [h] { CostumeFW::ReconcileActorByHandle(h); });
        }
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
    static void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{ RE::Character::VTABLE[0] };
        func = vtbl.write_vfunc(0x6A, thunk);
        SKSE::log::info("Character::Load3D hook installed (0x6A)");
    }
};
```

- Install は `kDataLoaded` の `Load3DHook::Install()` 直後(plugin.cpp:135)。
- **PlayerCharacter は独自 vtable なので二重発火しない**(両方に入れるのが正しい)。
- vfunc index 0x6A の SE/AE/VR 一致は**スパイク S3 で実測してから本実装に進む**
  (PlayerCharacter 側 0x6A は 3 ランタイム稼働実績あり。Character 側は未検証)。
- `HasActorBindings` / `ReconcileActorByHandle` は SkinRebind.h に新設(Phase 0 では
  binding が player のみ=NPC では常に false → フックは実質 no-op。これで
  「フック自体の安全性」だけを Phase 0 で先に実証する)。

### WP0.4 Reconcile の分割

`Reconcile()`(:1369-1461)を 3 層に分ける:

```cpp
void Reconcile()                    // 既存シグネチャ維持(全呼び出し元無変更)
{
    StartBindWatchdogOnce();
    ++g_persistDiag.reconcileCalls;
    for (auto& s : g_actors) { ReconcileActor(s); }
    SweepDeadActors();              // §2.5
}
void ReconcileActorByHandle(RE::ActorHandle h);  // FindState → ReconcileActor
static void ReconcileActor(ActorState& s);       // 旧 Reconcile 本体の actor 版
```

`ReconcileActor` への書き換え規則(旧本体 :1373-1460 との対応):
- :1382 `player` 取得 → `actor = ResolveActor(s)`。**actor が null(unloaded/dead)なら
  pin だけ解放して return**(3D が無いのに NiPointer で古い骨を掴み続けない — 新規の
  ライフサイクル要件。旧コードには存在しない)。
- :1393-1396 worn 判定 `player->GetWornArmor(it.tokenForm)` → `actor->GetWornArmor(...)`。
  persist(tokenForm==0)常時表示の分岐はそのまま。
- :1401-1412 hide-when-worn → `actor->GetWornArmor(slot)`。`IsBoxToken` 判定は Phase 1 で
  `IsCefToken`(box∪publish∪npcPersist キャリア)に拡張(§7.4)。
- :1418-1430 sex 再解決 → `EffectiveSexOf(actor, it.id)`(§2.8)。
- :1437-1443 dead-bind sweep → `actor->Get3D(false)` + `DetachNodes(s, it.id)`。
- :1445 `InjectInternal` → `InjectFor(s, ...)`。
- :1446-1460 real-body → `s.realBodyShown` を使い `InjectRealBody(s)` / `DetachRealBody(s)`(§2.9)。
- retry 予算の再武装(:1379-1381)は `s.rebindRetryBudget = kRebindRetryBudget`(per-actor)。

`RequestRebindRetry`(:371)→ `RequestRebindRetry(ActorState& s, id)`: キュー/予算/queued
フラグを `s` のメンバで。遅延タスク(:387-399)は handle をキャプチャし、実行時に
`FindState` で引き直す(state の実体は vector 内で動き得るため**参照をキャプチャしない**。
handle→FindState が Phase 0 の不変ルール)。

### WP0.5 Detach 系の per-actor 化

- `DetachAllInjected()`(:1626)→ 内部を `DetachAllInjectedOn(RE::Actor*)` に切り出し、
  公開版は全 state+player を走査して合算。
- `DetachAll`(:1610)/`ClearRegistry`(:2254)/`ActiveSnapshot`(:2244)/`ListActive`(:1652):
  - `ClearRegistry` = **全 state の items/pins/retry をクリアし、NPC state は vector から除去**
    (player slot は残す)。co-save revert(Cosave.cpp:157)から呼ばれる契約は不変。
  - `ActiveSnapshot` は **player state のみ**を返す(既存の co-save 'ACTV' 契約を壊さない。
    NPC binding の永続化は Phase 1 の 'PUBB' が別建てで持つ)。
  - `ListActive` は全 state をアクター名付きで出力(`cef list` の情報量が増えるのは許容)。
- `DetachSkinned`/`HideInjectedNodes`/`RefreshGender`(:2259-2281): 既存シグネチャは
  player state 向けラッパとして維持。内部関数は `(ActorState&, id)`。

### WP0.6 watchdog の per-actor 化

`BindWatchdogTick`(:1317-1338)の走査を全 state 化:

```cpp
for (auto& s : g_actors) {
    RE::Actor* actor = ResolveActor(s);
    if (!actor) continue;
    auto* r3 = actor->Get3D(false);
    if (!r3) continue;
    for (const auto& it : s.items) {
        if (HasDeadPhysicsBind(s, it.id, r3)) {   // 内部で NodeName/pin を per-actor 参照
            ++g_persistDiag.watchdogReconciles;
            ReconcileActor(s);
            break;                                 // 1 tick 1 actor 1 修復(既存挙動踏襲)
        }
    }
}
```
head-rebuild grace(:1322-1324)は player 専用条件なので `s.isPlayer` のときだけ適用。
`HasDeadPhysicsBind`(:416)はシグネチャ現状維持で可(root 引数済み)。

### WP0.7 real-body の actor 引数化(§2.9 として詳細)

`InjectRealBody()`(:1063-1157)/`DetachRealBody()`(:951)を `(ActorState& s)` 引数化:
- :1065 player 取得 → `ResolveActor(s)`。
- :1081-1082 `base = actor->GetActorBase()` / `race = actor->GetRace()`。
- 以降の解決ロジック(:1087-1153)は**そのまま流用可能**(既に base->skin(WNAM 相当)優先
  → race->skin フォールバック、engine-faithful ゲート `baseAA && raceMatched && HasBodyModel`、
  性別フォールバック、`skinTextures[sex]` の `ApplySkinTextures` 適用、という actor 概念で
  書かれている。プレイヤー固定なのは取得元の 3 行だけ)。
- 1p 注入(:1149-1152)は `s.isPlayer` 時のみ。
- morph 引数(常時 true、:1146/:1151)へ `actor` を渡す。
- ノード名 `kRealBodyNode`("CEF_RealBody"、:949)は per-actor スケルトンに付くので
  名前衝突しない(検索は各 actor の root 起点)。
- **注意**: `reqSex` のデフォルトは base 無し時 kMale(:1115)、`PlayerSex()` のデフォルトは
  kFemale(:1183)。**この非対称は既存挙動なので保存する**(勝手に統一しない)。

### WP0.8 sex / addon 解決の actor 引数化

- `PlayerSex()`(:1179)→ `ActorSexOf(RE::Actor*)`(base->GetSex、base 無しは kFemale 維持)。
  `PlayerSex()` はラッパで残す。
- `EffectiveSex(id)`(:1189)→ `EffectiveSexOf(RE::Actor*, id)`: `GenderModeFor(id)` が
  1/2 なら強制、0 なら `ActorSexOf(actor)`。公開 `EffectiveSexFor(id)`(:1568)は
  player ラッパのまま(BoxStore の manifest 経路が使用中のため契約不変)。
- `PickAddonForPlayer(armo)`(:1481)→ 内部 `PickAddonForRace(armo, race)` に切り出し、
  公開版はラッパ維持。`ResolveArmaModels`(:1223)の ARMO 経路(:1233)に
  `RE::TESRace*` 引数を追加(`ResolveArmaModelsFor(localID, plugin, sex, race, out3p, out1p)`;
  既存シグネチャは player race を渡すラッパ)。

### WP0.9 Phase 0 のスコープ外(明示)

- `RegisterArmaById`/`RegisterBoxById`/`DefineBox`/`InjectArmaById`(:1573-1608, 2189-2242)は
  **player ラッパのまま**(NPC 登録 API は Phase 1 で 'PUBB' 復元用に新設)。
- headpart 系(:1666-2105)・mouth watchdog(:1860)・`RebuildPlayerHead`(:1996)・
  head rebuild debounce(:334-336, 2017)は**無変更**。
- `EnumerateContentShapes`(:1536)は player 3D を使わない(NIF ロードのみ)ので無変更。

### WP0.10 Phase 0 コミット分割の目安

1. ActorState 導入+g_active/pins/retry の移送(挙動同一、player slot のみ)
2. InjectCtx 貫通(RebindGeometry/InjectOnRoot/InjectFor)
3. Reconcile 3 層化+DetachNodes/Detach 系 per-actor 化
4. real-body / sex / addon の actor 引数化
5. watchdog per-actor 化
6. Character::Load3D フック(S3 実測後)+HasActorBindings/ReconcileActorByHandle 公開
7. 回帰ゲート実施(§12.1)→ タグ付け

---

## 3. co-save 拡張('PUBS' / 'PUBB' / 'NPRS')

Cosave.cpp に追記。既存パターン(WriteString/ReadStringChecked、上限ガード、
ResolveFormID remap、未解決持ち越し)を**そのまま踏襲**する。

### 3.1 レコード定義

```cpp
constexpr std::uint32_t kRecordPubState = 'PUBS';   // per-publish per-save 状態
constexpr std::uint32_t kPubStateVersion = 1;
constexpr std::uint32_t kRecordPubBind  = 'PUBB';   // publish×actor binding
constexpr std::uint32_t kPubBindVersion = 1;
constexpr std::uint32_t kRecordNpcPersist = 'NPRS'; // Phase 1.5
constexpr std::uint32_t kNpcPersistVersion = 1;
constexpr std::uint32_t kMaxBindCount = 1024;       // 破損ガード
```

### 3.2 'PUBS' レイアウト(Save 側)

```
u32 count
count × { u8 pubSlot;  u8 flags(bit0=hidden) }
```
書き込みは PublishStore(§6)の `PubSaveState()` スナップショットから。読みは
`RestorePubState(slot, hidden)` を呼ぶ。**カタログ(グローバル)に無い pubSlot でも捨てない**
(カタログはプロファイル別にズレ得る。restore 側で dormant 保持)。

### 3.3 'PUBB' レイアウト

```
u32 count
count × {
    u8  pubSlot
    u32 actorFormID          // 保存時値。ロードで ResolveFormID remap
    u8  flags                // bit0=holder(インベントリ所持) bit1=wearer(装備中)
}
```
- ロード: `a_intfc->ResolveFormID(saved, resolved)` 失敗時は **'ACTV' の ROOT H と同型で
  `g_unresolvedPubBinds` に (pubSlot, savedFormID, flags) を退避**し、Save 時に再書き出し
  (Cosave.cpp:76-81 のパターンをそのまま複製)。
- 解決成功: `PublishStore::RestoreBinding(pubSlot, resolvedFormID, flags)`。
  **actor の実体化はしない**(handle 取得は Reconcile/Load3D 側の遅延で行う —
  ロード時点で対象セルが attach 済みとは限らない)。
- Revert: バインド表と unresolved 退避を clear('ACTV' と同じ場所、Cosave.cpp:157-167)。

### 3.4 'NPRS' レイアウト(Phase 1.5)

```
u32 count
count × {
    u8  poolSlot             // 隠しキャリア pool index 0-7
    u32 actorFormID          // remap + 未解決退避は 'PUBB' と同一規約
    u32 contentCount
    contentCount × string    // colon-form content id(共有カタログ参照)
}
```

### 3.5 統合点

- `SaveCallback`(Cosave.cpp:55)末尾に 3 レコードの書き出しを追加。
- `LoadCallback`(:95)の while ループに `kRecordPubState`/`kRecordPubBind`/`kRecordNpcPersist`
  分岐を追加(既存 'STOR' 分岐 :103-115 が手本)。
- `LoadCallback` 末尾の task(:151-154)に **publish/npc-persist の再適用**を追加:
  `Reconcile(); SyncPersistManifest();` の後に `CostumeFW::ReapplyNpcBindings();`
  (PublishStore 公開。ロード済みセルの着用者への注入は per-actor Load3D/Reconcile が拾うので、
  ここでは binding 表の再構築と、ロード済み actor への初回 ReconcileActor 発行だけ行う)。

---

## 4. イベント sink 分岐(plugin.cpp)

### 4.1 EquipSink(plugin.cpp:60-85)

```cpp
RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e, ...) override
{
    if (!e || !e->actor) return kContinue;
    // 安価な順にフィルタ: base form が我々のトークンか → どの actor か。
    const bool pub = CostumeFW::IsPublishToken(e->baseObject);       // §6 公開(hash set)
    const bool npr = CostumeFW::IsNpcPersistCarrier(e->baseObject);  // Phase 1.5
    auto* actor = e->actor->As<RE::Actor>();
    const bool isPlayer = actor == RE::PlayerCharacter::GetSingleton();
    if (isPlayer) {
        // 既存経路そのまま(:75-78): Reconcile + ApplyBoxAbilities。
        // player が publish トークンを装備した場合もこの Reconcile が拾う
        // (player state に publish binding を持たせる、§7.2)。
        SKSE::GetTaskInterface()->AddTask([] {
            CostumeFW::Reconcile();
            CostumeFW::ApplyBoxAbilities();
        });
    } else if (pub || npr) {
        const auto h = actor->GetHandle();
        const RE::FormID base = e->baseObject;
        const bool equipped = e->equipped;
        SKSE::GetTaskInterface()->AddTask([h, base, equipped] {
            CostumeFW::OnNpcTokenEquip(h, base, equipped);   // §7.3
        });
    }
    return kContinue;
}
```
**注意**: `TESEquipEvent` は全 actor で全装備品につき発火する。NPC 分岐は
`pub||npr`(unordered_set 照会)を actor 解決より先に評価し、通常装備品を最短で捨てる。

### 4.2 ContainerSink(plugin.cpp:90-117)

既存の Replenish 分岐(:106-110)は**無変更**(box トークン専用のまま)。続けて追加:

```cpp
// Publish tokens: track custody. NEVER replenish (leaving the player is the point).
if (CostumeFW::IsPublishToken(e->baseObj)) {
    const RE::FormID from = e->oldContainer, to = e->newContainer, base = e->baseObj;
    SKSE::GetTaskInterface()->AddTask([from, to, base] {
        CostumeFW::OnPublishTokenMoved(base, from, to);      // §7.3
    });
}
```
`TESContainerChangedEvent` の from/to は **FormID(reference)**。actor 解決は task 内で
`TESForm::LookupByID<RE::TESObjectREFR>` → `As<RE::Actor>()`(null なら容器/ワールド=追跡外。
ただし **holder 解除は from 側 FormID が binding 表に居れば actor 解決できなくても行う** —
死体消滅・セルリセットでも表が腐らないように)。

---

## 5. アドオン ESP 仕様(CostumeFW_NPC.esp)

> レコードは**全て新規プラグイン**。コア CostumeFW.esp への追加・変更は禁止。
> 作成手段はリポジトリのESPツーリングに従う(§5.3)。

### 5.1 レコード表(ローカル FormID は ESL 互換域 0x800 起点)

| FormID | 型 | EDID | 内容 |
|---|---|---|---|
| 0x800-0x807 | ARMO | `CFW_PubToken01`..`08` | publish トークン。name は仮 "Costume (unpublished)"(ランタイムで上書き §7.5)。playable。エンチャント無し。BOD2 はダミー値(slot44 等 — ランタイムスタンプで上書き) |
| 0x808-0x80F | ARMA | `CFW_PubCarrier01`..`08` | 各トークンの carrier。M/F worldmodel は**プリ作成回転ファイルの r0 を初期値**で指す(§6.3 の命名)。1st-person モデル空 |
| 0x810-0x817 | ARMO | `CFW_NpcPersistTok01`..`08` | Phase 1.5 隠しキャリア。**non-playable フラグ**。BOD2 = ini 既定スロット(54) |
| 0x818-0x81F | ARMA | `CFW_NpcPersistCar01`..`08` | 同 carrier ARMA(単一ファイル・両性別同一) |

- 各 ARMA の race: `DefaultRace` + additionalRaces にバニラ playable 10 種+吸血鬼変種。
  **child race は入れない**。
- ESL フラグ: 付与を既定(SkyrimVRESL 前提は VR パッチ側で既に要求済み)。リリース時に
  コア ESP の方針と最終整合。
- トークン ARMO→ARMA の紐付けは 1:1(`armorAddons = [対応 ARMA]`)。

### 5.2 presence 検知

BoxStore.cpp(または新設 PublishStore.cpp)に:

```cpp
constexpr const char* kNpcPlugin = "CostumeFW_NPC.esp";
constexpr std::uint32_t kPubTokenFirstId = 0x800;   // ..0x807
constexpr std::uint32_t kPubCarrierFirstId = 0x808;
constexpr std::uint32_t kNprTokenFirstId = 0x810;   // Phase 1.5
constexpr int kPubPoolSize = 8;

bool NpcEspLoaded();   // kDataLoaded で TESDataHandler::LookupModByName(kNpcPlugin) &&
                       // LookupForm<TESObjectARMO>(kPubTokenFirstId, kNpcPlugin) を 1 回だけ判定しキャッシュ
```

- gate 対象: publish ボタン(§9)・NPC ページ本体(§9)・manifest published/npcPersist
  フラグメント生成(§6.2)・sink の `IsPublishToken`(ESP 不在なら常に false)。
- **co-save の読み書きは gate しない**(§3 — dormant データ温存)。

### 5.3 ESP の作成・梱包・espmerge 除外

**リポジトリの ESP 事情(実測)**: ESP バイナリはリポジトリに無く、配備済み MO2 mod フォルダ
(`K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW`)が実体。編集は「CostumeFW.esp をマスターに
した patch esp を書き、リリース前に `tools/espmerge`(Mutagen C#)で手動 fold」が既存
ワークフロー(package.ps1:6-7 が前提ステップとして列挙、fold 規則は espmerge/Program.cs:110-127)。

アドオン ESP はこのワークフローの**外**に置く:

1. **作成**: xEdit で新規プラグイン `CostumeFW_NPC.esp`(マスター: Skyrim.esm のみ。
   コア CostumeFW.esp をマスターにしない — 相互独立)。§5.1 のレコード表が唯一の真実源。
   ESL フラグ付与。配備先は新規 mod フォルダ `CostumeFW_NPC`(コアと別フォルダ)。
   ARMA の初期 worldmodel は `CostumeFW\PubNN_carrier_{m,f}_r0.nif`(§6.3 の命名)を指す。
2. **espmerge 除外**: fold は手動実行(自動パイプライン無し)なので「渡さない」が原則。
   事故防止に (a) tools/espmerge/Program.cs 冒頭コメントへ
   「CostumeFW_NPC.esp is a PERMANENT separate addon — NEVER fold it」を追記、
   (b) tools/package.ps1 の前提ステップ列(package.ps1:6-7)へ同旨 1 行を追記。
3. **プレースホルダプール生成**: publish 用回転ファイルを package_assets へ追加する
   1 回きりの生成スクリプト(tools/gen_pub_placeholders.ps1、新規):
   - `package_assets\meshes\CostumeFW\boxtoken.nif` 相当(既存プレースホルダ 234B 級)をコピーし
     `PubNN_carrier_{m,f}_r{0..7}.nif`(N=01..08 → 8×2×8=128 個)を作成。
   - `package_assets\meshes\CostumeFW\XML\Box44_physics_r0.xml` を雛形に
     `PubNN_{m,f}_physics_r{0..7}.xml`(128 個)を作成。
   - (Phase 1.5)`NpcPersistNN_carrier_r{0..7}.nif` + `NpcPersistNN_physics_r{0..7}.xml`(各 64 個)。
   - 生成後、配備側 mod フォルダ `CostumeFW_NPC\meshes\CostumeFW\` へも初回コピー
     (実機はこちらを read/write する。以後 in-proc sync が上書きしていく)。
4. **梱包**: tools/package_npc_addon.ps1(新規、package_vr_patch.ps1 を雛形に):
   - 収集物 = 配備フォルダの `CostumeFW_NPC.esp` +
     **package_assets の pristine プレースホルダ**(PubNN_*/NpcPersistNN_* のみ)+ `README_NPC.txt`。
   - **配備フォルダの meshes は絶対に梱包しない**(auto-sync がユーザーの第三者コスチューム由来
     メッシュで上書き済み — v1.3.0 の 85MB 漏洩事故の再発防止。package.ps1:26-31 と同じ理由)。
   - `-Version` 必須、コアと同版番でロックステップ。stale-deploy ガード(esp mtime 検査)も
     package.ps1:46-50 から移植。
5. **本体 zip は不変**(package.ps1 に PubNN_* を**含めない** — アドオン専用アセット)。

---

## 6. スナップショットカタログと両性別キャリア焼き

新 TU **src/PublishStore.{h,cpp}** を作り CMakeLists の DLL SOURCES(CMakeLists.txt:45-61)へ追加。
BoxStore.cpp(既に 3100 行)は「フック 2 点+既存関数の再利用」に留める。

### 6.1 データ構造と CEF_settings.json 拡張

現行スキーマ(schema "cef.settings/1"、書き側 WriteJson = BoxStore.cpp:194-268、読み側
LoadBoxes = :1006-1284)に**任意キーを 2 つ追加**(スキーマ文字列は据え置き — 旧 DLL は未知キーを
無視し、新 DLL は `doc.value()` 既定値で旧 JSON を読める):

```jsonc
"published": [ {
    "pubSlot": 0,                      // 0-7 = CFW_PubToken01..08
    "label": "Maid Outfit",
    "sourceSlot": 47,                  // 元 box トークンの biped slot(スタンプ元)
    "armorType": 1,                    // 0/1/2(SetTokenStats と同写像)
    "manualAbility": "",               // colon-form SPEL id("" = 無し)
    "rev": 3,                          // 内容改訂カウンタ(焼きの hash とは独立の UI 用)
    "contents": [ "00A1B2:SomeMod.esp", ... ],
    "settings": {                      // per-content 設定の DEEP COPY(追跡解除の実体)
        "hideRules":    { "<id>": [37] },
        "genderModes":  { "<id>": 2 },
        "bodyMorph":    [ "<id>" ],
        "hideShapes":   { "<id>": ["3BA_Body"] },
        "showRealBody": [ "<id>" ],
        "enchants":     { "<id>": [ {"mgef":"XXXXXX:P.esp","mag":20.0} ] }
    }
} ],
"npcConfig": { "maxNpcInjected": 8 }   // チューナブルは ini でなく settings.json が本リポの慣用
                                       // (ini は kill-switch 専用 — Config.h:9-31)
```

PublishStore 側の型(settings の内形は BoxStore の各マップと同形):

```cpp
struct PubSnapshot {
    int pubSlot{ -1 };
    std::string label;
    int sourceSlot{ 0 };
    int armorType{ 0 };
    std::string manualAbility;
    int rev{ 0 };
    std::vector<std::string> contents;
    std::unordered_map<std::string, std::vector<int>> hideRules;
    std::unordered_map<std::string, int> genderModes;
    std::unordered_set<std::string> bodyMorphOn;
    std::unordered_map<std::string, std::vector<std::string>> hideShapes;
    std::unordered_set<std::string> showRealBody;
    std::unordered_map<std::string, std::vector<EnchEffect>> enchants;  // EnchEffect は BoxStore.cpp:115
};
std::vector<PubSnapshot> g_published;   // グローバルカタログ(全セーブ共有)
```

**フック 2 点**: WriteJson の doc 組立末尾(BoxStore.cpp:260 過ぎ、`WriteCarrierManifest()` 呼び出し
:267 より前)に `PublishStore::EmitJson(doc)`、LoadBoxes の型ガード try(:1074-1249)内に
`PublishStore::ParseJson(doc)`。ingest 検証: pubSlot 範囲外/重複エントリは警告して捨てる、
contents/settings の colon-id は `CanonicalizeColonId`(SkinRebind.h:79)を通す。

### 6.2 manifest 拡張(WriteCarrierManifest)

`WriteCarrierManifest`(BoxStore.cpp:402-500)の doc に `NpcEspLoaded()` のとき **published 配列**を
追加(version は 1 のまま — sync 側は未知キー無視で後方互換):

```jsonc
"published": [ {
    "pub": 0,                          // pubSlot
    "contents_m": [ { "id": "...", "nif": "<male-resolved path>" } ],
    "contents_f": [ { "id": "...", "nif": "<female-resolved path>" } ]
} ]
```

解決は既存 `resolveContent` lambda(:413-448)の**性別引数版**を作る:
`resolveContentForSex(id, RE::SEX sex, const PubSnapshot& snap)` —
- forced-gender は **snap.genderModes**(live の `GenderModeFor` ではない)を見る: 1→kMale 固定、
  2→kFemale 固定、0→引数 sex。
- ARMA 解決と sex-fallback(:437-446)は同一ロジック(male 版は male モデル優先・無ければ female、
  female 版はその逆)。
- ARMO→ARMA は `PickAddonForPlayer`(:426)のままで可(publish 焼きはメッシュ形状の解決であり
  race 依存の分岐は現行 box と同等の割り切り)。

compare-skip(:482-491)と `ScheduleAutoSync()`(:499)は既存のまま published 差分にも効く。

### 6.3 nifcarrier::Sync の published 対応(NifCarrierCore.cpp)

`Sync`(NifCarrierCore.h:146、実装は NifCarrierCore.cpp の box ループ)に published 処理を追加:

- 入力: manifest の `published[]`。各エントリ×各性別(m/f)を「1 本の box 相当」として
  **既存 per-box パイプラインをそのまま通す**: 解決済み nif リスト→hash-skip→
  1 件なら ZeroAlpha / 2 件以上は IsolateContent+Merge+MergeXml+SetXml→Validate ゲート→
  temp 書き→atomic publish→回転。
- **回転は既存 `kSlots = 8`(NifCarrierCore.cpp:1538)を流用**(PLAN §4 の「回転 4」は本書で
  8 に上書き — 定数と回転ロジックを分岐させない方が退行面で安全。プレースホルダは §5.3 で
  8 回転分生成済み)。
- 命名(box 規則 :2421-2428 に倣う):
  - NIF: `CostumeFW/Pub{NN}_carrier_{m|f}_r{rev%8}.nif`(NN = pubSlot+1 を 2 桁)
  - XML: `meshes\CostumeFW\XML\Pub{NN}_{m|f}_physics_r{rev%8}.xml`
- ハッシュキーは (pub, sex) 毎に独立(片性別だけの再焼きを許す)。
- carriers.json への書き出し(:2469-2478 の全体 rewrite に追加):
  ```jsonc
  "published": { "0": { "rev": 3, "fileM": "CostumeFW/Pub01_carrier_m_r3.nif",
                          "fileF": "CostumeFW/Pub01_carrier_f_r3.nif" } }
  ```
  rev 算出は box と同じ「旧エントリ+1」(:2419 のパターン)。
- **失敗時規約は box と同一**: validate 不合格/コピー失敗 → 旧リビジョン温存 + failed++、
  hash は publish 成功後に最後に書く(NIFCARRIER_INPROC §6.11 の順序修正を踏襲)。

### 6.4 ApplyCarrierOverridesImpl の publish repoint

`ApplyCarrierOverridesImpl`(BoxStore.cpp:706-776)の box ループ後に追加(NpcEspLoaded() ガード):

```cpp
// Published tokens: sex-split repoint (male/female slots point at DIFFERENT files).
bool RepointCarrierSexed(RE::TESObjectARMO* armo, const std::string& mFile, const std::string& fFile)
{
    if (!armo || armo->armorAddons.empty()) return false;
    auto* arma = armo->armorAddons.front();
    bool changed = false;
    if (mFile != arma->bipedModels[RE::SEXES::kMale].model.c_str())   { arma->bipedModels[RE::SEXES::kMale].model = mFile.c_str();   changed = true; }
    if (fFile != arma->bipedModels[RE::SEXES::kFemale].model.c_str()) { arma->bipedModels[RE::SEXES::kFemale].model = fFile.c_str(); changed = true; }
    return changed;
}
```
- carriers.json `published[std::to_string(pubSlot)]` から fileM/fileF を取り、**両方**
  `CarrierFileOnDisk`(:564-599)ゲートを通す(どちらか不合格ならそのトークンは repoint しない
  — ESP 初期値 r0 のまま)。
- refreshChanged 時の挙動は box と同じ「DebugNotification のみ」(:767-769。自動 re-equip 禁止
  — §14-2。NPC への反映は NPC ページの Refresh)。

---

## 7. publish のライフサイクル実装

### 7.1 判定ヘルパ(PublishStore 公開)

```cpp
bool NpcEspLoaded();                       // §5.2(kDataLoaded で 1 回判定・キャッシュ)
bool IsPublishToken(RE::FormID form);      // kDataLoaded で 0x800..0x807 の解決済み FormID を
                                           // unordered_set に張る。ESP 不在なら常に false
RE::TESObjectARMO* PubTokenArmo(int pubSlot);
const PubSnapshot* PubBySlot(int pubSlot);
const PubSnapshot* PubByTokenForm(RE::FormID form);
```

**ガードのタダ乗り(重要な確認事項)**: `IsTokenPluginFile`(BoxStore.cpp:277-288)は
「プラグイン名の先頭 9 文字が "CostumeFW"(大小無視)」判定なので、**`CostumeFW_NPC.esp` は
自動的に一致**する。よって:
- capture 候補リスト(WornArmors :1880-1883 / InventoryArmors :1925-1928)から publish トークンは
  **無改修で除外**される。
- ROOT C ガード `IsTokenColonId`(:2848-2859)も**無改修で publish トークンを content 不可**にする。
- **代償となる制約**: `TokenPool()`(:1952-1979)は「IsTokenPluginFile ∧ fullName が
  "Costume Box" で始まる」で box トークンを発見するため、**publish トークンの fullName を
  "Costume Box" で始めてはならない**(box プールに混入する)。§7.5 の命名規則
  `"Costume: <label>"` / 未 publish 時 `"Costume (unpublished)"` はこの制約を満たす。
  単体テスト的に `cef pub diag` で TokenPool 件数不変を確認すること(§9.3)。

hide-when-worn の自トークン免除(SkinRebind.cpp:1408 の `IsBoxToken(worn->GetFormID())`)は
`IsCefToken(form)` = `IsBoxToken(form) || IsPublishToken(form) || IsNpcPersistCarrier(form)` に
差し替える(BoxStore.h に宣言追加。Phase 1 時点で第 3 項は常 false の stub で可)。

### 7.2 バインディング表(ランタイム)と ActiveItem 拡張

```cpp
// PublishStore 内(main-thread only)。co-save 'PUBB' と 1:1 対応。
struct PubBinding {
    int pubSlot;
    RE::FormID actorFormID;     // co-save 表現(remap 済み)
    RE::ActorHandle handle;     // 遅延解決(actor ロード時に充填)
    bool holder{ false };
    bool wearer{ false };
};
std::vector<PubBinding> g_pubBindings;
std::vector<PubBinding> g_unresolvedPubBinds;   // ROOT H 退避(§3.3)
```

`ActiveItem`(SkinRebind.cpp:73)に `std::shared_ptr<const PubSnapshot> snapshot;` を追加。
Reconcile/InjectFor の per-content 設定参照 6 種を小関数に集約し、snapshot 非 null なら
スナップショット側を読む(§8)。

装備検知で binding が立つと、その actor の `ActorState.items` にスナップショット contents から
`ActiveItem` 群を登録する:

```cpp
void RegisterBindingItems(ActorState& s, const PubSnapshot& snap, RE::FormID tokenForm)
{
    auto sp = SnapshotPtr(snap.pubSlot);            // shared_ptr(カタログ所有)
    for (const auto& id : snap.contents) {
        std::uint32_t lid; std::string plg; ModelRef m3p, m1p;
        if (!ParseColonId(id, lid, plg)) continue;
        RE::Actor* actor = ResolveActor(s);
        const RE::SEX sex = EffectiveSexOfSnap(actor, id, *sp);   // snap.genderModes 参照版
        if (!ResolveArmaModelsFor(lid, plg, sex, actor->GetRace(), m3p, m1p)) continue;  // §2.8
        Register(s, id, m3p, m1p, /*tokenId=*/PubTokenColonId(snap.pubSlot), tokenForm, sex, sp);
    }
}
```

登録後は **Phase 0 の ReconcileActor が全部やる**(worn ゲート・hide-when-worn・sex 再解決・
dead-bind・real-body・morph)。binding 解除(recall/unpublish/トークン喪失)で
`DetachSkinned(s, id)` 相当+items から除去。

### 7.3 イベントハンドラ(§4 の task 着地点)

```cpp
void OnNpcTokenEquip(RE::ActorHandle h, RE::FormID base, bool equipped)   // main thread
{
    const PubSnapshot* snap = PubByTokenForm(base);
    if (!snap) return;                                  // (Phase 1.5: npr 分岐をここに追加)
    RE::Actor* actor = h.get().get();
    if (!actor) return;
    auto& b = EnsureBinding(snap->pubSlot, actor);      // 表に無ければ追加(+holder=true)
    b.wearer = equipped;
    auto& s = EnsureActorState(actor);                  // SkinRebind 公開(無ければ作成)
    if (equipped) {
        if (CountInjectedNpcs() >= MaxNpcInjected() && !HasItems(s, snap->pubSlot)) {
            NotifyCapExceeded(actor, *snap);            // 注入せず追跡のみ(§9.4 表示)
            return;
        }
        RegisterBindingItems(s, *snap, base);
    }
    ReconcileActor(s);                                  // equipped=false は worn ゲートが隠す
    SyncNpcAbility(actor, *snap, equipped);             // §7.6
}

void OnPublishTokenMoved(RE::FormID base, RE::FormID fromRef, RE::FormID toRef)  // main thread
{
    const PubSnapshot* snap = PubByTokenForm(base);
    if (!snap) return;
    // to 側: actor なら holder 記録(player も 1 actor として扱う)
    if (auto* to = RE::TESForm::LookupByID<RE::TESObjectREFR>(toRef); to)
        if (auto* a = to->As<RE::Actor>()) EnsureBinding(snap->pubSlot, a).holder = true;
    // from 側: actor 解決の成否に関わらず、FormID 一致の binding から holder を落とす
    //(死体消滅/セルリセットでも表が腐らない)。wearer が立ったままなら equip sink が別途落とす。
    DropHolderByFormId(snap->pubSlot, fromRef);
    PruneBinding(snap->pubSlot, fromRef);   // holder も wearer も false なら表から除去
}
```

### 7.4 Publish(box → published、Move)

SMF の box ページからの確定操作(確認モーダル後、AddTask 内 = main thread):

```
Publish(boxToken):
 1. 検証: NpcEspLoaded() / FreePubSlot() あり / box.contents に CanResolveContent(id) が 1 件以上
 2. snap = BuildSnapshot(box):
      contents ← box.contents(canonical 済み)
      settings ← live マップ(g_hideRules 等、BoxStore.cpp:77-120)から該当 id 分を DEEP COPY
      sourceSlot ← TokenSlot(box.token) / armorType ← box.armorType
      manualAbility ← box.ability / label ← box.label / rev = 1
 3. g_published.push_back(snap)                       // カタログ追加
 4. 元 box の解体(順序固定):
      a. WearBoxToken(box.token, false)               // worn なら脱がす(BoxStore.cpp:2723)
      b. 各 content: DetachSkinned(id)                // player の注入を落とす(登録ごと)
      c. RemoveBox(box.token)                         // 定義+json(BoxStore.h:380。preset は
                                                      //  ここで自然に解放 = PLAN §4 の preset 切離し)
      d. GiveOrRemoveToken(box.token, false)          // プレイヤーからスロットトークン回収(:2746)
      ※ ResetTokenStats は RemoveBox 側の既存経路が行う(挙動確認の上、無ければ明示呼び出し)
      ※ 隠しストアの実物は動かさない(custody 不変 — PLAN §4)
 5. WriteJson()                                       // published 反映 → :267 WriteCarrierManifest
                                                      //  → published フラグメント → auto-sync が
                                                      //  m/f を焼く → ApplyCarrierOverridesImpl が repoint
 6. StampPublishToken(snap)                           // §7.5
 7. player->AddObjectToContainer(PubTokenArmo(snap.pubSlot), nullptr, 1, nullptr)
 8. Reconcile()                                       // player 側の後片付けを 1 パスで収束
```

### 7.5 トークンスタンプ(ロード毎再適用)

```cpp
void StampPublishToken(const PubSnapshot& snap)   // main thread
{
    auto* token = PubTokenArmo(snap.pubSlot);
    auto* arma  = token && !token->armorAddons.empty() ? token->armorAddons.front() : nullptr;
    if (!token || !arma) return;
    // 1) slot mask(ARMO+ARMA 両方): sourceSlot の単一 bit
    const auto slotFlag = static_cast<RE::BIPED_MODEL::BipedObjectSlot>(1u << (snap.sourceSlot - 30));
    token->bipedModelData.bipedObjectSlots = slotFlag;
    arma->bipedModelData.bipedObjectSlots  = slotFlag;   // S2 スパイクで実挙動確認済みが前提
    // 2) stats: SetTokenStats と同じ集計・写像(BoxStore.cpp:2315-2338 / 2295-2308 を関数抽出して共用)
    //    armorRating = Σ contents GetArmorRating() * 100 / weight = Σ weight /
    //    bipedModelData.armorType = 写像(armorType)
    // 3) keywords: ApplyKeywordsToToken(:2261-2292)を token 引数版に抽出して snap.contents で適用
    //    (KeywordPassThroughAllowed :2222-2238 のフィルタごと共用)
    // 4) fullName(SetTokenStats は触らないので新規。spell->fullName 代入 :2192 が先例):
    token->fullName = ("Costume: " + snap.label).c_str();
}
void ResetPublishToken(int pubSlot);   // unpublish 用: rating/weight 0、keywords クリア
                                       // (ClearTokenKeywords 共用)、fullName="Costume (unpublished)"、
                                       // slot mask はダミー(slot44)へ戻す
```

再適用フック: `LoadBoxes` 末尾(:1268-1283 の SetTokenStats 群と同じ場所)+ kPostLoadGame の
`ApplyCarrierOverrides(false)` 経路(plugin.cpp:166)から全 published 分を `StampPublishToken`。
**フォーム編集は volatile**(セーブされない)なのでこの 2 点で毎セッション収束させる。

### 7.6 abilities(NPC への enchant/manual spell)

- 合成 spell: `BuildEnchantSpell(contents, name)`(BoxStore.cpp:2160-2198)を **snapshot 版**に拡張
  — enchant 参照を `g_contentEnchants` でなく `snap.enchants` から引く(引数に map を渡す形へ
  リファクタし既存呼び出しは live マップを渡す)。キャッシュは `g_pubSpells[pubSlot]`
  (`ClearBoxSpellCache` :2549 と同じくロード時に忘れて再合成)。名前 "Costume Stats (NPC)"。
- 付与/除去 `SyncNpcAbility(actor, snap, worn)`: `SyncSpell`(:2475-2486)を actor 引数版に抽出
  (現行は player 固定引数で既に actor を取る形 — 呼び出し側の player 固定を外すだけ)。
  manual ability(snap.manualAbility)も同時に Sync。
- ライフサイクル: (a) OnNpcTokenEquip(§7.3)、(b) kPostLoadGame の再適用 task で
  「wearer=true かつ actor 解決可」の binding に再付与(dynamic form はセーブに残らないため)、
  (c) recall/unpublish で Remove。**unloaded actor への Add/Remove はスパイク S6 の結果が出るまで
  loaded actor 限定**とし、ロード時 sweep(Character::Load3D → ReconcileActor 内)で収束させる。
- マスター OFF(CefEnabled=false): ReconcileActor が worn ゲートより先に全 hide する既存構造
  (:1390 cefOn)に乗るので視覚は自動。spell は ApplyBoxAbilities 相当の NPC 版 sweep を
  Reconcile 後に呼ぶ(worn でも cefOn=false なら Remove — :2513 の `worn = cefOn && ...` と同型)。

### 7.7 Recall(回収)/ Unpublish

```
Recall(pubSlot):                        // NPC ページの [回収](確認モーダル)
 1. unresolved 分(g_unresolvedPubBinds に同 pubSlot)があれば UI で
    「N 件は未解決 — 回収網の外(確認して続行)」を出してから:
 2. for b in g_pubBindings[pubSlot]:
      ref = LookupByID<TESObjectREFR>(b.actorFormID)  // persistent actor は unloaded でも可
      if ref: ref->RemoveItem(token, 99, kRemove, nullptr, player)   // GiveOrRemoveToken :2762 と同型
      if actor loaded: state から items 除去 + ReconcileActor + SyncNpcAbility(false)
 3. player 手持ちへ集約(RemoveItem の宛先 = player)。binding 表・'PUBB' 側該当分をクリア
 4. 表示状態('PUBS' hidden)は維持(再配布に備える)

Unpublish(pubSlot):                     // 前提: 既知 holder ゼロ(UI がゲート)
 1. box 復元: tok = NextFreeToken()(:1992。sourceSlot のトークンが空いていれば優先)
    AddBox(label, tok, "")(BoxStore.h:378)→ contents を順次 AddBox で追加
 2. settings write-back: snap.settings を live マップへ書き戻し。
    **既に live 側に同 id のエントリがある場合は live 優先で skip + 警告ログ**(PLAN §8)
 3. g_published から除去 → WriteJson()(manifest から published フラグメントが消え、
    carriers.json エントリは残るが参照されない — 掃除は不要、次の publish が上書き)
 4. ResetPublishToken(pubSlot) → プレイヤーの publish トークン残数を RemoveItem で全回収
 5. GiveOrRemoveToken(tok, true) で新スロットトークン配布 → Reconcile()
```

---

## 8. NPC 適用の実行時挙動(Reconcile 拡張)

- publish binding を持つ actor の state には §7.2 の `RegisterBindingItems` が `ActiveItem` 群
  (tokenId=publish トークン colon-id、tokenForm=publish トークン FormID、snapshot 付き)を登録。
  以降は **Phase 0 の ReconcileActor がそのまま全部やる**(worn ゲート・hide-when-worn・
  sex 再解決・dead-bind sweep・real-body・morph)。
- **per-content 設定の間接化**: 現行の直接参照 5 箇所を小関数へ集約する:

| 現参照 | 位置 | 置換 |
|---|---|---|
| `HideSlotsFor(it.id)` | SkinRebind.cpp:1402 | `HideSlotsForItem(it)` |
| `EffectiveSex(it.id)`(genderMode) | :1419 | `EffectiveSexOfItem(actor, it)` |
| `ShouldApplyBodyMorph(a_id)`(bodyMorph) | :904 | `it` 経由に引数変更 |
| `HideShapesFor(a_id)` | :907 | 同上 |
| `ShowRealBodyOn(it.id)` | :1446 | `ShowRealBodyOnItem(it)` |

  各小関数は `it.snapshot ? スナップショット側マップ : 既存 BoxStore accessor` を返すだけ。
  enchant(6 種目)は注入経路でなく ability 合成(§7.6)での参照なのでここには出ない。
- **sex 再解決**(:1418-1430)が snapshot 経由になることで、「男性 NPC が着る→male bake、
  forced-gender content は強制側」が自動成立(carrier 側は §6 の M/F 別ファイルでエンジンが選択、
  注入側はこの EffectiveSexOfItem が選択 — 両者の判定則は同一に保つこと)。
- **キャップ**: `CountInjectedNpcs()` = items 非空の非 player state 数。`MaxNpcInjected()` =
  settings.json npcConfig(既定 8)。超過時は §7.3 の通り追跡のみ(注入もキャリアも動かさない
  — トークンは worn でも視覚なし。NPC ページに "cap exceeded" 行を出す §9)。
- **1p**: publish トークン ARMA は 1p モデル空(§5.1)+ NPC state は firstPersonRoot=nullptr
  (§2.3)なので、プレイヤーが publish トークンを着た場合も 3p のみ表示(仕様)。

---

## 9. UI(SMF NPC ページ+MCM stub+console+診断)

**言語について(PLAN §8 の訂正)**: CEF に言語切替機構は**存在しない**(調査確定)。UI 文字列は
全て英語ハードコードが既存規約(SmfUI.cpp / MCM とも。日本語表示はユーザーが SMF 側の
`SKSEMenuFramework.ini` EnableJapanese+CJK フォントを設定する話で、CEF のコードには関与しない)。
**NPC 機能の新規文字列も英語ハードコードで書く。**

### 9.1 SMF NPC ページ

- 登録: `SmfUI.cpp:784-788` の AddSectionItem 列の "Presets" と "Diagnostics" の間に
  `AddSectionItem("NPC", RenderNpc);` を挿入。`RenderNpc` は `void __stdcall`(既存 5 ページと同型)。
- 描画スレッド規約(SmfUI.cpp:18-25 / Papyrus.cpp:27): **read-only スナップショットのみ**、
  変更は全て `SKSE::GetTaskInterface()->AddTask`。フレーム毎の重い再計算は
  `ImGui::IsWindowAppearing()` でスナップショット(:447-450 のパターン)。
- 構成(上から):

| コントロール | 表示/動作 | native 呼び出し(AddTask 内) |
|---|---|---|
| ESP 不在時 | `TextWrapped`: "The NPC token add-on plugin (CostumeFW_NPC.esp) is not installed. Install it to use NPC distribution." + published 定義が残っていれば "N published definition(s) are dormant." **以降を描画しない** | — |
| published 一覧(CollapsingHeader per entry) | "Pub 01: <label> (slot 47, 3 item(s), AR 25.0)" + 着用者数/未解決数 | 読みは `PublishStore` の snapshot accessor |
| [Show/Hide] トグル | 'PUBS' hidden の反転(全着用者の視覚) | `SetPubHidden(pubSlot, on)` → 各 binding actor へ ReconcileActor |
| [Refresh] | loaded 着用者に unequip+equip サイクル(FSMP 収束の**唯一の**自動 driver) | `RefreshPubWearers(pubSlot)` — WearBoxToken(:2723)の actor 版を各 wearer に |
| [Recall] (確認モーダル) | §7.7 Recall。未解決があれば "N unresolved binding(s) will be dropped" を明示 | `RecallPublished(pubSlot)` |
| [Unpublish] (確認モーダル) | §7.7。holder が残っていれば disabled+理由表示 | `UnpublishToBox(pubSlot)` |
| [Edit contents] | スナップショット編集(§9.5、Phase 1 末尾) | — |
| 着用者リスト行 | "Lydia (loaded, worn)" / "(unresolved FormID 0402A749)" | per-row [Equip]/[Unequip](loaded のみ・ユーザー押下=ユーザー駆動)= `SetNpcTokenWorn(handle, pubSlot, on)` |
| cap 行 | "Injected NPCs: 3 / 8"(超過 binding があれば "+2 tracked (cap exceeded)") | 読みのみ |

- box ページ(RenderBoxes :366)の各 box ブロック末尾に **[Publish for NPC]** ボタン+確認モーダル
  (:319-354 のモーダルパターン)。`NpcEspLoaded()` false 時は `ImGui::BeginDisabled` +
  ツールチップ "Requires the CostumeFW_NPC.esp add-on."。確定で `Publish(boxToken)`(§7.4)。

### 9.2 MCM stub ページ(papyrus/CostumeFW_MCM.psc)

最小 diff 3 点+native 1 本(調査 D7 の手順どおり):
1. `BuildPages()`(:168-188): 配列サイズ `n + 5` → `n + 6`(:170)、固定ページ "NPC" を
   "Presets" の前に挿入。
2. `OnPageReset`(:198-226)に `elseIf a_page == "NPC"` → `ResetNpcStubPage()`:
   ```papyrus
   Function ResetNpcStubPage()
       SetCursorFillMode(TOP_TO_BOTTOM)
       AddHeaderOption("NPC Distribution")
       AddTextOption("NPC distribution is managed in the", "")
       AddTextOption("SKSE Menu Framework (SMF) menu, not in this MCM.", "")
       AddTextOption("Costume Expansion FW's UI is migrating to", "")
       AddTextOption("SKSE Menu Framework going forward; this MCM", "")
       AddTextOption("remains for transition purposes only.", "")
       if !CFW_Native.NpcEspLoaded()
           AddEmptyOption()
           AddTextOption("Note: the NPC token add-on plugin", "")
           AddTextOption("(CostumeFW_NPC.esp) is not installed.", "")
       endif
   EndFunction
   ```
   (SkyUI の AddTextOption は 1 行幅が狭い — 実機で折返しを見て行分割を調整)
3. `CURRENT_VERSION`(:13)を 22 → 23(OnVersionUpdate :98-102 が Pages を再構築し既存セーブに
   新ページが出る)。
4. native: `CFW_Native.psc` に `Bool Function NpcEspLoaded() Global Native` を追加し、
   `Papyrus.cpp` の `RegisterPapyrus`(:926-1015)へ
   `a_vm->RegisterFunction("NpcEspLoaded", kClass, NpcEspLoadedNative);` を 1 行追加。
5. コンパイル(HANDOVER §8.6d :509-520 のコマンド。**リポジトリ papyrus/ を import 先頭に**
   置く罠に注意)→ pex を配備フォルダ Scripts へ。

### 9.3 `cef pub` コンソール(Commands.cpp)

追加手順は既存レシピ(調査 B5)どおり 5 点セット: `HandleConsoleCommand`(:111-405)へ
`else if (sub == "pub")` 分岐、usage 2 箇所(:116, :403)、Commands.h:12-17 のコメント更新。

```
cef pub                → published 一覧(slot/label/holder/wearer/unresolved/cap)を Print
cef pub refresh <n>    → RefreshPubWearers(n)(AddTask)
cef pub recall <n>     → RecallPublished(n)(AddTask)
cef pub diag           → TokenPool 件数(box プール混入チェック §7.1)+ IsPublishToken 表 +
                         binding 表 dump(ログへ)
```

### 9.4 Diagnostics(DiagLines、BoxStore.cpp:1597-1689)

`"# Persist"` セクションの後に追加(`out.push_back("# NPC")` — セクションヘッダ規約は
BoxStore.h:194):

```
# NPC
addon esp: loaded | NOT LOADED
published: 3 (dormant: 0)         ← ESP 不在時は dormant 数
bindings: 4 wearer(s), 6 holder(s), 1 unresolved
injected: 3 / 8 (cap)
pub 01 'Maid Outfit': rev 3, carrier m=r3 f=r3, 2 wearer(s)   ← carriers.json published から
```

SMF Diagnostics ページ(RenderDiagnostics :756)は DiagLines を流すだけなので無改修で反映。

### 9.5 内容物変更(Edit contents、Phase 1 末尾 — 遅延可)

- NPC ページの publish エントリ内に box 内容編集 UI の縮小版(contents 一覧+
  [Add from inventory](InventoryArmors :1892 を流用 — publish トークンは §7.1 のタダ乗りで
  既に除外される)+ [Remove])を出す。**操作対象は snapshot**(live マップに触らない)。
- 確定で: snap.rev++ → `WriteJson()` → manifest published 差分 → auto-sync 再焼き →
  repoint → UI に "Wearers need a Refresh to pick up the new carrier." を表示(自動 re-equip は
  しない — §14-2)。
- 迂回路(recall → unpublish → 編集 → 再 publish)が §7 だけで成立するため、本節は
  スケジュール圧力時に 1.x へ送ってよい(PLAN §10)。

---

## 10. Phase 1.5 — NPC persist 実装

publish(§5-§9)の機構を最大限再利用する。差分だけを列挙:

### 10.1 データ

```cpp
struct NprAssignment {                       // co-save 'NPRS' と 1:1(§3.4)
    int poolSlot;                            // 0-7 = CFW_NpcPersistTok01..08
    RE::FormID actorFormID;
    RE::ActorHandle handle;                  // 遅延解決
    std::vector<std::string> contents;       // 共有 persist カタログの id(DEEP COPY しない)
    RE::SEX bakedSex;                        // 割当時に確定(bake 対象)
};
std::vector<NprAssignment> g_nprAssignments;     // per-save(co-save が真実源)
```
- per-content 設定は **live 共有マップを読む**(publish と逆 — ActiveItem.snapshot を null のまま
  登録するだけで §8 の間接化が自動的に live 側へ倒れる)。
- カタログ削除された id は active-but-uncataloged 扱いで表示維持(M2 §3-6 と同型)。

### 10.2 焼きと manifest

- manifest に `npcPersist`: `[ { "pool": 0, "sex": "f", "contents": [ {id,nif} ] } ]`。
  解決 sex は `bakedSex`(割当時の actor 性別)固定 — **1 本焼き**。
- Sync 側: publish と同じ per-box パイプライン。命名
  `CostumeFW/NpcPersist{NN}_carrier_r{rev%8}.nif` / `XML\NpcPersist{NN}_physics_r{rev%8}.xml`。
  carriers.json `"npcPersist": { "0": { "rev", "file" } }`。
- repoint は既存 `RepointCarrier`(:514-527、両性別同一ファイル)を**無改修流用**。
- **manifest は per-save 状態を反映する**(assignments は co-save 由来)— player persist の
  M2 前例(manifest persist 断片= this save's active set、`SyncPersistManifest` BoxStore.h:185)と
  同じ意味論。セーブ切替で内容が変われば hash 差分で自動再焼き。

### 10.3 割当フロー(NPC ページ persist セクション / console)

```
AssignNpcPersist(actor, contents):
 1. NpcEspLoaded() / FreeNprSlot() / contents 全て CanResolveContent
 2. g_nprAssignments += { slot, actor->GetFormID(), handle, contents, ActorSexOf(actor) }
 3. SyncNpcPersistManifest()               // manifest npcPersist 断片更新 → auto-sync → repoint
 4. actor->AddObjectToContainer(NprTokenArmo(slot), ...) +
    ActorEquipManager::EquipObject(actor, ...)     // 割当クリック=ユーザー駆動の装備
 5. RegisterBindingItems 相当(snapshot=null、tokenForm=NprToken FormID)→ ReconcileActor
```
- 対象取得: (a) NPC ページの **[Use crosshair target]** ボタン =
  `RE::CrosshairPickData::GetSingleton()->target`(コンソール開中は
  `RE::Console::GetSelectedRef()` を優先)、(b) `cef npcpersist add <contentId>`(コンソール
  選択 ref に対して)。`cef npcpersist remove|list` も同レシピで追加。
- 解除 = unequip + RemoveItem + assignments から除去 + ReconcileActor + pool/焼き解放。

### 10.4 worn 自動復元(この機構だけが「自動装備」を許される — PLAN §0 決定)

- EquipSink の npr 分岐: `equipped=false` かつ assignment が生きている場合、
  **30 秒 debounce +連続 3 回で諦めるリトライ上限**付きで re-equip を task 投行
  (outfit リセットとの発振防止 — スパイク S9 の判定基準)。諦めた assignment は
  NPC ページに "auto-restore suspended (re-equip manually)" を表示。
- Character::Load3D / kPostLoadGame の再適用 task でも worn を検査し、脱げていれば同経路で復元。
- マスター OFF 中は復元停止(§6 の cefOn ゲートに従う)。
- **FSMP 収束目的の再装備は引き続き手動のみ**(NPC ページ Refresh)。復元とは目的も
  トリガも別物として実装し、コードコメントにも区別を明記する(§14-2)。

### 10.5 隠しトークンの不可視性

- ESP 側 non-playable フラグ(§5.1)で取引/gift/ルート UI から消える。
- ContainerSink の publish 分岐(§4.2)に npr トークンは**含めない**(移動追跡不要 —
  assignment が真実源。万一外部 mod が剥がしても 10.4 が復元する)。
- `IsCefToken` へ `IsNpcPersistCarrier` を組み込み(§7.1)、hide-when-worn の免除を有効化。

---

## 11. スパイク実験(本実装の前提確認)

| # | 実験 | 手順 | 判定 | fallback |
|---|---|---|---|---|
| S1 | in-proc VFS create 可視性 | in-proc sync スレッドから新規パス `meshes/CostumeFW/_probe.nif` を書き、同セッションで BSModelDB::Demand できるか | 可視→publish 回転ファイルのプリ作成を撤廃可(将来) | 不可視→プリ作成回転を維持(§5/§6 は既定でプリ作成前提) |
| S2 | slot mask ランタイムスタンプ | 汎用トークンの BOD2(ARMO+ARMA)を kDataLoaded 後に書換→装備→表示スロット/競合装備の退避を確認。装備中の書換は不可(必ず未装備状態で) | 装備挙動がスタンプ値に従う | 従わない場合: publish pool を「スロット別プリセット群」に切替(ESP レコード増) |
| S3 | Character::Load3D vfunc | SE/AE/VR で `RE::Character::VTABLE[0]` 0x6A hook を仮組みし、NPC のセル出入りでログ発火を確認(CTD 無し) | 3 ランタイム一致 | 不一致ランタイムは TESObjectLoadedEvent + cell attach に切替 |
| S4 | 同一 pub トークン複数 NPC | 2 体に同一トークンを装備させ、両者の FSMP 揺れ・注入表示・unequip 片側の独立性を確認 | 両立 | FSMP 側の共有制約が出たら「1 トークン 1 着用者」制約を UI で課す |
| S5 | NPC skin 解決マトリクス | ノルド女/男・カジート・アルゴニアン+WNAM 持ち NPC で real-body 注入し肌テクスチャ一致を確認(RMSS 併用環境含む) | 全組で正しい肌 | 獣人で崩れる場合は獣人 real-body を v1 除外し UI 警告 |
| S6 | unloaded NPC への spell | 遠隔セルの binding actor に AddSpell/RemoveSpell を試行 | 安全に成功 or 明確に失敗 | 失敗するなら worn 変化時 loaded actor のみ適用+ロード時 sweep で収束(§8) |
| S7 | メモリ計測 | NPC×3 に注入+morph、30 分プレイで commit 差分を計測。増分は帰属を取ってから判断 | ベースライン比 +2GB 未満 | 超過時は morph の NPC 既定 OFF 化を検討 |
| S8 | マンネキン | マンネキンに pub トークンを与え装備させる | 表示+物理が出れば README 小ネタ | 出なければ「対象外」と README 明記のみ |
| S9 | unplayable 強制装備(1.5) | non-playable ARMO を ActorEquipManager で装備、UI 非表示・outfit リセット後の残留・自動復元ループの発振有無 | 安定装備+復元が 1 回で収束 | 発振するなら復元に debounce(30s)+リトライ上限 |

---

## 12. 検証マトリクス

### 12.1 Phase 0 回帰ゲート(プレイヤー挙動不変の証明)

ビルド刻印確認後、実機で全て green にすること:

1. box: トークン装備→内容表示/解除→非表示。`cef list` の表示が従来同等。
2. persist: 表示継続。head-carrier 持ちセットで save/load →歯が揃っている(mouth watchdog)。
3. hide-when-worn: ブーツ装備でネイル非表示→外すと復帰。
4. RaceMenu: 開閉+性別変更→正しい性別モデルで再注入。
5. セル移動・fast travel・save/load/revert(別セーブ切替)→注入復元・二重表示無し。
6. carrier 再装備で揺れ反映。watchdog: `cef persist` の診断カウンタが動く。
7. body-morph opt-in content の形状追従。real-body 表示 content の肌一致。
8. abilities: box 装備で合成 spell 付与/解除。
9. Character フック: 街 1 周して CTD/ログ異常無し(binding ゼロで no-op のこと)。
10. VR smoke(可能なら): 起動+box 着脱。

### 12.2 Phase 1 受け入れテスト(抜粋・実装後に §6-§9 確定と併せ拡充)

1. publish→トークン付与→ソース box 消滅→スロットトークン解放(新 box 作成可)。
2. gift でフォロワーへ→管理 mod で装備→内容表示+揺れ(Refresh 1 回まで許容)。
3. 男性 NPC に装備→男性側 bake の表示。forced-gender content は強制側。
4. save/load→着用継続。アドオン ESP を外して load→CTD 無し・UI に案内・co-save 温存
   →ESP 復帰で配布状況ごと復活。
5. 回収→全所持者から回収。unpublish→box 復帰。
6. 表示 OFF→全着用者非表示(worn は維持)。
7. maxNpcInjected 超過時の拒否表示。
8. MCM stub 文言表示(EN)。SMF 未導入環境で MCM だけでも案内が読めること。

---

## 13. 教訓の埋め込み(実装中に踏みやすい地雷)

| 地雷 | 由来 | 回避 |
|---|---|---|
| ビルドしたのに直らない | MO2 usvfs の stale DLL | ログ刻印確認を習慣化(§0-2) |
| メモリ膨張を CEF のせいにする | 過去 2 回とも他 DLL | 帰属計測してから疑う(§0-6) |
| morph を全 content に適用 | wig で +15GB 事故 | per-content opt-in(既定 OFF)を publish snapshot でも維持 |
| `HasPartOf` の any-bit 誤用 | slot mask は `.all()` 判定 | slot 判定コードを書く時は既存実装からコピー |
| 自レコードを content 化 | ROOT C | kNpcPlugin を IsTokenColonId 系へ追加(§7 確定後) |
| 未解決 FormID を捨てる | ROOT H / ac78534 | 'PUBB'/'NPRS' の退避ベクタ必須(§3.3) |
| 焼き済み carrier をリリース zip が汚す | v1.3.0 の 85MB 漏洩 | staging は package_assets の pristine プールのみから |
| BSDismember slot 61 中和を忘れる | body-part 調停で消える | InjectOnRoot/RebindGeometry の既存経路を通す(自前で attach しない) |
| 1p で static 判定が狂う | :593 の比較が player 前提 | InjectCtx.firstPersonRoot 方式に従う(§2.3) |
| state 参照のキャプチャ | vector 再配置で dangle | 遅延タスクは handle キャプチャ→FindState 引き直し(§2.5) |

---

## 14. 禁則(実装で「良かれと思って」やってはいけないこと)

1. **facegen head 経路(SkinAllGeometry/ChangeHeadPart/HDPT pool)を NPC に使わない。**
   NPC persist は §10 の armor 経路のみ。理由と決定は PLAN §6b(gray-face/mouth-drop 級の
   脆さ×アクター数、HDPT pool は 1 アクター専用設計)。
2. **FSMP 収束のための自動再装備(unequip/equip サイクル)を書かない。** 収束 driver は
   NPC ページの Refresh ボタンのみ(ユーザー決定)。scene-graph の再注入(rebind retry)は
   自動で良い — 「装備サイクル」と「再注入」の区別を崩さない。
   例外は Phase 1.5 の worn **自動復元**(常時適用指定の維持)のみ(ユーザー確認済み)。
3. **コア CostumeFW.esp にレコードを足さない。**(アドオン分離はユーザー決定)
4. **publish スナップショットを live 設定に「最適化」で共有しない。** deep copy が仕様
   (追跡解除の実体)。逆に Phase 1.5 の NPC persist は共有参照が仕様(ライブ追跡)。混ぜない。
5. **プレイヤー挙動の既定値を「ついでに」統一しない**(例: PlayerSex の kFemale 既定 vs
   real-body reqSex の kMale 既定)。既存挙動の保存が Phase 0 の完了条件。
6. **co-save に box/publish 定義本体を書かない**(定義はグローバル JSON、co-save は per-save
   状態のみ — M2 哲学。Cosave.cpp:56-59 のコメントが契約)。
