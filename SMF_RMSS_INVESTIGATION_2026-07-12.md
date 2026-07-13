# 調査: ① MCM→SKSE Menu Framework 両対応 / ② RaceMenu Selector of Skins 互換（2026-07-12）

> 対象: CEF v1.2.1（nifcarrier-inproc ブランチ時点）。Nexus コメント起点の 2 要望の実装可否調査。
> 結論: ①②とも**実現可能**。② は競合点を 1 箇所（Show real body の skin 解決）に特定、**本体取り込み**（別パッチ mod 不成立）。
> **決定（2026-07-12・ユーザー）**:
> - **① は「SMF がメイン・MCM は移行期間限定」**。恒久的な両立はしない。理由: SkyUI MCM には登録 mod 数上限（128 枠）があり、更新の見込めない古い mod のために枠を空けるべき — CEF が 1 枠を恒久占有しない。調査時の「実行時共存」推奨は**移行期間の形として採用**し、最終的に MCM を撤去する。
> - **② は本体取り込みで即実装**（本書 §2.3 の 2 修正・同日実装済み）。

---

## ① SKSE Menu Framework（SMF）両対応

### 1.1 SMF の事実（1 次ソース確認済み・2026-07-12 時点）

- **配布/保守**: [Nexus 120352](https://www.nexusmods.com/skyrimspecialedition/mods/120352)。最新 **v3.13-Hotfix2（2026-07-09）**・7,437 endorsements・788k DL。現行リポジトリ [QTR-Modding/SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)（**LGPL-2.1**。v1 は MIT）。活発に保守中。
- **統合モデル**: ヘッダオンリー `SKSEMenuFramework.h`（v3・~520KB）。全呼び出しが実行時 `GetModuleHandle("SKSEMenuFramework")` + `GetProcAddress`。**リンク依存なし・SMF 不在なら全 API が no-op**。`IsInstalled()` = `Data/SKSE/Plugins/SKSEMenuFramework.dll` の存在チェック。真のソフト依存。
- ⚠ **CEF 固有の罠**: v2 ヘッダは static 初期化でモジュールハンドルを 1 回だけ捕獲する。SKSE は DLL をファイル名順にロードし **CostumeExpansionFW.dll は SKSEMenuFramework.dll より先** → v2 ヘッダでは永久に null。**v3 ヘッダを使い、登録は kDataLoaded 以降**（[plugin.cpp](plugin.cpp) の `OnMessage` kDataLoaded が適所）。
- **API**: `SetSection("Costume Expansion FW")` / `AddSectionItem("page", renderFn)`（`"Folder/Item"` ネスト可）/ `AddWindow`。ImGui 1,396 関数を cimgui 経由で再輸出（`ImGui::` でほぼ普通に書ける）。全 mod が単一 ImGui コンテキスト共有 → ウィンドウ名に `"##CostumeFW"` サフィックス必須。
- **スレッド**: render callback はゲームの UI 描画呼び出し内で同期実行（Present hook ではなく RelocationID トランポリン）。公式例は callback 内で直接エンジン API を叩くが、それは既定 pause（`freezeTime`）前提。**CEF は既存規約（mutate = `AddTask` 遅延・[Papyrus.cpp:27](src/Papyrus.cpp:27)）をそのまま維持するのが安全**。
- **ランタイム**: CommonLibSSE-NG・SE 1.5.97 / AE（1.6.1130+）/ VR。Address Library 必須・SSE Engine Fixes も要件欄で必須。**SkyUI 非依存**。
- **操作**: 既定 F1（ini 変更可）。**ゲームパッド対応あり**（既定 LB ダブル押し + D-pad/A/B、ImGui gamepad nav）→ MCM 代替として成立。
- **日本語**: v3 で CJK 対応済み。ただし `SKSEMenuFramework.ini` の `EnableJapanese` は**既定 false**、かつ CJK グリフ入りフォントを `PrimaryFont` に置く必要。JP 翻訳 mod（フォント同梱）が存在。→ CEF の説明文に案内を 1 行入れるだけで足りる（CEF の UI 文字列自体は現状英語）。
- **設定永続化 API は無し**（UI 専用）。消費者が自前保存 = CEF は CEF_settings.json + co-save が既にあり**そのまま使える**。
- **既知の問題**: 3.12-Hotfix で Papyrus デッドロック報告（300+ suspended stacks・3.13 での修正は未確認）→ CEF 側で**対応最低バージョンを明記**推奨。Wheeler/dMenu 等 ImGui 系との衝突報告は見つからず。

### 1.2 CEF への適合性

- CEF の状態は**全て native 層**（costume_boxes.json / CEF_settings.json / co-save）。MCM は薄い UI で、console `cef` コマンドが既に第 2 の UI 面 → **SMF ページは第 3 の UI 面**として自然に追加できる。SkyUI の 128 行制限・`ForcePageReset`・content ページネーション（F2）等の制約は SMF では消滅。
- ただし **MCM psc にしかないロジック**がある: custody 返却（`ReturnItem`/`ReturnItemStoreOnly`/`ReturnDroppedContents`）、隠しストア生成（`GetStore` の `PlaceAtMe 0x80D`）、token equip/unequip、uninstall cleanup、警告ダイアログ。SMF 化にはこれらの **native 化が必要** — これは監査の指摘「検証と custody が Papyrus/MCM 層のみに存在」（HANDOVER §9.4 由来）の解消と同方向で、**移行自体がアーキテクチャ改善**。native 化後は MCM もその関数を呼ぶ薄いラッパに畳める。

### 1.3 移行形態（決定 2026-07-12: SMF メイン・MCM は移行期間限定）

**ユーザー決定**: MCM と SMF の恒久並走はしない。SkyUI MCM の登録 mod 数上限（128 枠）を CEF が占有し続けないため、**メイン UI = SMF、MCM は移行期間のみ残して撤去**する。移行手順:

1. SMF UI をフル機能で実装（下記 P1-P2）— この間 MCM は現状のまま併存（実行時共存はこの期間の形態）。
2. SMF がパリティ到達後、MCM を「移行案内のみの 1 ページ」に縮退（数式リリースの猶予期間・SMF 未導入ユーザーへの案内板）。
3. 最終的に MCM クエスト+psc を撤去。**注意**: 既存セーブにはクエストスクリプトのインスタンスが残るため orphaned-script 警告が出る（無害だが、撤去リリースノートに記載。`Prepare for uninstall` 相当の移行手順を案内）。

以下は調査時の共存パターンの記録（移行期間の設計としてそのまま有効）:

- 実世界に「MCM と SMF を FOMOD で排他選択」する mod は**発見できず**。観測される標準は**実行時共存 / アドオン DLL 型**（DWP Integration 175387・Casting Bar SMF 173070・Oxygen Meter 2 F&A）。両システムは完全独立（MCM=Papyrus/SkyUI、SMF=native ImGui）なので 1 つの DLL が両方へ登録して問題ない。
- **SkyUI が無ければ MCM は現れず、SMF が無ければ SMF ページが現れない** — ユーザーのインストール構成で自然に「選択」が成立し、FOMOD 不要。両方ある環境では両方出るが、同一 native 状態を読むだけなので不整合は起きない（MCM は毎ページ再クエリ設計）。ドキュメントで「SMF 推奨・MCM は維持のみ」を宣言。
- どうしても FOMOD 排他にする場合の正道は **MCM クエストを `CostumeFW_MCM.esp`（espfe）へ分離**してオプション化。espmerge 運用に 1 プラグイン増える + 既存セーブで MCM 消失の注意が要るため、**初期リリースでは非推奨**（要望が出たら）。

### 1.4 段階計画・工数感

| 段階 | 内容 | 規模 |
|---|---|---|
| P1 | v3 ヘッダ vendor・`IsInstalled()` ゲート・Main（有効トグル/依存状態）+ Diagnostics ページ | 小（読み取り中心。フォント/パッド/freeze の実機知見取得を兼ねる） |
| P2 | custody native 化（ReturnItem 系・store 生成・token equip）+ Boxes/Persist/Presets ページ | 中（MCM 1,536 行のうち操作系の移植） |
| P3 | パリティ確認 → MCM 凍結（以後の新機能は SMF 先行）→（任意）MCM 分離 espfe | 小 |

**P1 実装記録（2026-07-12）**: [src/SmfUI.cpp](src/SmfUI.cpp) / [src/SmfUI.h](src/SmfUI.h) 新設・[plugin.cpp](plugin.cpp) kDataLoaded で `SmfUI::Register()`（`IsInstalled()` ゲート・SMF 不在は info ログ 1 行で no-op）。ページ = Main（Enable CEF トグル: Papyrus `SetEnabled` と同一の AddTask 三点セット / Reload settings / 依存状態 / バージョン）・Boxes（読み取り専用ツリー: slot・worn・contents 名）・Diagnostics（`DiagLines()` を Refresh ボタンでスナップショット表示 — 毎フレーム再収集しない）。
- ヘッダは `src/external/SKSEMenuFramework.h`（519KB・**commit f93a729 固定**・`SKSEMenuFramework.h.commit.txt` に記録・取得時に不審コード無しを確認）。
- **判明した罠**: v3 ヘッダの ImGui ラッパは名前空間 **`ImGuiMCP`**（`ImGui` ではない・消費者側の ImGui との衝突回避リネーム）→ `namespace ImGui = ImGuiMCP;` エイリアスで吸収。ヘッダ由来の警告（codecvt 非推奨 C4996 / C4099 / C5054）は include を `#pragma warning(push/disable/pop)` で包んで抑制。
- **ライセンス留意**: v3 リポジトリは LGPL-2.1。ヘッダ vendor の可否は リリース前に作者確認（消費用ヘッダは配布目的のはずだが明文なし）。
- スレッド規約: 読み取りは Papyrus VM と同じ「read-only snapshot 許容」、mutate は全て `AddTask`（CFW_Native と同一契約）。

**P2 実装記録（2026-07-12・P1 実機 OK 後）**: custody native 化 + Boxes フル操作ページ。
- **custody native 化**（監査「custody が Papyrus 層のみ」解消）: 隠しストアを native が所有 — `EnsureStoreRef()`（PlaceAtMe 0x80D・disabled・force-persist・自動生成）＋ **co-save 'STOR' レコードで FormID を per-save 永続化**（`ResolveFormID` remap・Revert で必ずクリア = ROOT A のクロスセーブ防護を維持。旧 plugin.cpp の kPostLoadGame クリアは復元を消す順序バグになるため撤去）。`CaptureItemToStore()`（worn/instance-data ExtraDataList 優先で移動 = 焼き入れ/付呪保持）・`WearBoxToken()`・`GiveOrRemoveToken()`（ActorEquipManager）。`SetStoreRef`（MCM handoff）は **first-wins 採用**に変更: native が生きたストアを持つ場合は MCM の別コンテナを拒否（1 セーブ 1 custody home）。**移行注意**: 既存セーブは初回どちらかの UI で capture/return する前に一度 MCM を開くと旧ストアが native に引き継がれる（開かず SMF で capture すると新ストアが生成され、旧ストア内の既捕獲品は fabricate フォールバック対象になる — 破壊はしないが複製経路。P2b で MCM GetStore の native 委譲により完全統一予定）。
- **UiOps 層**（[src/UiOps.h](src/UiOps.h) + Papyrus.cpp 末尾）: Papyrus native の実装本体（匿名 namespace）への**転送ラッパのみ**を同一 TU から公開。SMF と MCM が同一実装を共有し drift 不可能（AddBox/RemoveBox(Content)/SetBoxEnabled/ArmorType/Gender/HideSlots/HideShape/BodyMorph/ShowRealBody/ScanShapes/Assign・Clear・ExportPreset/FindContentHolder/ContentHasScript）。
- **SMF Boxes ページ**（MCM box ページのフル移植）: + New box（空きスロット picker）／Wear・Distribute トグル／Armor type・Preset combo（drop 分は A-4 どおり store-only 返却）／Export as preset／+ Add worn / + Add from inventory（filter 付き・capture は MCM 順序 = FindContentHolder ガード → CanResolveContent ガード → register → enchant snapshot → store 移動 → token 自動装着）／content 選択 → 詳細（morph・gender・hide-slots・show-real-body・per-shape hide・scan・remove）／Delete box（確認 modal・store-first 返却）。ツリー ID は **slot キー**（index shift 対策 = MCM ROOT I の類推）。combo 内リスト（presets/worn/inventory）は `IsWindowAppearing()` でオープン時スナップショット（毎フレームのディスク/インベントリ走査を回避）。
- 残り（P2b）: Persist ページ・Presets ページ・Uninstall cleanup・MCM GetStore の native 委譲（psc 再コンパイル要）。

**P2b 実装記録（2026-07-13・P2 実機 OK 後）**: SMF が**フル機能到達**。
- **Persist ページ**: worn/inventory capture（holder/resolve ガード + register-first + enchant snapshot + store 移動）・persist preset（drop 分は「pre-assign 時 ACTIVE なら fabricate・それ以外 store-only」の MCM 政策どおり）・Export・カタログ行（active visual-only トグル / morph / gender / hide-slots / remove = active なら fabricate 返却+deactivate ペア、inactive は store-only）・Remove all（P1-4: この save が表示する分だけ返却）・uncataloged actives の [deactivate]（実行時 re-check の stale-row ガード付き）。
- **Presets ページ**: 一覧（assigned 状態: free / Persist / Box N）+ description/author + Assign to box。
- **Uninstall（Main・確認 modal）**: MCM UninstallCleanup の native twin — box contents + ACTIVE persist を fabricate 返却・token 全撤去・DetachAll・`SetCefEnabled(false)` + SetEnabled 三点セット。
  - **セマンティクス（2026-07-13 明確化・ユーザー質問起点）**: uninstall は **custody の解消 + 表示の停止**であって定義の削除ではない。box 定義/persist カタログ/per-content 設定（hide/gender/morph/preset 割当）は CEF_settings.json に**意図的に温存**（MCM review C の決定を踏襲: enabled=false で再適用だけ止める）。だから MCM/SMF の一覧や LoreBox tooltip には定義が残り続ける。**再 Enable の「復元」= 定義と表示の復元のみ**: Reconcile+abilities+carrier が再適用され、Wear トグル（token 不所持なら 1 個自動付与）で衣装表示が戻る。**custody は復元されない**: 返却済みアイテムはプレイヤーの手元のまま・隠しストアは空。以後その content を remove すると fabricate 経路で新品が湧く＝原品と合わせて 1 個複製（CEF_STATE_SCOPE §4 の複製許容ポリシー内・仕様として認識）。定義ごと消す「factory reset」は別機能としてなら追加可能（現状は MCM と挙動を揃える）。
- **MCM GetStore の native 委譲**（psc 再コンパイル・§8.6d レシピ・pex 反映を GetStoreRef 文字列で確認済み）: `CFW_Native.GetStoreRef()` 新設。MCM の GetStore は native ストア優先・PlaceAtMe は初回生成フォールバックのみ（生成直後に `SetStoreRef` で即 adopt）。→ **P2 の「移行注意」（2 ストア分裂）は解消** — どちらの UI から入っても custody home は 1 つ。
- **P2 実機フィードバック反映（2026-07-12/13）**: Wear チェックの楽観表示（equip キューは SMF pause 中に処理されず、正直な毎フレーム再読はチェックを即戻しする — pending map で意図を表示し実状態が追いつき次第解除）／confirm modal の幅固定（auto-size modal 内 TextWrapped は極小幅折り返し → SetNextWindowSize(460, Appearing)）。
- **SMF UI は MCM の全機能をカバー**（Main/Boxes/Persist/Presets/Diagnostics + Uninstall）。次 = P3: パリティ実機確認 → MCM 凍結宣言（以後の新機能は SMF 先行）→ リリース時に SMF 推奨を明記。

リスク: SMF 本体の更新破壊（3.12 例）／LGPL-2.1 ヘッダの vendor 可否（作者に要確認）／render callback 内でのエンジン呼び出し規律。

---

## ② RaceMenu Selector of Skins - Unique Player Character（RMSS）互換

### 2.1 RMSS の仕組み（同梱ソース + esp 実読で確認）

- [Nexus 126721](https://www.nexusmods.com/skyrimspecialedition/mods/126721)・作者 Aix・v0.1.6（2024-08-20、実質更新停止・コミュニティパッチ群が保守）。**DLL なし**: esp 2 枚 + Papyrus 3 本 + テクスチャ。依存は RaceMenu のみ。**プレイヤー専用**。
- **ボディ**（"Body and Skin" スライダー）: SKSE native `ActorBase.SetSkin(Armor)` で**プレイヤー ActorBase の naked-skin ARMO（WNAM）を差し替え** → `QueueNiNodeUpdate()`。esp 内は skin 4 枠 × 男女の `Skin0XNaked[F]` ARMO → `Skin0XNakedTorso/Hands/Feet` ARMA → 男女別 skinTextures TXST（NAM0/NAM1）。skin01 は Unique Character 互換パス（`meshes/actors/character/unique/…` の専用 body NIF + `textures/actors/character/unique/…`）、skin02-04 はバニラ Character Assets メッシュ + `textures/aixbodyselector/skin0X/…` の専用 TXST。
- **顔**（"Face Texture" スライダー）: NiOverride node override（key 9・index 0/1/2/7・persist=true）で顔 headpart ノードのテクスチャを直接上書き。
- **永続化が 3 層で脆い**: スライダー値=Papyrus 変数／顔=SKEE co-save（自動再適用）／**ボディ SetSkin=どこにも保存されない**（`OnReloadSettings`/`On3DLoaded` の再適用だけで維持 = ロード時 last-writer-wins）。既知衝突（RSV 等）は全部ここ。
- `AixSchlongSelector.esp` = TNG/SOS 用オプション（`Skin0XNaked` ARMO を override して genital ARMA+TXST 追加）。

### 2.2 競合分析

- **直接衝突なし**: CEF はプレイヤー skin ARMO を**一切書かない**（読むのは realbody のみ）→ RSV 型「スキン巻き戻し」衝突に CEF は該当しない。RMSS は装備/costume に触れない。
- **3D 再構築の相互作用も安全な見込み**: RMSS スライダー → `QueueNiNodeUpdate` → CEF Load3D フック → Reconcile 再注入（スキン変更に自動追従）。CEF の `DoReset3D` → RMSS `On3DLoaded` 再適用（この経路は `QueueNiNodeUpdate` を呼ばないため**ループしない**）。実機確認 1 回推奨。
- **実競合は「Show real body」1 点のみ**:
  1. **skin 解決順**: `InjectRealBody`（[SkinRebind.cpp:1003](src/SkinRebind.cpp:1003)）が TNG の keyword-only skin 対策（2026-07-10）で **`race->skin` を `base->skin` より優先** → RMSS が設定した WNAM skin を無視し、素の種族ボディを注入（メッシュ・テクスチャとも不一致）。
  2. **skin テクスチャ未適用**: 注入は NIF 埋め込みテクスチャ + alternate texture swap のみで、**ARMA の per-sex skinTextures（NAM0/NAM1 TXST）を適用しない** → skin02-04（バニラメッシュ + 専用 TXST）は修正 1 だけでは色が乗らない。
- **本質的制約（どんなパッチでも不可能な領域）**: costume NIF に焼き込まれた肌はどのスキン mod とも一致しない。CEF の hide shapes + Show real body がまさにその解であり、上記修正後は RMSS 選択スキンがそこに出る。

### 2.3 修正 — 本体取り込み（別パッチ mod は不成立）【実装済み 2026-07-12】

> **実装記録**: 同日 [SkinRebind.cpp](src/SkinRebind.cpp) に両修正を実装・Release ビルド・MO2 mods へ配備済み。
> - 修正 1 = `InjectRealBody` の skin 解決を base->skin 優先（body addon 無しは race->skin へフォールバック）に変更。
> - 修正 2 = 新ヘルパ `ApplySkinTextures`（holder 配下全形状へ `bodyAA->skinTextures[sex]` を既存 `ApplyTextureSet` で適用・3p/1p 両方・idempotent 再適用あり = TXST だけの skin 切替は 3D 再構築なしでも収束）。
>
> **方針確定（同日・ユーザー）: RMSS 連携はマストではない。マスト = この修正が CEF を壊さないこと。** RMSS 固有の処理は CEF に入れない（実際ゼロ — 両修正は「エンジン忠実化」であり RMSS は偶発的受益者）。バニラ種族での RMSS 動作は RMSS 本体とそのコミュニティパッチ群の領分で、CEF は抱え込まない。
>
> **回帰硬化（同日実装）**: 修正 1 の base skin **採用条件を 3 条件に厳格化** — ①slot-32 body addon がある ②それがプレイヤー種族にレースマッチ（RNAM/additionalRaces。エンジンはレース不一致の skin ARMA を描画しないため）③実モデルを持つ（`HasBodyModel`）。1 つでも欠ければ **race->skin の旧経路と byte-identical**（anyBody 寛容も従来どおり）。→ 旧動作との差分は「エンジンが base skin を描画しているのに CEF が race skin を注入していた」誤りの是正だけに絞られた。修正 2 の `ApplyTextureSet` は alt-texture 機能で本番実績のある同一ルーチン（非 LightingShader 形状スキップ・TXST null スキップ = VampireLord で実証済み）。
>
> **実機ログで確認済み（11:37-11:41 セッション）**: TXST 適用が 3p 3 shapes + 1p 1 shape に動作（Succubus skin TXST 70000805）／VampireLord 変身追従＋TXST null スキップ／hide shapes 等の既存機能に変化なし。ログ強化: `realbody: skin '' (FormID, base|race)` で採用チェーンが読める。

- **修正 1（~10 行）**: skin 解決を「`base->skin` に slot-32 body addon（`PickBodyAddon` 非 null）があればそれ、無ければ `race->skin`」へ。エンジン自身の解決順（WNAM 優先）への忠実化。TNG の keyword-only skin は body addon を持たないため従来どおりフォールバック → **TNG 対応と両立**。
- **修正 2（~40-80 行）**: 注入した realbody 形状へ `bodyAA->skinTextures[sex]` を既存 `ApplyTextureSet`（[SkinRebind.cpp:638](src/SkinRebind.cpp:638)）で適用（1st person 含む）。
- **一般性**: RMSS 専用ではなく、vampire 種族 TXST・SOS フル・Unique Player 系・カスタム種族 skin 全般へのエンジン忠実化 → 「小規模かつ CEF 実装に有意義」の本体取り込み基準に合致。**RMSS 側の変更はゼロ**のため、別パッチ mod として出す配布物が存在しない（公開は compatibility notes への記載で足りる）。
- **検証手順**（2026-07-12 実機 1 回目の教訓: `realbody:` ログは「**Show real body が ON の content が box 装備で表示中**」のときだけ出る。hide shapes だけの content（例 Box 47 Marika）では InjectRealBody は呼ばれず、ログ無しが正常）:
  0. 前提: Show real body ON の content の box を**装備**する（現状フラグ ON は `000EFD:[Caenarvon] Cosplay Pack Gala.esp`＝Box 46。または任意 content の MCM 右パネルで ON にする）。装備で `realbody: skin '...' body addon ...` と `realbody: skin TXST ... -> N shape(s)` の 2 行が出ることがまず動作確認。
  1. RMSS 有効 + skin02 選択 → 上記 box 装備 → 注入ボディが skin02 テクスチャになる（ログの skin 名が `Skin02Naked[F]`）。
  2. skin01 で unique メッシュ（meshes/actors/character/unique）が注入される。
  3. TNG 退行なし: keyword-only skin 環境で `base skin '...' has no slot-32 body addon - trying race skin` が出て従来動作。
  4. スライダー変更（QueueNiNodeUpdate → 3D 再構築 → Reconcile）に再注入が追従。
  5. ロード後の再適用順（RMSS OnReloadSettings vs CEF kPostLoadGame Reconcile）で最終表示が正しい。
  - 注: box のスロットが body を隠さない場合（例 slot 47）、素の体は**エンジンが普通に描画**するので RMSS スキンは CEF 無関係に反映される＝そのケースは修正対象外（Show real body を入れると体が二重になる・MCM 警告どおり）。

### 2.4 Nexus コメント返信案

> Thanks! Both are on the radar now. (1) SKSE Menu Framework: investigated and planned — CEF will register an SMF section alongside the existing MCM (soft dependency; whichever you have installed shows up, no separate download needed). (2) Selector of Skins: they already coexist — CEF never touches the player's skin record. The one real gap is CEF's "Show real body" feature, which currently injects the default race body instead of your selected skin; a fix to respect the Selector's skin (mesh + textures) is planned for the next update, in CEF itself — no patch file needed.

---

*作成 2026-07-12。調査のみ（コード変更なし）。1 次ソース: SMF = GitHub QTR-Modding/SKSE-Menu-Framework-3 実コード + Nexus 120352 / RMSS = 同梱 .psc ソース + AixBodySelector.esp 実読 + Nexus 126721 とコミュニティパッチ群。*
