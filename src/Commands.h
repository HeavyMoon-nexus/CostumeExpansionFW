#pragma once

#include <string>

namespace RE
{
    class TESObjectREFR;
}

namespace CostumeFW
{
    // Install the console-command hook (Script::CompileAndRun). Call once on
    // kDataLoaded. Trampoline must already be allocated.
    void InstallConsoleHook();

    // Parse + dispatch a "cef ..." console line. Called by the hook.
    //   cef inject <FormID:Plugin.esp>  - resolve ARMA, inject (NIF + variant)
    //   cef box <token> <content>       - define a box
    //   cef detach <id> | clear | list | nuke | repair | carriers | testnif
    //   cef persist [regen|remove|on <id>|off <id>]
    //   cef morph [<id> on|off] | recover <id> | headdiag | hair <HDPT>
    // a_target = the console-selected reference the ENGINE passed into
    // Script::CompileAndRun. Used instead of RE::Console::GetSelectedRef(),
    // whose CommonLibSSE-NG implementation reads a raw (non-address-library)
    // offset that misses on 1.6.1170 and always returned null.
    void HandleConsoleCommand(const std::string& a_line, RE::TESObjectREFR* a_target = nullptr);
}
