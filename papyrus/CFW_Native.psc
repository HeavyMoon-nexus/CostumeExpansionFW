Scriptname CFW_Native Hidden

; Costume Expansion FW - native API (Phase A).
; Backed by CostumeExpansionFW.dll (RegisterPapyrus). All scene-graph mutators
; are deferred to the game's main thread, so they return True meaning
; "accepted/queued"; re-query GetActive()/IsActive() next frame to confirm.

; --- Queries -----------------------------------------------------------------

; Currently active (registered/injected) item ids.
String[] Function GetActive() Global Native

; True if the given colon-form id is currently active.
Bool Function IsActive(String id) Global Native

; Resolve a colon-form id "XXXXXX:Plugin.esp" to its runtime Form (None on
; failure). Use to equip/unequip a box token from the MCM.
Form Function ResolveForm(String colonId) Global Native

; --- Mutators (deferred to main thread; return True == accepted) --------------

; Register + inject a persist item (always shown) by colon-form id.
Bool Function RegisterPersist(String id) Global Native

; Define a box item: content (colon-form ARMA id) shown only while the box token
; (colon-form ARMO id) is worn.
Bool Function DefineBox(String content, String token) Global Native

; Detach + unregister one item by id.
Bool Function Detach(String id) Global Native

; Detach + unregister every active item.
Bool Function Clear() Global Native

; --- Box store (Phase B box UI) ----------------------------------------------
; Box defs are GLOBAL config persisted in costume_boxes.json (one box per token).
; Queries are index-based for listing; mutators are token-keyed and deferred to
; the main thread (True == accepted; re-read after ForcePageReset to confirm).

; Number of defined boxes.
Int Function GetBoxCount() Global Native

; The box's user-facing label / token colon-form id / packed content ids.
String Function GetBoxLabel(Int index) Global Native
String Function GetBoxToken(Int index) Global Native
String[] Function GetBoxContents(Int index) Global Native

; True if the box's token is currently worn by the player.
Bool Function IsBoxWorn(Int index) Global Native

; The box token's runtime Form (None if unresolved) - for EquipItem/UnequipItem.
Form Function GetBoxTokenForm(Int index) Global Native

; Create the box for token (with label) if absent, then append content if given.
Bool Function AddBox(String label, String token, String content) Global Native

; Remove one content from a box, or the whole box, or relabel it. returnStored
; (border audit ROOT A): the MCM passes false - it returns the captured item
; itself (its store-only-vs-fabricate custody). Another mod calling this native
; directly passes true so the item is handed back, not stranded in the store.
Bool Function RemoveBoxContent(String token, String content, Bool returnStored = false) Global Native
Bool Function RemoveBox(String token, Bool returnStored = false) Global Native
Bool Function SetBoxLabel(String token, String label) Global Native

; Currently-worn player armors (excluding our box tokens): display names and the
; parallel colon-form ids, same order. The "capture worn item" flow.
String[] Function GetWornItemNames() Global Native
String[] Function GetWornItemIds() Global Native

; ALL carried player armors (worn included; box tokens excluded), name-sorted,
; optionally narrowed by a case-insensitive name substring, and natively capped
; at 120. The "+ Add from inventory" capture flow - no equip needed, so no
; transient FSMP physics build happens.
String[] Function GetInventoryItemNames(String filter = "") Global Native
String[] Function GetInventoryItemIds(String filter = "") Global Native

; Pre-capture guard (review item 2): TRUE if the content id resolves to a usable
; mesh right now (same checks the queued registration makes). Capture flows call
; this BEFORE moving the physical item.
Bool Function CanResolveContent(String content) Global Native

; Create a new empty box auto-assigning the next free pool token. False if the
; shipped token pool (Costume Box 1..N) is exhausted.
Bool Function NewBox(String label) Global Native

; In-game item name for a colon-form id (falls back to the id).
String Function GetItemName(String colonId) Global Native

; --- Box abilities (Phase C) -------------------------------------------------
; A box can grant one ability (Spell) while its token is worn. The shipped
; ability catalog (Costume: Armor/Health/Magicka/...) drives the MCM picker.

; Catalog of shipped ability spells: display names + parallel colon-form ids.
String[] Function GetAbilityNames() Global Native
String[] Function GetAbilityIds() Global Native

; The box's currently-assigned ability colon-id ("" if none), by index.
String Function GetBoxAbility(Int index) Global Native

; Assign (or clear, with ability="") a box's OPTIONAL manual ability by token.
; (The box also auto-aggregates its contents' own stats; see GetBoxStats.)
Bool Function SetBoxAbility(String token, String ability) Global Native

; Human-readable summary of a box's AUTO-aggregated stats (armor + weight +
; each content's enchantment effects), reflected while the token is worn.
String Function GetBoxStats(Int index) Global Native

; --- Slot-token model (per-slot boxes + distribute toggle) -------------------
; Each box is bound to a fixed biped slot (44-60) via its "Costume Box <N>" token.

; Free (unused) slot-token colon-ids, slot-sorted - the "new box" slot picker.
String[] Function GetFreeTokenIds() Global Native

; The biped slot number (30-61) a token's ARMO occupies (0 if none).
Int Function GetTokenSlot(String token) Global Native

; The box index on biped slot `slot`, or -1 if none. Lets the per-box MCM pages
; (named by slot) resolve to their box even after a deletion shifts box indices.
Int Function GetBoxBySlot(Int slot) Global Native

; Box "distribute" flag: whether the token is given to / kept on the player.
Bool Function GetBoxEnabled(Int index) Global Native

; Set the distribute flag (off = the token is removed; escape hatch for a slot
; that conflicts in the load order). Flag only - caller equips/removes the token.
Bool Function SetBoxEnabled(String token, Bool enabled) Global Native

; Token armor class: 0=Clothing, 1=Light, 2=Heavy. Clothing gives NO armor even
; with a rating, so a box holding armor needs Light/Heavy to make armor count.
Int Function GetBoxArmorType(Int index) Global Native
Bool Function SetBoxArmorType(String token, Int armorType) Global Native

; --- Global settings (Main page) ---------------------------------------------
; Master CEF on/off (off = nothing injected). SetEnabled queues a reconcile.
Bool Function IsEnabled() Global Native
Bool Function SetEnabled(Bool on) Global Native

; True if RaceMenu/skee BodyMorph was acquired (body-morph follow available).
Bool Function IsBodyMorphReady() Global Native

; --- Presets (CEFP_*.json in SKSE\Plugins\CEF\Presets) -----------------------
; Distributable content sets. A box can be assigned a preset (exclusive: one
; preset <-> one box). Names and files are parallel arrays (act on the file).
String[] Function GetPresetNames() Global Native
String[] Function GetPresetFiles() Global Native

; Export a box's current contents as a new preset file; returns the file name.
String Function ExportPreset(String token, String name) Global Native

; Assign a preset (by file) to a box (replaces its contents). False on bad input
; or if the preset is already assigned to another box.
Bool Function AssignPreset(String token, String file) Global Native

; Clear a box's preset (back to manual; keeps current contents).
Bool Function ClearPreset(String token) Global Native

; The box's applied preset name ("" if manual).
String Function GetBoxPreset(String token) Global Native

; The token of the box currently using this preset name ("" if free).
String Function GetPresetAssignedTo(String name) Global Native

; --- Persist (token-less; worn-capture like a box) ----------------------------
; M2 (CEF_STATE_SCOPE.md): GetPersistContents = the shared CATALOG (all saves);
; what actually shows is the per-save ACTIVE set below. AddPersist = catalog add
; + activate on this save (+ capture); RemovePersist = catalog delete (other
; saves keep their actives).
String[] Function GetPersistContents() Global Native
Bool Function AddPersist(String content) Global Native
Bool Function RemovePersist(String content, Bool returnStored = false) Global Native

; Hand the MCM's hidden holding container (CFW_Storage) to the native layer so
; console / external-mod removal borders can return captured items (ROOT A).
; Call from OnConfigOpen; the native clears it on load until re-handed.
Function SetStoreRef(ObjectReference akStore) Global Native

; Where a content id is already held: "" if free, "persist" if in the persist
; catalog, else the holding box's token. Capture flows check this BEFORE moving
; the physical item - the injection registry is one-entry-per-id, so a second
; box/persist holding the same id would silently steal the display (P1-1).
String Function FindContentHolder(String content) Global Native

; Re-read CEF_settings.json NOW (boxes/catalog/hide/gender/morph/enchants) and
; re-apply. Per-save persist actives survive (co-save state). Queued to the
; main thread - the MCM page shows the new state on the next open.
Function ReloadSettings() Global Native

; Per-save activation (M2). SetPersistActive is VISUAL-ONLY (never moves items);
; on requires the id to be in the catalog.
String[] Function GetPersistActive() Global Native
Bool Function IsPersistActive(String id) Global Native
Bool Function SetPersistActive(String id, Bool bOn) Global Native

; Per-content body-morph opt-in (default OFF; the `cef morph` levers). Set
; queues a re-inject so the change shows immediately.
Bool Function GetBodyMorph(String id) Global Native
Function SetBodyMorph(String id, Bool bOn) Global Native

; Diagnostics page: compact status lines composed natively. Lines starting
; "# " are section headers.
String[] Function GetDiagLines() Global Native

; --- Hide-when-worn (§8.10) --------------------------------------------------
; A content id is hidden while one of the given vanilla biped slots (space-
; separated numbers, e.g. "30 31 42") is occupied by non-CEF real equipment,
; and is auto-reshown when freed. Works for box contents and persist items.
; GetHideSlots returns the current list as a string ("" = no rule). SetHideSlots
; replaces it; an empty/blank string clears the rule.
String Function GetHideSlots(String id) Global Native
Bool Function SetHideSlots(String id, String slots) Global Native

; --- Forced-gender NIF mode (per content) ------------------------------------
; Which body's mesh a content injects: 0 = follow the player's sex, 1 = force
; Male, 2 = force Female. SetContentGender re-resolves + re-injects on the main
; thread. Set from the MCM per-content "Body" menu (v1.2.1; no longer asked at
; capture - new captures start at 0 = follow the player).
Int Function GetContentGender(String content) Global Native
Bool Function SetContentGender(String content, Int mode) Global Native

; --- Persist preset (same CEFP_*.json format as box presets) ------------------
; The persist class can adopt a preset like a box; shared exclusivity pool (a
; preset on persist cannot also be on a box). Assign REPLACES persist contents.
String Function GetPersistPreset() Global Native
String Function ExportPersist(String name) Global Native
Bool Function AssignPersistPreset(String file) Global Native
Bool Function ClearPersistPreset() Global Native

; --- Enchantment capture -----------------------------------------------------
; Snapshot the currently-WORN item's effective enchantment (base OR player/
; instance enchantment) for a content, so the token's/persist's synthesized
; ability reproduces it. Call at capture time while the item is still equipped
; (before moving it to the store). Returns True if an enchantment was found.
Bool Function CaptureContentEnchant(String content) Global Native

; True if the content's base form has any Papyrus script attached (VMAD). CEF
; injects the mesh but never actually equips the item (and removes it from the
; inventory), so equip-/possession-driven scripts won't run - used to warn at
; capture. Best-effort (base-form scripts).
; True only for REAL attached scripts (native VM wrapper objects - which every
; form passed through Papyrus acquires - are filtered out).
Bool Function ContentHasScript(String content) Global Native
