# Changelog

## v1.2.1 (2026-07-07)

### Fixed
- **Removing an ACTIVE persist entry can no longer double-return.** The
  catalog remove now also deactivates the entry (return pairs with
  deactivation), and the "Active but not in catalog" [deactivate] row guards
  against stale page snapshots - previously the removed-but-still-active
  entry resurfaced there and its deactivate returned a second copy (review
  round 4).
- **Co-save restore honors the forced-gender NIF.** The token-less persist
  restore resolved models with the raw player sex, so a forced Male/Female
  NIF reverted on every load; it now uses the same effective sex as live
  registration.
- **A persist catalog remove keeps the per-content settings.** Hide rules,
  forced gender, body-morph opt-in and the captured enchant snapshot used to
  be erased with the catalog entry - but another save may keep the entry
  uncataloged-active and still display it, and lost its look/enchant on the
  next load. The maps now survive (a re-capture re-snapshots the enchant).
- **Preset validation checks displayability and drops duplicates.** Entries
  are validated with the same gate captures use (usable ARMO/ARMA model, not
  mere form existence), and duplicate ids no longer double the stats/enchant
  aggregation.
- **"Reload settings from disk" keeps uncataloged actives.** The reload
  re-registers every persist entry active on this save - including entries
  another character removed from the shared catalog (a supported state the
  co-save restore also preserves). They were silently dropped with no item
  return (review round 3).
- **Assigning a preset returns the items it replaces.** Box and persist
  preset assignment now returns the physical items of every content the
  preset drops (old minus new; overlapping entries keep their stored
  custody) - previously the old captured items were stranded in the hidden
  store with no UI path back.
- **Presets respect the cross-holder guard.** A preset containing an id
  already captured in another box (or persist) is rejected with the reason
  logged, matching the manual-capture guard - it used to silently steal the
  display and share per-content settings.
- **Removing an INACTIVE persist entry no longer fabricates an item.** The
  single-row "Remove from catalog" returns store-only for entries this save
  never displayed (the capturing character keeps the original); the new-copy
  fallback stays for active entries, per the 2026-07-06 decision.
- **The carrier manifest resolves content NIFs by effective sex** (player sex
  + per-content forced-gender override), matching the injection - it was
  hardwired female-first, so a forced-Male item or a male PC could get a
  carrier built from the other sex's NIF with a mismatched bone set. A Body
  menu change now also rebuilds the carrier when the shown NIF switches.
- **The Stats row shows the captured enchant snapshot** (what the synthesized
  ability actually applies) instead of only the base enchantment - a captured
  player enchant used to look unapplied.
- **Multi-content boxes: per-content bone/shape namespace isolation.** Outfit
  series that reuse custom bone names across items (e.g. COCO's shared
  cocoa01... chains - two COCO skirts share 104 custom bone names) collapsed
  onto a single node under ONE parent in the merged carrier: the first content
  won, so another item's chains could end up hanging from a veil's NPC Head
  root ("the outfit bunches up behind the head"), and the losing items'
  physics systems never built (static, no sway). Each content's custom bones
  and collision shapes are now renamed into a per-content namespace
  (`C<hash>_<name>`) in the carrier NIF and its physics XML, and
  the bind side resolves the prefixed name first (plain names still work for
  single-content carriers). The carrier hash salt was bumped, so every
  existing carrier rebuilds once on the next sync.
- **Carrier builds no longer skip non-SMP contents silently.** Items with no
  inline HDT physics xml in the NIF - including items whose physics is wired
  through FSMP's global defaultBBPs.xml, which CEF cannot detect yet - now log
  why they will inject without physics.
- **Capture is guarded across holders:** capturing an item that another box
  (or persist) already holds is rejected with "already captured in ..." —
  previously the second registration silently stole the display, and deleting
  either holder wiped the shared per-content settings (review P1-1).
- **A failed duplicate capture no longer overwrites settings:** the gender-mode
  pick and the enchant snapshot are written only after the capture actually
  succeeded (review P1-3).
- **Bulk persist returns are active-set only:** "Remove all persist" and
  "Prepare for uninstall" return items only for entries active on THIS save —
  catalog entries another character captured are no longer fabricated as fresh
  copies by bulk flows (review P1-4).
- **"Reload settings from disk" actually reloads:** the button re-reads
  CEF_settings.json and re-applies it live (boxes, catalog, hide/gender/morph/
  enchants; this save's persist actives survive). It previously only rewrote
  the current in-memory state (review P2-5).
- **The Armor type menu opens on the box's current value** instead of always
  box 0's (option-index mixup).
- **Capture pre-validates the mesh:** when a content's model cannot be
  resolved (no usable ARMA/3P model), the capture is refused up front with a
  message - previously the queued registration failed AFTER the item had
  already been moved into the hidden store ("success" notice, item gone,
  nothing shows) (review item 2).
- **Race-matched armor addon selection:** both the injection and the carrier
  manifest now pick the ARMO's addon matching the player's race (exact race,
  then additionalRaces, then the first addon) instead of blindly the first
  one - race-/sex-specific addon lists resolved to the wrong mesh before
  (review item 6).
- **The "has attached scripts" capture warning fires only for real scripts.**
  Passing a form through the MCM's own Papyrus flow binds a plain VM wrapper
  object ("Armor"/"Form") to the form's handle, which the check misread as an
  attached script - so EVERY capture warned. Native wrapper classes are now
  filtered out (the real script's class name is logged when one is found), and
  the message is phrased as a heads-up ("appears to have ... may not run")
  rather than a verdict.

### Changed
- **Capture menus list each item once.** The forced-gender NIF pick moved from
  the tripled capture rows to a per-content **"Body"** menu on box/persist
  rows (changeable any time now, not only at capture; new captures follow the
  player's gender). An **"Inventory filter"** input narrows the "+ Add from
  inventory" list by name (case-insensitive substring), and the native list
  cap rose from 40 to 120 entries.
- **One ESL-flagged plugin.** `CostumeFW_Boxes.esp` and
  `CostumeFW_Boxes_FSMPCarrier_001.esp` were merged into a single espfe plugin,
  **`CostumeFW.esp`** (52 records, no load-order slot consumed; built by
  `tools/espmerge`). Base-plugin FormIDs are unchanged; the carrier-patch's own
  records were renumbered +0x100 (persist head-part pool `0x909`-`0x911`,
  slot-31 wig token `0x913`). Box/persist definitions, ability assignments and
  per-content settings stored in `CEF_settings.json` migrate automatically on
  first load (ids are healed to the new plugin and written back). The three
  approach-C PoC head parts were dropped instead of carried over. A `SEQ` file
  now ships so the MCM quest starts when the plugin joins an existing save.
  The LoreBox KID ini was renamed to `CostumeFW_KID.ini` and now also covers
  the slot-31 wig token.
- **Known limitation (documented):** multiple individuals of the same base
  item cannot be captured separately - catalog/registry keys are base-form
  ids, so the second copy is rejected as a duplicate. The captured copy's
  player-enchant effects are snapshotted and applied; the item itself is
  preserved in the hidden store.
- **Upgrading mid-save:** the old plugins vanish from the load order, so box
  tokens disappear from the inventory once (CEF re-distributes them) and
  captured originals held by the old hidden container are returned as fresh
  copies on demand. A save that ever used persist on a pre-merge DEV build
  should do one TRANSITION load (new plugin enabled, old plugins still
  enabled): a one-shot sweep deregisters the old plugins' head parts
  automatically; save, then disable the old plugins.

## v1.2.0 (2026-07-07)

### Changed
- **The carrier builder now runs inside the plugin — no external tool.** The FSMP
  physics-carrier build (the former external C# `nifcarrier`) was ported to C++
  ([nifly](https://github.com/ousnius/nifly)) and statically linked into the DLL.
  Everything that setup used to need is gone: no .NET runtime, no
  `sync_carriers.cmd` path editing, no `CEF_sync_command.txt` — install the mod
  and auto-sync just works. Box/persist changes rebuild carriers in-process (2 s
  debounce, background thread), and content paths resolve through the same VFS
  the game sees — so any mod manager works, with no per-mod path maintenance.
  Power users: a present `CEF_sync_command.txt` still hands the build to the
  external tool (compat mode, kept for one release). This also retires the
  "plugin executes a command line read from a text file" surface by default.
- **Veil-class contents keep their collision shapes in merged carriers.** The
  C# tool's NIF library corrupted some contents' SSE vertex data during
  cross-file shape cloning, so such contents were baked bones-only (physics
  moved, collision inert). Upstream C++ nifly does not have that bug — those
  contents now carry their collision meshes. The per-content
  validate-or-bones-only safety gate remains in place.

### Fixed
- **Persist enchant effects now come from this save's ACTIVE set,** not the
  shared catalog — you no longer get the effects of persist entries another
  character cataloged but this save never activated.
- **Corrupt co-save data fails small:** record reads are bounds-checked
  (string/count caps, short-read detection), so a damaged co-save logs and
  skips instead of driving a huge allocation or a near-endless restore loop.
- **Carrier publish is failure-checked end to end:** every publish write
  (carrier, revision slot, physics XML, `carriers.json`) is verified, and the
  input hash is recorded only after a successful slot publish — a copy that
  fails (file locks, antivirus, VFS quirks) can no longer strand a box in a
  "hash says up-to-date, files say otherwise" state that never rebuilds. A
  wedged in-proc build is surfaced in MCM Diagnostics (code -2) after 120 s.

### License
- The distributed `CostumeExpansionFW.dll` binary is now **GPLv3** (it
  statically links nifly, GPL-3.0). The project's own source stays MIT. See
  `THIRD-PARTY-NOTICES.md` (component list) and the bundled GPLv3 text.

### Dependencies
- **.NET Runtime is no longer needed.** The optional `CEF-nifcarrier` download
  is legacy — only for the external-tool compat mode or development.

## v1.1.0 (2026-07-06)

### Changed
- **Body morph is now per-content OPT-IN (default OFF).** It used to be applied to
  every injected mesh; on a non-body mesh it is wasted work, and on a large one (a
  full SMP wig) a single skee vertex-diff pass allocated ~15GB that SSE Engine
  Fixes' allocator then retains. Turn it on per content in the MCM (persist page /
  box pages) or with `cef morph <id> on` — only BodySlide body-conforming meshes
  need it.
- **Persist is per-save now (shared catalog + per-save activation):** the persist
  list in `CEF_settings.json` is a catalog shared by every save; each save
  activates entries for itself (MCM toggle per row, or `cef persist on|off <id>`).
  A new character starts with nothing shown; deleting a catalog entry no longer
  strips other characters (their active items survive "uncataloged" and can be
  deactivated — with the item returned — from the MCM). Existing saves keep what
  they showed: the co-save already stored it, no migration.
- **Capture is transactional:** the worn item is moved into the holding container
  only after its registration succeeds; a duplicate capture leaves the item worn
  and says so in a modal message (a corner notification was easy to miss with the
  MCM open).
- **"Prepare for uninstall" now also disables CEF persistently** (writes
  `enabled=false`), so playing on / reloading no longer re-applies everything.
  Re-enable from MCM → Main if you change your mind.
- **Auto-sync hardening:** a timed-out nifcarrier child is terminated instead of
  being left to race the next sync, and an `.exe`-form sync command now runs
  without the `cmd /c` shell layer (`.cmd`/`.bat` wrappers still use it).
- **Carrier apply is user-driven (re-equip to apply):** when a box's content set
  changes, CEF now rebuilds the carrier and repoints the token ARMA at the new
  revision, then asks you to *re-equip the box token until the outfit sways* — it
  no longer tries to swap the token for you. Programmatic re-equips coalesce or
  stall in the equip queue, and FSMP recycles carrier ids while leaving the old
  carrier alive for a few seconds, so an auto-swap could bind the injected mesh to
  a dying carrier and leave it floating. A manual re-equip reloads cleanly once
  FSMP has settled. The old two-token flip-flop machinery has been removed.

### Fixed
- **A duplicate capture could silently swallow a physical copy** of the item into
  the holding container, with the UI still reporting success and no return path
  ever giving that copy back.
- **Skin-only physics bones dropped from merged carriers (veil didn't sway):**
  merging a multi-content box only walked the node hierarchy, so a content whose
  physics bones are referenced *only* by skin data (e.g. a Pharaoh veil's
  `PhSVeil_*`) lost those bones and went static in-game. `nifcarrier` now unions
  every named bone (skin-referenced included, parent chains preserved). When
  NiflySharp's cross-file shape clone would corrupt a specific content's SSE vertex
  data, that content is baked in bones-only rather than dropped, so the rest keep
  their collision meshes.
- **Carrier divide-by-zero CTD (Box44 class):** a box whose merged FSMP carrier NIF
  contained non-SSE geometry (`NiTriShape`) or a degenerate skin partition crashed the
  game with `EXCEPTION_INT_DIVIDE_BY_ZERO` in the vanilla skin-partition loader whenever
  the box token was worn — unstoppable from CEF config, since the engine (not CEF) loads
  a worn token's mesh. `nifcarrier` now validates skin data: `sync` excludes an
  individual crashing content, builds into a temp, runs a final validation gate on the
  assembled carrier, and only atomically publishes a passing build (keeping the previous
  good carrier + revision otherwise). New `nifcarrier validate <nif>` diagnostic. CEF's
  `ApplyCarrierOverrides` no longer repoints a token ARMA at a carrier file missing on
  disk (falls back to the ESP-default empty carrier).
- **One broken content no longer sinks a whole box:** a content that can't be
  resolved under the data roots (or whose physics XML is missing) is now *excluded*
  with a warning and the carrier is rebuilt from the rest, matching how a
  crash-prone content is already handled — instead of failing the entire box. A box
  that declared contents but resolved none keeps its previous carrier rather than
  clobbering it down to empty (guards a transient path miss). Auto-sync also
  captures nifcarrier's output to `Data\SKSE\Plugins\CEF_sync.log` (truncated each
  run) so the `[sync]`/`[merge]` decisions are visible instead of discarded.

### Added
- **Capture from inventory:** the persist page and every box page gain
  **"+ Add from inventory"** — pick any carried armor (name-sorted, first 40)
  without equipping it first, so no throwaway FSMP physics build happens just
  to capture the item. Player-enchanted items keep their enchantment (the
  snapshot now reads the carried entry too). "+ Add worn item" remains for
  reaching items past the list cap.
- **Custom-bone SMP cloth physics on persist items (head-carrier pool):** persist
  accessories with outfit-specific SMP bones (veils, dangling jewelry) now sway:
  nifcarrier builds an invisible head-part carrier pool from the active persist
  set and CEF registers/repoints it automatically — no re-equip needed, load-
  persistent, self-healing (bind watchdog, generation-aware re-bind, `cef repair`).
- **Wig support via a slot-31 box ("Costume Box 31: Hair (Wig)"):** capture an
  equipment wig into it; wearing the token masks your real hair natively (the
  standard equipment-wig mechanism) and the wig's SMP cloth + collision work as
  authored. Wigs should NOT go into Persist (the facegen path rebuilds them
  repeatedly at load and the retained allocations balloon memory).
- **MCM Diagnostics page:** master/skee/FSMP status, last carrier auto-sync
  result, per-box carrier revision (+ on-disk check), persist catalog/active
  counts + head-part registration, and churn counters.
- **Settings safety:** `CEF_settings.json` and the carrier manifest are written
  atomically (a crash mid-write can no longer truncate them), and a last-known-
  good `CEF_settings.json.bak` is restored automatically when the main file
  fails to parse — a corrupted settings file no longer silently wipes your boxes.
- **Console:** `cef recover <FormID:Plugin.esp>` (explicitly grant one copy of a
  content item), `cef persist on|off <id>` (per-save activation).
- **External hard kill-switch (crash recovery):** CEF can now be fully disabled from
  outside the game, read once at startup before any hook runs. Set `bEnabled=0` in
  `Data\SKSE\Plugins\CostumeExpansionFW.ini`, or just drop an empty
  `Data\SKSE\Plugins\CEF_DISABLE.txt` (existence forces off). When disabled, CEF
  registers no hooks/sinks, loads no boxes, and does no mesh injection or carrier/FSMP
  work — so a save a CEF crash left unloadable opens with the plugin inert. Distinct
  from the MCM master toggle, which only hides meshes while the hooks keep running.

## v1.01 (2026-07-01)

### Added
- **Hide when worn (per item):** an injected item can now be hidden while a real
  equipped item occupies chosen vanilla biped slots, and reappears automatically
  when that slot is freed. Set it per content in the MCM (e.g. `37` to hide foot
  nails under boots, or `30 31 42` to hide a wig under an auto-equipped helmet).
- **Inventory tooltips (LoreBox integration):** hovering a Costume Box token in
  your inventory shows its packed item names. Updates live as you add/remove
  contents. *Optional — requires "LoreBox - Item and Spell Tooltips" + KID.*
- **Forced-gender NIF mode:** when capturing a worn item you are asked which body
  mesh to inject — *Use player's gender / Force Male / Force Female* (a simple
  message box; no extra dependency). Lets a costume captured on one body show the
  other body's mesh.
- **Presets for the Persist class:** the always-on Persist set can now adopt a
  preset just like a box, using the same `CEFP_*.json` files. A preset can only be
  assigned to one box or to Persist at a time (shared exclusivity). Includes
  "Export as preset" for the Persist set.
- **Male body support (experimental):** injected content now resolves to the
  player character's body sex, with an automatic fallback to the other sex's mesh
  when an item ships only one. *Female PC remains the primary tested path.*

### Changed
- **MCM: one page per box.** Each box now has its own page (listed on the left),
  fixing the page option/scroll limit that was hit with many boxes. The "Boxes"
  page is now a short overview with "+ New box". Adding or deleting a box updates
  the page list after you close and reopen the MCM.
- **Presets now carry their per-content settings.** Exporting a box/Persist set as
  a preset also saves each item's hide-when-worn slots and forced-gender mode, so
  a distributed costume keeps that behavior when imported.

### Removed
- Unused leftover `costume_seed.json` (the seed system was retired in v1.0).

### Dependencies
- **Required:** SKSE64, Address Library, SkyUI, RaceMenu (skee).
- **Optional (feature add-ons):** Keyword Item Distributor (KID) + LoreBox
  (inventory tooltips).

---

## v1.0 (2026-06-27)
- Initial public release. Skin-rebind injection framework: attach skinned
  accessories (nails / piercings / costumes) outside the biped slot system to free
  real equipment slots. Persist class (always on) + box class (token-gated), body
  morph follow, keyword/stat passthrough, MCM, presets, clean uninstall.
