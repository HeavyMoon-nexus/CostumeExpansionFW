CostumeExpansionFW - Skyrim VR notes
====================================

WHAT THIS IS
------------
CostumeExpansionFW is built runtime-independent: ONE dll serves Skyrim SE, AE
and VR. There is no VR build of it and there never was - the dll works out
which engine loaded it. From 1.6.3 everything ships in a single installer, so
picking Skyrim VR in it gives you this file and nothing else. The dll, the
plugins, the meshes and the scripts all come from the core install, the same
ones an SE player gets.

If you still have the old VR Patch installed as a separate mod, REMOVE IT. It
holds a dll at the same path as this one, so depending on where it sits in your
mod order it can put the old dll back over the new one. Nothing can detect that
for you, because the file has the same name either way. The same goes for the
old NPC add-on, which is an option in the installer now.

REQUIREMENTS (VR)
-----------------
* Skyrim VR 1.4.15
* SKSEVR 2.0.12                    https://skse.silverlock.org/
* VR Address Library for SKSEVR    https://www.nexusmods.com/skyrimspecialedition/mods/58101
  (required - the plugin resolves engine addresses through its
   version-1-4-15-0.csv and will not start without it)
* Skyrim VR ESL Support            https://www.nexusmods.com/skyrimspecialedition/mods/106712
  (required - CostumeFW.esp and CostumeFW_BoxPool1.esp are ESL-flagged with a
   1.71 header and vanilla Skyrim VR loads neither. The same mod covers the
   optional CostumeFW_NPC.esp add-on. The optional CostumeFW_Abilities.esp is
   a full plugin, so it needs nothing special)
* SKSE Menu Framework              https://www.nexusmods.com/skyrimspecialedition/mods/120352
  + ImGui VR Helper                https://www.nexusmods.com/skyrimspecialedition/mods/183466
  (the main UI - see UI IN VR below)
* SkyUI VR (legacy MCM)            https://github.com/Odie/skyui-vr
* RaceMenu VR 0.4.14 (skee)        https://www.nexusmods.com/skyrimspecialedition/mods/19080
  (optional-files tab of the RaceMenu page; needed for body-morph follow)
* optional:    FSMP (its current builds support VR) - SMP cloth physics on
               injected content
* recommended: VRIK Player Avatar - vanilla Skyrim VR renders no player body,
               so without VRIK you cannot see your own costume in the headset

UI IN VR
--------
The SKSE Menu Framework section ("Costume Expansion FW") is the main UI, and on
VR it needs SKSE Menu Framework + ImGui VR Helper. Without them it is simply
absent; nothing breaks, but see what the MCM cannot reach below.

The SkyUI MCM is still here and still works for boxes and persist entries. What
it does not reach: NPC distribution, the capture blacklist (Blocked), the
Recovery page, box renaming, the per-item Item data switches, the verbose-
logging switch, and - from 1.6.4 - any biped slot that holds more than one box.
Its pages are keyed by the slot number, so a slot with two boxes names neither
of them, and those slots get no page at all rather than a page that edits the
wrong box. Its Persist page also runs out of room once the catalog grows past
roughly a dozen entries, which is SkyUI's option buffer rather than a CEF limit.

Without either menu, the console commands still work: "cef pub" and
"cef npcpersist" cover listing, refresh, recall and persist assignment, and
"cef tokens" lists every box token and what is using it.

BETA STATUS / HOW TO REPORT
---------------------------
VR support is community-tested: the code paths were verified against the VR
address database and against shipping VR mods, but NOT on VR hardware by the
author. If anything misbehaves:

1. Panic switch: create an empty file  Data\SKSE\Plugins\CEF_DISABLE.txt
   CEF becomes fully inert (no hooks, no injection, no co-save handling) and
   any save opens clean.
   DO NOT SAVE while that file is there. "No co-save handling" means CEF
   writes nothing into the save, so saving loses the hidden storage, your
   persist items and any NPC costume bindings. Open the save, check that it
   loads, then quit to desktop.
   To keep playing with CEF's costumes off but its data intact, set
   "enabled": false in  Data\SKSE\Plugins\CEF_settings.json  instead.
2. Report with  Documents\My Games\Skyrim VR\SKSE\CostumeExpansionFW.log
   attached. Its first lines show a build stamp and
   "runtime: Skyrim VR 1.4.15.0" - please include them. The last two runs are
   kept beside it as CostumeExpansionFW.prev1.log and .prev2.log, so if you
   restarted after a crash, prev1 is usually the run that crashed.

NPC FEATURES ON VR
------------------
The NPC add-on (CostumeFW_NPC.esp + its meshes) is an option in the installer
and works on VR under the same requirements as the core - Skyrim VR ESL Support
handles its plugin too. Two VR-specific caveats:

* The NPC 3D-load hook (Character::Load3D) is statically VR-safe (verified
  against the VR vtable data) but has NOT yet been exercised on VR hardware.
  If you get crashes when NPCs stream in, that hook is the prime suspect -
  please report it. To keep playing meanwhile, uninstall the NPC add-on
  (CostumeFW_NPC.esp), which takes that whole feature out; the hook itself
  only comes off with CEF_DISABLE.txt, and you cannot save while that is on
  (see the panic switch note above).
* You cannot see your own published costume in first person without VRIK
  (as with all player-worn CEF content).

KNOWN VR NOTES
--------------
* Tempering reads a little differently. On SE/AE a captured item's improvement
  is measured with the engine's own item-card arithmetic; on VR that path is
  not available, so CEF uses the raw improvement multiplier instead. Values can
  come out slightly higher than the same item would show on SE/AE.
* With VRIK, items on your own head slots are hidden together with your head
  near the camera (normal VRIK behavior); on NPCs they display as usual.
* The "cef" console commands run through a VR-specific hook offset. If the
  console misbehaves ONLY on VR, please report it; every other feature is
  independent of that hook.
* Invisibility / effect-shader follow: a costume shown while you are invisible
  picks up the effect the moment it appears, and that part works the same on
  VR. What VR does not get is the per-frame maintenance pass, which rides on a
  PlayerCharacter::Update hook whose index differs on VR (the same reason frame
  containment is SE/AE only). In practice a costume already on screen when an
  effect STARTS or ENDS follows the engine's own timing on VR instead of CEF's;
  re-showing the item resynchronizes it.
