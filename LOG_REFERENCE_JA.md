# CostumeExpansionFW (CEF) ログリファレンス(v1.3.0現在)

**ログファイル**: `Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log`
- ゲーム起動のたびに上書きされます(前回分は残りません)
- 書式: `[時刻] [レベル] メッセージ`。以下、**[warn]** / **[error]** 表記のない行は info です
- 補助ログ: `Data\SKSE\Plugins\CEF_sync.log`(MO2では overwrite に出ます)— キャリア自動再生成(nifcarrier)の詳細。auto-sync 失敗時はこちらを確認

## まず見るポイント(トラブル時)
1. 先頭の `CostumeExpansionFW loaded (file ...)` — file の日時が導入したDLLより古ければ、MO2が古いDLLをキャッシュしています(MO2を完全終了→再起動)
2. `settings: loaded ...` — 設定ファイルが読めているか
3. 装備しても揺れない → `remapped ... static` と `auto-sync:` の失敗行を探す(失敗時は CEF_sync.log へ)
4. そもそも表示されない → `NOT SHOWN` / `LoadNif failed` を探す

## 1. 起動時(毎回出る)
- `CostumeExpansionFW loaded (file A / compile B)` — ビルドスタンプ。A = ゲームが実際に読み込んだDLLの更新日時、B = コンパイル日時
- `serialization installed` — セーブ連動データ(co-save)ハンドラの登録
- `skee BodyMorph interface: acquired / unavailable (...)` — RaceMenu連携の可否。unavailable でも動作します(体型モーフのみ不可)
- `PlayerCharacter::Load3D hook installed (0x6A)` — セル移動・ロード時に再適用するためのフック
- `console hook installed (Script::CompileAndRun)` — `cef` コンソールコマンドの有効化
- `LoreBox: BSScaleformTranslator::Translate hook installed (vfunc 0x2)` — アイテム名ツールチップ整形
- `SMF: registered section 'Costume Expansion FW' (5 pages)` — SKSE Menu Framework UI の登録成功
- `SMF: SKSEMenuFramework.dll not installed - SMF UI skipped (MCM / console remain available)` — SMF未導入(問題なし。MCMとコンソールは使えます)
- `Papyrus natives registered (CFW_Native)` — MCM用ネイティブ関数の登録
- `bind watchdog started (...ms tick)` — FSMPスケルトン再構築の監視開始

**キルスイッチ(出たらCEFは完全に無効化されています)**
- `CEF hard-DISABLED by CEF_DISABLE.txt - plugin is inert`
- `CEF hard-DISABLED by CostumeExpansionFW.ini (bEnabled=0) - plugin is inert`
- `CostumeExpansionFW is DISABLED via external config - doing nothing`

## 2. settings: — 設定ファイル(CEF_settings.json)
- `settings: loaded N box(es) (M content), K persist (catalog), enabled=...` — 読込完了サマリ(正常系の要)
- `settings: no ... (no settings yet)` — 初回起動でファイルがまだ無い(正常)
- `settings: wrote N box def(s) (enabled=...)` — 設定の保存
- `settings: reapplied N box(es)` — セーブロード後の再適用
- `settings: reloaded from disk (MCM) - ...` — MCMからの再読込
- `settings: migrating legacy ...` — 旧形式ファイルからの自動移行
- `settings: healed pre-merge plugin ids -> CostumeFW.esp (v1.2.1 consolidation)` — 旧バージョンのIDを自動修復
- **[warn]** `settings: recovered from ..._bak (main file corrupt; ...)` — 本体JSONが破損 → バックアップから復旧
- **[error]** `settings: JSON parse error / field type error (...) - loaded empty` — 設定破損。ファイルは修理できるようそのまま残されます
- **[warn]** `boxes: LoadBoxes drops ...` — 不正なエントリの自動除去(「1トークン=1box」「1アイテム=1ホルダー」ルールの強制)

## 3. キャリア自動再生成(FSMP物理)
box構成の変更 → キャリアNIF再生成 → 差し替え、の自動ループのログです。
- `carrier manifest updated (N box(es)) - rebuilding FSMP carriers` — 構成変更を検知、再生成を予約
- `auto-sync: rebuilding carriers (in-proc nifcarrier)` — 再生成の開始
- `auto-sync: done - applying carrier revisions` — 成功。続けて差し替えが走ります
- `carrier override: slot N token '...' -> <file> (re-equip to apply)` — 差し替え完了。**再装備で反映されます**
- **[error]** `auto-sync: ... failed / timed out / still running after Ns` — 再生成失敗。詳細は `CEF_sync.log` を確認
- **[warn]** `carrier override: slot N carrier '...' missing/invalid on disk - keeping ESP default` — 生成物が見つからず既定キャリアに退避
- **[warn]** `carriers.json parse failed / field type error - keeping ESP-default carriers` — carriers.json 破損時のフェイルセーフ
- `persist carrier: '...' model -> <file>` — 頭部(HDPT)キャリアの差し替え
- **[warn]** `persist carrier: ... missing on disk / unknown pool part / part file missing` — 頭部キャリア資材の欠落(スキップして現状維持)
- `persist carrier: production pool + PoC leftovers deregistered (manual remove)` — `cef persist remove` による登録解除

## 4. メッシュ注入(装備変更・セル移動・ロードのたび)
インデントされた行は直前の処理の詳細です。
- `Reconcile: N active item(s) (cef enabled=...)` — 再適用パスの入口
- `bound N bone(s) to FSMP physics-driven node(s) (SMP sway; e.g. ...)` — **成功の合図。SMPで揺れます**
- **[warn]** `remapped N unresolved bone(s) to nearest ancestor (static, no SMP sway; e.g. ...)` — ボーン未解決で静的表示に退避(**揺れません**。キャリア未装着が典型原因)
- `rebind retry queued (+Nms): FSMP carrier may still be attaching` → `rebind retry: re-injecting N static item(s)` — 上記の自動リトライ
- **[warn]** `NOT SHOWN '...' on '...': skin rebind failed (corrupt source bone) / no skinned geometry in NIF` — **表示失敗の最終警告**(メッシュ側の問題)
- **[error]** `LoadNif failed (err=N) for '...'` — NIFファイルの読込失敗
- **[warn]** `skin missing skinData/bones / bone[N] is null / boneWorldTransforms is null` — スキンデータ異常の各段階
- `BODYTRI '...' -> holder (...)` / `BODYTRI: none on ...` — 体型モーフ用データの引き継ぎ有無
- `body morph: ApplyVertexDiff (BODYTRI present / NO BODYTRI ...)` — 体型モーフの適用(既定OFF、`cef morph` または MCM でON)
- `hideshape gate '...' -> dropping N shape(s)` — シェイプ単位の非表示設定の適用
- `bind watchdog: '...' holds a dead merge generation - reconciling` — FSMP再構築に取り残された注入物を検知して自動修復(`cef repair` の自動版)
- `DetachAllInjected[...]: removed N CostumeFW_* node(s)` — 全注入ノードの除去(OFF切替・repair時)
- `DefineBox content='...' token='...'` — box定義の登録

## 5. realbody: — 素体スキン表示
- `realbody: skin '...' (ID, plugin) body addon ... model3p='...'` → `realbody: injected player body addon ...` — プレイヤーの素体を特定して注入(正常系)
- `realbody: skin TXST ... -> N shape(s)` — スキンテクスチャの適用
- `realbody: base skin ... not adopted (...) - using race skin` — カスタムスキン不採用 → 種族既定へ
- **[warn]** `realbody: no skin ARMO on player / has no slot-32 body addon / addon has no model` — 素体の解決失敗

## 6. persist head: / mouth: — 頭部パーツ
- `persist head: + '...' / - '...' (ID)` — 頭部パーツの登録/解除
- `persist head: DoReset3D (facegen rebuild)` — 顔の再構築を発行
- `mouth: '...' registered but ABSENT from facegen head (...) - clean rebuild #N` — **歯が消える既知問題の自動修復**(ロード数秒後に検知して再構築)
- **[warn]** `mouth: '...' still dropped after N rebuild(s) - giving up` — 自動修復を断念(要報告)

## 7. boxes: / custody: / persist / recover: — アイテム管理
- `boxes: AddBox label='...' token='...' content='...'` — box作成
- **[warn]** `boxes: AddBox rejects ...` — 作成拒否(既に別boxが保持、CEF自身のアイテム等)
- **[warn]** `boxes: NewBox - token pool exhausted` — box数の上限に到達
- `boxes: replenished lost token ...` — 売却/ドロップされたトークンの自動再付与
- `boxes: captured N enchant effect(s) for '...'` — 元装備のエンチャント効果をトークンへ複製
- `custody: created hidden store ... / captured 1x '...' / returned stored '...' / fabricated 1x '...'` — 原品(元アイテム)の保管と返却
- **[warn]** `custody: ...` 各種 — 保管庫の不整合に対する防御(通常は自動回復)
- `persist on/off: '...' on this save` — セーブ単位のpersist切替
- **[warn]** `persist: ... rejected / refusing` — カタログ未登録・box重複などによる拒否
- **[warn]** `hide:/gender:/morph:/realbody:/hideshape: '...' is held by no box/persist - ignoring` — 未保有IDへの設定操作を無視
- `recover: granted 1x '...'` — `cef recover` による救済付与

## 8. cosave: — セーブデータ連動
- `cosave: saved N persist item(s) (M unresolved carried)` — 保存。unresolved はプラグインを外した装備の持ち越し分
- `cosave: restored N item(s)` / `cosave: hidden store ... restored` — ロード成功
- **[warn]** `cosave: '...' unresolved - carried for next save` — 該当プラグインが現在外れている(戻せば復活します)
- **[warn]** `cosave: could not restore '...'` / `hidden store ... did not resolve` — 復元失敗
- **[error]** `cosave: corrupt record ... skipped` — co-save破損(該当分をスキップ)
- `cosave: reverted (registry cleared)` — ロード開始時のリセット(正常)

## 9. preset: — プリセット
- `preset: exported <file> (N content)` — 書き出し
- `preset: assigned '...' to persist / to box N (N content)` — 適用
- **[warn]** `preset: '...' has N unresolved content (skipped)` — 一部装備のプラグインが未導入
- **[warn]** `preset: ... already assigned / already captured in ...` — 適用拒否(他のboxが保持中)
- **[error]** `preset: cannot open / JSON parse error / cannot write` — ファイル異常

## 10. コンソールコマンドの出力(`cef ...` を打ったときだけ)
- `cef list` → `active: N item(s)` + ID一覧
- `cef shapes <id>` → `shapes for '...': N shape(s)` + `ON/off <名前> [slot N]`
- `cef morph` → 体型モーフのON/OFF一覧
- `cef headdiag` / `cef hair <id>` → 頭部ボーン診断(開発・検証用)
- MCMでキャプチャした時: `capture: '...' carries attached script '...'` — スクリプト付き装備への注意喚起
