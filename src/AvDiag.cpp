#include "AvDiag.h"

#include "ConsoleOut.h"

#include "RE/A/Actor.h"
#include "RE/A/ActorValueInfo.h"
#include "RE/A/ActorValueList.h"
#include "RE/A/ActorValues.h"
#include "RE/P/PlayerCharacter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        void Print(std::string_view a_msg) { ConsolePrint(a_msg); }

        constexpr std::uint32_t kAvCount =
            static_cast<std::uint32_t>(RE::ActorValue::kTotal);

        struct AvReading
        {
            float perm{ 0.0f };
            float temp{ 0.0f };
            float damage{ 0.0f };
            float current{ 0.0f };
            float base{ 0.0f };

            bool Modified() const { return perm != 0.0f || temp != 0.0f || damage != 0.0f; }
        };

        // Baselines are per actor: `cef av base` on the player and then `diff`
        // on a follower would otherwise subtract one actor's numbers from
        // another's and report a difference that is really just two people.
        std::unordered_map<std::uint32_t, std::vector<AvReading>> g_baseline;

        AvReading Read(RE::Actor* a_actor, RE::ActorValue a_av)
        {
            AvReading r;
            r.perm = a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIERS::kPermanent, a_av);
            r.temp = a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIERS::kTemporary, a_av);
            r.damage = a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIERS::kDamage, a_av);
            if (auto* owner = a_actor->AsActorValueOwner()) {
                r.current = owner->GetActorValue(a_av);
                r.base = owner->GetBaseActorValue(a_av);
            }
            return r;
        }

        std::vector<AvReading> ReadAll(RE::Actor* a_actor)
        {
            std::vector<AvReading> out(kAvCount);
            for (std::uint32_t i = 0; i < kAvCount; ++i) {
                out[i] = Read(a_actor, static_cast<RE::ActorValue>(i));
            }
            return out;
        }

        std::string AvName(std::uint32_t a_index)
        {
            auto* list = RE::ActorValueList::GetSingleton();
            auto* info = list ? list->GetActorValue(static_cast<RE::ActorValue>(a_index)) : nullptr;
            if (info && info->enumName && *info->enumName) {
                return info->enumName;
            }
            return std::format("av{}", a_index);
        }

        // "350" rather than "350.0", but "22.5" kept - a fortify is usually a
        // whole number and the extra ".0" on forty of them is just noise.
        std::string Num(float a_value)
        {
            if (a_value == static_cast<float>(static_cast<long long>(a_value))) {
                return std::format("{}", static_cast<long long>(a_value));
            }
            return std::format("{:.2f}", a_value);
        }

        std::string Signed(float a_value)
        {
            return (a_value > 0.0f ? "+" : "") + Num(a_value);
        }

        std::string Line(std::uint32_t a_index, const AvReading& a_r)
        {
            std::string s = std::format("  {:<20} perm {:>8}", AvName(a_index), Signed(a_r.perm));
            if (a_r.temp != 0.0f) {
                s += std::format("  temp {:>8}", Signed(a_r.temp));
            }
            if (a_r.damage != 0.0f) {
                s += std::format("  dmg {:>8}", Signed(a_r.damage));
            }
            s += std::format("   (now {})", Num(a_r.current));
            return s;
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        std::string ActorLabel(RE::Actor* a_actor)
        {
            const char* nm = a_actor->GetName();
            return std::format("{} ({:08X})", (nm && *nm) ? nm : "<unnamed>", a_actor->GetFormID());
        }

        // Both halves of every report go to the log too. A screenshot of the
        // console loses the scrollback, and these numbers are the evidence in a
        // stat-passthrough report.
        void Emit(const std::string& a_line)
        {
            Print(a_line);
            SKSE::log::info("av: {}", a_line);
        }
    }

    void AvReport(RE::Actor* a_actor, const std::string& a_arg)
    {
        auto* actor = a_actor ? a_actor : RE::PlayerCharacter::GetSingleton();
        if (!actor) {
            Print("[CEF] av: no actor");
            return;
        }
        const std::string arg = Lower(a_arg);
        const auto now = ReadAll(actor);

        if (arg == "base") {
            g_baseline[actor->GetFormID()] = now;
            int modified = 0;
            for (const auto& r : now) {
                modified += r.Modified() ? 1 : 0;
            }
            Emit(std::format("[CEF] av: baseline taken for {} - {} value(s) already modified",
                ActorLabel(actor), modified));
            return;
        }

        if (arg == "diff") {
            const auto it = g_baseline.find(actor->GetFormID());
            if (it == g_baseline.end()) {
                Print("[CEF] av: no baseline for this actor - run `cef av base` first");
                return;
            }
            Emit(std::format("[CEF] av diff for {}:", ActorLabel(actor)));
            int changed = 0;
            for (std::uint32_t i = 0; i < kAvCount; ++i) {
                const float dPerm = now[i].perm - it->second[i].perm;
                const float dTemp = now[i].temp - it->second[i].temp;
                const float dDmg = now[i].damage - it->second[i].damage;
                if (dPerm == 0.0f && dTemp == 0.0f && dDmg == 0.0f) {
                    continue;
                }
                ++changed;
                std::string s = std::format("  {:<20} perm {:>8}", AvName(i), Signed(dPerm));
                if (dTemp != 0.0f) {
                    s += std::format("  temp {:>8}", Signed(dTemp));
                }
                if (dDmg != 0.0f) {
                    s += std::format("  dmg {:>8}", Signed(dDmg));
                }
                Emit(s);
            }
            if (changed == 0) {
                Emit("  (no change)");
            }
            return;
        }

        if (!arg.empty() && arg != "all") {
            auto* list = RE::ActorValueList::GetSingleton();
            const auto av = list ? list->LookupActorValueByName(arg) : RE::ActorValue::kNone;
            if (av == RE::ActorValue::kNone) {
                // The console spelling is the trap this command exists for, so
                // say what WAS found rather than just "unknown".
                Print(std::format("[CEF] av: '{}' is not an actor value name - "
                                  "try `cef av` to see what is actually modified",
                    a_arg));
                return;
            }
            const auto i = static_cast<std::uint32_t>(av);
            Emit(std::format("[CEF] av for {}:", ActorLabel(actor)));
            Emit(Line(i, now[i]) + std::format("   base {}", Num(now[i].base)));
            return;
        }

        const bool all = (arg == "all");
        Emit(std::format("[CEF] av for {}{}:", ActorLabel(actor),
            all ? "" : " (modified only)"));
        int shown = 0;
        for (std::uint32_t i = 0; i < kAvCount; ++i) {
            if (!all && !now[i].Modified()) {
                continue;
            }
            Emit(Line(i, now[i]));
            ++shown;
        }
        if (shown == 0) {
            Emit("  (nothing modified - every actor value is at its base)");
        }
    }

    std::string AvConsoleName(std::uint32_t a_index) { return AvName(a_index); }
}
