#include "BoxStore.h"
#include "BodyMorph.h"
#include "SkinRebind.h"
#include "nifcarrier/NifCarrierCore.h"

#include "RE/A/ActorEquipManager.h"
#include "RE/B/BGSBipedObjectForm.h"
#include "RE/B/BGSHeadPart.h"
#include "RE/C/ConsoleLog.h"
#include "RE/B/BipedAnim.h"
#include "RE/B/BGSKeyword.h"
#include "RE/C/ConcreteFormFactory.h"
#include "RE/E/Effect.h"
#include "RE/E/EffectSetting.h"
#include "RE/E/EnchantmentItem.h"
#include "RE/I/IFormFactory.h"
#include "RE/I/InventoryEntryData.h"
#include "RE/M/MagicSystem.h"
#include "RE/M/Misc.h"  // RE::DebugNotification
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
#include <cctype>
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
        // Last-known-good copy, snapshotted ONLY after a successful parse at load
        // (never touched by WriteJson, so a corrupt main file can't clobber it).
        constexpr const char* kSettingsBakPath = "Data\\SKSE\\Plugins\\CEF_settings.json.bak";
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

        // Body-morph OPT-IN: content colon-ids whose injected mesh should receive
        // the player's skee body morph (RaceMenu body sliders). Default is OFF
        // (absent = off): body morph is only needed for BodySlide/body-conforming
        // meshes, and applying it to accessories (hair/nails/piercings/veil) is
        // unnecessary AND drove a severe memory balloon (skee ApplyVertexDiff makes
        // huge allocations that SSE Engine Fixes' allocator retains - dump
        // 2026-07-05). CEF's custom-slot content can't be auto-classified (nails/
        // piercings use arbitrary modder-chosen slots), so the user opts in per
        // content. GLOBAL config, content-keyed.
        std::unordered_set<std::string> g_bodyMorphOn;

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

        // M2 (CEF_STATE_SCOPE.md §3): a persist item is ACTIVE on this save when
        // it sits in the injection registry token-less (restored from the co-save
        // or activated live). g_persist is only the shared CATALOG; everything
        // per-save (manifest persist fragment, head-carrier desired set) keys off
        // the ACTIVE set, never the catalog.
        bool AnyPersistActive()
        {
            for (const auto& it : ActiveSnapshot()) {
                if (it.tokenId.empty()) {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::string> ActivePersistIds()
        {
            std::vector<std::string> out;
            for (const auto& it : ActiveSnapshot()) {
                if (it.tokenId.empty()) {
                    out.push_back(it.id);
                }
            }
            // Registry order varies with the restore/add sequence; sort so the
            // manifest is deterministic and nifcarrier's hash-skip stays effective.
            std::sort(out.begin(), out.end());
            return out;
        }

        // Write a_data to a_path via a sibling ".tmp" + atomic rename, so a CTD /
        // process kill mid-write can never leave a truncated file at a_path (the
        // old trunc-overwrite could destroy CEF_settings.json - Codex review
        // 2026-07-05 A-1). MoveFileEx(REPLACE_EXISTING) is atomic on NTFS and is
        // hooked by MO2's usvfs like the rest of the Win32 file API.
        bool WriteFileAtomic(const char* a_path, const std::string& a_data)
        {
            const std::string tmp = std::string(a_path) + ".tmp";
            {
                std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
                if (!f) {
                    return false;
                }
                f << a_data;
                f.flush();
                if (!f.good()) {
                    return false;
                }
            }
            if (!MoveFileExA(tmp.c_str(), a_path, MOVEFILE_REPLACE_EXISTING)) {
                SKSE::log::error("atomic write: MoveFileEx failed ({}) for {}",
                    GetLastError(), a_path);
                DeleteFileA(tmp.c_str());
                return false;
            }
            return true;
        }

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

            // Body-morph opt-in: store the ON content-ids (default off = absent).
            auto morphs = nlohmann::json::array();
            for (const auto& id : g_bodyMorphOn) {
                morphs.push_back(id);
            }
            doc["bodyMorph"] = std::move(morphs);

            auto enchants = nlohmann::json::object();
            for (const auto& [id, effs] : g_contentEnchants) {
                auto arr2 = nlohmann::json::array();
                for (const auto& e : effs) {
                    arr2.push_back({ { "mgef", e.mgef }, { "mag", e.magnitude } });
                }
                enchants[id] = std::move(arr2);
            }
            doc["enchants"] = std::move(enchants);

            if (!WriteFileAtomic(kSettingsPath, doc.dump(2))) {
                SKSE::log::error("settings: cannot write {}", kSettingsPath);
                return;
            }
            SKSE::log::info("settings: wrote {} box def(s) (enabled={})", g_boxes.size(), g_cefEnabled);
            WriteCarrierManifest();
        }

        // The single plugin that ships every CEF record (v1.2.1: the old
        // CostumeFW_Boxes.esp + CostumeFW_Boxes_FSMPCarrier_001.esp pair was
        // merged into ESL-flagged CostumeFW.esp by tools/espmerge). houseCARL
        // still writes new records to fresh patch plugins during development -
        // fold those in with tools/espmerge at release.
        constexpr std::string_view kTokenPlugin = "CostumeFW.esp";

        bool IsTokenPluginFile(const RE::TESFile* a_file)
        {
            if (!a_file) {
                return false;
            }
            // CEF's own records live in CostumeFW.esp and any CostumeFW_* dev patch
            // (e.g. CostumeFW_VanillaSlots_001.esp = F1 vanilla-slot tokens, folded
            // into CostumeFW.esp at release). Prefix match so a not-yet-folded patch's
            // tokens are still recognized as ours.
            const auto name = a_file->GetFilename();
            return name.size() >= 9 && ::_strnicmp(name.data(), "CostumeFW", 9) == 0;
        }

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

        // v1.2.1 plugin consolidation: CostumeFW_Boxes.esp and
        // CostumeFW_Boxes_FSMPCarrier_001.esp were merged into CostumeFW.esp
        // (espfe). Base-plugin local ids were kept verbatim; the patch plugin's
        // NEW records were renumbered +0x100 (0x8xx -> 0x9xx) - the rule lives
        // in tools/espmerge, keep the two in sync. Colon-ids persisted by
        // pre-merge builds (settings JSON) are healed here so existing box /
        // persist definitions survive the merge.
        bool MigrateLegacyColonId(std::string& a_id)
        {
            constexpr std::string_view kOldBase = ":CostumeFW_Boxes.esp";
            constexpr std::string_view kOldPatch = ":CostumeFW_Boxes_FSMPCarrier_001.esp";
            const auto endsWithCI = [](std::string_view s, std::string_view suffix) {
                return s.size() >= suffix.size() &&
                       ::_strnicmp(s.data() + (s.size() - suffix.size()),
                           suffix.data(), static_cast<size_t>(suffix.size())) == 0;
            };
            if (endsWithCI(a_id, kOldBase)) {
                a_id = a_id.substr(0, a_id.size() - kOldBase.size()) + ":" +
                       std::string(kTokenPlugin);
                return true;
            }
            if (endsWithCI(a_id, kOldPatch)) {
                const std::string prefix = a_id.substr(0, a_id.size() - kOldPatch.size());
                // ROOT B [285]: require a valid 1-6 digit hex prefix. A garbage prefix
                // used to be silently rewritten to the fabricated id "000100".
                const bool okHex = !prefix.empty() && prefix.size() <= 6 &&
                    prefix.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
                if (!okHex) {
                    SKSE::log::warn("settings: unparseable legacy patch id '{}' - left as-is", a_id);
                    return false;
                }
                const auto lid = static_cast<std::uint32_t>(std::strtoul(prefix.c_str(), nullptr, 16));
                // ROOT J: mirror espmerge's disposition (tools/espmerge Program.cs).
                // The PoC records 0x806-0x808 were DROPPED (kDropPatchIds), so they
                // have no +0x100 image - leave such an id unhealed so it fails to
                // resolve and is dropped, instead of fabricating a dangling 000906.
                // Every OTHER patch-new record was renumbered +0x100 (kRenumberOffset).
                // Keep this in lockstep with espmerge if those constants ever change.
                if (lid == 0x806u || lid == 0x807u || lid == 0x808u) {
                    SKSE::log::warn("settings: legacy PoC id '{}' was dropped by the merge - not healing", a_id);
                    return false;
                }
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "%06X", lid + 0x100u);
                a_id = std::string(buf) + ":" + std::string(kTokenPlugin);
                return true;
            }
            return false;
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
            // Resolve a colon-id content to its worn NIF path (female 3P first,
            // male fallback - mirrors ResolveArmaModels' sex fallback). Returns
            // a null json when the id/ARMA/model can't be resolved.
            const auto resolveContent = [&](const std::string& id) -> nlohmann::json {
                const auto colon = id.find(':');
                if (colon == std::string::npos) {
                    return nullptr;
                }
                const auto lid = static_cast<std::uint32_t>(
                    std::strtoul(id.substr(0, colon).c_str(), nullptr, 16));
                const std::string plugin = id.substr(colon + 1);
                auto* arma = dh->LookupForm<RE::TESObjectARMA>(lid, plugin);
                if (!arma) {
                    if (auto* armo = dh->LookupForm<RE::TESObjectARMO>(lid, plugin)) {
                        // Race-matched addon, same rule as the injection side -
                        // the carrier must be built from the mesh that shows.
                        arma = PickAddonForPlayer(armo);
                    }
                }
                if (!arma) {
                    return nullptr;
                }
                // Same sex the INJECTION resolves (player sex + per-content
                // forced-gender override), so the carrier is built from the
                // NIF that actually shows - female-first here desynced the
                // carrier from a forced-Male / male-PC mesh whose bone set
                // differs (review 2026-07-07 P2).
                const RE::SEX sex = EffectiveSexFor(id);
                const RE::SEX other =
                    (sex == RE::SEXES::kMale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
                const char* nif = arma->bipedModels[sex].model.c_str();
                if (!nif || !*nif) {
                    nif = arma->bipedModels[other].model.c_str();
                }
                if (!nif || !*nif) {
                    return nullptr;
                }
                return { { "id", id }, { "nif", nif } };
            };
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                nlohmann::json jb;
                jb["slot"] = SlotNumberOf(ResolveArmo(b.token));
                jb["token"] = b.token;
                auto contents = nlohmann::json::array();
                for (const auto& id : b.contents) {
                    if (auto c = resolveContent(id); !c.is_null()) {
                        contents.push_back(std::move(c));
                    }
                }
                jb["contents"] = std::move(contents);
                arr.push_back(std::move(jb));
            }
            doc["boxes"] = std::move(arr);
            // approach-C persist section: the token-less class rides the facegen
            // head path. sync builds Persist_carrier/_partNN (+ the per-*-shape
            // renamed physics XML) from these; CEF registers the head-part pool
            // and repoints its models (stage 3b).
            // M2: the fragment tracks THIS SAVE'S ACTIVE set, not the shared
            // catalog, so FSMP never builds physics for content this character
            // doesn't show (CEF_STATE_SCOPE.md §5).
            {
                auto pcontents = nlohmann::json::array();
                for (const auto& id : ActivePersistIds()) {
                    if (auto c = resolveContent(id); !c.is_null()) {
                        pcontents.push_back(std::move(c));
                    }
                }
                doc["persist"] = { { "contents", std::move(pcontents) } };
            }

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
            if (!WriteFileAtomic(kManifestPath, out)) {
                SKSE::log::error("carrier manifest: cannot write {}", kManifestPath);
                return;
            }
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

        // Repoint a token ARMO's carrier ARMA (both sexes) at a_file. Returns true if
        // the path actually changed (the ARMA was on a different revision before).
        bool RepointCarrier(RE::TESObjectARMO* a_armo, const std::string& a_file)
        {
            if (!a_armo || a_armo->armorAddons.empty()) {
                return false;
            }
            auto* arma = a_armo->armorAddons.front();
            const char* cur = arma->bipedModels[RE::SEXES::kFemale].model.c_str();
            if (cur && a_file == cur) {
                return false;  // already on this revision
            }
            arma->bipedModels[RE::SEXES::kMale].model = a_file.c_str();
            arma->bipedModels[RE::SEXES::kFemale].model = a_file.c_str();
            return true;
        }

        // --- approach-C persist head-carrier pool (stage 3b) --------------------
        // The engine materializes ONE geometry per head part, renamed to the
        // part's editorID, so every mesh the physics XML references needs its own
        // HDPT (C §9-18). The pool is static ESP records; nifcarrier assigns the
        // current build's meshes to pool slots (carriers.json "persist" entry),
        // CEF repoints the assigned models and registers exactly those parts on
        // the player. Registration itself is save-persisted (player appearance);
        // the model repoints are volatile form edits reapplied on every pass.
        constexpr const char* kCarrierPlugin = "CostumeFW.esp";
        constexpr std::uint32_t kPersistCarrierId = 0x000909;    // CFW_PersistCarrier
        constexpr std::uint32_t kPersistProxyFirstId = 0x00090A;  // CFW_PersistProxy01..08
        constexpr int kMaxPersistProxies = 8;

        RE::BGSHeadPart* LookupPoolPart(std::uint32_t a_localId)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<RE::BGSHeadPart>(a_localId, kCarrierPlugin) : nullptr;
        }

        // The full pool (carrier first), skipping unresolved records (plugin
        // absent / trimmed). Empty = the persist head path is unavailable.
        std::vector<RE::BGSHeadPart*> PersistPool()
        {
            std::vector<RE::BGSHeadPart*> pool;
            if (auto* c = LookupPoolPart(kPersistCarrierId)) {
                pool.push_back(c);
            }
            for (int i = 0; i < kMaxPersistProxies; ++i) {
                if (auto* p = LookupPoolPart(kPersistProxyFirstId + i)) {
                    pool.push_back(p);
                }
            }
            return pool;
        }

        bool CarrierFileOnDisk(const std::string& a_file)
        {
            // ROOT F (border audit 2026-07-09): carriers.json is disk-editable and
            // externally written, so validate BOTH the path and the file before a
            // live ARMA/HDPT model is repointed at it. Reject traversal / absolute /
            // UNC / non-.nif paths and truncated-or-non-NIF files - fall back to the
            // ESP-default carrier rather than feed the engine's skin loader a bad
            // model (a wrong/garbage NIF can render junk geometry or, for the
            // NiTriShape / zero-vertex class, divide-by-zero CTD).
            if (a_file.size() < 4 || a_file.find("..") != std::string::npos ||
                a_file.find(':') != std::string::npos) {
                return false;
            }
            if (a_file[0] == '\\' || a_file[0] == '/') {
                return false;  // absolute / UNC (a meshes-relative path never leads with a slash)
            }
            if (::_strnicmp(a_file.c_str() + (a_file.size() - 4), ".nif", 4) != 0) {
                return false;
            }
            std::string diskPath = "Data\\meshes\\" + a_file;
            std::replace(diskPath.begin(), diskPath.end(), '/', '\\');
            std::ifstream f(diskPath, std::ios::binary);
            if (!f) {
                return false;
            }
            // NIF header magic ("Gamebryo File Format" / "NetImmerse File Format")
            // catches a truncated slot file or a non-NIF path substituted by hand.
            char buf[24]{};
            f.read(buf, sizeof(buf));
            if (f.gcount() < static_cast<std::streamsize>(sizeof(buf))) {
                return false;
            }
            const std::string_view head(buf, sizeof(buf));
            return head.find("Gamebryo") != std::string_view::npos ||
                   head.find("NetImmerse") != std::string_view::npos;
        }

        // Repoint + registration reconcile for the persist head-carrier pool.
        // Called from ApplyCarrierOverridesImpl on every pass (load + runtime).
        void ApplyPersistCarrier(const nlohmann::json& a_doc, bool a_refreshChanged)
        {
            auto* carrier = LookupPoolPart(kPersistCarrierId);
            if (!carrier) {
                return;  // no pool (plugin absent) - persist head path unavailable
            }
            const auto pool = PersistPool();

            bool repointChanged = false;
            const auto repoint = [&](RE::BGSHeadPart* a_part, const std::string& a_file) {
                std::string path = a_file;  // meshes-relative; engine wants backslashes
                std::replace(path.begin(), path.end(), '/', '\\');
                const char* cur = a_part->model.c_str();
                if (cur && path == cur) {
                    return;
                }
                a_part->model = path.c_str();
                repointChanged = true;
                SKSE::log::info("persist carrier: '{}' model -> {}",
                    a_part->GetFormEditorID(), path);
            };

            // Desired registration: carrier + assigned proxies while the persist
            // SMP set is non-empty (fragment count; pre-count fragments assume
            // non-empty) AND CEF is enabled AND persist contents exist at all.
            std::vector<RE::BGSHeadPart*> desired;
            const auto pj = a_doc.find("persist");
            if (pj != a_doc.end() && pj->is_object()) {
                const int count = pj->value("count", -1);
                // M2: keyed off the per-save ACTIVE set (not the catalog) - a
                // character with nothing activated must not carry the head parts.
                const bool active = count != 0 && g_cefEnabled && AnyPersistActive();
                const std::string file = pj->value("file", "");
                if (!file.empty() && !CarrierFileOnDisk(file)) {
                    // Fail-safe (mirrors the box carriers): never point a HDPT at
                    // a missing file, and don't churn the registration on a disk
                    // hiccup - keep whatever is currently applied.
                    SKSE::log::warn(
                        "persist carrier: '{}' missing on disk - keeping current state", file);
                    return;
                }
                if (!file.empty()) {
                    repoint(carrier, file);
                    if (active) {
                        desired.push_back(carrier);
                    }
                    for (const auto& pe : pj->value("parts", nlohmann::json::array())) {
                        const std::string eid = pe.value("editorid", "");
                        const std::string pfile = pe.value("file", "");
                        RE::BGSHeadPart* proxy = nullptr;
                        for (auto* p : pool) {
                            const char* ped = p->GetFormEditorID();
                            if (p != carrier && ped && eid == ped) {
                                proxy = p;
                                break;
                            }
                        }
                        if (!proxy) {
                            SKSE::log::warn(
                                "persist carrier: unknown pool part '{}' - its collision is inert", eid);
                            continue;
                        }
                        if (pfile.empty() || !CarrierFileOnDisk(pfile)) {
                            SKSE::log::warn(
                                "persist carrier: part '{}' file '{}' missing - skipped", eid, pfile);
                            continue;
                        }
                        repoint(proxy, pfile);
                        if (active) {
                            desired.push_back(proxy);
                        }
                    }
                }
            }
            // No persist entry / inactive set -> desired stays empty = deregister.

            // One-shot migration sweep (v1.2.1 merge): old-plugin CFW_* parts
            // still registered on the save are removed here; the merged pool is
            // reconciled right after. Both feed the same rebuild request.
            const bool legacySwept = SweepLegacyCfwHeadParts(pool);
            const bool regChanged = ReconcilePersistHeadParts(desired, pool) || legacySwept;
            // Load-time race avoidance (Codex 1-2). The head-part registration is
            // SAVE-PERSISTED, so on load the ENGINE rebuilds the head (and FSMP the
            // wig physics) ONCE, naturally, like normal equipment. A CEF-forced
            // DoReset3D on top of that only adds redundant rebuilds that race the
            // engine's own multi-pass head build - the non-deterministic 10/27GB
            // lottery (each extra FSMP build retains a ~2.5GB Engine Fixes arena).
            // So on the LOAD pass (a_refreshChanged=false) do NOT force a rebuild
            // for a repoint-only change: the kDataLoaded repoint (which runs before
            // the engine's build) already points the head part at the current NIF.
            // Still rebuild when the REGISTRATION actually changed (parts added/
            // removed - a save predating the registration, a live toggle) and on any
            // RUNTIME content change; the debounce coalesces bursts either way.
            const bool needRebuild =
                regChanged || (a_refreshChanged && repointChanged && !desired.empty());
            if (needRebuild) {
                RequestPersistHeadRebuild(regChanged ? "registration" : "model repoint");
                if (a_refreshChanged) {
                    RE::DebugNotification("Costume persist physics updated");
                }
            }
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
            // ROOT B (border audit 2026-07-09): the parse is guarded above, but the
            // per-slot field reads below (doc[key].value(...) and the persist
            // fragment) are typed accesses that throw json::type_error on a
            // wrong-typed member of an otherwise-valid carriers.json. Catch it - a
            // malformed override just leaves the ESP-default carriers in place,
            // never an uncaught throw out of the load / sync path.
            try {
            bool anyChanged = false;
            for (const auto& b : g_boxes) {
                auto* armoA = ResolveArmo(b.token);
                if (!armoA || armoA->armorAddons.empty()) {
                    continue;
                }
                const int slot = SlotNumberOf(armoA);
                const auto key = std::to_string(slot);
                if (!doc.contains(key)) {
                    continue;
                }
                const std::string file = doc[key].value("file", "");
                if (file.empty()) {
                    continue;
                }
                // Fail-safe: never repoint a token ARMA at a carrier that isn't on
                // disk. If a bad build was quarantined/removed (see nifcarrier's
                // divide-by-zero gate), fall back to the ESP default carrier so the
                // worn token can't load a missing/stale path. The carriers.json path
                // is meshes-relative; it resolves through MO2's VFS like our other
                // relative reads.
                if (!CarrierFileOnDisk(file)) {
                    SKSE::log::warn(
                        "carrier override: slot {} carrier '{}' missing/invalid on disk - keeping ESP default",
                        key, file);
                    continue;
                }
                // Repoint the token's carrier ARMA at the newest revision. USER-DRIVEN
                // APPLY: the new carrier LOADS when the user re-equips the token - CEF
                // does NOT auto-swap (a programmatic re-equip coalesces / stalls in the
                // equip queue and can't be automated reliably). A manual re-equip reloads
                // cleanly once FSMP has settled.
                if (RepointCarrier(armoA, file)) {
                    SKSE::log::info("carrier override: slot {} token '{}' -> {} (re-equip to apply)",
                        key, b.token, file);
                    anyChanged = true;
                }
            }
            // Runtime content change (not the load-time pass): prompt the user to re-equip.
            // FSMP merges the new carrier's bones asynchronously, so it can take a couple
            // of re-equips to catch - tell the user to repeat until the SMP sways.
            if (a_refreshChanged && anyChanged) {
                RE::DebugNotification("Costume Box updated - re-equip the box token until the outfit sways");
            }
            // Persist class (approach C): the head-part pool needs no re-equip -
            // CEF can fire the facegen rebuild itself, so this is fully automatic.
            ApplyPersistCarrier(doc, a_refreshChanged);
            } catch (const std::exception& e) {
                SKSE::log::warn("carriers.json field type error ({}) - keeping ESP-default carriers", e.what());
            }
        }

        // --- auto-sync: rebuild carriers when the manifest changes ---------------
        // Full hands-off loop: manifest change (the exact "box content set changed"
        // signal) -> debounce -> rebuild -> ApplyCarrierOverridesImpl(true) on the
        // main thread (slot repoint + two-phase re-equip). Default is the IN-PROC
        // nifcarrier_core build (v1.2, NIFCARRIER_INPROC.md); a present
        // CEF_sync_command.txt (one line: the command to run) keeps the external
        // C# tool in charge instead - compat mode for one release.
        constexpr const char* kSyncCommandPath = "Data\\SKSE\\Plugins\\CEF_sync_command.txt";
        // nifcarrier's stdout/stderr is captured here (truncated each run) so the
        // [sync]/[merge] decisions - e.g. a content excluded, veil kept bones-only -
        // are visible instead of discarded. Lands in MO2 overwrite via the VFS.
        constexpr const char* kSyncLogPath = "Data\\SKSE\\Plugins\\CEF_sync.log";
        constexpr int kSyncDebounceMs = 2000;     // MCM edits come in bursts
        constexpr DWORD kSyncTimeoutMs = 120000;  // safety net for a wedged child

        std::atomic<bool> g_syncScheduled{ false };
        std::atomic<bool> g_syncRunning{ false };
        std::atomic<bool> g_syncRerun{ false };  // manifest changed while a sync ran
        // Last auto-sync outcome for the MCM Diagnostics page: -999 = none this
        // session, -2 = timed out (external: child terminated; in-proc: wedged,
        // auto-sync blocked until restart), -3 = failed to start, otherwise the
        // nifcarrier exit code (in-proc: 0 ok / 2 failed).
        std::atomic<int> g_lastSyncExit{ -999 };

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
            // Review C (sync hardening): an .exe target runs DIRECTLY - no shell
            // between the game and the child. .cmd/.bat wrappers still need
            // cmd.exe (CreateProcess cannot exec batch files); that shell layer
            // is a local-dev convenience - distribute exe-form commands.
            std::string full = cmd;
            {
                std::string first = cmd;
                if (!first.empty() && first.front() == '"') {
                    const auto q = first.find('"', 1);
                    first = (q == std::string::npos) ? first.substr(1) : first.substr(1, q - 1);
                } else {
                    const auto sp = first.find_first_of(" \t");
                    if (sp != std::string::npos) {
                        first = first.substr(0, sp);
                    }
                }
                const auto endsWithNoCase = [](std::string_view s, std::string_view suf) {
                    if (s.size() < suf.size()) {
                        return false;
                    }
                    for (std::size_t i = 0; i < suf.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])) !=
                            std::tolower(static_cast<unsigned char>(suf[i]))) {
                            return false;
                        }
                    }
                    return true;
                };
                if (!endsWithNoCase(first, ".exe")) {
                    full = "cmd /c \"" + cmd + "\"";
                }
            }
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::vector<char> buf(full.begin(), full.end());
            buf.push_back('\0');

            // Redirect the child's stdout+stderr into CEF_sync.log (truncate per run)
            // so nifcarrier's diagnostics survive. Inheritable handle; BOTH streams
            // point at it. On failure we just run without redirection (log lost, sync
            // still works). Console output is Shift-JIS - readable in a JP text editor.
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE hLog = CreateFileA(kSyncLogPath, GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            const BOOL inherit = (hLog != INVALID_HANDLE_VALUE) ? TRUE : FALSE;
            if (inherit) {
                si.dwFlags |= STARTF_USESTDHANDLES;
                si.hStdOutput = hLog;
                si.hStdError = hLog;
                si.hStdInput = INVALID_HANDLE_VALUE;
            }
            BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, inherit,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (hLog != INVALID_HANDLE_VALUE) {
                CloseHandle(hLog);  // child holds its own inherited copy
            }
            if (!ok) {
                SKSE::log::error("auto-sync: CreateProcess failed ({}) for: {}", GetLastError(), cmd);
                g_lastSyncExit = -3;
                g_syncRunning = false;
                return;
            }
            SKSE::log::info("auto-sync: rebuilding carriers ({})", cmd);
            CloseHandle(pi.hThread);
            std::thread([h = pi.hProcess]() {
                DWORD code = 1;
                if (WaitForSingleObject(h, kSyncTimeoutMs) == WAIT_OBJECT_0) {
                    GetExitCodeProcess(h, &code);
                    g_lastSyncExit = static_cast<int>(code);
                } else {
                    // A wedged child must not outlive the wait: once
                    // g_syncRunning drops, a rerun would race it on the same
                    // carrier slots + sync log (review A-3). nifcarrier
                    // publishes atomically, so a mid-build kill cannot leave a
                    // partial carrier behind.
                    SKSE::log::warn("auto-sync: nifcarrier timed out - terminating child");
                    TerminateProcess(h, 1);
                    WaitForSingleObject(h, 5000);
                    g_lastSyncExit = -2;
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

        // In-proc sync (NIFCARRIER_INPROC.md Phase 5): nifcarrier_core on a
        // detached background thread. The sync body is pure file I/O + nifly -
        // no engine API; the only engine call is the completion AddTask. Reuses
        // the external path's g_syncRunning/g_syncRerun coalescing. No hard
        // timeout: unlike a child process, a wedged thread cannot be killed -
        // containment is the validate gates + try/catch inside Sync(). A
        // watchdog only SURFACES the wedge (log + Diagnostics -2); it must not
        // clear g_syncRunning - a rerun would race the zombie thread on the
        // same slot files (the review A-3 hazard the child-kill used to avoid).
        std::atomic<std::uint64_t> g_syncGen{ 0 };

        void RunInProcSync()
        {
            if (g_syncRunning.exchange(true)) {
                g_syncRerun = true;  // coalesce: rerun once the current sync ends
                return;
            }
            SKSE::log::info("auto-sync: rebuilding carriers (in-proc nifcarrier)");
            const std::uint64_t gen = ++g_syncGen;
            std::thread([gen]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(kSyncTimeoutMs));
                if (g_syncRunning.load() && g_syncGen.load() == gen) {
                    SKSE::log::error(
                        "auto-sync: in-proc sync still running after {}s - likely wedged; auto-sync is blocked until restart",
                        kSyncTimeoutMs / 1000);
                    g_lastSyncExit = -2;
                }
            }).detach();
            std::thread([]() {
                nifcarrier::SyncResult sr;
                try {
                    nifcarrier::SyncOptions opts;
                    opts.manifestPath = "Data\\SKSE\\Plugins\\CEF_carrier_manifest.json";
                    opts.dataRoots = { "Data" };  // usvfs resolves like the engine
                    opts.outRoot = "Data";
                    opts.emptyNif = "Data\\meshes\\CostumeFW\\boxtoken.nif";
                    sr = nifcarrier::Sync(opts);
                } catch (...) {
                    sr.ok = false;
                    sr.log += "[sync] FAILED: unhandled exception\n";
                }
                {
                    // Diagnostics parity: same log file the external tool wrote.
                    std::ofstream f(kSyncLogPath, std::ios::binary | std::ios::trunc);
                    f << sr.log;
                }
                g_lastSyncExit = sr.ok ? 0 : 2;
                g_syncRunning = false;
                if (g_syncRerun.exchange(false)) {
                    RunInProcSync();
                    return;
                }
                if (sr.ok) {
                    SKSE::GetTaskInterface()->AddTask([]() {
                        SKSE::log::info("auto-sync: done - applying carrier revisions");
                        ApplyCarrierOverridesImpl(true);
                    });
                } else {
                    SKSE::log::error("auto-sync: in-proc sync failed - carriers unchanged (see CEF_sync.log)");
                }
            }).detach();
        }

        void ScheduleAutoSync()
        {
            if (g_syncScheduled.exchange(true)) {
                return;  // a debounced run is already pending
            }
            RunAfterDelayMs(kSyncDebounceMs, []() {
                g_syncScheduled = false;
                if (ReadSyncCommand().empty()) {
                    RunInProcSync();  // v1.2 default
                } else {
                    SpawnSyncProcess();  // external tool keeps priority (compat)
                }
            });
        }
    }

    void LoadBoxes()
    {
        g_boxes.clear();
        g_persist.clear();
        g_hideRules.clear();
        g_genderModes.clear();
        g_bodyMorphOn.clear();
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
        bool fromBackup = false;
        try {
            f >> doc;
        } catch (const std::exception& e) {
            SKSE::log::error("settings: JSON parse error: {}", e.what());
            if (migrated) {
                return;  // legacy file was corrupt; nothing else to try
            }
            // Corrupt main file: fall back to the last-known-good backup taken at
            // the previous successful load. Without this, the cleared state above
            // would look like "no settings" and the next WriteJson would make the
            // loss permanent (Codex review 2026-07-05 A-1).
            std::ifstream bak(kSettingsBakPath);
            if (!bak) {
                return;
            }
            try {
                bak >> doc;
            } catch (const std::exception& e2) {
                SKSE::log::error("settings: backup parse error: {}", e2.what());
                return;
            }
            fromBackup = true;
            SKSE::log::warn("settings: recovered from {} (main file corrupt; "
                            "it is rewritten on the next settings change)",
                kSettingsBakPath);
        }
        // Heal pre-merge colon-ids (v1.2.1 plugin consolidation) wherever the
        // settings persist them; a healed file is rewritten once below.
        bool healed = false;
        // ROOT B (border audit 2026-07-09): the field reads below are typed
        // value()/get<> accesses. A WRONG-TYPED field in a hand-edited settings file
        // (e.g. "boxes": 5) throws json::type_error - which the parse-only guard above
        // did NOT cover, so it crashed at EVERY launch. Wrap the extraction: a bad
        // field discards the partial state and leaves the on-disk file intact (no
        // data-destroying WriteJson) instead of crashing. Also enforce ingest
        // invariants here (one box per token, one holder per content id).
        std::vector<std::string> seenTokens;    // ROOT B: one box per token
        std::vector<std::string> seenContents;  // ROOT B: one holder per content id
        try {
        g_cefEnabled = doc.value("enabled", true);
        const auto boxes = doc.value("boxes", nlohmann::json::array());
        for (const auto& jb : boxes) {
            BoxDefInfo b;
            b.label = jb.value("label", std::string{});
            b.token = jb.value("token", std::string{});
            healed |= MigrateLegacyColonId(b.token);
            healed |= CanonicalizeColonId(b.token);  // ROOT D
            if (b.token.empty()) {
                continue;
            }
            if (!IsTokenColonId(b.token)) {
                // ROOT C: a box token must be a CEF plugin record; anything else means
                // SetTokenStats below would rewrite a foreign/vanilla ARMO's stats.
                SKSE::log::warn("boxes: LoadBoxes drops box with non-CEF token '{}'", b.token);
                healed = true;
                continue;
            }
            if (std::find(seenTokens.begin(), seenTokens.end(), b.token) != seenTokens.end()) {
                // ROOT B: token-keyed mutators only ever reach the first box, so a
                // second box on the same token is a dead "item printer" - drop it.
                SKSE::log::warn("boxes: LoadBoxes drops second box on token '{}'", b.token);
                healed = true;
                continue;
            }
            seenTokens.push_back(b.token);
            for (const auto& c : jb.value("contents", nlohmann::json::array())) {
                if (c.is_string()) {
                    auto id = c.get<std::string>();
                    healed |= MigrateLegacyColonId(id);
                    healed |= CanonicalizeColonId(id);  // ROOT D
                    if (IsTokenColonId(id)) {  // ROOT C: CEF-own id smuggled as content
                        SKSE::log::warn(
                            "boxes: LoadBoxes drops CEF-own content id '{}' from box '{}'", id, b.token);
                        healed = true;
                        continue;
                    }
                    if (std::find(seenContents.begin(), seenContents.end(), id) != seenContents.end()) {
                        // ROOT B: the injection registry is one-entry-per-id; a dup
                        // across boxes last-wins and the extra remove row fabricates.
                        SKSE::log::warn(
                            "boxes: LoadBoxes drops duplicate content id '{}' (one holder per id)", id);
                        healed = true;
                        continue;
                    }
                    seenContents.push_back(id);
                    b.contents.push_back(std::move(id));
                }
            }
            b.ability = jb.value("ability", std::string{});
            healed |= MigrateLegacyColonId(b.ability);
            healed |= CanonicalizeColonId(b.ability);  // ROOT D
            b.enabled = jb.value("enabled", true);
            b.armorType = std::clamp(jb.value("armorType", 0), 0, 2);  // ROOT B: valid class only
            b.preset = jb.value("preset", std::string{});
            b.uiVisible = jb.value("uiVisible", true);
            b.wear = jb.value("wear", false);
            g_boxes.push_back(std::move(b));
        }
        const auto persist = doc.value("persist", nlohmann::json::object());
        for (const auto& c : persist.value("contents", nlohmann::json::array())) {
            if (c.is_string()) {
                auto id = c.get<std::string>();
                healed |= MigrateLegacyColonId(id);
                healed |= CanonicalizeColonId(id);  // ROOT D
                if (IsTokenColonId(id)) {  // ROOT C
                    SKSE::log::warn("boxes: LoadBoxes drops CEF-own persist id '{}'", id);
                    healed = true;
                    continue;
                }
                if (std::find(seenContents.begin(), seenContents.end(), id) != seenContents.end()) {
                    SKSE::log::warn("boxes: LoadBoxes drops persist id '{}' (already a box content)", id);
                    healed = true;
                    continue;
                }
                seenContents.push_back(id);
                g_persist.push_back(std::move(id));
            }
        }
        g_persistPreset = persist.value("preset", std::string{});
        const auto rules = doc.value("hideRules", nlohmann::json::object());
        for (auto it = rules.begin(); it != rules.end(); ++it) {
            std::vector<int> slots;
            for (const auto& s : it.value()) {
                if (s.is_number_integer()) {
                    const int slot = s.get<int>();
                    if (slot >= 30 && slot <= 61) {  // ROOT B: valid biped slots only
                        slots.push_back(slot);
                    } else {
                        healed = true;
                    }
                }
            }
            if (!slots.empty()) {
                std::string key = it.key();
                healed |= MigrateLegacyColonId(key);
                healed |= CanonicalizeColonId(key);  // ROOT D
                g_hideRules[std::move(key)] = std::move(slots);
            }
        }
        const auto genders = doc.value("genderModes", nlohmann::json::object());
        for (auto it = genders.begin(); it != genders.end(); ++it) {
            if (it.value().is_number_integer()) {
                const int m = it.value().get<int>();
                if (m >= 1 && m <= 2) {
                    std::string key = it.key();
                    healed |= MigrateLegacyColonId(key);
                    healed |= CanonicalizeColonId(key);  // ROOT D
                    g_genderModes[std::move(key)] = m;
                }
            }
        }
        for (const auto& id : doc.value("bodyMorph", nlohmann::json::array())) {
            if (id.is_string()) {
                auto s = id.get<std::string>();
                healed |= MigrateLegacyColonId(s);
                healed |= CanonicalizeColonId(s);  // ROOT D
                g_bodyMorphOn.insert(std::move(s));
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
                std::string key = it.key();
                healed |= MigrateLegacyColonId(key);
                healed |= CanonicalizeColonId(key);  // ROOT D
                g_contentEnchants[std::move(key)] = std::move(effs);
            }
        }
        } catch (const std::exception& e) {
            // ROOT B: a wrong-typed field threw mid-extraction. Discard the partial
            // state and leave CEF_settings.json untouched so the user can fix it -
            // no crash, and no data-destroying WriteJson of an empty store.
            SKSE::log::error("settings: field type error ({}) - loaded empty; "
                             "CEF_settings.json left intact for repair", e.what());
            g_boxes.clear();
            g_persist.clear();
            g_hideRules.clear();
            g_genderModes.clear();
            g_bodyMorphOn.clear();
            g_contentEnchants.clear();
            g_persistPreset.clear();
            g_cefEnabled = true;
            return;
        }
        // ROOT B: snapshot the last-known-good backup only AFTER a clean, fully
        // validated load (previously taken before the field reads, so a file that
        // parsed but had a bad field could clobber the good .bak).
        if (!migrated && !fromBackup) {
            CopyFileA(kSettingsPath, kSettingsBakPath, FALSE);
        }
        if (healed) {
            SKSE::log::info(
                "settings: healed pre-merge plugin ids -> {} (v1.2.1 consolidation)",
                kTokenPlugin);
        }
        if (migrated || healed) {
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
        // Persist is NOT registered from the catalog: the ACTIVE set is per-save
        // and comes from the co-save restore (M2, CEF_STATE_SCOPE.md §3). A new
        // character starts with nothing shown.
        SKSE::log::info("settings: loaded {} box(es) ({} content), {} persist (catalog), enabled={}",
            g_boxes.size(), contentCount, g_persist.size(), g_cefEnabled);
        // Point each token ARMA at its current carrier revision (carriers.json).
        // No refresh here: tokens haven't equipped yet at load time.
        ApplyCarrierOverridesImpl(false);
    }

    void ApplyCarrierOverrides(bool a_refreshChanged)
    {
        ApplyCarrierOverridesImpl(a_refreshChanged);
    }

    void PersistCarrierStatus()
    {
        auto* console = RE::ConsoleLog::GetSingleton();
        const auto say = [&](const std::string& s) {
            SKSE::log::info("{}", s);
            if (console) {
                console->Print(s.c_str());
            }
        };
        const auto pool = PersistPool();
        if (pool.empty()) {
            say("[CEF] persist: head-part pool unavailable (carrier plugin absent)");
            return;
        }
        // carriers.json persist entry (if any)
        std::ifstream f(kCarriersJsonPath);
        nlohmann::json doc;
        if (f) {
            try {
                f >> doc;
            } catch (...) {
                doc = nlohmann::json::object();
            }
        }
        if (const auto pj = doc.find("persist"); pj != doc.end() && pj->is_object()) {
            say("[CEF] persist entry: rev=" + std::to_string(pj->value("rev", -1)) +
                " count=" + std::to_string(pj->value("count", -1)) +
                " file=" + pj->value("file", std::string{ "?" }) +
                " parts=" + std::to_string(pj->value("parts", nlohmann::json::array()).size()));
        } else {
            say("[CEF] persist entry: none (no persist build yet)");
        }
        say("[CEF] persist: catalog=" + std::to_string(g_persist.size()) +
            " active(this save)=" + std::to_string(ActivePersistIds().size()) +
            std::string(g_cefEnabled ? "" : " (CEF disabled)"));
        for (auto* part : pool) {
            const char* ed = part->GetFormEditorID();
            const bool reg = PlayerHasHeadPart(part);
            say(std::string("  ") + (ed ? ed : "?") + (reg ? " REGISTERED" : " -") +
                "  model=" + part->model.c_str());
        }
        // Churn diagnostics (Codex Phase 2): after a fresh load the target is
        // headRebuild exec ~1 and low reconcile/watchdog counts.
        say("[CEF] churn: " + PersistDiagString());
    }

    void PersistCarrierRemove()
    {
        // Deregister the whole production pool - the rescue lever for a
        // contaminated save; the pool re-registers on the next content change /
        // `cef persist regen`. (The PoC-leftover purge is gone with the v1.2.1
        // plugin merge: the PoC records were not carried into CostumeFW.esp, so
        // purge PoC-era saves on a pre-merge build BEFORE switching plugins.)
        auto parts = PersistPool();
        if (parts.empty()) {
            return;
        }
        if (ReconcilePersistHeadParts({}, parts)) {
            SKSE::log::info("persist carrier: production pool + PoC leftovers deregistered (manual remove)");
            RebuildPlayerHead();
        } else {
            SKSE::log::info("persist carrier: nothing registered to remove");
        }
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
        if (!CefEnabled()) {
            // ROOT G [1166]: master switch off (incl. right after uninstall cleanup,
            // which leaves box.enabled=true) - don't force tokens back onto the player.
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
        // Persist actives are per-save: the co-save restore (which precedes this
        // kPostLoadGame pass) already re-registered them (M2, CEF_STATE_SCOPE.md §3).
        SKSE::log::info("settings: reapplied {} box(es)", g_boxes.size());
    }

    void ReloadSettingsFromDisk()
    {
        // Snapshot this save's persist actives BEFORE the registry is wiped -
        // they are co-save state, invisible to the settings JSON.
        const auto actives = ActivePersistIds();
        for (const auto& it : ActiveSnapshot()) {
            DetachSkinned(it.id);
        }
        ClearRegistry();
        // Full clear + JSON re-read + box content re-register + token stats.
        // Its trailing carrier pass sees an empty active set (deregisters the
        // persist pool); the re-reconcile below re-registers - both requests
        // coalesce into ONE debounced head rebuild.
        LoadBoxes();
        // Re-register EVERY snapshot active - including entries no longer in
        // the reloaded catalog. Uncataloged actives are a supported M2 state
        // ("another character removed the entry; this save keeps showing it
        // until deactivated"), and the co-save restore path never filters by
        // catalog either. Filtering here silently vanished them with no item
        // return (review 2026-07-07 P1-a).
        int restored = 0;
        for (const auto& id : actives) {
            if (RegisterBoxById(id, {})) {
                ++restored;
            }
        }
        Reconcile();
        RebuildPersistAbility();
        ApplyBoxAbilities();
        ApplyCarrierOverrides(false);  // persist pool reconcile with actives back
        SKSE::log::info("settings: reloaded from disk (MCM) - {} box(es), {}/{} persist active restored",
            g_boxes.size(), restored, actives.size());
    }

    std::vector<std::string> PersistContents()
    {
        return g_persist;
    }

    bool AddPersistContent(const std::string& a_content)
    {
        if (a_content.empty()) {
            return false;
        }
        // ROOT C/D border quarantine (parity with AddBox): canonicalize, refuse a
        // CEF-own id as content, and refuse an id a BOX already holds (the injection
        // registry is one-entry-per-id).
        std::string content = a_content;
        CanonicalizeColonId(content);
        if (IsTokenColonId(content)) {
            SKSE::log::warn("persist: rejects CEF-own id '{}' as content", content);
            return false;
        }
        const std::string holder = ContentHolder(content);
        if (!holder.empty() && holder != "persist") {
            SKSE::log::warn("persist: '{}' rejected - already captured in box '{}'", content, holder);
            return false;
        }
        // "Already added" means ACTIVE ON THIS SAVE (M2, CEF_STATE_SCOPE.md §3):
        // capturing a catalog entry on a second character must succeed - only a
        // same-save duplicate fails (so the MCM never swallows the item).
        for (const auto& it : ActiveSnapshot()) {
            if (it.tokenId.empty() && it.id == content) {
                return false;
            }
        }
        if (std::find(g_persist.begin(), g_persist.end(), content) == g_persist.end()) {
            g_persist.push_back(content);  // catalog add (shared across saves)
            WriteJson();
        }
        return true;
    }

    bool RemovePersistContent(const std::string& a_content)
    {
        const auto it = std::find(g_persist.begin(), g_persist.end(), a_content);
        if (it == g_persist.end()) {
            return false;
        }
        g_persist.erase(it);
        // Per-content maps (hide/gender/morph/enchant snapshot) survive a
        // catalog remove ON PURPOSE (review round 4): another save may keep
        // this entry uncataloged-ACTIVE and still display it - erasing here
        // degraded its look/enchant/gender after that save's next load. A
        // re-capture re-snapshots the enchant anyway; the orphaned entries
        // are a few bytes in the shared json. (Box content removal still
        // erases - box contents have no per-save active state.)
        WriteJson();
        return true;
    }

    bool PersistSetActive(const std::string& a_id, bool a_on)
    {
        std::string id = a_id;  // ROOT D: canonical so catalog / active compares match
        CanonicalizeColonId(id);
        bool active = false;
        for (const auto& it : ActiveSnapshot()) {
            if (it.tokenId.empty() && it.id == id) {
                active = true;
                break;
            }
        }
        if (a_on) {
            if (std::find(g_persist.begin(), g_persist.end(), id) == g_persist.end()) {
                SKSE::log::warn(
                    "persist on: '{}' is not in the catalog (capture it via the MCM first)", id);
                return false;
            }
            // ROOT C: reject an id that a BOX also holds (only reachable via an
            // unvalidated JSON that put the same id in a box AND the catalog).
            // ContentHolder checks boxes first, so a box holder != "persist".
            const std::string holder = ContentHolder(id);
            if (!holder.empty() && holder != "persist") {
                SKSE::log::warn("persist on: '{}' is also box content ({}) - refusing", id, holder);
                return false;
            }
            if (active) {
                return true;  // idempotent
            }
            if (!RegisterBoxById(id, {})) {
                return false;
            }
        } else {
            if (!active) {
                return false;
            }
            DetachSkinned(id);  // detach + unregister; the catalog is untouched
        }
        Reconcile();
        RebuildPersistAbility();
        ApplyBoxAbilities();
        SyncPersistManifest();  // the persist fragment tracks the active set
        SKSE::log::info("persist {}: '{}' on this save", a_on ? "on" : "off", a_id);
        return true;
    }

    void SyncPersistManifest()
    {
        WriteCarrierManifest();
    }

    std::vector<std::string> PersistActiveIds()
    {
        return ActivePersistIds();
    }

    std::vector<std::string> DiagLines()
    {
        std::vector<std::string> out;
        out.push_back("# Status");
        out.push_back(std::string("CEF master: ") + (g_cefEnabled ? "enabled" : "DISABLED"));
        out.push_back(std::string("RaceMenu/skee body morph: ") +
                      (BodyMorph::Available() ? "acquired" : "NOT AVAILABLE"));
        out.push_back(std::string("FSMP (hdtSMP64.dll): ") +
                      (GetModuleHandleA("hdtSMP64.dll") ? "loaded" : "NOT LOADED"));
        {
            const int sync = g_lastSyncExit.load();
            std::string s = "carrier auto-sync: ";
            if (sync == -999) {
                s += "(none this session)";
            } else if (sync == -2) {
                s += "TIMED OUT - see CEF_sync.log";
            } else if (sync == -3) {
                s += "FAILED TO START - check CEF_sync_command.txt";
            } else if (sync == 0) {
                s += "ok (exit 0)";
            } else {
                s += "FAILED (exit " + std::to_string(sync) + ") - see CEF_sync.log";
            }
            out.push_back(s);
        }

        nlohmann::json cj;
        {
            std::ifstream f(kCarriersJsonPath);
            if (f) {
                try {
                    f >> cj;
                } catch (...) {
                    cj = nlohmann::json::object();
                }
            }
        }
        auto* player = RE::PlayerCharacter::GetSingleton();

        out.push_back("# Boxes");
        if (g_boxes.empty()) {
            out.push_back("(no boxes)");
        }
        for (const auto& b : g_boxes) {
            const int slot = SlotNumberOf(ResolveArmo(b.token));
            std::string line = "box " + std::to_string(slot) + ": " +
                               std::to_string(b.contents.size()) + " item(s)";
            if (!b.enabled) {
                line += ", disabled";
            }
            const std::uint32_t tf = ResolveFormId(b.token);
            if (player && tf && player->GetWornArmor(tf)) {
                line += ", WORN";
            }
            const std::string key = std::to_string(slot);
            if (cj.contains(key) && cj[key].is_object()) {
                line += ", carrier r" + std::to_string(cj[key].value("rev", 0));
                const std::string file = cj[key].value("file", std::string{});
                if (!file.empty() && !CarrierFileOnDisk(file)) {
                    line += " (FILE MISSING)";
                }
            } else {
                line += ", carrier: none built";
            }
            out.push_back(line);
        }

        out.push_back("# Persist");
        out.push_back("catalog=" + std::to_string(g_persist.size()) +
                      "  active(this save)=" + std::to_string(ActivePersistIds().size()));
        if (const auto pj = cj.find("persist"); pj != cj.end() && pj->is_object()) {
            out.push_back("carrier r" + std::to_string(pj->value("rev", -1)) +
                          ", smp content=" + std::to_string(pj->value("count", -1)) +
                          ", proxy parts=" +
                          std::to_string(pj->value("parts", nlohmann::json::array()).size()));
        } else {
            out.push_back("carrier: none built");
        }
        {
            const auto pool = PersistPool();
            int reg = 0;
            for (auto* p : pool) {
                if (PlayerHasHeadPart(p)) {
                    ++reg;
                }
            }
            out.push_back("head parts registered: " + std::to_string(reg) + "/" +
                          std::to_string(pool.size()));
        }
        out.push_back("# Churn (this session)");
        out.push_back(PersistDiagString());
        return out;
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
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        // Keep only valid vanilla biped slots (30-61), de-duplicated.
        std::vector<int> clean;
        for (const int s : a_slots) {
            if (s >= 30 && s <= 61 &&
                std::find(clean.begin(), clean.end(), s) == clean.end()) {
                clean.push_back(s);
            }
        }
        if (clean.empty()) {
            g_hideRules.erase(id);  // empty list = clear the rule
        } else {
            if (ContentHolder(id).empty()) {  // ROOT E [1476]: no orphan side-map entries
                SKSE::log::warn("hide: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            g_hideRules[id] = std::move(clean);
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
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_mode == 1 || a_mode == 2) {
            if (ContentHolder(id).empty()) {  // ROOT E [1476]: no orphan side-map entries
                SKSE::log::warn("gender: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            g_genderModes[id] = a_mode;
        } else {
            g_genderModes.erase(id);  // 0 (or invalid) = follow player
        }
        // WriteJson's trailing WriteCarrierManifest picks up the flip: the
        // manifest resolves content NIFs by effective sex (P2 fix), so a
        // gender change that switches the shown NIF rebuilds the carrier.
        WriteJson();
        return true;
    }

    bool BodyMorphOn(const std::string& a_id)
    {
        return g_bodyMorphOn.contains(a_id);
    }

    bool SetBodyMorphOn(const std::string& a_id, bool a_on)
    {
        if (a_id.empty()) {
            return false;
        }
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_on) {
            if (ContentHolder(id).empty()) {  // ROOT E [1498]: no orphan bodyMorph entries
                SKSE::log::warn("morph: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            g_bodyMorphOn.insert(id);
        } else {
            g_bodyMorphOn.erase(id);
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
            // Exclude our own box tokens (base pool esp + the carrier patch).
            auto* file = armo->GetFile(0);
            if (IsTokenPluginFile(file)) {
                continue;
            }
            const char* nm = armo->GetFullName();
            out.push_back({ (nm && *nm) ? std::string(nm) : MakeColonId(armo), MakeColonId(armo) });
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::vector<WornItem> InventoryArmors(const std::string& a_filter)
    {
        std::vector<WornItem> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }
        // Case-insensitive substring match on the display name (byte-wise
        // tolower: ASCII folds, multi-byte names still match byte-exact).
        const auto matchesFilter = [&](const std::string& a_name) {
            if (a_filter.empty()) {
                return true;
            }
            const auto it = std::search(a_name.begin(), a_name.end(),
                a_filter.begin(), a_filter.end(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
            return it != a_name.end();
        };
        auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
            return a_obj.Is(RE::FormType::Armor);
        });
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (count <= 0) {
                continue;
            }
            auto* armo = obj->As<RE::TESObjectARMO>();
            if (!armo) {
                continue;
            }
            // Exclude our own box tokens.
            auto* file = armo->GetFile(0);
            if (IsTokenPluginFile(file)) {
                continue;
            }
            const char* nm = armo->GetFullName();
            std::string name = (nm && *nm) ? std::string(nm) : MakeColonId(armo);
            if (!matchesFilter(name)) {
                continue;
            }
            out.push_back({ std::move(name), MakeColonId(armo) });
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        // The MCM lists each entry ONCE (v1.2.1: the forced-gender pick moved to
        // the per-content "Body" menu); SkyUI's menu dialog degrades past ~128
        // rows, so cap just under that. The "Inventory filter" input (and the
        // worn-capture menu) reaches anything past the cap.
        constexpr std::size_t kMaxItems = 120;
        if (out.size() > kMaxItems) {
            SKSE::log::info(
                "boxes: inventory capture list truncated {} -> {} (alphabetical; filter='{}')",
                out.size(), kMaxItems, a_filter);
            out.resize(kMaxItems);
        }
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
            if (!IsTokenPluginFile(file)) {
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
        // against what we added last time so KID/other keywords survive. Applied to
        // ONE named twin token (plan Y applies it to both so a consumer mod sees the
        // box's keywords whichever twin is currently worn).
        void ApplyKeywordsToToken(const std::string& a_tokenId, const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_tokenId);
            if (!token) {
                return;
            }
            ClearTokenKeywords(a_tokenId, token);
            auto& mine = g_boxKeywords[a_tokenId];
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
            SKSE::log::debug("boxes: token '{}' passthrough keywords +{}", a_tokenId, mine.size());
        }

        // Write the box's armor/weight/class + keywords onto ONE named twin token.
        void ApplyStatsToToken(const std::string& a_tokenId, const BoxDefInfo& a_box,
            float a_armorSum, float a_weightSum, RE::BIPED_MODEL::ArmorType a_type)
        {
            auto* token = ResolveArmo(a_tokenId);
            if (!token) {
                return;
            }
            token->armorRating = static_cast<std::uint32_t>(a_armorSum * 100.0f);
            token->weight = a_weightSum;
            token->bipedModelData.armorType = a_type;
            SKSE::log::debug("boxes: token '{}' stats armor={} weight={} type={}",
                a_tokenId, a_armorSum, a_weightSum, a_box.armorType);
            ApplyKeywordsToToken(a_tokenId, a_box);
        }

        // Write a box's contents' armor + weight DIRECTLY onto its token ARMO's
        // fields, so the worn token provides them through the normal equip system.
        // armorRating is stored as CK-value * 100. Sums 0 -> token fields cleared.
        // Plan Y: applied to BOTH twins so a flip-flop swap (worn twin changes) never
        // drops the passthrough keywords / stats a consumer mod reads on the token.
        void SetTokenStats(const BoxDefInfo& a_box)
        {
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
            ApplyStatsToToken(a_box.token, a_box, armorSum, weightSum, type);
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
        // Find the player item matching this content's base form and read its
        // EFFECTIVE enchantment (instance/player enchantment if present, else the
        // base enchantment). Prefer the WORN entry; fall back to any carried
        // entry so the inventory-capture flow (item never equipped) still
        // snapshots player enchantments.
        RE::EnchantmentItem* ench = nullptr;
        RE::EnchantmentItem* carried = nullptr;
        auto inv = player->GetInventory([](RE::TESBoundObject& a_obj) {
            return a_obj.Is(RE::FormType::Armor);
        });
        for (auto& [obj, data] : inv) {
            if (!obj || obj->GetFormID() != baseId) {
                continue;
            }
            const auto& [count, entry] = data;
            if (count <= 0 || !entry) {
                continue;
            }
            if (entry->IsWorn()) {
                ench = entry->GetEnchantment();
                break;
            }
            if (!carried) {
                carried = entry->GetEnchantment();
            }
        }
        if (!ench) {
            ench = carried;
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
        // Built from THIS SAVE'S ACTIVE set, not the shared catalog (M2) - a
        // non-active entry another character cataloged must not grant effects
        // here. Activation changes invalidate via RebuildPersistAbility().
        RE::SpellItem* PersistAbilityFor()
        {
            if (!g_persistSpellBuilt) {
                g_persistSpell = BuildEnchantSpell(ActivePersistIds(), "Costume Stats (Persist)");
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
        const bool cefOn = CefEnabled();  // ROOT G: master switch gates box abilities too
        for (const auto& b : g_boxes) {
            // With CEF disabled nothing is injected, so a worn token must not still
            // grant its contents' enchant/armor effects (only the persist spell was
            // gated before - border audit [2161]).
            const bool worn = cefOn && TokenWorn(b.token);
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
            // Same priority as the synthesized ability (BuildEnchantSpell):
            // the captured player-enchant snapshot beats the base enchantment.
            // Showing only the base made a captured enchant look unapplied
            // (review 2026-07-07 P3).
            if (const auto snap = g_contentEnchants.find(c);
                snap != g_contentEnchants.end() && !snap->second.empty()) {
                for (const auto& e : snap->second) {
                    auto* mgef = ResolveMgef(e.mgef);
                    auto* full = mgef ? mgef->As<RE::TESFullName>() : nullptr;
                    const char* nm = full ? full->GetFullName() : nullptr;
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf), "%s %.0f",
                        (nm && *nm) ? nm : "effect", e.magnitude);
                    effs.push_back(buf);
                }
            } else if (auto* ench = armo->formEnchanting) {
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

    // --- Hidden-store custody (border audit 2026-07-09, ROOT A) ---------------
    // FormID of the MCM's disabled holding container (CFW_Storage 0x80D). Set by
    // SetStoreRef (MCM OnConfigOpen); cleared on game load (per-save ref).
    static std::uint32_t g_storeFormId = 0;

    void SetStoreRef(RE::TESObjectREFR* a_store)
    {
        g_storeFormId = a_store ? a_store->GetFormID() : 0;
    }

    bool ReturnStoredItem(const std::string& a_id, bool a_fabricate)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t formId = ResolveFormId(a_id);
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!player || !obj) {
            return false;
        }
        // Prefer the captured original in the hidden store (keeps tempering /
        // player-enchant instance data), exactly like the MCM's ReturnItem.
        if (g_storeFormId) {
            auto* sform = RE::TESForm::LookupByID(g_storeFormId);
            auto* store = sform ? sform->As<RE::TESObjectREFR>() : nullptr;
            if (store) {
                const auto counts = store->GetInventoryCounts();
                const auto it = counts.find(obj);
                if (it != counts.end() && it->second > 0) {
                    store->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
                    SKSE::log::info("custody: returned stored '{}' to player", a_id);
                    return true;
                }
            }
        }
        if (a_fabricate) {
            player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            SKSE::log::info("custody: fabricated 1x '{}' (none stored on this save)", a_id);
            return true;
        }
        return false;
    }

    bool RecoverContentItem(const std::string& a_id)
    {
        // ROOT A ([2279]): drain the store first, fabricate only on a store miss -
        // so recovering a still-stored item no longer mints a second copy.
        if (ReturnStoredItem(a_id, true)) {
            SKSE::log::info("recover: granted 1x '{}' ({})", ItemDisplayName(a_id), a_id);
            return true;
        }
        SKSE::log::warn("recover: '{}' does not resolve to an inventory item", a_id);
        return false;
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

    bool IsTokenColonId(const std::string& a_id)
    {
        const auto colon = a_id.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const std::string_view plugin(a_id.data() + colon + 1, a_id.size() - colon - 1);
        // Any CEF plugin: CostumeFW.esp (post-merge), the two pre-merge plugins, or a
        // CostumeFW_* dev patch (F1 vanilla-slot tokens). All start "CostumeFW";
        // costume CONTENT mods never do. Prefix match, case-insensitive.
        return plugin.size() >= 9 && ::_strnicmp(plugin.data(), "CostumeFW", 9) == 0;
    }

    std::string ContentHolder(const std::string& a_content)
    {
        if (a_content.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            if (std::find(b.contents.begin(), b.contents.end(), a_content) !=
                b.contents.end()) {
                return b.token;
            }
        }
        if (std::find(g_persist.begin(), g_persist.end(), a_content) != g_persist.end()) {
            return "persist";
        }
        return {};
    }

    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content)
    {
        if (a_token.empty()) {
            return false;
        }
        // ROOT C/D border quarantine: canonicalize the ids, and refuse a CEF-own id
        // (a box token / pool part) or an id already held elsewhere as content. The
        // MCM pre-checks both (IsTokenPluginFile list filter + FindContentHolder),
        // but the native / preset / hand-edited-JSON routes reach here unchecked.
        std::string token = a_token;
        CanonicalizeColonId(token);
        if (!IsTokenColonId(token)) {  // ROOT C: token must be a CEF plugin record
            SKSE::log::warn("boxes: AddBox rejects non-CEF token '{}'", token);
            return false;
        }
        std::string content = a_content;
        if (!content.empty()) {
            CanonicalizeColonId(content);
            if (IsTokenColonId(content)) {
                SKSE::log::warn("boxes: AddBox rejects CEF-own id '{}' as content", content);
                return false;
            }
            const std::string holder = ContentHolder(content);
            if (!holder.empty() && holder != token) {
                SKSE::log::warn("boxes: AddBox rejects '{}' - already captured in '{}'", content, holder);
                return false;
            }
        }
        int idx = FindBox(token);
        if (idx < 0) {
            g_boxes.push_back({ a_label, token, {} });
            idx = static_cast<int>(g_boxes.size()) - 1;
        } else if (!a_label.empty()) {
            g_boxes[idx].label = a_label;
        }

        auto& box = g_boxes[idx];
        if (!content.empty()) {
            if (std::find(box.contents.begin(), box.contents.end(), content) !=
                box.contents.end()) {
                // Duplicate: report failure so the MCM capture flow does NOT move
                // the physical item into the store (a swallowed extra copy could
                // never be returned - review A-2 / CEF_STATE_SCOPE.md §4).
                SKSE::log::info("boxes: AddBox duplicate content '{}' in '{}'", content, token);
                return false;
            }
            box.contents.push_back(content);  // caller registers + reconciles
        }

        WriteJson();
        SetTokenStats(g_boxes[idx]);  // write armor/weight onto the token now
        SKSE::log::info("boxes: AddBox label='{}' token='{}' content='{}'", a_label, token, content);
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
        g_bodyMorphOn.erase(a_content);     // and its body-morph opt-in
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
        // Cross-holder guard, preset edition (P1-1 parity, review 2026-07-07):
        // reject when any incoming id is held by a BOX.
        for (const auto& c : a_contents) {
            const std::string ch = ContentHolder(c);
            if (!ch.empty() && ch != "persist") {
                SKSE::log::warn(
                    "preset: '{}' rejected - content '{}' is already captured in box '{}'",
                    a_presetName, c, ch);
                return false;
            }
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
        // Cross-holder guard, preset edition (P1-1 parity, review 2026-07-07):
        // the injection registry is one-entry-per-id, so a preset shipping an
        // id that ANOTHER box or persist already holds must not apply - it
        // would steal the display and share per-content settings.
        for (const auto& c : a_contents) {
            const std::string ch = ContentHolder(c);
            if (!ch.empty() && ch != a_token) {
                SKSE::log::warn(
                    "preset: '{}' rejected - content '{}' is already captured in '{}'",
                    a_presetName, c, ch);
                return false;
            }
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
