#include "BoxStore.h"
#include "SkinRebind.h"

#include "RE/A/ActorEquipManager.h"
#include "RE/B/BGSBipedObjectForm.h"
#include "RE/B/BGSKeyword.h"
#include "RE/C/ConcreteFormFactory.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/I/IFormFactory.h"
#include "RE/I/InventoryEntryData.h"
#include "RE/M/MagicSystem.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/S/SpellItem.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"
#include "RE/T/TESObjectARMO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace CostumeFW
{
    namespace
    {
        // Resolved through MO2's VFS as Data\SKSE\Plugins\...; runtime writes land
        // in the MO2 overwrite folder and read back through the same path.
        constexpr const char* kSettingsPath = "Data\\SKSE\\Plugins\\CEF_settings.json";
        constexpr const char* kOldBoxesPath = "Data\\SKSE\\Plugins\\costume_boxes.json";  // migrated
        constexpr const char* kSchema = "cef.settings/1";

        // GLOBAL box definitions (config; all saves). One box per token. Mutated
        // only on the main thread (loads + queued native tasks); MCM queries read
        // it from the VM thread, tolerated like the rest of the registry.
        std::vector<BoxDefInfo> g_boxes;

        // Master CEF on/off (Main page). Persisted in CEF_settings.json.
        bool g_cefEnabled = true;

        // Persist content (colon-form ARMA ids): always-injected, token-less. Phase 1
        // stores + injects these; worn-capture UI and seed retirement come in the MCM
        // rework (the capture flow mirrors a box but with no token gate).
        std::vector<std::string> g_persist;

        int FindBox(const std::string& a_token)
        {
            for (std::size_t i = 0; i < g_boxes.size(); ++i) {
                if (g_boxes[i].token == a_token) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        void WriteJson()
        {
            nlohmann::json doc;
            doc["schema"] = kSchema;
            doc["enabled"] = g_cefEnabled;
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                nlohmann::json jb;
                jb["label"] = b.label;
                jb["token"] = b.token;
                jb["contents"] = b.contents;
                jb["ability"] = b.ability;
                jb["enabled"] = b.enabled;
                jb["armorType"] = b.armorType;
                jb["preset"] = b.preset;
                jb["uiVisible"] = b.uiVisible;
                jb["wear"] = b.wear;
                arr.push_back(std::move(jb));
            }
            doc["boxes"] = std::move(arr);
            doc["persist"]["contents"] = g_persist;

            std::ofstream f(kSettingsPath, std::ios::trunc);
            if (!f) {
                SKSE::log::error("settings: cannot write {}", kSettingsPath);
                return;
            }
            f << doc.dump(2);
            SKSE::log::info("settings: wrote {} box def(s) (enabled={})", g_boxes.size(), g_cefEnabled);
        }

        // Plugin that ships the box-token pool (Costume Box 1..N ARMOs).
        constexpr std::string_view kTokenPlugin = "CostumeFW_Boxes.esp";

        // Build the project's colon-form id "XXXXXX:Plugin.esp" from a form: its
        // plugin-local FormID (ESL-masked) + defining plugin filename.
        std::string MakeColonId(RE::TESForm* a_form)
        {
            char buf[8]{};
            std::snprintf(buf, sizeof(buf), "%06X", a_form->GetLocalFormID());
            auto* file = a_form->GetFile(0);
            const std::string plugin = file ? std::string(file->GetFilename()) : std::string{};
            return std::string(buf) + ":" + plugin;
        }

        // The biped slot number (30-61) of a single-slot ARMO (the lowest set
        // bit + 30), 0 if none. Used to order/identify the slot-tokens.
        int SlotNumberOf(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) {
                return 0;
            }
            const std::uint32_t mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            for (int bit = 0; bit < 32; ++bit) {
                if (mask & (1u << bit)) {
                    return bit + 30;
                }
            }
            return 0;
        }

        RE::TESObjectARMO* ResolveArmo(const std::string& a_colonId);  // fwd (defined below)
        void SetTokenStats(const BoxDefInfo& a_box);                   // fwd (defined below)
        void ResetTokenStats(const std::string& a_token);              // fwd (defined below)
    }

    void LoadBoxes()
    {
        g_boxes.clear();
        g_persist.clear();
        g_cefEnabled = true;

        // Read CEF_settings.json; fall back to the legacy costume_boxes.json once
        // (migration) and rewrite into the new file at the end of load.
        bool migrated = false;
        std::ifstream f(kSettingsPath);
        if (!f) {
            std::ifstream old(kOldBoxesPath);
            if (!old) {
                SKSE::log::info("settings: no {} (no settings yet)", kSettingsPath);
                return;
            }
            SKSE::log::info("settings: migrating legacy {} -> {}", kOldBoxesPath, kSettingsPath);
            f.swap(old);
            migrated = true;
        }
        nlohmann::json doc;
        try {
            f >> doc;
        } catch (const std::exception& e) {
            SKSE::log::error("settings: JSON parse error: {}", e.what());
            return;
        }

        g_cefEnabled = doc.value("enabled", true);
        const auto boxes = doc.value("boxes", nlohmann::json::array());
        for (const auto& jb : boxes) {
            BoxDefInfo b;
            b.label = jb.value("label", std::string{});
            b.token = jb.value("token", std::string{});
            if (b.token.empty()) {
                continue;
            }
            for (const auto& c : jb.value("contents", nlohmann::json::array())) {
                if (c.is_string()) {
                    b.contents.push_back(c.get<std::string>());
                }
            }
            b.ability = jb.value("ability", std::string{});
            b.enabled = jb.value("enabled", true);
            b.armorType = jb.value("armorType", 0);
            b.preset = jb.value("preset", std::string{});
            b.uiVisible = jb.value("uiVisible", true);
            b.wear = jb.value("wear", false);
            g_boxes.push_back(std::move(b));
        }
        const auto persist = doc.value("persist", nlohmann::json::object());
        for (const auto& c : persist.value("contents", nlohmann::json::array())) {
            if (c.is_string()) {
                g_persist.push_back(c.get<std::string>());
            }
        }
        if (migrated) {
            WriteJson();  // persist into CEF_settings.json (legacy file left as-is)
        }

        // Register every box content for worn-gated injection (no inject yet) and
        // write each box's aggregate armor/weight onto its token.
        int contentCount = 0;
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                if (RegisterBoxById(c, b.token)) {
                    ++contentCount;
                }
            }
            SetTokenStats(b);
        }
        // Persist content: register token-less (always-injected).
        for (const auto& c : g_persist) {
            RegisterBoxById(c, {});  // empty token -> tokenForm 0 -> always show
        }
        SKSE::log::info("settings: loaded {} box(es) ({} content), {} persist, enabled={}",
            g_boxes.size(), contentCount, g_persist.size(), g_cefEnabled);
    }

    bool CefEnabled()
    {
        return g_cefEnabled;
    }

    bool SetCefEnabled(bool a_on)
    {
        g_cefEnabled = a_on;
        WriteJson();
        return g_cefEnabled;
    }

    bool IsBoxToken(std::uint32_t a_form)
    {
        if (a_form == 0) {
            return false;
        }
        for (const auto& b : g_boxes) {
            if (ResolveFormId(b.token) == a_form) {
                return true;
            }
        }
        return false;
    }

    void ReplenishToken(std::uint32_t a_tokenForm)
    {
        if (a_tokenForm == 0) {
            return;
        }
        // Only replenish a token whose box is ENABLED (distribution on). A disabled
        // box's token is meant to be gone, so don't fight the user removing it.
        bool enabledBox = false;
        for (const auto& b : g_boxes) {
            if (ResolveFormId(b.token) == a_tokenForm) {
                enabledBox = b.enabled;
                break;
            }
        }
        if (!enabledBox) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::LookupByID(a_tokenForm);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!player || !obj) {
            return;
        }
        const auto counts = player->GetInventoryCounts();
        const auto it = counts.find(obj);
        const std::int32_t have = (it != counts.end()) ? it->second : 0;
        if (have <= 0) {
            player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            SKSE::log::info("boxes: replenished lost token {:08X}", a_tokenForm);
        }
    }

    void RefreshWornToken(const std::string& a_token)
    {
        const std::uint32_t formId = ResolveFormId(a_token);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        // Only refresh if the token is actually worn; otherwise the next normal
        // equip already carries the updated keywords.
        if (!player || !obj || player->GetWornArmor(formId) == nullptr) {
            return;
        }
        auto* eqm = RE::ActorEquipManager::GetSingleton();
        if (!eqm) {
            return;
        }
        eqm->UnequipObject(player, obj);
        eqm->EquipObject(player, obj);
        SKSE::log::info("boxes: re-equipped worn token '{}' (keyword refresh)", a_token);
    }

    void ReapplyBoxes()
    {
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                RegisterBoxById(c, b.token);
            }
            SetTokenStats(b);  // token fields revert on load - re-apply
        }
        for (const auto& c : g_persist) {
            RegisterBoxById(c, {});  // token-less persist
        }
        SKSE::log::info("settings: reapplied {} box(es) + {} persist", g_boxes.size(), g_persist.size());
    }

    std::vector<std::string> PersistContents()
    {
        return g_persist;
    }

    bool AddPersistContent(const std::string& a_content)
    {
        if (a_content.empty() ||
            std::find(g_persist.begin(), g_persist.end(), a_content) != g_persist.end()) {
            return false;
        }
        g_persist.push_back(a_content);
        WriteJson();
        return true;
    }

    bool RemovePersistContent(const std::string& a_content)
    {
        const auto it = std::find(g_persist.begin(), g_persist.end(), a_content);
        if (it == g_persist.end()) {
            return false;
        }
        g_persist.erase(it);
        WriteJson();
        return true;
    }

    std::vector<WornItem> WornArmors()
    {
        std::vector<WornItem> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }
        auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
            return a_obj.Is(RE::FormType::Armor);
        });
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (count <= 0 || !entry || !entry->IsWorn()) {
                continue;
            }
            auto* armo = obj->As<RE::TESObjectARMO>();
            if (!armo) {
                continue;
            }
            // Exclude our own box tokens (they live in CostumeFW_Boxes.esp).
            auto* file = armo->GetFile(0);
            if (file && file->GetFilename() == kTokenPlugin) {
                continue;
            }
            const char* nm = armo->GetFullName();
            out.push_back({ (nm && *nm) ? std::string(nm) : MakeColonId(armo), MakeColonId(armo) });
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::vector<std::string> TokenPool()
    {
        std::vector<std::pair<int, std::string>> pairs;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return {};
        }
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo) {
                continue;
            }
            auto* file = armo->GetFile(0);
            if (!file || file->GetFilename() != kTokenPlugin) {
                continue;
            }
            const char* nm = armo->GetFullName();
            if (nm && std::string_view(nm).starts_with("Costume Box")) {
                pairs.emplace_back(SlotNumberOf(armo), MakeColonId(armo));
            }
        }
        std::sort(pairs.begin(), pairs.end());  // by slot number
        std::vector<std::string> out;
        out.reserve(pairs.size());
        for (auto& p : pairs) {
            out.push_back(std::move(p.second));
        }
        return out;
    }

    std::vector<std::string> FreeTokens()
    {
        std::vector<std::string> out;
        for (const auto& t : TokenPool()) {
            if (FindBox(t) < 0) {
                out.push_back(t);
            }
        }
        return out;
    }

    std::string NextFreeToken()
    {
        const auto free = FreeTokens();
        return free.empty() ? std::string{} : free.front();
    }

    int TokenSlot(const std::string& a_token)
    {
        return SlotNumberOf(ResolveArmo(a_token));
    }

    bool BoxEnabled(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx >= 0 && g_boxes[idx].enabled;
    }

    bool SetBoxEnabled(const std::string& a_token, bool a_enabled)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].enabled = a_enabled;
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // disabled -> token stats cleared, enabled -> applied
        return true;
    }

    int BoxArmorType(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? 0 : g_boxes[idx].armorType;
    }

    bool SetBoxArmorType(const std::string& a_token, int a_type)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].armorType = (a_type >= 0 && a_type <= 2) ? a_type : 0;
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // re-apply armor class onto the token
        return true;
    }

    bool NewBox(const std::string& a_label)
    {
        const std::string token = NextFreeToken();
        if (token.empty()) {
            SKSE::log::warn("boxes: NewBox - token pool exhausted");
            return false;
        }
        return AddBox(a_label, token, {});  // definition only
    }

    std::string ItemDisplayName(const std::string& a_colonId)
    {
        const std::uint32_t formId = ResolveFormId(a_colonId);
        if (formId == 0) {
            return a_colonId;
        }
        auto* form = RE::TESForm::LookupByID(formId);
        auto* full = form ? form->As<RE::TESFullName>() : nullptr;
        const char* nm = full ? full->GetFullName() : nullptr;
        return (nm && *nm) ? std::string(nm) : a_colonId;
    }

    namespace
    {
        // Resolve a colon-form id to a SpellItem (nullptr on failure).
        RE::SpellItem* ResolveSpell(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            if (formId == 0) {
                return nullptr;
            }
            auto* form = RE::TESForm::LookupByID(formId);
            return form ? form->As<RE::SpellItem>() : nullptr;
        }

        bool TokenWorn(const std::string& a_token)
        {
            const std::uint32_t form = ResolveFormId(a_token);
            if (form == 0) {
                return false;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player && player->GetWornArmor(form) != nullptr;
        }

        // --- Stat passthrough ---
        // Armor + weight are written DIRECTLY onto the token ARMO's own fields
        // (armorRating / weight) - the token IS worn equipment, so the engine
        // applies them naturally (real armor scaling, real carried weight). This
        // is correct where a CarryWeight magic effect was NOT (the AV is max
        // capacity, not the item's own weight). See SetTokenStats below.
        // Enchantment effects are still aggregated into a RUNTIME ability spell
        // (dynamic forms aren't serialized -> rebuilt on load, no stacking).

        // token -> synthesized enchant ability (nullptr = built but none). Process-
        // global; cleared on game load (ClearBoxSpellCache).
        std::unordered_map<std::string, RE::SpellItem*> g_boxSpells;

        void AddEffect(RE::SpellItem* a_spell, RE::EffectSetting* a_mgef, float a_magnitude)
        {
            if (!a_mgef) {
                return;
            }
            auto* eff = new RE::Effect();
            eff->baseEffect = a_mgef;
            eff->effectItem.magnitude = a_magnitude;
            eff->effectItem.area = 0;
            eff->effectItem.duration = 0;
            a_spell->effects.push_back(eff);
        }

        RE::TESObjectARMO* ResolveArmo(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            return form ? form->As<RE::TESObjectARMO>() : nullptr;
        }

        // Build the aggregate ENCHANT ability for a box (its contents' enchantment
        // effects). Returns nullptr if no content is enchanted. Armor + weight are
        // handled separately on the token's own fields (SetTokenStats).
        RE::SpellItem* BuildBoxAbility(const BoxDefInfo& a_box)
        {
            std::vector<RE::Effect*> enchEffects;
            for (const auto& c : a_box.contents) {
                auto* armo = ResolveArmo(c);
                if (!armo || !armo->formEnchanting) {
                    continue;
                }
                for (auto* e : armo->formEnchanting->effects) {
                    if (e && e->baseEffect) {
                        enchEffects.push_back(e);  // copied below
                    }
                }
            }
            if (enchEffects.empty()) {
                return nullptr;
            }

            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
            auto* spell = factory ? factory->Create() : nullptr;
            if (!spell) {
                SKSE::log::error("boxes: SpellItem factory create failed");
                return nullptr;
            }
            spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
            spell->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
            spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
            spell->data.costOverride = 0;
            spell->fullName = "Costume Stats";

            for (auto* e : enchEffects) {
                AddEffect(spell, e->baseEffect, e->effectItem.magnitude);
            }

            SKSE::log::info("boxes: synth enchant ability for '{}' ({} effect(s))",
                a_box.token, enchEffects.size());
            return spell;
        }

        // --- Keyword passthrough -------------------------------------------------
        // Aggregate the contents' keywords onto the worn token so consumer mods
        // (WornHasKeyword detectors, perk conditions, etc.) see them. Keywords live
        // on the token's BASE form and are invisible until the token is worn, so we
        // set them at content-change/load time (before equip) - the natural equip
        // event then carries them to any detector. Structural keywords (armor class
        // / slot / material) are excluded so the invisible token isn't mistaken for
        // real armor in perk/equip logic; armor class is handled via armorType.

        // token colon-id -> keywords WE added (so we can remove ONLY ours on the
        // next apply, preserving keywords KID/other mods distributed to the token).
        // Process-global; base-form keyword arrays reset to plugin+KID state on a
        // process restart, so this never accumulates across sessions.
        std::unordered_map<std::string, std::vector<RE::BGSKeyword*>> g_boxKeywords;

        bool KeywordPassThroughAllowed(std::string_view a_ed)
        {
            for (const auto p : { "OCF_", "ArmorMaterial", "WeapType", "WAF_" }) {
                if (a_ed.starts_with(p)) {
                    return false;
                }
            }
            static const std::unordered_set<std::string_view> kBlocked{
                "ArmorHeavy", "ArmorLight", "ArmorClothing", "ArmorJewelry",
                "ArmorCuirass", "ArmorBoots", "ArmorGauntlets", "ArmorHelmet",
                "ArmorShield", "ArmorBracer", "ArmorCirclet",
                "ClothingBody", "ClothingHead", "ClothingFeet", "ClothingHands",
                "ClothingRing", "ClothingNecklace", "ClothingCirclet",
                "VendorItemJewelry", "VendorItemClothing", "VendorItemArmor"
            };
            return !kBlocked.contains(a_ed);
        }

        // Remove the keywords we previously added to this token (preserving any
        // distributed by other mods). Pass an already-resolved ARMO if available.
        void ClearTokenKeywords(const std::string& a_token, RE::TESObjectARMO* a_token3)
        {
            auto it = g_boxKeywords.find(a_token);
            if (it == g_boxKeywords.end()) {
                return;
            }
            if (a_token3) {
                for (auto* kw : it->second) {
                    a_token3->RemoveKeyword(kw);
                }
            }
            it->second.clear();
        }

        // Re-apply the contents' (filtered) keyword union onto the token, diffing
        // against what we added last time so KID/other keywords survive.
        void SetTokenKeywords(const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_box.token);
            if (!token) {
                return;
            }
            ClearTokenKeywords(a_box.token, token);
            auto& mine = g_boxKeywords[a_box.token];
            if (a_box.enabled) {
                for (const auto& c : a_box.contents) {
                    auto* armo = ResolveArmo(c);
                    if (!armo) {
                        continue;
                    }
                    for (auto* kw : armo->GetKeywords()) {
                        if (!kw) {
                            continue;
                        }
                        const char* ed = kw->formEditorID.c_str();
                        if (!ed || !*ed || !KeywordPassThroughAllowed(ed)) {
                            continue;
                        }
                        // AddKeyword returns true only if newly added; record only
                        // those so we never strip a pre-existing (KID) keyword later.
                        if (token->AddKeyword(kw)) {
                            mine.push_back(kw);
                        }
                    }
                }
            }
            SKSE::log::info("boxes: token '{}' passthrough keywords +{}", a_box.token, mine.size());
        }

        // Write a box's contents' armor + weight DIRECTLY onto its token ARMO's
        // fields, so the worn token provides them through the normal equip system.
        // armorRating is stored as CK-value * 100. Sums 0 -> token fields cleared.
        void SetTokenStats(const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_box.token);
            if (!token) {
                return;
            }
            float armorSum = 0.0f;
            float weightSum = 0.0f;
            if (a_box.enabled) {
                for (const auto& c : a_box.contents) {
                    if (auto* armo = ResolveArmo(c)) {
                        armorSum += armo->GetArmorRating();
                        weightSum += armo->weight;
                    }
                }
            }
            token->armorRating = static_cast<std::uint32_t>(armorSum * 100.0f);
            token->weight = weightSum;

            // Armor class: Clothing ignores armorRating for DR, so a box holding
            // armor must be Light/Heavy. Map our code (0=Cloth/1=Light/2=Heavy) to
            // the engine enum (kLightArmor=0, kHeavyArmor=1, kClothing=2).
            using AT = RE::BIPED_MODEL::ArmorType;
            AT type = AT::kClothing;
            if (a_box.armorType == 1) {
                type = AT::kLightArmor;
            } else if (a_box.armorType == 2) {
                type = AT::kHeavyArmor;
            }
            token->bipedModelData.armorType = type;

            SKSE::log::info("boxes: token '{}' stats armor={} weight={} type={}",
                a_box.token, armorSum, weightSum, a_box.armorType);

            SetTokenKeywords(a_box);  // aggregate contents' keywords onto the token
        }

        // Clear a token ARMO's stat fields (freed box / disabled).
        void ResetTokenStats(const std::string& a_token)
        {
            auto* token = ResolveArmo(a_token);
            if (token) {
                token->armorRating = 0;
                token->weight = 0.0f;
            }
            ClearTokenKeywords(a_token, token);  // strip our passthrough keywords
        }

        // Get (build + cache) the synthesized ability for a token, or nullptr.
        RE::SpellItem* BoxAbilityFor(const BoxDefInfo& a_box)
        {
            auto it = g_boxSpells.find(a_box.token);
            if (it != g_boxSpells.end()) {
                return it->second;
            }
            RE::SpellItem* spell = BuildBoxAbility(a_box);
            g_boxSpells[a_box.token] = spell;
            return spell;
        }
    }

    std::vector<WornItem> AbilityCatalog()
    {
        std::vector<WornItem> out;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return out;
        }
        for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
            if (!spell) {
                continue;
            }
            auto* file = spell->GetFile(0);
            if (!file || file->GetFilename() != kTokenPlugin) {
                continue;
            }
            const char* nm = spell->GetFullName();
            if (nm && std::string_view(nm).starts_with("Costume:")) {
                out.push_back({ std::string(nm), MakeColonId(spell) });
            }
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::string BoxAbility(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].ability;
    }

    bool SetBoxAbility(const std::string& a_token, const std::string& a_ability)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].ability = a_ability;  // caller applies on the main thread
        WriteJson();
        return true;
    }

    void RemoveBoxAbilitySpell(const std::string& a_ability)
    {
        if (a_ability.empty()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell = ResolveSpell(a_ability);
        if (player && spell && player->HasSpell(spell)) {
            player->RemoveSpell(spell);
        }
    }

    namespace
    {
        void SyncSpell(RE::Actor* a_player, RE::SpellItem* a_spell, bool a_worn)
        {
            if (!a_spell) {
                return;
            }
            const bool has = a_player->HasSpell(a_spell);
            if (a_worn && !has) {
                a_player->AddSpell(a_spell);
            } else if (!a_worn && has) {
                a_player->RemoveSpell(a_spell);
            }
        }
    }

    void ApplyBoxAbilities()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        for (const auto& b : g_boxes) {
            const bool worn = TokenWorn(b.token);
            // Synthesized ENCHANT ability (armor/weight are on the token's fields).
            SyncSpell(player, BoxAbilityFor(b), worn);
            // Optional manual extra ability (dormant unless set in json).
            if (!b.ability.empty()) {
                SyncSpell(player, ResolveSpell(b.ability), worn);
            }
        }
    }

    void RebuildBoxAbility(const std::string& a_token)
    {
        auto it = g_boxSpells.find(a_token);
        if (it == g_boxSpells.end()) {
            return;  // not built yet; ApplyBoxAbilities builds it fresh
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (it->second && player && player->HasSpell(it->second)) {
            player->RemoveSpell(it->second);
        }
        g_boxSpells.erase(it);  // next ApplyBoxAbilities rebuilds + reapplies
    }

    void ClearBoxSpellCache()
    {
        // Dynamic ability forms aren't serialized; a save/load drops them from the
        // actor. Just forget our cache so the next apply rebuilds from scratch.
        g_boxSpells.clear();
    }

    std::string BoxStatsSummary(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        const auto& box = g_boxes[a_index];
        float armorSum = 0.0f;
        float weightSum = 0.0f;
        std::vector<std::string> effs;
        for (const auto& c : box.contents) {
            auto* armo = ResolveArmo(c);
            if (!armo) {
                continue;
            }
            armorSum += armo->GetArmorRating();
            weightSum += armo->weight;
            if (auto* ench = armo->formEnchanting) {
                for (auto* e : ench->effects) {
                    if (!e || !e->baseEffect) {
                        continue;
                    }
                    auto* full = e->baseEffect->As<RE::TESFullName>();
                    const char* nm = full ? full->GetFullName() : nullptr;
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf), "%s %.0f",
                        (nm && *nm) ? nm : "effect", e->effectItem.magnitude);
                    effs.push_back(buf);
                }
            }
        }
        std::string out;
        if (armorSum > 0.0f) {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "Armor +%.0f", armorSum);
            out += buf;
        }
        if (weightSum > 0.0f) {
            char buf[40]{};
            std::snprintf(buf, sizeof(buf), "%sWeight %.1f", out.empty() ? "" : " | ", weightSum);
            out += buf;
        }
        for (const auto& e : effs) {
            out += (out.empty() ? "" : " | ");
            out += e;
        }
        return out.empty() ? "(no stats)" : out;
    }

    int BoxCount()
    {
        return static_cast<int>(g_boxes.size());
    }

    BoxDefInfo BoxAt(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        return g_boxes[a_index];
    }

    bool BoxWornAt(int a_index)
    {
        const std::uint32_t form = BoxTokenFormAt(a_index);
        if (form == 0) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && player->GetWornArmor(form) != nullptr;
    }

    std::uint32_t BoxTokenFormAt(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return 0;
        }
        return ResolveFormId(g_boxes[a_index].token);
    }

    std::vector<std::string> BoxContents(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::vector<std::string>{} : g_boxes[idx].contents;
    }

    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content)
    {
        if (a_token.empty()) {
            return false;
        }
        int idx = FindBox(a_token);
        if (idx < 0) {
            g_boxes.push_back({ a_label, a_token, {} });
            idx = static_cast<int>(g_boxes.size()) - 1;
        } else if (!a_label.empty()) {
            g_boxes[idx].label = a_label;
        }

        auto& box = g_boxes[idx];
        if (!a_content.empty() &&
            std::find(box.contents.begin(), box.contents.end(), a_content) == box.contents.end()) {
            box.contents.push_back(a_content);  // caller registers + reconciles
        }

        WriteJson();
        SetTokenStats(g_boxes[idx]);  // write armor/weight onto the token now
        SKSE::log::info("boxes: AddBox label='{}' token='{}' content='{}'", a_label, a_token, a_content);
        return true;
    }

    bool RemoveBoxContent(const std::string& a_token, const std::string& a_content)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        auto& box = g_boxes[idx];
        const auto it = std::find(box.contents.begin(), box.contents.end(), a_content);
        if (it == box.contents.end()) {
            return false;
        }
        box.contents.erase(it);  // caller detaches the node
        WriteJson();
        SetTokenStats(box);  // recompute token armor/weight
        return true;
    }

    bool RemoveBox(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes.erase(g_boxes.begin() + idx);  // caller detaches each content node
        WriteJson();
        ResetTokenStats(a_token);  // freed token: clear its stat fields
        return true;
    }

    bool SetBoxLabel(const std::string& a_token, const std::string& a_label)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].label = a_label;
        WriteJson();
        return true;
    }

    std::string PresetAssignedTo(const std::string& a_presetName)
    {
        if (a_presetName.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            if (b.preset == a_presetName) {
                return b.token;
            }
        }
        return {};
    }

    std::string BoxPreset(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].preset;
    }

    bool AssignPreset(const std::string& a_token, const std::string& a_presetName,
        const std::vector<std::string>& a_contents)
    {
        const int idx = FindBox(a_token);
        if (idx < 0 || a_presetName.empty()) {
            return false;
        }
        // Exclusivity: a preset may be assigned to only one box at a time.
        const std::string holder = PresetAssignedTo(a_presetName);
        if (!holder.empty() && holder != a_token) {
            SKSE::log::warn("preset: '{}' already assigned to box {}", a_presetName, holder);
            return false;
        }
        g_boxes[idx].preset = a_presetName;
        g_boxes[idx].contents = a_contents;  // box mirrors the preset's contents
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // recompute token armor/weight/keywords for new contents
        SKSE::log::info("preset: assigned '{}' to box {} ({} content)",
            a_presetName, a_token, a_contents.size());
        return true;
    }

    bool ClearPreset(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].preset.clear();  // contents remain (now manual)
        WriteJson();
        return true;
    }
}
