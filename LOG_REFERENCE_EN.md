# CostumeExpansionFW (CEF) Log Reference (as of v1.3.0)

**Log file**: `Documents\My Games\Skyrim.ini\SKSE\CostumeExpansionFW.log`
- Overwritten on every game launch (previous runs are not kept)
- Format: `[time] [level] message`. Lines below are info unless marked **[warn]** / **[error]**
- Companion log: `Data\SKSE\Plugins\CEF_sync.log` (lands in MO2's overwrite) — details of the carrier auto-rebuild (nifcarrier). Check it whenever auto-sync fails

## Where to look first (troubleshooting)
1. The first line `CostumeExpansionFW loaded (file ...)` — if the *file* timestamp is older than the DLL you installed, MO2 is serving a stale cached copy (fully close and reopen MO2)
2. `settings: loaded ...` — did the settings file load?
3. Costume shows but doesn't sway → look for `remapped ... static` and failed `auto-sync:` lines (then check CEF_sync.log)
4. Costume doesn't show at all → look for `NOT SHOWN` / `LoadNif failed`

## 1. Startup (always present)
- `CostumeExpansionFW loaded (file A / compile B)` — build stamp. A = last-write time of the DLL the game actually loaded, B = compile time
- `serialization installed` — co-save (save-file data) handler registered
- `skee BodyMorph interface: acquired / unavailable (...)` — RaceMenu integration. CEF still works when unavailable (only body morphs are off)
- `PlayerCharacter::Load3D hook installed (0x6A)` — hook that re-applies costumes on cell change / load
- `console hook installed (Script::CompileAndRun)` — enables the `cef` console commands
- `LoreBox: BSScaleformTranslator::Translate hook installed (vfunc 0x2)` — item-name tooltip integration
- `SMF: registered section 'Costume Expansion FW' (5 pages)` — SKSE Menu Framework UI registered
- `SMF: SKSEMenuFramework.dll not installed - SMF UI skipped (MCM / console remain available)` — SMF not installed (harmless; MCM and console still work)
- `Papyrus natives registered (CFW_Native)` — native functions for the MCM
- `bind watchdog started (...ms tick)` — watchdog for FSMP skeleton rebuilds started

**Kill switch (when present, CEF is fully inert)**
- `CEF hard-DISABLED by CEF_DISABLE.txt - plugin is inert`
- `CEF hard-DISABLED by CostumeExpansionFW.ini (bEnabled=0) - plugin is inert`
- `CostumeExpansionFW is DISABLED via external config - doing nothing`

## 2. settings: — the settings file (CEF_settings.json)
- `settings: loaded N box(es) (M content), K persist (catalog), enabled=...` — load summary (the key "all good" line)
- `settings: no ... (no settings yet)` — first launch, file doesn't exist yet (normal)
- `settings: wrote N box def(s) (enabled=...)` — settings saved
- `settings: reapplied N box(es)` — re-applied after loading a save
- `settings: reloaded from disk (MCM) - ...` — reload triggered from the MCM
- `settings: migrating legacy ...` — automatic migration from an older file format
- `settings: healed pre-merge plugin ids -> CostumeFW.esp (v1.2.1 consolidation)` — old-version IDs auto-repaired
- **[warn]** `settings: recovered from ..._bak (main file corrupt; ...)` — main JSON corrupt → restored from backup
- **[error]** `settings: JSON parse error / field type error (...) - loaded empty` — settings corrupt; the file is left untouched so it can be repaired
- **[warn]** `boxes: LoadBoxes drops ...` — invalid entries removed on load (enforces "one token = one box", "one item = one holder")

## 3. Carrier auto-rebuild (FSMP physics)
The automatic loop: box set changes → carrier NIFs are rebuilt → carriers are swapped in.
- `carrier manifest updated (N box(es)) - rebuilding FSMP carriers` — change detected, rebuild scheduled
- `auto-sync: rebuilding carriers (in-proc nifcarrier)` — rebuild started
- `auto-sync: done - applying carrier revisions` — success; the swap runs next
- `carrier override: slot N token '...' -> <file> (re-equip to apply)` — swap done. **Re-equip the token to see it**
- **[error]** `auto-sync: ... failed / timed out / still running after Ns` — rebuild failed. See `CEF_sync.log` for details
- **[warn]** `carrier override: slot N carrier '...' missing/invalid on disk - keeping ESP default` — generated file missing, falling back to the default carrier
- **[warn]** `carriers.json parse failed / field type error - keeping ESP-default carriers` — fail-safe for a corrupt carriers.json
- `persist carrier: '...' model -> <file>` — head (HDPT) carrier swap
- **[warn]** `persist carrier: ... missing on disk / unknown pool part / part file missing` — head-carrier asset missing (skipped, current state kept)
- `persist carrier: production pool + PoC leftovers deregistered (manual remove)` — deregistered via `cef persist remove`

## 4. Mesh injection (on every equip change / cell change / load)
Indented lines are details of the operation right above them.
- `Reconcile: N active item(s) (cef enabled=...)` — entry point of the re-apply pass
- `bound N bone(s) to FSMP physics-driven node(s) (SMP sway; e.g. ...)` — **the success line: the mesh sways with SMP**
- **[warn]** `remapped N unresolved bone(s) to nearest ancestor (static, no SMP sway; e.g. ...)` — bones didn't resolve, fell back to a static bind (**no sway**; usually the carrier isn't attached yet)
- `rebind retry queued (+Nms): FSMP carrier may still be attaching` → `rebind retry: re-injecting N static item(s)` — automatic retry for the above
- **[warn]** `NOT SHOWN '...' on '...': skin rebind failed (corrupt source bone) / no skinned geometry in NIF` — **final "not displayed" warning** (a problem in the source mesh)
- **[error]** `LoadNif failed (err=N) for '...'` — NIF file failed to load
- **[warn]** `skin missing skinData/bones / bone[N] is null / boneWorldTransforms is null` — stages of bad skin data
- `BODYTRI '...' -> holder (...)` / `BODYTRI: none on ...` — whether body-morph (tri) data was carried over
- `body morph: ApplyVertexDiff (BODYTRI present / NO BODYTRI ...)` — body morph applied (off by default; enable via `cef morph` or the MCM)
- `hideshape gate '...' -> dropping N shape(s)` — per-shape hide settings applied
- `bind watchdog: '...' holds a dead merge generation - reconciling` — detected an injected mesh stranded by an FSMP skeleton rebuild and auto-repaired it (the automatic version of `cef repair`)
- `DetachAllInjected[...]: removed N CostumeFW_* node(s)` — all injected nodes removed (toggle off / repair)
- `DefineBox content='...' token='...'` — box definition registered

## 5. realbody: — real-body skin display
- `realbody: skin '...' (ID, plugin) body addon ... model3p='...'` → `realbody: injected player body addon ...` — player's body resolved and injected (normal flow)
- `realbody: skin TXST ... -> N shape(s)` — skin textures applied
- `realbody: base skin ... not adopted (...) - using race skin` — custom skin not adopted → race default
- **[warn]** `realbody: no skin ARMO on player / has no slot-32 body addon / addon has no model` — body resolution failed

## 6. persist head: / mouth: — head parts
- `persist head: + '...' / - '...' (ID)` — head part registered / deregistered
- `persist head: DoReset3D (facegen rebuild)` — face rebuild issued
- `mouth: '...' registered but ABSENT from facegen head (...) - clean rebuild #N` — **auto-fix for the known "teeth disappear" issue** (detected a few seconds after load, then rebuilt)
- **[warn]** `mouth: '...' still dropped after N rebuild(s) - giving up` — auto-fix gave up (please report this)

## 7. boxes: / custody: / persist / recover: — item management
- `boxes: AddBox label='...' token='...' content='...'` — box created
- **[warn]** `boxes: AddBox rejects ...` — creation refused (item already captured elsewhere, CEF's own items, etc.)
- **[warn]** `boxes: NewBox - token pool exhausted` — box count limit reached
- `boxes: replenished lost token ...` — a sold/dropped token was automatically given back
- `boxes: captured N enchant effect(s) for '...'` — source item's enchant effects copied onto the token
- `custody: created hidden store ... / captured 1x '...' / returned stored '...' / fabricated 1x '...'` — storing and returning the original item
- **[warn]** `custody: ...` — defenses against store inconsistencies (usually self-healing)
- `persist on/off: '...' on this save` — per-save persist toggle
- **[warn]** `persist: ... rejected / refusing` — refused (not in catalog, conflicts with a box, etc.)
- **[warn]** `hide:/gender:/morph:/realbody:/hideshape: '...' is held by no box/persist - ignoring` — setting change on an ID that nothing holds
- `recover: granted 1x '...'` — rescue grant via `cef recover`

## 8. cosave: — save-file data
- `cosave: saved N persist item(s) (M unresolved carried)` — saved; "unresolved" are items whose plugin is currently missing, carried forward
- `cosave: restored N item(s)` / `cosave: hidden store ... restored` — load succeeded
- **[warn]** `cosave: '...' unresolved - carried for next save` — that plugin is currently disabled (comes back when re-enabled)
- **[warn]** `cosave: could not restore '...'` / `hidden store ... did not resolve` — restore failed
- **[error]** `cosave: corrupt record ... skipped` — corrupt co-save chunk (skipped)
- `cosave: reverted (registry cleared)` — reset at load start (normal)

## 9. preset: — presets
- `preset: exported <file> (N content)` — exported
- `preset: assigned '...' to persist / to box N (N content)` — applied
- **[warn]** `preset: '...' has N unresolved content (skipped)` — some items' plugins are not installed
- **[warn]** `preset: ... already assigned / already captured in ...` — refused (held by another box)
- **[error]** `preset: cannot open / JSON parse error / cannot write` — file problem

## 10. Console command output (only when you type `cef ...`)
- `cef list` → `active: N item(s)` + list of IDs
- `cef shapes <id>` → `shapes for '...': N shape(s)` + `ON/off <name> [slot N]`
- `cef morph` → body-morph ON/off list
- `cef headdiag` / `cef hair <id>` → head-bone diagnostics (dev/testing)
- When capturing via the MCM: `capture: '...' carries attached script '...'` — heads-up for scripted equipment
