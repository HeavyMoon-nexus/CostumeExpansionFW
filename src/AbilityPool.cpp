#include "AbilityPool.h"

#include "AtomicWrite.h"
#include "AvDiag.h"  // AvConsoleName - the legacy report must name actor values
#include "SkinRebind.h"  // ActiveSnapshot - what is worn RIGHT NOW
#include "ConsoleOut.h"
#include "FormId.h"

#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/A/ActorValues.h"
#include "RE/M/Misc.h"  // DebugNotification
#include "RE/S/SpellItem.h"
#include "RE/T/TESObjectARMO.h"
#include "RE/T/TESCondition.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CostumeFW::abilities
{
    namespace
    {
        void Print(std::string_view a_msg) { ConsolePrint(a_msg); }

        constexpr const char* kPoolPlugin = "CostumeFW_Abilities.esp";
        constexpr std::uint32_t kPoolBase = 0x800;
        constexpr int kPoolSize = 1024;  // must match tools/make_ability_pool.py

        constexpr const char* kRegistryPath = "Data\\SKSE\\Plugins\\CEF_abilities.json";
        constexpr int kSchema = 1;

        // ---------------------------------------------------------------
        // Recipe
        // ---------------------------------------------------------------

        // A condition parameter is a void* that the engine reads as a small
        // integer for some functions and as a TESForm* for others - about four
        // in ten of the conditional wearable enchantments in a heavy load order
        // use the form kind (WornHasKeyword, GetEquipped, IsSpellTarget,
        // GetGlobalValue...). A pointer is a different address next launch, so
        // storing the raw bits would restore a condition pointing at whatever
        // happens to live there. BoxStore's SameConditionData compares these as
        // raw bits and says so; that is right for COMPARING and wrong for
        // saving, which is why none of this reuses it.
        struct RecipeParam
        {
            bool isForm{ false };
            std::string form;          // colon id when isForm
            std::uint64_t raw{ 0 };    // the bits otherwise
        };

        struct RecipeCondition
        {
            std::uint16_t function{ 0 };
            std::uint8_t flags{ 0 };
            std::uint8_t object{ 0 };
            std::uint32_t dataID{ 0 };
            bool compareIsGlobal{ false };
            float compareFloat{ 0.0f };
            std::string compareGlobal;
            RecipeParam params[2];
        };

        struct RecipeEffect
        {
            std::string mgef;
            float magnitude{ 0.0f };
            std::uint32_t area{ 0 };
            std::uint32_t duration{ 0 };
            float cost{ 0.0f };
            std::vector<RecipeCondition> conditions;
        };

        struct Entry
        {
            std::string content;              // the content this was built for
            std::uint32_t generation{ 0 };    // a changed source gets a NEW slot
            bool tombstone{ false };
            bool invalid{ false };            // hydration failed; grant nothing
            std::vector<RecipeEffect> effects;
        };

        std::vector<std::optional<Entry>> g_slots(kPoolSize);
        std::unordered_map<std::string, int> g_byContent;  // content -> live slot
        State g_state = State::Uninitialized;
        std::string g_why;
        std::string g_legacyReport;  // set when a pre-1.6.3 save is loaded

        // A content id names an armor; the enchantment is whatever that armor
        // carries. The old mechanism preferred a captured snapshot or the worn
        // instance's enchantment over the base form's, so this is an
        // approximation - which is why the legacy report says "estimates" and
        // sends the reader to `cef av` for the real numbers.
        const RE::EnchantmentItem* EnchantmentOf(const std::string& a_contentId);

        void Disable(std::string a_why)
        {
            g_state = State::DisabledSafe;
            g_why = std::move(a_why);
            SKSE::log::error("abilities: DISABLED - {}", g_why);
        }

        // ---------------------------------------------------------------
        // Forms
        // ---------------------------------------------------------------

        RE::TESForm* LookupColon(const std::string& a_id)
        {
            const auto colon = a_id.find(':');
            if (colon == std::string::npos) {
                return nullptr;
            }
            std::uint32_t local = 0;
            try {
                local = static_cast<std::uint32_t>(std::stoul(a_id.substr(0, colon), nullptr, 16));
            } catch (...) {
                return nullptr;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm(local, a_id.substr(colon + 1)) : nullptr;
        }

        RE::SpellItem* PoolSpell(int a_slot)
        {
            if (a_slot < 0 || a_slot >= kPoolSize) {
                return nullptr;
            }
            auto* form = LookupColon(std::format(
                "{:06X}:{}", kPoolBase + static_cast<std::uint32_t>(a_slot), kPoolPlugin));
            return form ? form->As<RE::SpellItem>() : nullptr;
        }

        // Turn condition parameters that are really form pointers into colon
        // ids, in ONE pass over the form table per recipe.
        //
        // Two things this deliberately does NOT do. It does not build a
        // persistent pointer->id index: a heavy load order holds millions of
        // forms and that map would cost more memory than the whole plugin, and
        // memory balloons have been misattributed to CEF enough times already.
        // And it never DEREFERENCES a candidate - the pass compares addresses
        // by value, so a parameter that was a small integer all along is simply
        // never matched. Dereferencing to ask "are you a form?" is how you
        // crash on the parameter that was the number 2.
        std::unordered_map<std::uint64_t, std::string> ResolveParamForms(
            const std::vector<std::uint64_t>& a_candidates)
        {
            std::unordered_map<std::uint64_t, std::string> out;
            if (a_candidates.empty()) {
                return out;
            }
            const auto& [map, lock] = RE::TESForm::GetAllForms();
            [[maybe_unused]] const RE::BSReadWriteLock l{ lock };
            if (!map) {
                return out;
            }
            for (const auto& [id, form] : *map) {
                if (!form) {
                    continue;
                }
                const auto addr = reinterpret_cast<std::uint64_t>(form);
                if (std::find(a_candidates.begin(), a_candidates.end(), addr) !=
                    a_candidates.end()) {
                    out.emplace(addr, MakeColonId(form));
                }
            }
            return out;
        }

        // The condition functions whose parameters include a STRING - a Papyrus
        // variable name - rather than a form or a number. Their addresses change
        // between launches like any other pointer, and unlike a form there is
        // nothing to look the value back up by.
        //
        // CEF refuses these rather than reaching for a way to store the string,
        // and that is a decision, not a gap. A condition like this gates an
        // effect on another mod's SCRIPT STATE: measured on Devious Devices'
        // zadx_EnchSlowBoots, whose third condition reads a quest variable in
        // DD's own quest. Copying that into a costume box would produce an
        // effect switching on and off according to a variable with nothing to do
        // with the box. Named here so the refusal can say what it means instead
        // of printing a number at someone.
        constexpr const char* ScriptStateFunction(std::uint16_t a_fn)
        {
            switch (a_fn) {
            case 53: return "GetScriptVariable";
            case 79: return "GetQuestVariable";
            case 629: return "GetVMQuestVariable";
            case 630: return "GetVMScriptVariable";
            default: return nullptr;
            }
        }

        // Above the null page and pointer-aligned. Every integer parameter the
        // condition functions actually use is tiny (an actor value index, an
        // equipped-item-type enum, 0/1/2), so this only ever nominates real
        // addresses - and nominating a wrong one costs nothing, because the
        // pass above simply will not match it.
        constexpr bool LooksLikePointer(std::uint64_t a_value)
        {
            return a_value >= 0x10000 && (a_value & 7) == 0;
        }

        // ---------------------------------------------------------------
        // Building a recipe
        // ---------------------------------------------------------------

        // No partial recipes (design 5.7): one field that cannot be written
        // faithfully fails the whole thing, and the caller grants nothing. A
        // half-restored condition is worse than no effect, because the player
        // gets a bonus under rules nobody chose.
        bool BuildRecipe(const RE::EnchantmentItem* a_ench, std::vector<RecipeEffect>& a_out,
            std::string& a_why)
        {
            a_out.clear();
            if (!a_ench) {
                a_why = "no enchantment";
                return false;
            }

            // Collect every parameter that might be a form, across the whole
            // enchantment, so the form table is walked once rather than per
            // condition.
            std::vector<std::uint64_t> candidates;
            for (const auto* e : a_ench->effects) {
                if (!e) {
                    continue;
                }
                for (const auto* c = e->conditions.head; c; c = c->next) {
                    for (int i = 0; i < 2; ++i) {
                        const auto raw = reinterpret_cast<std::uint64_t>(c->data.functionData.params[i]);
                        if (LooksLikePointer(raw)) {
                            candidates.push_back(raw);
                        }
                    }
                    if (c->data.flags.global) {
                        const auto raw = reinterpret_cast<std::uint64_t>(c->data.comparisonValue.g);
                        if (LooksLikePointer(raw)) {
                            candidates.push_back(raw);
                        }
                    }
                }
            }
            const auto resolved = ResolveParamForms(candidates);

            for (const auto* e : a_ench->effects) {
                if (!e || !e->baseEffect) {
                    continue;
                }
                RecipeEffect re;
                re.mgef = MakeColonId(e->baseEffect);
                if (re.mgef.find(':') == std::string::npos || re.mgef.back() == ':') {
                    a_why = std::format("magic effect {} has no defining plugin", re.mgef);
                    return false;
                }
                re.magnitude = e->effectItem.magnitude;
                re.area = e->effectItem.area;
                re.duration = e->effectItem.duration;
                re.cost = e->cost;

                for (const auto* c = e->conditions.head; c; c = c->next) {
                    // runOnRef is an ObjectRefHandle - a runtime handle into the
                    // reference table, with no stable identity to write down. A
                    // condition that uses one cannot be rebuilt, so the recipe
                    // is refused rather than rebuilt without it: dropping the
                    // "run on" target silently re-points the condition at the
                    // subject, which is a different rule.
                    if (c->data.runOnRef) {
                        a_why = "a condition runs on a specific reference, which cannot be saved";
                        return false;
                    }
                    RecipeCondition rc;
                    rc.function = static_cast<std::uint16_t>(c->data.functionData.function.get());
                    rc.flags = *reinterpret_cast<const std::uint8_t*>(&c->data.flags);
                    rc.object = static_cast<std::uint8_t>(c->data.object.get());
                    rc.dataID = c->data.dataID;

                    rc.compareIsGlobal = c->data.flags.global;
                    if (rc.compareIsGlobal) {
                        const auto raw =
                            reinterpret_cast<std::uint64_t>(c->data.comparisonValue.g);
                        const auto it = resolved.find(raw);
                        if (it == resolved.end()) {
                            a_why = "a condition compares against a global that cannot be identified";
                            return false;
                        }
                        rc.compareGlobal = it->second;
                    } else {
                        rc.compareFloat = c->data.comparisonValue.f;
                    }

                    for (int i = 0; i < 2; ++i) {
                        const auto raw =
                            reinterpret_cast<std::uint64_t>(c->data.functionData.params[i]);
                        const auto it = resolved.find(raw);
                        if (it != resolved.end()) {
                            rc.params[i].isForm = true;
                            rc.params[i].form = it->second;
                        } else {
                            rc.params[i].raw = raw;
                            // A value that looked like a pointer and matched no
                            // form is the dangerous case: either a form from a
                            // plugin that is not loaded, or something we do not
                            // understand. Either way the bits are meaningless
                            // next launch.
                            if (LooksLikePointer(raw)) {
                                if (const char* fn = ScriptStateFunction(rc.function)) {
                                    a_why = std::format(
                                        "it reads another mod's script state ({}), which is not "
                                        "something a box can carry - the effect would switch on "
                                        "and off according to a variable that has nothing to do "
                                        "with the box",
                                        fn);
                                } else {
                                    a_why = std::format(
                                        "condition function {} takes a parameter that is neither a "
                                        "number nor a form CEF can identify (effect {}, condition "
                                        "{}, parameter {}), so it cannot be rebuilt after a restart",
                                        rc.function, a_out.size(), re.conditions.size(), i + 1);
                                }
                                return false;
                            }
                        }
                    }
                    re.conditions.push_back(std::move(rc));
                }
                a_out.push_back(std::move(re));
            }

            if (a_out.empty()) {
                a_why = "the enchantment contributes no effects";
                return false;
            }
            return true;
        }

        const RE::EnchantmentItem* EnchantmentOf(const std::string& a_contentId)
        {
            auto* form = LookupColon(a_contentId);
            if (!form) {
                return nullptr;
            }
            if (auto* asEnch = form->As<RE::EnchantmentItem>()) {
                return asEnch;
            }
            if (auto* armo = form->As<RE::TESObjectARMO>()) {
                return armo->formEnchanting;
            }
            return nullptr;
        }

        // ---------------------------------------------------------------
        // Registry
        // ---------------------------------------------------------------

        std::uint64_t Fnv1a(std::string_view a_text)
        {
            std::uint64_t h = 1469598103934665603ull;
            for (const unsigned char c : a_text) {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }

        nlohmann::json ToJson(const RecipeCondition& a_c)
        {
            nlohmann::json j;
            j["fn"] = a_c.function;
            j["flags"] = a_c.flags;
            j["object"] = a_c.object;
            j["dataID"] = a_c.dataID;
            if (a_c.compareIsGlobal) {
                j["global"] = a_c.compareGlobal;
            } else {
                j["value"] = a_c.compareFloat;
            }
            auto arr = nlohmann::json::array();
            for (const auto& p : a_c.params) {
                nlohmann::json pj;
                if (p.isForm) {
                    pj["form"] = p.form;
                } else {
                    pj["raw"] = p.raw;
                }
                arr.push_back(pj);
            }
            j["params"] = arr;
            return j;
        }

        bool FromJson(const nlohmann::json& a_j, RecipeCondition& a_out)
        {
            try {
                a_out.function = a_j.at("fn").get<std::uint16_t>();
                a_out.flags = a_j.at("flags").get<std::uint8_t>();
                a_out.object = a_j.at("object").get<std::uint8_t>();
                a_out.dataID = a_j.at("dataID").get<std::uint32_t>();
                if (a_j.contains("global")) {
                    a_out.compareIsGlobal = true;
                    a_out.compareGlobal = a_j.at("global").get<std::string>();
                } else {
                    a_out.compareFloat = a_j.at("value").get<float>();
                }
                const auto& arr = a_j.at("params");
                for (std::size_t i = 0; i < 2 && i < arr.size(); ++i) {
                    if (arr[i].contains("form")) {
                        a_out.params[i].isForm = true;
                        a_out.params[i].form = arr[i].at("form").get<std::string>();
                    } else {
                        a_out.params[i].raw = arr[i].at("raw").get<std::uint64_t>();
                    }
                }
            } catch (const std::exception&) {
                return false;
            }
            return true;
        }

        nlohmann::json AbilitiesArray()
        {
            auto arr = nlohmann::json::array();
            for (int i = 0; i < kPoolSize; ++i) {
                const auto& s = g_slots[static_cast<std::size_t>(i)];
                if (!s) {
                    continue;
                }
                nlohmann::json j;
                j["slot"] = i;
                j["content"] = s->content;
                j["generation"] = s->generation;
                j["tombstone"] = s->tombstone;
                auto eff = nlohmann::json::array();
                for (const auto& e : s->effects) {
                    nlohmann::json ej;
                    ej["mgef"] = e.mgef;
                    ej["magnitude"] = e.magnitude;
                    ej["area"] = e.area;
                    ej["duration"] = e.duration;
                    ej["cost"] = e.cost;
                    auto cond = nlohmann::json::array();
                    for (const auto& c : e.conditions) {
                        cond.push_back(ToJson(c));
                    }
                    ej["conditions"] = cond;
                    eff.push_back(ej);
                }
                j["effects"] = eff;
                arr.push_back(j);
            }
            return arr;
        }

        // Rotate two generations before replacing. The registry is the only
        // record of which ability a save is pointing at: lose it and the saves
        // are not recoverable by inspection, because a pool spell says nothing
        // about who it was for. Losing settings loses a preference; losing this
        // loses the mapping.
        void RotateBackups()
        {
            namespace fs = std::filesystem;
            const fs::path cur{ kRegistryPath };
            std::error_code ec;
            fs::remove(fs::path{ std::string(kRegistryPath) + ".bak2" }, ec);
            fs::rename(fs::path{ std::string(kRegistryPath) + ".bak1" },
                fs::path{ std::string(kRegistryPath) + ".bak2" }, ec);
            fs::copy_file(cur, fs::path{ std::string(kRegistryPath) + ".bak1" },
                fs::copy_options::overwrite_existing, ec);
        }

        bool SaveRegistry()
        {
            nlohmann::json doc;
            doc["schema"] = kSchema;
            const auto arr = AbilitiesArray();
            const auto body = arr.dump();
            doc["checksum"] = std::format("{:016x}", Fnv1a(body));
            doc["abilities"] = arr;

            RotateBackups();
            if (!WriteFileAtomic(kRegistryPath, doc.dump(2))) {
                SKSE::log::error("abilities: could not write {} - no new allocation is in effect",
                    kRegistryPath);
                return false;
            }
            return true;
        }

        // Returns false only for a registry that EXISTS and is broken. A
        // missing file is a first run.
        bool LoadRegistry(std::string& a_why)
        {
            g_slots.assign(kPoolSize, std::nullopt);
            g_byContent.clear();

            std::ifstream in(kRegistryPath);
            if (!in) {
                return true;
            }
            nlohmann::json doc;
            try {
                in >> doc;
            } catch (const std::exception& e) {
                a_why = std::format("{} is not readable ({})", kRegistryPath, e.what());
                return false;
            }
            const int schema = doc.value("schema", 0);
            if (schema != kSchema) {
                a_why = std::format("{} is schema {}, this build reads {}", kRegistryPath, schema,
                    kSchema);
                return false;
            }
            if (!doc.contains("abilities") || !doc["abilities"].is_array()) {
                a_why = std::format("{} has no abilities list", kRegistryPath);
                return false;
            }
            const auto stored = doc.value("checksum", std::string{});
            const auto actual = std::format("{:016x}", Fnv1a(doc["abilities"].dump()));
            if (stored != actual) {
                a_why = std::format("{} does not match its own checksum - it has been edited or "
                                    "truncated (a copy of the last two good ones is beside it)",
                    kRegistryPath);
                return false;
            }

            for (const auto& j : doc["abilities"]) {
                const int slot = j.value("slot", -1);
                if (slot < 0 || slot >= kPoolSize) {
                    a_why = std::format("{} refers to slot {}, which is outside the pool",
                        kRegistryPath, slot);
                    return false;
                }
                Entry e;
                e.content = j.value("content", std::string{});
                e.generation = j.value("generation", 0u);
                e.tombstone = j.value("tombstone", false);
                for (const auto& ej : j.value("effects", nlohmann::json::array())) {
                    RecipeEffect re;
                    re.mgef = ej.value("mgef", std::string{});
                    re.magnitude = ej.value("magnitude", 0.0f);
                    re.area = ej.value("area", 0u);
                    re.duration = ej.value("duration", 0u);
                    re.cost = ej.value("cost", 0.0f);
                    for (const auto& cj : ej.value("conditions", nlohmann::json::array())) {
                        RecipeCondition rc;
                        if (!FromJson(cj, rc)) {
                            a_why = std::format("{} has a condition this build cannot read (slot "
                                                "{})",
                                kRegistryPath, slot);
                            return false;
                        }
                        re.conditions.push_back(std::move(rc));
                    }
                    e.effects.push_back(std::move(re));
                }
                if (!e.tombstone && !e.content.empty()) {
                    g_byContent[e.content] = slot;
                }
                g_slots[static_cast<std::size_t>(slot)] = std::move(e);
            }
            return true;
        }

        // ---------------------------------------------------------------
        // Hydration
        // ---------------------------------------------------------------

        void ApplyCondition(RE::TESConditionItem* a_node, const RecipeCondition& a_c, bool& a_ok)
        {
            a_node->data.functionData.function =
                static_cast<RE::FUNCTION_DATA::FunctionID>(a_c.function);
            *reinterpret_cast<std::uint8_t*>(&a_node->data.flags) = a_c.flags;
            a_node->data.object = static_cast<RE::CONDITIONITEMOBJECT>(a_c.object);
            a_node->data.dataID = a_c.dataID;

            if (a_c.compareIsGlobal) {
                auto* g = LookupColon(a_c.compareGlobal);
                if (!g) {
                    a_ok = false;
                    return;
                }
                a_node->data.comparisonValue.g = reinterpret_cast<RE::TESGlobal*>(g);
            } else {
                a_node->data.comparisonValue.f = a_c.compareFloat;
            }
            for (int i = 0; i < 2; ++i) {
                if (a_c.params[i].isForm) {
                    auto* f = LookupColon(a_c.params[i].form);
                    if (!f) {
                        a_ok = false;
                        return;
                    }
                    a_node->data.functionData.params[i] = f;
                } else {
                    a_node->data.functionData.params[i] =
                        reinterpret_cast<void*>(a_c.params[i].raw);
                }
            }
        }

        // Fill one pool spell from its recipe. Called only from
        // InitAtDataLoaded and from a fresh allocation, so nothing can be
        // holding the spell yet - which is the rule that matters, because
        // rewriting the effect list under a live ability is what strands a
        // modifier in the first place.
        bool Hydrate(int a_slot)
        {
            auto& s = g_slots[static_cast<std::size_t>(a_slot)];
            if (!s) {
                return true;
            }
            auto* spell = PoolSpell(a_slot);
            if (!spell) {
                s->invalid = true;
                SKSE::log::error("abilities: slot {} does not resolve in {}", a_slot, kPoolPlugin);
                return false;
            }

            std::vector<RE::Effect*> built;
            bool ok = true;
            for (const auto& re : s->effects) {
                auto* mgef = LookupColon(re.mgef);
                auto* base = mgef ? mgef->As<RE::EffectSetting>() : nullptr;
                if (!base) {
                    SKSE::log::warn("abilities: slot {} wants magic effect {}, which does not "
                                    "resolve - is its plugin still enabled?",
                        a_slot, re.mgef);
                    ok = false;
                    break;
                }
                auto* eff = new RE::Effect();
                eff->baseEffect = base;
                eff->effectItem.magnitude = re.magnitude;
                eff->effectItem.area = re.area;
                eff->effectItem.duration = re.duration;
                eff->cost = re.cost;

                RE::TESConditionItem** tail = &eff->conditions.head;
                for (const auto& rc : re.conditions) {
                    auto* node = new RE::TESConditionItem();
                    node->next = nullptr;
                    ApplyCondition(node, rc, ok);
                    if (!ok) {
                        break;
                    }
                    *tail = node;
                    tail = &node->next;
                }
                built.push_back(eff);
                if (!ok) {
                    break;
                }
            }

            if (!ok) {
                // Nothing partial reaches the spell. The Effects built so far
                // are leaked rather than deleted: ~Effect runs ~TESCondition,
                // and unpicking a chain that ApplyCondition abandoned halfway
                // is more ways to be wrong than the handful of bytes is worth
                // on a path that only runs when something is already broken.
                s->invalid = true;
                SKSE::log::error("abilities: slot {} ({}) could not be rebuilt - it will not be "
                                 "granted to anyone",
                    a_slot, s->content);
                return false;
            }

            spell->effects.clear();
            for (auto* e : built) {
                spell->effects.push_back(e);
            }
            s->invalid = false;
            return true;
        }
    }

    // -------------------------------------------------------------------
    // Public
    // -------------------------------------------------------------------

    void InitAtDataLoaded()
    {
        g_state = State::Uninitialized;
        g_why.clear();

        // 1. Is the pool there, and big enough?
        auto* first = PoolSpell(0);
        auto* last = PoolSpell(kPoolSize - 1);
        const bool poolPresent = first && last;

        // 2. Read the registry regardless, because whether a MISSING pool is a
        //    broken install or simply an option the user did not tick is
        //    decided by whether anything was ever allocated (design 5.9).
        std::string why;
        const bool registryOk = LoadRegistry(why);
        const bool registryEmpty = g_byContent.empty() &&
            std::none_of(g_slots.begin(), g_slots.end(),
                [](const auto& s) { return s.has_value(); });

        if (!registryOk) {
            Disable(why + ". Enchantment passthrough is off until this is sorted out; nothing "
                          "has been changed on your character.");
            return;
        }

        if (!poolPresent) {
            if (registryEmpty) {
                g_state = State::DisabledSafe;
                g_why = std::format(
                    "{} is not installed, so enchantment passthrough is off. Costumes look "
                    "exactly the same; they just do not carry stats. Re-run the installer and "
                    "tick it if you want them back.",
                    kPoolPlugin);
                SKSE::log::info("abilities: {}", g_why);
            } else {
                // The dangerous half of 5.9. Saves already point at abilities in
                // a plugin that is no longer there, and pretending the feature
                // is merely "off" would let those saves keep loading while the
                // modifiers they applied can never be taken back off.
                Disable(std::format(
                    "{} is MISSING but {} has already handed out abilities. Your saves refer to "
                    "forms in that plugin. Put it back - re-run the installer with enchantment "
                    "passthrough ticked - before loading a save, or the bonuses it applied "
                    "cannot be removed.",
                    kPoolPlugin, kRegistryPath));
            }
            return;
        }
        // PoolValidated -> RecipesLoaded, both reached by getting here: the pool
        // resolved at both ends and the registry parsed with a matching
        // checksum. They are named in the enum because the design names them,
        // not because anything can observe the gap.
        g_state = State::RecipesLoaded;

        // 3. Fill every allocated ability NOW, before any save can be read.
        //    Measured: doing this after the save is up strands the modifier.
        int ok = 0;
        int bad = 0;
        for (int i = 0; i < kPoolSize; ++i) {
            if (!g_slots[static_cast<std::size_t>(i)]) {
                continue;
            }
            if (Hydrate(i)) {
                ++ok;
            } else {
                ++bad;
            }
        }
        g_state = State::Ready;  // via AbilitiesHydrated - see above

        const auto u = PoolUsage();
        SKSE::log::info("abilities: ready - {} rebuilt, {} unavailable, {} live / {} tombstoned / "
                        "{} free of {}",
            ok, bad, u.used, u.tombstoned, u.free, u.total);
    }

    State CurrentState() { return g_state; }
    bool Ready() { return g_state == State::Ready; }
    std::string DisabledReason() { return g_state == State::Ready ? std::string{} : g_why; }

    Usage PoolUsage()
    {
        Usage u;
        u.total = kPoolSize;
        for (const auto& s : g_slots) {
            if (!s) {
                continue;
            }
            if (s->tombstone) {
                ++u.tombstoned;
            } else {
                ++u.used;
            }
        }
        u.free = u.total - u.used - u.tombstoned;
        return u;
    }

    RE::SpellItem* AbilityFor(const std::string& a_contentId)
    {
        if (!Ready()) {
            return nullptr;
        }
        if (const auto it = g_byContent.find(a_contentId); it != g_byContent.end()) {
            const auto& s = g_slots[static_cast<std::size_t>(it->second)];
            return (s && !s->invalid) ? PoolSpell(it->second) : nullptr;
        }

        const auto* ench = EnchantmentOf(a_contentId);
        std::vector<RecipeEffect> effects;
        std::string why;
        if (!BuildRecipe(ench, effects, why)) {
            // The whole enchantment is refused, not the one effect that failed
            // (design 5.7). Half an enchantment is a different enchantment, and
            // the author did not design that one.
            SKSE::log::info("abilities: {} passes no stats through - {}", a_contentId, why);
            return nullptr;
        }

        // Monotonic: never reuse a slot, not even a tombstoned one. An old save
        // can still name it, and handing that id to a different content would
        // apply someone else's bonus rather than nothing at all - a wrong number
        // is harder to notice than a missing one (design 5.2).
        int slot = -1;
        for (int i = 0; i < kPoolSize; ++i) {
            if (!g_slots[static_cast<std::size_t>(i)]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            SKSE::log::error("abilities: the pool is full ({} of {} used). New content keeps its "
                             "looks and loses its stats; existing content is unaffected.",
                kPoolSize, kPoolSize);
            return nullptr;
        }

        Entry e;
        e.content = a_contentId;
        e.effects = std::move(effects);
        g_slots[static_cast<std::size_t>(slot)] = std::move(e);

        // Save BEFORE building the form (design 6): a slot that is in use but
        // not on disk would be handed out again next launch, to different
        // content, while a save still points at it.
        if (!SaveRegistry()) {
            g_slots[static_cast<std::size_t>(slot)].reset();
            return nullptr;
        }
        if (!Hydrate(slot)) {
            return nullptr;
        }
        g_byContent[a_contentId] = slot;
        SKSE::log::info("abilities: {} -> slot {} ({} effect(s))", a_contentId, slot,
            g_slots[static_cast<std::size_t>(slot)]->effects.size());
        return PoolSpell(slot);
    }

    void ReportLegacySave(bool a_prePoolSave)
    {
        if (!a_prePoolSave) {
            return;
        }
        // The co-save cannot answer this: its ACTV record holds PERSIST items
        // only, because box definitions are global config. Which boxes were
        // worn is in the save's own equip state, so it is read here, after the
        // reconcile has re-applied it.
        std::vector<std::string> contents;
        for (const auto& it : ActiveSnapshot()) {
            contents.push_back(it.id);
        }

        // Sum what the old mechanism would have been applying. Unconditional
        // effects are separated from conditional ones because only the first
        // group is certain: a conditional effect applied only if its condition
        // happened to be true at the moment of the save, and saying "+40 frost
        // resist is stuck" to someone who saved standing up would send them
        // subtracting a number they never had.
        std::unordered_map<std::uint32_t, float> certain;
        std::unordered_map<std::uint32_t, float> possible;
        int unreadable = 0;
        int noStats = 0;

        for (const auto& id : contents) {
            std::vector<RecipeEffect> effects;
            std::string why;
            if (!BuildRecipe(EnchantmentOf(id), effects, why)) {
                ++noStats;
                continue;
            }
            for (const auto& e : effects) {
                auto* form = LookupColon(e.mgef);
                auto* mgef = form ? form->As<RE::EffectSetting>() : nullptr;
                if (!mgef) {
                    ++unreadable;
                    continue;
                }
                const auto av = static_cast<std::uint32_t>(mgef->data.primaryAV);
                if (av >= static_cast<std::uint32_t>(RE::ActorValue::kTotal)) {
                    // A script effect or anything else that is not a plain
                    // number on an actor value. Counted, not guessed at.
                    ++unreadable;
                    continue;
                }
                (e.conditions.empty() ? certain : possible)[av] += e.magnitude;
            }
        }

        std::string out =
            "[CEF] This save was made before 1.6.3.\n"
            "\n"
            "Up to 1.6.2 a costume's enchantment bonuses were applied through a spell that\n"
            "only existed while the game was running. Loading the save in a new process\n"
            "could not take them back off again, so whatever was applied when you saved is\n"
            "now part of your character. 1.6.3 stops this happening from here on; it cannot\n"
            "undo what is already there.\n";

        const auto lines = [](const std::unordered_map<std::uint32_t, float>& a_map) {
            std::vector<std::pair<std::string, float>> rows;
            rows.reserve(a_map.size());
            for (const auto& [av, total] : a_map) {
                if (total != 0.0f) {
                    rows.emplace_back(AvConsoleName(av), total);
                }
            }
            std::sort(rows.begin(), rows.end(),
                [](const auto& l, const auto& r) { return l.first < r.first; });
            return rows;
        };

        const auto certainRows = lines(certain);
        const auto possibleRows = lines(possible);

        if (!certainRows.empty()) {
            out += std::format("\nMost likely stuck, from the {} item(s) that were active:\n",
                contents.size());
            for (const auto& [name, total] : certainRows) {
                out += std::format("  {:<22} {:+g}\n", name, total);
            }
        }
        if (!possibleRows.empty()) {
            out += "\nAnd these, but only if their condition was true at the moment you saved\n"
                   "(sneaking, blocking, in combat and so on):\n";
            for (const auto& [name, total] : possibleRows) {
                out += std::format("  {:<22} {:+g}\n", name, total);
            }
        }
        if (contents.empty()) {
            // Worth saying rather than staying quiet. Nothing is worn, so this
            // load added nothing - but a character who wore boxes under 1.6.2
            // and took them off later is still carrying whatever was live at
            // every quit, and would otherwise never be told.
            out +=
                "\nNothing is being worn right now, so this load has not added anything.\n"
                "If this character has worn CEF boxes under 1.6.2 or earlier, though, what\n"
                "each of those sessions left behind is still there, and it adds up.\n";
        } else if (certainRows.empty() && possibleRows.empty()) {
            out += std::format(
                "\nNone of the {} item(s) being worn passes a plain numeric bonus through, so\n"
                "there is probably nothing new stuck. Check anyway.\n",
                contents.size());
        }
        if (unreadable) {
            out += std::format("\n{} effect(s) are not a simple number on an actor value and are\n"
                               "not counted above.\n",
                unreadable);
        }

        out +=
            "\nThese are estimates from what the items hold now. Check the real numbers:\n"
            "  cef av\n"
            "and take one off with, for example:\n";
        const auto& sample = !certainRows.empty() ? certainRows : possibleRows;
        if (!sample.empty()) {
            out += std::format("  player.modav {} -{:g}\n", sample.front().first,
                sample.front().second);
        } else {
            out += "  player.modav health -250\n";
        }
        out +=
            "\nCEF will not do that for you. It cannot tell its own leftovers from a bonus\n"
            "another mod applied on purpose, and subtracting the wrong one is worse than\n"
            "leaving it.\n";

        g_legacyReport = out;

        // The console is not open at load, so the log gets the whole thing and
        // the screen gets one line that says where to find it.
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            SKSE::log::warn("legacy: {}", line);
        }
        RE::DebugNotification("CEF: this save predates 1.6.3 - open the console and type: "
                              "cef abilities legacy");
    }

    void AbilitiesCommand(const std::string& a_args)
    {
        std::istringstream iss(a_args);
        std::string sub;
        iss >> sub;

        if (sub.empty() || sub == "state" || sub == "usage") {
            const auto u = PoolUsage();
            Print(std::format("[CEF abilities] {} - {} used, {} tombstoned, {} free of {}",
                Ready() ? "ready" : "NOT READY", u.used, u.tombstoned, u.free, u.total));
            if (!Ready()) {
                Print(std::format("  {}", g_why));
            }
            return;
        }

        if (sub == "list") {
            int shown = 0;
            for (int i = 0; i < kPoolSize; ++i) {
                const auto& s = g_slots[static_cast<std::size_t>(i)];
                if (!s) {
                    continue;
                }
                const auto line = std::format("  {:4} {}{}{}  {} effect(s)  <- {}", i,
                    s->tombstone ? "tomb " : "live ", s->invalid ? "INVALID " : "",
                    std::format("gen{}", s->generation), s->effects.size(), s->content);
                Print(line);
                SKSE::log::info("abilities:{}", line);
                if (++shown >= 40) {
                    Print("  ... (the rest is in the log)");
                    break;
                }
            }
            if (shown == 0) {
                Print("[CEF abilities] nothing allocated yet");
            }
            return;
        }

        if (sub == "legacy") {
            if (g_legacyReport.empty()) {
                Print("[CEF abilities] this save was not made before 1.6.3 (or nothing was "
                      "active in it) - nothing to report");
                return;
            }
            std::istringstream rep(g_legacyReport);
            std::string line;
            while (std::getline(rep, line)) {
                Print(line);
            }
            return;
        }

        if (sub == "alloc") {
            std::string id;
            std::getline(iss, id);
            while (!id.empty() && id.front() == ' ') {
                id.erase(id.begin());
            }
            if (id.find(':') == std::string::npos) {
                Print("[CEF abilities] usage: cef abilities alloc <FormID:Plugin.esp>");
                return;
            }
            auto* spell = AbilityFor(id);
            Print(spell ? std::format("[CEF abilities] {} -> {:08X}", id, spell->GetFormID())
                        : std::format("[CEF abilities] no ability for {} - see the log", id));
            return;
        }

        Print("[CEF abilities] state | list | alloc <id> | usage | legacy");
    }
}
