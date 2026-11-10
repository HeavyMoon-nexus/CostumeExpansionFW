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
        std::vector<std::string> plugins;  // source-plugin filename PREFIX (CI) - the
                                           // user's; the shipped ones are exact (W7)
        std::vector<std::string> ids;      // canonical colon-ids (CI)
        bool allowNonPlayable{ false };    // soft switch: show non-playable armors
        bool disableDefaults{ false };     // soft switch: shipped deny-list off
    };

    // Shipped L2 defaults, honored unless disableDefaults. Kept beside the
    // matcher so code and data cannot drift.
    const std::vector<std::string>& DefaultBlockNames();    // { "CORE Carrier" }
    // { "MARA.esp", "MARA.esm", "MARA.esl" } - whole names, matched exactly.
    const std::vector<std::string>& DefaultBlockPlugins();

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

    // Can a_local name a record of a plugin at all? A light (ESL) plugin holds
    // local ids up to FFF, a full one up to FFFFFF. The engine's own FormID
    // arithmetic ADDS the local id without masking it, so an id past the end
    // does not fail - it lands in a DIFFERENT plugin: 012A45 saved against an
    // ESP that was later ESL-flagged (ESLify, a mod update) points 0x12 light
    // slots further along, at somebody else's record (review 2026-09-23 C11).
    [[nodiscard]] bool LocalIdInRange(std::uint32_t a_local, bool a_light);

    // Whether two colon-ids name the same record: the same local id as a NUMBER
    // ("801" and "000801" are one id) in the same plugin, compared without case
    // because Windows file names are. "012345:MyMod.esp" and "012345:mymod.esp"
    // were two contents to every ownership check, so a preset could put an item
    // a box already held into a second box (review 2026-09-23 W26). False when
    // either side does not parse and the two strings differ.
    [[nodiscard]] bool SameColonId(std::string_view a_lhs, std::string_view a_rhs);

    // --- UTF-8 (review 2026-09-23 C1) ----------------------------------------
    // Everything CEF persists is JSON, and nlohmann's dump() THROWS on text that
    // is not valid UTF-8 rather than writing it. Plugin names, mesh paths and
    // NIF shape names are raw bytes in whatever codepage their author used, so
    // these are the checks that keep such text out of the files.

    // Well-formed per Unicode Table 3-7 - the exact set dump() accepts. Refuses
    // the overlong lead bytes C0/C1, overlong E0 80-9F and F0 80-8F, the
    // surrogate range ED A0-BF, anything past U+10FFFF (F4 90+ and F5+), lone
    // continuation bytes and a sequence cut short.
    [[nodiscard]] bool IsStrictUtf8(std::string_view a_s);
    // The longest prefix of a_s, at most a_maxBytes long, that does not end in
    // the middle of a multi-byte sequence - for cutting text to fit a fixed
    // buffer without leaving half a character at the end.
    [[nodiscard]] std::size_t Utf8PrefixLength(std::string_view a_s, std::size_t a_maxBytes);
    // a_s with every byte that is not part of a well-formed sequence written as
    // the four characters \xNN (upper-case hex). Well-formed text comes back
    // unchanged, and the result is always well-formed. Deterministic, so two
    // different byte strings never share an escape.
    [[nodiscard]] std::string EscapeInvalidUtf8(std::string_view a_s);

    // --- Handing a captured item back (review 2026-09-23 C2 / C3) -------------
    // What one return request comes to. The whole "may CEF mint a copy" rule is
    // this one function, so every path that returns items follows it and the
    // host tests pin the table.
    enum class ReturnAction
    {
        kFromStore,  // the captured original comes back out of the store
        kRecreate,   // a plain copy is minted - the store has nothing to return
        kNothing,    // nothing to hand back, and minting a copy would duplicate
    };
    // a_inStore     this save's store holds the item
    // a_fabricate   the caller allows a plain copy when the store misses
    // a_refOnly     the id is only LISTED - a preset named it, CEF never held it
    // a_uninstallContext / a_uninstallRan
    //               an uninstall pass is asking, and one already ran on this save
    [[nodiscard]] ReturnAction DecideReturn(bool a_inStore, bool a_fabricate, bool a_refOnly,
        bool a_uninstallContext, bool a_uninstallRan);
}
