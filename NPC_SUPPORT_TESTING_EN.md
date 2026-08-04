# NPC Support — In-Game Test Guide (v1.4.0-beta.1)

> For community testers of the **NPC support test build** (GitHub pre-release
> v1.4.0-beta.1). This is the English edition of `NPC_SUPPORT_TESTING.md` with
> the developer-only steps replaced by archive installation. Item IDs (G/P/N/S/F)
> match the Japanese sheet, so reports stay cross-referenceable.
>
> Fill each `[ ]` with **PASS** or **FAIL(+ a short note)**. For any FAIL,
> attach `Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log`.

---

## 0. Setup

- [ ] **0-1 Install the CORE archive** (`CostumeExpansionFW-1.4.0-beta.1.7z`) as a mod
  (fresh install, or let it fully overwrite an existing CostumeExpansionFW install).
  Requirements: Skyrim SE/AE **1.6.1170**, SKSE64, Address Library, RaceMenu, SkyUI.
  Optional: FSMP (for cloth physics on injected content).
- [ ] **0-2 Install the NPC ADD-ON archive** (`CostumeFW_NPC-1.4.0-beta.1.7z`) as a
  **separate** mod. Enable the mod AND the `CostumeFW_NPC.esp` plugin (ESL-flagged —
  costs no load-order slot). Never merge it into anything.
- [ ] **0-3 Log banner**: after launch, the top of `CostumeExpansionFW.log` shows a
  fresh build stamp, `runtime: Skyrim SE 1.6.1170.0`, and
  `Character::Load3D hook installed (0x6A)`.
- [ ] **0-4 MCM sanity**: the "Costume Expansion FW" MCM has a new **"NPC"** page
  (a short notice pointing to the SKSE Menu Framework UI). If you also use SKSE
  Menu Framework, the SMF menu has a new **NPC** section.
- [ ] **0-5 Safety**: test on a **disposable save**. If anything goes badly wrong,
  create an empty file `Data\SKSE\Plugins\CEF_DISABLE.txt` — CEF becomes fully
  inert and any save opens clean.

---

## 1. Regression gate — existing PLAYER features must be unchanged

**Please run these first**; the NPC feature was built on a refactor of the player
core, and proving the player side unchanged is the top priority.

- [ ] **G1 box**: equip a box token → contents show; unequip → hidden. `cef list`
  output sane (entries now carry an actor name — that's intended).
- [ ] **G2 persist**: persist items stay shown. With a head-carrier set (wig etc.),
  save/load → **teeth are intact** (`cef headdiag` if in doubt).
- [ ] **G3 hide-when-worn**: a content with a hide rule (e.g. nails hidden by boots)
  disappears when the gating armor is worn, returns when removed.
- [ ] **G4 RaceMenu**: open/close and change sex → content re-injects with the
  correct-sex model.
- [ ] **G5 lifecycle**: cell changes, fast travel, save/load, and loading a
  DIFFERENT save → injections restore, no double meshes.
- [ ] **G6 physics**: re-equipping a box token restores cloth sway (FSMP users).
- [ ] **G7 morph/real-body**: body-morph opt-in content follows sliders; real-body
  content shows the correct skin tone/texture.
- [ ] **G8 abilities**: box equip grants its stat spell; unequip removes it; the
  MCM/SMF master toggle removes and restores it too.
- [ ] **G9 city walk**: with NO NPC assignments yet, walk a full loop of Whiterun or
  Solitude → no CTD, no log spam, no stutter.

---

## 2. Publish-for-NPC

- [ ] **P1 publish**: box page → **[Publish for NPC]** → a "Costume: <label>" token
  appears in your inventory, the source box leaves the box list, and its slot
  token is freed (you can create a new box).
- [ ] **P2 give & wear**: hand the token to a follower (gift menu or a follower
  manager) and have them equip it → the costume displays on the follower.
  For SMP content, one **[Refresh]** press on the NPC page is allowed for the
  physics to settle.
- [ ] **P3 sex**: a male NPC wearing it shows the male-baked mesh; forced-gender
  contents stay forced.
- [ ] **P4 add-on removal resilience**: save while a follower wears one; disable
  `CostumeFW_NPC.esp`; load → **no CTD**, UI shows an "add-on not installed"
  notice; re-enable the esp → distribution state comes back.
- [ ] **P5 recall**: **[Recall]** pulls every tracked copy back to the player.
- [ ] **P6 unpublish**: right after a recall, **[Unpublish]** is enabled → restores
  the normal box (with its per-content settings) and the token reverts to
  "Costume (unpublished)".
- [ ] **P7 hide**: **[Hide]** blanks the costume on all wearers (token stays worn);
  **[Show]** restores it.
- [ ] **P8 cap**: with `maxNpcInjected` (default 8) exceeded, the 9th wearer is
  tracked but not injected, and the NPC page says so.
- [ ] **P9 master toggle**: while a follower wears a published costume, switch the
  master toggle OFF → the costume vanishes AND "Costume Stats: <label>"
  disappears from the follower's Active Effects; ON restores both.
- [ ] **P10 MCM stub**: the MCM "NPC" page text renders; with the add-on esp
  disabled it also shows the missing-add-on note.
- [ ] **P11 player wear**: you can wear a publish token yourself → third-person
  display (no first-person view of it — by design).
- [ ] **P12 console**: `cef pub` lists publishes; `cef pub diag` shows the box
  TokenPool count unchanged (no publish tokens leaked into the box pool).

---

## 3. NPC persist (always-on costume for a chosen NPC)

- [ ] **N1 assign**: NPC page persist section → **[Use crosshair target]** on an NPC
  → add a content from the persist catalog → it displays immediately.
- [ ] **N2 auto-restore**: strip the hidden carrier via a follower manager/outfit
  change → it re-equips within ~30 s. It must NOT oscillate (rapid
  equip/unequip loops); after 3 failed restores it shows "suspended".
- [ ] **N3 lifecycle**: the assignment survives cell changes and save/load.
- [ ] **N4 unassign**: removing the assignment unequips and deletes the hidden
  carrier and frees the pool slot.
- [ ] **N5 invisibility**: the hidden carrier item never appears in trade / gift /
  pickpocket / loot UIs.
- [ ] **N6 console**: `cef npcpersist list` shows the assignment.

---

## 4. Deeper probes (if you have time)

- [ ] **S2 slot stamping**: publish a body-slot (32) box → an NPC equipping it
  unequips their cuirass; removing it restores.
- [ ] **S4 two wearers**: the SAME publish token on two NPCs at once → both display
  and animate; unequipping one doesn't disturb the other.
- [ ] **S5 skin matrix**: real-body content on a female Nord, male Nord, Khajiit and
  Argonian follower → skin matches each. (Beast-race issues are useful reports,
  not necessarily blockers.)
- [ ] **S6 remote actors**: with a wearer left in a distant cell, toggle master
  OFF/ON → loaded actors update instantly, the remote one converges when it
  loads, no errors in the log.
- [ ] **S7 memory**: 3 NPCs injected with morphs for ~30 minutes → note commit-size
  growth if any (please report numbers rather than impressions).
- [ ] **S8 mannequins** (fun): give a mannequin a publish token — does it display?
- [ ] **F4 registry sweep**: after dismissing a follower who wore a costume and
  traveling far away, `cef list` no longer shows their entry (it returns when
  they re-equip).

---

## 5. Reporting

Please include with every report:

1. The **first ~5 lines** of `CostumeExpansionFW.log` (build stamp + runtime line).
2. The item ID (e.g. `P6 FAIL`) and what you saw vs. expected.
3. For crashes: the crash logger output if you have one, and whether
   `CEF_DISABLE.txt` let the save open again.
4. Your setup basics: game version, follower manager used, FSMP yes/no,
   RaceMenu version.

File reports as GitHub issues on this repository. Thank you for testing!
