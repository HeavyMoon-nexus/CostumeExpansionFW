#include "BoxStore.h"
#include "SkinRebind.h"

#include "RE/A/ActorEquipManager.h"
#include "RE/B/BGSBipedObjectForm.h"
#include "RE/B/BGSKeyword.h"
#include "RE/C/ConcreteFormFactory.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/I/IFormFactory.h"
#include "RE/I/InventoryEntryData.h"
#include "RE/M/MagicSystem.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/S/Sexes.h"
#include "RE/S/SpellItem.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"
#include "RE/T/TESObjectARMA.h"
#include "RE/T/TESObjectARMO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Windows.h>  // CreateProcess (auto-sync child process)

namespace CostumeFW
{
    namespace
    {
        // Resolved through MO2's VFS as Data\SKSE\Plugins\...; runtime writes land
        // in the MO2 overwrite folder and read back through the same path.
        constexpr const char* kSettingsPath = "Data\\SKSE\\Plugins\\CEF_settings.json";
        constexpr const char* kOldBoxesPath = "Data\\SKSE\\Plugins\\costume_boxes.json";  // migrated
        constexpr const char* kSchema = "cef.settings/1";

        // GLOBAL box definitions (config; all saves). One box per token. Mutated
        // only on the main thread (loads + queued native tasks); MCM queries read
        // it from the VM thread, tolerated like the rest of the registry.
        std::vector<BoxDefInfo> g_boxes;

        // Master CEF on/off (Main page). Persisted in CEF_settings.json.
        bool g_cefEnabled = true;

        // Persist content (colon-form ARMA ids): always-injected, token-less. Phase 1
        // stores + injects these; worn-capture UI and seed retirement come in the MCM
        // rework (the capture flow mirrors a box but with no token gate).
        std::vector<std::string> g_persist;

        // Hide-when-worn rules (§8.10): content colon-id -> vanilla biped slots
        // (30-61) that, while occupied by non-CEF gear, hide that content. GLOBAL
        // config; keyed by content id so persist + box are covered uniformly.
        std::unordered_map<std::string, std::vector<int>> g_hideRules;

        // Forced-gender NIF mode: content colon-id -> 0 player / 1 male / 2 female.
        // Absent entry = 0 (follow player). GLOBAL config, content-keyed.
        std::unordered_map<std::string, int> g_genderModes;

        // Captured worn enchantment per content: content colon-id -> effect list
        // (MGEF colon-id + magnitude). Snapshots the EFFECTIVE enchantment at
        // capture (base OR player/instance), since the base ARMO alone misses
        // instance enchantments. GLOBAL config, content-keyed.
        struct EnchEffect
        {
            std::string mgef;  // MGEF colon-id
            float magnitude{ 0.0f };
        };
        std::unordered_map<std::string, std::vector<EnchEffect>> g_contentEnchants;

        // Persist class's applied preset name ("" = manual). Mirrors a box's preset.
        std::string g_persistPreset;

        int FindBox(const std::string& a_token)
        {
            for (std::size_t i = 0; i < g_boxes.size(); ++i) {
                if (g_boxes[i].token == a_token) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        void WriteCarrierManifest();  // fwd (defined below)

        void WriteJson()
        {
            nlohmann::json doc;
            doc["schema"] = kSchema;
            doc["enabled"] = g_cefEnabled;
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                nlohmann::json jb;
                jb["label"] = b.label;
                jb["token"] = b.token;
                jb["contents"] = b.contents;
                jb["ability"] = b.ability;
                jb["enabled"] = b.enabled;
                jb["armorType"] = b.armorType;
                jb["preset"] = b.preset;
                jb["uiVisible"] = b.uiVisible;
                jb["wear"] = b.wear;
                arr.push_back(std::move(jb));
            }
            doc["boxes"] = std::move(arr);
            doc["persist"]["contents"] = g_persist;
            doc["persist"]["preset"] = g_persistPreset;

            auto rules = nlohmann::json::object();
            for (const auto& [id, slots] : g_hideRules) {
                rules[id] = slots;
            }
            doc["hideRules"] = std::move(rules);

            auto genders = nlohmann::json::object();
            for (const auto& [id, mode] : g_genderModes) {
                genders[id] = mode;
            }
            doc["genderModes"] = std::move(genders);

            auto enchants = nlohmann::json::object();
            for (const auto& [id, effs] : g_contentEnchants) {
                auto arr2 = nlohmann::json::array();
                for (const auto& e : effs) {
                    arr2.push_back({ { "mgef", e.mgef }, { "mag", e.magnitude } });
                }
                enchants[id] = std::move(arr2);
            }
            doc["enchants"] = std::move(enchants);

            std::ofstream f(kSettingsPath, std::ios::trunc);
            if (!f) {
                SKSE::log::error("settings: cannot write {}", kSettingsPath);
                return;
            }
            f << doc.dump(2);
            SKSE::log::info("settings: wrote {} box def(s) (enabled={})", g_boxes.size(), g_cefEnabled);
            WriteCarrierManifest();
        }

        // Plugin that ships the box-token pool (Costume Box 1..N ARMOs).
        constexpr std::string_view kTokenPlugin = "CostumeFW_Boxes.esp";

        // Build the project's colon-form id "XXXXXX:Plugin.esp" from a form: its
        // plugin-local FormID (ESL-masked) + defining plugin filename.
        std::string MakeColonId(RE::TESForm* a_form)
        {
            char buf[8]{};
            std::snprintf(buf, sizeof(buf), "%06X", a_form->GetLocalFormID());
            auto* file = a_form->GetFile(0);
            const std::string plugin = file ? std::string(file->GetFilename()) : std::string{};
            return std::string(buf) + ":" + plugin;
        }

        // The biped slot number (30-61) of a single-slot ARMO (the lowest set
        // bit + 30), 0 if none. Used to order/identify the slot-tokens.
        int SlotNumberOf(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) {
                return 0;
            }
            const std::uint32_t mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            for (int bit = 0; bit < 32; ++bit) {
                if (mask & (1u << bit)) {
                    return bit + 30;
                }
            }
            return 0;
        }

        RE::TESObjectARMO* ResolveArmo(const std::string& a_colonId);  // fwd (defined below)
        void SetTokenStats(const BoxDefInfo& a_box);                   // fwd (defined below)
        void ResetTokenStats(const std::string& a_token);              // fwd (defined below)

        // --- FSMP carrier manifest (approach B) --------------------------------
        // Inputs for tools/nifcarrier `sync`: per box, the resolved worn-NIF path
        // of every content. sync rebuilds Box<slot>_carrier.nif (+ merged physics
        // XML when 2+ contents carry SMP) from this; the per-token ARMA points at
        // that carrier, so equipping the token makes FSMP grow the physics bones
        // CEF's rebind then binds the injected meshes to. Written on every box-def
        // persist, skipped when nothing changed (keeps sync's hash-skip effective).
        constexpr const char* kManifestPath = "Data\\SKSE\\Plugins\\CEF_carrier_manifest.json";

        void ScheduleAutoSync();  // fwd (defined below)

        void WriteCarrierManifest()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            nlohmann::json doc;
            doc["version"] = 1;
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                nlohmann::json jb;
                jb["slot"] = SlotNumberOf(ResolveArmo(b.token));
                jb["token"] = b.token;
                auto contents = nlohmann::json::array();
                for (const auto& id : b.contents) {
                    const auto colon = id.find(':');
                    if (colon == std::string::npos) {
                        continue;
                    }
                    const auto lid = static_cast<std::uint32_t>(
                        std::strtoul(id.substr(0, colon).c_str(), nullptr, 16));
                    const std::string plugin = id.substr(colon + 1);
                    auto* arma = dh->LookupForm<RE::TESObjectARMA>(lid, plugin);
                    if (!arma) {
                        if (auto* armo = dh->LookupForm<RE::TESObjectARMO>(lid, plugin);
                            armo && !armo->armorAddons.empty()) {
                            arma = armo->armorAddons.front();
                        }
                    }
                    if (!arma) {
                        continue;
                    }
                    // Female 3P model first (CEF's dominant authoring case), male fallback
                    // - mirrors ResolveArmaModels' sex fallback.
                    const char* nif = arma->bipedModels[RE::SEXES::kFemale].model.c_str();
                    if (!nif || !*nif) {
                        nif = arma->bipedModels[RE::SEXES::kMale].model.c_str();
                    }
                    if (!nif || !*nif) {
                        continue;
                    }
                    contents.push_back({ { "id", id }, { "nif", nif } });
                }
                jb["contents"] = std::move(contents);
                arr.push_back(std::move(jb));
            }
            doc["boxes"] = std::move(arr);

            const std::string out = doc.dump(1);
            {
                std::ifstream prev(kManifestPath);
                if (prev) {
                    const std::string cur((std::istreambuf_iterator<char>(prev)),
                        std::istreambuf_iterator<char>());
                    if (cur == out) {
                        return;
                    }
                }
            }
            std::ofstream f(kManifestPath, std::ios::trunc);
            if (!f) {
                SKSE::log::error("carrier manifest: cannot write {}", kManifestPath);
                return;
            }
            f << out;
            SKSE::log::info(
                "carrier manifest updated ({} box(es)) - rebuilding FSMP carriers",
                g_boxes.size());
            ScheduleAutoSync();
        }

        // --- carrier revision overrides (restart-free swaps) --------------------
        // tools/nifcarrier `sync` rewrites a PRE-CREATED revision slot file and
        // records it in carriers.json. usvfs shows external REWRITES of existing
        // files but never externally-created NEW files (verified in-game
        // 2026-07-03), which is exactly why the slots are pre-created. Repointing
        // the token ARMA at the new slot path makes the next equip load a path the
        // engine has not cached this session = the freshly built carrier. This is
        // a volatile in-memory form edit - reapplied on every settings load.
        constexpr const char* kCarriersJsonPath = "Data\\meshes\\CostumeFW\\carriers.json";

        // Tokens with a carrier swap mid-flight (detach -> unequip -> equip ->
        // verification passes). A new revision arriving during a swap must not
        // start a second interleaved sequence - it re-applies once the slot frees.
        std::unordered_set<std::string> g_swapInProgress;
        std::atomic<bool> g_reapplyQueued{ false };

        void ApplyCarrierOverridesImpl(bool a_refreshChanged);  // fwd (recursion via requeue)

        // The swap sequencer. In-game 2026-07-03 showed two failure modes of the
        // naive "repoint + re-equip": (1) injected meshes re-bound to the OLD
        // carrier's renamed bones in the unequip->clean window - a bind that looks
        // successful but the nodes die at FSMP's next cleanArmor sweep, leaving
        // invisible/broken meshes the retry system never touches ("bound" != needs
        // retry); (2) the new carrier's async model load outlives the rebind-retry
        // budget. So: detach the box's contents FIRST (nothing can hold stale
        // bones), unequip, equip after the detach settles, then run detach+
        // Reconcile verification passes while the new carrier attaches.
        void RunCarrierSwap(const std::string& a_token, const std::vector<std::string>& a_contents)
        {
            const std::uint32_t formId = ResolveFormId(a_token);
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!player || !obj || player->GetWornArmor(formId) == nullptr) {
                return;  // not worn: next normal equip loads the new path anyway
            }
            auto* eqm = RE::ActorEquipManager::GetSingleton();
            if (!eqm) {
                return;
            }
            g_swapInProgress.insert(a_token);
            for (const auto& c : a_contents) {
                DetachSkinned(c);  // pre-detach: no stale binds to dying bones
            }
            eqm->UnequipObject(player, obj);
            const std::string token = a_token;
            const std::vector<std::string> contents = a_contents;
            RunAfterDelayMs(600, [formId, token, contents]() {
                auto* pl = RE::PlayerCharacter::GetSingleton();
                auto* f = RE::TESForm::LookupByID(formId);
                auto* o = f ? f->As<RE::TESBoundObject>() : nullptr;
                if (pl && o) {
                    if (auto* m = RE::ActorEquipManager::GetSingleton()) {
                        m->EquipObject(pl, o);
                        SKSE::log::info("carrier swap: re-equipped token '{}'", token);
                    }
                }
                // Verification passes: the fresh carrier attaches asynchronously
                // (multi-MB NIFs take seconds). Detach + Reconcile re-runs the
                // bind cleanly; anything still static falls to the retry system.
                auto verify = [token, contents](bool a_last) {
                    for (const auto& c : contents) {
                        DetachSkinned(c);
                    }
                    Reconcile();
                    if (a_last) {
                        g_swapInProgress.erase(token);
                        SKSE::log::info("carrier swap: verification complete for token '{}'", token);
                    }
                };
                RunAfterDelayMs(1500, [verify]() { verify(false); });
                RunAfterDelayMs(4000, [verify]() { verify(true); });
            });
        }

        void ApplyCarrierOverridesImpl(bool a_refreshChanged)
        {
            std::ifstream f(kCarriersJsonPath);
            if (!f) {
                return;  // no carriers.json = nothing to override (ESP defaults apply)
            }
            nlohmann::json doc;
            try {
                f >> doc;
            } catch (const std::exception& e) {
                SKSE::log::warn("carriers.json parse failed: {}", e.what());
                return;
            }
            for (const auto& b : g_boxes) {
                auto* armo = ResolveArmo(b.token);
                if (!armo || armo->armorAddons.empty()) {
                    continue;
                }
                const auto key = std::to_string(SlotNumberOf(armo));
                if (!doc.contains(key)) {
                    continue;
                }
                const std::string file = doc[key].value("file", "");
                if (file.empty()) {
                    continue;
                }
                auto* arma = armo->armorAddons.front();
                const char* cur = arma->bipedModels[RE::SEXES::kFemale].model.c_str();
                if (cur && file == cur) {
                    continue;  // already on this revision
                }
                arma->bipedModels[RE::SEXES::kMale].model = file.c_str();
                arma->bipedModels[RE::SEXES::kFemale].model = file.c_str();
                SKSE::log::info("carrier override: slot {} token '{}' -> {}", key, b.token, file);
                if (a_refreshChanged) {
                    if (g_swapInProgress.contains(b.token)) {
                        // Mid-swap: never interleave a second unequip/equip chain
                        // (the in-flight equip already loads the newest path). Try
                        // the whole apply again once the slot frees.
                        if (!g_reapplyQueued.exchange(true)) {
                            RunAfterDelayMs(6000, []() {
                                g_reapplyQueued = false;
                                ApplyCarrierOverridesImpl(true);
                            });
                        }
                        continue;
                    }
                    RunCarrierSwap(b.token, b.contents);
                }
            }
        }

        // --- auto-sync: spawn nifcarrier when the manifest changes ---------------
        // Full hands-off loop: manifest change (the exact "box content set changed"
        // signal) -> debounce -> spawn the sync command as a child process -> wait
        // for it on a background thread -> ApplyCarrierOverridesImpl(true) on the
        // main thread (slot repoint + two-phase re-equip). Enabled by the presence
        // of CEF_sync_command.txt (one line: the command to run); absent = manual
        // mode (run sync_carriers.cmd + `cef carriers` yourself).
        constexpr const char* kSyncCommandPath = "Data\\SKSE\\Plugins\\CEF_sync_command.txt";
        constexpr int kSyncDebounceMs = 2000;     // MCM edits come in bursts
        constexpr DWORD kSyncTimeoutMs = 120000;  // safety net for a wedged child

        std::atomic<bool> g_syncScheduled{ false };
        std::atomic<bool> g_syncRunning{ false };
        std::atomic<bool> g_syncRerun{ false };  // manifest changed while a sync ran

        std::string ReadSyncCommand()
        {
            std::ifstream f(kSyncCommandPath);
            if (!f) {
                return {};
            }
            std::string line;
            std::getline(f, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            return line;
        }

        void SpawnSyncProcess()
        {
            const std::string cmd = ReadSyncCommand();
            if (cmd.empty()) {
                return;
            }
            if (g_syncRunning.exchange(true)) {
                g_syncRerun = true;  // coalesce: rerun once the current child exits
                return;
            }
            std::string full = "cmd /c \"" + cmd + "\"";
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::vector<char> buf(full.begin(), full.end());
            buf.push_back('\0');
            if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                    nullptr, nullptr, &si, &pi)) {
                SKSE::log::error("auto-sync: CreateProcess failed ({}) for: {}", GetLastError(), cmd);
                g_syncRunning = false;
                return;
            }
            SKSE::log::info("auto-sync: rebuilding carriers ({})", cmd);
            CloseHandle(pi.hThread);
            std::thread([h = pi.hProcess]() {
                DWORD code = 1;
                if (WaitForSingleObject(h, kSyncTimeoutMs) == WAIT_OBJECT_0) {
                    GetExitCodeProcess(h, &code);
                } else {
                    SKSE::log::warn("auto-sync: nifcarrier timed out");
                }
                CloseHandle(h);
                g_syncRunning = false;
                if (g_syncRerun.exchange(false)) {
                    SpawnSyncProcess();
                    return;
                }
                if (code == 0) {
                    SKSE::GetTaskInterface()->AddTask([]() {
                        SKSE::log::info("auto-sync: done - applying carrier revisions");
                        ApplyCarrierOverridesImpl(true);
                    });
                } else {
                    SKSE::log::error("auto-sync: nifcarrier failed (exit {}) - carriers unchanged", code);
                }
            }).detach();
        }

        void ScheduleAutoSync()
        {
            if (ReadSyncCommand().empty()) {
                return;  // manual mode
            }
            if (g_syncScheduled.exchange(true)) {
                return;  // a debounced spawn is already pending
            }
            RunAfterDelayMs(kSyncDebounceMs, []() {
                g_syncScheduled = false;
                SpawnSyncProcess();
            });
        }
    }

    void LoadBoxes()
    {
        g_boxes.clear();
        g_persist.clear();
        g_hideRules.clear();
        g_genderModes.clear();
        g_contentEnchants.clear();
        g_persistPreset.clear();
        g_cefEnabled = true;

        // Read CEF_settings.json; fall back to the legacy costume_boxes.json once
        // (migration) and rewrite into the new file at the end of load.
        bool migrated = false;
        std::ifstream f(kSettingsPath);
        if (!f) {
            std::ifstream old(kOldBoxesPath);
            if (!old) {
                SKSE::log::info("settings: no {} (no settings yet)", kSettingsPath);
                return;
            }
            SKSE::log::info("settings: migrating legacy {} -> {}", kOldBoxesPath, kSettingsPath);
            f.swap(old);
            migrated = true;
        }
        nlohmann::json doc;
        try {
            f >> doc;
        } catch (const std::exception& e) {
            SKSE::log::error("settings: JSON parse error: {}", e.what());
            return;
        }

        g_cefEnabled = doc.value("enabled", true);
        const auto boxes = doc.value("boxes", nlohmann::json::array());
        for (const auto& jb : boxes) {
            BoxDefInfo b;
            b.label = jb.value("label", std::string{});
            b.token = jb.value("token", std::string{});
            if (b.token.empty()) {
                continue;
            }
            for (const auto& c : jb.value("contents", nlohmann::json::array())) {
                if (c.is_string()) {
                    b.contents.push_back(c.get<std::string>());
                }
            }
            b.ability = jb.value("ability", std::string{});
            b.enabled = jb.value("enabled", true);
            b.armorType = jb.value("armorType", 0);
            b.preset = jb.value("preset", std::string{});
            b.uiVisible = jb.value("uiVisible", true);
            b.wear = jb.value("wear", false);
            g_boxes.push_back(std::move(b));
        }
        const auto persist = doc.value("persist", nlohmann::json::object());
        for (const auto& c : persist.value("contents", nlohmann::json::array())) {
            if (c.is_string()) {
                g_persist.push_back(c.get<std::string>());
            }
        }
        g_persistPreset = persist.value("preset", std::string{});
        const auto rules = doc.value("hideRules", nlohmann::json::object());
        for (auto it = rules.begin(); it != rules.end(); ++it) {
            std::vector<int> slots;
            for (const auto& s : it.value()) {
                if (s.is_number_integer()) {
                    slots.push_back(s.get<int>());
                }
            }
            if (!slots.empty()) {
                g_hideRules[it.key()] = std::move(slots);
            }
        }
        const auto genders = doc.value("genderModes", nlohmann::json::object());
        for (auto it = genders.begin(); it != genders.end(); ++it) {
            if (it.value().is_number_integer()) {
                const int m = it.value().get<int>();
                if (m >= 1 && m <= 2) {
                    g_genderModes[it.key()] = m;
                }
            }
        }
        const auto enchants = doc.value("enchants", nlohmann::json::object());
        for (auto it = enchants.begin(); it != enchants.end(); ++it) {
            std::vector<EnchEffect> effs;
            for (const auto& e : it.value()) {
                const std::string mgef = e.value("mgef", std::string{});
                if (!mgef.empty()) {
                    effs.push_back({ mgef, e.value("mag", 0.0f) });
                }
            }
            if (!effs.empty()) {
                g_contentEnchants[it.key()] = std::move(effs);
            }
        }
        if (migrated) {
            WriteJson();  // persist into CEF_settings.json (legacy file left as-is)
        }

        // Register every box content for worn-gated injection (no inject yet) and
        // write each box's aggregate armor/weight onto its token.
        int contentCount = 0;
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                if (RegisterBoxById(c, b.token)) {
                    ++contentCount;
                }
            }
            SetTokenStats(b);
        }
        // Persist content: register token-less (always-injected).
        for (const auto& c : g_persist) {
            RegisterBoxById(c, {});  // empty token -> tokenForm 0 -> always show
        }
        SKSE::log::info("settings: loaded {} box(es) ({} content), {} persist, enabled={}",
            g_boxes.size(), contentCount, g_persist.size(), g_cefEnabled);
        // Point each token ARMA at its current carrier revision (carriers.json).
        // No refresh here: tokens haven't equipped yet at load time.
        ApplyCarrierOverridesImpl(false);
    }

    void ApplyCarrierOverrides(bool a_refreshChanged)
    {
        ApplyCarrierOverridesImpl(a_refreshChanged);
    }

    bool CefEnabled()
    {
        return g_cefEnabled;
    }

    bool SetCefEnabled(bool a_on)
    {
        g_cefEnabled = a_on;
        WriteJson();
        return g_cefEnabled;
    }

    bool IsBoxToken(std::uint32_t a_form)
    {
        if (a_form == 0) {
            return false;
        }
        for (const auto& b : g_boxes) {
            if (ResolveFormId(b.token) == a_form) {
                return true;
            }
        }
        return false;
    }

    void ReplenishToken(std::uint32_t a_tokenForm)
    {
        if (a_tokenForm == 0) {
            return;
        }
        // Only replenish a token whose box is ENABLED (distribution on). A disabled
        // box's token is meant to be gone, so don't fight the user removing it.
        bool enabledBox = false;
        for (const auto& b : g_boxes) {
            if (ResolveFormId(b.token) == a_tokenForm) {
                enabledBox = b.enabled;
                break;
            }
        }
        if (!enabledBox) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::LookupByID(a_tokenForm);
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!player || !obj) {
            return;
        }
        const auto counts = player->GetInventoryCounts();
        const auto it = counts.find(obj);
        const std::int32_t have = (it != counts.end()) ? it->second : 0;
        if (have <= 0) {
            player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            SKSE::log::info("boxes: replenished lost token {:08X}", a_tokenForm);
        }
    }

    void RefreshWornToken(const std::string& a_token)
    {
        const std::uint32_t formId = ResolveFormId(a_token);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        // Only refresh if the token is actually worn; otherwise the next normal
        // equip already carries the updated keywords.
        if (!player || !obj || player->GetWornArmor(formId) == nullptr) {
            return;
        }
        auto* eqm = RE::ActorEquipManager::GetSingleton();
        if (!eqm) {
            return;
        }
        eqm->UnequipObject(player, obj);
        eqm->EquipObject(player, obj);
        SKSE::log::debug("boxes: re-equipped worn token '{}' (keyword refresh)", a_token);
    }

    void ReapplyBoxes()
    {
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                RegisterBoxById(c, b.token);
            }
            SetTokenStats(b);  // token fields revert on load - re-apply
        }
        for (const auto& c : g_persist) {
            RegisterBoxById(c, {});  // token-less persist
        }
        SKSE::log::info("settings: reapplied {} box(es) + {} persist", g_boxes.size(), g_persist.size());
    }

    std::vector<std::string> PersistContents()
    {
        return g_persist;
    }

    bool AddPersistContent(const std::string& a_content)
    {
        if (a_content.empty() ||
            std::find(g_persist.begin(), g_persist.end(), a_content) != g_persist.end()) {
            return false;
        }
        g_persist.push_back(a_content);
        WriteJson();
        return true;
    }

    bool RemovePersistContent(const std::string& a_content)
    {
        const auto it = std::find(g_persist.begin(), g_persist.end(), a_content);
        if (it == g_persist.end()) {
            return false;
        }
        g_persist.erase(it);
        g_hideRules.erase(a_content);       // drop any hide rule for the removed content
        g_genderModes.erase(a_content);     // and its gender override
        g_contentEnchants.erase(a_content);  // and its captured enchantment
        WriteJson();
        return true;
    }

    std::vector<int> HideSlotsFor(const std::string& a_id)
    {
        const auto it = g_hideRules.find(a_id);
        return it == g_hideRules.end() ? std::vector<int>{} : it->second;
    }

    bool SetHideSlots(const std::string& a_id, const std::vector<int>& a_slots)
    {
        if (a_id.empty()) {
            return false;
        }
        // Keep only valid vanilla biped slots (30-61), de-duplicated.
        std::vector<int> clean;
        for (const int s : a_slots) {
            if (s >= 30 && s <= 61 &&
                std::find(clean.begin(), clean.end(), s) == clean.end()) {
                clean.push_back(s);
            }
        }
        if (clean.empty()) {
            g_hideRules.erase(a_id);  // empty list = clear the rule
        } else {
            g_hideRules[a_id] = std::move(clean);
        }
        WriteJson();
        return true;
    }

    int GenderModeFor(const std::string& a_id)
    {
        const auto it = g_genderModes.find(a_id);
        return it == g_genderModes.end() ? 0 : it->second;
    }

    bool SetGenderMode(const std::string& a_id, int a_mode)
    {
        if (a_id.empty()) {
            return false;
        }
        if (a_mode == 1 || a_mode == 2) {
            g_genderModes[a_id] = a_mode;
        } else {
            g_genderModes.erase(a_id);  // 0 (or invalid) = follow player
        }
        WriteJson();
        return true;
    }

    std::vector<WornItem> WornArmors()
    {
        std::vector<WornItem> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }
        auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
            return a_obj.Is(RE::FormType::Armor);
        });
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (count <= 0 || !entry || !entry->IsWorn()) {
                continue;
            }
            auto* armo = obj->As<RE::TESObjectARMO>();
            if (!armo) {
                continue;
            }
            // Exclude our own box tokens (they live in CostumeFW_Boxes.esp).
            auto* file = armo->GetFile(0);
            if (file && file->GetFilename() == kTokenPlugin) {
                continue;
            }
            const char* nm = armo->GetFullName();
            out.push_back({ (nm && *nm) ? std::string(nm) : MakeColonId(armo), MakeColonId(armo) });
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::vector<std::string> TokenPool()
    {
        std::vector<std::pair<int, std::string>> pairs;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return {};
        }
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo) {
                continue;
            }
            auto* file = armo->GetFile(0);
            if (!file || file->GetFilename() != kTokenPlugin) {
                continue;
            }
            const char* nm = armo->GetFullName();
            if (nm && std::string_view(nm).starts_with("Costume Box")) {
                pairs.emplace_back(SlotNumberOf(armo), MakeColonId(armo));
            }
        }
        std::sort(pairs.begin(), pairs.end());  // by slot number
        std::vector<std::string> out;
        out.reserve(pairs.size());
        for (auto& p : pairs) {
            out.push_back(std::move(p.second));
        }
        return out;
    }

    std::vector<std::string> FreeTokens()
    {
        std::vector<std::string> out;
        for (const auto& t : TokenPool()) {
            if (FindBox(t) < 0) {
                out.push_back(t);
            }
        }
        return out;
    }

    std::string NextFreeToken()
    {
        const auto free = FreeTokens();
        return free.empty() ? std::string{} : free.front();
    }

    int TokenSlot(const std::string& a_token)
    {
        return SlotNumberOf(ResolveArmo(a_token));
    }

    int BoxIndexForSlot(int a_slot)
    {
        for (std::size_t i = 0; i < g_boxes.size(); ++i) {
            if (SlotNumberOf(ResolveArmo(g_boxes[i].token)) == a_slot) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string LoreBoxContentsForSlot(int a_slot)
    {
        for (const auto& b : g_boxes) {
            if (SlotNumberOf(ResolveArmo(b.token)) != a_slot) {
                continue;
            }
            std::string out;
            for (const auto& c : b.contents) {
                if (!out.empty()) {
                    out += ", ";
                }
                out += ItemDisplayName(c);
            }
            return out;  // "" if this box is empty
        }
        return {};  // no box on this slot
    }

    bool BoxEnabled(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx >= 0 && g_boxes[idx].enabled;
    }

    bool SetBoxEnabled(const std::string& a_token, bool a_enabled)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].enabled = a_enabled;
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // disabled -> token stats cleared, enabled -> applied
        return true;
    }

    int BoxArmorType(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? 0 : g_boxes[idx].armorType;
    }

    bool SetBoxArmorType(const std::string& a_token, int a_type)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].armorType = (a_type >= 0 && a_type <= 2) ? a_type : 0;
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // re-apply armor class onto the token
        return true;
    }

    bool NewBox(const std::string& a_label)
    {
        const std::string token = NextFreeToken();
        if (token.empty()) {
            SKSE::log::warn("boxes: NewBox - token pool exhausted");
            return false;
        }
        return AddBox(a_label, token, {});  // definition only
    }

    std::string ItemDisplayName(const std::string& a_colonId)
    {
        const std::uint32_t formId = ResolveFormId(a_colonId);
        if (formId == 0) {
            return a_colonId;
        }
        auto* form = RE::TESForm::LookupByID(formId);
        auto* full = form ? form->As<RE::TESFullName>() : nullptr;
        const char* nm = full ? full->GetFullName() : nullptr;
        return (nm && *nm) ? std::string(nm) : a_colonId;
    }

    namespace
    {
        // Resolve a colon-form id to a SpellItem (nullptr on failure).
        RE::SpellItem* ResolveSpell(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            if (formId == 0) {
                return nullptr;
            }
            auto* form = RE::TESForm::LookupByID(formId);
            return form ? form->As<RE::SpellItem>() : nullptr;
        }

        bool TokenWorn(const std::string& a_token)
        {
            const std::uint32_t form = ResolveFormId(a_token);
            if (form == 0) {
                return false;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player && player->GetWornArmor(form) != nullptr;
        }

        // --- Stat passthrough ---
        // Armor + weight are written DIRECTLY onto the token ARMO's own fields
        // (armorRating / weight) - the token IS worn equipment, so the engine
        // applies them naturally (real armor scaling, real carried weight). This
        // is correct where a CarryWeight magic effect was NOT (the AV is max
        // capacity, not the item's own weight). See SetTokenStats below.
        // Enchantment effects are still aggregated into a RUNTIME ability spell
        // (dynamic forms aren't serialized -> rebuilt on load, no stacking).

        // token -> synthesized enchant ability (nullptr = built but none). Process-
        // global; cleared on game load (ClearBoxSpellCache).
        std::unordered_map<std::string, RE::SpellItem*> g_boxSpells;
        // The persist class's aggregate enchant ability (persist has no token, so
        // it's kept separately and granted while CEF is enabled).
        RE::SpellItem* g_persistSpell = nullptr;
        bool g_persistSpellBuilt = false;

        void AddEffect(RE::SpellItem* a_spell, RE::EffectSetting* a_mgef, float a_magnitude)
        {
            if (!a_mgef) {
                return;
            }
            auto* eff = new RE::Effect();
            eff->baseEffect = a_mgef;
            eff->effectItem.magnitude = a_magnitude;
            eff->effectItem.area = 0;
            eff->effectItem.duration = 0;
            a_spell->effects.push_back(eff);
        }

        RE::TESObjectARMO* ResolveArmo(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            return form ? form->As<RE::TESObjectARMO>() : nullptr;
        }

        RE::EffectSetting* ResolveMgef(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            return form ? form->As<RE::EffectSetting>() : nullptr;
        }

        // Build a constant-effect self ability from a content list's enchantments.
        // Per content, uses the CAPTURED snapshot (covers player/instance
        // enchantments) if present, else the base ARMO's own enchantment. Returns
        // nullptr if none of the contents contributes an effect.
        RE::SpellItem* BuildEnchantSpell(const std::vector<std::string>& a_contents, const char* a_name)
        {
            std::vector<std::pair<RE::EffectSetting*, float>> effs;
            for (const auto& c : a_contents) {
                const auto snap = g_contentEnchants.find(c);
                if (snap != g_contentEnchants.end()) {
                    for (const auto& e : snap->second) {
                        if (auto* mgef = ResolveMgef(e.mgef)) {
                            effs.emplace_back(mgef, e.magnitude);
                        }
                    }
                } else if (auto* armo = ResolveArmo(c); armo && armo->formEnchanting) {
                    for (auto* e : armo->formEnchanting->effects) {
                        if (e && e->baseEffect) {
                            effs.emplace_back(e->baseEffect, e->effectItem.magnitude);
                        }
                    }
                }
            }
            if (effs.empty()) {
                return nullptr;
            }
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
            auto* spell = factory ? factory->Create() : nullptr;
            if (!spell) {
                SKSE::log::error("boxes: SpellItem factory create failed");
                return nullptr;
            }
            spell->data.spellType = RE::MagicSystem::SpellType::kAbility;
            spell->data.castingType = RE::MagicSystem::CastingType::kConstantEffect;
            spell->data.delivery = RE::MagicSystem::Delivery::kSelf;
            spell->data.costOverride = 0;
            spell->fullName = a_name;
            for (auto& [mgef, mag] : effs) {
                AddEffect(spell, mgef, mag);
            }
            SKSE::log::debug("boxes: synth enchant ability '{}' ({} effect(s))", a_name, effs.size());
            return spell;
        }

        // Build the aggregate ENCHANT ability for a box. Armor + weight are handled
        // separately on the token's own fields (SetTokenStats).
        RE::SpellItem* BuildBoxAbility(const BoxDefInfo& a_box)
        {
            return BuildEnchantSpell(a_box.contents, "Costume Stats");
        }

        // --- Keyword passthrough -------------------------------------------------
        // Aggregate the contents' keywords onto the worn token so consumer mods
        // (WornHasKeyword detectors, perk conditions, etc.) see them. Keywords live
        // on the token's BASE form and are invisible until the token is worn, so we
        // set them at content-change/load time (before equip) - the natural equip
        // event then carries them to any detector. Structural keywords (armor class
        // / slot / material) are excluded so the invisible token isn't mistaken for
        // real armor in perk/equip logic; armor class is handled via armorType.

        // token colon-id -> keywords WE added (so we can remove ONLY ours on the
        // next apply, preserving keywords KID/other mods distributed to the token).
        // Process-global; base-form keyword arrays reset to plugin+KID state on a
        // process restart, so this never accumulates across sessions.
        std::unordered_map<std::string, std::vector<RE::BGSKeyword*>> g_boxKeywords;

        bool KeywordPassThroughAllowed(std::string_view a_ed)
        {
            for (const auto p : { "OCF_", "ArmorMaterial", "WeapType", "WAF_" }) {
                if (a_ed.starts_with(p)) {
                    return false;
                }
            }
            static const std::unordered_set<std::string_view> kBlocked{
                "ArmorHeavy", "ArmorLight", "ArmorClothing", "ArmorJewelry",
                "ArmorCuirass", "ArmorBoots", "ArmorGauntlets", "ArmorHelmet",
                "ArmorShield", "ArmorBracer", "ArmorCirclet",
                "ClothingBody", "ClothingHead", "ClothingFeet", "ClothingHands",
                "ClothingRing", "ClothingNecklace", "ClothingCirclet",
                "VendorItemJewelry", "VendorItemClothing", "VendorItemArmor"
            };
            return !kBlocked.contains(a_ed);
        }

        // Remove the keywords we previously added to this token (preserving any
        // distributed by other mods). Pass an already-resolved ARMO if available.
        void ClearTokenKeywords(const std::string& a_token, RE::TESObjectARMO* a_token3)
        {
            auto it = g_boxKeywords.find(a_token);
            if (it == g_boxKeywords.end()) {
                return;
            }
            if (a_token3) {
                for (auto* kw : it->second) {
                    a_token3->RemoveKeyword(kw);
                }
            }
            it->second.clear();
        }

        // Re-apply the contents' (filtered) keyword union onto the token, diffing
        // against what we added last time so KID/other keywords survive.
        void SetTokenKeywords(const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_box.token);
            if (!token) {
                return;
            }
            ClearTokenKeywords(a_box.token, token);
            auto& mine = g_boxKeywords[a_box.token];
            if (a_box.enabled) {
                for (const auto& c : a_box.contents) {
                    auto* armo = ResolveArmo(c);
                    if (!armo) {
                        continue;
                    }
                    for (auto* kw : armo->GetKeywords()) {
                        if (!kw) {
                            continue;
                        }
                        const char* ed = kw->formEditorID.c_str();
                        if (!ed || !*ed || !KeywordPassThroughAllowed(ed)) {
                            continue;
                        }
                        // AddKeyword returns true only if newly added; record only
                        // those so we never strip a pre-existing (KID) keyword later.
                        if (token->AddKeyword(kw)) {
                            mine.push_back(kw);
                        }
                    }
                }
            }
            SKSE::log::debug("boxes: token '{}' passthrough keywords +{}", a_box.token, mine.size());
        }

        // Write a box's contents' armor + weight DIRECTLY onto its token ARMO's
        // fields, so the worn token provides them through the normal equip system.
        // armorRating is stored as CK-value * 100. Sums 0 -> token fields cleared.
        void SetTokenStats(const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_box.token);
            if (!token) {
                return;
            }
            float armorSum = 0.0f;
            float weightSum = 0.0f;
            if (a_box.enabled) {
                for (const auto& c : a_box.contents) {
                    if (auto* armo = ResolveArmo(c)) {
                        armorSum += armo->GetArmorRating();
                        weightSum += armo->weight;
                    }
                }
            }
            token->armorRating = static_cast<std::uint32_t>(armorSum * 100.0f);
            token->weight = weightSum;

            // Armor class: Clothing ignores armorRating for DR, so a box holding
            // armor must be Light/Heavy. Map our code (0=Cloth/1=Light/2=Heavy) to
            // the engine enum (kLightArmor=0, kHeavyArmor=1, kClothing=2).
            using AT = RE::BIPED_MODEL::ArmorType;
            AT type = AT::kClothing;
            if (a_box.armorType == 1) {
                type = AT::kLightArmor;
            } else if (a_box.armorType == 2) {
                type = AT::kHeavyArmor;
            }
            token->bipedModelData.armorType = type;

            SKSE::log::debug("boxes: token '{}' stats armor={} weight={} type={}",
                a_box.token, armorSum, weightSum, a_box.armorType);

            SetTokenKeywords(a_box);  // aggregate contents' keywords onto the token
        }

        // Clear a token ARMO's stat fields (freed box / disabled).
        void ResetTokenStats(const std::string& a_token)
        {
            auto* token = ResolveArmo(a_token);
            if (token) {
                token->armorRating = 0;
                token->weight = 0.0f;
            }
            ClearTokenKeywords(a_token, token);  // strip our passthrough keywords
        }

        // Get (build + cache) the synthesized ability for a token, or nullptr.
        RE::SpellItem* BoxAbilityFor(const BoxDefInfo& a_box)
        {
            auto it = g_boxSpells.find(a_box.token);
            if (it != g_boxSpells.end()) {
                return it->second;
            }
            RE::SpellItem* spell = BuildBoxAbility(a_box);
            g_boxSpells[a_box.token] = spell;
            return spell;
        }
    }

    std::vector<WornItem> AbilityCatalog()
    {
        std::vector<WornItem> out;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return out;
        }
        for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
            if (!spell) {
                continue;
            }
            auto* file = spell->GetFile(0);
            if (!file || file->GetFilename() != kTokenPlugin) {
                continue;
            }
            const char* nm = spell->GetFullName();
            if (nm && std::string_view(nm).starts_with("Costume:")) {
                out.push_back({ std::string(nm), MakeColonId(spell) });
            }
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::string BoxAbility(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].ability;
    }

    bool SetBoxAbility(const std::string& a_token, const std::string& a_ability)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].ability = a_ability;  // caller applies on the main thread
        WriteJson();
        return true;
    }

    void RemoveBoxAbilitySpell(const std::string& a_ability)
    {
        if (a_ability.empty()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell = ResolveSpell(a_ability);
        if (player && spell && player->HasSpell(spell)) {
            player->RemoveSpell(spell);
        }
    }

    bool CaptureEnchant(const std::string& a_content)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t baseId = ResolveFormId(a_content);
        if (!player || baseId == 0) {
            return false;
        }
        // Find the currently-worn player item matching this content's base form and
        // read its EFFECTIVE enchantment (instance/player enchantment if present,
        // else the base enchantment). Must run while the item is still equipped.
        RE::EnchantmentItem* ench = nullptr;
        auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
            return a_obj.Is(RE::FormType::Armor);
        });
        for (auto& [obj, data] : inv) {
            if (!obj || obj->GetFormID() != baseId) {
                continue;
            }
            const auto& [count, entry] = data;
            if (count <= 0 || !entry || !entry->IsWorn()) {
                continue;
            }
            ench = entry->GetEnchantment();
            break;
        }
        std::vector<EnchEffect> effs;
        if (ench) {
            for (auto* e : ench->effects) {
                if (e && e->baseEffect) {
                    effs.push_back({ MakeColonId(e->baseEffect), e->effectItem.magnitude });
                }
            }
        }
        const bool has = !effs.empty();
        if (has) {
            SKSE::log::info("boxes: captured {} enchant effect(s) for '{}'", effs.size(), a_content);
            g_contentEnchants[a_content] = std::move(effs);
        } else {
            g_contentEnchants.erase(a_content);
        }
        WriteJson();
        return has;
    }

    namespace
    {
        void SyncSpell(RE::Actor* a_player, RE::SpellItem* a_spell, bool a_worn)
        {
            if (!a_spell) {
                return;
            }
            const bool has = a_player->HasSpell(a_spell);
            if (a_worn && !has) {
                a_player->AddSpell(a_spell);
            } else if (!a_worn && has) {
                a_player->RemoveSpell(a_spell);
            }
        }

        // Get (build + cache) the persist class's aggregate enchant ability.
        RE::SpellItem* PersistAbilityFor()
        {
            if (!g_persistSpellBuilt) {
                g_persistSpell = BuildEnchantSpell(g_persist, "Costume Stats (Persist)");
                g_persistSpellBuilt = true;
            }
            return g_persistSpell;
        }
    }

    void ApplyBoxAbilities()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        for (const auto& b : g_boxes) {
            const bool worn = TokenWorn(b.token);
            // Synthesized ENCHANT ability (armor/weight are on the token's fields).
            SyncSpell(player, BoxAbilityFor(b), worn);
            // Optional manual extra ability (dormant unless set in json).
            if (!b.ability.empty()) {
                SyncSpell(player, ResolveSpell(b.ability), worn);
            }
        }
        // Persist class: no token, always shown while CEF is enabled -> grant its
        // aggregate enchant ability whenever CEF is on.
        SyncSpell(player, PersistAbilityFor(), CefEnabled());
    }

    void RebuildBoxAbility(const std::string& a_token)
    {
        auto it = g_boxSpells.find(a_token);
        if (it == g_boxSpells.end()) {
            return;  // not built yet; ApplyBoxAbilities builds it fresh
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (it->second && player && player->HasSpell(it->second)) {
            player->RemoveSpell(it->second);
        }
        g_boxSpells.erase(it);  // next ApplyBoxAbilities rebuilds + reapplies
    }

    void RebuildPersistAbility()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (g_persistSpell && player && player->HasSpell(g_persistSpell)) {
            player->RemoveSpell(g_persistSpell);
        }
        g_persistSpell = nullptr;
        g_persistSpellBuilt = false;  // next ApplyBoxAbilities rebuilds + reapplies
    }

    void ClearBoxSpellCache()
    {
        // Dynamic ability forms aren't serialized; a save/load drops them from the
        // actor. Just forget our cache so the next apply rebuilds from scratch.
        g_boxSpells.clear();
        g_persistSpell = nullptr;
        g_persistSpellBuilt = false;
    }

    std::string BoxStatsSummary(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        const auto& box = g_boxes[a_index];
        float armorSum = 0.0f;
        float weightSum = 0.0f;
        std::vector<std::string> effs;
        for (const auto& c : box.contents) {
            auto* armo = ResolveArmo(c);
            if (!armo) {
                continue;
            }
            armorSum += armo->GetArmorRating();
            weightSum += armo->weight;
            if (auto* ench = armo->formEnchanting) {
                for (auto* e : ench->effects) {
                    if (!e || !e->baseEffect) {
                        continue;
                    }
                    auto* full = e->baseEffect->As<RE::TESFullName>();
                    const char* nm = full ? full->GetFullName() : nullptr;
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf), "%s %.0f",
                        (nm && *nm) ? nm : "effect", e->effectItem.magnitude);
                    effs.push_back(buf);
                }
            }
        }
        std::string out;
        if (armorSum > 0.0f) {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "Armor +%.0f", armorSum);
            out += buf;
        }
        if (weightSum > 0.0f) {
            char buf[40]{};
            std::snprintf(buf, sizeof(buf), "%sWeight %.1f", out.empty() ? "" : " | ", weightSum);
            out += buf;
        }
        for (const auto& e : effs) {
            out += (out.empty() ? "" : " | ");
            out += e;
        }
        return out.empty() ? "(no stats)" : out;
    }

    int BoxCount()
    {
        return static_cast<int>(g_boxes.size());
    }

    BoxDefInfo BoxAt(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        return g_boxes[a_index];
    }

    bool BoxWornAt(int a_index)
    {
        const std::uint32_t form = BoxTokenFormAt(a_index);
        if (form == 0) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && player->GetWornArmor(form) != nullptr;
    }

    std::uint32_t BoxTokenFormAt(int a_index)
    {
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return 0;
        }
        return ResolveFormId(g_boxes[a_index].token);
    }

    std::vector<std::string> BoxContents(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::vector<std::string>{} : g_boxes[idx].contents;
    }

    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content)
    {
        if (a_token.empty()) {
            return false;
        }
        int idx = FindBox(a_token);
        if (idx < 0) {
            g_boxes.push_back({ a_label, a_token, {} });
            idx = static_cast<int>(g_boxes.size()) - 1;
        } else if (!a_label.empty()) {
            g_boxes[idx].label = a_label;
        }

        auto& box = g_boxes[idx];
        if (!a_content.empty() &&
            std::find(box.contents.begin(), box.contents.end(), a_content) == box.contents.end()) {
            box.contents.push_back(a_content);  // caller registers + reconciles
        }

        WriteJson();
        SetTokenStats(g_boxes[idx]);  // write armor/weight onto the token now
        SKSE::log::info("boxes: AddBox label='{}' token='{}' content='{}'", a_label, a_token, a_content);
        return true;
    }

    bool RemoveBoxContent(const std::string& a_token, const std::string& a_content)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        auto& box = g_boxes[idx];
        const auto it = std::find(box.contents.begin(), box.contents.end(), a_content);
        if (it == box.contents.end()) {
            return false;
        }
        box.contents.erase(it);  // caller detaches the node
        g_hideRules.erase(a_content);       // drop any hide rule for the removed content
        g_genderModes.erase(a_content);     // and its gender override
        g_contentEnchants.erase(a_content);  // and its captured enchantment
        WriteJson();
        SetTokenStats(box);  // recompute token armor/weight
        return true;
    }

    bool RemoveBox(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes.erase(g_boxes.begin() + idx);  // caller detaches each content node
        WriteJson();
        ResetTokenStats(a_token);  // freed token: clear its stat fields
        return true;
    }

    bool SetBoxLabel(const std::string& a_token, const std::string& a_label)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].label = a_label;
        WriteJson();
        return true;
    }

    std::string PresetAssignedTo(const std::string& a_presetName)
    {
        if (a_presetName.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            if (b.preset == a_presetName) {
                return b.token;
            }
        }
        if (g_persistPreset == a_presetName) {
            return "persist";  // sentinel: held by the persist class
        }
        return {};
    }

    std::string PersistPreset()
    {
        return g_persistPreset;
    }

    bool AssignPresetToPersist(const std::string& a_presetName,
        const std::vector<std::string>& a_contents)
    {
        if (a_presetName.empty()) {
            return false;
        }
        // Shared exclusivity pool with boxes: reject if a box already holds it.
        const std::string holder = PresetAssignedTo(a_presetName);
        if (!holder.empty() && holder != "persist") {
            SKSE::log::warn("preset: '{}' already assigned to box {}", a_presetName, holder);
            return false;
        }
        g_persist = a_contents;  // persist mirrors the preset's contents
        g_persistPreset = a_presetName;
        WriteJson();
        SKSE::log::info("preset: assigned '{}' to persist ({} content)",
            a_presetName, a_contents.size());
        return true;
    }

    bool ClearPersistPreset()
    {
        g_persistPreset.clear();  // contents remain (now manual)
        WriteJson();
        return true;
    }

    std::string BoxPreset(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].preset;
    }

    bool AssignPreset(const std::string& a_token, const std::string& a_presetName,
        const std::vector<std::string>& a_contents)
    {
        const int idx = FindBox(a_token);
        if (idx < 0 || a_presetName.empty()) {
            return false;
        }
        // Exclusivity: a preset may be assigned to only one box at a time.
        const std::string holder = PresetAssignedTo(a_presetName);
        if (!holder.empty() && holder != a_token) {
            SKSE::log::warn("preset: '{}' already assigned to box {}", a_presetName, holder);
            return false;
        }
        g_boxes[idx].preset = a_presetName;
        g_boxes[idx].contents = a_contents;  // box mirrors the preset's contents
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // recompute token armor/weight/keywords for new contents
        SKSE::log::info("preset: assigned '{}' to box {} ({} content)",
            a_presetName, a_token, a_contents.size());
        return true;
    }

    bool ClearPreset(const std::string& a_token)
    {
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].preset.clear();  // contents remain (now manual)
        WriteJson();
        return true;
    }
}
