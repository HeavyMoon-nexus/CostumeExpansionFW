# Changelog

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
