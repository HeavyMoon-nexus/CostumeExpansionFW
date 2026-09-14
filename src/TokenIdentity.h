#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Who owns a form, decided from its plugin name and nothing else.
//
// This is the string layer of the v1.6.4 token identity rules, deliberately kept
// free of RE/CommonLibSSE so it links into the host-side policy_tests and the
// rules can be pinned by a test rather than by a comment. The parts that need
// the game - does the form exist, is it an ARMO, which file DEFINED it
// (GetFile(0)), what is the loaded file's real casing - live in BoxStore.
//
// Two predicate families, and the difference between them is load-bearing:
//
//   BROAD  (IsCefPlugin / IsCefColonId)
//     "Is this any CEF record?" A 9-character prefix match on "CostumeFW".
//     Used to keep CEF's own records OUT of the capture pickers and out of box
//     CONTENTS. A publish token must never become content either, so this side
//     must stay broad. Narrowing it is the single most likely way to break this
//     file (TH13 exists to catch exactly that).
//
//   NARROW (IsBoxTokenPlugin / IsBoxTokenColonId)
//     "Does this plugin DEFINE box tokens?" An explicit allowlist, not a prefix.
//     Used to decide the box token pool and to validate a box definition's
//     token. CostumeFW_NPC.esp passes the broad test and must fail this one -
//     it holds the publish tokens and the NPC persist carriers.
//
// Membership is never decided by a display name. A token's FULL is rewritten at
// runtime (a box's label is stamped onto it) and can be translated by an
// xTranslator'd copy of the plugin, so a name test decides nothing reliable.
namespace CostumeFW::tokenid
{
    // CEF's own plugin names, in their official casing. A pool generation is not
    // listed here - it is recognised by BoxPoolGeneration/AbilityPoolGeneration.
    inline constexpr std::string_view kCefPrefix{ "CostumeFW" };
    inline constexpr std::string_view kCorePlugin{ "CostumeFW.esp" };
    inline constexpr std::string_view kNpcPlugin{ "CostumeFW_NPC.esp" };
    inline constexpr std::string_view kAbilityGen1Plugin{ "CostumeFW_Abilities.esp" };
    inline constexpr std::string_view kBoxPoolStem{ "CostumeFW_BoxPool" };
    inline constexpr std::string_view kAbilityPoolStem{ "CostumeFW_Abilities" };

    // --- plugin names --------------------------------------------------------

    // BROAD. Any plugin whose name starts "CostumeFW" (case-insensitive): the
    // core, the NPC add-on, every pool generation, and a not-yet-folded dev patch.
    [[nodiscard]] bool IsCefPlugin(std::string_view a_name);

    // NARROW. A plugin that DEFINES box tokens: the core (generation 0) or a
    // CostumeFW_BoxPoolN.esp. Nothing else - adding CostumeFW_NPC.esp here is the
    // defect this whole layer exists to prevent, and TH14 fails if it appears.
    [[nodiscard]] bool IsBoxTokenPlugin(std::string_view a_name);

    // Generation number parsed out of a pool plugin name, 0 when the name is not
    // one. The suffix is read as a NUMBER, so BoxPool10 sorts after BoxPool2, and
    // a leading zero is refused so one generation cannot have two spellings
    // ("BoxPool01" is not generation 1).
    //   CostumeFW_BoxPool1.esp -> 1,  CostumeFW_BoxPool10.esp -> 10
    //   CostumeFW_BoxPool.esp / BoxPool0.esp / BoxPool01.esp -> 0
    [[nodiscard]] int BoxPoolGeneration(std::string_view a_name);

    // Same for the ability pool, where generation 1 carries an exception name: it
    // shipped in 1.6.3 as CostumeFW_Abilities.esp and cannot be renamed, so
    // "Abilities1.esp" is NOT a valid spelling of it.
    //   CostumeFW_Abilities.esp -> 1,  CostumeFW_Abilities2.esp -> 2
    //   CostumeFW_Abilities1.esp -> 0
    [[nodiscard]] int AbilityPoolGeneration(std::string_view a_name);

    // The official casing of a CEF plugin name given in any casing; empty when
    // the name is not CEF's. MO2 preserves the casing a mod author typed, so a
    // hand-edited settings file can carry "costumefw.esp" - which resolves to the
    // same form but is a different string, and the box store keys on the string.
    [[nodiscard]] std::string CanonicalCefPluginName(std::string_view a_name);

    // The official file name for a generation (empty when a_generation < 1).
    [[nodiscard]] std::string BoxPoolPluginName(int a_generation);
    [[nodiscard]] std::string AbilityPoolPluginName(int a_generation);

    // --- colon-ids ("XXXXXX:Plugin.esp") -------------------------------------
    // Both parse strictly (policy::ParseColonId): every character before the
    // colon must be a hex digit. A malformed id is not a CEF id.

    [[nodiscard]] bool IsCefColonId(std::string_view a_id);       // BROAD
    [[nodiscard]] bool IsBoxTokenColonId(std::string_view a_id);  // NARROW

    // Rewrite a CEF colon-id into its canonical form: %06X local id + the plugin
    // name in official casing. True when a_id changed. A non-CEF or unparseable
    // id is left exactly as it was (the caller's resolver rejects it), so this is
    // safe to run over every id in a settings file.
    bool CanonicalizeCefColonId(std::string& a_id);
}
