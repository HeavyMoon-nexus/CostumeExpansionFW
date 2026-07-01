# nifcarrier

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
```

XML path (`meshes\Caenarvon\Cosplay\XML\CPBVeil.xml`) and NIF header
(`Gamebryo 20.2.0.7`, SSE) are preserved verbatim.

> **Not yet verified in-game.** Whether NiflySharp's round-trip output loads in the
> live engine / FSMP is confirmable only on real hardware (B §9-4). Header, extra
> data, bone count and hierarchy all match the source, so confidence is high.

## Roadmap (built on this base)

- **Level-3 merge carrier** (1 token = many contents): union the bone branches of a
  box's contents into one NIF + one concatenated XML (B §4-2 / §10-2). `AddBlock` /
  `CloneNamedNode` cover the branch grafting.
- **C body-anchor re-route**: nest a content's physics bone chain under a
  `NPC Pelvis` anchor node so the chain grows from the pelvis, not the head
  (C §9-10) — a hierarchy-rewrite on top of the same load/save path.

## License

Uses NiflySharp, licensed **GPL-3.0**. This build tool links it and is therefore
GPL-3.0 as well. It is a build-time asset generator and is not shipped in, nor does
it affect the license of, the CEF runtime DLL.
