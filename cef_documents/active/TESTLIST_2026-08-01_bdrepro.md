# TESTLIST 2026-08-01 — BD Ungulates によるローカル再現(sub-second stomp)

目的: 報告者の実 mod(鹿種族)+実損傷状態(テクスチャ 6 枚欠落)で
「attach 後 1 秒未満のホルダー children stomp」をローカル再現し、
**test.4 の frame containment に現行犯逮捕させる**(SCENE CORRUPTION +
hexdump = 書き手の指紋。CTD でも 106350/106353 --near 照合で同定可)。

## 静的調査の確定事項(2026-08-01、ゲーム不要分は完了)

- mod: `K:\Mo2_SkyrimSE1170\mods\Ungulate Races - Horse-Deer-Minotaur`
  (BDUngulates.esp 770KB・ルーズファイル・BSA/DLL なし・pex 4 本)
- **報告者の MISSING 6 枚は mod に全部同梱**(ESP 内 TXST パスとも完全一致)
  → 報告者側は部分破損=再インストールで直る(PM 文面を強化済み)。
  欠け方(diffuse 残存・msn/sk/s 欠落)はテクスチャ最適化ツールの食い残しが典型。
- **鹿種族の skeleton = vanilla パスの skeletonBeast_female.nif**(crash 18 と一致)。
  crash 19 の skeleton_female は **proxyRaces**(NewNord 等の互換プロキシ、
  BDUngulateRaceController.pex に列挙)で説明が付く=標準スケルトン+鹿スキンは正規状態。
- **BDMinoOverlays.pex = RaceMenuBase 拡張で body/hand/feet/face paint を登録**
  (鹿用: Unisex Fawn Spots / Gazelle Stripe)。skee SOvl 機構をこの mod が正面から使う。
  ※ SOvl シェイプ自体は skee が全身分を常設(ペイント未選択でも default.dds で存在)。

## §0 環境(ゲーム前に確認)

- MO2 プロファイル: `test CEF bug`(**FSMP 4.0.1 が 3.5.0 より上**・MARA 無効のまま)
- `Ungulate Races - Horse-Deer-Minotaur` を有効化 + BDUngulates.esp 有効
- test.4 DLL(compile **Aug 1 2026 06:07:44**)+ `bDebugMode=1`
- 起動ログで確認: compile スタンプ / `Update containment hook installed (0xAD)` /
  hdtSMP64 のサイズが 4.0.1 のもの(報告者実測 4,231,168 / 2026-07-05)

## §1 ベースライン(テクスチャ健全のまま)

1. 新規 or showracemenu で **女性の鹿種族**(RaceMenu の race リストで "Deer";
   出なければ proxy 側でも可)。body paint に **Unisex Fawn Spots** を適用
   (オーバーレイに実コンテンツを持たせる)
2. persist add を 5〜10 回(SMP 物+static 物を混ぜる)+ 装備替え数回
3. 期待: CTD なし・SCENE CORRUPTION なし(§1 が汚れたらそれ自体が大発見)

## §2 損傷模擬(報告者状態の再現)

ゲームを閉じてから 6 枚をリネーム:

```powershell
$bd = "K:\Mo2_SkyrimSE1170\mods\Ungulate Races - Horse-Deer-Minotaur\textures\actors\character\BDDeerTextures\Female"
Get-ChildItem "$bd\Hands\FemaleHands_msn.dds","$bd\Hands\FemaleHands_sk.dds","$bd\Hands\FemaleHands_s.dds","$bd\Feet\FemFeet_msn.dds","$bd\Feet\FemFeet_sk.dds","$bd\Feet\FemFeet_s.dds" | Rename-Item -NewName { $_.Name + ".cef_bd_off" }
```

1. 同キャラで起動(overlay 再構築のため RaceMenu を一度開閉 or 装備替え)
2. persist add を 10〜20 回・equip/unequip churn・セル移動を混ぜる
3. 観察分岐:
   - **SCENE CORRUPTION + 生存** = 大当たり。ログの hexdump・タイムスタンプ・
     直前イベントを確保(BUGREPORT の判定表どおり)
   - **CTD** = crash log を `tools/addrlib/parse_versionlib.py --near` で
     106350/106353 照合。一致なら報告者機序のローカル再現成立
   - **無風** = 陰性。未一致の変数を記録して §3 へ:
     skee 版(ローカル= BodyMorph v4/2024-01、報告者= **v5/2026-04-18 2,521,088 bytes**
     — overlay 内部が別物の可能性が高い、次の第一変数)/ cbp.dll 同居 /
     Relight・intellightent 等ライト系 / メモリ圧(報告者は private 19-21GB)

## §3 復元(必須・§2 の直後に)

```powershell
$bd = "K:\Mo2_SkyrimSE1170\mods\Ungulate Races - Horse-Deer-Minotaur\textures\actors\character\BDDeerTextures\Female"
Get-ChildItem $bd -Recurse -Filter "*.cef_bd_off" | Rename-Item -NewName { $_.Name -replace '\.cef_bd_off$','' }
```

復元後 `Get-ChildItem $bd -Recurse -Filter "*.cef_bd_off"` が 0 件を確認。

## メモ

- §4(07-31 の Succubus 模擬)が陰性だった差分: 種族が本物(skeletonBeast 系)/
  スキン TXST が本物 / ペイント適用を明示 / FSMP 4.0.1。それでも陰性なら
  skee v5 導入が次の一手(報告者との最後の大きな版差)。
- PM 送付はこの再現と**並行**でよい(報告者を単一障害点にしない方針のまま)。
