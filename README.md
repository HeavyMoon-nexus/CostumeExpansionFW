# Costume Expansion FW (CEF)

An SKSE framework for Skyrim SE/AE (1.6.1170, CommonLibSSE-NG) that **shows skinned
accessories and costumes without consuming their biped slots**. It clones an item's
skinned geometry and re-binds it directly to the live skeleton, bypassing the equip
system — so several items can share one slot, and an accessory's original (often
contested) slot is freed. Effectively a slot-budget expansion for the 32 fixed biped
slots (30–61).

Two display classes:

- **Persist** — always-on accessories (nails, piercings). No token, no slot gate.
  The persist list is a **catalog shared across saves**; each save activates the
  entries it wants to show (per-row MCM toggle) — a new character starts clean.
- **Box** — token-gated costumes. Equipping a box's invisible token shows its packed
  contents; a strip mod unequipping the token hides them (automatic compatibility).
  Multiple contents can ride on a single box token (one slot). A slot-31 box
  (**"Costume Box 31: Hair (Wig)"**) masks your real hair while worn — put an
  equipment wig in it.

Captured items keep their individual data (tempering / player enchantment) in a hidden
holding container. A worn box token also passes through its contents' **armor, weight,
armor class, enchantment effects, and keywords**, so the costume behaves like real
worn gear.

## Requirements

- Skyrim SE/AE **1.6.1170**
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080) (skee — required for body-morph follow and the SkyUI MCM SDK)
- SkyUI (MCM) — optional, and on its way out. The MCM is still here in 1.6.4 and
  still works; see "Which UI" below for what it no longer reaches
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
  — the main UI
- `CostumeFW.esp` (ships with the mod — ESL-flagged, no load-order slot; box
  tokens, persist head-part pool, MCM quest)
- `CostumeFW_BoxPool1.esp` (ships with the mod from 1.6.4 — ESL-flagged, no
  load-order slot; three more box tokens per biped slot, so a slot can hold
  several boxes)
- *(optional)* [Faster HDT-SMP (FSMP)](https://www.nexusmods.com/skyrimspecialedition/mods/57339)
  — custom-bone SMP cloth physics on injected content (3.5.0 tested)

### Skyrim VR (community beta)

The same DLL runs on Skyrim VR 1.4.15 (built multi-runtime via CommonLibSSE-NG).
There is no VR build and there never was: the DLL works out which engine loaded
it, and the old **VR Patch** archive shipped that same file. From 1.6.3 there is
one installer — pick Skyrim VR in it and you get `README_VR.txt`; the DLL, the
plugins, the meshes and the scripts all come from the core install. If the old
VR Patch is still installed as a separate mod, remove it: it holds a DLL at the
same path, so depending on mod order it can put the old one back over the new.

Swap the runtime requirements:

- [SKSEVR](https://skse.silverlock.org/) 2.0.12
- [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101) — required
- [Skyrim VR ESL Support](https://www.nexusmods.com/skyrimspecialedition/mods/106712) —
  required (`CostumeFW.esp` and `CostumeFW_BoxPool1.esp` are ESL-flagged with a 1.71
  header; vanilla VR loads neither. It covers the optional `CostumeFW_NPC.esp` too;
  `CostumeFW_Abilities.esp` is a full plugin and needs nothing special)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
  \+ [ImGui VR Helper](https://www.nexusmods.com/skyrimspecialedition/mods/183466)
  — the main UI on VR as well; see "Which UI" for what the MCM no longer reaches
- [SkyUI VR](https://github.com/Odie/skyui-vr) (the legacy MCM)
- [RaceMenu VR 0.4.14](https://www.nexusmods.com/skyrimspecialedition/mods/19080) (skee)
- *(recommended)* [VRIK Player Avatar](https://www.nexusmods.com/skyrimspecialedition/mods/23416)
  — vanilla VR renders no player body, so without it you can't see your own costume

One behavioral difference: tempering on VR uses the raw improvement multiplier
rather than the engine's item-card measurement, so a captured item's armor can
read slightly higher than the same item does on SE/AE.

VR support is community-tested (no VR rig here); `CEF_DISABLE.txt` (see
"Turning CEF off from outside the game", and read the note about saving there)
makes the plugin fully inert if anything misbehaves, and the log's first lines
(`runtime: Skyrim VR 1.4.15.0` + build stamp) identify the build in reports.

## Which UI

CEF has two menus that read the same settings: the SkyUI MCM and the SKSE Menu
Framework (SMF) section, both named "Costume Expansion FW". **Use SMF.** The MCM
is behind and will be removed; earlier notes named 1.6.3 and then 1.6.4 for that
and both slipped, so this no longer names a version.

What the MCM does not reach: NPC distribution, the capture blacklist
(**Blocked**), the **Recovery** page, box renaming, the per-item passthrough
toggles, and the verbose-logging switch.

Two things it could not do in 1.6.4 and can now: a biped slot holding more than
one box gets a page per box again (its pages are keyed by the box, not the slot,
so a shared slot names each box by its label), and the Persist catalog is shown
a screen at a time instead of running out of SkyUI option positions and handing
the last rows no controls. The page list itself stops at 122 boxes, which is a
Papyrus array limit; past that the Boxes page says how many have no page and SMF
has them all.

On VR, SMF needs
[ImGui VR Helper](https://www.nexusmods.com/skyrimspecialedition/mods/183466).

Nothing about your setup is tied to either menu: boxes, presets and settings
live in `CEF_settings.json` and your save, so switching menus changes nothing.

## Usage (MCM: "Costume Expansion FW")

- **Main** — master on/off, dependency status, version.
- **Persist** — `+ Add worn item` / `+ Add from inventory` capture an accessory (from
  your equipped items, or straight from your inventory — no need to equip first; the
  **Inventory filter** input narrows the list by name) into the shared catalog and
  activate it on this save. Per row: **active-on-this-save** toggle (visual only —
  never moves items), **Body morph** toggle, **Body** (forced-gender NIF) menu,
  hide-when-worn slots, and remove from catalog. Entries another character removed
  from the catalog stay active here until you deactivate them (which returns the item).
- **Boxes** — `+ New box` picks a free slot; per box: Distribute token, Wear (show/hide),
  `+ Add worn item` / `+ Add from inventory` (capture into the box; same filter input),
  Armor type, **Preset** (assign a preset's contents), **Export as preset**, Stats,
  Delete, and per-item remove / Body morph / Body (forced NIF) / hide-when-worn.
- **Presets** — lists installed `CEFP_*.json` presets; assign one to a box.
- **Diagnostics** — read-only status: dependencies, last carrier auto-sync result,
  per-box carrier revisions, persist registration, churn counters.

Presets (`Data\SKSE\Plugins\CEF\Presets\CEFP_*.json`) are the shareable unit — a named,
human-readable set of content. Settings live in `Data\SKSE\Plugins\CEF_settings.json` —
a **catalog shared across saves** (box definitions, per-content options); which persist
entries actually *show* is per-save, stored in the co-save.

**Known limitations:**
- Items are tracked by their *base form* — two individuals of the same item (e.g.
  differently enchanted copies) cannot be captured separately; the second is rejected
  as a duplicate. The captured copy's player-enchant effects are snapshotted and
  applied through the token/persist ability.
- SMP items whose physics is wired **only** through FSMP's global `defaultBBPs.xml`
  (no inline HDT extra data in the NIF) are not detected as SMP — they inject without
  physics (static). The carrier sync log says so per item.

## Turning CEF off from outside the game

Both switches below are read at startup, before CEF touches anything, so you can set
either one without launching the game. They are not interchangeable. Pick by what you
are trying to do.

### Stop injecting, keep the save data (`CEF_settings.json`)

Set `"enabled": false` in `Data\SKSE\Plugins\CEF_settings.json`. This is the same switch
as the Main page toggle. Nothing is injected and the enchantment abilities come off, but
the hooks, the box definitions and the co-save data all keep working, so setting it back
to `true` gives you your costumes back exactly as they were.

One thing it does not touch: a box token you are still wearing stays worn, and it is a
real piece of equipment, so its armor rating, weight and passthrough keywords stay in
effect while the costume is invisible. Take the token off if you want those gone too.

Use this when you want to play without CEF's costumes, or when the crash you are chasing
happens while something is worn.

### Hard off, for a save that will not load (`bEnabled=0` / `CEF_DISABLE.txt`)

- `Data\SKSE\Plugins\CostumeExpansionFW.ini` → `[General]` `bEnabled=0`
- or create an empty `Data\SKSE\Plugins\CEF_DISABLE.txt` — its mere presence forces CEF
  off, so there is nothing to mistype.

CEF then registers no hooks, no event sinks, no serialization and no Papyrus functions,
loads no boxes, and does no mesh injection or carrier/FSMP work. The plugin is fully
inert, which is what lets a save that a CEF crash left unloadable open again. Use it when
the first switch is not enough — a crash during the carrier build or the persist head
rebuild, for instance, happens whether or not anything is being injected.

**Do not save the game while CEF is hard off.** Registering no serialization means CEF
writes nothing into the save, so that save loses what is in the hidden storage, which
persist items it shows, which NPCs wear a published costume, and which NPCs have a persist
assignment. The storage container is still in the save with the items inside it, but CEF
has no way left to find it. Open the save, check that it loads, then quit to desktop —
fix the cause, re-enable CEF, and load the save again.

## Uninstalling

Before removing the mod, open **Main → Prepare for uninstall** (SMF, or the MCM's copy of
it). This returns every captured item to you, recalls published costumes from NPCs and
strips NPC persist assignments, removes all box tokens from your inventory, detaches the
injected meshes, **and disables CEF persistently** (`enabled=false` in the settings —
reloading or playing on will not re-apply anything). Save, then remove the mod.

It empties the hidden storage: the persist entries this save has switched off, anything
left over from a rollback, and the second and later copies of a stack all come back too.
The one thing it cannot return is an item whose costume plugin is switched off. The game
already took that item out of the storage when the save loaded without its plugin, so CEF
counts those and tells you, instead of saying "safe to uninstall".

**If it says some items could not be returned, don't save.** Those items are still in
the save you loaded, but a save made now won't have them. Quit, turn the plugin back on,
load the same save and run it again.

Running it a second time on the same save is safe. It only hands back what is still in
storage, and the confirmation says so before you proceed. It needs a loaded save, so the
SMF button stays disabled at the main menu. If CEF's DLL is not running (`CEF_DISABLE.txt`,
`bEnabled=0`, or a failed load), the MCM's button empties the storage into your inventory
itself and says so. If it cannot reach the storage either, it tells you not to remove the
mod yet.

**"Re-enable from Main" is not an undo.** It turns CEF back on, but your items are already
back in your inventory. The box definitions still list them, so taking one out of a box
afterwards makes a fresh plain copy instead of finding the original — you end up with two.
If you want the boxes working again, capture the items into them again.

## Known limitations

- **Body morph (RaceMenu sliders)** is per-content **opt-in, default OFF** — turn it on
  in the MCM (or `cef morph <id> on`) for BodySlide body-conforming meshes. Keep it OFF
  for hair/jewelry/accessories: it is wasted work there, and skee's vertex-diff pass on
  a large mesh makes allocations that SSE Engine Fixes' allocator retains (a full wig
  cost ~15GB before this gate existed).
- **Body physics (CBPC / standard-bone SMP)** works on injected meshes (they bind to the
  live, physics-driven bones).
- **Custom-bone SMP cloth physics** (outfit-specific bones — skirts, veils, wigs)
  **works on injected content when FSMP is installed**: CEF auto-builds an invisible
  physics *carrier* from a box's contents (rebuilt on change — re-equip the token until
  the outfit sways) and a head-part carrier pool for persist items (fully automatic).
  The carrier builder is **built into the plugin** (v1.2+) — no external tool or .NET
  runtime required. (Power users: a `Data\SKSE\Plugins\CEF_sync_command.txt` hands the
  build to an external command instead — legacy compat mode.)
  Without FSMP — or for content the carrier validator excludes (legacy `NiTriShape`
  meshes) — such parts render **statically** on their nearest real ancestor bone instead
  of vanishing. Full wigs belong in the slot-31 box, not in Persist (the facegen path
  rebuilds their physics repeatedly at load and the retained allocations balloon memory).
- Content must be weighted to bones present on your live skeleton (standard XPMSSE). Content
  built for a different/extended skeleton may show parts statically (logged as a remap).
- **Published costumes and NPC-persist items use the PLAYER's race for their carrier.**
  A costume that ships separate meshes per race is built from the player-race
  variant, so it can look wrong on an NPC of another race. Boxes on your own
  character are not affected.
- **A mesh path that is not UTF-8 gets no physics carrier.** A costume whose mesh folder
  or file name was saved in a legacy codepage is still captured and shown, but its SMP
  cloth parts render statically: the carrier build is driven by a JSON manifest, and JSON
  has to be UTF-8. Diagnostics lists these under "Mesh paths that are not UTF-8 text".
- **The ability pool is append-only.** A costume whose enchantment changes takes
  a new ability and keeps the old one, because a save may still refer to it. A
  piece that is re-enchanted or tempered often therefore costs more than one.
  `cef abilities` shows how many are left per plugin; installing the next
  `CostumeFW_Abilities` plugin adds more.

## Non-English text in the menu

CEF guarantees valid UTF-8 for every string it shows, but the glyphs come
from SKSE Menu Framework's own font. If item names in your language render
as "?", install a font that covers your language into
`SKSE Menu Framework/SKSE/Plugins/fonts/`, point `PrimaryFont` at it in
`SKSEMenuFramework.ini`, and enable the language under `[Fonts]`. Enable
ONLY the language you play in - all CJK ranges at once can exceed the GPU
texture limit and crash on startup. This is SMF-side configuration; CEF
does not bundle fonts.

## Mod compatibility

- **Strip / unequip mods** work automatically: a box hides when its token is unequipped.
- **Capture blacklist (v1.5.0).** The capture pickers deliberately skip: **runtime-created
  (FF) items** (another mod's invisible host/utility items — reading their inventory data
  can crash instantly, and CEF could never restore them across a save anyway),
  **non-playable armors**, items on the **deny-list** (defaults ship for MARA's
  "CORE Carrier"; add your own on the SMF **Blocked** page), and any armor carrying the
  **`CEF_NoCapture`** keyword — mod authors can tag their utility items via ESP or KID
  (see the included `CostumeFW_NoCapture_KID.ini` template) to keep them out of CEF.
  Since 1.6.4.1 CEF also refuses, with the reason, an item from a plugin whose **file name
  is not UTF-8** (a name saved in a legacy codepage such as cp1252 or Shift-JIS). CEF writes
  the plugin name into its settings file, and that file has to be UTF-8.
- **MARA (Multiple Amulets and Rings... Automatically).** Its "CORE Carrier" host item
  crashed the game when picked in `+ Add worn item` (report: 2026-07-22); v1.5.0 blocks
  it (all four layers above match it). While MARA is running, CFW also **refuses to
  capture jewelry you are currently wearing** — stripping a worn ring/amulet out from
  under MARA crashes inside MARA itself — unequip it first, or capture it from your
  inventory. Two more co-existence notes: (1) a box token never carries `ArmorJewelry`
  (CEF drops every keyword that names an equipment class before stamping the rest onto
  the token), but a slot-35/36 box still occupies the amulet/ring biped slot, so MARA
  may try to manage the invisible token anyway — if an amulet/ring box misbehaves with
  MARA installed, move that box to another slot; (2) CEF natively stacks the looks and
  enchant effects of several jewelry pieces in one box (one slot), which covers the
  common "wear multiple amulets" wish without re-slotting.
- **Community Shaders, decapitation mods, FSMP versions**: audited against the crash
  classes reported around similar frameworks — CEF creates no renderer-facing geometry
  from scratch, installs no blocking UI, and re-derives all form mutations on its own
  ESP records each load. Details: `MARA_CRASH_CLASS_AUDIT.md`.

## Building

CommonLibSSE-NG via vcpkg + CMake (Ninja, MSVC, C++23, triplet `x64-windows-static-md`).

```
build.cmd release
```

`build.cmd` sets up the MSVC/CMake environment and (optionally) deploys the built DLL to
`%SKYRIM_MODS_FOLDER%\CostumeExpansionFW\SKSE\Plugins\`. Adjust the paths in `build.cmd`
to your environment.

The Papyrus scripts live in `papyrus/` (`CFW_Native.psc`, `CostumeFW_MCM.psc`). Compile
them with the Skyrim Papyrus Compiler against an import path that includes the SkyUI MCM
SDK (`SKI_ConfigBase` etc.) and the base game scripts, e.g.:

```
PapyrusCompiler CFW_Native    -import="<SkyUI SDK + base scripts>" -output="Scripts" -flags=TESV_Papyrus_Flags.flg
PapyrusCompiler CostumeFW_MCM -import="<SkyUI SDK + base scripts>" -output="Scripts" -flags=TESV_Papyrus_Flags.flg
```

## Credits

- **RaceMenu / skee** by expired6978 — body-morph integration (`IBodyMorphInterface`) and MCM SDK.
- **CommonLibSSE-NG** (Ryan-rsm-McKenzie / powerof3 and contributors).
- **HDT-SMP family** (hydrogensaysHDT, aers, DaymareOn; FSMP / FlexSMP variants) — body physics that injected meshes inherit via standard bones.
- **SkyUI** team — MCM framework.
- Injection recipe adapted from skee's AttachMesh approach.
- **Baki** — sustained crash-log diagnosis and field testing across the v1.5.x line.
  The custom-race carrier failure (v1.5.2) was found and confirmed on their character,
  and the head-part teeth watchdog got its first real-world run there.

## License

- **Source code of this project**: MIT — see [LICENSE](LICENSE).
- **Distributed plugin binary (`CostumeExpansionFW.dll`)**: **GPLv3**, because it
  statically links [nifly](https://github.com/ousnius/nifly) (GPLv3) for the
  in-process carrier build. Corresponding source is this repository. See
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full component list
  and the packaging checklist.
