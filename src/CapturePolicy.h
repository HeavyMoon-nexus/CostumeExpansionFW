#pragma once

// Capture-blacklist policy - PURE std-only module (no RE/SKSE types) so the
// string layers are host-unit-testable (tests/policy_tests.cpp builds this
// file alone). Form-level layers (dynamic-form, record flags, keywords) stay
// in BoxStore.cpp where RE is available.
//
// v1.3.2 review r2 (MARA_GUARD_ADVERSARIAL_REVIEW.md):
// - P1-5: the live policy is published as an IMMUTABLE snapshot
//   (std::atomic<std::shared_ptr<const CapturePolicy>> in BoxStore.cpp).
//   Readers take ONE snapshot per operation (a whole picker enumeration uses
//   the same generation); mutators copy-and-publish. No shared vector is
//   ever mutated in place while another thread may read it.
// - P1-4: there is deliberately NO "allowDynamic" switch. Runtime/no-file
//   forms are a HARD invariant: TESForm::GetLocalFormID() dereferences
//   GetFile(0) unchecked (the mechanism behind the original "+ Add worn
//   item" CTD), and no dynamic form can survive CEF's colon-id persistence -
//   there is no legitimate capture use, so the skip must not be liftable
//   from UI or settings.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CostumeFW::policy
{
    struct CapturePolicy
    {
        std::vector<std::string> names;    // exact, or trailing '*' = prefix (CI)
        std::vector<std::string> plugins;  // source-plugin filename prefix (CI)
        std::vector<std::string> ids;      // canonical colon-ids (CI)
        bool allowNonPlayable{ false };    // soft switch: show non-playable armors
        bool disableDefaults{ false };     // soft switch: shipped deny-list off
    };

    // Shipped L2 defaults, honored unless disableDefaults. Kept beside the
    // matcher so code and data cannot drift.
    const std::vector<std::string>& DefaultBlockNames();    // { "CORE Carrier" }
    const std::vector<std::string>& DefaultBlockPlugins();  // { "MARA" }

    // Case-insensitive string primitives (ASCII fold - plugin filenames and
    // the shipped names are ASCII; user entries match byte-CI like the rest
    // of CEF's name handling).
    [[nodiscard]] bool EqualsCI(std::string_view a_lhs, std::string_view a_rhs);
    [[nodiscard]] bool PrefixCI(std::string_view a_str, std::string_view a_prefix);
    // Name pattern: exact (CI), or trailing '*' = prefix match.
    [[nodiscard]] bool NameMatches(std::string_view a_name, std::string_view a_pattern);

    [[nodiscard]] bool NameDenied(const CapturePolicy& a_policy, std::string_view a_name);
    [[nodiscard]] bool PluginDenied(const CapturePolicy& a_policy, std::string_view a_filename);
    [[nodiscard]] bool IdDenied(const CapturePolicy& a_policy, std::string_view a_canonicalId);

    // Colon-id "XXXXXX:Plugin.esp" pure helpers - the single source of truth
    // for parse/format (SkinRebind's ParseColonId/CanonicalizeColonId and
    // BoxStore's MakeColonId delegate here). Semantics preserved from the
    // original: hex local id before the colon (stoul base 16), plugin = the
    // raw remainder (an EMPTY plugin parses - the resolver rejects it later).
    [[nodiscard]] bool ParseColonId(std::string_view a_id, std::uint32_t& a_localOut,
        std::string& a_pluginOut);
    // %06X is a MINIMUM width: 6 digits for normal local ids (byte-identical
    // to the historic format), 8 digits for 0xFF-range values - never
    // truncated (the pre-1.3.2 char[8] truncation bug).
    [[nodiscard]] std::string FormatColonId(std::uint32_t a_local, std::string_view a_plugin);
    // Rewrites a_id into canonical form; true if it changed.
    bool CanonicalizeColonIdStr(std::string& a_id);
}
