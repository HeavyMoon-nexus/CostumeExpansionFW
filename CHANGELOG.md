# Changelog

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
  mesh to inject — *Use player's gender / Force Male / Force Female*. Lets a
  costume captured on one body show the other body's mesh. *The popup requires
  UIExtensions; without it, captures default to the player's gender.*
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
  (inventory tooltips), UIExtensions (gender-choice popup).

---

## v1.0 (2026-06-27)
- Initial public release. Skin-rebind injection framework: attach skinned
  accessories (nails / piercings / costumes) outside the biped slot system to free
  real equipment slots. Persist class (always on) + box class (token-gated), body
  morph follow, keyword/stat passthrough, MCM, presets, clean uninstall.
