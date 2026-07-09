#include "Papyrus.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "Preset.h"
#include "SkinRebind.h"

#include "RE/B/BSAtomic.h"
#include "RE/I/IObjectHandlePolicy.h"
#include "RE/S/SkyrimVM.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"
#include "RE/V/VirtualMachine.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        constexpr const char* kClass = "CFW_Native";

        // --- Queries (read-only; safe on the Papyrus VM thread) ---------------
        // g_active is mutated only by main-thread tasks; while the MCM is open the
        // game is paused, so no Reconcile/equip task races these reads.

        std::vector<RE::BSFixedString> GetActive(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& it : ActiveSnapshot()) {
                out.emplace_back(it.id);
            }
            return out;
        }

        bool IsActive(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            const std::string id = a_id.c_str();
            for (const auto& it : ActiveSnapshot()) {
                if (it.id == id) {
                    return true;
                }
            }
            return false;
        }

        // Resolve a colon-form id "XXXXXX:Plugin.esp" to its runtime Form (none on
        // failure). Lets the MCM equip/unequip a box token without re-parsing the
        // FormKey in Papyrus. TESDataHandler lookup only - no scene graph.
        RE::TESForm* ResolveForm(RE::StaticFunctionTag*, RE::BSFixedString a_colonId)
        {
            const std::string colonId = a_colonId.c_str();
            const auto colon = colonId.find(':');
            if (colon == std::string::npos) {
                return nullptr;
            }
            std::uint32_t localID = 0;
            try {
                localID = static_cast<std::uint32_t>(std::stoul(colonId.substr(0, colon), nullptr, 16));
            } catch (...) {
                return nullptr;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return nullptr;
            }
            return dh->LookupForm(localID, colonId.substr(colon + 1));
        }

        // --- Mutators (scene-graph touching; deferred to the main thread) ------
        // These return true == "accepted/queued", not "succeeded". The MCM should
        // re-read GetActive()/IsActive() on the next frame to confirm the result.

        bool RegisterPersist(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            const std::string id = a_id.c_str();
            if (id.empty()) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([id] { InjectArmaById(id); });
            return true;
        }

        bool DefineBoxNative(RE::StaticFunctionTag*, RE::BSFixedString a_content, RE::BSFixedString a_token)
        {
            const std::string content = a_content.c_str();
            const std::string token = a_token.c_str();
            if (content.empty() || token.empty()) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([content, token] { CostumeFW::DefineBox(content, token); });
            return true;
        }

        bool Detach(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            const std::string id = a_id.c_str();
            if (id.empty()) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([id] { DetachSkinned(id); });
            return true;
        }

        bool Clear(RE::StaticFunctionTag*)
        {
            SKSE::GetTaskInterface()->AddTask([] { DetachAll(); });
            return true;
        }

        // --- Box store: queries (read-only; index-based for MCM listing) -------

        std::int32_t GetBoxCount(RE::StaticFunctionTag*)
        {
            return BoxCount();
        }

        RE::BSFixedString GetBoxLabel(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxAt(a_index).label;
        }

        RE::BSFixedString GetBoxToken(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxAt(a_index).token;
        }

        std::vector<RE::BSFixedString> GetBoxContents(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& c : BoxAt(a_index).contents) {
                out.emplace_back(c);
            }
            return out;
        }

        bool IsBoxWorn(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxWornAt(a_index);
        }

        // The token's runtime Form, for the MCM to EquipItem/UnequipItem it.
        RE::TESForm* GetBoxTokenForm(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            const std::uint32_t form = BoxTokenFormAt(a_index);
            return form ? RE::TESForm::LookupByID(form) : nullptr;
        }

        // --- Box store: mutators (deferred to main thread; true == accepted) ---

        // Box mutators split work: the box-DEFINITION change (g_boxes + json) runs
        // synchronously here so the MCM's ForcePageReset reflects it immediately;
        // the SCENE/registry change (RegisterBoxById/DetachSkinned/Reconcile) is
        // queued to the main thread (NiSkinInstance touch must not race Load3D).

        bool AddBoxNative(RE::StaticFunctionTag*, RE::BSFixedString a_label,
            RE::BSFixedString a_token, RE::BSFixedString a_content)
        {
            const std::string label = a_label.c_str();
            const std::string token = a_token.c_str();
            const std::string content = a_content.c_str();
            if (!AddBox(label, token, content)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([token, content] {
                if (!content.empty()) {
                    RegisterBoxById(content, token);
                }
                Reconcile();           // show it now if the token is already worn
                RebuildBoxAbility(token);  // contents changed -> re-synth stats
                ApplyBoxAbilities();
                RefreshWornToken(token);  // re-fire equip so keyword detectors re-scan
            });
            return true;
        }

        // a_returnStored (border audit ROOT A): the MCM passes FALSE - it hands the
        // captured item back itself (its nuanced store-only-vs-fabricate custody).
        // Another mod calling this native directly passes TRUE so the item is not
        // stranded in the hidden store. Default FALSE keeps every existing MCM call
        // (2-arg) unchanged.
        bool RemoveBoxContentNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_content, bool a_returnStored)
        {
            const std::string token = a_token.c_str();
            const std::string content = a_content.c_str();
            if (!RemoveBoxContent(token, content)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([token, content, a_returnStored] {
                if (a_returnStored) {
                    ReturnStoredItem(content, true);  // external caller: return the captured item
                }
                DetachSkinned(content);
                RebuildBoxAbility(token);
                ApplyBoxAbilities();
                RefreshWornToken(token);  // re-fire equip so keyword detectors re-scan
            });
            return true;
        }

        bool RemoveBoxNative(RE::StaticFunctionTag*, RE::BSFixedString a_token, bool a_returnStored)
        {
            const std::string token = a_token.c_str();
            const std::vector<std::string> contents = BoxContents(token);  // capture first
            const std::string ability = BoxAbility(token);
            if (!RemoveBox(token)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([token, contents, ability, a_returnStored] {
                for (const auto& c : contents) {
                    if (a_returnStored) {
                        ReturnStoredItem(c, true);
                    }
                    DetachSkinned(c);
                }
                RemoveBoxAbilitySpell(ability);  // drop the manual spell if any
                RebuildBoxAbility(token);         // drop the synthesized stat spell
            });
            return true;
        }

        bool SetBoxLabelNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_label)
        {
            // Label-only: no scene change.
            return SetBoxLabel(a_token.c_str(), a_label.c_str());
        }

        // --- Worn-item capture + auto token (the simplified box workflow) ------

        std::vector<RE::BSFixedString> GetWornItemNames(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& w : WornArmors()) {
                out.emplace_back(w.name);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetWornItemIds(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& w : WornArmors()) {
                out.emplace_back(w.id);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetInventoryItemNames(RE::StaticFunctionTag*,
            RE::BSFixedString a_filter)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& w : InventoryArmors(a_filter.c_str())) {
                out.emplace_back(w.name);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetInventoryItemIds(RE::StaticFunctionTag*,
            RE::BSFixedString a_filter)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& w : InventoryArmors(a_filter.c_str())) {
                out.emplace_back(w.id);
            }
            return out;
        }

        // Pre-capture guard (review item 2): can this content's mesh resolve
        // right now? Pure data reads - synchronous, so the MCM can refuse the
        // capture BEFORE it moves the physical item.
        bool CanResolveContentNative(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            return CanResolveContent(a_content.c_str());
        }

        // Create an empty box auto-assigning the next free pool token (def-only,
        // synchronous so the MCM reflects it on ForcePageReset). False if the pool
        // is exhausted.
        bool NewBoxNative(RE::StaticFunctionTag*, RE::BSFixedString a_label)
        {
            return NewBox(a_label.c_str());
        }

        // Display name for a colon-form id (for showing box contents by name).
        RE::BSFixedString GetItemName(RE::StaticFunctionTag*, RE::BSFixedString a_colonId)
        {
            return ItemDisplayName(a_colonId.c_str());
        }

        // --- Slot-token model (per-slot boxes + distribute toggle) -------------

        // Free (unused) slot-token colon-ids, slot-sorted - the MCM "new box"
        // slot picker.
        std::vector<RE::BSFixedString> GetFreeTokenIds(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& t : FreeTokens()) {
                out.emplace_back(t);
            }
            return out;
        }

        // The biped slot number (30-61) of a token's ARMO (0 if none).
        std::int32_t GetTokenSlot(RE::StaticFunctionTag*, RE::BSFixedString a_token)
        {
            return TokenSlot(a_token.c_str());
        }

        // The box index on biped slot a_slot, or -1 (slot-keyed page resolution).
        std::int32_t GetBoxBySlot(RE::StaticFunctionTag*, std::int32_t a_slot)
        {
            return BoxIndexForSlot(a_slot);
        }

        // Box "distribute" flag (whether the token is given to the player).
        bool GetBoxEnabled(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxAt(a_index).enabled;
        }

        bool SetBoxEnabledNative(RE::StaticFunctionTag*, RE::BSFixedString a_token, bool a_enabled)
        {
            // Flag + json only; the MCM equips/removes the token to apply visually.
            return SetBoxEnabled(a_token.c_str(), a_enabled);
        }

        // Token armor class (0=Clothing, 1=Light, 2=Heavy). Clothing gives no armor.
        std::int32_t GetBoxArmorType(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxAt(a_index).armorType;
        }

        bool SetBoxArmorTypeNative(RE::StaticFunctionTag*, RE::BSFixedString a_token, std::int32_t a_type)
        {
            return SetBoxArmorType(a_token.c_str(), a_type);
        }

        // --- Box abilities (Phase C) ------------------------------------------

        std::vector<RE::BSFixedString> GetAbilityNames(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& a : AbilityCatalog()) {
                out.emplace_back(a.name);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetAbilityIds(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& a : AbilityCatalog()) {
                out.emplace_back(a.id);
            }
            return out;
        }

        RE::BSFixedString GetBoxAbility(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxAt(a_index).ability;
        }

        // Human-readable summary of the box's auto-aggregated stats (read-only).
        RE::BSFixedString GetBoxStats(RE::StaticFunctionTag*, std::int32_t a_index)
        {
            return BoxStatsSummary(a_index);
        }

        // Assign/clear a box's ability. Def change is synchronous (immediate MCM
        // feedback); the spell add/remove is queued to the main thread.
        bool SetBoxAbilityNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_ability)
        {
            const std::string token = a_token.c_str();
            const std::string ability = a_ability.c_str();
            const std::string oldAbility = BoxAbility(token);  // before the change
            if (!SetBoxAbility(token, ability)) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([oldAbility, ability] {
                if (!oldAbility.empty() && oldAbility != ability) {
                    RemoveBoxAbilitySpell(oldAbility);
                }
                ApplyBoxAbilities();  // grants the new one if the token is worn
            });
            return true;
        }

        // --- Global on/off (Main page) ---------------------------------------
        bool IsEnabled(RE::StaticFunctionTag*)
        {
            return CefEnabled();
        }

        // RaceMenu/skee BodyMorph acquired? (Main page dependency status.)
        bool IsBodyMorphReady(RE::StaticFunctionTag*)
        {
            return BodyMorph::Available();
        }

        bool SetEnabled(RE::StaticFunctionTag*, bool a_on)
        {
            SetCefEnabled(a_on);  // sync def + json
            SKSE::GetTaskInterface()->AddTask([] {
                Reconcile();  // master off hides everything; on re-shows
                ApplyBoxAbilities();
                // Persist head carriers follow the master switch: off deregisters
                // the pool (head physics gone), on re-registers it (stage 3b).
                ApplyCarrierOverrides(false);
            });
            return CefEnabled();
        }

        // --- Presets ---------------------------------------------------------
        // Catalog: parallel name/file arrays (MCM lists names, acts on files).
        std::vector<RE::BSFixedString> GetPresetNames(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& p : Preset::List()) {
                out.emplace_back(p.name);
            }
            return out;
        }

        std::vector<RE::BSFixedString> GetPresetFiles(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& p : Preset::List()) {
                out.emplace_back(p.file);
            }
            return out;
        }

        // Export a box's current contents as a new preset file (file I/O only,
        // no scene). Returns the written file name ("" on failure).
        // Gather the per-content hide rules + gender modes for a content list, to
        // carry into an exported preset (§8.10/8.11 + gender).
        void CollectPresetMaps(const std::vector<std::string>& a_contents,
            std::unordered_map<std::string, std::vector<int>>& a_hide,
            std::unordered_map<std::string, int>& a_gender)
        {
            for (const auto& c : a_contents) {
                auto slots = HideSlotsFor(c);
                if (!slots.empty()) {
                    a_hide[c] = std::move(slots);
                }
                if (const int g = GenderModeFor(c); g != 0) {
                    a_gender[c] = g;
                }
            }
        }

        // Restore a preset's hide rules + gender modes for its resolvable contents
        // (after assigning it to a box or persist).
        void RestorePresetMaps(const Preset::PresetInfo& a_preset,
            const std::vector<std::string>& a_ok)
        {
            for (const auto& [cid, slots] : a_preset.hideRules) {
                if (std::find(a_ok.begin(), a_ok.end(), cid) != a_ok.end()) {
                    SetHideSlots(cid, slots);
                }
            }
            for (const auto& [cid, mode] : a_preset.genderModes) {
                if (std::find(a_ok.begin(), a_ok.end(), cid) != a_ok.end()) {
                    SetGenderMode(cid, mode);
                }
            }
        }

        RE::BSFixedString ExportPresetNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_name)
        {
            const auto contents = BoxContents(a_token.c_str());
            std::unordered_map<std::string, std::vector<int>> hideRules;
            std::unordered_map<std::string, int> genderModes;
            CollectPresetMaps(contents, hideRules, genderModes);
            return Preset::Export(a_name.c_str(), contents, hideRules, genderModes);
        }

        // Assign a preset (by file) to a box: read + validate (skip missing), set the
        // def (exclusivity-checked), then queue the scene swap on the main thread.
        bool AssignPresetNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_file)
        {
            const std::string token = a_token.c_str();
            auto preset = Preset::Read(a_file.c_str());
            if (!preset.valid) {
                return false;
            }
            std::vector<std::string> ok, missing;
            Preset::Validate(preset.contents, ok, missing);
            if (!missing.empty()) {
                SKSE::log::warn("preset: '{}' has {} unresolved content (skipped)",
                    preset.name, missing.size());
            }
            const auto oldContents = BoxContents(token);  // before reassign
            if (!AssignPreset(token, preset.name, ok)) {  // sync def + json (exclusivity)
                return false;
            }
            // Restore the preset's hide rules + gender modes for its resolvable
            // contents so a distributed costume keeps its hide/gender behavior.
            RestorePresetMaps(preset, ok);
            SKSE::GetTaskInterface()->AddTask([token, oldContents, ok] {
                for (const auto& c : oldContents) {
                    DetachSkinned(c);
                }
                for (const auto& c : ok) {
                    RegisterBoxById(c, token);
                }
                Reconcile();
                RebuildBoxAbility(token);
                ApplyBoxAbilities();
                RefreshWornToken(token);
            });
            return true;
        }

        bool ClearPresetNative(RE::StaticFunctionTag*, RE::BSFixedString a_token)
        {
            return ClearPreset(a_token.c_str());  // contents remain (manual)
        }

        RE::BSFixedString GetBoxPreset(RE::StaticFunctionTag*, RE::BSFixedString a_token)
        {
            return BoxPreset(a_token.c_str());
        }

        // The token of the box currently using this preset ("" if free) - for the
        // MCM to gray out an already-assigned preset.
        RE::BSFixedString GetPresetAssignedTo(RE::StaticFunctionTag*, RE::BSFixedString a_name)
        {
            return PresetAssignedTo(a_name.c_str());
        }

        // --- Persist (token-less, worn-capture; mirrors box capture) ----------
        std::vector<RE::BSFixedString> GetPersistContents(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& c : PersistContents()) {
                out.emplace_back(c);
            }
            return out;
        }

        // Register a content as persist (the .psc captures the worn item to the
        // holding container first, like a box). Def is synchronous; injection queued.
        bool AddPersistNative(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            const std::string content = a_content.c_str();
            if (!AddPersistContent(content)) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([content] {
                RegisterBoxById(content, {});  // token-less -> always shown
                Reconcile();
                RebuildPersistAbility();  // contents changed -> re-synth persist enchant
                ApplyBoxAbilities();
                SyncPersistManifest();  // persist manifest fragment tracks the ACTIVE set (M2)
            });
            return true;
        }

        // Def-only read (no scene touch) - synchronous is fine, the MCM needs the
        // answer BEFORE it decides to move the physical item (P1-1 capture guard).
        RE::BSFixedString FindContentHolderNative(RE::StaticFunctionTag*,
            RE::BSFixedString a_content)
        {
            return ContentHolder(a_content.c_str());
        }

        // MCM "Reload settings from disk". Queued: the reload rebuilds the
        // registry and touches the scene graph (main thread only).
        void ReloadSettingsNative(RE::StaticFunctionTag*)
        {
            SKSE::GetTaskInterface()->AddTask([] { ReloadSettingsFromDisk(); });
        }

        bool RemovePersistNative(RE::StaticFunctionTag*, RE::BSFixedString a_content, bool a_returnStored)
        {
            const std::string content = a_content.c_str();
            if (!RemovePersistContent(content)) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([content, a_returnStored] {
                if (a_returnStored) {
                    // Persist is a SHARED catalog - store-only (no fabricate) so an
                    // external catalog-remove can't mint copies of an item still
                    // active on another save.
                    ReturnStoredItem(content, false);
                }
                DetachSkinned(content);
                Reconcile();
                RebuildPersistAbility();
                ApplyBoxAbilities();
                SyncPersistManifest();  // persist manifest fragment tracks the ACTIVE set (M2)
            });
            return true;
        }

        // Hand the MCM's hidden holding container to the native layer (ROOT A) so
        // console / external-mod removal borders can return captured items. Called
        // from the MCM's OnConfigOpen; cleared on load until the MCM re-sets it.
        void SetStoreRefNative(RE::StaticFunctionTag*, RE::TESObjectREFR* a_store)
        {
            SetStoreRef(a_store);
        }

        // --- C-phase MCM support (Diagnostics / body morph / M2 activation) ----

        std::vector<RE::BSFixedString> GetDiagLinesNative(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& l : DiagLines()) {
                out.emplace_back(l);
            }
            return out;
        }

        bool GetBodyMorphNative(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            return BodyMorphOn(a_id.c_str());
        }

        void SetBodyMorphNative(RE::StaticFunctionTag*, RE::BSFixedString a_id, bool a_on)
        {
            const std::string id = a_id.c_str();
            SKSE::GetTaskInterface()->AddTask([id, a_on] {
                SetBodyMorphOn(id, a_on);   // def + json
                HideInjectedNodes(id);      // drop the node so Reconcile re-injects
                Reconcile();                // with the new decision
            });
        }

        std::vector<RE::BSFixedString> GetPersistActiveNative(RE::StaticFunctionTag*)
        {
            std::vector<RE::BSFixedString> out;
            for (const auto& id : PersistActiveIds()) {
                out.emplace_back(id);
            }
            return out;
        }

        bool IsPersistActiveNative(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            const std::string id = a_id.c_str();
            for (const auto& it : ActiveSnapshot()) {
                if (it.tokenId.empty() && it.id == id) {
                    return true;
                }
            }
            return false;
        }

        bool SetPersistActiveNative(RE::StaticFunctionTag*, RE::BSFixedString a_id, bool a_on)
        {
            const std::string id = a_id.c_str();
            // Pre-validate on the VM thread so the MCM toggle gets a truthful
            // return; the scene mutation runs on the main thread like every
            // other mutator (PersistSetActive re-checks there).
            bool active = false;
            for (const auto& it : ActiveSnapshot()) {
                if (it.tokenId.empty() && it.id == id) {
                    active = true;
                    break;
                }
            }
            if (a_on == active) {
                return true;  // already in the requested state
            }
            if (a_on) {
                bool inCatalog = false;
                for (const auto& c : PersistContents()) {
                    if (c == id) {
                        inCatalog = true;
                        break;
                    }
                }
                if (!inCatalog) {
                    return false;
                }
            }
            SKSE::GetTaskInterface()->AddTask([id, a_on] { PersistSetActive(id, a_on); });
            return true;
        }

        // --- Hide-when-worn (§8.10) -------------------------------------------
        // Slots are a space-separated list of vanilla biped slot numbers (e.g.
        // "30 31 42"); the content is hidden while one of those slots is held by
        // non-CEF gear. Works for box contents and persist items (keyed by id).

        RE::BSFixedString GetHideSlots(RE::StaticFunctionTag*, RE::BSFixedString a_id)
        {
            std::string out;
            for (const int s : HideSlotsFor(a_id.c_str())) {
                if (!out.empty()) {
                    out += ' ';
                }
                out += std::to_string(s);
            }
            return out;
        }

        bool SetHideSlotsNative(RE::StaticFunctionTag*, RE::BSFixedString a_id, RE::BSFixedString a_slots)
        {
            const std::string id = a_id.c_str();
            if (id.empty()) {
                return false;
            }
            std::vector<int> slots;
            std::istringstream ss(a_slots.c_str());
            int v;
            while (ss >> v) {
                slots.push_back(v);
            }
            if (!SetHideSlots(id, slots)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([] { Reconcile(); });  // apply now
            return true;
        }

        // --- Forced-gender NIF mode (per content) -----------------------------
        // 0 = follow player, 1 = force Male, 2 = force Female. The MCM asks at
        // the MCM per-content "Body" menu (v1.2.1; no longer asked at capture).

        std::int32_t GetContentGender(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            return GenderModeFor(a_content.c_str());
        }

        bool SetContentGenderNative(RE::StaticFunctionTag*, RE::BSFixedString a_content,
            std::int32_t a_mode)
        {
            const std::string content = a_content.c_str();
            if (!SetGenderMode(content, a_mode)) {  // sync def + json
                return false;
            }
            // Re-resolve + re-inject this content for the new effective sex. If it
            // isn't registered yet (set just before AddBox/AddPersist), this is a
            // no-op and the subsequent register picks up the new gender.
            SKSE::GetTaskInterface()->AddTask([content] { RefreshGender(content); });
            return true;
        }

        // --- Persist preset (mirror of the box preset flow) -------------------

        RE::BSFixedString GetPersistPresetNative(RE::StaticFunctionTag*)
        {
            return PersistPreset();
        }

        RE::BSFixedString ExportPersistNative(RE::StaticFunctionTag*, RE::BSFixedString a_name)
        {
            const auto contents = PersistContents();
            std::unordered_map<std::string, std::vector<int>> hideRules;
            std::unordered_map<std::string, int> genderModes;
            CollectPresetMaps(contents, hideRules, genderModes);
            return Preset::Export(a_name.c_str(), contents, hideRules, genderModes);
        }

        bool AssignPersistPresetNative(RE::StaticFunctionTag*, RE::BSFixedString a_file)
        {
            auto preset = Preset::Read(a_file.c_str());
            if (!preset.valid) {
                return false;
            }
            std::vector<std::string> ok, missing;
            Preset::Validate(preset.contents, ok, missing);
            if (!missing.empty()) {
                SKSE::log::warn("preset: '{}' has {} unresolved content (skipped)",
                    preset.name, missing.size());
            }
            const auto oldContents = PersistContents();  // before reassign
            if (!AssignPresetToPersist(preset.name, ok)) {  // sync def + json (exclusivity)
                return false;
            }
            RestorePresetMaps(preset, ok);
            SKSE::GetTaskInterface()->AddTask([oldContents, ok] {
                for (const auto& c : oldContents) {
                    DetachSkinned(c);  // unregister old persist contents
                }
                for (const auto& c : ok) {
                    RegisterBoxById(c, {});  // token-less -> always shown
                }
                Reconcile();
                RebuildPersistAbility();  // new contents -> re-synth persist enchant
                ApplyBoxAbilities();
                SyncPersistManifest();  // persist manifest fragment tracks the ACTIVE set (M2)
            });
            return true;
        }

        // Snapshot the currently-worn item's effective enchantment (base OR
        // player/instance) for a content, so the synthesized box/persist ability
        // includes it. Must be called while the item is still equipped (the .psc
        // calls it at capture time, before moving the item to the store).
        bool CaptureContentEnchantNative(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            return CaptureEnchant(a_content.c_str());  // sync def + json (inventory read)
        }

        // True if the content's base form has a REAL Papyrus script attached
        // (VMAD user script). A bare attachedScripts existence check is wrong:
        // merely passing a form through a Papyrus variable binds a plain
        // wrapper object of the form's NATIVE class ("Armor"/"Form") to the
        // same VM handle - and the MCM capture flow itself does that via
        // ResolveForm(), so EVERY capture warned (2026-07-07, all captured
        // contents verified VMAD-less). Count only script classes that are not
        // native wrappers. Best-effort (base-form scripts; a plain heads-up).
        bool ContentHasScriptNative(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            const std::uint32_t formId = ResolveFormId(a_content.c_str());
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            if (!form) {
                return false;
            }
            auto* svm = RE::SkyrimVM::GetSingleton();
            auto* ivm = svm ? svm->impl.get() : nullptr;
            if (!ivm) {
                return false;
            }
            auto* vm = static_cast<RE::BSScript::Internal::VirtualMachine*>(ivm);
            auto* policy = vm->GetObjectHandlePolicy1();
            if (!policy) {
                return false;
            }
            const RE::VMHandle handle = policy->GetHandleForObject(form->GetFormType(), form);
            if (handle == policy->EmptyHandle()) {
                return false;
            }
            RE::BSSpinLockGuard lock(vm->attachedScriptsLock);
            const auto it = vm->attachedScripts.find(handle);
            if (it == vm->attachedScripts.end()) {
                return false;
            }
            for (auto& attached : it->second) {
                auto* obj = attached.get();
                auto* info = obj ? obj->GetTypeInfo() : nullptr;
                const char* name = info ? info->GetName() : nullptr;
                if (!name || !*name) {
                    continue;
                }
                if (::_stricmp(name, "Armor") != 0 && ::_stricmp(name, "Form") != 0) {
                    SKSE::log::info("capture: '{}' carries attached script '{}'",
                        a_content.c_str(), name);
                    return true;
                }
            }
            return false;
        }

        bool ClearPersistPresetNative(RE::StaticFunctionTag*)
        {
            return ClearPersistPreset();  // contents remain (manual)
        }
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        a_vm->RegisterFunction("GetActive", kClass, GetActive);
        a_vm->RegisterFunction("IsActive", kClass, IsActive);
        a_vm->RegisterFunction("ResolveForm", kClass, ResolveForm);
        a_vm->RegisterFunction("RegisterPersist", kClass, RegisterPersist);
        a_vm->RegisterFunction("DefineBox", kClass, DefineBoxNative);
        a_vm->RegisterFunction("Detach", kClass, Detach);
        a_vm->RegisterFunction("Clear", kClass, Clear);

        a_vm->RegisterFunction("GetBoxCount", kClass, GetBoxCount);
        a_vm->RegisterFunction("GetBoxLabel", kClass, GetBoxLabel);
        a_vm->RegisterFunction("GetBoxToken", kClass, GetBoxToken);
        a_vm->RegisterFunction("GetBoxContents", kClass, GetBoxContents);
        a_vm->RegisterFunction("IsBoxWorn", kClass, IsBoxWorn);
        a_vm->RegisterFunction("GetBoxTokenForm", kClass, GetBoxTokenForm);
        a_vm->RegisterFunction("AddBox", kClass, AddBoxNative);
        a_vm->RegisterFunction("RemoveBoxContent", kClass, RemoveBoxContentNative);
        a_vm->RegisterFunction("RemoveBox", kClass, RemoveBoxNative);
        a_vm->RegisterFunction("SetBoxLabel", kClass, SetBoxLabelNative);

        a_vm->RegisterFunction("GetWornItemNames", kClass, GetWornItemNames);
        a_vm->RegisterFunction("GetWornItemIds", kClass, GetWornItemIds);
        a_vm->RegisterFunction("GetInventoryItemNames", kClass, GetInventoryItemNames);
        a_vm->RegisterFunction("GetInventoryItemIds", kClass, GetInventoryItemIds);
        a_vm->RegisterFunction("CanResolveContent", kClass, CanResolveContentNative);
        a_vm->RegisterFunction("NewBox", kClass, NewBoxNative);
        a_vm->RegisterFunction("GetItemName", kClass, GetItemName);

        a_vm->RegisterFunction("GetFreeTokenIds", kClass, GetFreeTokenIds);
        a_vm->RegisterFunction("GetTokenSlot", kClass, GetTokenSlot);
        a_vm->RegisterFunction("GetBoxBySlot", kClass, GetBoxBySlot);
        a_vm->RegisterFunction("GetBoxEnabled", kClass, GetBoxEnabled);
        a_vm->RegisterFunction("SetBoxEnabled", kClass, SetBoxEnabledNative);
        a_vm->RegisterFunction("GetBoxArmorType", kClass, GetBoxArmorType);
        a_vm->RegisterFunction("SetBoxArmorType", kClass, SetBoxArmorTypeNative);

        a_vm->RegisterFunction("GetAbilityNames", kClass, GetAbilityNames);
        a_vm->RegisterFunction("GetAbilityIds", kClass, GetAbilityIds);
        a_vm->RegisterFunction("GetBoxAbility", kClass, GetBoxAbility);
        a_vm->RegisterFunction("SetBoxAbility", kClass, SetBoxAbilityNative);
        a_vm->RegisterFunction("GetBoxStats", kClass, GetBoxStats);

        a_vm->RegisterFunction("IsEnabled", kClass, IsEnabled);
        a_vm->RegisterFunction("SetEnabled", kClass, SetEnabled);
        a_vm->RegisterFunction("IsBodyMorphReady", kClass, IsBodyMorphReady);
        a_vm->RegisterFunction("GetPresetNames", kClass, GetPresetNames);
        a_vm->RegisterFunction("GetPresetFiles", kClass, GetPresetFiles);
        a_vm->RegisterFunction("ExportPreset", kClass, ExportPresetNative);
        a_vm->RegisterFunction("AssignPreset", kClass, AssignPresetNative);
        a_vm->RegisterFunction("ClearPreset", kClass, ClearPresetNative);
        a_vm->RegisterFunction("GetBoxPreset", kClass, GetBoxPreset);
        a_vm->RegisterFunction("GetPresetAssignedTo", kClass, GetPresetAssignedTo);

        a_vm->RegisterFunction("GetPersistContents", kClass, GetPersistContents);
        a_vm->RegisterFunction("AddPersist", kClass, AddPersistNative);
        a_vm->RegisterFunction("RemovePersist", kClass, RemovePersistNative);
        a_vm->RegisterFunction("SetStoreRef", kClass, SetStoreRefNative);
        a_vm->RegisterFunction("FindContentHolder", kClass, FindContentHolderNative);
        a_vm->RegisterFunction("ReloadSettings", kClass, ReloadSettingsNative);
        a_vm->RegisterFunction("GetDiagLines", kClass, GetDiagLinesNative);
        a_vm->RegisterFunction("GetBodyMorph", kClass, GetBodyMorphNative);
        a_vm->RegisterFunction("SetBodyMorph", kClass, SetBodyMorphNative);
        a_vm->RegisterFunction("GetPersistActive", kClass, GetPersistActiveNative);
        a_vm->RegisterFunction("IsPersistActive", kClass, IsPersistActiveNative);
        a_vm->RegisterFunction("SetPersistActive", kClass, SetPersistActiveNative);

        a_vm->RegisterFunction("GetHideSlots", kClass, GetHideSlots);
        a_vm->RegisterFunction("SetHideSlots", kClass, SetHideSlotsNative);

        a_vm->RegisterFunction("GetContentGender", kClass, GetContentGender);
        a_vm->RegisterFunction("SetContentGender", kClass, SetContentGenderNative);

        a_vm->RegisterFunction("GetPersistPreset", kClass, GetPersistPresetNative);
        a_vm->RegisterFunction("ExportPersist", kClass, ExportPersistNative);
        a_vm->RegisterFunction("AssignPersistPreset", kClass, AssignPersistPresetNative);
        a_vm->RegisterFunction("ClearPersistPreset", kClass, ClearPersistPresetNative);
        a_vm->RegisterFunction("CaptureContentEnchant", kClass, CaptureContentEnchantNative);
        a_vm->RegisterFunction("ContentHasScript", kClass, ContentHasScriptNative);

        SKSE::log::info("Papyrus natives registered ({})", kClass);
        return true;
    }
}
