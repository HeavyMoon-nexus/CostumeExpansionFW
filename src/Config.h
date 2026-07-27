#pragma once

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

namespace CostumeFW
{
    // Read one boolean key from Data\SKSE\Plugins\CostumeExpansionFW.ini.
    // Whitespace/case tolerant ("bEnabled = 0", tabs, CRLF); comments (; #) and
    // section headers are skipped. Missing file / missing key / unparsable value
    // -> a_default. a_key must be lower-case and without spaces.
    inline bool IniFlag(std::string_view a_key, bool a_default)
    {
        std::ifstream f("Data\\SKSE\\Plugins\\CostumeExpansionFW.ini");
        if (!f) {
            return a_default;
        }
        bool out = a_default;
        std::string line;
        while (std::getline(f, line)) {
            std::string t;
            for (const char c : line) {
                if (c != ' ' && c != '\t' && c != '\r') {
                    t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '[') {
                continue;
            }
            const auto eq = t.find('=');
            if (eq == std::string::npos || std::string_view(t).substr(0, eq) != a_key) {
                continue;
            }
            const std::string_view v = std::string_view(t).substr(eq + 1);
            if (v == "0" || v == "false") {
                out = false;  // last occurrence wins (a duplicated key is user error)
            } else if (v == "1" || v == "true") {
                out = true;
            }
        }
        return out;
    }

    // Emergency master kill-switch, read from an EXTERNAL config file the user can
    // edit WITHOUT launching the game. Purpose: if a CEF-related CTD leaves a save
    // that no longer loads, turn CEF off here and it becomes completely inert -
    // no Load3D hook, no event sinks, no box load, no mesh injection, no
    // carrier/FSMP work, no auto-sync, no co-save restore - so the save opens clean.
    //
    // This is DISTINCT from the MCM master toggle (CefEnabled(), CEF_settings.json
    // "enabled"): that one only hides/shows injected meshes at runtime while every
    // hook and load-time task still runs. HardDisabled() short-circuits before any
    // of that, which is what makes it a safe recovery switch.
    //
    // Paths are relative and resolve through MO2's VFS as Data\SKSE\Plugins\...
    // (same as the other CEF files). Read ONCE per process (a static local) - the
    // value can't change mid-session anyway.
    //
    // Two ways to trip it (either is enough):
    //   1) Existence of  Data\SKSE\Plugins\CEF_DISABLE.txt
    //      A typo-proof panic flag: the file just has to exist (contents ignored).
    //   2) Data\SKSE\Plugins\CostumeExpansionFW.ini
    //        [General]
    //        bEnabled=0        ; 1 (or missing file/key) = enabled, 0 = disabled
    //
    // Default (no ini, no flag file) = ENABLED, so a normal install is unaffected.
    inline bool HardDisabled()
    {
        static const bool disabled = [] {
            // (1) Panic flag file: existence alone disables (can't be mistyped).
            if (std::ifstream("Data\\SKSE\\Plugins\\CEF_DISABLE.txt").good()) {
                SKSE::log::warn("CEF hard-DISABLED by CEF_DISABLE.txt - plugin is inert");
                return true;
            }
            // (2) ini bEnabled=0. Missing file / missing key = enabled (default on).
            if (!IniFlag("benabled", true)) {
                SKSE::log::warn(
                    "CEF hard-DISABLED by CostumeExpansionFW.ini (bEnabled=0) - plugin is inert");
                return true;
            }
            return false;
        }();
        return disabled;
    }

    // Diagnostic lever for the persist-CTD investigation (BUGREPORT_2026-07-27,
    // hypothesis F2). Adding to persist makes CEF rebuild the player's facegen
    // head ~2.5-3.5s later (manifest -> carrier sync -> DoReset3D) so the persist
    // head-carrier picks up its SMP physics. That rebuild is the single most
    // fragile thing CEF does, and it is the suspect a user CAN test: with this
    // set to 0 the rebuild is skipped entirely, so if the crash goes away the
    // cause is on that side, and if it persists the cause is elsewhere.
    //
    //   [Diagnostics]
    //   bPersistHeadRebuild=0
    //
    // NOT a fix and not a supported mode: with the rebuild suppressed, persist
    // costumes will not gain SMP physics until it is set back to 1. Read once
    // per process, like the master switch.
    inline bool PersistHeadRebuildEnabled()
    {
        static const bool on = [] {
            const bool v = IniFlag("bpersistheadrebuild", true);
            if (!v) {
                SKSE::log::warn(
                    "DIAGNOSTIC: persist head rebuild DISABLED by CostumeExpansionFW.ini "
                    "(bPersistHeadRebuild=0) - persist costumes will not get SMP physics "
                    "until this is set back to 1");
            }
            return v;
        }();
        return on;
    }
}
