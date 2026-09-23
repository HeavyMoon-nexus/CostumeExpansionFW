#include "CapturePolicy.h"

#include <cctype>
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
        const auto digits = a_id.substr(0, colon);
        // EVERY character before the colon must be a hex digit, and there must be
        // at least one. std::stoul alone is too permissive here: it consumes the
        // longest valid prefix and stops, so "801junk:Plugin.esp" came back as a
        // clean 0x801 and the junk vanished without a word. The id is a border
        // value (settings file, preset, console, another mod's call), so a
        // malformed one has to be refused rather than silently reinterpreted.
        //
        // What stays accepted, because both are real: a plugin part that is EMPTY
        // (a dynamic 0xFF-range form has no source file), and fewer than 6 digits
        // ("801:Plugin.esp" - FormatColonId's %06X is a minimum width, not a
        // requirement on input).
        if (digits.empty()) {
            return false;
        }
        for (const char c : digits) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        try {
            a_localOut = static_cast<std::uint32_t>(std::stoul(std::string(digits), nullptr, 16));
        } catch (...) {
            return false;  // out_of_range: more digits than a FormID can hold
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

    bool LocalIdInRange(std::uint32_t a_local, bool a_light)
    {
        return a_local <= (a_light ? 0xFFFu : 0xFFFFFFu);
    }

    bool SameColonId(std::string_view a_lhs, std::string_view a_rhs)
    {
        // Canonical ids - every one CEF stores - differ in nothing but case when
        // they name one record, so this settles almost every call without a
        // parse; the parse is for a spelling that slipped in unnormalized.
        if (EqualsCI(a_lhs, a_rhs)) {
            return true;
        }
        std::uint32_t l1 = 0;
        std::uint32_t l2 = 0;
        std::string p1;
        std::string p2;
        return ParseColonId(a_lhs, l1, p1) && ParseColonId(a_rhs, l2, p2) && l1 == l2 &&
               EqualsCI(p1, p2);
    }

    namespace
    {
        // Length of the well-formed sequence starting at a_i (1-4), or 0 when
        // the byte there does not start one. Unicode Table 3-7, row by row.
        std::size_t WellFormedLength(std::string_view a_s, std::size_t a_i)
        {
            const auto at = [&](std::size_t k) {
                return static_cast<unsigned char>(a_s[a_i + k]);
            };
            const auto in = [](unsigned char c, unsigned char lo, unsigned char hi) {
                return c >= lo && c <= hi;
            };
            const std::size_t left = a_s.size() - a_i;
            const unsigned char b0 = at(0);
            if (b0 < 0x80) {
                return 1;
            }
            if (in(b0, 0xC2, 0xDF)) {
                return left >= 2 && in(at(1), 0x80, 0xBF) ? 2 : 0;
            }
            if (in(b0, 0xE0, 0xEF)) {
                if (left < 3) {
                    return 0;
                }
                const unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;  // no overlongs
                const unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;  // no surrogates
                return in(at(1), lo, hi) && in(at(2), 0x80, 0xBF) ? 3 : 0;
            }
            if (in(b0, 0xF0, 0xF4)) {
                if (left < 4) {
                    return 0;
                }
                const unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;  // no overlongs
                const unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;  // nothing past U+10FFFF
                return in(at(1), lo, hi) && in(at(2), 0x80, 0xBF) && in(at(3), 0x80, 0xBF) ? 4
                                                                                           : 0;
            }
            return 0;  // 80-BF (a lone continuation), C0/C1, F5-FF
        }
    }

    bool IsStrictUtf8(std::string_view a_s)
    {
        for (std::size_t i = 0; i < a_s.size();) {
            const std::size_t n = WellFormedLength(a_s, i);
            if (n == 0) {
                return false;
            }
            i += n;
        }
        return true;
    }

    std::size_t Utf8PrefixLength(std::string_view a_s, std::size_t a_maxBytes)
    {
        if (a_s.size() <= a_maxBytes) {
            return a_s.size();
        }
        // Back off while the first byte left OUT is a continuation byte: the
        // character it belongs to started inside the kept part, so it goes too.
        std::size_t n = a_maxBytes;
        while (n > 0 && (static_cast<unsigned char>(a_s[n]) & 0xC0) == 0x80) {
            --n;
        }
        return n;
    }

    std::string EscapeInvalidUtf8(std::string_view a_s)
    {
        std::string out;
        out.reserve(a_s.size());
        for (std::size_t i = 0; i < a_s.size();) {
            const std::size_t n = WellFormedLength(a_s, i);
            if (n > 0) {
                out.append(a_s.substr(i, n));
                i += n;
                continue;
            }
            char esc[8]{};
            std::snprintf(esc, sizeof(esc), "\\x%02X", static_cast<unsigned char>(a_s[i]));
            out += esc;
            ++i;
        }
        return out;
    }
}
