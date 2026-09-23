#pragma once

#include <spdlog/sinks/basic_file_sink.h>

#include <Windows.h>  // GetShortPathNameW (SetupLog's ASCII fallback)

#include <filesystem>
#include <optional>
#include <string>

namespace CostumeFW
{
    namespace detail
    {
        // Keep the two previous runs' logs alongside the current one.
        //
        // The log is truncated at every launch, and a Skyrim problem is
        // routinely found one launch too late: the crash, the stranded
        // modifier, the thing that did not happen - you restart to look, and
        // the evidence is gone. That cost a measurement on 2026-09-12, where
        // two saves that had never been a test of anything were read as two
        // failures because the session log that would have said so had already
        // been overwritten.
        //
        //   CostumeExpansionFW.log         this run
        //   CostumeExpansionFW.prev1.log   the run before
        //   CostumeExpansionFW.prev2.log   the one before that
        //
        // Named so they are obvious to a user asked to attach a log, rather
        // than hidden behind a numeric suffix a text editor will not open.
        //
        // Every step is best-effort and swallows its error: a log that cannot
        // rotate must still open, and a plugin must never fail to load over
        // one. Runs before the sink is created, so nothing holds the file.
        inline void RotateLogs(const std::filesystem::path& a_current)
        {
            namespace fs = std::filesystem;
            const auto sibling = [&a_current](const char* a_stem) {
                return a_current.parent_path() /
                       (a_current.stem().string() + a_stem + a_current.extension().string());
            };
            const auto prev1 = sibling(".prev1");
            const auto prev2 = sibling(".prev2");

            std::error_code ec;
            fs::remove(prev2, ec);
            fs::rename(prev1, prev2, ec);
            fs::rename(a_current, prev1, ec);
        }
    }

    // Routes spdlog (and thus SKSE::log) to
    //   Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log
    //
    // NEVER throws. This is the first line of SKSEPluginLoad, and a throw out of
    // it either crashes the game at startup or - if SKSE swallows it - leaves CEF
    // silently unloaded, and a save written then carries no CEF co-save: the
    // hidden storage, persist items and NPC bindings go with it and nothing says
    // "DO NOT SAVE" (review 2026-09-23 C6 c).
    //
    // The way it threw: spdlog opens files through the ANSI API, so the path is
    // handed over as path::string(), which converts to the ANSI codepage and
    // THROWS when a character has no spelling there - a Japanese user name on an
    // English-locale Windows puts one in the Documents path. So, in order: the
    // normal place; the same folder by its 8.3 short name, which is ASCII; and
    // Data\SKSE\Plugins as the last resort, saying so on the first line. With no
    // log at all the plugin still loads.
    inline void SetupLog()
    {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        std::string where;  // said first when the log is not in its usual place
        const auto open = [&sink](const std::filesystem::path& a_file) {
            try {
                detail::RotateLogs(a_file);
            } catch (...) {
                // Rotation is a convenience; the log itself matters more.
            }
            try {
                sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(a_file.string(), true);
                return true;
            } catch (...) {
                return false;
            }
        };
        std::optional<std::filesystem::path> dir;
        try {
            dir = SKSE::log::log_directory();
        } catch (...) {
        }
        if (dir && !open(*dir / "CostumeExpansionFW.log")) {
            // The same folder by its 8.3 name. Empty when 8.3 names are off on the
            // volume, in which case the next step takes over.
            std::wstring shortDir(MAX_PATH, L'\0');
            const DWORD n = ::GetShortPathNameW(dir->c_str(), shortDir.data(),
                static_cast<DWORD>(shortDir.size()));
            if (n > 0 && n < shortDir.size()) {
                shortDir.resize(n);
                if (open(std::filesystem::path(shortDir) / "CostumeExpansionFW.log")) {
                    where = "the Documents path has characters this system's codepage "
                            "cannot spell - logging through its short (8.3) name";
                }
            }
        }
        if (!sink) {
            if (open(std::filesystem::path("Data\\SKSE\\Plugins\\CostumeExpansionFW.log"))) {
                where = "the usual log folder (Documents\\My Games\\...\\SKSE) could not be "
                        "opened - this log is in Data\\SKSE\\Plugins instead (MO2: overwrite)";
            }
        }
        if (!sink) {
            return;  // no log anywhere; the plugin still loads
        }
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
        if (!where.empty()) {
            SKSE::log::warn("log: {}", where);
        }
    }
}
