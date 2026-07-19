#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
    class TESObjectREFR;
}

namespace CostumeFW
{
    // A box definition: a token ARMO (colon-form id) that, while worn, shows its
    // packed contents (colon-form ARMA ids). label is a user-facing name. Box
    // defs are GLOBAL config (all saves), persisted in CEF_settings.json - NOT
    // in the co-save (which holds only per-save persist items). One box per token.
    struct BoxDefInfo
    {
        std::string label;
        std::string token;
        std::vector<std::string> contents;
        std::string ability;  // colon-form Spell id granted while the token is worn ("" = none)
        bool enabled{ true };  // distribute the token to the player? off = removed (conflict escape)
        int armorType{ 0 };    // token armor class: 0=Clothing, 1=Light, 2=Heavy (armorRating
                               // only counts toward DR for Light/Heavy, not Clothing)
        std::string preset;    // applied preset name (CEFP_ suffix), "" = manual contents
        bool uiVisible{ true };  // show this box in the MCM box list
        bool wear{ false };      // desired default Wear (show contents) state
    };

    // --- Global CEF settings (CEF_settings.json) ----------------------------
    // Master on/off for the whole framework (Main page). Off = nothing injected.
    bool CefEnabled();
    bool SetCefEnabled(bool a_on);  // persists + returns the new state

    // Token-less always-injected persist content (colon-form ids).
    std::vector<std::string> PersistContents();
    bool AddPersistContent(const std::string& a_content);     // def + json (dedup)
    bool RemovePersistContent(const std::string& a_content);  // def + json

    // Persist preset (§ persist-preset): the persist class can adopt a CEFP preset
    // exactly like a box - same CEFP_*.json format, same exclusivity pool (a preset
    // assigned to persist cannot also be on a box, and vice versa). Assigning
    // REPLACES the persist contents with the preset's. "persist" is the sentinel
    // PresetAssignedTo() returns when persist holds a preset.
    std::string PersistPreset();  // applied preset name ("" if manual)
    bool AssignPresetToPersist(const std::string& a_presetName,
        const std::vector<std::string>& a_contents);  // def + json (exclusivity)
    bool ClearPersistPreset();  // back to manual (keeps current contents); def + json

    // --- Hide-when-worn rules (§8.10) ----------------------------------------
    // A content id (box content OR persist) is hidden while ANY of its listed
    // vanilla biped slots (30-61) is occupied by NON-CEF real equipment, and is
    // auto-reshown when that slot frees. Lets the user selectively reclaim the
    // vanilla "hide -> unequip restores" ARMA mask CEF bypasses (e.g. hide foot
    // nails under boots/slot 37, hide a wig under an auto-helmet/slot 30,31,42).
    // GLOBAL config (CEF_settings.json), keyed by content id so persist and box
    // items are covered uniformly. Empty list = clear the rule.
    std::vector<int> HideSlotsFor(const std::string& a_id);
    bool SetHideSlots(const std::string& a_id, const std::vector<int>& a_slots);  // def + json

    // --- Forced-gender NIF mode (per content) --------------------------------
    // Which body's ARMA model a content injects: 0 = follow the player's sex
    // (default), 1 = force Male, 2 = force Female. Lets a costume captured on one
    // body show the other body's mesh on demand. GLOBAL config (CEF_settings.json),
    // keyed by content id (box + persist alike). 0 = clear the entry.
    int GenderModeFor(const std::string& a_id);
    bool SetGenderMode(const std::string& a_id, int a_mode);

    // --- Body-morph opt-in (per content) -------------------------------------
    // Whether a content id's injected mesh receives the player's skee body morph
    // (RaceMenu body sliders). Default OFF: body morph is only needed for
    // BodySlide/body-conforming meshes, and applying it to accessories drove a
    // severe memory balloon (skee ApplyVertexDiff allocations retained by SSE
    // Engine Fixes' allocator). CEF's custom-slot content can't be auto-classified
    // (nails/piercings use arbitrary slots), so the user opts in per content
    // ("turn it on if the mesh looks wrong"). GLOBAL config (CEF_settings.json).
    bool BodyMorphOn(const std::string& a_id);
    bool SetBodyMorphOn(const std::string& a_id, bool a_on);  // def + json  // def + json

    // --- Show the player's real body under this content (per content) --------
    // Opt-in (default off). Pairs with HideShapes: after dropping a costume's own
    // body shape, inject the player's morphed skin (naked) body so the garments
    // sit on the real body. Intended for body-slot tokens that mask the real body.
    bool ShowRealBodyOn(const std::string& a_id);
    bool SetShowRealBodyOn(const std::string& a_id, bool a_on);  // def + json

    // --- Per-content SHAPE hide (default none) -------------------------------
    // A costume that ships its own body doubles the player's real BodySlide body
    // (identical meshes overlap invisibly, but a minor variant - 3BA vs 3BAv2 -
    // clips). The biped slot can't tell the naked-body shape from a garment (a
    // whole slot-32 outfit skins every shape to slot 32), so the user picks WHICH
    // shapes to drop BY NAME. The injection skips a shape whose NIF name is in this
    // content's set. GLOBAL config (CEF_settings.json), keyed by content id.
    std::vector<std::string> HideShapesFor(const std::string& a_id);
    bool IsHideShape(const std::string& a_id, const std::string& a_shape);
    bool SetHideShape(const std::string& a_id, const std::string& a_shape, bool a_on);  // def + json

    // Runtime cache of a content's injected shapes (shape NIF name -> biped slot,
    // -1 if not dismembered), populated on the main thread by the injection and by
    // `cef shapes` so the MCM can list them without loading the NIF on the VM
    // thread. NOT persisted; empty until the content is injected/scanned once.
    std::vector<std::pair<std::string, int>> ContentShapesFor(const std::string& a_id);
    void SetContentShapes(const std::string& a_id,
        const std::vector<std::pair<std::string, int>>& a_shapes);

    // --- LoreBox tooltip integration (soft dependency) -----------------------
    // The comma-joined in-game names of the contents of the box on biped slot
    // a_slot ("" if there is no box on that slot, or it is empty). Fed to the
    // BSScaleformTranslator hook so the LoreBox mod (if installed) can show a
    // token's packed contents when the token is hovered in the inventory.
    std::string LoreBoxContentsForSlot(int a_slot);

    // --- Presets (assignment; def + json only, exclusivity-checked) ----------
    // The token of the box currently using a_presetName, "" if none (exclusivity).
    std::string PresetAssignedTo(const std::string& a_presetName);
    // This box's applied preset name ("" if manual contents).
    std::string BoxPreset(const std::string& a_token);
    // Assign a_presetName (with its already-read a_contents) to a box: replaces the
    // box's contents with the preset's. Rejected (false) if another box already uses
    // a_presetName (1 preset <-> 1 box). def + json only; the caller re-registers the
    // scene (detach old content, register new, Reconcile) on the main thread.
    bool AssignPreset(const std::string& a_token, const std::string& a_presetName,
        const std::vector<std::string>& a_contents);
    // Detach the preset from a box (back to manual; keeps the current contents). def+json.
    bool ClearPreset(const std::string& a_token);

    // Load costume_boxes.json into the in-memory box store and register every
    // content for worn-token-gated injection. Call once on kDataLoaded. The
    // caller Reconciles afterward. Main thread (mutates the registry).
    void LoadBoxes();

    // MCM "Reload settings from disk": drop every injected node + the whole
    // registry, re-run LoadBoxes() against the CURRENT CEF_settings.json, then
    // re-register ALL of this save's persist actives (co-save-owned - the JSON
    // knows nothing about them; uncataloged actives survive too, like the
    // co-save restore) and reconcile. The transient empty-actives window
    // coalesces into one debounced head rebuild. Main thread.
    void ReloadSettingsFromDisk();

    // True if the FormID is any defined box's token - so the equip sink fires even
    // for ability-only boxes, and hide-when-worn treats it as "our own".
    bool IsBoxToken(std::uint32_t a_form);

    // Resilience: if a defined box's token has left the player's inventory (sold /
    // dropped) and the player now has none, give one back. So the box system can
    // never be permanently broken by losing the token. Main thread.
    void ReplenishToken(std::uint32_t a_tokenForm);

    // If the token is currently worn, re-equip it (unequip+equip) so consumer
    // mods that detect worn keywords on the equip event re-scan. Needed only when
    // a box's contents (hence its passthrough keywords) change WHILE the token is
    // already worn - the vanilla equip event is the only worn-keyword refresh
    // trigger. No-op if not worn. Main thread (touches the actor).
    void RefreshWornToken(const std::string& a_token);

    // Repoint each box token's ARMA at its current carrier revision from
    // carriers.json (restart-free FSMP carrier swaps; the revision slots are
    // pre-created files that tools/nifcarrier `sync` rewrites in place).
    // Volatile in-memory form edit, reapplied on every settings load. With
    // a_refreshChanged, re-equips worn tokens whose revision changed so the
    // engine loads the new (uncached) path and FSMP rebuilds. Main thread.
    void ApplyCarrierOverrides(bool a_refreshChanged);

    // Re-register every box's contents into the active registry after a co-save
    // revert wiped it (the box store's own list survives revert). Does not touch
    // the json. Persist is NOT reapplied here: its ACTIVE set is per-save and
    // the co-save restore owns it (M2, CEF_STATE_SCOPE.md §3). Call on
    // kPostLoadGame before Reconcile. Main thread.
    void ReapplyBoxes();

    // M2 (CEF_STATE_SCOPE.md §3): activate/deactivate a persist item ON THIS
    // SAVE without touching the shared catalog (`cef persist on|off <id>`).
    // on: the id must already be in the catalog; registers + reconciles.
    // off: detaches + unregisters (works for active-but-uncataloged ids too).
    // Main thread.
    bool PersistSetActive(const std::string& a_id, bool a_on);

    // Rewrite the carrier manifest so its persist fragment tracks THIS SAVE'S
    // ACTIVE set. Call after a persist activation change lands in the registry
    // (post-registration task / co-save restore); compare-skips when unchanged.
    // The WriteJson path also writes the manifest, but it runs BEFORE the
    // registration task, so activation changes need this explicit second pass.
    void SyncPersistManifest();

    // This save's ACTIVE persist ids (registry token-less entries, sorted).
    std::vector<std::string> PersistActiveIds();

    // MCM Diagnostics page: compact status lines composed on the C++ side so
    // the console levers and the MCM share one source of truth (the Papyrus
    // just renders). Lines starting "# " are section headers. VM-thread
    // tolerated (read-only snapshots).
    std::vector<std::string> DiagLines();

    // --- approach-C persist head carriers (stage 3b) --------------------------
    // The persist class's SMP physics rides the facegen head path: nifcarrier
    // `sync` builds a head-part POOL (CFW_PersistCarrier = bones + physics XML +
    // trigger shape, CFW_PersistProxy01..08 = one per additional collision mesh)
    // and records the assignment in carriers.json's "persist" entry.
    // ApplyCarrierOverrides() repoints the pool HDPT models at the current
    // revision AND reconciles the player's headParts registration (carrier +
    // assigned proxies while the persist SMP set is non-empty and CEF is on;
    // deregistered otherwise), rebuilding the head when anything changed.
    // These are the manual console levers on top of that:
    void PersistCarrierStatus();  // `cef persist` - print pool registration + entry
    void PersistCarrierRemove();  // `cef persist remove` - deregister pool + rebuild head

    // One currently-worn armor: its display name + colon-form id (for the MCM
    // "add worn item" capture flow).
    struct WornItem
    {
        std::string name;
        std::string id;
    };

    // Currently-equipped player ARMOs (excluding our own box tokens), for capture.
    std::vector<WornItem> WornArmors();

    // ALL carried player ARMOs (worn included; our box tokens excluded), for the
    // "+ Add from inventory" capture flow - the item never has to be equipped,
    // so no transient FSMP armor build happens. Sorted by name, optionally
    // narrowed by a case-insensitive name substring (the MCM "Inventory
    // filter" input), and capped natively at 120 rows (SkyUI's menu dialog
    // degrades past ~128).
    std::vector<WornItem> InventoryArmors(const std::string& a_filter);

    // The shipped slot-token pool (CostumeFW.esp ARMOs named "Costume Box*"),
    // as colon-form ids, sorted by their biped slot number. FreeTokens = those not
    // yet bound to a box def (for the MCM "new box" slot picker).
    std::vector<std::string> TokenPool();
    std::vector<std::string> FreeTokens();
    std::string NextFreeToken();  // first free slot-token ("" if none) - legacy auto-assign

    // The biped slot number (30-61) a token ARMO occupies, 0 if none/unresolved.
    int TokenSlot(const std::string& a_token);

    // The box index whose token occupies biped slot a_slot, or -1 if none. Lets
    // the MCM's per-box pages (named by slot) resolve to their box even after a
    // deletion shifts box indices.
    int BoxIndexForSlot(int a_slot);

    // Per-box "distribute" flag: whether the token is given to / kept on the
    // player. Off removes it (escape hatch for a slot that conflicts in the LO).
    bool BoxEnabled(const std::string& a_token);
    bool SetBoxEnabled(const std::string& a_token, bool a_enabled);

    // Per-box token armor class (0=Clothing, 1=Light, 2=Heavy). Clothing gives no
    // armor even with a rating, so a box holding armor needs Light/Heavy here.
    int BoxArmorType(const std::string& a_token);
    bool SetBoxArmorType(const std::string& a_token, int a_type);

    // Create a new (empty) box auto-assigning the next free pool token. Returns
    // false if the pool is exhausted. Definition-only (no scene change).
    bool NewBox(const std::string& a_label);

    // Resolve a colon-form id to its in-game item name (falls back to the id).
    std::string ItemDisplayName(const std::string& a_colonId);

    // `cef recover <id>`: deliberately grant ONE copy of a content item to the
    // player. Drains the hidden store first (returns the captured original), so it
    // no longer double-copies a still-stored item; fabricates a fresh base copy
    // only when this save has no stored copy. Main thread (touches inventory).
    bool RecoverContentItem(const std::string& a_id);

    // --- Hidden-store custody (border audit 2026-07-09, ROOT A) ---------------
    // The MCM owns a disabled holding container (CFW_Storage 0x80D, PlaceAtMe) that
    // preserves captured items WITH their instance data (tempering / player enchant).
    // It was reachable ONLY from the MCM script, so every non-MCM removal route
    // (console, another mod's direct native call) stranded the physical item. The
    // MCM now hands the ref to the native layer (OnConfigOpen) so those borders can
    // return it too. Cleared on game load (the ref is per-save). Main thread.
    void SetStoreRef(RE::TESObjectREFR* a_store);

    // Return one captured copy of a_id to the player: the hidden store's original
    // if it holds one (keeps instance data), else - only if a_fabricate - a fresh
    // base copy (the accepted cross-save duplication, CEF_STATE_SCOPE.md §4).
    // Returns true if a copy reached the player. Main thread (touches inventory).
    bool ReturnStoredItem(const std::string& a_id, bool a_fabricate);

    // --- SMF-era custody (P2): the native layer OWNS the hidden store ---------
    // The store FormID is co-save persisted ('STOR'), so an SMF-only session can
    // capture/return without the MCM ever opening. The MCM's SetStoreRef handoff
    // becomes the legacy-adoption path (first resolving store wins per save).
    std::uint32_t StoreFormId();                    // 0 = none recorded
    void RestoreStoreFormId(std::uint32_t a_id);    // co-save load/revert path
    // Resolve the recorded store, else CREATE it (PlaceAtMe CFW_Storage 0x80D at
    // the player, disabled, force-persist) and record it. Main thread only.
    RE::TESObjectREFR* EnsureStoreRef();
    // Move ONE physical copy of a_id from the player into the hidden store,
    // preferring the worn / instance-data ExtraDataList so tempering + player
    // enchants travel with it (what the MCM's Papyrus move preserved). Main
    // thread only.
    bool CaptureItemToStore(const std::string& a_id);
    // Equip (adding one if absent) / unequip a box token, silent - the native
    // twin of the MCM's ToggleBoxToken. Main thread only.
    bool WearBoxToken(const std::string& a_token, bool a_wear);
    // Distribute-toggle item side: give one token, or unequip + remove all
    // copies - the native twin of the MCM's ToggleDistribute. Main thread only.
    void GiveOrRemoveToken(const std::string& a_token, bool a_give);

    // --- Box abilities (Phase C) ---------------------------------------------
    // The shipped ability-spell catalog (CostumeFW.esp SPEL named
    // "Costume:*"), as {name, colon-id}; used by the MCM ability picker.
    std::vector<WornItem> AbilityCatalog();

    // The box's assigned ability colon-id ("" if none), by token.
    std::string BoxAbility(const std::string& a_token);

    // Set/clear a box's ability (def + json only; caller applies on main thread).
    bool SetBoxAbility(const std::string& a_token, const std::string& a_ability);

    // Snapshot a content's EFFECTIVE enchantment (base OR player/instance
    // enchantment) into the store, keyed by content id, so the synthesized ability
    // can reproduce it later (the base ARMO alone misses instance enchantments).
    // Prefers the WORN player item matching the content's base form; falls back
    // to any carried entry, so the inventory-capture flow (item never equipped)
    // still snapshots player enchantments. Stores the effect list (MGEF colon-id
    // + magnitude); def + json. Returns true if an enchantment was found and
    // stored. Main/VM thread (inventory read only).
    struct EnchantEffectInfo
    {
        std::string mgef;
        float magnitude{ 0.0f };
    };
    std::vector<EnchantEffectInfo> ContentEnchantSnapshot(const std::string& a_content);
    bool CaptureEnchant(const std::string& a_content);

    // Sync every box's abilities to the player: token worn -> AddSpell, else
    // RemoveSpell. Applies the AUTO-synthesized stat ability (contents' enchant +
    // armor + weight) plus any optional manual ability, AND the persist class's
    // aggregate enchant ability (granted while CEF is enabled). Idempotent. Main thread.
    void ApplyBoxAbilities();

    // Drop + forget the persist class's synthesized enchant ability so the next
    // ApplyBoxAbilities rebuilds it (call after persist contents change). Main thread.
    void RebuildPersistAbility();

    // Drop + forget a box's synthesized stat ability so the next ApplyBoxAbilities
    // rebuilds it (call after its contents change). Main thread.
    void RebuildBoxAbility(const std::string& a_token);

    // Forget all synthesized abilities (call on game load; dynamic forms are
    // dropped by save/load, so just clear the cache and rebuild). Main thread.
    void ClearBoxSpellCache();

    // Human-readable summary of a box's aggregated stats (for the MCM display).
    std::string BoxStatsSummary(int a_index);

    // Remove one (manual) ability spell from the player if present. Main thread.
    void RemoveBoxAbilitySpell(const std::string& a_ability);

    // --- Queries (read-only; index-based for MCM listing) ---
    int BoxCount();
    BoxDefInfo BoxAt(int a_index);            // empty fields if out of range
    bool BoxWornAt(int a_index);              // token currently equipped on player
    std::uint32_t BoxTokenFormAt(int a_index);  // resolved token FormID (0 if none)
    std::vector<std::string> BoxContents(const std::string& a_token);  // by token (for detach)

    // --- Definition mutators (token-keyed; edit the store + rewrite json ONLY) ---
    // These do NOT touch the scene graph / active registry, so they are safe to
    // call synchronously from the Papyrus VM thread (giving the MCM immediate
    // feedback). The caller separately queues RegisterBoxById/DetachSkinned/
    // Reconcile on the main thread to apply the visual change.
    //
    // Where a content id is already held: "" if free, "persist" if in the
    // shared persist catalog, else the holding box's token. The injection
    // registry is keyed per content id (a second registration silently steals
    // the first, and removing either holder wipes the shared per-content
    // settings), so capture flows must block while ANY box or the persist
    // catalog holds the id (review 2026-07-07 P1-1).
    std::string ContentHolder(const std::string& a_content);

    // ROOT C (border audit 2026-07-09): TRUE if the colon-form id belongs to a CEF
    // plugin (CostumeFW.esp or the two pre-merge plugins). CEF's own records are box
    // tokens / carrier / pool / ability forms - NEVER valid CONTENT (content is
    // third-party costume meshes). Ingest borders reject a CEF-own id used as content
    // (the MCM capture lists already filter it via IsTokenPluginFile; the native /
    // preset / hand-edited-JSON routes did not). Cheap string check - no form lookup.
    bool IsTokenColonId(const std::string& a_id);

    // AddBox: create the box for a_token if absent (with a_label), then append
    // a_content if non-empty and not already present. Returns false on bad input.
    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content);
    bool RemoveBoxContent(const std::string& a_token, const std::string& a_content);
    bool RemoveBox(const std::string& a_token);
    bool SetBoxLabel(const std::string& a_token, const std::string& a_label);
}
