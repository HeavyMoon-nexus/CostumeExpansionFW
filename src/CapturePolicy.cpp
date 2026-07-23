#include "CapturePolicy.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace CostumeFW::policy
{
    const std::vector<std::string>& DefaultBlockNames()
    {
        // MARA (Nexus 173949) ships no plugin - its "CORE Carrier" host armor
        // is a runtime form, so the NAME entry is the effective one (L1
        // already blocks it as a dynamic form; the name is the belt to L1's
        // suspenders and stays correct even if a future MARA persists the
        // carrier differently). Evidence: CEF Nexus report 2026-07-22, MARA
        // bug #1059563 (both: third-party UI touches the carrier -> CTD).
        static const std::vector<std::string> kNames = { "CORE Carrier" };
        return kNames;
    }

    const std::vector<std::string>& DefaultBlockPlugins()
    {
        // Future-proofing for a hypothetical MARA.esp (prefix match).
        static const std::vector<std::string> kPlugins = { "MARA" };
        return kPlugins;
    }

    bool EqualsCI(std::string_view a_lhs, std::string_view a_rhs)
    {
        return a_lhs.size() == a_rhs.size() &&
               (a_lhs.empty() || ::_strnicmp(a_lhs.data(), a_rhs.data(), a_lhs.size()) == 0);
    }

    bool PrefixCI(std::string_view a_str, std::string_view a_prefix)
    {
        return !a_prefix.empty() && a_str.size() >= a_prefix.size() &&
               ::_strnicmp(a_str.data(), a_prefix.data(), a_prefix.size()) == 0;
    }

    bool NameMatches(std::string_view a_name, std::string_view a_pattern)
    {
        if (!a_pattern.empty() && a_pattern.back() == '*') {
            return PrefixCI(a_name, a_pattern.substr(0, a_pattern.size() - 1));
        }
        return EqualsCI(a_name, a_pattern);
    }

    bool NameDenied(const CapturePolicy& a_policy, std::string_view a_name)
    {
        if (a_name.empty()) {
            return false;
        }
        if (!a_policy.disableDefaults) {
            for (const auto& pattern : DefaultBlockNames()) {
                if (NameMatches(a_name, pattern)) {
                    return true;
                }
            }
        }
        for (const auto& pattern : a_policy.names) {
            if (NameMatches(a_name, pattern)) {
                return true;
            }
        }
        return false;
    }

    bool PluginDenied(const CapturePolicy& a_policy, std::string_view a_filename)
    {
        if (a_filename.empty()) {
            return false;
        }
        if (!a_policy.disableDefaults) {
            for (const auto& prefix : DefaultBlockPlugins()) {
                if (PrefixCI(a_filename, prefix)) {
                    return true;
                }
            }
        }
        for (const auto& prefix : a_policy.plugins) {
            if (PrefixCI(a_filename, prefix)) {
                return true;
            }
        }
        return false;
    }

    bool IdDenied(const CapturePolicy& a_policy, std::string_view a_canonicalId)
    {
        for (const auto& entry : a_policy.ids) {
            if (EqualsCI(a_canonicalId, entry)) {
                return true;
            }
        }
        return false;
    }

    bool ParseColonId(std::string_view a_id, std::uint32_t& a_localOut, std::string& a_pluginOut)
    {
        const auto colon = a_id.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        try {
            a_localOut = static_cast<std::uint32_t>(
                std::stoul(std::string(a_id.substr(0, colon)), nullptr, 16));
        } catch (...) {
            return false;
        }
        a_pluginOut = std::string(a_id.substr(colon + 1));
        return true;
    }

    std::string FormatColonId(std::uint32_t a_local, std::string_view a_plugin)
    {
        // buf sized for 8-digit values: %06X is a minimum width, and the
        // pre-1.3.2 char[8] silently truncated 0xFF-range ids.
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "%06X", a_local);
        std::string out(buf);
        out += ':';
        out += a_plugin;
        return out;
    }

    bool CanonicalizeColonIdStr(std::string& a_id)
    {
        std::uint32_t local = 0;
        std::string plugin;
        if (!ParseColonId(a_id, local, plugin)) {
            return false;  // unparseable - leave it for the resolver to reject
        }
        std::string canonical = FormatColonId(local, plugin);
        if (canonical == a_id) {
            return false;
        }
        a_id = std::move(canonical);
        return true;
    }
}
