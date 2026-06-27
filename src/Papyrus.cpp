#include "Papyrus.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "Preset.h"
#include "SkinRebind.h"

#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"

#include <cstdint>
#include <string>
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

        bool RemoveBoxContentNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_content)
        {
            const std::string token = a_token.c_str();
            const std::string content = a_content.c_str();
            if (!RemoveBoxContent(token, content)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([token, content] {
                DetachSkinned(content);
                RebuildBoxAbility(token);
                ApplyBoxAbilities();
                RefreshWornToken(token);  // re-fire equip so keyword detectors re-scan
            });
            return true;
        }

        bool RemoveBoxNative(RE::StaticFunctionTag*, RE::BSFixedString a_token)
        {
            const std::string token = a_token.c_str();
            const std::vector<std::string> contents = BoxContents(token);  // capture first
            const std::string ability = BoxAbility(token);
            if (!RemoveBox(token)) {  // sync def + json
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([token, contents, ability] {
                for (const auto& c : contents) {
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
        RE::BSFixedString ExportPresetNative(RE::StaticFunctionTag*, RE::BSFixedString a_token,
            RE::BSFixedString a_name)
        {
            const auto contents = BoxContents(a_token.c_str());
            return Preset::Export(a_name.c_str(), contents);
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
            });
            return true;
        }

        bool RemovePersistNative(RE::StaticFunctionTag*, RE::BSFixedString a_content)
        {
            const std::string content = a_content.c_str();
            if (!RemovePersistContent(content)) {
                return false;
            }
            SKSE::GetTaskInterface()->AddTask([content] {
                DetachSkinned(content);
                Reconcile();
            });
            return true;
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
        a_vm->RegisterFunction("NewBox", kClass, NewBoxNative);
        a_vm->RegisterFunction("GetItemName", kClass, GetItemName);

        a_vm->RegisterFunction("GetFreeTokenIds", kClass, GetFreeTokenIds);
        a_vm->RegisterFunction("GetTokenSlot", kClass, GetTokenSlot);
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

        SKSE::log::info("Papyrus natives registered ({})", kClass);
        return true;
    }
}
