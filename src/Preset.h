#pragma once

#include <string>
#include <vector>

// Distributable costume presets: CEFP_<name>.json under
// Data\SKSE\Plugins\CEF\Presets\. A preset is just a named, human-readable set of
// content (colon-form ARMA ids) plus metadata, the unit creators share. A box can
// be ASSIGNED a preset (see BoxStore AssignPreset), which makes the box show the
// preset's contents. Assignment is exclusive (one preset <-> one box).
namespace CostumeFW::Preset
{
    struct PresetInfo
    {
        std::string name;          // display name (from json "name", else filename stem)
        std::string file;          // file name only, e.g. "CEFP_MyOutfit.json"
        std::string author;
        std::string description;
        std::vector<std::string> requiredPlugins;
        std::vector<std::string> contents;  // colon-form ids
        bool valid{ false };       // parsed ok
    };

    // Scan the preset folder for CEFP_*.json. Returns each preset fully read (light
    // enough; presets are small). Sorted by display name.
    std::vector<PresetInfo> List();

    // Read one preset by file name (e.g. "CEFP_X.json"). valid=false on failure.
    PresetInfo Read(const std::string& a_file);

    // Write a preset built from a_contents. The file is CEFP_<a_name>.json (a_name
    // is sanitized; a "CEFP_" prefix in a_name is not duplicated). If that file
    // already exists, a _1/_2/... suffix is appended (higher = later). Returns the
    // file name written ("" on failure). Creates the folder if missing.
    std::string Export(const std::string& a_name, const std::vector<std::string>& a_contents,
        const std::string& a_author = {}, const std::string& a_description = {});

    // Split contents into resolvable vs missing (FormID/plugin not loaded) so an
    // import can skip + report the missing ones instead of silently failing.
    void Validate(const std::vector<std::string>& a_contents,
        std::vector<std::string>& a_resolvable, std::vector<std::string>& a_missing);
}
