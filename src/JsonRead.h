#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace CostumeFW
{
    // Typed reads for JSON CEF does not fully control: carriers.json (written by
    // the carrier builder, and by hand when someone is debugging), the ability
    // registry, anything a user can open in a text editor.
    //
    // nlohmann's value(key, default) is not the safe read it looks like. It
    // THROWS when the key is present with another type (type_error.302 - "rev":
    // null, "schema": "2") and when the node is not an object at all (306 - a
    // file whose root is an array). A throw that leaves a native, a task or the
    // kDataLoaded handler is a crash, and the ones this replaces crashed at every
    // launch or the moment a diagnostics page opened (review 2026-09-23 C16).
    //
    // These answer the default instead: a field of the wrong type reads as
    // "absent", which every caller already has an answer for. New code reading
    // such a file uses these rather than value().
    [[nodiscard]] inline int JsonInt(const nlohmann::json& a_obj, const char* a_key, int a_def)
    {
        if (!a_obj.is_object()) {
            return a_def;
        }
        const auto it = a_obj.find(a_key);
        return it != a_obj.end() && it->is_number_integer() ? it->get<int>() : a_def;
    }

    [[nodiscard]] inline std::string JsonStr(const nlohmann::json& a_obj, const char* a_key,
        const std::string& a_def = {})
    {
        if (!a_obj.is_object()) {
            return a_def;
        }
        const auto it = a_obj.find(a_key);
        return it != a_obj.end() && it->is_string() ? it->get<std::string>() : a_def;
    }

    [[nodiscard]] inline bool JsonBool(const nlohmann::json& a_obj, const char* a_key, bool a_def)
    {
        if (!a_obj.is_object()) {
            return a_def;
        }
        const auto it = a_obj.find(a_key);
        return it != a_obj.end() && it->is_boolean() ? it->get<bool>() : a_def;
    }

    // The array under a_key, or an empty one when it is missing or is not an
    // array. By reference, so iterating it copies nothing.
    [[nodiscard]] inline const nlohmann::json& JsonArray(const nlohmann::json& a_obj,
        const char* a_key)
    {
        static const nlohmann::json kEmpty = nlohmann::json::array();
        if (!a_obj.is_object()) {
            return kEmpty;
        }
        const auto it = a_obj.find(a_key);
        return it != a_obj.end() && it->is_array() ? *it : kEmpty;
    }
}
