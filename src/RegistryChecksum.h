#pragma once

// The ability registry's checksum, header-only and free of CommonLib so the
// host test runs the very code the DLL does: tests/registry_checksum.cpp feeds
// it a fixture, and tests/registry_checksum_test.py holds tools/cefjson.py - the
// Python side that edits the registry for tests - to the same answer (review
// 2026-09-23 M13). A disagreement there is a registry CEF refuses at launch.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace CostumeFW::abilities
{
    // FNV-1a, 64-bit.
    [[nodiscard]] inline std::uint64_t Fnv1a(std::string_view a_text)
    {
        std::uint64_t h = 1469598103934665603ull;
        for (const unsigned char c : a_text) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    // What CEF_abilities.json carries as "checksum": FNV-1a over the COMPACT
    // dump of the abilities array, as 16 lowercase hex digits. Throws what
    // dump() throws - text that is not UTF-8.
    [[nodiscard]] inline std::string RegistryChecksum(const nlohmann::json& a_abilities)
    {
        return std::format("{:016x}", Fnv1a(a_abilities.dump()));
    }
}
