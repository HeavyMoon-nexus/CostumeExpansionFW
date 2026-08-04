# PLAN 2026-08-04: per-content "Item data" トグル ＋ ボックスのリネーム

出典: Nexus コメント（durnviir2 / 鹿の報告者とは別人・v1.5.x 系を 8 時間プレイして不具合報告ゼロ）の
機能要望 2 件。過去のやり取りは `nexus_from_durnviir2.txt`。

> If its okay to suggest that is can you add an on/off switch to disable enchantments of specific
> items from enchantments. (中略) there are some outfits that can't remove enchantments sadly or
> don't have unenchanted version of said outfit and a possible update of renaming boxes outfit perhaps?

状態: **実装済み 2026-08-04（npc-mainline dadd18d=B データ層 + 30fbe6a=C/D/E UI・リネーム）。
Nexus 返信は未投稿（§F ドラフトを使用可）。**
実装時の差分: MCM 平置き（§C 後段）は見送り — MCM は移行専用で撤去予定のため SMF 限定とした。
§B の未検証事項（着用中トークンへの即時反映に RefreshWornToken が要るか）は in-game gate で確認:
worn 状態の box で Weight/Armor トグルを切り替え、所持重量/防具値が即時に動くかを見る。
動かなければ再装備で反映される旨を UI 注記するか RefreshWornToken を呼ぶ。

---

## A. 決定事項

1. 付呪だけでなく **重量・防具値も** per-content で OFF にできるようにする。
   （重量は CEF 開発初期に「引き継がれない」問題があって後から足した経緯があるため、
   切りたい人がいる想定。防具値は重量と同一関数・同一ループなので同時に入れる。）
2. トグルが増えて UI が縦に伸びるため、SMF 側は **"Item data" 折り畳み**にまとめる。
3. ボックスのリネーム（label ＋ インベントリ表示名）も同じ更新で入れる。

---

## B. データモデル — per-content フラグ 3 本（既定すべて ON = 現行動作）

`CEF_settings.json` に content id キーで追加。既存の per-content マップ
（`g_hideRules` / `g_genderModes` / `g_bodyMorphOn` / `g_showRealBody`）と同じ寿命・同じ扱い。

| フラグ | 効果 | ゲート位置 |
|---|---|---|
| `statEnchant` | 付呪効果を合成 ability に入れない | `src/BoxStore.cpp:2970` `BuildEnchantSpell` — box と persist の**両方**がここを通る単一チョーク |
| `statWeight` | 重量をトークンに加算しない | `src/BoxStore.cpp:3127` `SetTokenStats` |
| `statArmor` | 防具値を加算しない | 同上（同一ループ内） |

付随して触る箇所（これで全部）:

- 表示 `BoxStatsSummary`（`src/BoxStore.cpp:3428` 付近）— OFF の項目を集計から外す
- NPC publish スナップショット — 重量 `src/PublishStore.cpp:114` / 付呪 `src/PublishStore.cpp:506`
  （per-content 設定は id キーのグローバルなので、公開衣装にも同じ判断が乗る）
- content 削除時の掃除 — `src/BoxStore.cpp:3851` の erase 群に 3 行追加
- settings の read/write（`WriteJson` / ロード側）

トグル反転時の適用は既存の contents-change 経路を再利用:

- 付呪 → `RebuildBoxAbility` / `RebuildPersistAbility` → `ApplyBoxAbilities`
- 重量・防具 → `SetTokenStats`

**未検証**: 着用中トークンへの即時反映に `RefreshWornToken` が要るか（base form 編集はキーワード
passthrough と同性質 — `src/BoxStore.h:157`）。実装時に実機確認。

## C. UI — "Item data" 折り畳み

SMF は `ImGui::CollapsingHeader` がベンダーヘッダにある（`src/external/SKSEMenuFramework.h:4950`）。
畳んだ中身は 3 トグル ＋ **現在値の読み取り専用サマリ 1 行**
（例 `Fortify Destruction 25 | Weight 8.0 | Armor 26`）。値が見えないと何を切っているか分からない。

置き場所:

- box の content 詳細パネル `src/SmfUI.cpp:284` `RenderContentDetail`
  — `Show real body under` の下、`Remove from box` の上
- persist カタログ行 `src/SmfUI.cpp:742` — 既に `TreeNode` で 1 行ずつ畳んであるので内側にネスト

**MCM は SkyUI に折り畳みが無い**ため、フォールドは SMF 限定。MCM は右カラムの content 詳細
（`papyrus/CostumeFW_MCM.psc:621`）にトグルを平置き。MCM は移行専用・撤去予定なので投資しない。

## D. persist の非対称性（死んだスイッチを出さないため）

重量・防具値はトークン ARMO への直書き方式で、トークンを持たない persist クラスには
**元から適用されていない**（`SetTokenStats` は box 専用）。したがって persist 行の "Item data" は
**付呪トグルのみ**、重量/防具はグレーアウト＋注記（「persist はトークンを持たないため重量は元から
加算されません」）。

## E. ボックスのリネーム

半分は既に実装済み:

- `BoxDefInfo.label` と `SetBoxLabel` native は存在し、`src/Papyrus.cpp:973` で登録済み。
  **呼ぶ UI が無いだけ**。label は作成時にトークン名（"Costume Box 32"）で初期化されたまま
  （`papyrus/CostumeFW_MCM.psc:1222`）。
- インベントリ表示名の変更も前例あり: publish 経路が `token->fullName = "Costume: " + label` を
  実施済み（`src/PublishStore.cpp:816`）。同じ volatile form 編集を設定ロード時に再適用する
  （armorRating / keyword passthrough と同じ寿命）。

注意: base form 名なので LoreBox ツールチップ等、表示箇所すべてに波及する。

---

## F. Nexus 返信ドラフト（未投稿・8 行程度）

重量・防具トグルまで広げた版。「an upcoming update」はリリース計画に合わせて差し替える。

> Thank you — a clean 8-hour run is genuinely useful data, and "drip like a balenciaga" made my day. 😄
>
> Both suggestions are doable and both are queued for an upcoming update.
>
> **Item data switches:** you've found a real gap — the passthrough is currently all-or-nothing.
> I'll add per-item switches under a collapsible "Item data" section on each item's page, so you can
> drop an outfit's enchantment and keep everything else. Weight and armor rating get their own
> switches in the same place, since some people want the look without the load. All default to ON,
> so nothing changes unless you touch them, and it's fully reversible — CEF keeps the captured data
> and simply stops feeding it in, so switching it back on restores what you had with no re-capture.
>
> **Renaming boxes:** further along than you'd guess — boxes already carry an internal name, it just
> always defaults to "Costume Box \<slot\>" with no field to edit it. The update will let you rename a
> box, and the name will follow through to the token in your inventory, so it reads as the outfit it
> holds instead of a slot number.
>
> Please do keep reporting — thanks again.

## G. 次アクション

1. Nexus 返信の投稿（USER）
2. 実装（B → C → D → E の順。settings スキーマ追加 → SMF → MCM → リネーム）
3. CHANGELOG / README / BACKLOG への反映
