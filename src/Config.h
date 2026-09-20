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
    // AND WHY IT IS ONLY A RECOVERY SWITCH
    //   "Registers nothing" includes InstallSerialization, so a game saved while
    //   this is on carries NO CEF co-save chunk. Reloading that save afterwards
    //   loses the hidden store's ref (the container and its items are still in
    //   the save, but nothing can find them again), the store manifest the loss
    //   detector compares against, this save's active persist set, the publish
    //   bindings and the npc-persist assignments. The chunk cannot be preserved
    //   by writing it back verbatim either: the co-save header is rebuilt with
    //   the CURRENT plugin list, so unresolved form ids would be reinterpreted
    //   against a different load order - which is exactly the situation someone
    //   recovering a save is in.
    //   So this switch is for opening a save, confirming it loads, and quitting
    //   to desktop. Playing on without CEF's costumes is what the settings-file
    //   "enabled" is for, and that one keeps the save data intact. The user-
    //   facing version of this note lives in CostumeExpansionFW.ini and README.md.
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
                SKSE::log::warn(
                    "CEF hard-DISABLED by CEF_DISABLE.txt - plugin is inert. "
                    "DO NOT SAVE while it is: CEF writes nothing into the save, so saving "
                    "here loses the hidden storage, this save's persist items and any NPC "
                    "costume bindings. Open the save, check it loads, then quit to desktop. "
                    "To play with the costumes off but the data intact, delete this file and "
                    "put an empty CEF_NOINJECT.txt in its place instead.");
                return true;
            }
            // (2) ini bEnabled=0. Missing file / missing key = enabled (default on).
            if (!IniFlag("benabled", true)) {
                SKSE::log::warn(
                    "CEF hard-DISABLED by CostumeExpansionFW.ini (bEnabled=0) - plugin is inert. "
                    "DO NOT SAVE while it is: CEF writes nothing into the save, so saving "
                    "here loses the hidden storage, this save's persist items and any NPC "
                    "costume bindings. Open the save, check it loads, then quit to desktop. "
                    "To play with the costumes off but the data intact, set bEnabled=1 and "
                    "put an empty CEF_NOINJECT.txt in SKSE\\Plugins instead.");
                return true;
            }
            return false;
        }();
        return disabled;
    }

    // The OTHER switch, and the reason this one exists as a file at all.
    //
    // There have always been two ways to stop CEF and they are not the same
    // thing. HardDisabled() above registers nothing, which is what makes it a
    // recovery switch and also what makes saving in that state cost you the
    // hidden storage, this save's persist items and every NPC costume binding.
    // The gentle one - CEF_settings.json's "enabled": false - stops the
    // injection, the abilities and the token hand-outs while the hooks, the
    // co-save and the load-time bookkeeping all keep running, so the save data
    // is untouched and the costumes come straight back.
    //
    // Until now the gentle one could only be reached by hand-editing json, and
    // the dangerous one was a file you could drop in place. Somebody who cannot
    // launch the game takes the switch they can reach, which was the wrong one.
    // This is the same gentle switch with the same interface as the panic flag:
    // create Data\SKSE\Plugins\CEF_NOINJECT.txt and CEF loads normally and
    // injects nothing; delete it and it picks up where it left off.
    //
    // It OVERRIDES the stored setting rather than replacing it: CefEnabled()
    // reads it, WriteJson never writes it, so the master toggle in the UI is
    // still whatever the user last chose and is there again when the file goes.
    // Read once per process, like the others.
    inline bool NoInject()
    {
        static const bool on = [] {
            if (!std::ifstream("Data\\SKSE\\Plugins\\CEF_NOINJECT.txt").good()) {
                return false;
            }
            SKSE::log::warn(
                "CEF is loaded but NOT INJECTING - CEF_NOINJECT.txt is present. Hooks, the "
                "co-save and every load-time check are running as usual, so your save data is "
                "safe and saving here is fine; costumes, enchantment abilities and box tokens "
                "are simply not applied. Delete the file to turn them back on. (This is the "
                "same thing as \"enabled\": false in CEF_settings.json, and it overrides it "
                "without changing what is in the file.)");
            return true;
        }();
        return on;
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
