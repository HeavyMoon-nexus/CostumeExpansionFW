# CEF 公式 API 設計 — F3（他 mod 連携 / preset box の配布・回収）

> 対象: CEF **v1.2.1**（`CostumeFW.esp`・CommonLibSSE-NG・Skyrim SE/AE 1.6.1170）。本書は F3「公式 API」の設計確定版。
> **決定（2026-07-10・ユーザー）**:
> - **API 表面 = Papyrus ファサードのみ**。C++ inter-plugin（SKSE messaging）API は**非採用**（§10 の将来トラック）。
> - **変更範囲 = preset 適用 box の配布/回収 ＋ 読み取りクエリ**。フル遠隔管理（外部から box の作成/編集/削除/capture 全部）は**不採用**。
> - **player 専用**。NPC/セーブ間 per-instance box/自動配布は **F4**（本書のスコープ外・§6 で境界を明記）。
> **前提（全て充足済み）**: 監査 [BORDER_AUDIT_2026-07-09.md](BORDER_AUDIT_2026-07-09.md) の border 硬化（ROOT A/B/C/D/E/G/F/J）は
> HANDOVER §9.2 の Phase 0/1/4 で**解消済み**。F3 はこの硬化した border の上にのみ成立する（後付け不可・§5）。
> **姉妹**: [HANDOVER.md](HANDOVER.md) §9.2（機能ロードマップ）/ [CEF_STATE_SCOPE.md](CEF_STATE_SCOPE.md) §4（複製ポリシー）/ BORDER_AUDIT。

---

## 0. 要約

- **公開面は新規 Papyrus スクリプト `CFW_API`（非 Hidden・自己文書化）1 枚**。消費者 mod はこれだけを叩く。
- 既存 `CFW_Native`（68 natives・`Hidden`）は**内部 MCM 支援面のまま据え置き**。バージョン保証せず自由に churn できる。`CFW_API` が ABI を分離する緩衝層。
- **目玉 = preset 適用 box の配布/回収**: `DistributeBox(slot, presetFile, label) → token` と `ReclaimBox(slot)`。
- **custody の核心決定**: **回収は `returnStored=false`（store-only・鋳造禁止）**。preset/参照 content は「表示参照」であって所有物理アイテムではないため、`returnStored=true` で回収すると**毎回コピーが湧く**（[BoxStore.cpp:2495](src/BoxStore.cpp:2495)）。one-holder-per-id 不変（P0.3）が strand も封じる（§5）。
- **読み取りは slot 番号キー**（最も安定した外部識別子）。token colon-id はロードオーダー依存で脆いので二次的。
- **reactive 連携は mod イベント**（`SendModEvent`）でポーリング不要に（§4.6）。
- 実装は**大半が純 Papyrus ラッパ ＋ 少数の atomic native**（配布/回収/content 増減）。§7。

---

## 1. スコープ

### 1.1 In（本書で設計）
| 区分 | 操作 |
|---|---|
| **配布/回収**（目玉） | 空きスロットに token を確保 → preset 適用 → プレイヤーに token 付与 → 後で回収 |
| **preset 管理** | preset 一覧・box への適用・box からの preset 書き出し |
| **命令的ビルダー**（補助） | 空 box 作成 → 参照 content の増減（preset を使わない構築経路） |
| **読み取りクエリ** | CEF 有効判定・空きスロット・box 有無/token/content 一覧・装備判定・preset 一覧 |
| **イベント** | box 配布/回収/装備/解除の mod イベント通知 |

### 1.2 Out（本書のスコープ外）
- **C++ inter-plugin API**（SKSE messaging・関数ポインタ handshake）→ §10 将来トラック。greenfield（現状 CEF は inter-plugin API を一切公開していない・[plugin.cpp:221](plugin.cpp:221) は inbound の stock SKSE listener のみ）。
- **フル遠隔管理**（外部から MCM 相当の全 mutate: player 実インベントリからの capture、任意 content の store 返却付き削除、ability/stat 直編集）。監査 ROOT A/C/D/E の主経路化リスクが最大のため今回は開けない。
- **NPC 対応 / セーブ間 per-instance box / 自動配布/自動プリセット** = **F4**。§6 に境界。
- **preset ファイルの生成 UI**（既存の `ExportPreset` で足りる）。

---

## 2. 設計原則

1. **狭く硬い facade**: 外部が必要とする操作だけを公開。全 mutate 入口は既存 border ヘルパ（`CanonicalizeColonId`/`IsTokenColonId`）と custody を**内包**し、消費者が custody を間違えられない形にする。
2. **custody を construction で正しく**: 配布は custody 義務を生まない。回収は store-only（鋳造禁止）で複製を構造的に不可能に（§5）。
3. **バージョン付き・安定**: `Int GetAPIVersion()`。破壊的変更でのみ整数を上げる。`CFW_API` の関数シグネチャは安定契約、`CFW_Native` は内部で自由。
4. **slot キー優先**: 外部識別子は biped slot 番号（1 スロット 1 box）。token colon-id はロードオーダー依存で壊れやすいので session 内ハンドルとしてのみ返す。
5. **master-switch 尊重**: 全 mutate native は `CefEnabled()` ゲート（ROOT G）。CEF OFF/uninstall 時は no-op。
6. **player 専用の明示**: NPC は API レベルで拒否（F4 まで）。

---

## 3. なぜ `CFW_Native` をそのまま公開しないか

`CFW_Native` は MCM を動かすための**内部面**であり、公開 API ではない。理由:

- **`Hidden` 宣言**（[CFW_Native.psc:1](papyrus/CFW_Native.psc:1)）: CK に出ない＝「内部」の意思表示。
- **内部表現を露出**: colon-id（`%06X:Plugin.esp`）、box **index**（並び順に依存・ROOT I の stale-index 問題の温床）、MCM ページ状態など、消費者が知るべきでない詳細。
- **バージョン保証なし**: 68 関数は MCM の都合で追加/変更される。外部がこれに直結すると CEF 更新で壊れる。
- **custody を誤れる**: 外部が `RemoveBox(token, true)` を素直に呼ぶと**複製が湧く**（§5.2）。生 native は「正しい呼び方」を強制できない。
- **監査の警告**: F3 は「到達性の低い別経路バグ」を「主経路バグ」に格上げする（HANDOVER §9.2）。生 native を公開＝別経路をそのまま主経路化。**facade で入口を絞ることが緩和策そのもの**。

→ `CFW_API`（非 Hidden）を薄く被せ、公開契約と内部 ABI を分離する。SkyUI/MCM-helper と同じパターン。

---

## 4. 公開面: `CFW_API` Papyrus スクリプト

`Scriptname CFW_API`（**非 Hidden**）。全関数 `Global`。消費者は `CFW_API.Func(...)` で呼ぶ。

### 4.1 バージョン / 発見

```papyrus
Int Function GetAPIVersion() Global      ; 現行 = 1。破壊的変更でのみ +1
Bool Function IsAvailable() Global       ; CEF がロード済みかの軽量判定（内部で GetModByName）
```

**ソフト依存の作法**（消費者側）:
```papyrus
If CFW_API.IsAvailable() && CFW_API.GetAPIVersion() >= 1
    ; CEF がある。API を使う
EndIf
```
CEF 不在時は Papyrus が呼び出しを None 解決するため、`IsAvailable()` ガードで安全に落ちる。消費者は `CostumeFW.esp` をマスターにしない（ソフト依存）。

### 4.2 配布 / 回収（目玉）

```papyrus
; 空き slot に token を確保 → preset 適用 → プレイヤーに token 付与。
; 戻り = 確保した token の colon-id（session ハンドル）／失敗時 ""。
; 失敗理由: slot に空き token なし・preset 解決不能・CEF 無効。ログに詳細。
String Function DistributeBox(Int aiSlot, String asPresetFile, String asLabel) Global

; slot の box を丸ごと撤去し token をプレイヤーから回収。
; content は store-only 返却（鋳造しない＝複製なし・§5.2）。box def と token item を除去。
Bool Function ReclaimBox(Int aiSlot) Global

; def を変えず token item だけ付与/回収（一時貸与用）。box は残る。
Bool Function GiveBoxToken(Int aiSlot) Global
Bool Function TakeBoxToken(Int aiSlot) Global
```

- `DistributeBox` は**atomic native**（§7）: 空き token 確保・box def 作成・preset の Read+Validate+Assign・token 付与を 1 つの border で行う。部分解決（未所持 content mod）は解決可能分のみ適用し、全滅なら "" を返す（`AssignPreset` の挙動を踏襲・[Papyrus.cpp:488](src/Papyrus.cpp:488)）。
- `ReclaimBox` は `RemoveBox(token, returnStored=false)` 相当（[Papyrus.cpp:206](src/Papyrus.cpp:206)）＋ `player.RemoveItem(token)`。**鋳造禁止が要点**（§5.2）。
- `Give/TakeBoxToken` は純 Papyrus: `Game.GetPlayer().AddItem(CFW_Native.ResolveForm(token))` / `RemoveItem`。

### 4.3 preset 管理

```papyrus
String[] Function ListPresetNames() Global   ; CFW_Native.GetPresetNames
String[] Function ListPresetFiles() Global   ; CFW_Native.GetPresetFiles（DistributeBox/ApplyPreset に渡す値）
Bool     Function ApplyPreset(Int aiSlot, String asPresetFile) Global  ; 既存 box に preset 再適用
String   Function ExportBoxAsPreset(Int aiSlot, String asName) Global  ; box → CEFP_<name>.json（戻り = ファイル名）
String   Function GetBoxPresetFile(Int aiSlot) Global                  ; その box に割当中の preset ファイル／""
```

preset ファイルは `Data\SKSE\Plugins\CEF\Presets\CEFP_<name>.json`・schema `cef.preset/1`（[Preset.cpp:19-20](src/Preset.cpp:19)）。消費者 mod は**自作の `CEFP_*.json` を同梱**し、`DistributeBox(slot, "CEFP_Necromancer.json", ...)` で配布できる（宣言的配布）。

### 4.4 命令的ビルダー（補助・preset を使わない構築）

```papyrus
String Function CreateBox(Int aiSlot, String asLabel) Global          ; 空 box 作成 → token 返却／""
Bool   Function AddContent(Int aiSlot, String asArmaColonId) Global   ; 参照 content 追加（border 検証）
Bool   Function RemoveContent(Int aiSlot, String asArmaColonId) Global; content 除去（store-only）
```

- `AddContent` は `IsTokenColonId` 拒否・`CanonicalizeColonId`・one-holder-per-id を通す（[BoxStore.cpp:2592](src/BoxStore.cpp:2592) の AddBox 検証を共有）。
- `RemoveContent` は `RemoveBoxContent(token, content)` の `returnStored=false` 経路（§5.2）。

### 4.5 読み取りクエリ（全て slot キー・mutate なし）

```papyrus
Bool     Function IsCefEnabled() Global               ; master switch（CFW_Native.IsEnabled）
Int[]    Function GetFreeSlots() Global               ; box 未割当の空き token の slot 一覧
Bool     Function IsSlotFree(Int aiSlot) Global
Bool     Function HasBox(Int aiSlot) Global           ; GetBoxBySlot(slot) >= 0
String   Function GetToken(Int aiSlot) Global         ; その slot の box token colon-id／""
String[] Function GetContents(Int aiSlot) Global      ; content colon-id 配列
Bool     Function IsBoxWorn(Int aiSlot) Global        ; token 装備中か
String   Function GetSlotName(Int aiSlot) Global      ; "Head"/"Body"/... 表示名
```

実装は既存の slot キー native の薄いラッパ: `GetBoxBySlot(slot)→index`（[Papyrus.cpp:895](src/Papyrus.cpp:895)）→ `GetBoxContents(index)`/`IsBoxWorn(index)`。空きスロットは `GetFreeTokenIds()`＋`GetTokenSlot()` から導出。

### 4.6 mod イベント（reactive 連携・任意だが推奨）

ポーリング不要にするため、CEF が状態変化時に `SKSE::ModCallbackEvent` を投げる。消費者は `RegisterForModEvent`。

| イベント名 | strArg | numArg | 発火点 |
|---|---|---|---|
| `CFW_BoxDistributed` | token colon-id | slot | `DistributeBox` 成功 |
| `CFW_BoxReclaimed` | token colon-id | slot | `ReclaimBox` 成功 |
| `CFW_BoxWorn` | token colon-id | slot | EquipSink で box token 装備検知 |
| `CFW_BoxUnworn` | token colon-id | slot | EquipSink で解除検知 |

発火は C++ 側（[plugin.cpp](plugin.cpp) の `EquipSink` と配布/回収 native）から `SKSE::GetTaskInterface()->AddTask` 経由でメインスレッド dispatch。

### 4.7 スレッド / 戻り値規約（消費者向け）

`CFW_Native` の契約（[Papyrus.cpp:27-29](src/Papyrus.cpp:27)）をそのまま公開契約にする:

- **読み取りは同期**（VM スレッドで即値）。
- **mutate（配布/回収/content 増減/preset 適用）は `AddTask` で遅延**。戻り `true` = **「受理/キュー済み」**であって「シーン反映完了」ではない。実際の見た目反映は次フレーム以降。
- 従って消費者は「`DistributeBox` が非空を返した＝配布定義は成立」と解釈し、揺れ/表示確定は数フレーム＋（SMP 物理なら）数回再装備を要すると理解する（FSMP 非同期アタッチ・HANDOVER §2）。

---

## 5. custody / セキュリティ契約（監査対応）

F3 の設計で最も慎重を要する部分。**配布/回収は custody 義務を最小化することで複製/紛失をゼロにする**。

### 5.1 配布は custody 義務を生まない
preset/参照 content は**既存の armor レコード（サードパーティ costume メッシュ）への参照 colon-id** であって、プレイヤーの所有物理アイテムではない。`AssignPreset`/`AddContent` は box の content リストにこの参照を積んで Reconcile（メッシュ注入）するだけで、**hidden store への capture を一切行わない**。→ 配布は返却義務を生まない。付与されるのは token ARMO の物理 item 1 個だけ。

### 5.2 回収は store-only（鋳造禁止）— ★複製トラップの核心
`ReturnStoredItem(id, fabricate=true)` は store に無い content に対し **`AddObjectToContainer` で新品を鋳造**する（[BoxStore.cpp:2495-2498](src/BoxStore.cpp:2495)）。preset 配布 box の content は store に入っていない（§5.1）ので、**`ReclaimBox` を `returnStored=true` で実装すると回収のたびに costume ARMA の物理コピーがプレイヤーに湧く**＝item 複製。

> これは監査が警告した「別経路→主経路の格上げ」の実例。`returnStored=true` は一見「F3 の土台」（[Papyrus.cpp:181-185](src/Papyrus.cpp:181) のコメント）だが、それは**フル遠隔管理**（プレイヤー capture 済み実物を外部が削除する）シナリオ用。本 F3 の**配布/回収スコープでは content は参照なので `returnStored=false` が正**。

∴ **`ReclaimBox`/`RemoveContent` は `returnStored=false`（store-only）**:
- store に**本当に**（＝別途 MCM で capture された実物が）あればそれだけ返す。
- なければ**何もしない**（鋳造しない）。参照 content は静かに消える＝正しい。

### 5.3 one-holder-per-id が strand も封じる
「store に本当にある content を落として strand しないか」の懸念は **one-holder-per-id 不変**（P0.3・[BoxStore.cpp](src/BoxStore.cpp) の ingest 検証）が封じる: 1 つの content id は 1 box/persist にしか属せない。F3 が参照として配布した content を、プレイヤーが同時に別 box へ capture することは不可能（ingest で弾かれる）。従って F3 配布 content が store に実物を持つ状況は発生せず、`returnStored=false` で strand は起きない。

### 5.4 監査 ROOT 対応表（F3 が各硬化に乗る形）

| ROOT | 硬化（済） | F3 API がどう乗るか |
|---|---|---|
| **A** custody | custody を C++ 層へ（`ReturnStoredItem`/`SetStoreRef`） | 配布は義務なし・回収は store-only（§5.2）。one-holder が strand 封鎖 |
| **B** JSON ingest | `Preset::Read` typed + try/catch | preset ファイルは `Preset.Read+Validate` を通過（[Preset.cpp:210](src/Preset.cpp:210)）してのみ適用 |
| **C** token 検証 | `IsTokenColonId` | `DistributeBox` は slot の CEF 空き token を確保（外部が任意 ARMO を token 指定できない）。content は Validate が CEF-own id を reject |
| **D** colon-id 正規化 | `CanonicalizeColonId` | 全 native 入口で正規化 |
| **E** native ゲート | 永続書込ゲート | 全 mutate native が `CefEnabled()` 早期 return |
| **G** master switch | `ReplenishToken`/abilities ゲート | 同上・CEF OFF 時 API は no-op |

---

## 6. token pool 制約と F4 境界

- F3 は**既存の出荷済み token pool 内**で動く: バニラ 11 スロット（F1・30/32/33/34/35/36/37/38/40/42/43）＋ 既存カスタム（31/44-60 等）。`TokenPool()` は "CostumeFW" 接頭辞プラグイン × 名前 "Costume Box*" で発見（[BoxStore.cpp:1792](src/BoxStore.cpp:1792)）。
- **1 スロット = 1 token = 1 box**。あるスロットが既に box を持てば `DistributeBox` はそのスロットで失敗（"" 返す・`IsSlotFree` で事前判定可）。
- **pool 枯渇**（全スロットが埋まっている）→ `DistributeBox` は "" ＋ ログ警告。消費者は `GetFreeSlots()` で事前確認すべき。
- **新パターン token の動的生成（同一スロットに複数 box）・NPC への配布・セーブ間 per-instance = F4**。F3 の API はここに踏み込まない。ただし将来 F4 で `DistributeBox` に `targetActor` 引数を**追加**できるよう、v1 は player 固定・スロット単位で設計（前方互換）。

---

## 7. 実装計画（新 native / 純 Papyrus の切り分け）

「Papyrus facade のみ」= 公開面が Papyrus。実装補助として `CFW_Native`（Hidden）に少数の atomic native を追加してよい。

### 7.1 新 native（`CFW_Native` に追加・atomic + custody + 検証）
| native | 役割 | 既存部品 |
|---|---|---|
| `ApiDistribute(slot, presetFile, label) → token` | 空き token 確保 → box def → Read+Validate+Assign preset → token 付与 → イベント | `GetFreeTokenIds`/`AddBox`/`Preset::Read`/`Validate`/`AssignPreset`/`ResolveForm` |
| `ApiReclaim(slot) → bool` | box 撤去（returnStored=false）＋ token 回収 ＋ イベント | `RemoveBox`/`RemoveItem` |
| `ApiCreateBoxOnSlot(slot, label) → token` | 空 box を slot 指定で atomic 作成 | `GetFreeTokenIds`/`AddBox` |
| `ApiAddContent(slot, arma) → bool` / `ApiRemoveContent(slot, arma) → bool` | 参照 content 増減（store-only） | AddBox 検証共有 / `RemoveBoxContent(false)` |

> `AddBox` は content 必須（[Papyrus.cpp:160](src/Papyrus.cpp:160)）なので、空 box は `ApiCreateBoxOnSlot` で atomic 化（空 token 選択レースと空 content 問題を C++ 内で解決）。

### 7.2 純 Papyrus（`CFW_API` 内で既存 native をラップ）
- 全読み取りクエリ（§4.5）: `GetBoxBySlot`→index→各 getter。
- `Give/TakeBoxToken`: `Game.GetPlayer().AddItem/RemoveItem(ResolveForm(token))`。
- preset 一覧・`ApplyPreset`（既存 box）: `GetPresetFiles`/`AssignPreset`。
- `GetAPIVersion`/`IsAvailable`: 定数 ＋ `Game.GetModByName("CostumeFW.esp")`。

### 7.3 イベント発火（C++）
- 配布/回収 native の末尾 `AddTask` で `SendModEvent`（`SKSE::ModCallbackEvent`）。
- `CFW_BoxWorn/Unworn`: 既存 `EquipSink`（[plugin.cpp](plugin.cpp)）で token 装備/解除を検知した箇所に dispatch を 1 行追加。

### 7.4 段階
1. **P-A**: 読み取りクエリ ＋ `IsAvailable`/`GetAPIVersion`（純 Papyrus・native 追加ゼロ）→ reactive mod が即使える。
2. **P-B**: `ApiCreateBoxOnSlot`/`ApiAddContent`/`ApiRemoveContent` ＋ ビルダー facade。
3. **P-C**: `ApiDistribute`/`ApiReclaim`（preset 経路）＋ 配布/回収 facade。
4. **P-D**: mod イベント 4 種。
5. **P-E**: 消費者向けドキュメント（§9）＋ `CFW_API.psc` を配布物に同梱（消費者は `.pex` を参照）。

---

## 8. バージョニングと安定性保証

- `GetAPIVersion()` = **1**。以後、**既存関数のシグネチャ/意味を変える破壊的変更でのみ +1**。関数の**追加は版を上げない**（消費者は存在を前提にせず版で分岐）。
- `CFW_API` の公開関数は**安定契約**。内部 `CFW_Native` は自由に変えてよい（facade が吸収）。
- 非推奨化は「1 版アナウンス → 次版で削除」。CHANGELOG に API 節を設ける。
- 消費者への約束: **slot 番号は安定キー**。token colon-id は session ハンドル（ロードオーダー変更で変わり得る）。

---

## 9. 消費者向けサンプル（クエスト報酬で costume box を配布）

```papyrus
; クエスト mod が「ネクロマンサーのローブ」costume を報酬として渡す例。
; 同梱: Data\SKSE\Plugins\CEF\Presets\CEFP_Necromancer.json（slot 44 の box body に costume ARMA 参照）

Function GiveNecromancerReward()
    If !CFW_API.IsAvailable() || CFW_API.GetAPIVersion() < 1
        Debug.Notification("Costume Expansion FW が必要です")
        Return
    EndIf
    Int slot = 44
    If !CFW_API.IsSlotFree(slot)
        ; 既にそのスロットが埋まっている → 別スロットを探すかスキップ
        Return
    EndIf
    String token = CFW_API.DistributeBox(slot, "CEFP_Necromancer.json", "Necromancer's Robes")
    If token != ""
        Debug.Notification("ネクロマンサーのローブを受け取った（箱を装備して表示）")
    EndIf
EndFunction

; 後で剥奪（クエスト失敗など）
Function RevokeNecromancerReward()
    CFW_API.ReclaimBox(44)   ; box 撤去 ＋ token 回収・複製なし
EndFunction

; 反応: box を装備したら BGM を変える等
Event OnInit()
    RegisterForModEvent("CFW_BoxWorn", "OnCefBoxWorn")
EndEvent
Event OnCefBoxWorn(String asToken, Float afSlot)
    If afSlot as Int == 44
        ; ネクロマンサー装備中の演出
    EndIf
EndEvent
```

---

## 10. 未決事項・将来拡張

- **C++ inter-plugin API**（今回非採用）: 需要が出たら CommonLib 流の query-message handshake（`InterfaceExchangeMessage` 相当）で `CFW_API` を写像した関数ポインタ struct を公開。CEF は現状 outbound（skee 消費・[BodyMorph.cpp:29](src/BodyMorph.cpp:29)）のみで listener 側は net-new。**Papyrus facade を先に安定させてから写像するのが順当**。
- **F4 前方互換**: `DistributeBox`/`ReclaimBox` に `targetActor` を後方互換で追加（デフォルト player）。NPC custody・セーブ間 per-instance は F4 の状態設計（CEF_STATE_SCOPE 系）を要する。
- **preset の requiredPlugins 事前判定**: `DistributeBox` 前に消費者が「必要 costume mod が入っているか」を問える read（`PresetResolvable(file) → bool`）を P-C で追加検討。
- **フル遠隔管理**（今回非採用）: もし将来開くなら、`returnStored=true` 経路（capture 済み実物の外部削除）を**別 API（例 `CFW_ManageAPI`）として分離**し、監査 ROOT A の主経路化を明示的に受け入れた上で custody を再設計する。配布/回収 API と混ぜない。

---

*作成 2026-07-10。F3 設計確定（Papyrus facade / 配布・回収＋読み取り）。実装は本書 §7 の段階に従う。*
