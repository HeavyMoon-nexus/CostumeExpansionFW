#pragma once

#include <string>
#include <vector>

namespace CostumeFW
{
    // Register the SKSE co-save serialization callbacks (persist the active item
    // set across game restarts). Call once in SKSEPluginLoad.
    void InstallSerialization();

    // This save's persist actives that did not resolve at load (their plugin is
    // off, or the capture policy refuses them now) and are carried to the next
    // save instead of dropped (ROOT H). Their originals are still in this save's
    // hidden store, so anything deciding what the store "holds" has to count
    // them (review 2026-09-23 C4). Main thread.
    std::vector<std::string> UnresolvedActiveIds();
}
