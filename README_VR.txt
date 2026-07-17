CostumeExpansionFW - Skyrim VR Patch
====================================

WHAT THIS IS
------------
CostumeExpansionFW is built runtime-independent: ONE dll serves Skyrim SE, AE
and VR. This patch carries that dll plus these VR install notes. Install the
MAIN CostumeExpansionFW package first, then this patch, letting the patch win
file conflicts (in MO2: place the patch below the main mod). Everything else
(esp, meshes, scripts, KID ini) is identical across runtimes and comes from
the main package.

REQUIREMENTS (VR)
-----------------
* Skyrim VR 1.4.15
* SKSEVR 2.0.12                    https://skse.silverlock.org/
* VR Address Library for SKSEVR    https://www.nexusmods.com/skyrimspecialedition/mods/58101
  (required - the plugin resolves engine addresses through its
   version-1-4-15-0.csv and will not start without it)
* Skyrim VR ESL Support            https://www.nexusmods.com/skyrimspecialedition/mods/106712
  (required - CostumeFW.esp is ESL-flagged with a 1.71 header; vanilla
   Skyrim VR loads neither)
* SkyUI VR (MCM)                   https://github.com/Odie/skyui-vr
* RaceMenu VR 0.4.14 (skee)        https://www.nexusmods.com/skyrimspecialedition/mods/19080
  (optional-files tab of the RaceMenu page; needed for body-morph follow)
* optional:    FSMP 4.x (its current builds support VR) - SMP cloth physics
               on injected content
* recommended: VRIK Player Avatar - vanilla Skyrim VR renders no player body,
               so without VRIK you cannot see your own costume in the headset

UI IN VR
--------
The MCM ("Costume Expansion FW") is the primary UI and covers every feature.
The SKSE Menu Framework section also works in VR if you additionally install
SKSE Menu Framework + ImGui VR Helper
(https://www.nexusmods.com/skyrimspecialedition/mods/183466); without them it
is simply absent - nothing breaks.

BETA STATUS / HOW TO REPORT
---------------------------
VR support is community-tested: the code paths were verified against the VR
address database and against shipping VR mods, but NOT on VR hardware by the
author. If anything misbehaves:

1. Panic switch: create an empty file  Data\SKSE\Plugins\CEF_DISABLE.txt
   CEF becomes fully inert (no hooks, no injection, no co-save handling) and
   any save opens clean.
2. Report with  Documents\My Games\Skyrim VR\SKSE\CostumeExpansionFW.log
   attached. Its first lines show a build stamp and
   "runtime: Skyrim VR 1.4.15.0" - please include them.

KNOWN VR NOTES
--------------
* With VRIK, items on your own head slots are hidden together with your head
  near the camera (normal VRIK behavior); on NPCs they display as usual.
* The "cef" console commands run through a VR-specific hook offset. If the
  console misbehaves ONLY on VR, please report it; every other feature is
  independent of that hook.
