#pragma once

#include <string>

namespace CostumeFW
{
    // Install the console-command hook (Script::CompileAndRun). Call once on
    // kDataLoaded. Trampoline must already be allocated.
    void InstallConsoleHook();

    // Parse + dispatch a "cef ..." console line. Called by the hook.
    //   cef seed                       - load costume_seed.json, inject enabled
    //   cef inject <FormID:Plugin.esp> - resolve ARMA, inject (NIF + variant)
    //   cef detach <id>                - detach one item
    //   cef clear                      - detach all
    //   cef list                       - list active items
    void HandleConsoleCommand(const std::string& a_line);
}
