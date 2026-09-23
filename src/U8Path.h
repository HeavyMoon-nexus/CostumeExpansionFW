#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace CostumeFW
{
    // UTF-8 text <-> std::filesystem::path, with no ANSI codepage in between.
    //
    // CEF keeps text as UTF-8 (the settings are JSON, ImGui draws UTF-8), but
    // std::filesystem's narrow-string side is the ANSI codepage: constructing a
    // path from a std::string reads it as ACP, and path::string() converts back
    // to ACP - and MSVC's STL THROWS std::system_error when a character has no
    // ACP spelling. A preset named 'CEFP_巫女服.json' on an English-locale PC,
    // or 'CEFP_Café.json' on a Japanese one, crashed every page that lists
    // presets and, through the kDataLoaded assignment migration, every launch
    // (review 2026-09-23 C6). Going through char8_t skips the codepage.
    [[nodiscard]] inline std::filesystem::path U8Path(std::string_view a_utf8)
    {
        return std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(a_utf8.data()), a_utf8.size()));
    }

    [[nodiscard]] inline std::string U8Str(const std::filesystem::path& a_path)
    {
        const std::u8string u = a_path.u8string();
        return { reinterpret_cast<const char*>(u.data()), u.size() };
    }
}
