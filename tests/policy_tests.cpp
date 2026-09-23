// Host-side unit tests for the PURE capture-policy and token-identity layers.
// No CommonLib/RE - builds and runs on the build host. Covers the "policy"
// rows of MARA_GUARD_ADVERSARIAL_REVIEW.md par.6.1 (name exact/prefix, plugin
// prefix, explicit id, defaults on/off, ASCII case, malformed ids, empty
// plugin, the never-truncate colon-id formatter) and the v1.6.4 token identity
// rules (TH1-TH14 of PLAN_2026-09-15_164_token_pools.md). Form-level layers
// (does it exist, is it an ARMO, which file defined it) need RE and stay
// in-game checks.

#include "CapturePolicy.h"
#include "TokenIdentity.h"

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

// v1.6.4 token identity (TH1-TH14). The two starred groups are the regression
// guards: they fail if somebody narrows the BROAD predicate, or widens the
// NARROW one to include the NPC add-on.
namespace
{
    void TokenIdentityChecks()
    {
        using namespace CostumeFW::tokenid;

        // TH1 - the core plugin defines generation 0's box tokens.
        CHECK(IsBoxTokenColonId("000801:CostumeFW.esp"));
        // TH2 - the publish tokens and the NPC persist carriers are NOT box
        // tokens, even though they live in a plugin that passes the broad test.
        CHECK(!IsBoxTokenColonId("000800:CostumeFW_NPC.esp"));
        CHECK(!IsBoxTokenColonId("000810:CostumeFW_NPC.esp"));
        // TH3 - neither is an ability pool.
        CHECK(!IsBoxTokenColonId("000801:CostumeFW_Abilities.esp"));
        CHECK(!IsBoxTokenColonId("000801:CostumeFW_Abilities2.esp"));
        // TH4 / TH5 / TH8 - a malformed id is not a CEF id.
        CHECK(!IsBoxTokenColonId("XYZ:CostumeFW.esp"));
        CHECK(!IsBoxTokenColonId("801junk:CostumeFW.esp"));
        CHECK(!IsBoxTokenColonId(":CostumeFW.esp"));
        CHECK(!IsBoxTokenColonId(""));
        CHECK(!IsBoxTokenColonId("CostumeFW.esp"));  // no colon
        CHECK(!IsCefColonId("801junk:CostumeFW.esp"));
        // TH6 - casing does not change which form an id names, so it must not
        // change the answer; canonicalising rewrites it to the official spelling.
        CHECK(IsBoxTokenColonId("000801:costumefw.esp"));
        {
            std::string id = "000801:costumefw.esp";
            CHECK(CanonicalizeCefColonId(id) && id == "000801:CostumeFW.esp");
            id = "801:COSTUMEFW_BOXPOOL2.ESP";
            CHECK(CanonicalizeCefColonId(id) && id == "000801:CostumeFW_BoxPool2.esp");
            id = "000801:CostumeFW.esp";
            CHECK(!CanonicalizeCefColonId(id) && id == "000801:CostumeFW.esp");
            id = "000801:SomeCostume.esp";  // not ours: left alone
            CHECK(!CanonicalizeCefColonId(id) && id == "000801:SomeCostume.esp");
        }
        // TH7 - the NPC add-on in any casing: broad yes, narrow no.
        CHECK(IsCefColonId("000800:costumefw_npc.esp"));
        CHECK(!IsBoxTokenColonId("000800:costumefw_npc.esp"));
        // TH9 - the narrow test is an allowlist, not a prefix match. A dev patch
        // that is not registered does not silently join the pool either.
        CHECK(!IsBoxTokenColonId("000801:CostumeFWX.esp"));
        CHECK(!IsBoxTokenColonId("000801:CostumeFW_VanillaSlots_001.esp"));
        CHECK(!IsBoxTokenColonId("000801:SomeCostume.esp"));

        // TH10 - the suffix is a NUMBER, so BoxPool10 comes after BoxPool2.
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool1.esp") == 1);
        CHECK(BoxPoolGeneration("costumefw_boxpool10.esp") == 10);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool2.esp") == 2);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool10.esp") >
              BoxPoolGeneration("CostumeFW_BoxPool2.esp"));
        CHECK(IsBoxTokenPlugin("CostumeFW_BoxPool1.esp"));
        // TH11 - one generation, one spelling.
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool.esp") == 0);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool0.esp") == 0);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool01.esp") == 0);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool1.esm") == 0);
        CHECK(BoxPoolGeneration("CostumeFW_BoxPool1x.esp") == 0);
        CHECK(!IsBoxTokenPlugin("CostumeFW_BoxPool.esp"));
        // TH12 - the ability pool's generation 1 carries an exception name.
        CHECK(AbilityPoolGeneration("CostumeFW_Abilities.esp") == 1);
        CHECK(AbilityPoolGeneration("CostumeFW_Abilities2.esp") == 2);
        CHECK(AbilityPoolGeneration("costumefw_abilities3.esp") == 3);
        CHECK(AbilityPoolGeneration("CostumeFW_Abilities1.esp") == 0);
        CHECK(AbilityPoolGeneration("CostumeFW_BoxPool2.esp") == 0);
        CHECK(AbilityPoolPluginName(1) == "CostumeFW_Abilities.esp");
        CHECK(AbilityPoolPluginName(2) == "CostumeFW_Abilities2.esp");
        CHECK(BoxPoolPluginName(3) == "CostumeFW_BoxPool3.esp");
        CHECK(BoxPoolPluginName(0).empty());

        // TH13 * - the BROAD predicate must stay broad. Every CEF record has to
        // keep failing the capture pickers and the box-content gate, publish
        // tokens included; narrowing this is how a publish token becomes content.
        CHECK(IsCefColonId("000801:CostumeFW.esp"));
        CHECK(IsCefColonId("000800:CostumeFW_NPC.esp"));
        CHECK(IsCefColonId("000810:CostumeFW_NPC.esp"));
        CHECK(IsCefColonId("000801:CostumeFW_Abilities.esp"));
        CHECK(IsCefColonId("000801:CostumeFW_Abilities2.esp"));
        CHECK(IsCefColonId("000801:CostumeFW_BoxPool1.esp"));
        CHECK(IsCefColonId("000801:CostumeFW_VanillaSlots_001.esp"));
        CHECK(!IsCefColonId("000801:SomeCostume.esp"));

        // TH14 * - the NARROW predicate must stay narrow. The NPC add-on in this
        // allowlist is the exact defect this layer exists to prevent.
        CHECK(IsBoxTokenPlugin("CostumeFW.esp"));
        CHECK(IsBoxTokenPlugin("costumefw.esp"));
        CHECK(IsBoxTokenPlugin("CostumeFW_BoxPool1.esp"));
        CHECK(IsBoxTokenPlugin("CostumeFW_BoxPool9.esp"));
        CHECK(!IsBoxTokenPlugin("CostumeFW_NPC.esp"));
        CHECK(!IsBoxTokenPlugin("CostumeFW_Abilities.esp"));
        CHECK(!IsBoxTokenPlugin("CostumeFW_Abilities2.esp"));
        CHECK(!IsBoxTokenPlugin("CostumeFWX.esp"));
        CHECK(!IsBoxTokenPlugin("CostumeFW_VanillaSlots_001.esp"));
        CHECK(!IsBoxTokenPlugin(""));

        // --- carrier keys (v1.6.4 step 5) -----------------------------------
        // The name every carrier artifact of a token is built under. Two boxes
        // on one slot must never produce the same key, and generation 0 must
        // keep producing the name its shipped ARMA already points at.
        CHECK(CarrierKeyFor("000811:CostumeFW.esp", 55) == "Box55");
        CHECK(CarrierKeyFor("000811:costumefw.esp", 31) == "Box31");  // casing is not identity
        CHECK(CarrierKeyFor("000800:CostumeFW_BoxPool1.esp", 55) == "BP01_000800");
        CHECK(CarrierKeyFor("800:CostumeFW_BoxPool1.esp", 55) == "BP01_000800");  // short hex
        CHECK(CarrierKeyFor("000800:costumefw_boxpool1.esp", 55) == "BP01_000800");
        CHECK(CarrierKeyFor("000800:CostumeFW_BoxPool10.esp", 55) == "BP10_000800");
        // The slot is generation 0's alone: a pool token's key does not move
        // when the slot does, and the same local id in two generations gives
        // two keys (the whole point of putting the generation in the name).
        CHECK(CarrierKeyFor("000800:CostumeFW_BoxPool1.esp", 61) == "BP01_000800");
        CHECK(CarrierKeyFor("000800:CostumeFW_BoxPool1.esp", 0) == "BP01_000800");
        CHECK(CarrierKeyFor("000800:CostumeFW_BoxPool2.esp", 55) !=
              CarrierKeyFor("000800:CostumeFW_BoxPool1.esp", 55));
        // Three boxes sharing slot 55, three distinct namespaces.
        CHECK(CarrierKeyFor("000801:CostumeFW_BoxPool1.esp", 55) !=
              CarrierKeyFor("000800:CostumeFW_BoxPool1.esp", 55));
        // Not a box token, or no slot to name generation 0 by -> no key at all,
        // and the caller leaves the box out of the manifest.
        CHECK(CarrierKeyFor("000800:CostumeFW_NPC.esp", 55).empty());
        CHECK(CarrierKeyFor("000801:CostumeFW_Abilities.esp", 55).empty());
        CHECK(CarrierKeyFor("000801:SomeCostume.esp", 55).empty());
        CHECK(CarrierKeyFor("801junk:CostumeFW.esp", 55).empty());
        CHECK(CarrierKeyFor("", 55).empty());
        CHECK(CarrierKeyFor("000811:CostumeFW.esp", 0).empty());
        CHECK(CarrierKeyFor("000811:CostumeFW.esp", 29).empty());
        CHECK(CarrierKeyFor("000811:CostumeFW.esp", 62).empty());
    }

    // Review 2026-09-23 C1 / R2. IsStrictUtf8 has to accept exactly what
    // nlohmann's dump() accepts: anything it waves through that dump() refuses
    // is a crash, and anything it refuses that dump() would take is a costume
    // somebody cannot capture for nothing. Bytes are spelled out so the source
    // encoding cannot change what is being tested.
    void Utf8Checks()
    {
        using namespace CostumeFW::policy;

        // Accepted: ASCII, CJK, an emoji (4-byte), the edges of each range.
        CHECK(IsStrictUtf8(""));
        CHECK(IsStrictUtf8("Iron Armor.esp"));
        CHECK(IsStrictUtf8("\xE8\xA1\xA3\xE8\xA3\x85"));        // U+8863 U+88C5 (the word used in 8-28)
        CHECK(IsStrictUtf8("\xF0\x9F\x98\x80"));                // U+1F600
        CHECK(IsStrictUtf8("\xC2\x80\xDF\xBF"));                // U+0080, U+07FF
        CHECK(IsStrictUtf8("\xE0\xA0\x80\xED\x9F\xBF"));        // U+0800, U+D7FF
        CHECK(IsStrictUtf8("\xEE\x80\x80\xEF\xBF\xBF"));        // U+E000, U+FFFF
        CHECK(IsStrictUtf8("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF"));  // U+10000, U+10FFFF

        // Refused: overlong forms.
        CHECK(!IsStrictUtf8("\xC0\x80"));
        CHECK(!IsStrictUtf8("\xC1\xBF"));
        CHECK(!IsStrictUtf8("\xE0\x80\x80"));
        CHECK(!IsStrictUtf8("\xE0\x9F\xBF"));
        CHECK(!IsStrictUtf8("\xF0\x80\x80\x80"));
        CHECK(!IsStrictUtf8("\xF0\x8F\xBF\xBF"));
        // Refused: surrogates, and past U+10FFFF - the three the OLD check let
        // through while dump() did not.
        CHECK(!IsStrictUtf8("\xED\xA0\x80"));
        CHECK(!IsStrictUtf8("\xED\xBF\xBF"));
        CHECK(!IsStrictUtf8("\xF4\x90\x80\x80"));
        CHECK(!IsStrictUtf8("\xF5\x80\x80\x80"));
        CHECK(!IsStrictUtf8("\xFF"));
        // Refused: cut short, and a lone continuation byte.
        CHECK(!IsStrictUtf8("\xE8\xA1"));
        CHECK(!IsStrictUtf8("\xF0\x9F\x98"));
        CHECK(!IsStrictUtf8("ab\xC3"));
        CHECK(!IsStrictUtf8("a\x80" "b"));
        // Refused: the same word in cp932 (the bytes a Japanese-locale mod
        // author's tools write), and cp1252's n-tilde.
        CHECK(!IsStrictUtf8("\x88\xDF\x91\x95"));
        CHECK(!IsStrictUtf8("Se\xF1ora"));

        // Cut to a buffer without leaving half a character behind.
        CHECK(Utf8PrefixLength("abc", 10) == 3);
        CHECK(Utf8PrefixLength("abc", 2) == 2);
        CHECK(Utf8PrefixLength("\xE8\xA1\xA3\xE8\xA3\x85", 6) == 6);
        CHECK(Utf8PrefixLength("\xE8\xA1\xA3\xE8\xA3\x85", 5) == 3);
        CHECK(Utf8PrefixLength("\xE8\xA1\xA3\xE8\xA3\x85", 4) == 3);
        CHECK(Utf8PrefixLength("\xE8\xA1\xA3\xE8\xA3\x85", 2) == 0);
        CHECK(Utf8PrefixLength("\xE8\xA1\xA3", 0) == 0);

        // Escaping: well-formed text is untouched, anything else becomes \xNN,
        // and the result is always something dump() will write.
        CHECK(EscapeInvalidUtf8("Iron Armor") == "Iron Armor");
        CHECK(EscapeInvalidUtf8("\xE8\xA1\xA3") == "\xE8\xA1\xA3");
        CHECK(EscapeInvalidUtf8("Se\xF1ora") == "Se\\xF1ora");
        CHECK(EscapeInvalidUtf8("\xED\xA0\x80") == "\\xED\\xA0\\x80");
        CHECK(IsStrictUtf8(EscapeInvalidUtf8("\x88\xDF\x91\x95")));
        CHECK(EscapeInvalidUtf8("\x88\xDF\x91\x95") != EscapeInvalidUtf8("\x89\xDF\x91\x95"));
    }
}

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

    // --- ParseColonId is strict about the local id (v1.6.4) ------------------
    // stoul eats the longest valid prefix and stops, so this came back as a
    // clean 0x801 with the junk silently dropped. A border value has to be
    // refused instead of reinterpreted (TH8).
    CHECK(!ParseColonId("801junk:Plugin.esp", local, plugin));
    CHECK(!ParseColonId(":Plugin.esp", local, plugin));       // no digits at all
    CHECK(!ParseColonId("", local, plugin));
    CHECK(!ParseColonId("0080 1:Plugin.esp", local, plugin));  // embedded space

    // --- The local id has to fit its plugin (review 2026-09-23 C11) ----------
    // The parse accepts up to 32 bits; whether the id can name a record of its
    // plugin is a separate question, because the engine ADDS the local id to
    // the plugin's base without masking it and an overlong one lands in some
    // other plugin. A light plugin holds FFF, a full one FFFFFF.
    CHECK(ParseColonId("1000D62:Skyrim.esm", local, plugin) && local == 0x1000D62u);
    CHECK(!LocalIdInRange(local, false));  // would have been Update.esm's 0x000D62
    CHECK(!ParseColonId("123456789:Skyrim.esm", local, plugin));  // more than 32 bits
    CHECK(LocalIdInRange(0xFFF, true));
    CHECK(!LocalIdInRange(0x1000, true));
    CHECK(!LocalIdInRange(0x12A45, true));  // an ESP id kept after the plugin was ESLified
    CHECK(LocalIdInRange(0x12A45, false));
    CHECK(LocalIdInRange(0xFFFFFF, false));
    CHECK(!LocalIdInRange(0x1000000, false));
    CHECK(LocalIdInRange(0, true) && LocalIdInRange(0, false));

    // --- Shipped defaults are what the docs claim ----------------------------
    CHECK(DefaultBlockNames().size() == 1 && DefaultBlockNames()[0] == "CORE Carrier");
    CHECK(DefaultBlockPlugins().size() == 1 && DefaultBlockPlugins()[0] == "MARA");

    TokenIdentityChecks();
    Utf8Checks();

    std::printf("policy_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
