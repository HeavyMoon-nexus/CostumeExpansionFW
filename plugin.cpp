// CostumeExpansionFW - skin-rebind injection PoC.
// Console commands (open ~ console, type):
//   cef seed | cef inject <FormID:Plugin.esp> | cef detach <id> | cef clear | cef list
// Real seed/config/co-save driven flow continues to replace this test driver.

#include "logger.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "Commands.h"
#include "Cosave.h"
#include "Papyrus.h"
#include "SkinRebind.h"

#include "RE/P/PlayerCharacter.h"
#include "RE/S/ScriptEventSourceHolder.h"
#include "RE/T/TESContainerChangedEvent.h"
#include "RE/T/TESEquipEvent.h"

namespace
{
    // Re-attach our injected meshes whenever the engine rebuilds the player 3D
    // (cell change, save/load, transform, resurrect, RaceMenu apply).
    // PlayerCharacter::Load3D is vfunc 0x6A. Pure vtable swap (no trampoline).
    // Defer the re-attach to next frame so the skeleton is settled.
    struct Load3DHook
    {
        static RE::NiAVObject* thunk(RE::PlayerCharacter* a_this, bool a_backgroundLoading)
        {
            auto* result = func(a_this, a_backgroundLoading);
            SKSE::GetTaskInterface()->AddTask([] { CostumeFW::Reconcile(); });
            return result;
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
            func = vtbl.write_vfunc(0x6A, thunk);
            SKSE::log::info("PlayerCharacter::Load3D hook installed (0x6A)");
        }
    };

    // Box mechanism: when the player equips/unequips a tracked box token, the
    // box contents must show/hide. Reconcile re-evaluates the worn predicate.
    class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static EquipSink* GetSingleton()
        {
            static EquipSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* a_event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (a_event && a_event->actor &&
                a_event->actor.get() == RE::PlayerCharacter::GetSingleton() &&
                (CostumeFW::IsTrackedToken(a_event->baseObject) ||
                    CostumeFW::IsBoxToken(a_event->baseObject))) {
                SKSE::GetTaskInterface()->AddTask([] {
                    CostumeFW::Reconcile();
                    CostumeFW::ApplyBoxAbilities();
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        EquipSink() = default;
    };

    // Resilience: if a box token leaves the player's inventory (sold/dropped),
    // give it back so the box system can't be permanently lost. Equip/unequip do
    // NOT fire this event, so strip mods are unaffected.
    class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static ContainerSink* GetSingleton()
        {
            static ContainerSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (a_event) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                const RE::FormID playerID = player ? player->GetFormID() : 0;
                if (a_event->oldContainer == playerID && a_event->newContainer != playerID &&
                    CostumeFW::IsBoxToken(a_event->baseObj)) {
                    const RE::FormID token = a_event->baseObj;
                    SKSE::GetTaskInterface()->AddTask([token] { CostumeFW::ReplenishToken(token); });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ContainerSink() = default;
    };

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            // Acquire skee's IBodyMorphInterface AFTER all kPostLoad handlers ran -
            // requesting at kPostLoad is too early and skee returns no interfaceMap
            // (matches OBody NG's reference, which exchanges at kPostPostLoad).
            CostumeFW::BodyMorph::RequestInterface();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            Load3DHook::Install();
            CostumeFW::InstallConsoleHook();
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink(EquipSink::GetSingleton());
                holder->AddEventSink(ContainerSink::GetSingleton());
            }
            // Load the GLOBAL box definitions (costume_boxes.json) once.
            SKSE::GetTaskInterface()->AddTask([] {
                CostumeFW::LoadBoxes();
                CostumeFW::Reconcile();
                CostumeFW::ApplyBoxAbilities();
            });
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // Save loaded: a co-save revert wiped the registry, so re-register the
            // global boxes, then re-apply everything (belt-and-suspenders to Load3D).
            // Synthesized stat abilities are dynamic forms dropped by the save, so
            // forget the cache and rebuild them fresh.
            SKSE::GetTaskInterface()->AddTask([] {
                CostumeFW::ClearBoxSpellCache();
                CostumeFW::ReapplyBoxes();
                CostumeFW::Reconcile();
                CostumeFW::ApplyBoxAbilities();
            });
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    CostumeFW::SetupLog();
    SKSE::Init(skse);
    SKSE::log::info("CostumeExpansionFW loaded");

    SKSE::AllocTrampoline(64);  // for the console CompileAndRun hook
    CostumeFW::InstallSerialization();
    SKSE::GetPapyrusInterface()->Register(CostumeFW::RegisterPapyrus);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
