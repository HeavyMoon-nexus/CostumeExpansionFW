# MARA 互換問題 — 捕獲ブラックリスト設計・実装計画

> ステータス: **v1.3.2 実装完了 + 敵対的レビュー r2/再レビュー r3 対応済み(2026-07-23)** — 実装記録 =
> [MARA_GUARD_IMPL.md](MARA_GUARD_IMPL.md)(§R = レビュー対応)/検証 =
> [MARA_CRASH_CLASS_AUDIT.md](MARA_CRASH_CLASS_AUDIT.md)/レビュー =
> [MARA_GUARD_ADVERSARIAL_REVIEW.md](MARA_GUARD_ADVERSARIAL_REVIEW.md)(全指摘受理)。
> §3.3 の「ループ先頭 skip」は r2 で「**GetInventory filter 境界**」へ強化、
> allowDynamic 構想は**撤去**(ハード不変条件)、§2-H1 の機序は
> **GetLocalFormID の null deref と確定**(TESForm.h:292-300)。
> リリース判断: §4 の「beta-2 か 1.3.2 hotfix か」は**安定版 v1.3.2 で確定**(報告環境が
> 安定版 v1.3.1 だったため)。beta 線への merge は §8-3 の後続作業。
> 以下は計画時点(2026-07-23 午前)の本文を保存 — 実装との差分は実装記録側が正。
> 起点: [CEF Nexus posts の報告](https://www.nexusmods.com/skyrimspecialedition/mods/183697?tab=posts)
> (InubashiriMomizi・2026-07-22・**CEF v1.3.1 / SSE 1.6.1170**) —「MARA が追加する 'CORE Carrier' が
> 装備中にあると、CFW メニューの Add worn item で即 CTD。ブラックリストが欲しい」。
> **安定版 v1.3.1 での報告 = NPC ベータ固有ではない。**
> 対象コード: branch `nifcarrier-inproc` HEAD `0772ed0`(捕獲経路は main と同一 — 差分は NPC 系のみ)。file:line は同時点。
> 相手 mod: [MARA (Nexus 173949)](https://www.nexusmods.com/skyrimspecialedition/mods/173949)
> v0.0.6(2026-03-23)・author AviationSpring・closed-source("Source will be released once it's
> cleaned and stabilized")。
> 姉妹: [HANDOVER.md](HANDOVER.md)/ [NPC_SUPPORT_PLAN.md](NPC_SUPPORT_PLAN.md)(様式)/ README「Compatibility」。
> CTD 根本調査(クラッシュログ解析)は**後回しで合意済み** — §2 に仮説を格納し、§7 が調査再開の入口。

---

## 0. 報告の読み(確定) — どちらの「Add worn item」か

| 語 | 帰属 | 根拠 |
|---|---|---|
| "Add worn item" | **CEF 自身の UI ラベル**(MCM/SMF の捕獲ボタン) | 原報告が「CFW メニューの Add worn item」と明言。MARA 側に同名 UI は無い(登録は装備で自動、メニューは指選択/アミュレット管理) |
| "CORE Carrier" | **MARA が実行時に作る装備アイテム** | 原報告「MARA adds an item called 'CORE Carrier'」。CEF の ARMO 命名は "Costume Box*" 系のみ。MARA 内部命名 COREInjector/CORESerializer とも整合 |

**シナリオ**: MARA の不可視ホスト装備「CORE Carrier」を着けたまま CEF の
`+ Add worn item` を押す → 捕獲ピッカー(またはピッカー内選択)の処理が
CORE Carrier に触れて即 CTD。**ブラックリスト = CEF の捕獲面に除外機構を持たせ、
Core Carrier(および同類)を既定で弾く機能**として実装する。MARA 側での対処は
不可能(closed-source・除外機構なし §1.2)。

## 1. 動作比較 — CEF と MARA

両者は「装備スロットを消費せずに装飾品を重ね着させる」という**同じ問題を別の方法で
解く隣人**であり、それが接触面を生む。

| 軸 | CEF | MARA v0.0.6 |
|---|---|---|
| 配布形態 | DLL + **CostumeFW.esp**(静的 ARMO トークン群・ESL) | **DLL のみ(MARA.dll)。ESP/ESL なし** → ゲーム内フォームは**全て実行時生成(0xFF)** |
| 見せ方 | 自前 NIF をディスクからロード→clone→骨格へ直接 bind(装備システム非経由)。トークン装備が表示条件 | リングの base mesh を取り「指ごとに remap」。v0.0.6 は **slot 35 のホストに集約**(完全 slotless は engine が「fight」するため断念、0.0.7 で再挑戦予定) |
| 対象決定 | ユーザーが明示捕獲(+ Add worn item / from inventory) | **自動**: slot 35/36 + ArmorJewelry キーワードの装備品を検知して処理 |
| 派生フォーム | 作らない(id = `local:plugin` の colon-id で静的フォームのみ参照) | **動的複製を量産**: "Silver Ring (Left) Misc" "Steel Earring (Right) Stat" 等のリネーム複製 + ホスト "CORE Carrier" |
| 永続化 | co-save + CEF_settings.json(colon-id 前提 = **動的フォームはそもそも表現不能**) | CORESerializer(詳細非公開)。"Fully save-safe" を標榜 |
| UI | MCM(SkyUI) + SMF | MCM なし。INI + PrismaUI(F10)。别作者連携 mod "I4 for MARA" が存在(= 素の inventory UI が混乱する自覚の傍証) |
| NPC | publish for NPC(トークン手渡し) | フォロワー対応(= **CORE Carrier 類は NPC インベントリにも湧き得る**) |
| 除外機構 | 本計画で追加 | **なし**(ポジティブゲートのみ。キーワード剥がしは opt-out にならない — keyword 無し slot-35 品も「悪い形で」触られる報告あり) |
| 周辺実績 | — | クラッシュ報告多数: Community Shaders は **MARA.dll 検出で自己無効化する kill-switch を出荷**、斬首 CTD・特定リング装備 CTD・PrismaUI デッドロック・FSMP 3.5.0 非互換等(bugs タブ実数 61 件・うち未解決 31/2026-07 全数調査 = [MARA_CRASH_CLASS_AUDIT.md](MARA_CRASH_CLASS_AUDIT.md) §1) |

### 1.1 CEF 側の関連機構(file:line 検証済み)

- **捕獲候補列挙**: `WornArmors()`(BoxStore.cpp:2005-2035)/`InventoryArmors()`(:2037-2095)。
  `player->GetInventory(FormType::Armor)` 全件から **CEF 自身のトークンだけを除外**
  (`IsTokenPluginFile` = "CostumeFW" 前方一致、:279-290)。
  **playable・モデル有無・動的フォーム(0xFF)・スロットのフィルタは無い** —
  バニラ UI が non-playable を隠すのに対し、**CEF のピッカーは生インベントリを全部触って
  全部出す**。他 mod が「通常 UI からは見えない前提」で持たせている半構築フォームに
  触るのは実質 CEF のような框架系だけ、という非対称がある。
- **捕獲ゲート**: `CanResolveContent`(SkinRebind.cpp:1651-1660)。MCM
  (CostumeFW_MCM.psc:1248/:1305)と SMF(SmfUI.cpp:113/:148)の両経路が状態変更前に呼ぶ。
  意味論は「ARMA モデルパスが解決できるか」のみ。
  - 動的フォーム: `MakeColonId` の `%06X`/`buf[8]` 切り詰め(BoxStore.cpp:297、**潜在バグ**)
    → 空プラグイン → `LookupForm` 失敗 → **偶然** refuse。明示ガードではなく、
    ゲート到達**前**に別の場所でフォームに触れて死ねば意味を成さない(→ §2 H1)。
  - 実 ESP レコードなら通過して捕獲が進行する(MARA には該当フォーム無しと判明したが、
    同類 mod 一般への防御として意味は残る)。
- **捕獲トランザクション**(MCM .psc:1231-1338 / SmfUI.cpp:106-163 共通):
  ① holder ガード → ② `CanResolveContent` → ③ 登録(json 書込 → **in-proc auto-sync**)→
  ④ `CaptureEnchant`(BoxStore.cpp:2581-2634・全経路 null-safe 実読確認)→
  ⑤ `CaptureItemToStore`(:2855-2888)= **装備中アイテムを `RemoveItem` で隠しコンテナへ剥ぎ取り**。
- **auto-sync(in-proc nifcarrier)**: `RunInProcSync`(BoxStore.cpp:1080-1130)。nifcarrier 側は
  多段ゲートで不適格 content を除外(ディスク解決不可/HDT XML 無し/0 頂点ガード =
  NifCarrierCore.cpp:220-245, :1649-1701)。`catch(...)` はハードウェア例外(AV)を捕まえない点だけ残余。
- **自トークン判定は FormID 完全一致**(SkinRebind.cpp:1472、PublishStore.cpp:329-332)—
  他 mod による CEF トークンの動的複製は CEF から**無反応**で無視される。

### 1.2 MARA 側の要点(公開情報・出典は §9)

- **ESP を持たない** → "CORE Carrier" は**ほぼ確実に 0xFF 動的 ARMO**(静的レコードを
  置く場所が存在しない)。スロットは v0.0.6 の「slotless attachments を slot 35 に集約」
  据え置きが濃厚。1 個のホストか per-item かは非公開。
- 除外/ブラックリスト機構・「この装備は登録するな」API は**存在しない**。
  他 mod から MARA へ「触らないでくれ」と伝える手段が無い以上、**防御は CEF 側に置くしかない**。
- 作者は COREInjector / CORESerializer を再利用可能プリミティブとして公開予定
  ("GEAR" slotless 框架構想)。= **同型のフォームを作る mod が今後増える見込み** →
  名指しでなく**構造で弾く**設計(§3 L1)が将来互換。
- 素性の悪い資産・特殊状況に弱い実績(§1 表の周辺実績)。CS の「検出して自衛」前例は、
  **相手を名指しで防御するのはコミュニティ的に妥当な作法**であることの傍証。

### 1.3 接触面マトリクス

| # | 接触面 | 向き | 帰結 |
|---|---|---|---|
| S1 | **CEF 捕獲ピッカーが CORE Carrier(0xFF・半構築)に触る** | CEF→MARA | フィルタ無しで列挙・表示・選択可 → **本件 CTD**(§2)。**本計画の主対象** |
| S2 | MARA の自動処理が CEF の slot-35/36 box トークンを拾う | MARA→CEF | CEF は VanillaSlots F1 で 35(Amulet)/36(Ring)トークンを出荷済み(HANDOVER.md:863-864)。**ジュエリーを詰めた box はキーワード passthrough で ArmorJewelry を帯び得る** → MARA のポジティブゲート(slot35+ArmorJewelry)に合致し、不可視トークンが MARA に「処理」される(mesh remap・リネーム・equip churn=表示 OFF 誘発)可能性。CTD 報告は無し。**v1 は README で注意喚起に留める**(§6)。恒久策(トークンへの passthrough から ArmorJewelry を除外するか)は §8 オープン |
| S3 | 双方の equip イベント監視の交錯 | 相互 | CEF は全操作を main-thread AddTask に直列化。CEF 側での再入は設計上抑止済み。MARA 側の応答は制御外 — S1 を断てば衝突機会自体が消える |
| S4 | NPC/フォロワー | 相互 | MARA はフォロワーにも CORE Carrier 類を湧かせ得るが、CEF の NPC 経路は自トークンの FormID 一致でしか動かず、NPC インベントリを列挙する UI も無い → 接触面は最小。プレイヤーの `+ Add from inventory` はプレイヤーインベントリのみ |

## 2. CTD 機序の仮説(調査は後回し — 優先度順の格納)

静的解析の結論: CEF の捕獲経路は**自コードとしては全段 null-safe**(列挙・ゲート・
enchant snapshot・sync まで各個検証済み)。ただし null-safe ≠ dangling-safe。

- **H1(最有力・改訂): 列挙/選択処理が MARA の実行時フォームの「通常 UI が決して
  触らないデータ」に触れて AV** — 原報告は「Add worn item で**即**クラッシュ」。
  `WornArmors()` はフォーム毎に colon-id 生成(GetLocalFormID/GetFile)・`GetName()`・
  `entry->IsWorn()`(ExtraDataList 走査)を行う。MARA が装備/リネームを常時弄る
  0xFF フォームのインベントリエントリは、半構築(fullName が一時バッファ・xList が
  MARA 側都合で差し替え中等)であり得る。バニラ UI は non-playable を表示しないため
  この経路を踏むのは框架系だけ、と考えると「MARA は他所で平気なのに CEF ピッカーだけ
  確定 CTD」が説明できる。**この仮説では、既存の「動的フォームは偶然 refuse」も
  到達前に死ぬため無力 — ループ先頭での form-level 事前 skip(§3 L1)が唯一の防御**。
- **H2: 捕獲進行後の ⑤ `RemoveItem` 剥ぎ取りで MARA の装備管理に再入** —
  MARA が常時管理する装備をメニュー内から剥ぐと MARA/エンジン biped 側で死ぬ筋。
  ただし進行には ② 通過(= 静的レコード)が要るため、**「MARA に ESP が無い」判明で
  大きく後退**。同類の静的トークンを持つ他 mod への一般防御として意識だけ残す。
- **H3: ③ 登録 → in-proc nifcarrier が異物 NIF をロード/マージ** — 多段ゲートで除外
  される設計 + そもそも ② を通らないと到達しない。低位。
- **H4: ④ enchant snapshot** — 実読で全経路 null-safe 確認済み。最下位。

> 判定材料は §7.1 のクラッシュログ + コンソール 1 コマンド(`help "CORE Carrier"` →
> FormID が FF 始まりか)で足りる。faulting module が CostumeExpansionFW.dll で
> スタックに WornArmors/GetInventory 系が出れば H1 確定。

## 3. ブラックリスト設計

### 3.1 目標と非目標

- **目標**: (a) CORE Carrier および同類を捕獲面に**出さない/触らない**(CTD の遮断)。
  (b) ピッカー以外の入口(Papyrus native 直呼び・プリセット取込)でも**拒否**。
  (c) ユーザーが CEF 更新なしで**追加登録できる**。
  (d) 名指しに依存しない**構造ガード**で、未知の同類(GEAR 派生等)も先回りで弾く。
- **非目標**: MARA 側 CTD の根治(制御外)。S2(逆方向)の恒久策(§8)。
  既に捕獲が成立してしまった壊れセーブの自動修復(既存 `cef recover` 導線)。

### 3.2 三層構造

| 層 | 内容 | 効く相手 |
|---|---|---|
| **L1 構造ガード(本命)** | ① `armo->GetFile(0) == nullptr`(= 動的フォーム)を**ループ先頭で** skip ② non-playable ARMO を既定 skip(設定で解除可) | CORE Carrier・MARA のリネーム複製群・未知の同類 mod 全部。**CEF の永続化は colon-id(local:plugin)前提で動的フォームを表現できない**ため、① は防御であると同時に意味論的にも正しい(捕獲できてもロード後に必ず迷子になる) |
| **L2 名指しブラックリスト** | 出荷既定(DLL 内蔵)+ ユーザー拡張(`CEF_settings.json`)。**(a) 名前パターン**(例: "CORE Carrier" 完全一致・前方一致)/**(b) プラグイン名パターン**/**(c) colon-id**。動的フォームにはプラグイン名が無いため **(a) が MARA 用の実効層** | ユーザーが踏んだ地雷の即時封鎖。L1 をすり抜ける静的トークン型の他 mod |
| **L3 キーワード opt-out(Phase 3)** | `CEF_NoCapture` キーワード保持 ARMO を拒否。KID で第三者が配布可能(自リポジトリに CostumeFW_KID.ini 運用実績) | 他 mod 作者の自衛登録・コミュニティパッチ(静的フォーム限定 — 動的フォームに KID は載らない点は明記) |

### 3.3 チョークポイントと**順序制約**

1. **列挙フィルタ(最重要)**: `WornArmors`/`InventoryArmors` のループ先頭、既存
   `IsTokenPluginFile` skip(BoxStore.cpp:2026/:2071)の位置に `IsCaptureBlocked(armo)` を追加。
   **順序制約: 判定は form-level の安全な読み(GetFile(0)・playable フラグ・GetName の
   null チェック付き読み)だけで完結させ、skip 決定前に `InventoryEntryData`
   (IsWorn/GetEnchantment/xList)に一切触れない**。H1 が正しい場合、これが唯一効く遮断点。
2. **意味論ゲート**: 新関数 `CanCaptureContent(id)`(= blacklist 判定 → 既存
   `CanResolveContent`)を新設し、MCM(.psc:1248/:1305)/SMF(SmfUI.cpp:113/:148)の
   呼び出しを差し替え。**プリセット検証**(v1.2.1 から捕獲と同一ゲート)と
   **Papyrus native `AddBox`/`AddPersist`(Papyrus.cpp:166-185/:559-573 — 現状ガード無しの
   外部入口)**にも同判定を敷く。
3. **拒否 UX**: ピッカー非表示が既定(黙って隠す — 出せば選べてしまうので)。ゲート側で
   弾いた場合は専用文言("blocked: incompatible/unsupported item")+ `SKSE::log` 1 行
   (名前・FormID・判定層入り — 報告導線)。
4. **検出ログ(triage 補助)**: `kDataLoaded` で `GetModuleHandleW(L"MARA.dll")` を 1 回
   見て `MARA.dll detected - dynamic/utility items are hidden from capture pickers` を
   info ログ。CS 式 kill-switch はしない(CEF の露出はピッカーに限局しており過剰)。

### 3.4 データ形式と永続化

既存 idiom(グローバル配列 = `bodyMorph` 型、BoxStore.cpp:231-235 / WriteJson)を踏襲:

```jsonc
// CEF_settings.json(追加分・ユーザー拡張のみ格納)
"captureBlacklist": {
  "names":   [],        // 名前パターン(既定: 完全一致。"prefix*" で前方一致)
  "plugins": [],        // プラグイン名 前方一致(大文字小文字無視)
  "ids":     [],        // colon-id 個別指定
  "allowNonPlayable": false, // L1-② の解除スイッチ
  "allowDynamic": false,     // L1-① の解除スイッチ(上級者・自己責任。既定 false)
  "disableDefaults": false   // 出荷既定を無効化(上級者)
}
```

- **出荷既定は DLL 内蔵** `kDefaultBlacklist`: names = `{"CORE Carrier"}`(+ 調査で
  増えたら追記)、plugins = `{"MARA"}`(将来 MARA が ESP を持った場合の保険)。
- MARA v0.0.7 で「完全 slotless」に戻る予定 = 実装が動く見込み → **バージョン非依存の
  L1 が主防御、L2 は速報対応**という役割分担を README にも明記。

### 3.5 付随修正(同一 PR)

- `MakeColonId` の `%06X`/`buf[8]` 切り詰め(BoxStore.cpp:294-301)を `%08X` + バッファ
  拡張で修正(0xFF フォームの破損 id が json に入り得る現行バグ。L1 導入後は実害経路が
  塞がるが、id 生成自体を正す)。
- `ParseColonId` の空プラグイン非 reject(SkinRebind.cpp:1269)は L1 導入後到達不能 —
  挙動変更せずコメントのみ。

## 4. 実装手順(Phase 分割)

### Phase 1 — ガード + 既定ブラックリスト(本体・v1.4.0-beta.2 目標)

1. `src/BoxStore.h/.cpp`: `g_captureBlacklist` 構造 + `IsCaptureBlocked(RE::TESObjectARMO*)`
   (form-level 判定のみ)/`IsCaptureBlockedId(const std::string&)`。WriteJson/LoadJson 拡張(§3.4)。
   内蔵既定 `kDefaultBlacklist`。
2. 列挙 2 箇所へループ先頭フィルタ挿入(§3.3-1・順序制約厳守)。
3. `CanCaptureContent(id)` 新設(SkinRebind.cpp)+ MCM/SMF/プリセット/native
   `AddBox`/`AddPersist` の呼び出し面差し替え(§3.3-2)。native 名を変えない実装なら
   .psc 再コンパイル不要 — 要確認、必要なら .pex 再出荷を本 Phase に含める。
4. `MakeColonId` 修正(§3.5)+ 拒否ログ/文言(§3.3-3)+ MARA.dll 検出ログ(§3.3-4)。
5. **リリース**: v1.4.0-beta.2(NPC ベータ継続ライン)。**報告者は安定版 v1.3.1** のため、
   ベータ安定化が長引くなら Phase 1 のみ cherry-pick した **v1.3.2 hotfix** を別途切る
   選択肢を残す(判断はリリース時点のベータ進捗次第 — ユーザー判断)。

### Phase 2 — UI 拡張(小)

- SMF(主 UI)に「Blocked items」開閉セクション: 既定+ユーザー分の一覧表示、
  ピッカーからの「Block this item」追加、ユーザー分の削除。MCM は表示のみ
  (SkyUI 128 枠と SMF 主 UI 決定に整合)。

### Phase 3 — コミュニティ機構(任意)

- L3 `CEF_NoCapture` キーワード判定 + 同梱 KID ini 雛形。README/Nexus に
  「mod 作者はこのキーワードで自分のユーティリティ装備を CEF の捕獲対象から外せる」を告知。

## 5. テスト計画

- **オフライン/ユニット**(既存流儀):
  - `MakeColonId`: 0xFF フォーム相当値で 8 桁安定(修正前後の差分確認)。
  - `IsCaptureBlocked`: names 完全一致/前方一致・plugins 前方一致・大文字小文字・
    ids 一致・allow*/disableDefaults の各スイッチ。
- **実機(MARA なしで可能な分)**:
  - non-playable ARMO(`player.additem` で投入)がピッカー非表示 → `allowNonPlayable` で出現。
  - 回帰: 通常捕獲(worn/inventory × box/persist)・プリセット取込・NPC publish フローが
    blacklist 非該当アイテムで無変化。settings.json 後方互換(旧 json 読み込み)。
  - コードレビュー assertion: skip 決定前に InventoryEntryData 系 API を呼んでいないこと(§3.3 順序制約)。
- **実機(MARA あり — ローカル導入 or テスター依頼)**:
  - CORE Carrier がピッカーに**出ない**こと(L1 で消える。L2 names でも冗長に該当)。
  - `allowDynamic:true` + `disableDefaults:true` で再現環境を作れること(= §7.1 の調査経路)。
    **再現はクラッシュログ収集環境でのみ・任意**。
  - S2 観察: ジュエリー入り slot-35 box を装備して MARA の反応(リネーム/複製/unequip)を記録。
- **VR**: 変更は共通コードのみ。コミュニティテスターのチェックリストに
  「ピッカー表示が従来どおり」の 1 行を追加。

## 6. ドキュメント / リリース物

- README「Compatibility」に MARA 節:
  - 既知問題と対処(CORE Carrier は既定ブラックリストで捕獲対象外)。
  - **slot-35/36 box にジュエリーを入れる場合の注意**(S2 — MARA が box トークンを
    ジュエリーと誤認し得る)。
  - **CEF ネイティブの代替導線**: 複数ジュエリーの「見た目+エンチャント効果」の重ね掛けは
    slot-35/36 box に複数 content を詰めれば CEF 単体で可能(armor/weight/enchant/keyword
    passthrough)。指ごとの配置調整は MARA 固有機能なので、共存したい人向けに上記注意を添える。
- NEXUS_DESCRIPTION 追記 + 次版 changelog + **報告者への返信文(EN)**(§7.1 の依頼を兼ねる)。

## 7. 保留事項(調査再開の入口)

### 7.1 報告者への返信 + 調査依頼(EN 下書き — 送付はユーザー判断)

> Thanks for the report and the crash log. This is MARA's runtime-created host item
> ("CORE Carrier") being picked up by CFW's capture list; a fix that hides such items
> from the pickers is planned for the next update. To pin down the exact crash site,
> two quick things would help:
> 1. In console: `help "CORE Carrier" 4` — does its FormID start with FF?
> 2. Does the CTD hit the moment you press "+ Add worn item" (list opens), or only
>    after you click the CORE Carrier entry in the list?
> Either way the next build will filter it out by default.

- 判定表: FF 確認 + 「ボタン押下で即」→ H1 確定。「エントリ選択後」→ H1(選択時参照)
  or H2 系。faulting module CostumeExpansionFW.dll + スタックに列挙系 → H1。
  クラッシュログ現物は報告済みとのこと — 入手して `WornArmors`/`GetInventory` 系
  シンボルの有無を見るのが最短。
- 併せて **MARA の正確な導入ファイル名**(MARA.dll 版数)を聞けると triage ログ(§3.3-4)の
  文言検証に使える。

### 7.2 MARA 上流への連絡(送付はユーザー判断・source 公開待ちでも可)

> Hi — CEF (Costume Expansion Framework, Nexus 183697) author here. Our capture UI
> enumerates the player's raw inventory, and MARA's runtime "CORE Carrier" item
> crashes the game when inspected (report: CEF posts, 2026-07-22). Next CEF build
> defensively skips dynamic (FF) armors, so the crash is contained on our side.
> Two asks, whenever CORE primitives land:
> 1. A stable, documented way to identify MARA/GEAR runtime forms (fixed name prefix
>    or a keyword), so frameworks can filter them precisely.
> 2. If you add an exclusion mechanism, we'll register our "Costume Box" tokens —
>    they can carry ArmorJewelry via keyword passthrough and sit on slots 35/36,
>    so MARA may currently try to manage them (see §1.3-S2 of our plan).

### 7.3 残余リスク

- in-proc sync の AV 非捕捉(H3 残余)は本計画スコープ外 — NIFCARRIER_INPROC.md(I-5)側で追跡。
- L1 で「動的フォームを捕獲したい」正当ユースを潰す可能性 — 現行アーキテクチャでは
  もともと永続化不能(§3.2)なので理論上ゼロだが、`allowDynamic` スイッチで逃げ道は残す。

## 8. オープン質問

1. CORE Carrier の playable フラグ・1 ホストか per-item か(§7.1 の回答か MARA source 公開で閉じる)。
2. S2 恒久策: box トークンの keyword passthrough から ArmorJewelry(0006BBE9)を
   既定除外すべきか(ジュエリー判定 perk/効果への影響とトレードオフ — 要ユーザー判断)。
3. ~~v1.3.2 hotfix を切るか~~ → **v1.3.2 で確定・実装済み**。残 = mara-guard-v1.3.2 ブランチの
   beta 線(nifcarrier-inproc)への merge + NPC 側 IsDead ゲート(監査 §4.2)。

## 9. 出典(MARA 公開情報)

- MARA mod page: https://www.nexusmods.com/skyrimspecialedition/mods/173949
  (description/files/posts/bugs/images 各タブ。v0.0.6 = 2026-03-23、~34k DL)
- 原報告(CEF posts): https://www.nexusmods.com/skyrimspecialedition/mods/183697?tab=posts
  (InubashiriMomizi・2026-07-22)
- I4 for MARA: https://www.nexusmods.com/skyrimspecialedition/mods/173989
- Community Shaders FAQ(MARA kill-switch): https://modding.wiki/en/skyrim/developers/community-shaders/faq
- JP データベース(日付・JP 報告): https://skyrimspecialedition.2game.info/detail.php?id=173949
- 要旨: ESP なし DLL 単体・slot35+ArmorJewelry ポジティブゲート・除外機構なし・
  COREInjector/CORESerializer("GEAR" 框架)公開予定・source は "released once it's
  cleaned and stabilized"(2026-07-23 時点で未公開・GitHub 無し)。
