#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Distributable costume presets: CEFP_<name>.json under
// Data\SKSE\Plugins\CEF\Presets\. A preset is just a named, human-readable set of
// content (colon-form ARMA ids) plus metadata, the unit creators share. A box can
// be ASSIGNED a preset (see BoxStore AssignPreset), which makes the box show the
// preset's contents. Assignment is exclusive (one preset <-> one box).
namespace CostumeFW::Preset
{
    // Everything a preset carries about ONE content, so a distributed costume
    // looks the same for the person who receives it. Deliberately a mirror of
    // CostumeFW::ContentSettings (SkinRebind.h) rather than a reuse of it: that
    // header drags in RE, and this layer is pure data + file I/O (it is the one
    // part of CEF a user opens in a text editor).
    //
    // Before v1.6.4 only the first two travelled, so a preset silently dropped
    // the shape hides, the body-morph opt-in and show-real-body - exactly the
    // settings that decide whether a costume sits on the body correctly (F15).
    struct ContentPrefs
    {
        std::vector<int> hideSlots;           // hide-when-worn biped slots (§8.10)
        int genderMode{ 0 };                  // 0 = follow sex, 1 = male, 2 = female
        bool bodyMorph{ false };              // skee body-morph opt-in
        std::vector<std::string> hideShapes;  // per-content shape hide
        bool showRealBody{ false };           // inject the player's own body under it
    };

    struct PresetInfo
    {
        std::string name;          // display name (from json "name", else filename stem)
        std::string file;          // file name only, e.g. "CEFP_MyOutfit.json"
        std::string author;
        std::string description;
        std::vector<std::string> requiredPlugins;
        std::vector<std::string> contents;  // colon-form ids
        // Per-content settings, carried with the preset so the appearance travels
        // with a distributed costume. content id -> prefs; only contents that have
        // something worth carrying appear.
        std::unordered_map<std::string, ContentPrefs> contentPrefs;
        bool valid{ false };       // parsed ok
    };

    // Scan the preset folder for CEFP_*.json. Returns each preset fully read (light
    // enough; presets are small). Sorted by display name.
    std::vector<PresetInfo> List();

    // Read one preset by file name (e.g. "CEFP_X.json"). valid=false on failure.
    PresetInfo Read(const std::string& a_file);

    // Write a preset built from a_contents (+ optional per-content prefs). The
    // file is CEFP_<a_name>.json (a_name is sanitized; a "CEFP_" prefix in a_name is
    // not duplicated). If that file already exists, a _1/_2/... suffix is appended
    // (higher = later). Returns the file name written ("" on failure). Creates the
    // folder if missing.
    std::string Export(const std::string& a_name, const std::vector<std::string>& a_contents,
        const std::unordered_map<std::string, ContentPrefs>& a_prefs = {},
        const std::string& a_author = {}, const std::string& a_description = {});

    // Resolve preset assignments that settings written before v1.6.2.1 stored as
    // a display NAME into the preset FILE that now identifies them (BoxStore
    // hands out the pending names; the folder scan lives here). Call once after
    // the settings load. Missing or ambiguous names stay unresolved; contents stay.
    void MigrateAssignments();

    // Split contents into resolvable vs missing (FormID/plugin not loaded) so an
    // import can skip + report the missing ones instead of silently failing.
    void Validate(const std::vector<std::string>& a_contents,
        std::vector<std::string>& a_resolvable, std::vector<std::string>& a_missing);
}
