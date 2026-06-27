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

; Remove one content from a box, or the whole box, or relabel it.
Bool Function RemoveBoxContent(String token, String content) Global Native
Bool Function RemoveBox(String token) Global Native
Bool Function SetBoxLabel(String token, String label) Global Native

; Currently-worn player armors (excluding our box tokens): display names and the
; parallel colon-form ids, same order. The "capture worn item" flow.
String[] Function GetWornItemNames() Global Native
String[] Function GetWornItemIds() Global Native

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

; --- Persist (token-less, always-shown; worn-capture like a box) --------------
String[] Function GetPersistContents() Global Native
Bool Function AddPersist(String content) Global Native
Bool Function RemovePersist(String content) Global Native
