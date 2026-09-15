#include "AbilityPool.h"

#include "AtomicWrite.h"
#include "AvDiag.h"  // AvConsoleName - the legacy report must name actor values
#include "BoxStore.h"    // ContentEffectsFor - what a content is really worth
#include "SkinRebind.h"  // ActiveSnapshot - what is worn RIGHT NOW
#include "ConsoleOut.h"
#include "FormId.h"
#include "TokenIdentity.h"  // which generation a pool plugin name is

#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/A/ActorValues.h"
#include "RE/M/Misc.h"  // DebugNotification
#include "RE/P/PlayerCharacter.h"
#include "RE/S/SpellItem.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESObjectARMO.h"
#include "RE/T/TESCondition.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"

#include <Windows.h>  // CopyFileA - the one-shot pre-1.6.4 registry copy

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CostumeFW::abilities
{
    namespace
    {
        void Print(std::string_view a_msg) { ConsolePrint(a_msg); }

        // Generation 1's plugin. It shipped in 1.6.3 under this name and cannot
        // be renamed, so it is the ONE exception in the generation naming - see
        // tokenid::AbilityPoolPluginName. Still spelled out here because most of
        // what the user is told names it.
        constexpr const char* kPoolPlugin = "CostumeFW_Abilities.esp";
        constexpr std::uint32_t kPoolBase = 0x800;
        // What generation 1 shipped with. Used ONLY to bound the schema-1
        // migration - the live size of a generation is measured, not assumed,
        // because the answer is whatever plugin is installed (see PoolLimit).
        constexpr int kLegacyPoolSize = 1024;
        // A generation cannot have more local ids than the ESL range holds, and
        // the probe that measures one stops here rather than trusting a number
        // in a file.
        constexpr std::uint32_t kMaxPerGeneration = 0x1000 - 0x800;

        constexpr const char* kRegistryPath = "Data\\SKSE\\Plugins\\CEF_abilities.json";
        // Kept at the moment 1.6.4 first reads a 1.6.3 registry, and never
        // again. The .bak1/.bak2 rotation below is TWO writes deep, so it is
        // gone after two allocations - this is the copy that makes going back to
        // 1.6.3 a file copy rather than a reconstruction. CEF never reads it.
        constexpr const char* kRegistryPre164Path =
            "Data\\SKSE\\Plugins\\CEF_abilities.pre164.json";
        constexpr int kSchema = 2;        // entries name an ability, not a slot
        constexpr int kSchemaLegacy = 1;  // ... a slot index into generation 1

        // ---------------------------------------------------------------
        // Recipe
        // ---------------------------------------------------------------

        // A condition parameter is a void* that the engine reads as a small
        // integer for some functions and as a TESForm* for others - about four
        // in ten of the conditional wearable enchantments in a heavy load order
        // use the form kind (WornHasKeyword, GetEquipped, IsSpellTarget,
        // GetGlobalValue...). A pointer is a different address next launch, so
        // storing the raw bits would restore a condition pointing at whatever
        // happens to live there. The effect-folding code this replaced compared
        // them as raw bits and said so in its own comment; that is right for
        // COMPARING and wrong for saving, which is why none of it was reused.
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

        // WHICH ability, across generations: the pool plugin's generation number
        // and the local FormID in it. Ordered by exactly that pair, which is the
        // order §7.3 allocates in - so the map's own iteration order IS the
        // allocation order, and every walk, listing and diagnostic comes out
        // stable without sorting anything.
        //
        // The local id alone does not identify an ability: generation 1's 0x800
        // and generation 2's 0x800 are different spells in different plugins,
        // and a save can hold either.
        struct PoolKey
        {
            int generation{ 0 };
            std::uint32_t local{ 0 };
            auto operator<=>(const PoolKey&) const = default;
        };

        std::map<PoolKey, Entry> g_entries;
        std::unordered_map<std::string, PoolKey> g_byContent;  // content -> live ability

        // Generations 1..N, contiguous from 1, as measured at kDataLoaded. A
        // generation installed ABOVE a gap is deliberately not in here: §4.2
        // stops new allocation on a broken run rather than skipping over it.
        std::vector<int> g_generations;
        std::unordered_map<int, std::uint32_t> g_genCount;  // generation -> abilities in it
        // Generations the registry names that are not loaded. Non-empty is the
        // fail-closed case (§7.4): a save holds abilities whose forms are gone.
        std::set<int> g_missingGenerations;
        // Set when the registry that was read was written by 1.6.3.
        bool g_migratedFromSchema1 = false;
        State g_state = State::Uninitialized;
        std::string g_why;
        std::string g_legacyReport;  // set when a pre-1.6.3 save is loaded


        // Set by Disable() and not by the benign "the ability plugin is not
        // installed" path, which lands in the same state. One is a fault the
        // user has to act on; the other is a choice they made in the installer,
        // and a message box every load for that is how a mod teaches people to
        // dismiss its message boxes.
        bool g_faulted = false;

        // Plugins a recipe named for a magic effect that would not resolve at
        // hydration. The pool as a whole is fine, so this is not a fault state -
        // but every slot that named one is an ability with no effects, holding a
        // modifier nobody can take off, and the user has to be told before they
        // save over it.
        std::set<std::string> g_missingPlugins;

        void Disable(std::string a_why)
        {
            g_state = State::DisabledSafe;
            g_why = std::move(a_why);
            g_faulted = true;
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

        // The canonical colon-id of a pool ability, which is what the registry
        // stores and what a diagnostic prints.
        std::string ColonIdOf(PoolKey a_key)
        {
            const auto plugin = tokenid::AbilityPoolPluginName(a_key.generation);
            return plugin.empty() ? std::string{} : std::format("{:06X}:{}", a_key.local, plugin);
        }

        // ... and back. False for anything that is not one of ours: a plugin
        // name no generation owns, a local id outside the ESL range, a malformed
        // id. The caller refuses the registry rather than guessing.
        bool ParseAbilityId(const std::string& a_id, PoolKey& a_out)
        {
            const auto colon = a_id.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= a_id.size()) {
                return false;
            }
            const auto hex = a_id.substr(0, colon);
            if (hex.size() > 8 ||
                hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                return false;  // bounded so the conversion below cannot throw
            }
            const int generation = tokenid::AbilityPoolGeneration(a_id.substr(colon + 1));
            if (generation <= 0) {
                return false;
            }
            const auto local = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
            if (local < kPoolBase || local >= kPoolBase + kMaxPerGeneration) {
                return false;
            }
            a_out = PoolKey{ generation, local };
            return true;
        }

        RE::SpellItem* PoolSpell(PoolKey a_key)
        {
            const auto id = ColonIdOf(a_key);
            if (id.empty()) {
                return nullptr;
            }
            auto* form = LookupColon(id);
            return form ? form->As<RE::SpellItem>() : nullptr;
        }

        // How many abilities a generation actually holds, measured by walking it
        // until a local id stops resolving. Nothing in a file is trusted for
        // this: the count is a property of the plugin that is installed, and a
        // count read from the registry would be a second place for it to be
        // wrong. Called once per generation at kDataLoaded.
        std::uint32_t PoolLimit(int a_generation)
        {
            std::uint32_t n = 0;
            while (n < kMaxPerGeneration && PoolSpell(PoolKey{ a_generation, kPoolBase + n })) {
                ++n;
            }
            return n;
        }

        // The generations installed, contiguous from 1. A generation above a gap
        // is left out on purpose (§4.2): allocating into it would put a save's
        // ability in a plugin the user is one step away from removing, and the
        // gap is what the diagnostic has to name.
        void MeasureGenerations()
        {
            g_generations.clear();
            g_genCount.clear();
            for (int generation = 1; generation <= 99; ++generation) {
                const auto plugin = tokenid::AbilityPoolPluginName(generation);
                if (plugin.empty() || !PluginIsLoaded(plugin)) {
                    break;
                }
                const auto count = PoolLimit(generation);
                if (count == 0) {
                    SKSE::log::error("abilities: {} is loaded but holds no pool spell at {:06X}",
                        plugin, kPoolBase);
                    break;
                }
                g_generations.push_back(generation);
                g_genCount[generation] = count;
                SKSE::log::info("abilities: {} - {} ability slot(s)", plugin, count);
            }
        }

        bool GenerationInstalled(int a_generation)
        {
            return g_genCount.contains(a_generation);
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
        bool BuildRecipe(const std::vector<SourceEffect>& a_src,
            std::vector<RecipeEffect>& a_out, std::string& a_why)
        {
            a_out.clear();
            if (a_src.empty()) {
                a_why = "it contributes no effects";
                return false;
            }

            // Collect every parameter that might be a form, across the whole
            // set, so the form table is walked once rather than per condition.
            std::vector<std::uint64_t> candidates;
            for (const auto& se : a_src) {
                const auto* e = se.live;
                if (!e) {
                    continue;  // a flat snapshot carries no conditions
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

            for (const auto& se : a_src) {
                const auto* e = se.live;
                RE::EffectSetting* base = e ? e->baseEffect : se.mgef;
                if (!base) {
                    continue;
                }
                RecipeEffect re;
                re.mgef = MakeColonId(base);
                if (re.mgef.find(':') == std::string::npos || re.mgef.back() == ':') {
                    a_why = std::format("magic effect {} has no defining plugin", re.mgef);
                    return false;
                }
                re.magnitude = e ? e->effectItem.magnitude : se.magnitude;
                re.area = e ? e->effectItem.area : 0u;
                re.duration = e ? e->effectItem.duration : 0u;
                re.cost = e ? e->cost : 0.0f;

                for (const auto* c = e ? e->conditions.head : nullptr; c; c = c->next) {
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
                a_why = "none of its effects name a magic effect that resolves";
                return false;
            }
            return true;
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

        nlohmann::json EffectsJson(const std::vector<RecipeEffect>& a_effects)
        {
            auto eff = nlohmann::json::array();
            for (const auto& e : a_effects) {
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
            return eff;
        }

        nlohmann::json AbilitiesArray()
        {
            auto arr = nlohmann::json::array();
            for (const auto& [key, e] : g_entries) {
                nlohmann::json j;
                // Schema 2 names the ABILITY, not a slot index: a slot index
                // only identified anything while there was one pool plugin.
                j["ability"] = ColonIdOf(key);
                j["content"] = e.content;
                // Renamed from "generation" (§7.2 / A17). This is the RECIPE
                // generation - how many times this content's enchantment has
                // changed - and sat one word away from the POOL generation,
                // which is now a real number in the same file.
                j["recipeGeneration"] = e.generation;
                j["tombstone"] = e.tombstone;
                j["effects"] = EffectsJson(e.effects);
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

        // Taken at the moment 1.6.4 has just read a 1.6.3 registry, and never
        // again. CopyFileA with bFailIfExists=TRUE is the whole "once"
        // mechanism: no flag to keep in step, and a pre164 file the user
        // restored by hand is not silently overwritten (same shape as the
        // settings one in BoxStore).
        void MaybeWritePre164Registry()
        {
            if (!g_migratedFromSchema1) {
                return;
            }
            if (::CopyFileA(kRegistryPath, kRegistryPre164Path, TRUE)) {
                SKSE::log::info(
                    "abilities: kept the 1.6.3 registry as {} before writing schema {} "
                    "(CEF never reads it; it is there if you go back)",
                    kRegistryPre164Path, kSchema);
            } else if (::GetLastError() != ERROR_FILE_EXISTS) {
                SKSE::log::warn("abilities: could not keep a pre-1.6.4 copy at {} (error {})",
                    kRegistryPre164Path, ::GetLastError());
            }
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
            g_entries.clear();
            g_byContent.clear();
            g_missingGenerations.clear();
            g_migratedFromSchema1 = false;

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
            if (schema != kSchema && schema != kSchemaLegacy) {
                a_why = std::format("{} is schema {}, this build reads {} and {}", kRegistryPath,
                    schema, kSchemaLegacy, kSchema);
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
                PoolKey key;
                if (schema == kSchemaLegacy) {
                    // §7.2. 1.6.3 stored an index into the only pool there was,
                    // so the ability it meant is arithmetic, not a guess. The
                    // range is checked before anything is written: a migration
                    // that runs on a file it did not fully understand is how a
                    // save ends up pointing at the wrong spell.
                    const int slot = j.value("slot", -1);
                    if (slot < 0 || slot >= kLegacyPoolSize) {
                        a_why = std::format("{} refers to slot {}, which is outside the pool",
                            kRegistryPath, slot);
                        return false;
                    }
                    key = PoolKey{ 1, kPoolBase + static_cast<std::uint32_t>(slot) };
                } else {
                    const auto id = j.value("ability", std::string{});
                    if (!ParseAbilityId(id, key)) {
                        a_why = std::format("{} names the ability '{}', which is not one of the "
                                            "pool plugins this build knows",
                            kRegistryPath, id);
                        return false;
                    }
                }
                if (g_entries.contains(key)) {
                    a_why = std::format("{} names the ability {} twice", kRegistryPath,
                        ColonIdOf(key));
                    return false;
                }
                Entry e;
                e.content = j.value("content", std::string{});
                // "generation" was the schema-1 spelling of recipeGeneration.
                e.generation = schema == kSchemaLegacy ? j.value("generation", 0u)
                                                       : j.value("recipeGeneration", 0u);
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
                            a_why = std::format(
                                "{} has a condition this build cannot read ({})", kRegistryPath,
                                ColonIdOf(key));
                            return false;
                        }
                        re.conditions.push_back(std::move(rc));
                    }
                    e.effects.push_back(std::move(re));
                }
                if (!e.tombstone && !e.content.empty()) {
                    g_byContent[e.content] = key;
                }
                g_entries.emplace(key, std::move(e));
            }
            if (schema == kSchemaLegacy) {
                g_migratedFromSchema1 = true;
                SKSE::log::info(
                    "abilities: read a schema {} registry - {} entry(ies) migrated to generation 1 "
                    "abilities", kSchemaLegacy, g_entries.size());
                // Before this build can write over it, and only once (§7.2 /
                // test A9). Done HERE rather than at the first save because a
                // session that loads and never allocates must still leave the
                // way back.
                MaybeWritePre164Registry();
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
        bool Hydrate(PoolKey a_key)
        {
            const auto it = g_entries.find(a_key);
            if (it == g_entries.end()) {
                return true;
            }
            Entry* s = &it->second;
            auto* spell = PoolSpell(a_key);
            if (!spell) {
                s->invalid = true;
                SKSE::log::error("abilities: {} does not resolve", ColonIdOf(a_key));
                return false;
            }

            std::vector<RE::Effect*> built;
            bool ok = true;
            for (const auto& re : s->effects) {
                auto* mgef = LookupColon(re.mgef);
                auto* base = mgef ? mgef->As<RE::EffectSetting>() : nullptr;
                if (!base) {
                    SKSE::log::warn("abilities: {} wants magic effect {}, which does not "
                                    "resolve - is its plugin still enabled?",
                        ColonIdOf(a_key), re.mgef);
                    // Remember WHICH plugin, so the warning can name it. The
                    // effect list is left untouched below, which means the
                    // spell stays the empty form the pool plugin ships - and an
                    // active effect the save restores onto an empty spell has
                    // nothing to end, so its modifier is stuck. There is no way
                    // to synthesise a remover: the actor value it wrote lives
                    // on the magic effect, and the magic effect is what is gone.
                    if (const auto colon = re.mgef.find(':');
                        colon != std::string::npos && colon + 1 < re.mgef.size()) {
                        g_missingPlugins.insert(re.mgef.substr(colon + 1));
                    }
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
                SKSE::log::error("abilities: {} ({}) could not be rebuilt - it will not be "
                                 "granted to anyone",
                    ColonIdOf(a_key), s->content);
                return false;
            }

            spell->effects.clear();
            for (auto* e : built) {
                spell->effects.push_back(e);
            }
            s->invalid = false;
            return true;
        }

        // The next ability §7.3 would lend out: generations ascending, local id
        // ascending within each. Never one that is already in the registry, not
        // even a tombstoned one - an old save can still name it, and handing it
        // to other content would pay somebody else's bonus rather than none at
        // all, which is harder to notice than a missing one (design 5.2).
        //
        // One merged walk rather than a lookup per candidate: g_entries is
        // ordered by the very key this iterates, so the cursor only moves
        // forward.
        std::optional<PoolKey> FirstFreeAbility()
        {
            auto it = g_entries.begin();
            for (const int generation : g_generations) {
                const auto count = g_genCount[generation];
                for (std::uint32_t i = 0; i < count; ++i) {
                    const PoolKey key{ generation, kPoolBase + i };
                    while (it != g_entries.end() && it->first < key) {
                        ++it;
                    }
                    if (it == g_entries.end() || it->first != key) {
                        return key;
                    }
                    ++it;
                }
            }
            return std::nullopt;
        }

        // Take the next free ability for a content and make it live: build the
        // recipe, write the registry, fill the spell.
        RE::SpellItem* Allocate(const std::string& a_contentId,
            const std::vector<SourceEffect>& a_effects, std::uint32_t a_generation)
        {
            std::vector<RecipeEffect> effects;
            std::string why;
            if (!BuildRecipe(a_effects, effects, why)) {
                // The WHOLE enchantment is refused, not the one effect that
                // failed (design 5.7). Half an enchantment is a different
                // enchantment, and the author did not design that one.
                SKSE::log::info("abilities: {} passes no stats through - {}", a_contentId, why);
                return nullptr;
            }

            // A slot THIS content has already had, holding exactly these
            // effects, is revived rather than duplicated. RefreshContent has
            // always matched before taking a new generation; this path had no
            // such check, so every route that goes through a tombstone and back
            // burned a slot. Measured 2026-09-14 (7.2 #23b): turning a costume
            // mod off and on again left
            //     18 gen0 tomb  000802:...  mag 175  cost 2199.901123046875
            //     19 gen0 live  000802:...  mag 175  cost 2199.901123046875
            // identical to the last bit, one slot poorer, with nothing to stop
            // the next toggle doing it again. Slots are never handed to other
            // content (5.2), so one burned this way is burned for good.
            const auto wanted = EffectsJson(effects);
            for (auto& [key, e] : g_entries) {
                if (e.invalid || e.content != a_contentId) {
                    continue;
                }
                if (EffectsJson(e.effects) != wanted) {
                    continue;
                }
                const bool revived = e.tombstone;
                e.tombstone = false;
                g_byContent[a_contentId] = key;
                if (revived) {
                    SaveRegistry();
                    SKSE::log::info("abilities: {} is worth what {} (recipe gen{}) already holds - "
                                    "reviving it, no new ability taken",
                        a_contentId, ColonIdOf(key), e.generation);
                }
                return PoolSpell(key);
            }

            // Monotonic: never reuse a slot for DIFFERENT content, not even a
            // tombstoned one. An old save can still name it, and handing that id
            // to other content would apply someone else's bonus rather than none
            // at all - a wrong number is harder to notice than a missing one
            // (design 5.2). The revival above is the same content at the same
            // value, which is the one case that cannot be mistaken for another.
            const auto picked = FirstFreeAbility();
            if (!picked) {
                const auto u = PoolUsage();
                SKSE::log::error(
                    "abilities: the pool is full ({} abilities across {} generation(s)). New "
                    "content keeps its looks and loses its stats; everything already allocated is "
                    "unaffected. Install {} to add more.",
                    u.total, g_generations.size(), NextPoolPluginName());
                return nullptr;
            }
            const PoolKey key = *picked;

            Entry e;
            e.content = a_contentId;
            e.generation = a_generation;
            e.effects = std::move(effects);
            g_entries.emplace(key, std::move(e));

            // Registry BEFORE the form (design 6). An ability in use but not on
            // disk would be handed out again next launch, to different content,
            // while a save still points at it.
            if (!SaveRegistry()) {
                g_entries.erase(key);
                return nullptr;
            }
            if (!Hydrate(key)) {
                return nullptr;
            }
            g_byContent[a_contentId] = key;
            SKSE::log::info("abilities: {} -> {} recipe gen{} ({} effect(s))", a_contentId,
                ColonIdOf(key), a_generation, g_entries.at(key).effects.size());

            // Say something while there is still room to act. "The pool is full"
            // arrives when the only remaining advice is that the next costume
            // loses its stats; slots are never handed to other content, so
            // nothing frees itself and the wall does not move back. Once per
            // run - the count only ever goes down within a session.
            static bool s_saidLow = false;
            if (!s_saidLow) {
                if (const auto u = PoolUsage(); u.free <= 64) {
                    s_saidLow = true;
                    SKSE::log::warn("abilities: {} slots left of {} ({} in use, {} kept for old "
                                    "saves). A costume whose enchantment changes takes a new slot "
                                    "and keeps the old one, so this is worth watching.",
                        u.free, u.total, u.used, u.tombstoned);
                }
            }
            return PoolSpell(key);
        }
    }

    // -------------------------------------------------------------------
    // Public
    // -------------------------------------------------------------------

    void InitAtDataLoaded()
    {
        g_state = State::Uninitialized;
        g_why.clear();

        // 1. Which generations are installed, and how big is each?
        MeasureGenerations();
        const bool poolPresent = !g_generations.empty();

        // 2. Read the registry regardless, because whether a MISSING pool is a
        //    broken install or simply an option the user did not tick is
        //    decided by whether anything was ever allocated (design 5.9).
        std::string why;
        const bool registryOk = LoadRegistry(why);
        const bool registryEmpty = g_byContent.empty() && g_entries.empty();

        if (!registryOk) {
            // "Nothing has been changed" was true and not enough. With the
            // registry refused, nothing is hydrated, so every pool spell is the
            // EMPTY form the plugin ships - and a save that already holds one
            // restores an active effect with no effects to end. Measured
            // 2026-09-14 (7.2 #20): +333 Health stayed after every piece of
            // equipment came off, with nothing in the Active Effects list to
            // point at. It comes back off the moment the registry is readable
            // again, so the recovery is the important half of this message.
            Disable(why +
                ". Enchantment passthrough is off until this is sorted out. Nothing has been "
                "changed on your character - but a bonus that was ALREADY applied stays on and "
                "cannot be taken off while this is broken, because the ability it came from has "
                "no effects to end. Type  cef abilities restore  to put the last good copy back, "
                "then restart.");
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
                // The dangerous half of 5.9, and the only state where SAVING is
                // what does the damage. The forms are GONE, so the engine drops
                // the active effects that named them without calling Finish()
                // and the modifiers are already stranded the moment the save is
                // up. The save on disk is still fine: it holds the references,
                // and they resolve again as soon as the plugin is back. Save
                // now and the numbers are written into the actor with nothing
                // left anywhere to say what produced them.
                //
                // So the instruction is "do not save", not "put it back before
                // loading" - this message can only be shown AFTER a save loads,
                // which made the old wording advice nobody could still act on.
                // It also has to say what the file IS: someone updating from
                // 1.6.2 has never seen its name (test read-through 2026-09-14).
                Disable(std::format(
                    "{0} is not loaded. It carries the enchantments your costumes pass through, "
                    "and this save was made with it.\n\n"
                    "Loading without it has already left those bonuses stuck on your character, "
                    "and there is nothing left to say where they came from.\n\n"
                    "DO NOT SAVE. Quit to desktop, turn {0} back on, and load this save again. "
                    "The bonuses come off by themselves once it is there.\n\n"
                    "Save while it is missing and the numbers are baked into that save for good.\n\n"
                    "({0} is new in 1.6.3. Re-run the CEF installer and tick the enchantment "
                    "option to get it back.)",
                    kPoolPlugin));
            }
            return;
        }
        // 2b. §7.4. A generation the registry NAMES but that is not loaded is the
        //     same danger as the whole pool being gone, in miniature: those
        //     spells do not resolve, the engine drops the active effects that
        //     named them without calling Finish(), and the modifiers are already
        //     stranded. It is NOT healed by re-pointing those entries at another
        //     pool's same local id - that would pay a different bonus - and the
        //     registry is not shrunk to fit.
        for (const auto& [key, e] : g_entries) {
            if (!GenerationInstalled(key.generation)) {
                g_missingGenerations.insert(key.generation);
            }
        }
        if (!g_missingGenerations.empty()) {
            const int firstMissing = *g_missingGenerations.begin();
            int affected = 0;
            for (const auto& [key, e] : g_entries) {
                affected += g_missingGenerations.contains(key.generation) ? 1 : 0;
            }
            Disable(std::format(
                "{0} is not loaded, and this save uses {1} ability(ies) from it.\n\n"
                "Loading without it has already left those bonuses stuck on your character, and "
                "there is nothing left to say where they came from.\n\n"
                "DO NOT SAVE. Quit to desktop, turn {0} back on, and load this save again. The "
                "bonuses come off by themselves once it is there.\n\n"
                "Save while it is missing and the numbers are baked into that save for good.",
                tokenid::AbilityPoolPluginName(firstMissing), affected));
            return;
        }
        // An UNREFERENCED generation above a gap is not an error to act on, but
        // it is not usable either: §4.2 allocates into a contiguous run only, so
        // say so rather than leaving the user to wonder why their new plugin
        // changed nothing.
        for (int generation = static_cast<int>(g_generations.size()) + 2; generation <= 99;
             ++generation) {
            const auto plugin = tokenid::AbilityPoolPluginName(generation);
            if (!plugin.empty() && PluginIsLoaded(plugin)) {
                SKSE::log::warn(
                    "abilities: {} is loaded but {} is not - a generation cannot be skipped, so "
                    "nothing will be allocated from it until the gap is filled",
                    plugin, tokenid::AbilityPoolPluginName(
                        static_cast<int>(g_generations.size()) + 1));
                break;
            }
        }

        // PoolValidated -> RecipesLoaded, both reached by getting here: the pool
        // resolved and the registry parsed with a matching checksum. They are
        // named in the enum because the design names them, not because anything
        // can observe the gap.
        g_state = State::RecipesLoaded;

        // 3. Fill every allocated ability NOW, before any save can be read.
        //    Measured: doing this after the save is up strands the modifier.
        int ok = 0;
        int bad = 0;
        for (const auto& [key, e] : g_entries) {
            if (Hydrate(key)) {
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

    // DisabledReason() was declared, defined, and called from NOWHERE, so a pool
    // that refused to load said so in the log and only there. The failure it
    // reports is invisible in game - a stranded modifier appears in no menu, not
    // even Active Effects (measured 7.2 #20) - and nobody opens a log they have
    // no reason to suspect. Once per process: the registry is read at
    // kDataLoaded and the answer cannot change until the next launch.
    void ReportStateToUser()
    {
        static bool s_told = false;
        if (s_told) {
            return;
        }
        std::string why;
        if (g_faulted && !g_why.empty()) {
            why = g_why;
        } else if (!g_missingPlugins.empty()) {
            // The pool is READY and most of it works, so this is not a disabled
            // state - only the slots whose magic effect is gone are dead, and
            // they are dead in the one way that cannot be undone from here:
            // their spell has no effects, so the modifier the save restored has
            // nothing to end. Measured 2026-09-14 (7.2 #23b): +175 Health left
            // on the character after the ability was removed, with nothing in
            // Active Effects. Turning the plugin back on fixes it; saving first
            // does not. So the only thing worth stopping is the save.
            std::string list;
            for (const auto& p : g_missingPlugins) {
                list += "\n    " + p;
            }
            why = std::format(
                "Some of the enchantments your costumes pass through come from plugins that are "
                "not loaded any more:\n{}\n\n"
                "Those bonuses are on your character right now and cannot be taken off while "
                "the plugins are missing.\n\n"
                "DO NOT SAVE. Quit to desktop, turn them back on, and load this save again. The "
                "bonuses come off by themselves once they are there.\n\n"
                "Save while they are missing and the numbers are baked into that save for good.\n\n"
                "Everything from plugins you still have is working normally.",
                list);
        }
        if (why.empty()) {
            return;
        }
        s_told = true;
        const std::string box = "Costume Expansion FW\n\n" + why;
        // Same delay as the legacy report: a message box thrown at a fading-in
        // UI is a message box nobody sees.
        RunAfterDelayMs(3000, [box] { RE::DebugMessageBox(box.c_str()); });
    }

    std::string NextPoolPluginName()
    {
        // The generation AFTER the highest contiguous installed one. g_generations
        // is that contiguous run (§4.2 allocates into nothing else), so its size
        // is the highest generation and +1 is what would extend it.
        return tokenid::AbilityPoolPluginName(static_cast<int>(g_generations.size()) + 1);
    }

    Usage PoolUsage()
    {
        Usage u;
        for (const int generation : g_generations) {
            u.total += static_cast<int>(g_genCount.at(generation));
        }
        for (const auto& [key, e] : g_entries) {
            // An entry in a generation that is not installed is counted as
            // neither used nor free: it is not occupying anything this build can
            // hand out, and the fail-closed path has already said so.
            if (!GenerationInstalled(key.generation)) {
                continue;
            }
            if (e.tombstone) {
                ++u.tombstoned;
            } else {
                ++u.used;
            }
        }
        u.free = std::max(0, u.total - u.used - u.tombstoned);
        return u;
    }

    RE::SpellItem* AbilityFor(const std::string& a_contentId,
        const std::vector<SourceEffect>& a_effects)
    {
        if (!Ready()) {
            return nullptr;
        }
        if (const auto it = g_byContent.find(a_contentId); it != g_byContent.end()) {
            const auto e = g_entries.find(it->second);
            return (e != g_entries.end() && !e->second.invalid) ? PoolSpell(it->second) : nullptr;
        }
        return Allocate(a_contentId, a_effects, 0);
    }

    void RefreshContent(const std::string& a_contentId)
    {
        if (!Ready()) {
            return;
        }
        const auto it = g_byContent.find(a_contentId);
        if (it == g_byContent.end()) {
            return;  // never allocated; the next SyncToActor builds it fresh
        }
        const PoolKey key = it->second;
        const auto curIt = g_entries.find(key);
        if (curIt == g_entries.end()) {
            return;
        }
        Entry* cur = &curIt->second;

        const auto source = ContentEffectsFor(a_contentId);
        std::vector<RecipeEffect> rebuilt;
        std::string why;
        if (!BuildRecipe(source, rebuilt, why)) {
            // It passes no stats through any more: un-enchanted, its plugin
            // turned off, its per-item toggle turned off. The slot's RECIPE is
            // left exactly as it is and only tombstoned, because a save may
            // still hold an active effect built from it, and that effect has to
            // keep resolving to the same numbers until it is taken off.
            if (!cur->tombstone) {
                cur->tombstone = true;
                g_byContent.erase(a_contentId);
                SaveRegistry();
                SKSE::log::info(
                    "abilities: {} passes no stats through any more ({}) - {} tombstoned",
                    a_contentId, why, ColonIdOf(key));
            }
            return;
        }
        if (EffectsJson(rebuilt) == EffectsJson(cur->effects)) {
            return;  // unchanged, which is the usual answer
        }

        // Changed - a re-enchant, a temper, or simply a different save. The old
        // recipe is NOT edited in place: a save may hold an effect built from
        // it, and rewriting it there would change what an existing bonus is
        // worth with nothing ever told about it (design 5.2).
        //
        // But before taking a new slot, look for one this content has ALREADY
        // had with exactly these effects. A content's value is per-save - it
        // reads the captured original out of THIS save's hidden store - so a
        // player alternating two characters flips between two values, and
        // allocating on every flip would burn the pool on nothing. Matching
        // first bounds the cost at one slot per distinct value the content has
        // ever been worth, which is what the generations were for.
        const auto rebuiltJson = EffectsJson(rebuilt);
        std::uint32_t highest = cur->generation;
        std::optional<PoolKey> reuse;
        for (const auto& [k, e] : g_entries) {
            if (e.content != a_contentId) {
                continue;
            }
            highest = std::max(highest, e.generation);
            if (!reuse && !e.invalid && EffectsJson(e.effects) == rebuiltJson) {
                reuse = k;
            }
        }

        cur->tombstone = true;
        g_byContent.erase(a_contentId);
        if (reuse) {
            auto& back = g_entries.at(*reuse);
            back.tombstone = false;
            g_byContent[a_contentId] = *reuse;
            SaveRegistry();
            SKSE::log::info("abilities: {} is back to the value it had in {} (recipe gen{}) - "
                            "{} tombstoned, no new ability taken",
                a_contentId, ColonIdOf(*reuse), back.generation, ColonIdOf(key));
            return;
        }
        SKSE::log::info("abilities: {} changed - {} tombstoned, taking recipe generation {}",
            a_contentId, ColonIdOf(key), highest + 1);
        Allocate(a_contentId, source, highest + 1);
    }

    void SyncToActor(RE::Actor* a_actor, const std::vector<std::string>& a_wanted)
    {
        if (!a_actor || !Ready()) {
            return;
        }
        std::unordered_set<std::string> want(a_wanted.begin(), a_wanted.end());

        // Allocate for anything wanted that has never had an ability. Only for
        // the ones with no slot: re-deriving a recipe costs a walk of the form
        // table for anything with conditions, and this runs on every equip
        // change. A content whose enchantment actually changed comes through
        // RefreshContent instead.
        for (const auto& c : want) {
            if (!g_byContent.contains(c)) {
                Allocate(c, ContentEffectsFor(c), 0);
            }
        }

        // Converge the WHOLE pool - every slot that HOLDS an entry, not the
        // live content->slot map. g_byContent names only the CURRENT slot for
        // each content, so the moment RefreshContent tombstones a slot and
        // points its content at a new generation, the old slot falls out of
        // this loop and keeps whatever it was granted. Measured 2026-09-14
        // (7.2 #19): slot 2 gen0 stayed GRANTED beside slot 12 gen1 and the
        // actor was paid both, 250 + 333 = 583, and taking the box off could
        // not remove it because nothing ever visited slot 2 again. A stranded
        // modifier, arriving from the one path that MAKES stale slots - which
        // is the exact failure this whole design exists to prevent.
        //
        // Only the abilities that were ever allocated are in the map, so this
        // walks exactly the forms there are - no scan over an empty pool.
        for (const auto& [key, entry] : g_entries) {
            const Entry* e = &entry;
            auto* spell = PoolSpell(key);
            if (!spell) {
                continue;
            }
            // Only the generation the content currently points at pays out.
            // Every older one is revoked, which is how a tombstoned slot comes
            // back off the actor it was granted to.
            const auto live = g_byContent.find(e->content);
            const bool current = live != g_byContent.end() && live->second == key;
            const bool grant =
                current && !e->invalid && !e->tombstone && want.contains(e->content);
            // Not "key": the loop already has one, and it is a PoolKey. This is
            // the human-readable label the grant/revoke log line carries.
            const std::string grantKey =
                current ? "content:" + e->content
                        : std::format("content:{} (stale gen{})", e->content, e->generation);
            if (grant) {
                GrantAbility(a_actor, spell, grantKey);
            } else {
                RevokeAbility(a_actor, spell, grantKey);
            }
        }
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
            if (!BuildRecipe(ContentEffectsFor(id), effects, why)) {
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

        // The console is not open at load, so the log takes the whole thing.
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            SKSE::log::warn("legacy: {}", line);
        }

        // A corner notification was the first try and it was gone before it
        // could be read. This is not a routine "box updated" message - it is
        // the one chance to tell someone that numbers are stuck on their
        // character - so it asks for a click. It fires ONCE per save: the next
        // save written by this build is version 3 and never reports again.
        std::string box = "Costume Expansion FW\n\nThis save was made before 1.6.3. ";
        if (!certainRows.empty() || !possibleRows.empty()) {
            box += "Enchantment bonuses applied by the old\nmechanism could not be removed when "
                   "it loaded, so they are still on your\ncharacter:\n";
            for (const auto& [name, total] : certainRows) {
                box += std::format("\n    {}  {:+g}", name, total);
            }
            for (const auto& [name, total] : possibleRows) {
                box += std::format("\n    {}  {:+g}  (only if its condition was on when you saved)",
                    name, total);
            }
            box += "\n\n";
        } else {
            box += "Nothing is worn right now, so this load added\nnothing - but anything left "
                   "behind by earlier sessions is still there.\n\n";
        }
        box += "Type  cef abilities legacy  in the console for the full explanation and\n"
               "how to take them off. CEF will not do that for you: it cannot tell its\n"
               "own leftovers from a bonus another mod applied on purpose.";

        // Not immediately: the load sequence is still settling and a message box
        // thrown at a fading-in UI is a message box nobody sees.
        RunAfterDelayMs(3000, [box] { RE::DebugMessageBox(box.c_str()); });
    }

    void AbilitiesCommand(RE::TESObjectREFR* a_target, const std::string& a_args)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* target = a_target ? a_target->As<RE::Actor>() : nullptr;
        std::istringstream iss(a_args);
        std::string sub;
        iss >> sub;

        if (sub.empty() || sub == "state" || sub == "usage") {
            const auto u = PoolUsage();
            Print(std::format("[CEF abilities] {} - {} used, {} tombstoned, {} free of {}",
                Ready() ? "ready" : "NOT READY", u.used, u.tombstoned, u.free, u.total));
            // Per generation (PLAN §9.2). The totals above cannot say WHICH
            // plugin is full, which is the only question whose answer is an
            // action - installing the next one.
            for (const int generation : g_generations) {
                int used = 0;
                int tomb = 0;
                for (const auto& [key, e] : g_entries) {
                    if (key.generation != generation) {
                        continue;
                    }
                    (e.tombstone ? tomb : used) += 1;
                }
                const int total = static_cast<int>(g_genCount.at(generation));
                Print(std::format("  {}: {} total / {} allocated / {} free",
                    tokenid::AbilityPoolPluginName(generation), total, used + tomb,
                    total - used - tomb));
            }
            for (const int generation : g_missingGenerations) {
                int affected = 0;
                for (const auto& [key, e] : g_entries) {
                    affected += key.generation == generation ? 1 : 0;
                }
                Print(std::format("  {}: NOT LOADED - {} allocated ability(ies) need it",
                    tokenid::AbilityPoolPluginName(generation), affected));
            }
            if (const auto next = FirstFreeAbility(); next) {
                Print(std::format("  next allocation: {}", ColonIdOf(*next)));
            } else if (!g_generations.empty()) {
                Print("  next allocation: none - every generation is full");
            }
            if (!Ready()) {
                Print(std::format("  {}", g_why));
            }
            return;
        }

        if (sub == "list") {
            int shown = 0;
            for (const auto& [key, e] : g_entries) {
                auto* spell = PoolSpell(key);
                const bool on = spell && player && player->HasSpell(spell);
                const auto line = std::format("  {} {}{}{} {} {} effect(s)  <- {}",
                    ColonIdOf(key), e.tombstone ? "tomb " : "live ", e.invalid ? "INVALID " : "",
                    std::format("recipe gen{}", e.generation), on ? "GRANTED" : "off    ",
                    e.effects.size(), e.content);
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

        if (sub == "restore") {
            namespace fs = std::filesystem;
            // Only when the pool is disabled. Rolling the registry back while it
            // is working would un-allocate slots that live saves already name,
            // and hand them to different content later - the wrong-number case
            // design 5.2 exists to avoid, arrived at through the rescue lever.
            if (Ready()) {
                Print("[CEF abilities] the registry is fine - nothing to restore. This only "
                      "puts the last good copy back when the pool is DISABLED.");
                return;
            }
            const fs::path cur{ kRegistryPath };
            const fs::path bak{ std::string(kRegistryPath) + ".bak1" };
            std::error_code ec;
            if (!fs::exists(bak, ec)) {
                Print(std::format("[CEF abilities] there is no {} to restore from", bak.string()));
                return;
            }
            // Keep whatever is there now. It is the only copy of what went
            // wrong, and someone who restores the wrong thing has no way back.
            ec.clear();
            fs::copy_file(cur, fs::path{ std::string(kRegistryPath) + ".broken" },
                fs::copy_options::overwrite_existing, ec);
            ec.clear();
            fs::copy_file(bak, cur, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                Print(std::format("[CEF abilities] could not restore it: {}", ec.message()));
                SKSE::log::error("abilities: restore failed - {}", ec.message());
                return;
            }
            Print("[CEF abilities] the last good registry is back. RESTART the game - the "
                  "abilities are rebuilt at load, so nothing changes until then.");
            SKSE::log::info("abilities: restored {} from .bak1 (the broken one is kept beside it "
                            "as .broken)", kRegistryPath);
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
            auto* spell = AbilityFor(id, ContentEffectsFor(id));
            Print(spell ? std::format("[CEF abilities] {} -> {:08X}", id, spell->GetFormID())
                        : std::format("[CEF abilities] no ability for {} - see the log", id));
            return;
        }

        if (sub == "npc" || sub == "npcoff") {
            // C7: the same proof on an actor that is not the player. Published
            // costumes are still on the old spells, so there is no other way to
            // put a pool ability on an NPC and watch it survive a restart.
            if (!target) {
                Print("[CEF abilities] click an actor in the console first");
                return;
            }
            std::vector<std::string> wanted;
            if (sub == "npc") {
                std::string id;
                std::getline(iss, id);
                while (!id.empty() && id.front() == ' ') {
                    id.erase(id.begin());
                }
                if (id.find(':') == std::string::npos) {
                    Print("[CEF abilities] usage: cef abilities npc <FormID:Plugin.esp>");
                    return;
                }
                wanted.push_back(id);
            }
            SyncToActor(target, wanted);
            Print(std::format("[CEF abilities] converged {} to {} wanted content(s) - `cef av` on "
                              "the same actor reads the result (wait ~10s)",
                target->GetName(), wanted.size()));
            return;
        }

        Print("[CEF abilities] state | list | alloc <id> | usage | legacy | npc <id> | npcoff");
    }
}
