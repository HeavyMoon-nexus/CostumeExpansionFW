#pragma once

#include <cctype>
#include <fstream>
#include <string>

namespace CostumeFW
{
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
            std::ifstream f("Data\\SKSE\\Plugins\\CostumeExpansionFW.ini");
            if (!f) {
                return false;
            }
            std::string line;
            while (std::getline(f, line)) {
                // Normalize: drop whitespace, lower-case. Tolerant of "bEnabled = 0",
                // tabs, CRLF, and either casing.
                std::string t;
                for (const char c : line) {
                    if (c != ' ' && c != '\t' && c != '\r') {
                        t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                }
                if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '[') {
                    continue;  // blank / comment / section header
                }
                if (t == "benabled=0" || t == "benabled=false") {
                    SKSE::log::warn(
                        "CEF hard-DISABLED by CostumeExpansionFW.ini (bEnabled=0) - plugin is inert");
                    return true;
                }
            }
            return false;
        }();
        return disabled;
    }
}
