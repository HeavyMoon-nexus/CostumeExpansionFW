#include "Preset.h"
#include "SkinRebind.h"  // ResolveFormId (validation)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace CostumeFW::Preset
{
    namespace
    {
        // Resolved through MO2's VFS; writes land in the overwrite folder and read
        // back through the same merged view.
        constexpr const char* kPresetDir = "Data\\SKSE\\Plugins\\CEF\\Presets";
        constexpr const char* kSchema = "cef.preset/1";
        constexpr std::string_view kPrefix = "CEFP_";

        // Keep file names filesystem-safe (presets are user-named).
        std::string Sanitize(std::string a_s)
        {
            for (auto& c : a_s) {
                if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos ||
                    static_cast<unsigned char>(c) < 0x20) {
                    c = '_';
                }
            }
            return a_s;
        }

        PresetInfo Parse(const nlohmann::json& a_doc, const std::string& a_file)
        {
            PresetInfo p;
            p.file = a_file;
            // display name: explicit "name", else filename stem.
            p.name = a_doc.value("name", std::string{});
            if (p.name.empty()) {
                p.name = a_file;
                if (auto dot = p.name.rfind(".json"); dot != std::string::npos) {
                    p.name.erase(dot);
                }
            }
            p.author = a_doc.value("author", std::string{});
            p.description = a_doc.value("description", std::string{});
            for (const auto& c : a_doc.value("requiredPlugins", nlohmann::json::array())) {
                if (c.is_string()) {
                    p.requiredPlugins.push_back(c.get<std::string>());
                }
            }
            for (const auto& c : a_doc.value("contents", nlohmann::json::array())) {
                if (c.is_string()) {
                    p.contents.push_back(c.get<std::string>());
                }
            }
            p.valid = true;
            return p;
        }
    }

    std::vector<PresetInfo> List()
    {
        std::vector<PresetInfo> out;
        std::error_code ec;
        const std::filesystem::path dir(kPresetDir);
        if (!std::filesystem::exists(dir, ec)) {
            return out;
        }
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!e.is_regular_file()) {
                continue;
            }
            const auto name = e.path().filename().string();
            const auto low = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            };
            const std::string ln = low(name);
            if (!ln.starts_with("cefp_") || !ln.ends_with(".json")) {
                continue;
            }
            auto p = Read(name);
            if (p.valid) {
                out.push_back(std::move(p));
            }
        }
        std::sort(out.begin(), out.end(),
            [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });
        return out;
    }

    PresetInfo Read(const std::string& a_file)
    {
        PresetInfo p;
        p.file = a_file;
        std::ifstream f(std::filesystem::path(kPresetDir) / a_file);
        if (!f) {
            SKSE::log::warn("preset: cannot open {}", a_file);
            return p;
        }
        nlohmann::json doc;
        try {
            f >> doc;
        } catch (const std::exception& e) {
            SKSE::log::error("preset: JSON parse error in {}: {}", a_file, e.what());
            return p;
        }
        return Parse(doc, a_file);
    }

    std::string Export(const std::string& a_name, const std::vector<std::string>& a_contents,
        const std::string& a_author, const std::string& a_description)
    {
        std::string base = Sanitize(a_name);
        if (base.empty()) {
            base = "preset";
        }
        // Avoid a duplicated prefix if the user already typed CEFP_.
        std::string lowBase = base;
        std::transform(lowBase.begin(), lowBase.end(), lowBase.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string stem = lowBase.starts_with("cefp_")
            ? base.substr(kPrefix.size())
            : base;

        std::error_code ec;
        const std::filesystem::path dir(kPresetDir);
        std::filesystem::create_directories(dir, ec);

        // Suffix _1/_2/... on collision (higher number = saved later).
        std::string file = std::string(kPrefix) + stem + ".json";
        for (int n = 1; std::filesystem::exists(dir / file, ec); ++n) {
            file = std::string(kPrefix) + stem + "_" + std::to_string(n) + ".json";
        }

        nlohmann::json doc;
        doc["schema"] = kSchema;
        doc["name"] = stem;
        doc["author"] = a_author;
        doc["description"] = a_description;
        // Record the defining plugins so an importer can warn on missing masters.
        std::vector<std::string> plugins;
        for (const auto& c : a_contents) {
            if (const auto colon = c.find(':'); colon != std::string::npos) {
                std::string plugin = c.substr(colon + 1);
                if (std::find(plugins.begin(), plugins.end(), plugin) == plugins.end()) {
                    plugins.push_back(std::move(plugin));
                }
            }
        }
        doc["requiredPlugins"] = plugins;
        doc["contents"] = a_contents;

        std::ofstream out(dir / file, std::ios::trunc);
        if (!out) {
            SKSE::log::error("preset: cannot write {}", file);
            return {};
        }
        out << doc.dump(2);
        SKSE::log::info("preset: exported {} ({} content)", file, a_contents.size());
        return file;
    }

    void Validate(const std::vector<std::string>& a_contents,
        std::vector<std::string>& a_resolvable, std::vector<std::string>& a_missing)
    {
        for (const auto& c : a_contents) {
            if (ResolveFormId(c) != 0) {
                a_resolvable.push_back(c);
            } else {
                a_missing.push_back(c);
            }
        }
    }
}
