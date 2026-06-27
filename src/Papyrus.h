#pragma once

namespace RE::BSScript
{
    class IVirtualMachine;
}

namespace CostumeFW
{
    // Register the CFW_Native Papyrus class natives. Pass to
    // SKSE::GetPapyrusInterface()->Register(...) in SKSEPluginLoad. Returns true
    // on success (signature required by the Papyrus interface).
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
}
