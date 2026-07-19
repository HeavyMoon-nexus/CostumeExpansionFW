# NPC サポート設計 — publish for NPC(凍結ボックス配布)

> ステータス: **合意済み(2026-07-18・ユーザー決定)**。実装未着手。
> 起点: [[npc-support-planning]](メモリ・2026-07-18 コードスイープ)→ 本セッションでプラン確定。
> 姉妹: [CEF_STATE_SCOPE.md](CEF_STATE_SCOPE.md)(M2 状態哲学 — 本設計はこれに整合)/
> [NIFCARRIER_INPROC.md](NIFCARRIER_INPROC.md)(in-proc sync — publish 時焼きの基盤)/ HANDOVER.md。
> 前提コード状態: v1.3.1(6877530)。本文の file:line は同時点。

---

## 0. 決定サマリ(2026-07-18 ユーザー決定)

| 論点 | 決定 |
|---|---|
| 配布モデル | **publish for NPC**: box を凍結スナップショット化 → 専用 publish トークンとしてプレイヤーに発行 → **バニラ/装備管理 mod の通常フローで NPC へ**。CEF は配布に介入せず追跡のみ |
| publish 方式 | **Move**: ソース box は通常ページから消え NPC ページ管理へ移行。スロットトークンは解放。回収+unpublish で通常 box に復帰 |
| v1 スコープ | コア(凍結内容注入・FSMP 物理・表示切替・回収・追跡)+ **real-body 注入 + NPC body-morph + enchant アビリティ**。**自動装備アシストは見送り** |
| NPC トークン ESP | **コア ESP(CostumeFW.esp)と統合しない別プラグイン**(アドオン形式・任意インストール)。UI 側は NPC ページを**常設**し、ESP ロードチェックで gate — 未ロード時は「NPC 用トークン ESP が導入されていません」の案内を表示(2026-07-18 追加決定) |
| MCM 側の扱い | NPC 管理ページは追加しない(SMF 主 UI 決定のまま)が、**MCM に stub 告知ページを 1 枚**置き、英語で「NPC 機能は SMF 側」+「**UI は今後 SKSE Menu Framework へ移行する**」を明記(2026-07-18 追加決定・§8) |
| FSMP 収束 | **手動のみ**: NPC ページの Refresh ボタンで再装備サイクル(ActorEquipManager)。自動再装備はしない — プレイヤー側「再装備で反映」のユーザー駆動原則([[plan-y-twin-flipflop]])を NPC にも延長 |
| persist クラス | **NPC にも提供する**(2026-07-18 追加決定)— ただし実装経路は head/facegen ではなく **armor-attach 経路の隠しキャリア方式**(§6b)。head-carrier/mouth 機構はプレイヤー頭部専用のまま([[persist-headcarrier-drops-teeth]])。常時性は **CEF が worn 状態を自動復元**して維持(FSMP 収束の再装備が手動のみである決定とは別機構 — ユーザー確認済み)。実装は **Phase 1.5**(publish コア実機検証後) |

**副産物**: per-actor 化により publish トークンは「凍結コスチュームの携行品」になる —
**プレイヤー自身が装備しても表示される**(万人用の固定衣装トークン。双子コーデは
publish 後に自分で装備すれば成立する)。

## 1. ユーザーフロー(全体像)

1. 通常ページで box を作り content を登録(従来どおり)。
2. box ページの **[Publish for NPC]** → 内容を凍結スナップショット化、両性別キャリアを
   in-proc 焼き、**publish トークン**(別プール)がプレイヤーのインベントリに入る。
   ソース box は通常ページから消える(Move)。スロットトークンは解放され新 box に使える。
3. プレイヤーが publish トークンを**通常のゲームフローで NPC へ渡す**(gift メニュー・
   フォロワー管理 mod・コンソール等。CEF は関与しない)。
4. NPC が装備(管理 mod の force-equip / フォロワー AI / NPC ページの Equip ボタン)→
   equip イベントで CEF が検知 → キャリアを FSMP が育骨 → 凍結内容を NPC の 3D に注入。
5. **NPC ページ**(SMF)で管理: 着用者/所持者リスト・表示 ON/OFF・Refresh(再装備)・
   回収・内容物変更・unpublish。
6. 回収 = 全所持者からトークンをプレイヤーへ引き上げ。unpublish = 通常 box へ復帰
   (スナップショットから box 定義を再生成)。

## 2. 現状のプレイヤー束縛(一般化対象)と追い風

2026-07-18 スイープ(全 file:line 検証済み)より:

| サイト | 現状 | 一般化 |
|---|---|---|
| 注入 | `InjectInternal`/`DetachNodes` が PlayerCharacter シングルトン(SkinRebind.cpp:897-943, :117-143)。**`InjectOnRoot` は既に root 非依存**(:741) | actor 引数を貫通させる |
| レジストリ | `g_active` フラット(content キー・プレイヤー暗黙、:82)。骨ピン `g_boundBoneRefs` も id キー(:114) | **{actorHandle → items}** の per-actor 化。(actor,id) キー |
| Reconcile | `player->GetWornArmor` で worn 判定・hide-when-worn もプレイヤー(:1382-1461) | per-actor 版 `Reconcile(actor)`(worn/hide とも actor の装備で判定) |
| 性別解決 | `EffectiveSex` = PlayerSex + forced-gender(:1181-1199)。キャリアはプレイヤー性別で焼き **ARMA 両性別スロットが同一ファイル**(BoxStore.cpp:437-443, :524-525) | 着用者性別 + スナップショットの forced-gender。**publish は両性別焼き→ARMA の M/F スロットに別ファイル**(エンジンが自動選択) |
| real-body/skin | プレイヤー base/race skin(SkinRebind.cpp:1063-1157) | NPC の WNAM→race skin 解決 + ApplySkinTextures(RMSS 教訓 = base-skin-first、[[rmss-conflict-analysis]]) |
| body-morph | skee へ渡す refr が常にプレイヤー(:886-889) | NPC refr を渡す(slot-gating 教訓維持 — [[bodymorph-on-hair-balloon]]) |
| アビリティ | `SyncSpell(player,…)`(BoxStore.cpp:2502-2523) | 凍結スナップショットから合成した enchant spell を着用 NPC に付与/除去 |
| ライフサイクル | Load3D フックは PlayerCharacter vtable のみ(plugin.cpp:47) | **Character vtable にも同 vfunc(0x6A)を 1 本追加**(binding 保有 actor のみ task 発行) |
| イベント | EquipSink/ContainerSink がプレイヤーフィルタ(plugin.cpp:73-74, :106) | publish トークン分岐を追加(§5) |
| co-save | ACTV/STOR のみ(プレイヤー暗黙) | 'PUBB'/'PUBS' 追加(§7)。未解決保全は ROOT H / ac78534 パターン踏襲 |

**追い風**: 不可視キャリア足場は actor 非依存(エンジンは NPC にも装備させ FSMP は育骨する —
物理の半分は今日動いている)。トークン stats/keywords はレコードレベルで NPC に有効。
in-proc nifcarrier(v1.2.0)で **publish 時のゲーム内焼きが可能**。VR はランタイム同一。

## 3. publish トークン(専用プール)

**既存スロットトークンは使わない。** 理由(いずれも致命的):
- `ReplenishToken`(plugin.cpp:106-110)がトークンのプレイヤー離脱で即補充 → gift と正面衝突。
- キャリアリビジョンファイルはトークン毎のグローバル共有 → 別セーブの live box 編集が
  published の凍結キャリアを上書きする(セーブ間汚染)。
- スロットトークンが publish に取られると通常プールが痩せる。

**設計**: **別プラグイン `CostumeFW_NPC.esp`(名称仮・アドオン)** に **publish プール 8 個**
(ARMO `CFW_PubToken01..08` +各 1 ARMA)。コア ESP には一切レコードを足さない(2026-07-18 決定):
- **分離の帰結**: NPC 機能を使わないユーザーはコア構成が完全不変(ロードオーダー・セーブとも)。
  アドオン単位でクリーンに導入/撤去できる。撤去時の co-save データは未解決保全(§7)で温存。
- **検知**: kDataLoaded で `TESDataHandler::LookupModByName`(+トークン 01 の LookupForm 実確認)
  → `NpcEspLoaded()` を BoxStore/PublishStore に公開。UI・publish 経路・manifest 焼きが全てこれで gate。
- **espmerge 除外**: リリースの same-name fold パイプラインに**含めない**
  ([[release-packaging-traps]] — アドオンはマージ対象外と build スクリプトに明記)。
- ESL フラグ可否(スロット節約)はコア ESP の方針と VR(SkyrimVRESL 前提)に合わせて実装時判断。
- publish プールの参照は plugin 名を定数分離(`kNpcPlugin`)し、既存 `kCarrierPlugin` 系と混ぜない。
- アドオン ESP の全レコード = publish プール 8(本節)+ **NPC persist 隠しキャリア 8(§6b)**。
  以後レコードを足す時もコア ESP には触らない。

publish 時にランタイムスタンプ(SetTokenStats 前例 = BoxStore.cpp:2315 のロード毎再適用パターン):
- **biped slot mask(ARMO と ARMA の両方)** ← ソース box のスロットを複写(体スロット box なら
  NPC の鎧を正しく退かす)
- **armorType/armorRating/weight/keywords** ← スナップショットの集計値(NPC の DR に自動で効く)
- **fullName** ← "Costume: <label>"(インベントリ/gift メニューで判別可能に)

ARMA race list は ESP でバニラ playable 種族+吸血鬼変種を列挙(**child 種族は含めない** —
子供 NPC は構造的に対象外)。カスタム種族 NPC は addon 不一致でキャリア非装着 → 注入は出るが
物理静止(劣化モード、v2 でランタイム race 追記を検討)。1st-person モデルは空(NPC に 1p は
無い。プレイヤーが着た場合も 3p のみで可、必要なら後続で 1p 対応)。

**CEF-own ガードへの編入(正しさ必須)**: publish トークン/新プラグインを既存の
「自分自身を content/対象にしない」ガード群へ漏れなく追加する —
- `IsTokenColonId` / IsTokenPluginFile(ROOT C: CEF 自レコードは content 不可)に kNpcPlugin を追加
- `WornArmors()`/`InventoryArmors()` のキャプチャ候補除外に publish トークンを追加
- hide-when-worn の「自 token は占有と見なさない」判定(SkinRebind.cpp:1408 の IsBoxToken)を
  `IsCefToken`(box ∪ publish)に拡張 — pub トークン着用スロットが他 content を誤って隠さないように

## 4. スナップショット(カタログ)とキャリア焼き

**置き場所 = CEF_settings.json `published[]`(グローバルカタログ)** — M2 哲学どおり
「定義はグローバル・状態は per-save」。フィールド:
`{ pubSlot, label, sourceSlot, contents[], per-content 設定の深いコピー
(hideSlots/genderMode/bodyMorphOn/showRealBody/hideShapes/enchant スナップショット),
armorType, 集計 stats, manualAbility, rev }`

- **深いコピー**が「追跡解除」の実体: 以後 live 側の per-content 設定編集は published に波及しない。
- **preset は publish 時に切り離す**: スナップショットは manual contents 扱いになり、
  preset 名は解放(1 preset ↔ 1 box の排他から抜ける)。unpublish 復帰も manual box として戻る
  (preset 再割当はユーザー操作)。
- **custody(実物)は動かさない**: 隠しストア内の捕獲アイテムは publish/unpublish をまたいで
  そのまま(「凍結 box が中身ごと持っている」心象と一致)。別セーブでは実物なし=複製許容規約
  (CEF_STATE_SCOPE.md §4)どおり。
- **worn 中の box を publish する場合の順序**: live トークンを unequip → 注入 detach →
  box def 削除 → pub トークン付与、を 1 パスで(境界監査の教訓どおり順序を固定し、
  途中状態を Reconcile 1 回に集約)。
- **content id の排他は課さない**: per-actor レジストリ化で registry steal が消えるため、
  同じ content id を新しい live box で再利用できる(publish が素材を永久ロックしない)。
  実物 custody は従来どおり隠しストア(per-save)。別セーブでの回収時実物なしは
  §4 複製許容規約のまま。
- **キャリア焼き**: manifest に `published[]` フラグメントを追加。content の NIF 解決を
  **male/female 両方**(スナップショットの forced-gender 優先、無い側はもう一方へフォールバック)
  で行い、in-proc sync が `PubNN_carrier_{m,f}_r{rev%N}.nif` を焼く → carriers.json に記録 →
  publish ARMA の male スロット→m ファイル、female スロット→f ファイルへ repoint。
  検証ゲート・atomic publish・previous-good 温存・hash skip は既存機構をそのまま通す。
- 回転スロットは維持(エンジン model キャッシュ破り)。**回転数は既存 kSlots=8 を流用**
  (実装仕様で確定 — 回転ロジックを分岐させない。当初案の「回転 4」は破棄)。プリ作成は
  8 pub × 2 sex × 8 回転 = NIF 128 + 物理 XML 128 を package_assets の
  **pristine プールから** staging([[release-packaging-traps]] 厳守)。
  ※in-proc の VFS create は「セッション中即可視」の期待あり(NIFCARRIER_INPROC I-4(b)) —
  Phase 1 スパイクで実測し、可視ならプリ作成を将来撤廃。
- **ESP 未ロード時**: manifest の `published[]` フラグメントは**書かない**(焼きも repoint も
  発生しない)。カタログの published 定義自体は温存 — アドオン再導入で復活する。
- **梱包**: プリ作成キャリアスロット+ESP を**アドオン zip として分離**(VR-Patch の
  分離梱包パターン踏襲、`package_npc_addon.ps1` 相当)。コア zip は不変。

## 5. イベントとライフサイクル

- **EquipSink**: 既存プレイヤー分岐に加え、`IsPublishToken(base)` なら actor 不問で:
  worn 状態を 'PUBB' に記録 → binding 確保 → `Reconcile(actor)` を task。
- **ContainerSink**: publish トークン分岐を追加 — actor インベントリへの出入りで
  所持者(holder)を記録/解除。**Replenish はしない**(publish トークンは旅立つのが仕事)。
- **Character::Load3D フック**(vfunc 0x6A・PlayerCharacter と同型の vtable swap):
  binding を持つ actor のみ Reconcile(actor) を発行(+プレイヤー同様 4 秒後の二次パス)。
- **dead-bind sweep / bind watchdog / rebind retry**: per-actor 化。
  **再注入(scene graph)は自動のまま**(これは装備サイクルではない)。
  **再装備(unequip/equip)は手動のみ** — NPC ページ Refresh ボタンが唯一の駆動点。
  watchdog は静止検知を NPC ページ/Diagnostics に表示するだけ(行動しない)。
- **骨ピン解放**: actor 3D 破棄で NiPointer ピンが古い骨を掴み続けないよう、
  binding sweep(Reconcile 内)で「3D なし actor のピン解放」を追加(プレイヤーには無かった
  新規ライフサイクル)。
- **死亡**: 死体の装備はそのまま = コスチュームも残す(通常挙動)。回収で回る。
- **outfit リセット注意**: 一般 NPC はセルリセットで outfit 再評価がトークンを脱がせ得る
  (バニラ挙動)。推奨対象はフォロワー/管理 mod 下の NPC — README に明記。

## 6. NPC 側適用(v1 フル機能)

- **注入**: スナップショット contents を actor 3D へ(InjectOnRoot 流用)。性別 =
  actor 性別(forced-gender 優先)。alt textures 同経路。
- **real-body**: showRealBody な content があれば NPC の実体身体を注入 —
  skin 解決は WNAM→race skin、**base-skin-first + ApplySkinTextures**(RMSS 修正と同型)。
- **body-morph**: bodyMorphOn(スナップショット)な content のみ `ApplyToNode(actor, holder)`。
  slot-gating(hair/head スキップ・HasPartOf .all())維持。**メモリ監視項目**(§9)。
- **enchant アビリティ**: スナップショットの enchant 集計から合成 spell(動的フォーム・
  セッション毎再生成 = ClearBoxSpellCache パターン)を worn 中の NPC に AddSpell、
  unequip/回収で RemoveSpell。manualAbility も同様に付与。
  ロード時は binding 復元後に idempotent 再付与。armorRating/重量はトークンレコードで自動。
- **hide-when-worn**: スナップショットのルールを actor の装備スロットで判定。
- **同一トークンの複数 NPC 同時着用は許容**(全員同じ凍結内容 — 制服ユースケース)。
- **マスター OFF(CefEnabled=false)は NPC にも波及**: 全 binding の視覚 detach +
  付与 spell の除去(binding 自体は保持 — ON 復帰で再適用)。プレイヤー側の
  「master off = 何も注入しない」不変条件を per-actor にそのまま拡張。
  NPC persist(§6b)の自動復元もマスター OFF 中は停止。

## 6b. NPC persist(常時適用クラス)— Phase 1.5

**位置づけ**: publish = 凍結スナップショットの「配布物」/ **NPC persist = 指定 NPC への
ライブな常時適用**(persist 共有カタログを追跡 — per-content 設定は共有マップ参照で
**深いコピーをしない**。publish と意図的に逆)。プレイヤー persist との対応:

| | プレイヤー persist | NPC persist |
|---|---|---|
| 物理入口 | ②facegen head 経路(方策C・HDPT pool) | **①armor-attach 経路**(隠しキャリア) |
| 発火条件 | equip 非依存(token-less が要件) | 隠しキャリア装備中(CEF が自動維持) |
| 理由 | プレイヤーはスロット節約が本業 = 装備を占有できない | **NPC のスロットはガラ空き** = 1 スロット占有が無害。head 経路の per-NPC 展開は gray-face/mouth-drop 級の脆さ×アクター数+1 アクター専用 HDPT pool の再設計になるため不採用 |
| アクティブ状態 | co-save ACTV(per-save) | co-save 'NPRS'(per-save・actor キー) |

**設計**:
- **隠しキャリア**: アドオン ESP に pool 8 個(`CFW_NpcPersistCarrier01..08` +各 1 ARMA)。
  **unplayable フラグ**(取引/スリ/gift/ルート UI に一切出ない = システムオブジェクト)。
  biped slot は設定の稀スロット(既定案 54。チューナブルの置き場は ini でなく
  CEF_settings.json `npcConfig` — 実装調査で確定した本リポの慣用)。割当操作時に CEF が直接 AddItem+装備
  (割当クリック = ユーザー駆動。publish の「配布はゲームフロー」原則の対象外)。
- **常時性の維持 = worn 自動復元**(2026-07-18 ユーザー確認済み): unequip 検知
  (EquipSink)/ Load3D / cell attach で脱げていれば再装備。**FSMP 収束目的の
  再装備は引き続き手動(Refresh)** — 自動復元は「ユーザーの常時適用指定を維持する」
  状態管理であり、収束リトライとは別機構という線引き。
- **bake**: 割当内容の変更時に in-proc sync。着用者確定のため**性別 1 本焼き**
  (`NpcPersist<slot>_r{rev%8}.nif`)— 既存 `RepointCarrier`(両性別同一ファイル)を
  無改造で流用。回転は kSlots=8 流用・プリ作成はアドオン梱包(NIF 64+XML 64)。
- **データ**: 割当 = co-save **'NPRS' v1**: count × { poolSlot u8, actorFormID u32,
  contentIds[](共有カタログの id 群) }。未解決 actor 保全は 'PUBB' と同規約。
  カタログ削除された id は active-but-uncataloged 表示(プレイヤー persist の M2 §3-6 と同型)。
- **UI(NPC ページ内 persist セクション)**: 割当一覧(NPC 名+contents+状態)/
  対象取得 = **crosshair ターゲット取込みボタン**+console `cef npcpersist <add|remove|list>` /
  content 追加 = persist カタログからピッカー / 解除 = unequip+アイテム除去+pool 解放。
- **適用機能**: real-body / body-morph / enchant アビリティ / hide-when-worn は §6 と同一機構
  (per-actor コアの上に乗るだけ)。
- **制約**: pool 8 = 同時 8 NPC まで(超過は拒否+表示)。maxNpcInjected キャップは
  publish 着用者と合算。物理なし注入のみの「軽量割当」(pool 不要・無制限)は Phase 2 検討。
- **死亡**: corpse 上も継続(通常挙動)。割当解除でいつでも完全撤去。

## 7. co-save スキーマ追加

- **'PUBS' v1**(per-publish の per-save 状態): count × { pubSlot u8, hidden bool(表示 OFF) }
- **'PUBB' v1**(binding): count × { pubSlot u8, actorFormID u32, flags u8(holder|wearer) }
- **'NPRS' v1**(NPC persist 割当・Phase 1.5): §6b 参照 — actor キー+共有カタログ id 群
  - ロード時 `ResolveFormID` で remap。**未解決エントリは保全して書き戻す**(ROOT H /
    ac78534 の未解決 actor 保全パターン)。NPC ページに "(unresolved)" 表示。
  - 0xFF 動的 actor もそのまま通す(セーブ内で有効)。
- 表示 ON/OFF は per-save(M2: 表示状態はアクティブ側)。スナップショット本体はグローバル(§4)。
- **NPC トークン ESP 未ロードのセーブでも 'PUBS'/'PUBB' は読み書きとも温存**(binding の
  actor FormID はアドオン ESP に依存しないが、機能全体が dormant になるだけでデータは
  消さない — アドオンを戻せば配布状況がそのまま復活する)。

## 8. SMF NPC ページ(v1)

- **ページは常設・ESP presence で内容を切替**(2026-07-18 決定):
  - `NpcEspLoaded()` が false → ページ本体の代わりに案内を表示:
    「NPC 用トークン ESP(CostumeFW_NPC.esp)が導入されていません。NPC 配布機能を使うには
    アドオンをインストールしてください。」+ カタログに published 定義が残っている場合は
    「(N 件の publish 定義が休眠中)」を併記。
  - **言語の訂正(2026-07-18 実装調査で確定)**: CEF 側に言語切替機構は存在しない — UI 文字列は
    SMF/MCM とも**英語ハードコード**が既存規約(EnableJapanese は SMF 自身の ini 設定+CJK
    フォントというユーザー側の話で、CEF のコードには関与しない)。NPC 機能の新規文字列も
    英語ハードコードで統一(上記の案内文言も実装は英語 — 確定文は NPC_SUPPORT_IMPL.md §9)。
  - 通常 box ページの **[Publish for NPC]** ボタンも同フラグで gate(未ロード時は
    無効化+同趣旨のヒント)。
- **MCM 側は stub 告知ページを 1 枚**(管理機能は置かない。SkyUI の mod 枠は既存 1 枠のまま —
  ページ追加はコスト外)。表示は英語固定(2026-07-18 決定)、文言案:
  > *"NPC distribution is managed in the SKSE Menu Framework (SMF) menu, not in this MCM.
  > Costume Expansion FW's UI is migrating to SKSE Menu Framework going forward —
  > this MCM remains for transition purposes only."*
  > (NPC ESP 未ロード時は追記): *"Note: the NPC token add-on plugin (CostumeFW_NPC.esp)
  > is not installed."*
  実装は CostumeFW_MCM.psc へのページ 1 枚+ネイティブの `NpcEspLoaded()` 呼び出し(Papyrus 変更は
  この stub のみ)。
- 上記以外の MCM は従来どおり transition-only(SMF 主 UI 決定 — [[smf-integration-facts]])で
  NPC 管理ページは追加しない。
- publish 一覧: label / 元スロット / contents 数 / stats 要約 / 着用者数(未解決数)。
- 行操作: **表示 ON/OFF** ・ **Refresh**(loaded 着用者へ再装備サイクル — FSMP 収束の唯一の自動driver)・
  **回収**(全記録所持者から RemoveItem→プレイヤー。unloaded にも効く。未解決エントリは
  確認付きドロップ)・ **内容物変更**(スナップショット直接編集 → rev++ → 再焼き →
  「着用者は Refresh してください」提示)・ **Unpublish**(回収済みが前提。空きスロット
  トークンへ box 復元 — per-content 設定の書き戻しは live 側に同 id があれば警告)。
- 着用者リスト: 名前(loaded/unloaded)・per-NPC の Equip/Unequip ボタン(loaded のみ・
  ユーザー押下 = ユーザー駆動)。
- **persist セクション**(Phase 1.5・§6b): 割当一覧・crosshair 取込み・カタログピッカー・解除。
- Diagnostics(DiagLines)に NPC ESP presence・publish 状態・binding 健康度
  (watchdog 静止検知)を追加。

## 9. ガード・上限・リスク

| 項目 | 対処 |
|---|---|
| 同時注入 NPC 数 | `maxNpcInjected` 既定 8(置き場は CEF_settings.json `npcConfig` — ini は kill-switch 専用が本リポの慣用)。超過は追跡のみ(注入スキップ+ページ表示)。RaceMenu 0.4.16 リーク(armor churn×actor 数)と FSMP コストが根拠 |
| メモリ | morph/注入の NPC 倍加は計測してから疑う([[fsmp-311-memory-balloon]] の帰属規律)。スパイク S7 |
| リファクタ回帰 | Phase 0 を独立させ、プレイヤー全機能の in-game 回帰ゲート(box 着脱・persist・RaceMenu・セル移動・save/load・VR smoke)を通してから Phase 1 へ |
| ビルド検証 | 常に build 刻印確認([[dll-deploy-lock-build-banner]]) |
| 凍結素材の消失 | content plugin 撤去時は該当 content 非表示+ページに missing 表示(未解決保全) |
| **既知の限界: 迷子トークン** | 回収時に未解決だった actor / コンソール複製のコピーは回収網の外。pub スロットを unpublish→再 publish すると、迷子コピーの着用者は**新しい凍結内容**を表示する(トークン=スロットの物理 ID のため)。緩和: unpublish は「既知所持者ゼロ」前提+未解決ドロップは確認付き+README 明記。Phase 2 で「loaded actor の野良 pub トークン掃討」ユーティリティを検討 |
| ライセンス/梱包 | 変更なし(GPL 構成・pristine staging 規約維持)。アドオン zip はコアと**同版番でロックステップ**リリース |

## 10. フェーズ計画

- **Phase 0 — actor 一般化リファクタ(機能追加なし)**: per-actor レジストリ・actor 貫通
  (Inject/Detach/Reconcile/watchdog/ピン)・Character::Load3D フック・per-actor sweep。
  プレイヤー挙動不変が完了条件(回帰ゲート §9)。
- **Phase 1 — publish コア**: アドオン ESP プール+分離梱包(§3-4)・`NpcEspLoaded()`
  presence 検知と UI/経路の gate(§8)・スナップショット/Move publish・
  両性別焼き+repoint・sink 分岐・'PUBS'/'PUBB'・NPC 適用フル(§6)・NPC ページ(§8)・
  MCM stub 告知ページ(§8・Papyrus 1 枚)・回収/unpublish・Diagnostics+`cef pub`
  コンソールパリティ(DiagLines 共有の既存パターン)。内容物変更(直接編集)は Phase 1 末尾
  (伸びたら 1.x 送り — 回収→unpublish→編集→再 publish の迂回路は初日から成立)。
  **リリース位置づけ(案): コア v1.4.0 + アドオン初版を同時公開**(番号はリリース時確定)。
- **Phase 1.5 — NPC persist(§6b)**: publish コアの実機検証で per-actor 共有機構を固めた後に
  上乗せ。隠しキャリア pool・'NPRS'・worn 自動復元・性別 1 本焼き・NPC ページ persist
  セクション・`cef npcpersist`。同リリース(v1.4.0)に入れるか次版かは Phase 1 完了時に判断
  (2026-07-18 ユーザー決定)。
- **Phase 2 —(順不同)**: 自動装備アシスト(v1 見送り分・per-publish opt-in)・per-NPC 表示切替・
  カスタム種族のランタイム race 追記・publish トークン 1p モデル・LoreBox hover 対応・
  SPID/outfit 経由の大量配布探索・**物理なし軽量 persist 割当**(pool 不要・無制限、§6b)。

### スパイク(Phase 0/1 内で実測)

- S1: in-proc VFS **create** の可視性(プリ作成撤廃可否)
- S2: slot mask ランタイムスタンプの装備挙動(再スタンプ/装備中変更)
- S3: Character::Load3D vfunc index の SE/AE/VR 実測
- S4: 同一 publish トークンを 2+ NPC 同時装備(FSMP 複数インスタンス・model cache)
- S5: NPC skin 解決マトリクス(WNAM/race/獣種族、RMSS 併用)
- S6: unloaded NPC への AddSpell/RemoveSpell 挙動
- S7: NPC×3 注入+morph のメモリ計測(帰属規律つき)
- S8: マンネキン(actor 実装)に pub トークンを着せる — 期待は「そのまま動く」(衣装展示という
  副産物ユースケース)。動けば README の小ネタ、動かなければ対象外と明記するだけ
- S9(Phase 1.5): unplayable ARMO の強制装備挙動(装備可否・UI 非表示の徹底・outfit
  リセットで脱げるか)+ 自動復元ループが暴れないこと(復元⇔リセットの往復検知・
  スロット 54 の実地衝突調査)

## 11. 明示的スコープ外

- **facegen head 経路の NPC 展開**(NPC persist は §6b の armor 経路で提供 —
  head-carrier/ChangeHeadPart/mouth-watchdog/HDPT pool はプレイヤー頭部専用のまま)
- 自動装備アシスト(Phase 2 候補として保留 — 2026-07-18 ユーザー判断)
- FSMP 収束目的の自動再装備(恒久方針: 収束の装備サイクルは常にユーザー駆動。
  §6b の worn 自動復元は「常時適用指定の維持」であり別機構 — 2026-07-18 確認済み)
- CEF 主導の配布 UI(publish の配布はゲームの通常フローに委ねるのが本設計の芯。
  NPC persist の割当操作はページ上のユーザー操作なのでこの原則と矛盾しない)
