# 実機テストリスト — スロットアラインメント検証増分(test.3+slotguard)

> 対象: 2026-07-30 深夜実装の「ポインタ値アラインメント検証」一式。
> 前提知識は BUGREPORT_2026-07-27_persist_ctd.md の「uint16 パターン仮説」
> 「ローカル静的調査」節。**このリストは上から順に**(M0 が全ての前提)。
> 所要目安: §1-§3 で 15 分、§4 込みで 25 分。

## 変更点サマリ(何をテストするのか)

| # | 変更 | 期待 |
|---|---|---|
| 1 | `PlausibleObjectPtr`: 非 null(>0x10000)+ **8B アライン** + カノニカルのポインタ値検証(deref なし) | bone-index 状破損値(0x1 / 0x0001000500050005 系)を 100% 拒否 |
| 2 | `HolderArrayBroken` が旧 `< 0x10000` 床から PlausibleObjectPtr へ | quarantine が uint16-run の `_data` も捕まえる |
| 3 | `ChildrenWalkable` 同上 | 同上(walk 入口) |
| 4 | walk 5 箇所(FindFsmpRenamedBone / dead-bind sweep / nodediag / bone census / headdiag)に**スロット値検証** — 不正スロットは skip+`SCENE CORRUPTION` error(9 回目以降 debug) | 破損スロットを踏まずに続行 |
| 5 | 新コンソールコマンド `cef slottest`(合成 slot 破損プローブ、mod 不要) | 検証器の検証が 30 秒で回る |

## §0. ビルド確認(M0)

> **現行 M0 スタンプ = DLL 2,680,320 bytes / mtime 2026-07-31 10:18:26**
> (budget 表示修正入り。**サイズは 23:49 版と同一 — mtime でのみ判別**。
> MO2 配備先の一致は 10:18 に確認済み)
> 履歴: 23:49:24 版(境界マトリクス入り)で §0-§2 実施 → 10:18:26 版は
> Diagnostics 表示修正のみ(下記実施記録参照)

- [x] 起動ログ 1 行目のスタンプ一致(05:14 / 10:08 の両セッションで
      `file 2026-07-30 23:49:24` を確認済み。**次回起動は 10:18:26 のはず** —
      [[dll-deploy-lock-build-banner]] の静かな失敗に注意)

## §1. 合成テスト(セーブ不要、メインメニューから可)

- [ ] `cef slottest` 実行。**全行 ok/rejected であること**:
      - **境界マトリクス 6 行すべて ok → `boundary matrix: ok`**
        (観測 3 値 reject / 0x8000 reject / 非カノニカル reject /
        **0x10000 は pass** — これは検出器の設計上の限界の明示。
        アライン済みカノニカルの偽アドレスは値検証では区別できない、が仕様)
      - `PlausibleObjectPtr -> rejected (ok)`
      - `ChildrenWalkable(holder) -> yes (ok - buffer itself is fine)`
      - `guarded walk: 3 visited, 1 skipped -> ok`
      - `slot[2] restored - teardown is safe`
      - ログに `SCENE CORRUPTION: child slot of 'CEF_SlotProbe' holds implausible
        pointer 0x1000500050005 (slottest, hit #1)` が 1 行出る
- [ ] slottest を**もう 1 回**実行 → hit #2 として error で出る(8 回までは error)
- [ ] `cef arraytest` 回帰: 従来通り全行 walkable=yes(アラインメント強化で
      正常配列が誤検出されていないこと)

## §2. 通常系回帰(既存セーブで 10 分)

> 偽陽性はエラーとしてだけでなく「**正常スロットの誤 skip = 機能の静かな劣化**」
> としても出る。エラー 0 件の確認と、下の 4 つの機能側回帰は別物として全部見る。

- [ ] ロード → `settings: loaded ...` → 注入完走、`[error]` 0 件
      (**特に `SCENE CORRUPTION` が正常環境で 1 件も出ないこと** = 偽陽性ゼロの確認)
- [ ] **SMP sway 回帰(誤 skip の最重要検出器)**: SMP costume を表示して
      `bound N bone(s) to FSMP physics-driven node(s)` の **N が従来と同じ**こと。
      FindFsmpRenamedBone が正常スロットを skip すると、エラーではなく
      「揺れていた服が static に落ちる」(`remapped N unresolved` の増加)として出る
- [ ] **watchdog の静粛(dead-bind 誤判定の検出器)**: 注入済みのまま 1-2 分放置して
      不要な re-inject・detach が起きないこと(スイープの誤 skip は FSMP 世代集合を
      欠けさせ、誤 dead 判定→再注入連発として出る)
- [ ] **1p 確認**: 一人称視点で costume 表示が従来通り(walk は 3p flat tree と
      1p NiNode の両方を踏む)
- [ ] **SMF Diagnostics ページを開く**: Physics bones(bone census = walk 変更箇所)が
      従来通りの数字を出すこと
- [ ] census 行(`attached: N content(s) ...`)従来通り
- [ ] box 再装備 19→2→19 / マスタースイッチ OFF→ON / `cef nuke` → 復帰、二重化なし
- [ ] persist ON→OFF→ON(1 アイテムで可)完走
- [ ] `cef nodediag` / `cef headdiag`: 従来通りの出力、unwalkable 0
- [ ] セーブ→ロード→維持

## §3. bDebugMode の ini 経路(前回未検証のまま報告者に送った項目!)

- [ ] `CostumeExpansionFW.ini` の `bDebugMode=1` で起動 →
      ログ冒頭付近に `DIAGNOSTIC MODE ON (CostumeExpansionFW.ini [Diagnostics]
      bDebugMode=1)` の **warn バナー**
- [ ] debug 行(`equip: ...` / healthpoll / mem 行 30s)が流れる
- [ ] SMF Diagnostics のチェックボックスが ON 状態を反映している
- [ ] チェックを外す → `diagnostic mode off` warn、debug 行が止まる
- [ ] **bDebugMode=1 のまま §2 を 1 周**: healthpoll が全ホルダー健全と言い続け、
      SCENE CORRUPTION も出ないこと(周期検査系との組み合わせ回帰)
- [ ] ini を 0 に戻して起動 → バナー無し(平常)

## §4. BDDeer 模擬実験(時間があれば。報告者機序の再現試行)

> 狙い: 「overlay のテクスチャ欠落 → 半初期化 shader → ライトパスで RIP=0」が
> 単独で再現するか。**再現したら大収穫**(報告者環境不要で watchpoint まで
> ローカルで追える)。再現しなくても「欠落単独では不十分」という絞り込み。

- [ ] 使用ボディのフォールバック手テクスチャ(例: `textures\actors\character\
      female\FemaleHands*.dds` 相当、実際に使われている 1 式)を**リネームで退避**
      (msn/_sk/_s の 3 枚だけ。diffuse は残す = 報告者と同じ欠け方)
- [ ] RaceMenu で hand overlay を 1 枚適用(SOvl を確実に生成させる)
- [ ] 着用中の SMP 服で persist add(capture の unequip を発生させる)を 5-10 回
- [ ] 観察: CTD するか / `SCENE CORRUPTION` が出るか / 何も起きないか
      (**どの結果でも BUGREPORT に記録**)
- [ ] 終了後テクスチャを**必ず**元に戻す(リネーム復帰)

## 実施記録(2026-07-31 朝、オーナー実行・ログ精読で確認)

**§0-§2 判定: 緑**(セッション 05:14-05:31、`CostumeExpansionFW.log` 2328 行)

- §0: M0 一致(`file 2026-07-30 23:49:24` を 1 行目で確認)
- §1 **全緑**: slottest ×2(境界マトリクス 6/6 ok・guarded walk 3visited/1skipped・
  restored・hit #1/#2 が error で発報)、arraytest 全行 walkable=yes
- §2 **緑**: error はセッション全体で slottest の意図的 2 件のみ =
  **実環境の SCENE CORRUPTION 偽陽性ゼロ**。SMP sway 回帰 OK(remapped 427 warn は
  キャリア装着前の初期パスの正常縮退で、05:23:51 に bound 群
  [GLBoa 19 / GLDressH 56 / GLDressI 60 他] に収束)。census 遷移で
  box 出し入れ 17→0→17・persist add/remove 44→45→44 の実施を確認。
  nodediag: 3p/1p 全 18 holder+RealBody が健全(unwalkable 0)。
  **headdiag(05:29:24)も緑**(初回集計で grep パターン違いにより見落とし →
  オーナー指摘で確認): 3p = 1121 Armor + 38 Head、3 merge groups
  (Armor_00000071 8 / **Armor_00000073 1113** / Head_00000005 38)、1p = 0(正常)、
  異常座標 0 件、ref 'NPC Head [Head]' 実位置あり。**1159 本をスロット検証込みの
  walk で列挙してエラーゼロ = 実地の大量スロット通過でも偽陽性ゼロの追加裏付け**。
  末尾 05:31 セーブロード → `registry roll call (post-load): 44 active, 0 problem(s)`。
  watchdog 起因の不要 re-inject なし。
- **SMF Diagnostics ページ確認(07-31 10:46 スクショ、第 2 セッション)**:
  census 行 `FSMP merged on player: 1159 bone(s) in 3 group(s); CFW's own 1113 in 1`
  = headdiag と完全一致(census walk 緑)。BLE detected・box 一覧・
  Churn(watchdog=0 deadbind=0 rebindRetry=0 = 静粛の追加裏付け)。
- **1p 視覚確認(オーナー)**: 表示あり・SMP 揺れなし = **仕様通り**
  (FSMP は 1p スケルトンにマージしない。headdiag 1p=0 が実測)
- **発見→修正済み: Diagnostics の bind 集計が routine Reconcile でゼロ化**
  (スクショの `CFW content needs: 0 / Heaviest shape: 0`)。機能は正常で表示のみの
  既存グセ: idempotent skip パスがカウンタを 0 上書きしていた(ログ裏取り:
  10:11:41 bound 記録 → 10:12:43/45 の skip Reconcile で消去)。
  skip パスでは前回実数を保持するよう修正、**新 M0 = 10:18:26**。
  - [ ] **次回起動での確認**: 表示 → equip 変更等で Reconcile を起こす →
        Diagnostics Refresh で needs/Heaviest が 0 にならず実数のままであること
- 副産物の実測: arraytest で cap が 1→2→3→4 と **AttachChild 毎に成長(毎回 realloc)**
  — 「FSMP 大量マージ = realloc 頻発」推定の傍証(growthSize 既定値の挙動)
- 残: §3(bDebugMode 実機経路)、§4(BDDeer 模擬)

## 撤退基準

- §1 の slottest が FAIL → 実装バグ。§2 以降は中止してコード修正へ
- §2 で `SCENE CORRUPTION` が正常環境で出る → 偽陽性。閾値(アライン条件)を
  即調査。**この状態で報告者に渡してはならない**(ログ洪水+誤誘導)
- §2 全緑 + §1 全緑 → この DLL を test.4 候補としてスタンプ記録
