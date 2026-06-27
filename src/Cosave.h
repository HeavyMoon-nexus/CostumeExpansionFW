#pragma once

namespace CostumeFW
{
    // Register the SKSE co-save serialization callbacks (persist the active item
    // set across game restarts). Call once in SKSEPluginLoad.
    void InstallSerialization();
}
