# Nexus 返信ドラフト 2026-08-04: 透明化伝播の報告者向け

状況: backlog 6-1(透明化が CEF アイテムに伝播しない)を実測調査した結果、
オーナー環境(SE 1.6.1170、バニラ透明化呪文)では**再現せず**。エンジンは
CEF 注入ジオメトリにも通常装備と同一の refraction リンクを張っていることを
`cef invisdiag`(v1.6 系に同梱予定)で確認済み。報告者の環境要因を切り分けるための
質問返信。投稿は USER。

---

Thanks for the report — I dug into this properly, and I need your help to
narrow it down, because it does not reproduce on my end.

On my setup (SE 1.6.1170, the vanilla Invisibility spell) CEF-displayed
items turn invisible together with regular equipment. I also verified it
below the surface: I added a diagnostic to the DLL that reads the
renderer's per-piece state, and during invisibility the engine applies the
exact same refraction link to CEF-displayed geometry that it applies to
normal worn armor — and removes it cleanly when the effect ends. So with
vanilla invisibility, propagation is working as intended.

To find what's different in your case, could you tell me:

1. **Which invisibility?** The vanilla spell/potion, or one added or
   changed by a mod (stealth overhauls, custom invisibility effects,
   script-based SetAlpha effects)?
2. **Which items stay visible?** Box contents, Persist items, or both —
   and is it one specific outfit or everything CEF displays?
3. **How visible?** Fully solid, or "distorted but not gone"?
4. **Order of operations:** does it happen when you cast invisibility
   while the costume is already showing — or when you turn a costume ON
   while already invisible? The second case is the one edge I have not
   covered yet, so if that's your scenario, that is exactly the clue I
   need.

The next update ships the diagnostic as a console command
(`cef invisdiag`). Running it once while invisible and pasting the output
here would settle the question immediately.

---

返信受領後: ケース4(透明中の新規表示)なら監査 doc 決着メモの最小レシピで対応、
mod 製透明化なら該当 mod の効果構成(archetype/EFSH の有無)を確認してから判断。
