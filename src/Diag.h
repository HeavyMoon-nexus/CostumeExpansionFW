#pragma once

#include "Config.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace CostumeFW::Diag
{
    // --- two-tier logging ----------------------------------------------------
    // Normal mode carries everything needed to diagnose the FIRST occurrence of
    // a crash (build stamp, stage markers, census, corruption transitions,
    // quarantine) - a roulette CTD cannot be asked to reproduce, so the evidence
    // minimum is always on and logs on change only. Debug mode releases VOLUME:
    // the spdlog debug level (many call sites already log there and are simply
    // filtered out today), the per-tick health poll, memory usage, equip traces.
    //
    // Iron rule: debug mode changes LOGGING ONLY, never behavior. A mode that
    // changes behavior cannot be trusted to reproduce the thing it watches
    // (quarantine and every other defense stays in normal mode).
    inline std::atomic<bool> g_debug{ false };

    inline bool Debug()
    {
        return g_debug.load(std::memory_order_relaxed);
    }

    inline void SetDebugMode(bool a_on, const char* a_source)
    {
        const bool was = g_debug.exchange(a_on);
        auto logger = spdlog::default_logger();
        if (!logger) {
            return;
        }
        if (a_on) {
            logger->set_level(spdlog::level::debug);
            // Crash evidence is only as good as what reached the disk; accept the
            // flush cost - debug mode is explicitly temporary.
            logger->flush_on(spdlog::level::debug);
            if (!was) {
                logger->warn(
                    "DIAGNOSTIC MODE ON ({}) - verbose logging: larger log file and some "
                    "overhead. Play normally; turn it off after the next crash is captured",
                    a_source);
            }
        } else {
            if (was) {
                logger->warn("diagnostic mode off ({})", a_source);
            }
            logger->set_level(spdlog::level::info);
            logger->flush_on(spdlog::level::info);
        }
    }

    // ini override for users without the SMF page:
    //   [Diagnostics]
    //   bDebugMode=1
    // Read once at startup; the SMF Diagnostics checkbox flips the same switch
    // at runtime (session-only - the ini is the persistent form).
    inline void InitFromIni()
    {
        if (IniFlag("bdebugmode", false)) {
            SetDebugMode(true, "CostumeExpansionFW.ini [Diagnostics] bDebugMode=1");
        }
    }

    // --- scene-mutator thread census ----------------------------------------
    // A single "main thread" identity is NOT a real invariant: measured
    // 2026-07-30, the SKSE task pump drained the kDataLoaded task, the
    // post-load task and the injection-time Reconcile on DIFFERENT OS threads
    // (load-phase vs gameplay-phase pumps), so an equality guard cried wolf
    // five times per session - twice. What IS worth recording is every
    // DISTINCT thread a scene mutator executes on, once: a genuine F1
    // recurrence (VM/render thread calling in) appears as a new id with a
    // telling timestamp; engine phase migration appears once per phase and is
    // quiet thereafter.
    inline std::size_t ThisThreadKey()
    {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
    }

    inline void NoteExecutionThread(const char* a_fn)
    {
        static std::mutex s_m;
        static std::unordered_map<std::size_t, int> s_seen;  // id -> first-seen order
        const auto key = ThisThreadKey();
        std::lock_guard lk{ s_m };
        if (s_seen.size() >= 8 || s_seen.contains(key)) {
            return;  // known thread (or enough distinct ids to prove churn)
        }
        const int n = static_cast<int>(s_seen.size()) + 1;
        s_seen.emplace(key, n);
        if (auto logger = spdlog::default_logger()) {
            logger->info("thread census: {} executing on thread #{} (id {:x})", a_fn, n, key);
        }
    }

    // Debug-mode memory line (working set / private bytes). Defined in
    // plugin.cpp, which already owns the <Windows.h> include - keeping the
    // psapi dependency out of the big TUs.
    void LogMemoryUsageDebugLine();
}
