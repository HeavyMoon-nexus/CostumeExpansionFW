// SPIKE ONLY - see AbilityPool.h. Throwaway code; measured, then deleted.

#include "AbilityPool.h"

#include "ConsoleOut.h"

#include "RE/A/Actor.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/S/SpellItem.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESObjectREFR.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace CostumeFW::pool
{
    namespace
    {
        void Print(std::string_view a_msg) { ConsolePrint(a_msg); }

        constexpr const char* kPoolPlugin = "CEFTest_AbilityPool.esp";
        constexpr std::uint32_t kPoolBase = 0x800;
        constexpr int kPoolSize = 8;

        // The registry lives OUTSIDE the save, on purpose. That is the one idea
        // in the pool design that makes the ordering possible at all: the shape
        // a save's ActiveEffect expects has to be known BEFORE that save is
        // read, so it cannot itself come out of the save. Production keeps a
        // serialized recipe here (design §4); the spike keeps a pointer to a
        // source enchantment and clones it, which measures the same thing
        // without having to settle condition serialization first.
        constexpr const char* kRegistryPath = "Data\\SKSE\\Plugins\\CEF_pooltest.json";

        struct Recipe
        {
            std::string source;   // "XXXXXX:Plugin.esp" of an ENCH to clone
            bool early{ true };   // hydrate at kDataLoaded, or only on command
        };

        std::array<std::optional<Recipe>, kPoolSize> g_slots{};

        // --- forms ------------------------------------------------------------

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
            auto* form = LookupColon(
                std::format("{:06X}:{}", kPoolBase + static_cast<std::uint32_t>(a_slot), kPoolPlugin));
            return form ? form->As<RE::SpellItem>() : nullptr;
        }

        // Deep-copy a condition chain: ~TESCondition deletes the whole chain, so
        // sharing nodes with the source form would hand the engine a double free.
        // Node data is plain; the param FORM pointers stay shared - forms outlive
        // spells. Same rule as BoxStore's copy.
        void CopyConditions(RE::TESCondition& a_dst, const RE::TESCondition& a_src)
        {
            RE::TESConditionItem** tail = &a_dst.head;
            for (auto* cur = a_src.head; cur; cur = cur->next) {
                auto* node = new RE::TESConditionItem();
                node->data = cur->data;
                node->next = nullptr;
                *tail = node;
                tail = &node->next;
            }
        }

        // --- registry ---------------------------------------------------------

        void LoadRegistry()
        {
            g_slots = {};
            std::ifstream in(kRegistryPath);
            if (!in) {
                return;
            }
            nlohmann::json doc;
            try {
                in >> doc;
            } catch (const std::exception& e) {
                SKSE::log::error("pool: registry unreadable ({}) - no slots restored", e.what());
                return;
            }
            if (!doc.contains("slots") || !doc["slots"].is_object()) {
                return;
            }
            for (auto it = doc["slots"].begin(); it != doc["slots"].end(); ++it) {
                int slot = -1;
                try {
                    slot = std::stoi(it.key());
                } catch (...) {
                    continue;
                }
                if (slot < 0 || slot >= kPoolSize || !it.value().is_object()) {
                    continue;
                }
                Recipe r;
                r.source = it.value().value("source", std::string{});
                r.early = it.value().value("early", true);
                if (!r.source.empty()) {
                    g_slots[static_cast<std::size_t>(slot)] = r;
                }
            }
        }

        void SaveRegistry()
        {
            nlohmann::json doc;
            doc["slots"] = nlohmann::json::object();
            for (int i = 0; i < kPoolSize; ++i) {
                const auto& s = g_slots[static_cast<std::size_t>(i)];
                if (!s) {
                    continue;
                }
                nlohmann::json j;
                j["source"] = s->source;
                j["early"] = s->early;
                doc["slots"][std::to_string(i)] = j;
            }
            std::error_code ec;
            std::filesystem::create_directories(
                std::filesystem::path(kRegistryPath).parent_path(), ec);
            std::ofstream out(kRegistryPath, std::ios::trunc);
            if (!out) {
                SKSE::log::error("pool: could not write {}", kRegistryPath);
                return;
            }
            out << doc.dump(2);
        }

        // --- the ability itself -----------------------------------------------

        // Take it off and PROVE it came off before touching the effect list.
        // Rewriting the list under a live ability is what strands a modifier:
        // the engine holds pointers to the Effects the active effect was built
        // from, and nothing ever runs to undo what they applied. This is design
        // §5.4, and it is also the confound that cost a measurement in the token
        // spike - so it is in from the first line here.
        bool EnsureOff(RE::SpellItem* a_spell, const char* a_why)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !a_spell || !player->HasSpell(a_spell)) {
                return true;
            }
            player->RemoveSpell(a_spell);
            if (player->HasSpell(a_spell)) {
                SKSE::log::error("pool: RemoveSpell did NOT take {:08X} off ({}) - refusing to "
                                 "touch its effects, the measurement would be worthless",
                    a_spell->GetFormID(), a_why);
                return false;
            }
            return true;
        }

        // Fill a pool spell from its recipe. The old Effect objects are
        // deliberately leaked rather than deleted: the engine may still hold
        // pointers into them, and this code is thrown away anyway.
        bool Hydrate(int a_slot, const char* a_why)
        {
            const auto& rec = g_slots[static_cast<std::size_t>(a_slot)];
            if (!rec) {
                return false;
            }
            auto* spell = PoolSpell(a_slot);
            if (!spell) {
                SKSE::log::error("pool: slot {} - {} does not resolve. Is {} enabled?", a_slot,
                    std::format("{:06X}", kPoolBase + static_cast<std::uint32_t>(a_slot)),
                    kPoolPlugin);
                return false;
            }
            if (!EnsureOff(spell, a_why)) {
                return false;
            }
            auto* src = LookupColon(rec->source);
            auto* ench = src ? src->As<RE::EnchantmentItem>() : nullptr;
            if (!ench) {
                SKSE::log::error("pool: slot {} source '{}' is not a resolvable enchantment",
                    a_slot, rec->source);
                return false;
            }

            spell->effects.clear();
            int conditioned = 0;
            for (auto* e : ench->effects) {
                if (!e || !e->baseEffect) {
                    continue;
                }
                auto* eff = new RE::Effect();
                eff->baseEffect = e->baseEffect;
                eff->effectItem = e->effectItem;
                eff->cost = e->cost;
                CopyConditions(eff->conditions, e->conditions);
                if (eff->conditions.head) {
                    ++conditioned;
                }
                spell->effects.push_back(eff);
            }
            SKSE::log::info("pool: slot {} ({:08X}) hydrated {} [{}] - {} effect(s), {} "
                            "conditioned, from {}",
                a_slot, spell->GetFormID(), a_why, rec->early ? "early" : "late",
                spell->effects.size(), conditioned, rec->source);
            return true;
        }

        std::string Describe(int a_slot)
        {
            const auto& rec = g_slots[static_cast<std::size_t>(a_slot)];
            auto* spell = PoolSpell(a_slot);
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool on = spell && player && player->HasSpell(spell);
            if (!spell) {
                return std::format("  {} : SPELL DOES NOT RESOLVE (is {} enabled?)", a_slot,
                    kPoolPlugin);
            }
            if (!rec) {
                return std::format("  {} : {:08X}  empty", a_slot, spell->GetFormID());
            }
            int conditioned = 0;
            for (auto* e : spell->effects) {
                if (e && e->conditions.head) {
                    ++conditioned;
                }
            }
            return std::format("  {} : {:08X}  {}  {} effect(s), {} conditioned  {}  <- {}",
                a_slot, spell->GetFormID(), rec->early ? "early" : "late ", spell->effects.size(),
                conditioned, on ? "GRANTED" : "off    ", rec->source);
        }
    }

    void HydrateAtDataLoaded()
    {
        LoadRegistry();
        int early = 0;
        int late = 0;
        int failed = 0;
        for (int i = 0; i < kPoolSize; ++i) {
            const auto& rec = g_slots[static_cast<std::size_t>(i)];
            if (!rec) {
                continue;
            }
            if (!rec->early) {
                ++late;
                continue;
            }
            if (Hydrate(i, "kDataLoaded")) {
                ++early;
            } else {
                ++failed;
            }
        }
        if (early || late || failed) {
            // This line is the experiment's timestamp. It has to appear BEFORE
            // the save-load lines in the log, or the early/late distinction the
            // whole measurement rests on did not actually happen.
            SKSE::log::info("pool: kDataLoaded - {} slot(s) hydrated early, {} left for the "
                            "console (late), {} failed",
                early, late, failed);
        }
    }

    void PoolCommand(RE::TESObjectREFR*, const std::string& a_args)
    {
        std::istringstream iss(a_args);
        std::string sub;
        iss >> sub;

        if (sub.empty() || sub == "list") {
            LoadRegistry();
            Print(std::format("[CEF pool] {} slots ({})", kPoolSize, kPoolPlugin));
            for (int i = 0; i < kPoolSize; ++i) {
                Print(Describe(i));
                SKSE::log::info("pool:{}", Describe(i));
            }
            return;
        }

        if (sub == "clear") {
            LoadRegistry();
            for (int i = 0; i < kPoolSize; ++i) {
                if (auto* spell = PoolSpell(i)) {
                    EnsureOff(spell, "clear");
                }
            }
            g_slots = {};
            SaveRegistry();
            Print("[CEF pool] every slot released and the registry emptied");
            return;
        }

        int slot = -1;
        {
            std::string slotArg;
            iss >> slotArg;
            try {
                slot = std::stoi(slotArg);
            } catch (...) {
                slot = -1;
            }
        }
        if (slot < 0 || slot >= kPoolSize) {
            Print(std::format("[CEF pool] usage: cef pool [list | clear] | "
                              "cef pool <set|on|off|hydrate> <0-{}> [args]",
                kPoolSize - 1));
            return;
        }

        LoadRegistry();

        if (sub == "set") {
            std::string source;
            std::string when;
            iss >> source >> when;
            if (source.find(':') == std::string::npos) {
                Print("[CEF pool] usage: cef pool set <slot> <FormID:Plugin.esp of an ENCH> [late]");
                return;
            }
            Recipe r;
            r.source = source;
            r.early = (when != "late");
            g_slots[static_cast<std::size_t>(slot)] = r;
            SaveRegistry();
            if (Hydrate(slot, "set")) {
                Print(std::format("[CEF pool] slot {} = {} ({}) - hydrated now and saved to the "
                                  "registry",
                    slot, source, r.early ? "early" : "late"));
                Print(Describe(slot));
            } else {
                Print(std::format("[CEF pool] slot {} recorded, but hydration FAILED - see the log",
                    slot));
            }
            return;
        }

        if (sub == "hydrate") {
            Print(Hydrate(slot, "console")
                      ? std::format("[CEF pool] slot {} refilled", slot)
                      : std::format("[CEF pool] slot {} could not be refilled - see the log", slot));
            return;
        }

        auto* spell = PoolSpell(slot);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!spell || !player) {
            Print(std::format("[CEF pool] slot {} does not resolve (is {} enabled?)", slot,
                kPoolPlugin));
            return;
        }

        if (sub == "on") {
            if (spell->effects.empty()) {
                Print(std::format("[CEF pool] slot {} is empty - `cef pool set` it first. An empty "
                                  "ability grants nothing and measures nothing",
                    slot));
                return;
            }
            if (player->HasSpell(spell)) {
                Print(std::format("[CEF pool] slot {} is already granted", slot));
                return;
            }
            player->AddSpell(spell);
            // Verify, the same way `off` does. Logging "granted" straight after
            // AddSpell records that the call was MADE, not that it took - and a
            // measurement built on that is worth nothing. The off path had this
            // check from the start and the on path did not, which is the same
            // asymmetry the design document has to avoid (§5.4).
            if (!player->HasSpell(spell)) {
                Print(std::format("[CEF pool] slot {} did NOT go on - AddSpell was refused", slot));
                SKSE::log::error("pool: AddSpell did NOT take slot {} ({:08X})", slot,
                    spell->GetFormID());
                return;
            }
            SKSE::log::info("pool: granted slot {} ({:08X}), {} effect(s)", slot,
                spell->GetFormID(), spell->effects.size());
            // A constant-effect ability lands a few seconds AFTER AddSpell, so an
            // immediate `cef av` reads a clean zero and looks like a failure. That
            // cost a false read on 2026-09-13.
            Print(std::format("[CEF pool] slot {} granted - wait ~10s before `cef av`, the "
                              "effect does not land instantly",
                slot));
            Print(Describe(slot));
            return;
        }

        if (sub == "off") {
            if (!player->HasSpell(spell)) {
                Print(std::format("[CEF pool] slot {} was not granted", slot));
                return;
            }
            player->RemoveSpell(spell);
            if (player->HasSpell(spell)) {
                Print(std::format("[CEF pool] slot {} REFUSED to come off - that is a result, "
                                  "write it down",
                    slot));
                SKSE::log::error("pool: RemoveSpell did NOT take slot {} ({:08X}) off", slot,
                    spell->GetFormID());
            } else {
                Print(std::format("[CEF pool] slot {} removed - now run `cef av`", slot));
                SKSE::log::info("pool: removed slot {} ({:08X})", slot, spell->GetFormID());
            }
            return;
        }

        Print("[CEF pool] list | set <slot> <ENCH id> [late] | on <slot> | off <slot> | "
              "hydrate <slot> | clear");
    }
}
