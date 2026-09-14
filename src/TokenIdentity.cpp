#include "TokenIdentity.h"

#include "CapturePolicy.h"  // EqualsCI / PrefixCI / ParseColonId / FormatColonId

#include <cctype>

namespace CostumeFW::tokenid
{
    namespace
    {
        // The numeric suffix between a stem and ".esp", or 0 when the name is not
        // "<stem><digits>.esp". Refuses an empty suffix and a leading zero: one
        // generation must have exactly one spelling, or the continuity check and
        // the diagnostics disagree about which generations are installed.
        int SuffixGeneration(std::string_view a_name, std::string_view a_stem)
        {
            constexpr std::string_view kExt{ ".esp" };
            // Strictly longer, so there is at least one digit between the two.
            if (a_name.size() <= a_stem.size() + kExt.size()) {
                return 0;
            }
            if (!policy::PrefixCI(a_name, a_stem) ||
                !policy::EqualsCI(a_name.substr(a_name.size() - kExt.size()), kExt)) {
                return 0;
            }
            const auto digits =
                a_name.substr(a_stem.size(), a_name.size() - a_stem.size() - kExt.size());
            if (digits.front() == '0') {
                return 0;  // "BoxPool0" / "BoxPool01"
            }
            int value = 0;
            for (const char c : digits) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return 0;
                }
                if (value > 100000) {
                    return 0;  // absurd; refuse rather than overflow
                }
                value = value * 10 + (c - '0');
            }
            return value;
        }

        // The plugin part of a colon-id, or false when the id is malformed.
        bool PluginOf(std::string_view a_id, std::string& a_pluginOut)
        {
            std::uint32_t local = 0;
            return policy::ParseColonId(a_id, local, a_pluginOut);
        }
    }

    bool IsCefPlugin(std::string_view a_name)
    {
        return policy::PrefixCI(a_name, kCefPrefix);
    }

    bool IsBoxTokenPlugin(std::string_view a_name)
    {
        return policy::EqualsCI(a_name, kCorePlugin) || BoxPoolGeneration(a_name) > 0;
    }

    int BoxPoolGeneration(std::string_view a_name)
    {
        return SuffixGeneration(a_name, kBoxPoolStem);
    }

    int AbilityPoolGeneration(std::string_view a_name)
    {
        // Generation 1 shipped in 1.6.3 as CostumeFW_Abilities.esp. The name is
        // published identity now, so it is the ONLY spelling of generation 1 -
        // "Abilities1.esp" is a different file that does not exist.
        if (policy::EqualsCI(a_name, kAbilityGen1Plugin)) {
            return 1;
        }
        const int n = SuffixGeneration(a_name, kAbilityPoolStem);
        return n >= 2 ? n : 0;
    }

    std::string BoxPoolPluginName(int a_generation)
    {
        if (a_generation < 1) {
            return {};
        }
        return std::string(kBoxPoolStem) + std::to_string(a_generation) + ".esp";
    }

    std::string AbilityPoolPluginName(int a_generation)
    {
        if (a_generation < 1) {
            return {};
        }
        if (a_generation == 1) {
            return std::string(kAbilityGen1Plugin);
        }
        return std::string(kAbilityPoolStem) + std::to_string(a_generation) + ".esp";
    }

    std::string CanonicalCefPluginName(std::string_view a_name)
    {
        for (const auto& known : { kCorePlugin, kNpcPlugin, kAbilityGen1Plugin }) {
            if (policy::EqualsCI(a_name, known)) {
                return std::string(known);
            }
        }
        if (const int n = BoxPoolGeneration(a_name); n > 0) {
            return BoxPoolPluginName(n);
        }
        if (const int n = AbilityPoolGeneration(a_name); n > 0) {
            return AbilityPoolPluginName(n);
        }
        return {};  // not one of ours (a dev patch included - it has no fixed casing here)
    }

    bool IsCefColonId(std::string_view a_id)
    {
        std::string plugin;
        return PluginOf(a_id, plugin) && IsCefPlugin(plugin);
    }

    bool IsBoxTokenColonId(std::string_view a_id)
    {
        std::string plugin;
        return PluginOf(a_id, plugin) && IsBoxTokenPlugin(plugin);
    }

    bool CanonicalizeCefColonId(std::string& a_id)
    {
        std::uint32_t local = 0;
        std::string plugin;
        if (!policy::ParseColonId(a_id, local, plugin)) {
            return false;
        }
        std::string canonicalPlugin = CanonicalCefPluginName(plugin);
        if (canonicalPlugin.empty()) {
            // Not a CEF plugin: only the local id's spelling is ours to fix.
            return policy::CanonicalizeColonIdStr(a_id);
        }
        std::string canonical = policy::FormatColonId(local, canonicalPlugin);
        if (canonical == a_id) {
            return false;
        }
        a_id = std::move(canonical);
        return true;
    }
}
