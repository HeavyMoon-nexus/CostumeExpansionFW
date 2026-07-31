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
#include "Diag.h"
#include "LoreBox.h"
#include "Papyrus.h"
#include "SkinRebind.h"
#include "SmfUI.h"

#include "RE/P/PlayerCharacter.h"
#include "RE/S/ScriptEventSourceHolder.h"
#include "RE/T/TESContainerChangedEvent.h"
#include "RE/T/TESEquipEvent.h"

#include <Windows.h>  // GetModuleHandleW (MARA co-presence triage line)
#include <Psapi.h>    // K32GetProcessMemoryInfo (debug-mode memory line)

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

    // Frame containment: PlayerCharacter::Update is vfunc 0xAD. The engine's
    // light-registration walk over the actor subtree runs INSIDE Update, and the
    // reporter's 2026-07-31 crash proved a stomped holder array gets read there
    // within ~1s of the attach - faster than every existing check (bind watchdog
    // 2.5s, Reconcile-entry). Prologue sweep: contain first, then let the engine
    // walk. Same pure vtable-swap pattern as Load3DHook; ~10 other mods chain on
    // this vfunc in the reporter's stack and compose fine.
    // Escape hatch: [Diagnostics] bFrameContainment=0.
    struct PlayerUpdateHook
    {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta)
        {
            CostumeFW::ContainmentSweepFrame();
            func(a_this, a_delta);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
            func = vtbl.write_vfunc(0xAD, thunk);
            SKSE::log::info("PlayerCharacter::Update containment hook installed (0xAD)");
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
                if (CostumeFW::Diag::Debug()) {
                    SKSE::log::debug("equip: {:08X} {}", a_event->baseObject,
                        a_event->equipped ? "on" : "off");
                }
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

    // Environment census: the co-plugins that shaped past incidents, with the
    // facts a support diff needs (path identity via size + mtime; the crash log
    // carries versions, the CEF log now carries WHICH files were in play). Runs
    // at kDataLoaded - every SKSE plugin is loaded by then; probing at
    // SKSEPluginLoad would miss later-loading DLLs.
    void LogEnvironmentCensus()
    {
        static constexpr const wchar_t* kModules[] = {
            L"hdtSMP64.dll",            // FSMP: merge budget + generation churn
            L"skee64.dll",              // RaceMenu: BodyMorph interface
            L"skyrimbonelimitfix.dll",  // Bone Limit Extender (>80-bone shapes)
            L"SMPFixes.dll",            // ABI-sensitive FSMP companion
            L"MARA.dll",                // capture-blacklist co-presence
            L"EngineFixes.dll",         // allocator/arena behavior
        };
        for (const auto* wname : kModules) {
            const std::string name = std::filesystem::path(wname).string();
            const HMODULE h = ::GetModuleHandleW(wname);
            if (!h) {
                SKSE::log::info("env: {} not loaded", name);
                continue;
            }
            std::string detail = "present";
            wchar_t buf[MAX_PATH]{};
            if (::GetModuleFileNameW(h, buf, MAX_PATH)) {
                try {
                    const std::filesystem::path p{ buf };
                    const auto sz = std::filesystem::file_size(p);
                    const auto ft = std::filesystem::last_write_time(p);
                    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
                    const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
                    std::tm tm{};
                    char ts[32]{ "?" };
                    if (localtime_s(&tm, &tt) == 0) {
                        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
                    }
                    detail = std::format("{} bytes, {}", sz, ts);
                } catch (...) {
                }
            }
            SKSE::log::info("env: {} present ({})", name, detail);
        }
    }

    // Registry roll call: resolve every persisted id once per load and name the
    // failures OUT LOUD. Key drift (a rebuilt / ESL-compacted merge changing its
    // FormIDs) reached the reporter as "my newest entry disappears"; the log now
    // says which entry became an orphan and why that happens.
    void LogRegistryRollCall(const char* a_tag)
    {
        std::size_t ok = 0;
        std::vector<std::string> bad;
        for (const auto& it : CostumeFW::ActiveSnapshot()) {
            if (CostumeFW::CanResolveContent(it.id)) {
                ++ok;
            } else {
                bad.push_back(it.id);
            }
        }
        for (const auto& id : CostumeFW::PersistContents()) {
            if (!CostumeFW::CanResolveContent(id)) {
                bad.push_back("catalog:" + id);
            }
        }
        SKSE::log::info("registry roll call ({}): {} active resolve, {} problem(s)",
            a_tag, ok, bad.size());
        std::size_t shown = 0;
        for (const auto& id : bad) {
            if (++shown > 10) {
                SKSE::log::warn("  ... and {} more", bad.size() - 10);
                break;
            }
            SKSE::log::warn("  UNRESOLVED '{}' - its plugin is missing or its FormIDs "
                            "changed (rebuilt/compacted merge?)", id);
        }
    }

    // Support mirror: copy CEF_settings.json next to the log, so every artifact
    // a report needs lives in ONE real folder (Documents\...\SKSE\) instead of
    // needing "find it in MO2's overwrite" instructions.
    void MirrorSettingsForSupport()
    {
        const auto dir = SKSE::log::log_directory();
        if (!dir) {
            return;
        }
        std::error_code ec;
        std::filesystem::copy_file("Data\\SKSE\\Plugins\\CEF_settings.json",
            *dir / "CEF_settings.mirror.json",
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            SKSE::log::debug("settings mirror skipped: {}", ec.message());
        }
    }

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
            // Compat triage line: MARA (Nexus 173949) keeps runtime-created
            // utility armors ("CORE Carrier") in the inventory that crash
            // third-party UIs on touch; CEF's capture surface skips that class
            // (v1.3.2 blacklist, MARA_COMPAT_PLAN.md). One info line so support
            // reports show the co-presence at a glance. Filename check only -
            // no behavior branches on it.
            if (::GetModuleHandleW(L"MARA.dll") != nullptr) {
                CostumeFW::SetMaraPresent(true);  // gates the M4-J worn-jewelry capture refusal
                SKSE::log::info(
                    "compat: MARA.dll detected - runtime/utility armors are hidden "
                    "from the capture pickers (capture blacklist), and capturing "
                    "WORN jewelry is refused (stripping it crashes MARA - M4-J)");
            }
            LogEnvironmentCensus();
            Load3DHook::Install();
            // VR: PlayerCharacter's vtable inserts virtuals before Update, so the
            // SE/AE index 0xAD would land on the wrong function - VR keeps the
            // 2.5s watchdog + Reconcile-entry sweeps as its containment cadence.
            if (REL::Module::IsVR()) {
                SKSE::log::info(
                    "frame containment: skipped on VR (vfunc index differs) - watchdog "
                    "cadence covers containment");
            } else if (CostumeFW::IniFlag("bframecontainment", true)) {
                PlayerUpdateHook::Install();
            } else {
                SKSE::log::warn(
                    "frame containment DISABLED (CostumeExpansionFW.ini [Diagnostics] "
                    "bFrameContainment=0) - corruption checks fall back to the 2.5s watchdog");
            }
            CostumeFW::InstallLoreBoxHook();  // soft LoreBox tooltip integration
            CostumeFW::InstallConsoleHook();
            CostumeFW::SmfUI::Register();  // SKSE Menu Framework section (soft; no-op without SMF)
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink(EquipSink::GetSingleton());
                holder->AddEventSink(ContainerSink::GetSingleton());
            }
            // Load the GLOBAL box definitions (costume_boxes.json) once.
            SKSE::GetTaskInterface()->AddTask([] {
                CostumeFW::LoadBoxes();
                CostumeFW::Reconcile();
                CostumeFW::ApplyBoxAbilities();
                LogRegistryRollCall("data-loaded");
                MirrorSettingsForSupport();
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
                // Hidden-store handling moved to the co-save (P2): RevertCallback
                // clears the per-save store id at load START, LoadCallback restores
                // THIS save's (ROOT A protection preserved). Clearing it here ran
                // AFTER the restore and wiped it.
                CostumeFW::ClearBoxSpellCache();
                CostumeFW::ReapplyBoxes();
                CostumeFW::ApplyCarrierOverrides(false);
                CostumeFW::Reconcile();
                CostumeFW::ApplyBoxAbilities();
                // Which character this session's lines belong to: M2 states
                // differ per save, and support logs used to leave it implicit.
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    SKSE::log::info("save loaded: player '{}'", pc->GetName());
                }
                LogRegistryRollCall("post-load");
                MirrorSettingsForSupport();
            });
            // Teeth-drop watchdog: the engine's load-time facegen build can drop the
            // mouth from the assembled head when a persist head-carrier (Misc HDPT)
            // shares it. Once the build has settled, one clean rebuild restores it
            // (what racemenu / a persist re-toggle did manually). No-op otherwise.
            CostumeFW::RunAfterDelayMs(4000, [] { CostumeFW::RestoreMouthIfDropped("post-load"); });
            // Merge-race rescue: the load-time injection can run before FSMP
            // finishes merging the persist-carrier bones (slow load orders take
            // tens of seconds), leaving items fully static with their retries
            // burned. Re-arm once the load has settled; silent when converged.
            CostumeFW::RunAfterDelayMs(8000, [] { CostumeFW::RearmStaticBinds("post-load settle"); });
            break;
        default:
            break;
        }
    }
}

namespace CostumeFW::Diag
{
    // Debug-mode memory line (declared in Diag.h; lives here with the Windows
    // includes). Attributes memory balloons (FSMP 3.1.1's 27GB, ForgetSpell
    // 1.2.5) in minutes instead of an evening of bisection.
    void LogMemoryUsageDebugLine()
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            SKSE::log::debug("mem: working set {} MB, private {} MB",
                pmc.WorkingSetSize >> 20, pmc.PrivateUsage >> 20);
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    CostumeFW::SetupLog();
    SKSE::Init(skse);
    // Build stamp: proves WHICH dll the game actually loaded. If the FILE
    // timestamp is older than your last build, MO2's usvfs served a stale
    // cached copy (rebuilding a dll while MO2 is running does NOT refresh it -
    // fully close AND reopen MO2, not just the game). Two stamps because
    // __DATE__/__TIME__ only refresh when THIS file recompiles - an
    // incremental build that links new objects elsewhere keeps the old string
    // (bit us 2026-07-07) - while the file mtime identifies the binary itself.
    {
        std::string fileStamp = "unknown";
        try {
            // Same game-CWD-relative path convention CEF uses everywhere; the
            // read goes through usvfs, so this is the exact file the game got.
            const auto ft =
                std::filesystem::last_write_time("Data\\SKSE\\Plugins\\CostumeExpansionFW.dll");
            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
            const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
            std::tm tm{};
            if (localtime_s(&tm, &tt) == 0) {
                char buf[32]{};
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
                fileStamp = buf;
            }
        } catch (...) {
        }
        SKSE::log::info("CostumeExpansionFW loaded (file {} / compile " __DATE__ " " __TIME__ ")",
            fileStamp);
    }

    // Runtime banner: one NG DLL serves SE/AE/VR, so community reports must show
    // WHICH engine loaded us. VR additionally requires the VR Address Library
    // (Data/SKSE/Plugins/version-1-4-15-0.csv) - REL fails hard without it.
    SKSE::log::info("runtime: Skyrim {} {}",
        REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE"),
        REL::Module::get().version().string("."));

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

    CostumeFW::Diag::InitFromIni();  // two-tier logging: ini form of the debug switch

    SKSE::AllocTrampoline(64);  // for the console CompileAndRun hook
    CostumeFW::InstallSerialization();
    SKSE::GetPapyrusInterface()->Register(CostumeFW::RegisterPapyrus);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
