#pragma once

#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>
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
    inline void SetupLog()
    {
        auto dir = SKSE::log::log_directory();
        if (!dir) {
            return;
        }
        auto path = *dir / "CostumeExpansionFW.log";
        detail::RotateLogs(path);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }
}
