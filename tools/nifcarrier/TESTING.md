# nifcarrier 実機テスト事項（タスク #3：carrier の実機検証）

`nifcarrier` が生成する NIF は、ヘッダ・extra data・ボーン数・親子階層がソースと一致し
自己検証を通っている。だが**唯一の未検証点**は一つに集約される：

> **NiflySharp が書き出した NIF を、Skyrim エンジン／FSMP が実際に読み込んで処理するか。**

これは起動中のゲーム＋FSMP でしか確認できない（B §9-4）。以下はそれを最小コストから
段階的に潰すためのテスト手順。**T0 → T1 → T2 → T3 の順で、前段が緑になってから次へ**。

---

## 0. 共通セットアップ

- **ビルド**: `cd tools/nifcarrier && dotnet build -c Release`（初回のみ nuget から `Nifly` 取得）。
- **実行**: `dotnet bin/Release/net9.0/nifcarrier.dll <command> ...`
- **CEF ログ**: `Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log`
  （判定の主軸。`cef headdiag` の列挙と `bound N bone(s) to FSMP physics-driven node(s)`）。
- **carrier の配置**: 生成した `.nif` を MO2 の CEF mod 配下（例
  `mods\CostumeExpansionFW\meshes\CostumeFW\<name>.nif`）に置く。
- **トークン ARMA への割当（houseCARL）**: `CostumeFW_Boxes.esp` の対象トークン ARMA の
  `WorldModel[1].File` を carrier の相対パスへ差し替え。共有 ARMA `000800` を差し替えると
  全 15 トークンに乗る（PoC は 1 トークン装備で足りる）。本番は per-token ARMA を切る。
- ~~タイミングの鉄則（B §9-9 実測）~~ → **不要になった（2026-07-02）**: CEF に rebind 自動リトライを
  実装済み（3p rebind が static fallback に落ちた item を記録し、1秒後に detach→再注入。外部トリガ
  ごとに最大2回）。注入と carrier attach の順序をユーザーが管理する必要はない。ログの並びは
  `remapped →  rebind retry queued (+1000ms) → rebind retry: re-injecting → bound`。

---

## T0. NiflySharp 出力の engine 互換性（全ての土台）

**狙い**: carrier 以前に「NiflySharp の writer が吐いた NIF をエンジンが描画するか」を、
geometry 付きの無変更 round-trip で確認する。ここが赤なら carrier 検証に進む意味はない。

1. `nifcarrier passthrough <content>.nif <out>.nif`（例 `CPL_VeilA.nif`）。
2. `<out>.nif` を、その content が実際に参照するパスに上書き配置（バックアップを取る）。
3. ゲームで対象を装備／表示。

| 判定 | 合格 | 失敗時の見立て |
|---|---|---|
| 見た目 | 元と同じく正常表示 | CTD・不可視・化け＝NiflySharp writer が非互換 → NifSkope 経由の手作業にフォールバック |

✅ T0 合格 = 以降の carrier/merge/anchor 出力も**書式としては**エンジンに通ると確定。

---

## T1. レベル2 不可視 carrier（方策B・box クラス）✅ 完全成立（2026-07-02 run5）

**結果**: フル NIF zero-alpha carrier で**不可視＋SMP 揺れ＋コリジョンがオリジナル同等**を実機確認。
製品 recipe＝`nifcarrier zeroalpha <content>.nif <carrier>.nif` 一発（shape 削除なし）。以下は手順の記録。

**狙い**: geometry を最小化した carrier がトークン装備で FSMP に物理を作らせ、二重表示（ほぼ）なしに
CEF 注入メッシュが揺れる（B §10-1）。レベル1（丸ごと流用）は B §9-9 で成立済み。

> **⚠️ 実機確定（2026-07-02）**: `carrier`（全 geometry 除去、shapes=0）は**①不発火**。skinned shape の
> 無い armor に対してはエンジンが biped-attach 関数（15535）自体を呼ばず、FSMP のどちらのイベントも
> 発火しない（機構は FSMP_REINVESTIGATION.md §2-1）。→ **`keep1`（最小 skinned shape を1つ残す）を
> 使うこと**。`keep1` での①発火は **run3 で実機成立済み**（bound 23／`Armor_<id>` 105本／T0 合格）。
> `carrier` の出力はボーン和集合のベース素材としては有効。

1. **`nifcarrier zeroalpha <content>.nif <carrier>.nif`（オリジナル NIF 直・shape 削除なし＝製品 recipe）**。
   `VERDICT shaderAlphaZero=True blendFlags=True` を確認。
   - **⚠️ shape を削ってはいけない**: SMP XML はコリジョンメッシュを shape 名で参照する
     （`per-triangle-shape`/`per-vertex-shape`、例: `VirtualHead`＝shader 無しの不可視プロキシ）。
     keep1（1 shape 残し）で run4 に**コリジョン差分**が出た実測あり。keep1 は「発火に skinned shape が
     必要」の診断用として残置。
   - zero-alpha が実機で効かない場合は `collapse`（全頂点原点潰し・第二候補）に差し替え。
2. carrier を配置し、トークン ARMA の `WorldModel` を carrier パスへ（houseCARL）。
3. その content を box の content に add（**同一 content NIF＝ボーン名一致**）。CEF 無効中に行う。
4. ゲーム起動 → トークン装備 → `cef headdiag`。
5. CEF 有効化 → `Reconcile`。

| チェック | 期待 |
|---|---|
| `cef headdiag`（トークン装備後） | `hdtSSEPhysics_AutoRename_Armor_<id> <bone>` が列挙される |
| CEF ログ（CEF 有効化後） | `bound N bone(s) to FSMP physics-driven node(s) (SMP sway; ...)` |
| 見た目（3人称） | **トークンの二重表示が消え**、注入メッシュだけが揺れる |
| 安定性 | 脱着・セル移動・再ロードで bound 維持（id は逐次なので毎回 suffix マッチで再解決） |

失敗切り分け: 改名ボーンが出ない→ARMA 割当か XML パス。bound が出ず static→タイミング順序（手順3-5）。
二重表示が残る→carrier が geometry を含む（`dump` で `shapes` を確認）。

---

## T2. レベル3 merge carrier（方策B・1トークン＝多重 content）✅ 成立（2026-07-02 run6）

**結果**: merged carrier＋統合 XML で単一 `Armor_<id>` に 107本（veil 105＋tail 2、tail は NPC Pelvis 配下）。両 content が bound し両方の揺れを目視確認。carrier 不可視。

**狙い**: 複数 content のボーン和集合を1 carrier に載せ、1トークンで全 content が揺れる（B §10-2）。

**recipe（全ステップ自動化済み・2026-07-02）**:
1. `nifcarrier mergexml <merged>.xml <content1>.xml <content2>.xml [...]`
   - FSMP パーサの無名 `<bone-default>` 汚染を防ぐため、冒頭で工場出荷テンプレートを
     `cef_factory` として保存し、各ドキュメント境界でリセットを挿入する（実装済・自動）。
   - 重複 bone は先勝ちで自動 skip（FSMP パーサ自身も skip するので整合）。
   - `per-*-shape` 名の重複は WARNING＝content 間で shape 名が固有であること。
2. `nifcarrier zeroalpha <base_content>.nif <tmp>.nif`（**per-shape コリジョンを持つ content を base に**）。
3. `nifcarrier merge <tmp2>.nif <tmp>.nif <content2>.nif [...]`（ボーン和集合＋**add 側の shape も
   CloneShape で複製**〔2026-07-02 増設〕＝全 content の per-*-shape コリジョン参照が carrier 内で解決。
   shape 名が content 間で衝突する場合は先勝ち＋WARNING）。
   ※ sync のパイプライン順序は merge → zeroalpha → setxml（複製 shape も透明化するため）。
4. `nifcarrier setxml <tmp2>.nif <carrier>.nif meshes\...\<merged>.xml`（extra data を統合 XML へ）。
5. carrier＋XML を配置、トークン ARMA に割当、box に全 content を add → T1 と同じ手順で検証。

**判定（追加分）**:

| チェック | 期待 |
|---|---|
| `cef headdiag` | 全 content のボーンが**同一 `Armor_<id>`**（1トークン＝1 addArmor）で列挙 |
| CEF ログ | 全 content 分の bound 合計 |
| 目視 | 複数 content が同時に揺れる |

注意: 2 content が**同名ボーンで別挙動**を意図する場合のみ単一ノードに潰れて競合。固有名なら実害なし。

---

## T3. head carrier（方策C・facegen ②経路）— ツール側は完成（2026-07-04 stage 2a）

**狙い**: 頭部（facegen ②）経路で physics キャリアを head part として焚き、体パーツの物理鎖は
`NPC Pelvis` 等の体アンカーから生やす（C §9-10）。C の active PoC stage2 に相当。

**stage 2a（2026-07-04）でツール側の前提は全て自動化済み**:
- **`headcarrier <out>.nif <content>.nif [c2 ...] [--xml <mergedXml>] [--anchor <liveBone>]`** — 一発合成。
  base 選定（トリガ shape を持つ content を merge base に＝in-place 保持で CloneShape 破損の影響外）→
  merge → zeroalpha → R3 孤児ボーン補完 → anchor → xml repoint → R1 ルート付け直し → validatehead ゲート。
- **`validatehead <nif>`** — head 経路の 3 ゲート＋CTD ゲート＋**チェーンアンカーレポート**:
  - R1: HDT extra data が**ルート直付け**（scanBBP はルートの extraData しか歩かない）
  - R2: **カスタムボーンを skin 参照する skinned shape ≥1**（無いと doSkeletonMerge 不発火 = REINVESTIGATION §2-2）
  - R3: skin 参照カスタムボーンが**ノードツリーから到達可能**（skin-only 孤児は merge されない）
  - `chain top 'X' -> anchors under 'Y'` — 各物理鎖の**実効 merge 先**をオフラインで表示（doSkeletonMerge は
    live 実在名をグローバル解決するため、実効アンカー＝チェーン先頭の直近 named 祖先）。
- **`anchorsmart`** — `anchor` の改良版: 既存同名ノードを再利用し、live 名の枝（`NPC`/`NPC *`/`CME *`…）は
  移動しない（標準ツリー巻き込み問題を解消）。

**オフライン検証済み（2026-07-04）**:
- Steammist（②発火の実機実績あり）→ validatehead 全ゲート OK・チェーンは `NPC Head [Head]` 配下
  （**§9-9 実機 headdiag と一致**＝validator の較正完了）。
- **Box44 実 merged carrier（5 content）→ そのまま全ゲート OK**。実 SMP 衣装は体アンカーを自前 scaffold
  している（panty/bunnytail→`NPC Pelvis`、boa→`NPC L/R UpperArm`・`Spine2`、hat/veil→`NPC Head`）＝
  **§9-10 recipe は実在 content の標準形。persist キャリアは sync の merge 出力をほぼ流用可能**。

実機手順（＝stage 2c、engine 側の未確定点 (a)(c) の判定）:
1. `headcarrier` でキャリア生成（単一 content なら `--xml` 不要）→ MO2 の CEF mod 配下に配置。
2. HDPT レコードを作成（houseCARL）: 非衝突 type（`kMisc` 本命）・NotPlayable・Model=キャリア。
3. head part 登録＋`DoReset3D`（`cef hair` 系 PoC コマンド流用）。**登録後は CEF 側で明示 `Reconcile()`**
   （C §9-11 (ii)：Load3D フック 0x6A 非発火）。
4. `cef headdiag`。

| チェック | 期待 |
|---|---|
| `cef headdiag` | `hdtSSEPhysics_AutoRename_Head_<id> <bone>` が validatehead の chain top レポートどおりのアンカー配下に出る |
| CEF ログ | content の `bound`（体パーツは骨盤等から揺れる） |
| 見た目 | スカート等が頭からでなく腰から追従して揺れる |

失敗切り分け: ②自体が不発火→head part 登録法（どの type が列挙されるか＝C stage2 (c)）。ボーンは出るが
キャリアの XML/ボーンでない→FMD 不付与で facegeom フォールバックに落ちている（C stage2 (a)、§9-10 前提）。

---

## まとめ（このドキュメントの位置づけ）

- **T0/T1 は今すぐ実行可能**（ツールは完成、生成物は自己検証済）。ここが緑なら方策B（box）の custom SMP は
  実質製品化ライン。
- **T3 のツール側残作業はゼロ**（2026-07-04 stage 2a: `headcarrier`/`validatehead`/`anchorsmart`）。残るは
  **head part 登録法（ESP HDPT＋CEF 側トリガ）のみ**。(a) FMD 付与・(c) 列挙は engine facegen 依存で、
  この active PoC でしか確定しない（C §9-11）。
- 参照: [FSMP_APPROACH_B.md](../../FSMP_APPROACH_B.md) §9-9/§10、[FSMP_APPROACH_C.md](../../FSMP_APPROACH_C.md) §9-9/§9-10/§9-11、[FSMP_INTEGRATION.md](../../FSMP_INTEGRATION.md)。

---

## 既知の制約: スケルトンのボーン数上限（2026-07-03 実機確定）

マージ型 carrier は box の全 content の**カスタム物理ボーン和集合**をライブスケルトンへ追加する。
box に SMP content を積み上げるとボーン数が線形に増え、**エンジンのボーン上限に到達すると一部の
skinned メッシュだけ物理が死ぬ**（実測: box44 に7点前後で arm SMP が「ロード後ですら」不動作 →
content 除去で復活）。「揺れない」報告の切り分けでは **carrier 側の bind ログが正常でもボーン上限を疑う**こと。

- ✅ **解決（2026-07-04・FSMP_BLE.md）**: BLE（Bone Limit Extender）の導入で上限は実質撤廃。
  「BLE が認識する形式で焼く」必要は**無い**（ソース精読で確定 — BLE はスケルトン側の上限を広げるだけで、
  nifcarrier の出力は従来どおりでよい）。300 本目安の運用制約は撤回。
  （任意の将来改善: sync にボーン合計の目安警告を出す — 未実装・BLE 環境では実害なし）

---

## 既知の制約: NiTriShape 系 SMP 衣装はゼロ除算 CTD（2026-07-03 実機確定）

**旧形式 `NiTriShape` を使う SMP 衣装を box に入れると、ゼロ除算によるエンジンレベルの CTD** が発生する
（ユーザー実機確認）。carrier パイプライン（merge の CloneShape / zeroalpha）を通った NiTriShape で
エンジンのスキニング系が 0 除算に落ちるとみられる（発生箇所の特定は未実施）。

- ✅ **対策 1 実装済み（2026-07-04・validate ゲート）**: `sync` は個別 content を検証して壊れるものを
  除外＋WARNING、組み上がった carrier も最終 validate を通ってから atomic publish（不合格なら前回良品を
  温存）。`nifcarrier validate <nif>` 単体診断もあり。→ NiTriShape content は CTD せず自動除外される
  （その content は表示のみ・揺れず）。
- 残候補（未実装・必要になったら）:
  1. NiTriShape→BSTriShape **変換**（SSE NIF Optimizer 相当）を nifcarrier に実装（大きめの工事）。
  2. ゼロ除算の発生箇所を特定してピンポイント回避（要デバッガ/クラッシュログ解析）。
