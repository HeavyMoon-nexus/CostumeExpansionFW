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

## Legacy optional tool (not part of the mod package)

`tools/nifcarrier` (C#) uses [NiflySharp](https://github.com/ousnius/NiflySharp)
(GPL-3.0). It is a development/oracle tool and an external-process fallback; it
is **not** shipped in the mod zip. Any separate distribution of that exe is a
GPLv3 distribution and needs the same treatment (license text + source offer).

## Packaging checklist (Nexus zip)

1. Stage from the deployed mod folder only (never from `build/` or `dist/`
   staging areas that may contain dev exes).
2. Include: `LICENSE` (MIT, own code), this file, and the GPLv3 text
   (`LICENSE.GPL-3.0.txt`).
3. The mod page license field: GPLv3 (binary), source at the GitHub repo.
