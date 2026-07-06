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
- SkyUI (MCM)
- `CostumeFW_Boxes.esp` (ships with the mod — provides the box tokens)
- *(optional)* [Faster HDT-SMP (FSMP)](https://www.nexusmods.com/skyrimspecialedition/mods/57339)
  — custom-bone SMP cloth physics on injected content (3.5.0 tested)

## Usage (MCM: "Costume Expansion FW")

- **Main** — master on/off, dependency status, version.
- **Persist** — `+ Add worn item` / `+ Add from inventory` capture an accessory (from
  your equipped items, or straight from your inventory — no need to equip first) into
  the shared catalog and activate it on this save. Per row: **active-on-this-save**
  toggle (visual only — never moves items), **Body morph** toggle, hide-when-worn
  slots, and remove from catalog. Entries another character removed from the catalog
  stay active here until you deactivate them (which returns the item).
- **Boxes** — `+ New box` picks a free slot; per box: Distribute token, Wear (show/hide),
  `+ Add worn item` / `+ Add from inventory` (capture into the box), Armor type,
  **Preset** (assign a preset's contents), **Export as preset**, Stats, Delete, and
  per-item remove / Body morph / hide-when-worn.
- **Presets** — lists installed `CEFP_*.json` presets; assign one to a box.
- **Diagnostics** — read-only status: dependencies, last carrier auto-sync result,
  per-box carrier revisions, persist registration, churn counters.

Presets (`Data\SKSE\Plugins\CEF\Presets\CEFP_*.json`) are the shareable unit — a named,
human-readable set of content. Settings live in `Data\SKSE\Plugins\CEF_settings.json` —
a **catalog shared across saves** (box definitions, per-content options); which persist
entries actually *show* is per-save, stored in the co-save.

## Disabling CEF from outside the game (recovery)

The MCM master toggle only hides/shows meshes at runtime; the plugin's hooks still run.
For a hard off — and to **recover a save that a CEF crash left unloadable** — use the
external config, read once at startup **before CEF touches anything**, so you can edit it
without launching the game. Two ways (either is enough):

- `Data\SKSE\Plugins\CostumeExpansionFW.ini` → `[General]` `bEnabled=0`
- Create an empty `Data\SKSE\Plugins\CEF_DISABLE.txt` — its mere presence forces CEF off
  (typo-proof panic switch).

When disabled, CEF registers no hooks, no event sinks, loads no boxes, and does no mesh
injection or carrier/FSMP work — the save opens with the plugin fully inert (the stale
co-save data is skipped). Set `bEnabled=1` / delete the flag file to re-enable.

## Uninstalling

Before removing the mod, open **MCM → Main → Prepare for uninstall**. This returns every
captured item to you, removes all box tokens from your inventory, detaches the injected
meshes, **and disables CEF persistently** (`enabled=false` in the settings — reloading or
playing on will not re-apply anything; re-enable from MCM → Main if you change your mind).
Save, then remove the mod.

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

## License

- **Source code of this project**: MIT — see [LICENSE](LICENSE).
- **Distributed plugin binary (`CostumeExpansionFW.dll`)**: **GPLv3**, because it
  statically links [nifly](https://github.com/ousnius/nifly) (GPLv3) for the
  in-process carrier build. Corresponding source is this repository. See
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full component list
  and the packaging checklist.
