#pragma once

#include <string>
#include <string_view>

#include "RE/C/ConsoleLog.h"

namespace CostumeFW
{
    // The ONE way CEF writes to the game console.
    //
    // RE::ConsoleLog::Print is `Print(const char* a_fmt, ...)` - a varargs
    // FORMAT call that forwards to VPrint. Passing a runtime string as the
    // first argument makes the user's own text the format: a costume label,
    // NIF shape name or NPC name containing '%' then reads arguments that were
    // never pushed (adversarial review 2026-09-09 F01 - garbled output, bad
    // reads, CTD, reachable by just renaming a box and listing it).
    //
    // Every console write goes through here so a later `->Print(str)` cannot
    // quietly reintroduce it. Never call ConsoleLog::Print directly.
    inline void ConsolePrint(std::string_view a_text)
    {
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            // string_view is not guaranteed NUL-terminated; materialize first.
            const std::string text(a_text);
            console->Print("%s", text.c_str());
        }
    }
}
