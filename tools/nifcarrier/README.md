# nifcarrier

> **LEGACY / FALLBACK (v1.2+).** The carrier build now runs **in-proc inside the
> CEF DLL** (`nifcarrier_core`, C++ nifly) — end users need NO .NET runtime and
> NO `CEF_sync_command.txt`. This C# tool remains for two purposes only:
> 1. **golden oracle** for the C++ port's offline tests (`verifytree`/`validate`/`dump`),
> 2. **fallback**: a present `CEF_sync_command.txt` makes CEF prefer this external
>    tool over the in-proc build (compat mode, kept for one release).
>
> Do NOT ship this exe in the mod package — it is GPL-3.0 (NiflySharp) and the
> in-proc build replaces it. See NIFCARRIER_INPROC.md (internal).

Headless authoring of **FSMP physics-carrier NIFs** for CEF — no NifSkope / Blender
GUI required. This is the tool that unblocks the shared B/C blocker documented in
[FSMP_APPROACH_B.md](../../FSMP_APPROACH_B.md) §9-4 and
[FSMP_APPROACH_C.md](../../FSMP_APPROACH_C.md) §9-10:

> authoring a NIF that carries a physics bone hierarchy + the root
> `NiStringExtraData "HDT Skinned Mesh Physics Object"` (XML path).

Built on [ousnius/NiflySharp](https://github.com/ousnius/NiflySharp) (nuget package
`Nifly`), a clean-room C#/.NET NIF library.

## Requirements

- .NET SDK 8.0+ (verified on 9.0.312)
- Network access on first build (restores `Nifly` from nuget.org). Offline: drop
  `nifly.<ver>.nupkg` into a local folder and add it as a NuGet source.

## Build & run

```sh
cd tools/nifcarrier
dotnet run -c Release -- <command> <args>
```

## Commands

| Command | Purpose |
|---|---|
| `dump <nif>` | List block-type tally, named `NiNode`s (bones), and HDT extra data. |
| `carrier <in.nif> <out.nif>` | **Level-2 invisible carrier**: strip every geometry shape (`BSTriShape`/`BSDynamicTriShape`/…) and sweep now-unreferenced skin/shader/texture/geomdata blocks, keeping only the bone `NiNode` hierarchy + root `HDT ...` extra data. Reloads and self-verifies. |
| `verifytree <src> <carrier>` | Assert the bone parent hierarchy is byte-for-byte identical between source and carrier. |
| `merge <out.nif> <base.nif> <add.nif> [add2 …]` | **Level-3 merge carrier** (1 token = many contents): union the bone branches of every `add` NIF into `base` (same algorithm as FSMP's `doSkeletonMerge` — shared bones reused, unique bones cloned). Root extra data inherited from `base`. Reloads and self-verifies. |
| `anchor <in.nif> <out.nif> <anchorBoneName>` | **C body-anchor re-route**: nest all root-level bone branches under a new node (e.g. `NPC Pelvis [Pelv]`) so the physics chain grows from that body bone instead of the head, when driven via the facegen head path (C §9-10). |
| `mergexml <out.xml> <in1.xml> <in2.xml> […]` | **Unified physics XML**: concatenate `<system>` docs with FSMP-correct semantics (`cef_factory` default-template snapshot at top + reset at each doc boundary; duplicate `<bone>` dropped first-wins; duplicate collision-shape names warned). |
| `setxml <in.nif> <out.nif> <xmlPath>` | Re-point the root `HDT Skinned Mesh Physics Object` extra data at another XML (verified readback). |
| `validate <nif>` | **Divide-by-zero gate**: report whether the NIF is safe to bake into an SSE carrier. Rejects non-SSE geometry (`NiTriShape`/`NiTriStrips` — LE/Oldrim, needs SSE NIF Optimizer) and skinned shapes with 0 vertices — the structures that make the engine's skin-partition loader raise `EXCEPTION_INT_DIVIDE_BY_ZERO`. |
| `sync <manifest> --data <root> […] --out <cefMod> [--empty <token.nif>]` | **Production driver**: read CEF's `CEF_carrier_manifest.json` and rebuild `Box<slot>_carrier.nif` (+ `XML/Box<slot>_physics.xml` when 2+ SMP contents) per box. Unchanged boxes skipped via hash files. See `sync_carriers.cmd`. |

### Skin-partition safety (why `validate`/`sync` gate)

A worn box token loads its carrier NIF through the **vanilla engine**, not CEF — so a
malformed carrier crashes the game with `INT_DIVIDE_BY_ZERO` in the skin-partition
loader, and no CEF config change (settings JSON, MCM toggle, external kill-switch) can
stop it (root cause of the Box44 CTD, 2026-07-03). To prevent a bad build from ever
reaching disk, `sync`:

1. **Excludes** any single content whose skin data fails `validate` (logged
   `WARNING … EXCLUDED`), building the box from the remaining valid contents.
2. Builds into a temp, runs the **final gate** (`validate`) on the assembled carrier,
   and only then **atomically publishes** to `Box<slot>_carrier.nif`. On failure it
   keeps the previous good carrier and does **not** bump the revision in `carriers.json`.

Fail-safe on the CEF side: `ApplyCarrierOverrides` refuses to repoint a token ARMA at a
carrier file missing on disk, so a quarantined bad build falls back to the ESP-default
(empty) carrier — invisible costume, never a crash. **Recommendation:** ship each token
ARMA's default `WorldModel` as the 234-byte empty carrier so an unbuilt/removed carrier
can never CTD.

## What a level-2 carrier is for

Assign the carrier to a box token's ARMA `WorldModel` (shared ARMA `000800`, or a
per-token ARMA). On equip, FSMP's **①armor-attach path** (`doSkeletonMerge`) builds
`hdtSSEPhysics_AutoRename_Armor_<id> <bone>` on the live skeleton — but because the
carrier has **no geometry**, there is no visible double of the costume (unlike a
level-1 whole-NIF carrier). CEF's suffix-match rebind
([`FindFsmpRenamedBone`](../../src/SkinRebind.cpp)) then binds the injected mesh onto
those physics-driven bones. See [FSMP_APPROACH_B.md](../../FSMP_APPROACH_B.md) §9-7
(levels 1/2/3) and §10.

## Verified result (CPL_VeilA.nif → carrier)

```
BEFORE : blocks=146 shapes=4  bones=120 hdtExtra=True
AFTER  : blocks=121 shapes=0  bones=120 hdtExtra=True   (13 KB)
RELOAD : shapes=0 hdtExtra=True bones=120
verifytree: IDENTICAL HIERARCHY (missing=0 parentMismatch=0)
merge  (VeilA carrier + Bunny_tail carrier): 120 -> 124 bones (+4 unique, shared reused), shapes=0
anchor (VeilA carrier, "NPC Pelvis [Pelv]"): anchor node created, branch re-parented, reload preserves name+hierarchy
```

XML path (`meshes\Caenarvon\Cosplay\XML\CPBVeil.xml`) and NIF header
(`Gamebryo 20.2.0.7`, SSE) are preserved verbatim.

> **Not yet verified in-game.** Whether NiflySharp's round-trip output loads in the
> live engine / FSMP is confirmable only on real hardware (B §9-4). Header, extra
> data, bone count and hierarchy all match the source, so confidence is high.

## Status / remaining work

- ✅ **Level-2 invisible carrier** — `carrier` command (verified above).
- ✅ **Level-3 merge carrier** — `merge` unions bone branches (verified above).
  Remaining: the **physics XML union is NOT done by this tool** — `merge` inherits the
  base NIF's root extra data, so you must supply one XML that unions every content's
  bones/constraints (B §4-2: for uniquely-named bones this is effectively concatenation).
- ✅ **Body-anchor re-route** — `anchor` command (verified above). Two caveats:
  - Run it on a **pure physics-bone carrier** (custom bones only). On a carrier that
    still holds the full standard skeleton tree (`NPC`, `NPC COM`, …) it nests that whole
    tree under the anchor — not intended. Reduce to custom bones first.
  - The FMD facegen path (C §9-10 (b)) also needs a **tiny skinned geometry** on the root
    so `processGeometry` doesn't skip it; `anchor` does not add that yet.
- ⏳ **In-game load** — the single unverified point for all of the above (see
  [TESTING.md](TESTING.md)).

## License

Uses NiflySharp, licensed **GPL-3.0**. This build tool links it and is therefore
GPL-3.0 as well. It is a build-time asset generator and is not shipped in, nor does
it affect the license of, the CEF runtime DLL.
