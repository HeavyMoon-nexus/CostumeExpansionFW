// Host-side unit tests for the PURE capture-policy layers (v1.3.2 review r2).
// No CommonLib/RE - builds and runs on the build host. Covers the "policy"
// rows of MARA_GUARD_ADVERSARIAL_REVIEW.md par.6.1: name exact/prefix, plugin
// prefix, explicit id, defaults on/off, ASCII case, malformed ids, empty
// plugin, and the never-truncate colon-id formatter. Form-level layers
// (IsDynamicForm / record flags / keywords) need RE and stay in-game checks.

#include "CapturePolicy.h"

#include <cstdio>
#include <string>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const char* a_what)
    {
        ++g_checks;
        if (!a_ok) {
            ++g_failures;
            std::printf("FAIL: %s\n", a_what);
        }
    }
}

#define CHECK(expr) Check((expr), #expr)

int main()
{
    using namespace CostumeFW::policy;

    // --- CI primitives ----------------------------------------------------
    CHECK(EqualsCI("CORE Carrier", "core carrier"));
    CHECK(EqualsCI("", ""));
    CHECK(!EqualsCI("CORE Carrier", "CORE Carrier2"));
    CHECK(PrefixCI("MARA.dll", "mara"));
    CHECK(PrefixCI("MARA", "MARA"));
    CHECK(!PrefixCI("MAR", "MARA"));
    CHECK(!PrefixCI("MARA", ""));  // empty prefix never matches (deny-list hygiene)

    // --- Name patterns ------------------------------------------------------
    CHECK(NameMatches("CORE Carrier", "CORE Carrier"));
    CHECK(NameMatches("core carrier", "CORE Carrier"));   // CI exact
    CHECK(!NameMatches("CORE Carrier X", "CORE Carrier"));  // exact is exact
    CHECK(NameMatches("CORE Carrier X", "CORE*"));           // trailing-star prefix
    CHECK(NameMatches("CORE", "CORE*"));
    CHECK(!NameMatches("XCORE", "CORE*"));
    CHECK(!NameMatches("anything", "*"));  // bare star = empty prefix = no match

    // --- Policy: names (defaults + user + disableDefaults) -----------------
    CapturePolicy pol;
    CHECK(NameDenied(pol, "CORE Carrier"));         // shipped default
    CHECK(NameDenied(pol, "core carrier"));         // CI
    CHECK(!NameDenied(pol, "Iron Armor"));
    CHECK(!NameDenied(pol, ""));                    // empty name never denied
    pol.disableDefaults = true;
    CHECK(!NameDenied(pol, "CORE Carrier"));        // defaults off
    pol.names.push_back("Cursed*");
    CHECK(NameDenied(pol, "Cursed Ring of Woe"));   // user prefix entry
    CHECK(!NameDenied(pol, "Blessed Ring"));
    pol.disableDefaults = false;

    // --- Policy: plugins ----------------------------------------------------
    CHECK(PluginDenied(pol, "MARA.esp"));           // shipped default (prefix)
    CHECK(PluginDenied(pol, "mara_extra.esl"));     // CI prefix
    CHECK(!PluginDenied(pol, "Skyrim.esm"));
    CHECK(!PluginDenied(pol, ""));                  // no filename = not plugin-denied
    pol.plugins.push_back("BadMod");
    CHECK(PluginDenied(pol, "BadMod.esp"));
    {
        CapturePolicy defOff;
        defOff.disableDefaults = true;
        CHECK(!PluginDenied(defOff, "MARA.esp"));   // defaults off
    }

    // --- Policy: ids ----------------------------------------------------------
    pol.ids.push_back("000D62:MARA.esp");
    CHECK(IdDenied(pol, "000d62:mara.esp"));        // CI
    CHECK(!IdDenied(pol, "000D63:MARA.esp"));

    // --- r4 selected-ARMA identity matrix (pure policy portion) ---------------
    // The wrapper ARMO may be allowed while its final addon is denied by source
    // plugin or by the addon's own canonical colon-id.
    CapturePolicy r4;
    r4.disableDefaults = true;
    r4.plugins.push_back("DeniedAddon");
    r4.ids.push_back("000A42:AllowedAddon.esp");
    CHECK(!PluginDenied(r4, "AllowedWrapper.esp") &&
          PluginDenied(r4, "DeniedAddon.esp"));
    CHECK(!IdDenied(r4, "000B00:AllowedWrapper.esp") &&
          IdDenied(r4, "000A42:AllowedAddon.esp"));
    CHECK(IdDenied(r4, FormatColonId(0xA42, "AllowedAddon.esp")));
    // --- ParseColonId ---------------------------------------------------------
    std::uint32_t local = 0;
    std::string plugin;
    CHECK(ParseColonId("000801:CostumeFW.esp", local, plugin) &&
          local == 0x801 && plugin == "CostumeFW.esp");
    CHECK(ParseColonId("FF000800:", local, plugin) &&
          local == 0xFF000800 && plugin.empty());   // empty plugin parses (resolver rejects later)
    CHECK(!ParseColonId("no-colon-here", local, plugin));
    CHECK(!ParseColonId("XYZ:Plugin.esp", local, plugin));  // bad hex
    CHECK(ParseColonId("801:Plugin.esp", local, plugin) && local == 0x801);  // short hex ok

    // --- FormatColonId: minimum width, NEVER truncates ---------------------
    CHECK(FormatColonId(0x801, "CostumeFW.esp") == "000801:CostumeFW.esp");
    CHECK(FormatColonId(0xFF000800, "") == "FF000800:");  // 8 digits survive (old char[8] bug)
    CHECK(FormatColonId(0, "X.esp") == "000000:X.esp");

    // --- CanonicalizeColonIdStr ----------------------------------------------
    std::string id = "000801:CostumeFW.esp";
    CHECK(!CanonicalizeColonIdStr(id) && id == "000801:CostumeFW.esp");  // already canonical
    id = "801:CostumeFW.esp";
    CHECK(CanonicalizeColonIdStr(id) && id == "000801:CostumeFW.esp");   // zero-pad
    id = "ff000800:Some.esp";
    CHECK(CanonicalizeColonIdStr(id) && id == "FF000800:Some.esp");      // case + no truncation
    id = "garbage";
    CHECK(!CanonicalizeColonIdStr(id) && id == "garbage");               // unparseable untouched

    // --- Shipped defaults are what the docs claim ----------------------------
    CHECK(DefaultBlockNames().size() == 1 && DefaultBlockNames()[0] == "CORE Carrier");
    CHECK(DefaultBlockPlugins().size() == 1 && DefaultBlockPlugins()[0] == "MARA");

    std::printf("policy_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
