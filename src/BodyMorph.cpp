#include "BodyMorph.h"

// skee64 SDK header (vendored from RaceMenu 0.4.20 ModderResource). It declares
// the engine types (TESObjectREFR, NiAVObject, ...) as GLOBAL-namespace forward
// declarations - distinct from RE::*, so there is no clash with PCH's RE/Skyrim.
// We only ever use them as opaque pointers, bridged from RE::* via reinterpret_cast.
#include "skee/IPluginInterface.h"

#include "RE/N/NiAVObject.h"
#include "RE/T/TESObjectREFR.h"

#include <cstdint>

namespace CostumeFW::BodyMorph
{
    namespace
    {
        IBodyMorphInterface* g_bodyMorph = nullptr;
    }

    void RequestInterface()
    {
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return;
        }
        InterfaceExchangeMessage msg;
        // dataLen = sizeof(pointer), matching the skee/OBody NG reference exchange.
        messaging->Dispatch(
            static_cast<std::uint32_t>(InterfaceExchangeMessage::kMessage_ExchangeInterface),
            &msg, sizeof(InterfaceExchangeMessage*), "skee");
        if (msg.interfaceMap) {
            g_bodyMorph = static_cast<IBodyMorphInterface*>(
                msg.interfaceMap->QueryInterface("BodyMorph"));
        }
        SKSE::log::info("skee BodyMorph interface: {}",
            g_bodyMorph ? "acquired" : "unavailable (RaceMenu/skee not found)");
    }

    bool Available()
    {
        return g_bodyMorph != nullptr;
    }

    void ApplyToNode(RE::TESObjectREFR* a_refr, RE::NiAVObject* a_node)
    {
        if (!g_bodyMorph || !a_refr || !a_node) {
            return;
        }
        auto* refr = reinterpret_cast<::TESObjectREFR*>(a_refr);
        if (!g_bodyMorph->HasMorphs(refr)) {
            return;  // actor carries no body morphs - nothing to match
        }
        g_bodyMorph->ApplyVertexDiff(refr, reinterpret_cast<::NiAVObject*>(a_node), false);
        SKSE::log::info("  applied body morph (skee ApplyVertexDiff)");
    }
}
