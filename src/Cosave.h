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

    // Carry a persist active that could not be registered to the next save
    // instead of letting it vanish from it. The co-save load did this from the
    // start (ROOT H); a settings reload is the other way to lose one - a
    // blacklist change re-registers every active, and one the new policy
    // refuses dropped out of the next ACTV for good, even after the entry was
    // lifted again (review 2026-09-23 W20). No duplicates. Main thread.
    void CarryPersistActive(const std::string& a_id);

    // Hand back the carried actives and forget them - for a caller about to
    // re-try registering them, which carries whatever fails again. Main thread.
    std::vector<std::string> TakeUnresolvedActives();
}
