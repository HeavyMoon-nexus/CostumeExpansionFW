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

- [x] **リネーム実施済み(07-31、Claude 実行)**: プレイヤーは Succubus 種族
      (実効相対パス `textures\actors\character\Succubus\Body\femalehands_1_*`)。
      overlay の msn/sk/s 継承元は素体の race スキン(realbody 用 TXST ではない)
      なのでこのパスで狙い通り。**全 22 ファイル**(msn/_s/_sk × 7 mod、
      disabled 分も頑健性のため込み、`old/` は実効外なので除外)を
      `.cef_s4_off` サフィックスでオフ。検証: 実効パスの 3 種 = 0 件、
      diffuse 7 件残存 = **報告者と同一の欠け方**。
      リネーム全リスト: scratchpad の `s4_renamed_files.txt`
- [ ] RaceMenu で hand overlay を 1 枚適用(SOvl を確実に生成させる。
      bDebugMode=1 のまま推奨 — 現状 ini は 1)
- [ ] 着用中の SMP 服で persist add(capture の unequip を発生させる)を 5-10 回
- [ ] 観察: CTD するか / `SCENE CORRUPTION` が出るか / 何も起きないか
      (**どの結果でも BUGREPORT に記録**。CTD したら crash log の被害者が
      Hands [SOvl0] 相当+テクスチャ [MISSING] 3 枚か照合 = 報告者機序の再現判定)
- [ ] 終了後テクスチャを**必ず**元に戻す。復元ワンライナー(Git Bash):
      `find /k/Mo2_SkyrimSE1170/mods -name "*.cef_s4_off" | while IFS= read -r f; do mv "$f" "${f%.cef_s4_off}"; done`
      復元後の確認: `find /k/Mo2_SkyrimSE1170/mods -name "*.cef_s4_off" | wc -l` が 0

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
- **§3 実施(07-31 10:37-10:47 第 3 セッション、M0=10:18:26 版)— 全緑**:
  ini バナー `DIAGNOSTIC MODE ON (CostumeExpansionFW.ini [Diagnostics]
  bDebugMode=1)` がログ 3 行目に発報(**報告者に送った ini 経路の実機実証**)/
  debug 行 7689 件・healthpoll 1380 行全健全・mem 行 30s 周期(WS 8.2-8.5GB 正常域)/
  SMF チェックボックス ON 反映(オーナー目視)→ OFF で
  `diagnostic mode off (SMF Diagnostics toggle)` 発報+debug 停止/
  **debug モード中も error 0 = SCENE CORRUPTION 偽陽性ゼロの debug 版確認**/
  thread census 5 スレッド観測(07-30 測定と整合)
  - [ ] 残: ini を 0 に戻して起動 → バナー無し(平常)確認
        ※ live mod の ini は現在 **bDebugMode=1 のまま**(§4 をやるなら 1 のままが
        好都合 — 模擬実験は debug ログがあるほど情報量が多い)
  - [ ] 残: budget 表示保持の宿題 — 次に Diagnostics ページを見たとき
        needs/Heaviest に実数が入っていること(10:18:26 版の修正確認)
- 残: §4(BDDeer 模擬)

## §4 実施記録+マージレース発見(07-31 昼、第 4 セッション 10:59-11:40)

- **§4 主観察: テクスチャ欠落状態で persist add 6 回+off/on 多数+DoReset3D 複数
  → CTD ゼロ・SCENE CORRUPTION ゼロ**(error 3 件 = slottest×2 + auto-sync 誤ウェッジ 1)。
  「欠落単独では(この環境・6 回では)null-call は再現しない」という結果。
  ※ RaceMenu hand overlay を適用したかは未確認(ログに写らない)— オーナーに要確認
- **副産物で本命級の発見**: 「newest persist entry に物理が付かない」の機序を
  実地確定(詳細 = BUGREPORT §4 セッションの節)。マージレース+救済経路欠如。
  **RearmStaticBinds 実装済み(f58fe79)**
- **対照実験がログに自然発生**: Aether 2 アイテム(過去に bound 経験あり)は
  11:34:07 に dead-bind watchdog が世代交代(→Head_0000000F)へ再 bind できた。
  **never-bound の 000DEF/000E7D だけが取り残された** = 機序の完璧な傍証
- テクスチャは**まだ欠落状態のまま**(復元していない。§4 を締めるときに
  上記ワンライナーで復元)

## §5(次セッション): マージレース救済の検証(M0 = 2,683,904 / 07-31 11:48:03)

- [ ] 起動ログ 1 行目 `file 2026-07-31 11:48:03` 一致
- [ ] ロード ~8 秒後: `re-arming N static item(s) for rebind (post-load settle)` が出る
      (000DEF/000E7D が static なら N≥2)
- [ ] その後の retry で `bound N bone(s) ... CACF1333C_...` / `C8B0C9ECB_...` が出て
      **Gala ドレスが実際に揺れる**(= レース敗者の救済成立)
- [ ] persist add を 1 回 → done 後 4-12 秒の `re-arming ...(head-rebuild settle)` →
      最新アイテムが自動で bound になる(**報告者症状の根治確認**)
- [ ] budget 表示保持の宿題: Diagnostics ページの needs/Heaviest が Reconcile 後も実数
- [ ] 誤発動がないこと: 全 bound 状態で `re-arming` が**出ない**(silent no-op)

## §6(次セッション): 痩身キャリア+sync 心拍の検証

> **現行 M0 = DLL 2,693,120 bytes / mtime 2026-07-31 12:42:31**(MO2 一致確認済み)。
> 内容 = スケルトン集合判定(c1cb9a2)+ハッシュ bump(p3/v3)+sync 心拍
> (3265256+ビート階段 2/7/17/47…秒)+レース救済(f58fe79、§5 で実証済み)。

- [ ] 起動ログ 1 行目 `file 2026-07-31 12:42:31` 一致
- [ ] 初回 sync の CEF_sync.log 冒頭に
      `live-skeleton set: N bone name(s) from 4 skeleton file(s)`(N は数百〜千級)
- [ ] ハッシュ bump により全キャリアが 1 回だけフル再生成される —
      **このとき心拍通知が 2s→7s→17s→47s… で画面に出る**(体感確認の本番)
- [ ] 完了時「Costume carriers: rebuilt N item(s)」通知
- [ ] SMF Diagnostics を sync 中に開くと `carrier auto-sync: RUNNING (box NN, step
      i/M, Xs)` がライブで見える
- [ ] 再生成後のキャリア痩身を実測(Claude が NIF の CEF 骨数を数える —
      旧 1330 → 数百見込み。Aether の 407 幽霊 ×2 が消えていること)
- [ ] 2 回目以降のフル再生成時間が短縮(旧実測 130-140s → 大幅減の見込み)
- [ ] `carrier diagnostic`(0 of N)の発生頻度が §5 セッション(7 回)より減る
      (マージ高速化でレース自体が減る)
- [ ] 旧「wedged; blocked until restart」error が出ない(新文言は 120s 時の
      warn「slow, not stuck」のみ)

## 撤退基準

- §1 の slottest が FAIL → 実装バグ。§2 以降は中止してコード修正へ
- §2 で `SCENE CORRUPTION` が正常環境で出る → 偽陽性。閾値(アライン条件)を
  即調査。**この状態で報告者に渡してはならない**(ログ洪水+誤誘導)
- §2 全緑 + §1 全緑 → この DLL を test.4 候補としてスタンプ記録

## §6 実施記録(07-31 夜、第 6 セッション 19:42-、M0=12:42:32 実測)

- [x] M0 一致(loaded 行 `file 2026-07-31 12:42:32`。§6 見出しの 12:42:31 は
      ls 秒丸め — 以後 12:42:32 を正とする)
- [x] `live-skeleton set: 751 bone name(s) from 4 skeleton file(s)` ✓
- [x] bump による一回きりの全量再生成 = **総時間 357 秒(19:46:08→19:52:05)**。
      オーナー体感の「317 秒」は 317s 心拍表示。内訳: **box 8 個は 2 秒で完了、
      persist が 355 秒**(本体 r5 は ~60 秒で完成、残り ~5 分は proxy parts
      7+個の生成・検証)
- [x] **心拍が設計通り完璧動作**: 2s→7s→17s→47s→以後 30s ごと、
      stage+step+経過秒付き(box 57, step 8/9, 2s → persist, step 9/9, ...)。
      120s 時の warn も新文言「slow, not stuck」
- [x] 旧「wedged; blocked until restart」error 消滅(**session error 0 件**)
- [x] レース再演の激減: carrier diagnostic **1 回**(§5 は 7 回)、rearm 1 回で救済
- [~] 痩身は**部分的**: 1330→1200 骨。スケルトン実在骨(Anal/Belly 等 74×3)は
      除外成功。**残る 333×3 は「イヤリング NIF に衣装フルセットの骨が同梱」
      (BodySlide ビルド由来、DragonPriestess 系+Bone001-333)で NIF に実在する
      カスタム骨** = live-skeleton 除外の守備範囲外
- 発見した小改善 2 件(次ビルド): ①完了通知「rebuilt N item(s)」が画面のみで
  ログに残らない(1 行足す)②次の痩身本丸 = **「xml が参照する骨+その祖先
  だけを焼く」**(イヤリングの実揺れ骨は数本〜数十本のはず。persist 全体で
  1200→数百、proxy 生成も比例して短縮の見込み。ツリー切断に注意 = 参照骨の
  祖先チェーン保持が必須)
- **次回以降の unchanged パスは従来通り ~1 秒**(bump は消化済み)

## §7(次セッション): 参照骨限定痩身の検証(M0 = 2,699,264 / 07-31 20:10:59)

> 内容 = xml/skin 参照+祖先だけを焼く leaf-delete 方式(ccd27b1)+p4/v4 bump
> +rebuilt 完了ログ行。**テクスチャは復元済み(§4 クローズ)**。

- [ ] 起動ログ 1 行目 `file 2026-07-31 20:10:59` 一致
- [ ] 初回 sync = p4/v4 bump の痩身再生成。**persist 所要時間の実測**
      (§6 実測 355 秒 → 大幅短縮の見込み。心拍の最終表示秒で読める)
- [ ] CEF_sync.log に `[isolate] <nif>: dropped N unreferenced node(s)` —
      Aether 系 2 本で N が 300 超のはず
- [ ] `auto-sync: rebuilt N item(s)` がログにも出る(新規ログ行)
- [ ] 再生成後のキャリア実効骨数(Claude が NIF ツリーを数える —
      素名幽霊も含めた実測。目標: 1200+素名 → 数百未満)
- [ ] SMP 揺れ回帰: persist の DressF/K が従来通り bound(keep 集合の取りこぼしが
      あれば「bound N 減少/remapped 増加」として出る — 撤退基準)
- [ ] レース再演頻度: 診断回数が §6(1 回)以下を維持
