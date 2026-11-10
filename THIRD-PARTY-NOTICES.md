# Third-party notices & binary distribution license

## TL;DR

- The **source code written for this project** is MIT (see [LICENSE](LICENSE)).
- The **distributed plugin binary `CostumeExpansionFW.dll` is licensed GPLv3**,
  because it statically links [nifly](https://github.com/ousnius/nifly) (GPLv3)
  for the in-process FSMP carrier build (v1.2+).
- Corresponding source for the binary: this repository —
  <https://github.com/HeavyMoon-nexus/CostumeExpansionFW>.

Every other linked dependency is GPL-compatible, so the combined binary can be
(and is) distributed under GPLv3. Redistribution of the DLL must follow GPLv3:
keep this notice, keep the license texts, keep the source link.

## Components linked into CostumeExpansionFW.dll

| Component | License | Role |
|---|---|---|
| [nifly](https://github.com/ousnius/nifly) (ousnius) | **GPL-3.0** | NIF read/write for the in-proc carrier build |
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | MIT | SKSE plugin framework |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON (settings/manifest/carriers) |
| [pugixml](https://github.com/zeux/pugixml) | MIT | HDT-SMP physics XML processing |
| [spdlog](https://github.com/gabime/spdlog) / [fmt](https://github.com/fmtlib/fmt) | MIT | logging (via CommonLibSSE) |
| [xbyak](https://github.com/herumi/xbyak) | BSD-3-Clause | runtime code generation (via CommonLibSSE) |
| Windows CNG (bcrypt) | OS component | SHA-256 content hashing |
| [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) consumer header (Thiago099 / QTR-Modding) | **LGPL-2.1** | in-game UI (v1.3+). Header-only, vendored at `src/external/` (pinned commit recorded alongside); all calls resolve the separately-installed SKSEMenuFramework.dll at runtime (dynamic linking — no SMF code is statically embedded). LGPL-2.1 is GPLv3-compatible. |

## Legacy optional tool (removed in v1.6.4)

`tools/nifcarrier` (C#, [NiflySharp](https://github.com/ousnius/NiflySharp),
GPL-3.0) is **gone**. The carrier build was ported to C++ in v1.2.1 and has been
the only shipping path ever since; nothing selected the external process at
runtime, so the C# copy was a second implementation of `carriers.json` that no
longer knew its current shape — running it by hand dropped the `published` and
`npcPersist` entries (F19). It was never in the mod zip. Its history is in git.

NiflySharp's GPL-3.0 obligations travelled with that exe only. The C++ carrier
builder uses [nifly](https://github.com/ousnius/nifly) (GPL-3.0), listed above,
so the GPLv3 treatment of the shipped package is unchanged.

## Packaging checklist (Nexus zip)

1. Stage from the deployed mod folder only (never from `build/` or `dist/`
   staging areas that may contain dev exes).
2. Include: `LICENSE` (MIT, own code), this file, and the GPLv3 text
   (`LICENSE.GPL-3.0.txt`).
3. The mod page license field: GPLv3 (binary), source at the GitHub repo.
