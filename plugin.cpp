// CostumeExpansionFW - skin-rebind injection PoC.
// Console commands (open ~ console, type):
//   cef seed | cef inject <FormID:Plugin.esp> | cef detach <id> | cef clear | cef list
// Real seed/config/co-save driven flow continues to replace this test driver.

#include "logger.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "Commands.h"
#include "Config.h"
#include "Cosave.h"
#include "LoreBox.h"
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
            // The engine builds the facegen head AGAIN a few seconds into the
            // load sequence; FSMP then retires the old Head merge generation and
            // an injected mesh bound to it keeps dead, world-frozen bones
            // (in-game 2026-07-04: bound at Head_00000001, headdiag showed
            // Head_00000002). Reconcile's dead-bind sweep repairs that - but
            // only when it runs, so run one more pass after the rebuild window.
            CostumeFW::RunAfterDelayMs(4000, [] { CostumeFW::Reconcile(); });
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
    // §8.10 hide-when-worn also needs a reconcile when the player equips/unequips
    // ANY real armor (boots/helmet that gate an injected item), not just our own
    // tokens - so we reconcile on every player equip change. Reconcile is cheap
    // (it just walks g_active and queries worn slots), so the extra fires are
    // negligible; ApplyBoxAbilities is idempotent.
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
                a_event->actor.get() == RE::PlayerCharacter::GetSingleton()) {
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
        // Defense-in-depth: SKSEPluginLoad already returns before registering this
        // listener when CEF is hard-disabled, so this normally can't fire. Guard it
        // anyway so no message ever installs a hook / sink / 3D task while disabled.
        if (CostumeFW::HardDisabled()) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            // Acquire skee's IBodyMorphInterface AFTER all kPostLoad handlers ran -
            // requesting at kPostLoad is too early and skee returns no interfaceMap
            // (matches OBody NG's reference, which exchanges at kPostPostLoad).
            CostumeFW::BodyMorph::RequestInterface();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            Load3DHook::Install();
            CostumeFW::InstallLoreBoxHook();  // soft LoreBox tooltip integration
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
        case SKSE::MessagingInterface::kNewGame:
            // Save loaded: a co-save revert wiped the registry, so re-register the
            // global boxes, then re-apply everything (belt-and-suspenders to Load3D).
            // Synthesized stat abilities are dynamic forms dropped by the save, so
            // forget the cache and rebuild them fresh. The carrier pass re-repoints
            // AND reconciles the persist head-part registration against the loaded
            // save (a CTD rollback can predate the registration - C §9-18); it
            // rebuilds the head only when something actually changed.
            SKSE::GetTaskInterface()->AddTask([] {
                CostumeFW::ClearBoxSpellCache();
                CostumeFW::ReapplyBoxes();
                CostumeFW::ApplyCarrierOverrides(false);
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
    // Build stamp: proves WHICH dll the game actually loaded. If this timestamp
    // is older than your last build, MO2's usvfs served a stale cached copy
    // (rebuilding a dll while MO2 is running does NOT refresh it - fully close
    // AND reopen MO2, not just the game). __DATE__/__TIME__ update whenever this
    // file recompiles.
    SKSE::log::info("CostumeExpansionFW loaded (build " __DATE__ " " __TIME__ ")");

    // Honor the external kill-switch as early as possible. When disabled we return
    // true (so the DLL still loads and its ESP masters resolve) but register
    // NOTHING - no trampoline, no serialization, no Papyrus, no message listener.
    // CEF becomes wholly inert, so a save a CEF CTD left unloadable opens clean:
    // the stale co-save chunk is simply skipped (no handler), and any MCM native
    // call returns None (a harmless Papyrus warning, never a crash).
    if (CostumeFW::HardDisabled()) {
        SKSE::log::warn("CostumeExpansionFW is DISABLED via external config - doing nothing");
        return true;
    }

    SKSE::AllocTrampoline(64);  // for the console CompileAndRun hook
    CostumeFW::InstallSerialization();
    SKSE::GetPapyrusInterface()->Register(CostumeFW::RegisterPapyrus);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
