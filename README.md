# Costume Expansion FW (CEF)

An SKSE framework for Skyrim SE/AE (1.6.1170, CommonLibSSE-NG) that **shows skinned
accessories and costumes without consuming their biped slots**. It clones an item's
skinned geometry and re-binds it directly to the live skeleton, bypassing the equip
system — so several items can share one slot, and an accessory's original (often
contested) slot is freed. Effectively a slot-budget expansion for the 32 fixed biped
slots (30–61).

Two display classes:

- **Persist** — always-on accessories (nails, piercings). No token, no slot gate.
- **Box** — token-gated costumes. Equipping a box's invisible token shows its packed
  contents; a strip mod unequipping the token hides them (automatic compatibility).
  Multiple contents can ride on a single box token (one slot).

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

## Usage (MCM: "Costume Expansion FW")

- **Main** — master on/off, dependency status, version.
- **Persist** — `+ Add worn item` captures a worn accessory as always-on; remove per item or all.
- **Boxes** — `+ New box` picks a free slot; per box: Distribute token, Wear (show/hide),
  `+ Add worn item` (capture into the box), Armor type, **Preset** (assign a preset's
  contents), **Export as preset**, Stats, Delete, and per-item remove.
- **Presets** — lists installed `CEFP_*.json` presets; assign one to a box.

Presets (`Data\SKSE\Plugins\CEF\Presets\CEFP_*.json`) are the shareable unit — a named,
human-readable set of content. Settings live in `Data\SKSE\Plugins\CEF_settings.json`
(global, shared across saves).

## Uninstalling

Before removing the mod, open **MCM → Main → Prepare for uninstall**. This returns every
captured item to you, removes all box tokens from your inventory, and detaches the injected
meshes. Save, then remove the mod.

## Known limitations

- **Body morph (RaceMenu sliders) follows** via skee, including real-time edits.
- **Body physics (CBPC / standard-bone SMP)** works on injected meshes (they bind to the
  live, physics-driven bones).
- **Custom-bone SMP cloth physics** (outfit-specific bones, e.g. skirt sway) does **not**
  animate on injected meshes — those bones only exist when the outfit is equipped through
  the engine, which CEF bypasses by design. Such content is shown **statically** (the
  affected part is bound to its nearest real ancestor bone) instead of vanishing. For full
  cloth sway, wear that item normally. (A public API on an open HDT-SMP implementation —
  e.g. FSMP/hdtSMP64 — to attach physics to an external node would lift this limit. Some
  setups use FlexSMP, which is FSMP-compatible but closed-source and unmaintained, so the
  open FSMP lineage is the realistic target for such a request.)
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

MIT — see [LICENSE](LICENSE).
