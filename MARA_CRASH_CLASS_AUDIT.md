# MARA クラッシュ監査 — CEF への写像と 3 配布物検証

> ステータス: **監査完了(2026-07-23)**。v1.3.2 対処の根拠文書。
> 問い: 「MARA に寄せられたクラッシュは CEF でも起こり得るか?」
> 方法: ① MARA の公開クラッシュ報告の**全数**列挙(bugs タブ 61/61 件・posts 551 コメント中
> クラッシュ/VR 関連を全捕捉・2game.info JP 11 件)→ ② 7 クラスに写像 → ③ CEF コードの静的検証。
> 検証対象: **[A] v1.3.1 安定版**(`main` @ 6877530)/ **[B] NPC addon**(`nifcarrier-inproc` @ 0772ed0、
> v1.4.0-beta.1)/ **[C] VR patch**(同一マルチランタイム DLL の VR 実行面)。file:line は各コミット時点。
> 姉妹: [MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md)(ブラックリスト計画)/
> [MARA_GUARD_IMPL.md](MARA_GUARD_IMPL.md)(v1.3.2 実装記録・レビュー用)。
> 注: 本書は**静的検証**。実機確認が要る項目は §5 に明示。

---

## 0. 結論サマリ

| クラス | 内容 | [A] 1.3.1 | [B] NPC addon | [C] VR | v1.3.2 対処 |
|---|---|---|---|---|---|
| C1 不正アセット取込 | legacy/0頂点 NIF のロードで divide-by-zero | **起きうる** | **起きうる**(NPC 注入で拡大) | **起きうる** | L1 で動的/非playable 流入を遮断(部分対処・§4.1) |
| C2 体再構築レース | 斬首・変身・RaceMenu・死体 | 緩和済み | 緩和済み+残余1件 | 緩和済み(要 smoke) | 対処不要(残余は beta 側 §5.2) |
| C3 レンダラ干渉(CS) | ランタイム生成メッシュ×シェーダ | **起き得ない** | 起き得ない | 起き得ない(CS は VR 対応) | 監視のみ |
| C4 ロード中 UI ブロック | MessageBox デッドロック | **起き得ない** | 起き得ない | 起き得ない | 不要 |
| C5 SMP ライフサイクル | 浮遊/複製/FSMP 版差 | 緩和済み | 緩和済み | 緩和済み | 不要 |
| C6 共有フォーム改変 | ベースフォームのリネーム等 | **起き得ない** | 起き得ない | 起き得ない | 不要 |
| C7 動的フォーム/セーブ | 0xFF 流入・id 破損 | **潜在バグあり** | 同左 | 同左 | **列挙 filter 境界+hard 判定+登録境界 admission で遮断**(r2) |

**開いていた面は C1 と C7 のみ**。v1.3.2(r2)の主張は敵対的レビュー §7 の狭め表現に従う:
「**MARA 型ランタイムフォーム(および deny 対象)を CEF の列挙・捕獲・登録経路から遮断する。
正規 plugin レコードが参照する破損 NIF の安全性は保証しない**」。C1 の legacy-NIF 面は
残余(§4.1 — BSA 内 NIF は事前検証不能のため、恒久策はレビュー §4 の VFS+隔離 validator 案を
次版検討)。
**r2 追記(2026-07-23)**: 敵対的レビューにより **H1 の機械的正体が確定** —
`TESForm::GetLocalFormID()` は `GetFile(0)` を無条件デリファレンスする(TESForm.h:292-300)。
v1.3.1 の `WornArmors()` は全外部装備に `MakeColonId` を呼ぶため、動的フォーム(CORE
Carrier)で **null deref = ボタン押下即 CTD**。クラッシュログとの突合(§5-3)だけが残る。

## 1. MARA クラッシュ報告の全体像(61 件 + posts)

集計(bugs タブ 61 件: New 31 / Fixed 25 / 情報待ち 2 / 調査中 1 / 非バグ 2)。
クラッシュは **3 世代構造**:

1. **0.0.3–0.0.5 世代(0.0.6 で修正)**: 装備/取引/変身 CTD の嵐 — 鉄鎧装備で CTD(#1052553)、
   フォロワーへ指輪手渡しで CTD(#1052434)、狼化/VL 変身 CTD(#1053612, #1055906 ほか多数)、
   1st→3rd 人称切替 CTD(posts)。装備インターセプトが宝飾品以外や変身レースに未対応だった痕跡。
2. **0.0.6 世代(未解決のまま作者 5/25 以降不在)**: **斬首 CTD**(#1058270 メガスレッド・男性 NPC 限定・
   `SkyrimSE.exe+14E0460` の null 参照、Next-Gen Decapitations/VioLens/Dismembering Framework で発火)、
   **slot-35 専有**による衝突(#1065034 Apachii ショール浮遊、#1058933 TNG 新規ゲーム CTD)、
   **CORE carrier の可視化**(#1059563 — 後述)、Community Shaders ブロック(#1075023)。
3. **全版通底**: SMP 崩れ(浮遊/複製/FSMP 3.5.0 で物理死 — クラッシュではない)、
   ネーミング/永続化破損(エンチャント名消失 #1062149/#1063585、装備時リネーム #1051131)。

**CEF にとって最重要の 1 件 = #1059563** 「ARMO item named CORE carrier in inventory」:
Follower Equip Control(第三者のインベントリ UI)がフォロワー在庫を開くと不可視のはずの
CORE carrier が**恒久可視化**(プレイヤー・全 NPC・クリーチャーにまで)、フォロワー在庫に
最大 6 個スタック、さらにセル遷移で「NPC 名を冠したファントム Misc アイテム」が湧き
**インベントリでホバーしただけで CTD**。作者返信で「carrier は全リングのアタッチ先・隠す意図」
と確認。→ **「第三者の生インベントリ列挙が MARA のランタイムフォームに触れると未定義動作」は
CEF 固有ではなく再現済みの一般事象**。CEF の `+ Add worn item` CTD 報告(2026-07-22)と同クラスで、
[MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md) §3.3 の「**InventoryEntryData に触る前に form-level で
skip する**」順序制約の独立な裏付け。

## 2. クラス定義と MARA 側根拠

| クラス | MARA 側の根拠報告(代表) | CEF に写像する問い |
|---|---|---|
| C1 | 特定リング装備 CTD 群: LotD Ring of Transmutation(posts 3/22+bugs 7/7)、CC リング(#1052543)、P&CE の envirocube 欠落テクスチャ(#posts Madcat221 分析)、hist flask 誤分類(#posts) | ユーザーが選んだ任意 ARMO の NIF をロード/クローン/注入する CEF は、不正アセットで死ぬか |
| C2 | 斬首 CTD 群(#1058270/#1063272/#1064698)、変身 CTD 群(#1053612/#1055906)、1st/3rd 切替 CTD | 骨格再構築・体スワップ中に注入ジオメトリ/骨参照が生き残るか |
| C3 | CS ブロック(#1075023、CS PR #2253)、BSLightingShader 系 CTD(posts Pannel/DadBot3k) | CEF の注入ジオメトリはレンダラ mod が窒息する形か |
| C4 | PrismaUI MessageBox デッドロック(posts WushuLate 詳報)、HUD フリーズ(#posts) | ロード中にメインスレッドを塞ぐ UI/待機があるか |
| C5 | SMP 浮遊/複製(#1087852)、FSMP 3.5.0 物理死(posts)、幽霊メッシュ(#1051445) | 注入体の SMP 世代管理・重複防止は堅いか |
| C6 | エンチャント名消失(#1062149 ほか)、装備時リネーム(#1051131)、同名指輪の相互上書き(#1051489) | CEF が実行時に改変するフォームは共有物に届き得るか |
| C7 | CORE carrier 可視化+ファントム hover-CTD(#1059563)、セーブ跨ぎの装備剥落(#1052119) | 動的フォームが CEF の状態(json/co-save)に入り込めるか |

## 3. 検証結果(file:line 付き詳細)

### C1 — 不正アセット取込: **起きうる(A/B/C 共通・未ガード)**

- 注入経路は `InjectOnRoot`(SkinRebind.cpp:786-939)→ `LoadNif`(:235-246)= **エンジンの
  `RE::BSModelDB::Demand`**(BSA+loose を VFS 経由)→ `Clone`(:808)→ skinned `BSGeometry` 走査
  (:830-851)→ `RebindGeometry`(:550-675)。
- 既存ガード: skinless 無視(:833)、0 骨 early-return(:563-565)、null 骨 fail-soft(:574-577)、
  skinned 皆無なら false(:863-866)、骨未解決は祖先リマップ(:604-627)。**null 安全は厚い**。
- **穴**: SSE スキンパーティションローダの divide-by-zero(legacy `NiTriShape`/`NiTriStrips`+
  0 頂点スキン)は **`BSModelDB::Demand` 内部のハードウェア例外**で、`LoadNif` のエラーチェック
  (:240)では捕捉不能。これを防ぐ `ValidateNifSkinnable`(NifCarrierCore.cpp:220-244 —
  コメントに「Box44 CTD の根本原因」)は **carrier 生成/マージ経路専用**(:1146/:1678/:1841/:2224)で、
  **直接注入経路には掛かっていない**。
- 露出範囲の限定: 装備中アイテムの捕獲はエンジンキャッシュ済みモデルの再 Demand なので新規危険なし。
  危険なのは「**CEF だけがロードする** content」= 捕獲後に隠しコンテナへ入った box/persist content と
  1st person(m1p)バリアント。MARA の「特定リング装備 CTD」の正確な CEF 版。
- [B] は同メカニズムが NPC 注入(PublishStore.cpp:835-857)にも乗るため爆風半径が広い。
  [C] はランタイム非依存(エンジン側例外)。
- **v1.3.2 対処**: L1 で「動的/非 playable ARMO」の流入を遮断(この層の不正フォーム由来分を封鎖)。
  legacy-NIF 面の残余と、事前検証を見送る理由は §4.1。

### C2 — 体再構築レース: **緩和済み(A/B/C)**

- CEF は斬首/変身/死亡の**専用ハンドラを持たない**(リポジトリ全域 grep 陰性)— 全て
  Load3D フック(plugin.cpp:30-53)+ EquipSink(:97-137)経由の**反応型**で、処理は AddTask で
  メインスレッド直列化。変身の装備自動解除 → token unequip → 表示 OFF は EquipSink 経路で成立。
- 守り: **bonePins**(SkinRebind.cpp:157-193、810b97d)がバインド先骨を NiPointer で
  ピン(2026-07-04 の freed-bone CTD の恒久修正)。注入ホルダは頭でなく `"NPC Root [Root]"` 配下
  (:816-817)なので斬首のヘッドノード除去に巻き込まれない。骨消失は次回 Reconcile の
  祖先リマップ(:604-627)が吸収。重複ホルダは `DetachNodes` の同名全掃(:181-188)。
- MARA の斬首 CTD(`mov rax,[rdx+0x48]` @ +14E0460)は「頭/首 teardown 中に装備追跡が
  null を踏む」筋 — CEF は斬首に介入せず、注入体も頭部非依存なので**構造的に別**。
- 残余: [B] のみ `OnNpcActorLoaded`(PublishStore.cpp:835-857)に `IsDead` ゲートがなく、
  **死体ロードで equip manager 再装備**が走る(骨 fault はしないが未検証の相互作用)→ §5.2。
  [C] は同緩和が「vfunc 0x6A が VR でも Load3D であること」に依存(v1.3.1 VR は community-beta で
  実動報告あり=経験的には firing)→ §5.1 に smoke 項目。

### C3 — レンダラ干渉: **起き得ない(A/B/C)**

- CEF は**ジオメトリをゼロから作らない**: 生成は「エンジンがロードした NIF の `Clone()`」
  (SkinRebind.cpp:808)と「シェーダを持たない素の holder `NiNode::Create`」(:872)の 2 箇所のみ。
  D3D/DXGI/present フックはゼロ。ライブなシェーダフラグ/アルファ改変もゼロ
  (NifCarrierCore の alpha 操作はオフラインのディスク NIF 限定)。
- `ApplyTextureSet`(:685-702)は**自クローンの** material に型正しい仮想関数のみ使用。
  community-REL の shader-init id(99866/106432)は「クローン envmap で落ちる+VR DB に無い」
  ため意図的に不使用(:677-684)— MARA が踏んだ「envmap 系シェーダ CTD」(posts Pannel/DadBot3k/
  Madcat221 の P&CE 分析)と同じ穴を**既に回避済み**。
- CS 側事実: MARA ブロックは `LoadLibrary("MARA.dll")` の**ファイル名チェック**
  (XSEPlugin.cpp、PR #2253・2026-05-01)で、技術的根拠の記載は皆無 —「CS 由来に見える CTD の
  誤帰属+ソース非公開」というクレーム負荷起因(doodlum の Discord 発言の転載が bugs #1075023 に
  現存)。**CS のコード/issue に CEF への言及はゼロ**。CS は VR も本流対応(v1.7.3)だが、
  CEF は C3 の攻撃面自体を持たない。

### C4 — ロード中 UI ブロック: **起き得ない(A/B/C)**

- `DebugMessageBox`/`MessageBox` 使用ゼロ(全域 grep)。通知は非ブロッキングの
  `DebugNotification` のみ。
- メインスレッドが sync スレッドを **join/wait する箇所ゼロ**。in-proc sync(BoxStore.cpp:1080-1130)
  も外部 exe 版(:958-1067)も detached、完了通知は AddTask 配達(:1122-1125)。wedge 時も
  watchdog はログ+`g_lastSyncExit=-2` を残すだけでメインスレッドは無傷(:1072-1096)。
- SMF セクションはユーザーがオーバーレイを開いている間のみ描画。ロード画面と併走し得る
  定期処理は bind watchdog の 2.5s tick 1 本で、`g_watchdogTickPending` で合流済み
  (SkinRebind.cpp:1442-1448)。PrismaUI デッドロック級の構造は存在しない。

### C5 — SMP ライフサイクル: **緩和済み(A/B/C)**

- 世代検知: `HasDeadPhysicsBind`(SkinRebind.cpp:505-541)が「骨が骨格から外れた(世代引退)」
  「同クラスに新しい世代 id が出た(stale)」を検出、watchdog(:1391-1460)が Reconcile。
- 重複防止: 名前冪等(`GetObjectByName` early-return :796-798)+ `DetachNodes` 同名全掃
  (:181-188)+ AddTask 直列化。MARA の「複製/浮遊 SMP」(#1087852)相当は構造的に残らない。
- FSMP 3.5.0: 命名互換は `ParseRenamedBone`(:260-297)/`FindFsmpRenamedBone`(:310-346)で
  解決済み(HANDOVER.md §5-6 オフライン検証)。MARA×3.5.0 の報告は「物理死」でクラッシュではなく、
  CEF は 3.5.0 実機検証済み。

### C6 — 共有フォーム改変: **起き得ない(A/B/C)**

- 実行時フォーム改変は**全て CEF 自前プールのみ**: [B] publish/npr トークン
  (`CostumeFW_NPC.esp` 0x800/0x810 固定 id、PublishStore.cpp:17-20/:333-344)の
  name/keyword/stats/slot restamp、[A+B] carrier ARMA/HDPT の model repoint
  (`CostumeFW.esp` 0x909+、BoxStore.cpp:635-643/:701-802)。repoint は `CarrierFileOnDisk`
  (:662-697)が NIF ヘッダ magic まで検証(この経路は C1 対策済み)。
- co-save には改変値を保存せず(colon-id/FormID/slot のみ、Cosave.cpp:79-129)、毎ロード再導出。
  MARA の「共有ベースフォームのリネームで全インスタンス名破壊」クラスとは構造的に無縁。

### C7 — 動的フォーム/セーブ整合: **潜在バグあり → v1.3.2 で閉じる(A/B/C)**

- `MakeColonId`(BoxStore.cpp:292-299)の `%06X` は**最小幅**指定なので 0xFF フォームの
  `GetLocalFormID()`(8 桁)を `buf[8]` に書くと**切り詰め**られ、`"FF00080:"` 型の破損 id が
  生成される。`CanonicalizeColonId`(SkinRebind.cpp:1508-1523)も同型。
- 列挙(`WornArmors` :1860-1890 / `InventoryArmors` :1892-1950)には動的/playable フィルタが無く、
  しかも `WornArmors` は **`entry->IsWorn()`(ExtraDataList 走査)がフォームレベル検査より先**
  (:1872)— #1059563 の hover-CTD と同じ「触っただけで死ぬ」面に露出。
- 外部入口: Papyrus native `AddBox`/`AddPersist`(Papyrus.cpp:160-179/:553-567)は
  ゲート無し(store 側 `AddBox` の ROOT C/D 検疫 :2883-2905 は CEF-own id と重複捕獲のみ)。
  プリセットは `CanResolveContent` ゲートあり(Preset.cpp:235)。
- ROOT H(未解決 id 保全)は persist [A+B](Cosave.cpp:83-88/:240-244)・NPC publish/persist
  [B](`CarryUnresolvedPubBinding`/`CarryUnresolvedNprAssignment`)で健在 — 「プラグイン一時
  無効化で登録が消える」MARA 型のセーブ劣化は既に防御済み。
- **v1.3.2 対処(確定)**: ループ先頭 L1 skip(順序入替を含む)+ buf 拡大(`buf[16]`、
  正規 6 桁 id は不変=後方互換)+ `CanCaptureContent` ゲートを SMF/MCM/preset/native 全入口に。

## 4. 開いたまま残る面(誠実な記載)

### 4.1 C1 の legacy-NIF 面は v1.3.2 では閉じ切らない

- 事前検証の唯一の既存実装(`ValidateNifPathImpl`)は `std::filesystem` ベースで
  **loose file しか見えない**(BSA 内 NIF は不可視、NifCarrierCore.cpp:1596-1610)。костюム mod の
  主流配布形態は BSA なので、「事前検証を通った=安全」という**偽の安心感**を作る。
  エンジン VFS でロードして検証する方式は「検証自体がクラッシュサイト」になり本末転倒。
- よって v1.3.2 は: (a) L1 で不正**フォーム**の流入を遮断(動的/非 playable — #1059563 クラスは
  これで全滅)、(b) 既存の carrier 経路ゲート(`ValidateNifSkinnable`/`CarrierFileOnDisk`)は現状維持、
  (c) 「不正**アセット**(legacy NIF)を持つ正規 ESP アイテムの捕獲」は開いたまま —
  README の既知の制約に 1 行、恒久策(SEH ガードや loose 限定 opt-in 検証)は後続検討。

### 4.2 v1.3.2 に含めない残余(場所が [B]/beta のため)

- `OnNpcActorLoaded` の `IsDead`/`IsDeleted` ゲート(PublishStore.cpp:839)— beta 線の次版で。
- in-proc sync の HW 例外非捕捉(BoxStore.cpp:1099-1109)— NIFCARRIER_INPROC.md I-5 で追跡中。

## 5. 実機確認チェックリスト(静的検証の限界)

1. **[C] VR smoke**: セル移動/RaceMenu 後に衣装が再アタッチされること(= vfunc 0x6A が VR でも
   Load3D)。v1.3.1 VR beta で実動報告があるため回帰確認レベル。
2. **[B] 死体再装備**: publish トークン装備 NPC を殺害→セル再訪 — クラッシュしないこと
   (ゲート追加は beta 次版)。
3. **v1.3.2 ガード**: MARA 導入環境で `+ Add worn item` / `+ Add from inventory` を開き
   CORE Carrier が**出ない**こと + **安全な別アイテムの捕獲が正常に通る**こと(レビュー
   P1-2 の CaptureEnchant 面。手順はレビュー §6.2「MARAあり」の 7 項を正とする)。
   従来 CTD の再現が必要な場合は **v1.3.1 を一時導入**して行う(r2 で allowDynamic は
   撤去済み — 修正前バイナリでの再現の方が証跡としても正しい)。クラッシュログの
   faulting アドレスが `GetLocalFormID` 相当(CostumeExpansionFW.dll 内)なら §0 の
   r2 追記どおり root cause 確定。
4. 回帰: 通常捕獲(worn/inventory × box/persist)・プリセット・`cef` コマンド・
   セーブ/ロード後の表示維持。

## 6. 出典

- MARA bugs/posts/JP: Nexus 173949 各タブ(61/61 件列挙・2026-07-23 取得)、
  skyrimspecialedition.2game.info/detail.php?id=173949
- CS: github.com/community-shaders/skyrim-community-shaders(XSEPlugin.cpp・PR #2253・v1.5.0)
- CEF 側: 本リポジトリ `main` @ 6877530 / `nifcarrier-inproc` @ 0772ed0(file:line は本文中)
- 原報告: CEF Nexus 183697 posts(2026-07-22)= [MARA_COMPAT_PLAN.md](MARA_COMPAT_PLAN.md) §0
