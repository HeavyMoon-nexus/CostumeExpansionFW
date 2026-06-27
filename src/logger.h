#pragma once

#include <spdlog/sinks/basic_file_sink.h>

namespace CostumeFW
{
    // Routes spdlog (and thus SKSE::log) to
    //   Documents\My Games\Skyrim Special Edition\SKSE\CostumeExpansionFW.log
    inline void SetupLog()
    {
        auto dir = SKSE::log::log_directory();
        if (!dir) {
            return;
        }
        auto path = *dir / "CostumeExpansionFW.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }
}
