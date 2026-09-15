#include "BoxStore.h"
#include "BodyMorph.h"
#include "SkinRebind.h"
#include "PublishStore.h"
#include "Preset.h"  // MigrateAssignments (settings reload re-reads preset assignments)
#include "StoreLock.h"
#include "AtomicWrite.h"  // WriteFileAtomic (shared with the ability registry)
#include "FormId.h"       // MakeColonId (shared with the ability pool)
#include "TokenIdentity.h"  // who owns a form, decided from its plugin (v1.6.4)
#include "AbilityPool.h"  // the audit reports the passthrough state alongside the pools
#include "ConsoleOut.h"  // ConsolePrint - the one console chokepoint (F01)
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
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <tuple>
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
        // The 1.6.3-shaped settings, kept once at the moment 1.6.4 first rewrites
        // them. The .bak above is "last known good", overwritten on every clean
        // load - so one save under 1.6.4 and the old shape is nowhere. CEF never
        // reads this file; it exists so going back to 1.6.3 is a copy, not a
        // reconstruction. Written once and never again (MaybeWritePre164Backup).
        constexpr const char* kSettingsPre164Path =
            "Data\\SKSE\\Plugins\\CEF_settings.pre164.json";
        constexpr const char* kSchema = "cef.settings/1";

        // GLOBAL box definitions (config; all saves). One box per token. Mutated
        // only on the main thread (loads + queued native tasks); MCM queries read
        // it from the VM thread, tolerated like the rest of the registry.
        std::vector<BoxDefInfo> g_boxes;

        // Master CEF on/off (Main page). Persisted in CEF_settings.json.
        bool g_cefEnabled = true;
        // Whether the last settings load finished cleanly. The orphan sweep
        // refuses to run without it: a failed or partial load leaves every held
        // set empty, and an empty held set makes the WHOLE hidden store look
        // orphaned. False on a parse/field error and on "no settings file yet".
        bool g_settingsLoadOk = false;
        // Stronger than !g_settingsLoadOk: the settings file EXISTS but could not
        // be loaded. Then CEF is holding empty state that is NOT the truth, and
        // writing it back destroys the user's real one. Every WriteJson refuses
        // while this is set, so a bad file (hand-edited, or a schema mistake of
        // ours - 2026-09-08) costs a session, not the data. Cleared by the next
        // clean load; a fresh install with no file at all is NOT this case and
        // writes normally.
        bool g_settingsUnreadable = false;

        // The settings file just read was still in the 1.6.3 shape (no box
        // carried a boxId). Set at load, consumed by the first write after it:
        // that write is the moment the old shape stops existing anywhere, so a
        // copy of it is kept first. See MaybeWritePre164Backup.
        bool g_settingsWasPre164 = false;

        // Custody history (v1.6.2): ONE row per content id CEF has ever taken
        // custody of, holding what last happened to it. Keyed by id on purpose,
        // not an event stream: the case it exists for is "a piece vanished and I
        // have no idea what it was", and there the id is the only thing that can
        // bring it back - it is gone from every box, so nothing else records it.
        // One row per id also means a bulk return can't flood out the history.
        std::unordered_map<std::string, CustodyLogEntry> g_custody;
        constexpr std::size_t kCustodyMax = 256;

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
        // Item-data passthrough opt-OUTs (PLAN_2026-08-04): default is ON
        // (= current behavior), so the sets hold the ids a user switched OFF.
        // statEnchant gates FillEnchantSpell (box AND persist single choke);
        // statWeight/statArmor gate SetTokenStats' per-content sums.
        std::unordered_set<std::string> g_statEnchantOff;
        std::unordered_set<std::string> g_statWeightOff;
        std::unordered_set<std::string> g_statArmorOff;

        // Per-content set of SHAPE NAMES to drop at injection (default none). Lets
        // the user hide a costume's bundled body shape by name so it doesn't double
        // the player's real body. GLOBAL config, content-keyed. PERSISTED.
        std::unordered_map<std::string, std::vector<std::string>> g_hideShapes;

        // Runtime cache: content id -> its injected shapes (NIF name, biped slot;
        // slot -1 if not dismembered). Populated on the main thread by the injection
        // / `cef shapes`. NOT persisted (re-derived from the NIF each session).
        std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> g_contentShapes;

        // Per-content "inject the player's real (naked) body under this content"
        // opt-in (default off). Pairs with hideShapes: drop the costume's own body
        // shape, then substitute the player's morphed skin body so garments sit on
        // the real body. For body-slot tokens that mask the real body; on custom
        // slots (real body already shows) it would double. GLOBAL config, content-keyed.
        std::unordered_set<std::string> g_showRealBody;

        // --- Capture blacklist policy (v1.3.2, MARA_COMPAT_PLAN.md §3; r2) ----
        // Published as an IMMUTABLE snapshot (review P1-5): readers (render/VM
        // thread pickers, gates) load one shared_ptr per operation; mutators
        // (main thread via AddTask) copy the current policy, modify the copy,
        // and publish it atomically. No vector is mutated in place while
        // another thread may be reading it. Function-local static dodges SIOF.
        std::atomic<std::shared_ptr<const policy::CapturePolicy>>& PolicySlot()
        {
            static std::atomic<std::shared_ptr<const policy::CapturePolicy>> slot{
                std::make_shared<const policy::CapturePolicy>()
            };
            return slot;
        }

        void PublishPolicy(policy::CapturePolicy a_next)
        {
            PolicySlot().store(
                std::make_shared<const policy::CapturePolicy>(std::move(a_next)));
        }

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

        // What THIS save's hidden store held when the save was written, from the
        // co-save. Per-save, exactly like g_storeFormId: box definitions are
        // global, so only this list can tell a real loss from the normal
        // "another character never captured it here".
        std::vector<std::string> g_storeManifest;

        // How many the last comparison found missing. The on-screen notice at
        // load scrolls away in seconds, and this one reports something that
        // cannot be undone - so the Recovery page carries it for the session too.
        // Atomic: the render thread reads it every frame it draws that page.
        std::atomic<int> g_lastStoreLoss{ 0 };

        // Captured tempering multiplier per content (absent = untempered). The
        // base ARMO's armorRating misses the smithing improvement exactly like it
        // misses the player enchantment (2game.info 2026-08-18), so it snapshots
        // at the same capture spot and scales every armor passthrough sum. The
        // value is the ENGINE-measured item-card ratio (GetArmorValue with/without
        // the instance list - see MeasureTemperMult), not the raw ExtraHealth:
        // the card bonus is a flat curve, not armorRating*health. GLOBAL config,
        // content-keyed.
        std::unordered_map<std::string, float> g_contentTemper;

        float TemperMultOf(const std::string& a_id)
        {
            const auto it = g_contentTemper.find(a_id);
            return it == g_contentTemper.end() ? 1.0f : it->second;
        }

        // The armor figure for UI READOUTS: the item-card value (skill/perk-
        // scaled via the engine's own GetArmorValue), so SMF's numbers agree
        // with every card the player compares them against (owner test
        // 2026-08-20: summary said +42 while both cards said 45). Multiplied by
        // the temper ratio it reproduces the real item's card exactly. Display
        // only - SetTokenStats/StampSnapshotStats keep writing the raw base.
        // Falls back to the raw rating on VR (GetArmorValue thunk unverified -
        // same conservative branch as MeasureTemperMult) and when the engine
        // value is unusable.
        float CardArmorOf(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) {
                return 0.0f;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || REL::Module::IsVR()) {
                return static_cast<float>(a_armo->GetArmorRating());
            }
            RE::InventoryEntryData bare{ a_armo, 0 };
            const float card = player->GetArmorValue(&bare);
            return card > 0.0f ? card : static_cast<float>(a_armo->GetArmorRating());
        }

        // Persist class's applied preset ("" = manual). Mirrors a box's preset:
        // the FILE is the identity, the name is for display.
        std::string g_persistPreset;
        std::string g_persistPresetName;

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

        // WriteFileAtomic moved to src/AtomicWrite.h - the ability registry needs
        // the same guarantee, and a second copy of it would be a second place to
        // fix.

        void MaybeWritePre164Backup();  // fwd (defined below)

        // Returns whether the settings actually reached disk. It used to be void,
        // so every caller reported success no matter what happened (F05 / X5) -
        // including AddBox, whose true/false the capture flow reads to decide
        // whether to move the physical item into the hidden store. Taking the
        // user's item and then failing to record where it went is the one
        // outcome worth refusing.
        bool WriteJson(bool a_writeManifest = true)
        {
            if (g_settingsUnreadable) {
                // One line per attempt: the user needs to know their edits are
                // not sticking, and why, without a log dive.
                SKSE::log::error(
                    "settings: REFUSING to write - {} exists but did not load. The file on "
                    "disk is your real data and is left alone; fix or remove it (a "
                    "last-known-good copy is at {}), then restart the game.",
                    kSettingsPath, kSettingsBakPath);
                return false;
            }
            MaybeWritePre164Backup();
            nlohmann::json doc;
            doc["schema"] = kSchema;
            doc["enabled"] = g_cefEnabled;
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                nlohmann::json jb;
                jb["boxId"] = b.boxId;
                jb["label"] = b.label;
                jb["token"] = b.token;
                jb["contents"] = b.contents;
                jb["ability"] = b.ability;
                jb["enabled"] = b.enabled;
                jb["armorType"] = b.armorType;
                jb["preset"] = b.preset;          // file = identity
                jb["presetName"] = b.presetName;  // display only
                jb["uiVisible"] = b.uiVisible;
                jb["wear"] = b.wear;
                arr.push_back(std::move(jb));
            }
            doc["boxes"] = std::move(arr);
            doc["persist"]["contents"] = g_persist;
            doc["persist"]["preset"] = g_persistPreset;
            doc["persist"]["presetName"] = g_persistPresetName;

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

            // Item-data opt-OUTs: store the OFF content-ids (default on = absent).
            const auto writeOffSet = [&doc](const char* a_key,
                                         const std::unordered_set<std::string>& a_set) {
                auto arr = nlohmann::json::array();
                for (const auto& id : a_set) {
                    arr.push_back(id);
                }
                doc[a_key] = std::move(arr);
            };
            writeOffSet("statEnchantOff", g_statEnchantOff);
            writeOffSet("statWeightOff", g_statWeightOff);
            writeOffSet("statArmorOff", g_statArmorOff);

            // Per-content hidden shape names: { content-id: [shapeName, ...] }.
            auto hideShapes = nlohmann::json::object();
            for (const auto& [id, names] : g_hideShapes) {
                if (!names.empty()) {
                    hideShapes[id] = names;
                }
            }
            doc["hideShapes"] = std::move(hideShapes);

            // Show-real-body opt-in: store the ON content-ids (default off = absent).
            auto realBodies = nlohmann::json::array();
            for (const auto& id : g_showRealBody) {
                realBodies.push_back(id);
            }
            doc["showRealBody"] = std::move(realBodies);

            // Capture blacklist: switches + the user's own deny-list extension.
            // Structural (hard) skips and the shipped defaults live in code.
            // No "allowDynamic" is written (or read back): the dynamic-form
            // skip is a hard invariant (review P1-4).
            {
                const auto pol = CapturePolicySnapshot();
                auto blacklist = nlohmann::json::object();
                blacklist["names"] = pol->names;
                blacklist["plugins"] = pol->plugins;
                blacklist["ids"] = pol->ids;
                blacklist["allowNonPlayable"] = pol->allowNonPlayable;
                blacklist["disableDefaults"] = pol->disableDefaults;
                doc["captureBlacklist"] = std::move(blacklist);
            }

            auto enchants = nlohmann::json::object();
            for (const auto& [id, effs] : g_contentEnchants) {
                auto arr2 = nlohmann::json::array();
                for (const auto& e : effs) {
                    arr2.push_back({ { "mgef", e.mgef }, { "mag", e.magnitude } });
                }
                enchants[id] = std::move(arr2);
            }
            doc["enchants"] = std::move(enchants);

            auto tempers = nlohmann::json::object();
            for (const auto& [id, mult] : g_contentTemper) {
                tempers[id] = mult;
            }
            doc["tempers"] = std::move(tempers);

            auto custody = nlohmann::json::object();
            for (const auto& [id, e] : g_custody) {
                custody[id] = { { "name", e.name }, { "event", e.event }, { "when", e.when } };
            }
            doc["custodyLog"] = std::move(custody);
            EmitPublishJson(doc);

            if (!WriteFileAtomic(kSettingsPath, doc.dump(2))) {
                SKSE::log::error("settings: cannot write {}", kSettingsPath);
                return false;
            }
            SKSE::log::info("settings: wrote {} box def(s) (enabled={})", g_boxes.size(), g_cefEnabled);
            if (a_writeManifest) {
                WriteCarrierManifest();
            }
            return true;
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

        // MakeColonId moved to src/FormId.h (the ability pool needs it too). Call
        // sites are unchanged: it is still CostumeFW::MakeColonId, just one
        // namespace further out.

        // --- boxId (v1.6.4) ---------------------------------------------------
        // The logical identity of a box, as opposed to the physical token it
        // currently holds. Sixteen hex characters either way, so nothing
        // downstream has to know whether an id was migrated or issued.
        // DeriveBoxId (the migrated half) is exported - see BoxStore.h.
        std::string NewBoxId()
        {
            // Issued once per box and never reused. Seeded per call from
            // random_device: boxes are created by hand, minutes apart, so the
            // cost is irrelevant and a shared engine would be one more piece of
            // mutable state under the store lock.
            static std::mt19937_64 s_rng{ std::random_device{}() };
            char buf[17]{};
            std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(s_rng()));
            return buf;
        }

        // --- pre-1.6.4 settings snapshot --------------------------------------
        // Taken at the moment 1.6.4 is about to write a file that was still in
        // the 1.6.3 shape, and never again. CopyFileA with bFailIfExists=TRUE is
        // the whole "once" mechanism: no flag to keep in sync, and a pre164 file
        // the user restored by hand is not silently overwritten.
        void MaybeWritePre164Backup()
        {
            if (!g_settingsWasPre164) {
                return;
            }
            g_settingsWasPre164 = false;  // one attempt per load, whatever happens
            if (::CopyFileA(kSettingsPath, kSettingsPre164Path, TRUE)) {
                SKSE::log::info(
                    "settings: kept the pre-1.6.4 file as {} before the first 1.6.4 write "
                    "(CEF never reads it; it is there if you go back to 1.6.3)",
                    kSettingsPre164Path);
            } else if (::GetLastError() != ERROR_FILE_EXISTS) {
                SKSE::log::warn("settings: could not keep a pre-1.6.4 copy at {} (error {})",
                    kSettingsPre164Path, ::GetLastError());
            }
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
            // v1.3.0 fold: CostumeFW_VanillaSlots_001.esp (F1 vanilla-slot tokens,
            // dev-only patch, ids 0x800-0x815) folded into CostumeFW.esp with
            // +0x200 (0x8xx -> 0xAxx; +0x100 would collide with the v1.2.1
            // renumber range 0x9xx). ROOT J: mirror tools/espmerge's per-patch
            // offset - change both together. No drop list for this patch.
            constexpr std::string_view kVanilla = ":CostumeFW_VanillaSlots_001.esp";
            if (endsWithCI(a_id, kVanilla)) {
                const std::string prefix = a_id.substr(0, a_id.size() - kVanilla.size());
                const bool okHex = !prefix.empty() && prefix.size() <= 6 &&
                    prefix.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
                if (!okHex) {
                    SKSE::log::warn("settings: unparseable legacy vanilla-slot id '{}' - left as-is", a_id);
                    return false;
                }
                const auto lid = static_cast<std::uint32_t>(std::strtoul(prefix.c_str(), nullptr, 16));
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "%06X", lid + 0x200u);
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
        bool CorePluginLoaded();                                       // fwd (defined below)
        std::string_view FilenameOf(const RE::TESFile* a_file);        // fwd (defined below)
        // The keyword that says "this ARMO is a box token". Named once: the
        // audit prints it, and tools/espmerge stamps it (kMarkerEdid there).
        constexpr const char* kBoxTokenMarkerEdid = "CFW_BoxTokenMarker";
        RE::BGSKeyword* BoxTokenMarkerKeyword();                       // fwd (defined below)
        void SetTokenStats(const BoxDefInfo& a_box);                   // fwd (defined below)
        void ApplyBoxLabelToToken(const BoxDefInfo& a_box);            // fwd (defined below)
        void RestoreTokenDefaultName(const std::string& a_token);      // fwd (defined below)
        void ResetTokenStats(const std::string& a_token);              // fwd (defined below)
        void ReapplyStatsForContent(const std::string& a_id);          // fwd (defined below)
        void ResetUnclaimedTokenStats();                               // fwd (defined below)
        // The inventory name each token carried BEFORE a box label was stamped
        // on it. Declared here so `cef tokens` can print it beside the current
        // one; filled by ApplyBoxLabelToToken, far below.
        extern std::unordered_map<std::uint32_t, std::string> g_tokenDefaultNames;

        // Tokens SetTokenStats has written to in this process. A box's armor,
        // weight, class, name and keywords live on the token ARMO's BASE form,
        // and the only record of "CEF put that there" was the box definition
        // itself - so a definition that disappeared took the undo with it. A
        // settings reload replaces g_boxes wholesale and restamps only the boxes
        // it read, which left a deleted box's token carrying its old stats for
        // the rest of the session (review 2026-09-11 F06).
        //
        // Process-lived on purpose: base-form fields reset to the plugin's
        // values when the process restarts, so this set has nothing to survive.
        std::unordered_set<std::string> g_stampedTokens;
        void RecordCustody(const std::string& a_id, const char* a_event);  // fwd (below)

        // --- FSMP carrier manifest (approach B) --------------------------------
        // Inputs for the carrier `sync` (src/nifcarrier): per box, its carrier
        // key and the resolved worn-NIF path of every content. sync rebuilds
        // <carrierKey>_carrier.nif (+ merged physics XML when 2+ contents carry
        // SMP) from this; the per-token ARMA points at that carrier, so equipping
        // the token makes FSMP grow the physics bones CEF's rebind then binds the
        // injected meshes to. Written on every box-def persist, skipped when
        // nothing changed (keeps sync's hash-skip effective).
        constexpr const char* kManifestPath = "Data\\SKSE\\Plugins\\CEF_carrier_manifest.json";

        void ScheduleAutoSync();  // fwd (defined below)

        // r3 (re-review P1-2) established that every derived processor must
        // iterate an admitted snapshot rather than raw box.contents, so a
        // configured-but-blocked content is not read by stats/keyword/ability/
        // UI code either. That filter used to be ONE list, built by resolving
        // each id's model through ResolveAdmittedModelPath (r4).
        //
        // It is now two, because the two questions are different (2026-09-11):
        //   - stats  -> StatAdmittedContents (BoxStore.h): blacklist / capture
        //     policy only. Race- and sex-independent.
        //   - visual -> ResolveAdmittedModelPath directly, at the seam that
        //     needs the model anyway (injection, WriteCarrierManifest).
        // Both stay silent; loud refusal logs belong at the explicit gates.

        void WriteCarrierManifest()
        {
            // r4: one immutable generation and one resolver for base admission,
            // selected-ARMA hard/id admission, and the exact model path used by
            // injection. No private ARMO->ARMA walk is allowed here.
            const auto pol = CapturePolicySnapshot();
            nlohmann::json doc;
            // 2 (v1.6.4): every box entry carries a "carrierKey". A version-1
            // manifest has only "slot", and the builder reads generation 0's
            // key off it - "Box<slot>" is exactly what version 1 was already
            // producing, so the fallback is byte-exact.
            doc["version"] = 2;
            const auto resolveContent = [&](const std::string& id) -> nlohmann::json {
                std::string nif;
                if (!ResolveAdmittedModelPath(
                        id, EffectiveSexFor(id), *pol, nif, false)) {
                    return nullptr;
                }
                return { { "id", id }, { "nif", nif } };
            };
            // Published snapshots are sex-split and frozen. Resolve from the
            // snapshot's gender override, never from the live box setting.
            const auto resolvePublishedContent = [&](const std::string& id, RE::SEX requested,
                                                     const PubSnapshot& snap) -> nlohmann::json {
                RE::SEX sex = requested;
                if (const auto it = snap.settings.find(id); it != snap.settings.end() && it->second) {
                    if (it->second->genderMode == 1) sex = RE::SEXES::kMale;
                    if (it->second->genderMode == 2) sex = RE::SEXES::kFemale;
                }
                std::string nif;
                if (!ResolveAdmittedModelPath(id, sex, *pol, nif, false)) {
                    return nullptr;
                }
                return { { "id", id }, { "nif", nif } };
            };
            auto arr = nlohmann::json::array();
            for (const auto& b : g_boxes) {
                // Several boxes share one biped slot from v1.6.4 on, so the slot
                // cannot name the artifacts any more - they would overwrite each
                // other's NIF, XML, hash and revision pool. The carrier key names
                // them, and a box whose token does not resolve has none: it used
                // to go in as slot 0 and have the builder make a Box0 carrier
                // nothing could ever wear.
                const int slot = SlotNumberOf(ResolveArmo(b.token));
                const std::string carrierKey = tokenid::CarrierKeyFor(b.token, slot);
                if (carrierKey.empty()) {
                    continue;
                }
                nlohmann::json jb;
                jb["slot"] = slot;
                jb["token"] = b.token;
                jb["carrierKey"] = carrierKey;
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
            if (NpcEspLoaded()) {
                auto published = nlohmann::json::array();
                for (const auto& snap : PublishedSnapshot()) {
                    nlohmann::json entry;
                    entry["pub"] = snap.pubSlot;
                    auto male = nlohmann::json::array();
                    auto female = nlohmann::json::array();
                    for (const auto& id : snap.contents) {
                        if (auto c = resolvePublishedContent(id, RE::SEXES::kMale, snap); !c.is_null())
                            male.push_back(std::move(c));
                        if (auto c = resolvePublishedContent(id, RE::SEXES::kFemale, snap); !c.is_null())
                            female.push_back(std::move(c));
                    }
                    entry["contents_m"] = std::move(male);
                    entry["contents_f"] = std::move(female);
                    published.push_back(std::move(entry));
                }
                doc["published"] = std::move(published);
                auto npcPersist = nlohmann::json::array();
                for (const auto& assignment : NprAssignmentsSnapshot()) {
                    if (assignment.unresolved) continue;
                    PubSnapshot live;
                    for (const auto& id : assignment.contents) {
                        auto cfg = std::make_shared<ContentSettings>();
                        cfg->genderMode = GenderModeFor(id);
                        live.settings.emplace(id, std::move(cfg));
                    }
                    auto contents = nlohmann::json::array();
                    const RE::SEX sex = assignment.female ?
                        RE::SEXES::kFemale : RE::SEXES::kMale;
                    for (const auto& id : assignment.contents) {
                        if (auto c = resolvePublishedContent(id, sex, live); !c.is_null())
                            contents.push_back(std::move(c));
                    }
                    npcPersist.push_back({
                        { "pool", assignment.poolSlot },
                        { "sex", assignment.female ? "f" : "m" },
                        { "contents", std::move(contents) }
                    });
                }
                doc["npcPersist"] = std::move(npcPersist);
            }
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
        // The carrier `sync` rewrites a PRE-CREATED revision slot file and
        // records it in carriers.json. usvfs shows external REWRITES of existing
        // files but never externally-created NEW files (verified in-game
        // 2026-07-03), which is exactly why the slots are pre-created. Repointing
        // the token ARMA at the new slot path makes the next equip load a path the
        // engine has not cached this session = the freshly built carrier. This is
        bool RepointCarrierSexed(RE::TESObjectARMO* a_armo, const std::string& a_male,
            const std::string& a_female)
        {
            if (!a_armo || a_armo->armorAddons.empty()) {
                return false;
            }
            auto* arma = a_armo->armorAddons.front();
            bool changed = false;
            if (a_male != arma->bipedModels[RE::SEXES::kMale].model.c_str()) {
                arma->bipedModels[RE::SEXES::kMale].model = a_male.c_str();
                changed = true;
            }
            if (a_female != arma->bipedModels[RE::SEXES::kFemale].model.c_str()) {
                arma->bipedModels[RE::SEXES::kFemale].model = a_female.c_str();
                changed = true;
            }
            return changed;
        }

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
                const std::string key = tokenid::CarrierKeyFor(b.token, slot);
                if (key.empty()) {
                    continue;  // not a box token: nothing of ours to repoint
                }
                auto entry = doc.find(key);
                if (entry == doc.end() && key == "Box" + std::to_string(slot)) {
                    // A carriers.json written before v1.6.4 keys generation 0 by
                    // the bare slot number. The next sync rewrites it under the
                    // carrier key; until then read the old key, or every box
                    // would drop back to the ESP-default (stale) carrier for a
                    // session. Generation 0 only - a pool token must never pick
                    // up the generation-0 entry that happens to share its slot.
                    entry = doc.find(std::to_string(slot));
                }
                if (entry == doc.end() || !entry->is_object()) {
                    continue;
                }
                const std::string file = entry->value("file", "");
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
                        "carrier override: {} carrier '{}' missing/invalid on disk - keeping ESP default",
                        key, file);
                    continue;
                }
                // Repoint the token's carrier ARMA at the newest revision. USER-DRIVEN
                // APPLY: the new carrier LOADS when the user re-equips the token - CEF
                // does NOT auto-swap (a programmatic re-equip coalesces / stalls in the
                // equip queue and can't be automated reliably). A manual re-equip reloads
                // cleanly once FSMP has settled.
                if (RepointCarrier(armoA, file)) {
                    SKSE::log::info("carrier override: {} token '{}' -> {} (re-equip to apply)",
                        key, b.token, file);
                    anyChanged = true;
                }
            }
            if (NpcEspLoaded()) {
                const auto pit = doc.find("published");
                if (pit != doc.end() && pit->is_object()) {
                    for (const auto& snap : PublishedSnapshot()) {
                        const auto key = std::to_string(snap.pubSlot);
                        const auto entry = pit->find(key);
                        if (entry == pit->end() || !entry->is_object()) {
                            continue;
                        }
                        const std::string male = entry->value("fileM", "");
                        const std::string female = entry->value("fileF", "");
                        if (!CarrierFileOnDisk(male) || !CarrierFileOnDisk(female)) {
                            SKSE::log::warn("published carrier {} missing/invalid - keeping ESP defaults", key);
                            continue;
                        }
                        if (RepointCarrierSexed(PubTokenArmo(snap.pubSlot), male, female)) {
                            SKSE::log::info("published carrier {} -> male={}, female={} (Refresh to apply)",
                                key, male, female);
                            anyChanged = true;
                        }
                    }
                }
            }
            if (NpcEspLoaded()) {
                const auto nit = doc.find("npcPersist");
                if (nit != doc.end() && nit->is_object()) {
                    for (const auto& assignment : NprAssignmentsSnapshot()) {
                        if (assignment.unresolved) continue;
                        const auto key = std::to_string(assignment.poolSlot);
                        const auto entry = nit->find(key);
                        if (entry == nit->end() || !entry->is_object()) continue;
                        const std::string file = entry->value("file", "");
                        if (!CarrierFileOnDisk(file)) {
                            SKSE::log::warn("NPC persist carrier {} missing/invalid - keeping ESP default", key);
                            continue;
                        }
                        if (RepointCarrier(NprTokenArmo(assignment.poolSlot), file)) {
                            SKSE::log::info("NPC persist carrier {} -> {} (manual Refresh applies FSMP)",
                                key, file);
                            anyChanged = true;
                        }
                    }
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
        // session, -2 = timed out (external child terminated), -3 = failed to
        // start, otherwise the nifcarrier exit code (in-proc: 0 ok / 2 failed).
        std::atomic<int> g_lastSyncExit{ -999 };
        // Live sync progress (owner feedback 2026-07-31: a silent multi-minute
        // rebuild is indistinguishable from a wedged one). Written by the sync
        // worker via SyncOptions::progress, read by the heartbeat thread and
        // the Diagnostics page.
        std::mutex g_syncStageMutex;
        std::string g_syncStage;
        std::atomic<int> g_syncDone{ 0 };
        std::atomic<int> g_syncTotal{ 0 };
        std::atomic<std::int64_t> g_syncStartMs{ 0 };

        std::string SyncProgressBrief()
        {
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int secs = static_cast<int>((nowMs - g_syncStartMs.load()) / 1000);
            std::string stage;
            {
                std::lock_guard lk(g_syncStageMutex);
                stage = g_syncStage;
            }
            const int total = g_syncTotal.load();
            if (total > 0) {
                return std::format("{}, step {}/{}, {}s", stage,
                    std::min(g_syncDone.load() + 1, total), total, secs);
            }
            return std::format("{}, {}s", stage.empty() ? "starting" : stage, secs);
        }

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
            {
                std::lock_guard lk(g_syncStageMutex);
                g_syncStage = "starting";
            }
            g_syncDone = 0;
            g_syncTotal = 0;
            g_syncStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::thread([gen]() {
                // Heartbeat, not a tripwire (owner feedback 2026-07-31): a full
                // rebuild of bloated carriers measured 130-140s while the worker
                // was perfectly healthy, and the old single 120s sleep then
                // declared it "wedged; blocked until restart" - wrong on both
                // counts (nothing blocks, and the run finishes). Poll instead
                // and keep the player informed. Beat ladder (owner-tuned): the
                // "is it stuck?" doubt sets in around 5s, so the FIRST beat
                // lands before it at 2s; once a notice has shown the player
                // will wait, so +5s, then +10s (anti-spam), then +30s repeating
                // (2, 7, 17, 47, 77... from start). The common ~1s unchanged
                // pass still shows nothing. The 120s mark logs a warning that
                // says SLOW, not stuck.
                using namespace std::chrono;
                static constexpr int kBeatGaps[] = { 2, 5, 10, 30 };  // then 30 repeating
                const auto start = steady_clock::now();
                std::size_t beat = 0;
                auto nextNotice = start + seconds(kBeatGaps[0]);
                bool loggedSlow = false;
                while (g_syncRunning.load() && g_syncGen.load() == gen) {
                    std::this_thread::sleep_for(milliseconds(500));
                    const auto now = steady_clock::now();
                    if (now < nextNotice) {
                        continue;
                    }
                    ++beat;
                    nextNotice = now + seconds(kBeatGaps[std::min<std::size_t>(
                        beat, std::size(kBeatGaps) - 1)]);
                    const auto brief = SyncProgressBrief();
                    SKSE::log::info("auto-sync: heartbeat - rebuilding ({})", brief);
                    const std::string msg =
                        std::format("Costume carriers: rebuilding ({})...", brief);
                    SKSE::GetTaskInterface()->AddTask(
                        [msg]() { RE::DebugNotification(msg.c_str()); });
                    if (!loggedSlow &&
                        duration_cast<seconds>(now - start).count() >= 120) {
                        loggedSlow = true;
                        SKSE::log::warn(
                            "auto-sync: still running after 120s - large carriers can take "
                            "minutes; this is slow, not stuck (heartbeat above, live status "
                            "in SMF Diagnostics)");
                    }
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
                    opts.progress = [](const char* a_stage, int a_done, int a_total) {
                        {
                            std::lock_guard lk(g_syncStageMutex);
                            g_syncStage = a_stage;
                        }
                        g_syncDone = a_done;
                        g_syncTotal = a_total;
                    };
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
                    const int built = sr.built;
                    SKSE::GetTaskInterface()->AddTask([built]() {
                        SKSE::log::info("auto-sync: done - applying carrier revisions");
                        ApplyCarrierOverridesImpl(true);
                        if (built > 0) {
                            // Close the heartbeat's loop on screen; an unchanged
                            // pass (built == 0) stays silent like before. Log it
                            // too - §6 review: the notice was screen-only, so a
                            // user-supplied log could not show completion.
                            SKSE::log::info("auto-sync: rebuilt {} item(s)", built);
                            RE::DebugNotification(
                                std::format("Costume carriers: rebuilt {} item(s)", built)
                                    .c_str());
                        }
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

    std::string DeriveBoxId(std::string_view a_seed)
    {
        // FNV-1a 64. Not a security hash; it needs to be stable across runs and
        // builds, which std::hash explicitly is not.
        //
        // A box that existed before 1.6.4 seeds this with its canonical token,
        // which is sound exactly because one box holds one token (ROOT B) - a
        // rule 1.6.4 keeps; only the biped slot stops being unique.
        std::uint64_t h = 1469598103934665603ULL;
        for (const unsigned char c : a_seed) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        char buf[17]{};
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(h));
        return buf;
    }

    void LoadBoxes()
    {
        StoreLock lk;
        g_boxes.clear();
        g_persist.clear();
        g_hideRules.clear();
        g_genderModes.clear();
        g_bodyMorphOn.clear();
        g_statEnchantOff.clear();
        g_statWeightOff.clear();
        g_statArmorOff.clear();
        g_hideShapes.clear();
        g_contentShapes.clear();
        g_showRealBody.clear();
        g_contentEnchants.clear();
        g_contentTemper.clear();
        g_persistPreset.clear();
        g_persistPresetName.clear();
        g_custody.clear();
        PublishPolicy({});
        g_cefEnabled = true;
        g_settingsLoadOk = false;  // set only on the clean path below

        // Read CEF_settings.json; fall back to the legacy costume_boxes.json once
        // (migration) and rewrite into the new file at the end of load.
        bool migrated = false;
        g_settingsUnreadable = false;  // re-decided by this load
        std::ifstream f(kSettingsPath);
        if (!f) {
            std::ifstream old(kOldBoxesPath);
            if (!old) {
                // No file at all: a fresh install, not a failure - writing must
                // work (g_settingsUnreadable stays false). But the sweep still
                // stays out: a settings file that went missing while the save
                // still holds a full store is the case we know LEAST about, and
                // an empty held set there would hand back everything at once.
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
                g_settingsUnreadable = true;
                return;  // legacy file was corrupt; nothing else to try
            }
            // Corrupt main file: fall back to the last-known-good backup taken at
            // the previous successful load. Without this, the cleared state above
            // would look like "no settings" and the next WriteJson would make the
            // loss permanent (Codex review 2026-07-05 A-1).
            std::ifstream bak(kSettingsBakPath);
            if (!bak) {
                g_settingsUnreadable = true;
                return;
            }
            try {
                bak >> doc;
            } catch (const std::exception& e2) {
                SKSE::log::error("settings: backup parse error: {}", e2.what());
                g_settingsUnreadable = true;
                return;
            }
            fromBackup = true;
            SKSE::log::warn("settings: recovered from {} (main file corrupt; "
                            "it is rewritten on the next settings change)",
                kSettingsBakPath);
        }
        // The read handle goes NOW, before anything below can write. This
        // function writes from inside its own body - the v1.2.1 id heal and the
        // legacy migration both call WriteJson - and WriteFileAtomic replaces the
        // file with MoveFileEx(MOVEFILE_REPLACE_EXISTING), which needs DELETE
        // access on the destination. MSVC opens an ifstream without
        // FILE_SHARE_DELETE, so while this stream lives that rename fails with
        // ERROR_ACCESS_DENIED (5) and the write is lost.
        //
        // v1.6.4 made that path the normal one for everybody: a box with no
        // boxId sets `healed`, so the FIRST load after the upgrade is exactly
        // when the settings are written from in here. Before, `healed` was a rare
        // repair and the failure went unnoticed.
        f.close();
        // --- CoreMissing: judge nothing, change nothing (v1.6.4) -------------
        // Every question this function asks about a token - is it an ARMO, does
        // its plugin define box tokens - is answered against the loaded plugins.
        // Without CostumeFW.esp the honest answer to all of them is "cannot say",
        // not "broken", so the definitions are left exactly as they are and the
        // file is not rewritten. CEF can do nothing useful this session anyway.
        //
        // This runs BEFORE the G3 circuit breaker below on purpose: G3 only trips
        // at two or more dropped boxes, so a user with a single box would not be
        // covered by it.
        if (!CorePluginLoaded()) {
            SKSE::log::error(
                "settings: {} is not loaded - leaving {} untouched. Box definitions are "
                "not read, judged or rewritten this session; install/enable the plugin "
                "and restart.",
                tokenid::kCorePlugin, kSettingsPath);
            g_settingsUnreadable = true;  // every WriteJson refuses while set
            return;
        }
        // CoreOutdated: the plugin is there but CFW_BoxTokenMarker is not, so the
        // DLL is 1.6.4 and the plugin files are still 1.6.3. Every token would
        // fail the marker test and be quarantined - a whole catalogue declared
        // broken by a half-finished install. The VR patch has shipped as a
        // DLL-only download before, so this is a real way to arrive here.
        if (!BoxTokenMarkerKeyword()) {
            SKSE::log::error(
                "settings: {} is older than this DLL - it has no CFW_BoxTokenMarker, so no "
                "token can be recognised. Leaving {} untouched; update the plugin files to "
                "1.6.4 and restart.",
                tokenid::kCorePlugin, kSettingsPath);
            g_settingsUnreadable = true;
            return;
        }
        // Heal pre-merge colon-ids (v1.2.1 plugin consolidation) wherever the
        // settings persist them; a healed file is rewritten once below.
        bool healed = false;
        // Set when a box arrives without a boxId: the file is still in the 1.6.3
        // shape, so a copy of it is kept before the first 1.6.4 write.
        bool wasPre164 = false;
        // ROOT B (border audit 2026-07-09): the field reads below are typed
        // value()/get<> accesses. A WRONG-TYPED field in a hand-edited settings file
        // (e.g. "boxes": 5) throws json::type_error - which the parse-only guard above
        // did NOT cover, so it crashed at EVERY launch. Wrap the extraction: a bad
        // field discards the partial state and leaves the on-disk file intact (no
        // data-destroying WriteJson) instead of crashing. Also enforce ingest
        // invariants here (one box per token, one holder per content id).
        std::vector<std::string> seenTokens;    // ROOT B: one box per token
        std::vector<std::string> seenContents;  // ROOT B: one holder per content id
        std::vector<std::string> seenBoxIds;    // v1.6.4: one definition per boxId
        int boxesSeen = 0;                      // rows the file offered
        int boxesDropped = 0;                   // rows this load refused (G3)
        try {
        g_cefEnabled = doc.value("enabled", true);
        const auto boxes = doc.value("boxes", nlohmann::json::array());
        for (const auto& jb : boxes) {
            ++boxesSeen;
            BoxDefInfo b;
            b.boxId = jb.value("boxId", std::string{});
            b.label = jb.value("label", std::string{});
            b.token = jb.value("token", std::string{});
            healed |= MigrateLegacyColonId(b.token);
            healed |= CanonicalizeColonId(b.token);  // ROOT D
            // v1.6.4: fold a CEF plugin name written in the wrong case onto the
            // official spelling. Both resolve to the same record, but the store
            // keys on the STRING - so "costumefw.esp" and "CostumeFW.esp" were two
            // different tokens, and FreeTokens would offer a token a box already
            // held.
            healed |= tokenid::CanonicalizeCefColonId(b.token);
            if (b.token.empty()) {
                ++boxesDropped;
                continue;
            }
            if (b.boxId.empty()) {
                // Pre-1.6.4 definition. Derive the id from the canonical token
                // rather than issuing a random one: migrating the same file twice
                // then produces the same ids, so a migration interrupted before
                // the save cannot hand one box two identities.
                b.boxId = DeriveBoxId(b.token);
                wasPre164 = true;
                healed = true;
            }
            if (std::find(seenBoxIds.begin(), seenBoxIds.end(), b.boxId) != seenBoxIds.end()) {
                SKSE::log::warn("boxes: LoadBoxes drops a second definition on boxId '{}'", b.boxId);
                ++boxesDropped;
                healed = true;
                continue;
            }
            // What the CURRENT load order can deliver for this token. The entry is
            // kept either way (see TokenState): a definition is the only record of
            // what a costume was made of, and dropping it to tidy the file is how
            // that record is lost. An unusable token is quarantined instead.
            b.tokenState = ClassifyBoxToken(b.token);
            if (b.tokenState != TokenState::Resolved) {
                SKSE::log::warn(
                    "boxes: box '{}' token '{}' is not usable ({}) - the definition is KEPT and "
                    "quarantined; see the Recovery page to re-point or remove it",
                    b.boxId, b.token, TokenStateReason(b.tokenState));
            }
            if (std::find(seenTokens.begin(), seenTokens.end(), b.token) != seenTokens.end()) {
                // ROOT B: token-keyed mutators only ever reach the first box, so a
                // second box on the same token is a dead "item printer" - drop it.
                SKSE::log::warn("boxes: LoadBoxes drops second box on token '{}'", b.token);
                ++boxesDropped;
                healed = true;
                continue;
            }
            seenTokens.push_back(b.token);
            seenBoxIds.push_back(b.boxId);
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
            b.presetName = jb.value("presetName", std::string{});
            // Pre-1.6.2.1: "preset" carried the display NAME. Park it as the name
            // and leave the file empty - Preset::MigrateAssignments() resolves it.
            // New rows always write presetName, including unresolved/manual rows.
            // Names can end in .json; valid files can use uppercase .JSON.
            if (!jb.contains("presetName")) {
                b.presetName = b.preset;
                b.preset.clear();
            } else if (b.presetName.empty()) {
                b.presetName = b.preset;  // empty display name: show something
            }
            b.uiVisible = jb.value("uiVisible", true);
            b.wear = jb.value("wear", false);
            g_boxes.push_back(std::move(b));
        }
        // --- G3: the all-or-nothing circuit breaker (v1.6.4) -----------------
        // If this load refused EVERY box the file offered, the likely cause is
        // something wrong on our side - a plugin that failed to load, an ingest
        // rule of ours that is too strict, a schema mistake - not a user who
        // corrupted all of their boxes at once. Applying that result and writing
        // it back turns a bad session into permanent data loss. The 2026-09-08
        // incident is exactly this shape, and only the .bak saved those boxes.
        //
        // Two or more, because refusing the single box a one-box file contains is
        // a legitimate outcome (it really was a publish token) and must stay
        // reportable. The one-box user is covered by the CoreMissing guard above,
        // which is the case that actually produces a false wipe.
        if (boxesDropped >= 2 && boxesDropped >= boxesSeen) {
            SKSE::log::error(
                "settings: REFUSED every box in {} ({} of {}) - that is far more likely to be "
                "our fault than yours, so nothing is applied and the file is left alone. "
                "Check the warnings above, then restart.",
                kSettingsPath, boxesDropped, boxesSeen);
            g_boxes.clear();
            g_settingsUnreadable = true;  // every WriteJson refuses while set
            return;
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
        g_persistPresetName = persist.value("presetName", std::string{});
        if (!persist.contains("presetName")) {
            g_persistPresetName = g_persistPreset;
            g_persistPreset.clear();
        } else if (g_persistPresetName.empty()) {
            g_persistPresetName = g_persistPreset;
        }
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
        for (const auto& id : doc.value("showRealBody", nlohmann::json::array())) {
            if (id.is_string()) {
                auto s = id.get<std::string>();
                healed |= MigrateLegacyColonId(s);
                healed |= CanonicalizeColonId(s);  // ROOT D
                g_showRealBody.insert(std::move(s));
            }
        }
        const auto readOffSet = [&doc, &healed](const char* a_key,
                                    std::unordered_set<std::string>& a_set) {
            for (const auto& id : doc.value(a_key, nlohmann::json::array())) {
                if (id.is_string()) {
                    auto s = id.get<std::string>();
                    healed |= MigrateLegacyColonId(s);
                    healed |= CanonicalizeColonId(s);  // ROOT D
                    a_set.insert(std::move(s));
                }
            }
        };
        readOffSet("statEnchantOff", g_statEnchantOff);
        readOffSet("statWeightOff", g_statWeightOff);
        readOffSet("statArmorOff", g_statArmorOff);
        {
            // Capture blacklist (v1.3.2): absent field (any pre-1.3.2 json) =
            // hard skips active, shipped defaults active, no user entries -
            // the safe default. A legacy "allowDynamic" key is deliberately
            // IGNORED (hard invariant since review r2, P1-4). Built into a
            // fresh policy and atomically published (review P1-5).
            const auto blacklist = doc.value("captureBlacklist", nlohmann::json::object());
            policy::CapturePolicy pol;
            for (const auto& entry : blacklist.value("names", nlohmann::json::array())) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    pol.names.push_back(entry.get<std::string>());
                }
            }
            for (const auto& entry : blacklist.value("plugins", nlohmann::json::array())) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    pol.plugins.push_back(entry.get<std::string>());
                }
            }
            for (const auto& entry : blacklist.value("ids", nlohmann::json::array())) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    auto id = entry.get<std::string>();
                    healed |= CanonicalizeColonId(id);  // ROOT D parity
                    pol.ids.push_back(std::move(id));
                }
            }
            pol.allowNonPlayable = blacklist.value("allowNonPlayable", false);
            pol.disableDefaults = blacklist.value("disableDefaults", false);
            PublishPolicy(std::move(pol));
        }
        const auto hideShapes = doc.value("hideShapes", nlohmann::json::object());
        for (auto it = hideShapes.begin(); it != hideShapes.end(); ++it) {
            std::vector<std::string> names;
            for (const auto& n : it.value()) {
                if (n.is_string()) {
                    names.push_back(n.get<std::string>());
                }
            }
            if (!names.empty()) {
                std::string key = it.key();
                healed |= MigrateLegacyColonId(key);
                healed |= CanonicalizeColonId(key);  // ROOT D
                g_hideShapes[std::move(key)] = std::move(names);
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
        const auto tempers = doc.value("tempers", nlohmann::json::object());
        for (auto it = tempers.begin(); it != tempers.end(); ++it) {
            if (!it.value().is_number()) {
                continue;
            }
            const float mult = it.value().get<float>();
            if (mult > 1.0001f) {  // tempering only raises; 1.0 = no entry
                std::string key = it.key();
                healed |= MigrateLegacyColonId(key);
                healed |= CanonicalizeColonId(key);  // ROOT D
                g_contentTemper[std::move(key)] = mult;
            }
        }
        // Shape-tolerant: the row-per-id object is the current form, but a build
        // between the sweep and the Recovery UI wrote an event ARRAY here. A
        // settings file is a user's data - never make one unreadable over a
        // schema change of ours (this exact mismatch threw and emptied the whole
        // store into the ROOT B path, 2026-09-08). Anything else is ignored.
        nlohmann::json custody = nlohmann::json::object();
        if (const auto node = doc.find("custodyLog"); node != doc.end()) {
            if (node->is_object()) {
                custody = *node;
            } else if (node->is_array()) {
                for (const auto& e : *node) {
                    if (e.is_object() && e.contains("id") && e["id"].is_string()) {
                        custody[e["id"].get<std::string>()] = e;
                    }
                }
            }
        }
        for (auto it = custody.begin(); it != custody.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            std::string key = it.key();
            healed |= MigrateLegacyColonId(key);
            healed |= CanonicalizeColonId(key);  // ROOT D
            if (key.empty()) {
                continue;
            }
            CustodyLogEntry ev;
            ev.id = key;
            ev.name = it.value().value("name", std::string{});
            ev.event = it.value().value("event", std::string{});
            ev.when = it.value().value("when", std::string{});
            g_custody[std::move(key)] = std::move(ev);
        }
        ParsePublishJson(doc);
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
        g_statEnchantOff.clear();
        g_statWeightOff.clear();
        g_statArmorOff.clear();
            g_contentEnchants.clear();
            g_contentTemper.clear();
            g_persistPreset.clear();
            g_persistPresetName.clear();
            g_custody.clear();
            PublishPolicy({});
            g_cefEnabled = true;
            // g_settingsLoadOk stays false: the orphan sweep must not run - and the
            // empty state we are holding must never reach the file (this is the
            // path a schema mistake of ours took on 2026-09-08, and only the .bak
            // saved the user's boxes).
            g_settingsUnreadable = true;
            return;
        }
        // Clean, fully validated load - the held sets are now trustworthy.
        g_settingsLoadOk = true;
        // Only now: the file really was the 1.6.3 shape and we really did read it.
        // The next write is the moment that shape stops existing anywhere, so
        // WriteJson keeps a copy of it first (MaybeWritePre164Backup).
        g_settingsWasPre164 = wasPre164;
        if (wasPre164) {
            SKSE::log::info("settings: pre-1.6.4 file - {} box(es) given a boxId derived from "
                            "their token", g_boxes.size());
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
        // After the new set is stamped: undo the tokens the reloaded file no
        // longer claims (F06). A box deleted from the json otherwise kept its
        // armor, weight, name and keywords on the token for the session.
        ResetUnclaimedTokenStats();
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
        StoreLock lk;
        ApplyCarrierOverridesImpl(a_refreshChanged);
    }

    void PersistCarrierStatus()
    {
        StoreLock lk;
        auto* console = RE::ConsoleLog::GetSingleton();
        const auto say = [&](const std::string& s) {
            SKSE::log::info("{}", s);
            if (console) {
                ConsolePrint(s.c_str());
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
        StoreLock lk;
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
        StoreLock lk;
        return g_cefEnabled;
    }

    bool SetCefEnabled(bool a_on)
    {
        StoreLock lk;
        g_cefEnabled = a_on;
        WriteJson();
        return g_cefEnabled;
    }

    bool BoxTokenUsable(const BoxDefInfo& a_box)
    {
        return a_box.tokenState == TokenState::Resolved;
    }

    const char* TokenStateReason(TokenState a_state)
    {
        switch (a_state) {
        case TokenState::Resolved:       return "";
        case TokenState::PoolMissing:    return "pool-missing";
        case TokenState::ParseError:     return "parse-error";
        case TokenState::ForeignPlugin:  return "foreign-plugin";
        case TokenState::NotArmo:        return "not-armo";
        case TokenState::NoMarker:       return "no-marker";
        case TokenState::UnresolvedForm: return "unresolved-form";
        }
        return "unknown";
    }

    bool IsBoxToken(std::uint32_t a_form)
    {
        StoreLock lk;
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
        StoreLock lk;
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
        StoreLock lk;
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
        StoreLock lk;
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                RegisterBoxById(c, b.token);
            }
            SetTokenStats(b);  // token fields revert on load - re-apply
            ApplyBoxLabelToToken(b);  // inventory name too (rename feature)
        }
        // Persist actives are per-save: the co-save restore (which precedes this
        // kPostLoadGame pass) already re-registered them (M2, CEF_STATE_SCOPE.md §3).
        SKSE::log::info("settings: reapplied {} box(es)", g_boxes.size());
    }

    void ReloadSettingsFromDisk()
    {
        StoreLock lk;
        // Snapshot this save's persist actives BEFORE the registry is wiped -
        // they are co-save state, invisible to the settings JSON.
        const auto actives = ActivePersistIds();
        // The re-derive used to be HERE, before LoadBoxes, and that was right
        // while RebuildBoxAbility meant "drop the synthesized spell and mark it
        // stale" - dropping it early, refilling it from the reloaded file
        // afterwards (review 2026-09-11 F05). The pool changed what the call
        // means: it is RefreshContent now, which re-derives on the spot. Run
        // before the file is re-read it re-derives from the settings being
        // replaced, and nothing looks again afterwards, because SyncToActor
        // hands out the slot a content already has without asking what it is
        // worth. An item-data toggle flipped in the json went on being ignored
        // - F05 back again, put there by repointing the function and not
        // re-reading its callers. It is below, after LoadBoxes, now.
        for (const auto& it : ActiveSnapshot()) {
            DetachSkinned(it.id);
        }
        ClearRegistry();
        // Full clear + JSON re-read + box content re-register + token stats.
        // Its trailing carrier pass sees an empty active set (deregisters the
        // persist pool); the re-reconcile below re-registers - both requests
        // coalesce into ONE debounced head rebuild.
        LoadBoxes();
        Preset::MigrateAssignments();  // same pre-1.6.2.1 fixup the startup load runs
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
        // ClearRegistry above wiped EVERY actor, not just the player - published
        // costumes and NPC persist went with it, and nothing put them back
        // (review 2026-09-09 F09: the only caller of ReapplyNpcBindings was the
        // co-save load). Reachable without touching the settings file at all:
        // ReevaluateContentAdmissions runs this whenever a blacklist flag
        // changes, so an unrelated deny stripped the costume off an NPC standing
        // in the same cell until they unloaded. It re-registers publish bindings
        // AND NPC persist wear, and reconciles the actors it touches.
        ReapplyNpcBindings();
        Reconcile();
        // Re-derive from the RELOADED definitions, which is the whole point of
        // a reload: the per-content enchant, weight and armor toggles live in
        // that file. InvalidateStatAbilities covers boxes, persist AND
        // published costumes in one pass, so it replaces the separate
        // RebuildPersistAbility that used to sit here and never covered the
        // other two.
        InvalidateStatAbilities();
        ApplyBoxAbilities();
        ApplyCarrierOverrides(false);  // persist pool reconcile with actives back
        // X-MAN (test run 2026-07-26): the manifest MUST be re-emitted here, not
        // only inside the quarantine transaction. A hand-edited settings JSON +
        // this reload (the MCM/SMF "reload from disk" lever) changed the policy
        // and the admitted set while the manifest kept the OLD content list -
        // observed as a deny taking visual effect at 12:45:18 with the manifest
        // still stamped 12:42. Emitting it at the tail of the one function that
        // rebuilds all derived state keeps "one manifest write per operation"
        // (ReevaluateContentAdmissions no longer writes it a second time).
        WriteCarrierManifest();
        SKSE::log::info("settings: reloaded from disk (MCM) - {} box(es), {}/{} persist active restored",
            g_boxes.size(), restored, actives.size());
    }

    std::vector<std::string> PersistContents()
    {
        StoreLock lk;
        return g_persist;
    }

    bool AddPersistContent(const std::string& a_content)
    {
        StoreLock lk;
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
        // v1.3.2 capture gate (parity with AddBox): native / preset routes
        // reach here unchecked.
        {
            std::string why;
            if (!CanCaptureContent(content, &why)) {
                SKSE::log::warn("persist: rejects '{}' - {}", content, why);
                return false;
            }
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
        // Stage marker (persist-CTD investigation 2026-07-27). The log flushes per
        // line (logger.h), so on a CTD the LAST "persist-add[...]" line names the
        // stage that was running. This is the first one: the click was accepted,
        // on the caller's thread (Papyrus VM for the MCM, render for SMF) - so a
        // crash between here and [register] is the UI-thread half.
        SKSE::log::info("persist-add[catalog] '{}' accepted (catalog {} entries)",
            content, g_persist.size());
        return true;
    }

    bool RemovePersistContent(const std::string& a_content)
    {
        StoreLock lk;
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
        StoreLock lk;
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
        StoreLock lk;
        WriteCarrierManifest();
    }

    std::vector<std::string> PersistActiveIds()
    {
        StoreLock lk;
        return ActivePersistIds();
    }

    std::vector<std::string> DiagLines()
    {
        StoreLock lk;
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
            if (g_syncRunning.load()) {
                s += "RUNNING (" + SyncProgressBrief() + ")";
            } else if (sync == -999) {
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

        // --- Physics bones -------------------------------------------------
        // Two independent axes; see SkinRebind.h BoneBudgetInfo. The 80 is a
        // real constant (SSE's DX11 skinning constant buffer, per SHAPE, per
        // draw - what Bone Limit Extender lifts). The FSMP merge totals have no
        // citable ceiling, so they report asked-vs-granted instead.
        {
            const auto bb = BoneBudget();
            const bool ble = GetModuleHandleA("skyrimbonelimitfix.dll") != nullptr;
            out.push_back("# Physics bones");
            out.push_back(std::string("Bone Limit Extender: ") +
                          (ble ? "detected - the 80-bone limit is lifted"
                               : "NOT detected (Nexus 177636)"));
            // Axis (1): per-shape, against the citable 80.
            {
                std::string s = "Heaviest shape: " + std::to_string(bb.worstShapeBones) +
                                " / " + std::to_string(kVanillaShapeBoneLimit) +
                                " bone(s) per shape (vanilla GPU skinning buffer)";
                if (!bb.worstShapeContent.empty()) {
                    s += " - " + ItemDisplayName(bb.worstShapeContent);
                }
                out.push_back(s);
                if (bb.worstShapeBones > kVanillaShapeBoneLimit && !ble) {
                    out.push_back("  ^ OVER the vanilla limit with no Bone Limit Extender. "
                                  "Skyrim SE copies a shape's bones into an 80-bone DX11 "
                                  "buffer to skin it on the GPU, and crashes past that. "
                                  "Install Bone Limit Extender (Nexus 177636).");
                }
            }
            // Axis (2): FSMP merge, asked vs granted.
            out.push_back("CFW content needs: " + std::to_string(bb.askedBones) +
                          " custom bone(s) - physics " + std::to_string(bb.boundBones) +
                          " / static " + std::to_string(bb.staticBones));
            out.push_back("FSMP merged on player: " + std::to_string(bb.mergedTotal) +
                          " bone(s) in " + std::to_string(bb.mergeGroups) + " group(s); CFW's own " +
                          std::to_string(bb.mergedCef) + " in " + std::to_string(bb.cefMergeGroups));
            if (bb.askedBones > 0 && bb.mergedCef == 0) {
                out.push_back("  ^ NONE of CFW's carrier bones were merged - FSMP takes a "
                              "carrier whole or not at all, so something else on this actor "
                              "won and CFW costumes hang static. Measured against a popular "
                              "body-collision mod: disabling it took CFW from 0 merged bones "
                              "to 3453. Try disabling other SMP-heavy mods one at a time.");
            } else if (bb.staticBones > bb.boundBones) {
                out.push_back("  ^ most bones fell back to static (no SMP sway). Check "
                              "CEF_sync.log for 'skipped for the carrier' - content with no "
                              "inline HDT xml is never given physics.");
            }
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
        {
            const auto stats = BoxTokenPoolStats();
            out.push_back(std::format(
                "token pool: {} total ({} in boxes, {} reserved by publish, {} free); "
                "box definitions: {}",
                stats.total, stats.inBoxes, stats.reserved, stats.free, stats.definitions));
        }
        if (g_boxes.empty()) {
            out.push_back("(no boxes)");
        }
        for (const auto& b : g_boxes) {
            const int slot = SlotNumberOf(ResolveArmo(b.token));
            // The carrier key, not the slot: a slot can hold several boxes now,
            // and "box 55" alone would name three of them the same.
            const std::string key = tokenid::CarrierKeyFor(b.token, slot);
            std::string line = "box " + std::to_string(slot) + " [" +
                               (key.empty() ? b.token : key) + "]: " +
                               std::to_string(b.contents.size()) + " item(s)";
            if (!b.enabled) {
                line += ", disabled";
            }
            const std::uint32_t tf = ResolveFormId(b.token);
            if (player && tf && player->GetWornArmor(tf)) {
                line += ", WORN";
            }
            // Same pre-1.6.4 fallback as ApplyCarrierOverridesImpl, so the
            // report agrees with the carrier the token is actually pointed at.
            auto entry = key.empty() ? cj.end() : cj.find(key);
            if (entry == cj.end() && key == "Box" + std::to_string(slot)) {
                entry = cj.find(std::to_string(slot));
            }
            if (entry != cj.end() && entry->is_object()) {
                line += ", carrier r" + std::to_string(entry->value("rev", 0));
                const std::string file = entry->value("file", std::string{});
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
        // Custody at a glance; `cef store` prints the full listing.
        {
            out.push_back("# Hidden store");
            const auto lines = StoreDiagLines();
            out.push_back(lines.empty() ? "(unavailable)" : lines.front().substr(6));
            out.push_back("full listing: console `cef store`");
        }
        const auto npc = NpcDiagLines();
        out.insert(out.end(), npc.begin(), npc.end());
        out.push_back("# Churn (this session)");
        out.push_back(PersistDiagString());
        return out;
    }

    std::vector<int> HideSlotsFor(const std::string& a_id)
    {
        StoreLock lk;
        const auto it = g_hideRules.find(a_id);
        return it == g_hideRules.end() ? std::vector<int>{} : it->second;
    }

    namespace
    {
        // Shared refusal for the five APPEARANCE settings when a published
        // costume owns the content. Publish froze them: RegisterSnapshot renders
        // from the snapshot's own ContentSettings, and unpublish restores those
        // over anything set meanwhile - so accepting the edit would leave the UI
        // claiming a setting that does nothing and then silently reverts.
        // Item-data (stat) toggles are NOT gated here: publish reads those live
        // on every ability rebuild, so they are meant to apply while published.
        bool RefusedAsPublished(const char* a_what, const std::string& a_id,
            const std::string& a_holder)
        {
            const int slot = PublishSlotOfHolder(a_holder);
            if (slot < 0) {
                return false;
            }
            SKSE::log::warn(
                "{}: '{}' belongs to published costume #{} - its look is frozen at publish "
                "time; unpublish it first to change this",
                a_what, a_id, slot + 1);
            return true;
        }
    }

    bool SetHideSlots(const std::string& a_id, const std::vector<int>& a_slots)
    {
        StoreLock lk;
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
            const std::string holder = ContentHolder(id);
            if (holder.empty()) {  // ROOT E [1476]: no orphan side-map entries
                SKSE::log::warn("hide: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            if (RefusedAsPublished("hide", id, holder)) {
                return false;
            }
            g_hideRules[id] = std::move(clean);
        }
        WriteJson();
        return true;
    }

    int GenderModeFor(const std::string& a_id)
    {
        StoreLock lk;
        const auto it = g_genderModes.find(a_id);
        return it == g_genderModes.end() ? 0 : it->second;
    }

    bool SetGenderMode(const std::string& a_id, int a_mode)
    {
        StoreLock lk;
        if (a_id.empty()) {
            return false;
        }
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_mode == 1 || a_mode == 2) {
            const std::string holder = ContentHolder(id);
            if (holder.empty()) {  // ROOT E [1476]: no orphan side-map entries
                SKSE::log::warn("gender: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            if (RefusedAsPublished("gender", id, holder)) {
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
        StoreLock lk;
        return g_bodyMorphOn.contains(a_id);
    }

    bool SetBodyMorphOn(const std::string& a_id, bool a_on)
    {
        StoreLock lk;
        if (a_id.empty()) {
            return false;
        }
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_on) {
            const std::string holder = ContentHolder(id);
            if (holder.empty()) {  // ROOT E [1498]: no orphan bodyMorph entries
                SKSE::log::warn("morph: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            if (RefusedAsPublished("morph", id, holder)) {
                return false;
            }
            g_bodyMorphOn.insert(id);
        } else {
            g_bodyMorphOn.erase(id);
        }
        WriteJson();
        return true;
    }

    bool ShowRealBodyOn(const std::string& a_id)
    {
        StoreLock lk;
        return g_showRealBody.contains(a_id);
    }

    bool SetShowRealBodyOn(const std::string& a_id, bool a_on)
    {
        StoreLock lk;
        if (a_id.empty()) {
            return false;
        }
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_on) {
            const std::string holder = ContentHolder(id);
            if (holder.empty()) {  // ROOT E: no orphan entries
                SKSE::log::warn("realbody: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            if (RefusedAsPublished("realbody", id, holder)) {
                return false;
            }
            g_showRealBody.insert(id);
        } else {
            g_showRealBody.erase(id);
        }
        WriteJson();
        return true;
    }

    std::vector<std::string> HideShapesFor(const std::string& a_id)
    {
        StoreLock lk;
        const auto it = g_hideShapes.find(a_id);
        return it != g_hideShapes.end() ? it->second : std::vector<std::string>{};
    }

    bool IsHideShape(const std::string& a_id, const std::string& a_shape)
    {
        StoreLock lk;
        const auto it = g_hideShapes.find(a_id);
        return it != g_hideShapes.end() &&
               std::find(it->second.begin(), it->second.end(), a_shape) != it->second.end();
    }

    bool SetHideShape(const std::string& a_id, const std::string& a_shape, bool a_on)
    {
        StoreLock lk;
        if (a_id.empty() || a_shape.empty()) {
            return false;
        }
        std::string id = a_id;
        CanonicalizeColonId(id);  // ROOT D
        if (a_on) {
            const std::string holder = ContentHolder(id);
            if (holder.empty()) {  // ROOT E: no orphan hide-shape entries
                SKSE::log::warn("hideshape: '{}' is held by no box/persist - ignoring", id);
                return false;
            }
            if (RefusedAsPublished("hideshape", id, holder)) {
                return false;
            }
            auto& names = g_hideShapes[id];
            if (std::find(names.begin(), names.end(), a_shape) == names.end()) {
                names.push_back(a_shape);
            }
        } else {
            const auto it = g_hideShapes.find(id);
            if (it != g_hideShapes.end()) {
                auto& names = it->second;
                names.erase(std::remove(names.begin(), names.end(), a_shape), names.end());
                if (names.empty()) {
                    g_hideShapes.erase(it);
                }
            }
        }
        WriteJson();
        return true;
    }

    std::vector<std::pair<std::string, int>> ContentShapesFor(const std::string& a_id)
    {
        StoreLock lk;
        const auto it = g_contentShapes.find(a_id);
        return it != g_contentShapes.end() ? it->second
                                           : std::vector<std::pair<std::string, int>>{};
    }

    void SetContentShapes(const std::string& a_id,
        const std::vector<std::pair<std::string, int>>& a_shapes)
    {
        StoreLock lk;
        g_contentShapes[a_id] = a_shapes;
    }

    // --- Capture blacklist (v1.3.2, MARA_COMPAT_PLAN.md §3) ------------------

    namespace
    {
        // L3 opt-out keyword: any ARMO carrying it is barred from capture.
        // Other mod authors (or users, via the shipped CostumeFW_NoCapture_KID.ini
        // template) tag their utility armors with it - KID auto-creates the
        // keyword, so no ESP dependency in either direction. Static forms only;
        // runtime forms can't receive KID keywords (the hard layer blocks those).
        // Deny-list defaults + string matchers live in src/CapturePolicy.*.
        constexpr const char* kNoCaptureKeyword = "CEF_NoCapture";
    }

    std::shared_ptr<const policy::CapturePolicy> CapturePolicySnapshot()
    {
        StoreLock lk;
        return PolicySlot().load();
    }

    CaptureBlock CaptureBlockReason(RE::TESObjectARMO* a_armo,
        const policy::CapturePolicy& a_policy)
    {
        StoreLock lk;
        if (!a_armo) {
            return CaptureBlock::kDynamicForm;  // treat as never-capturable
        }
        // HARD layer, FIRST, and formID-only (review P1-4): IsDynamicForm()
        // reads nothing but the formID, so even a half-built foreign form is
        // safe to classify. A runtime form's deeper data (source files, name,
        // keywords, inventory entry) must never be read - MARA-class mods keep
        // half-built runtime armors in the inventory that crash third-party
        // UIs on touch (MARA bug #1059563 hover-CTD; CEF Nexus report
        // 2026-07-22), and GetLocalFormID() would null-deref on GetFile(0).
        // Not user-liftable: capture could never work anyway (a colon-id with
        // no plugin is unrestorable on the next load).
        if (a_armo->IsDynamicForm()) {
            return CaptureBlock::kDynamicForm;
        }
        // HARD: a static-range form with no defining file is equally
        // unrestorable and equally unsafe for GetLocalFormID().
        const auto* file = a_armo->GetFile(0);
        if (!file) {
            return CaptureBlock::kNoDefiningFile;
        }
        // L2a: source-plugin deny-list (defaults + user). Filename read only.
        if (policy::PluginDenied(a_policy, file->GetFilename())) {
            return CaptureBlock::kPlugin;
        }
        // L1 soft: non-playable armors are engine/framework internals (skins,
        // tokens, hosts); the vanilla inventory UI hides them, so a raw
        // GetInventory picker must hide them too. Record-flag read only.
        if ((a_armo->formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) != 0 &&
            !a_policy.allowNonPlayable) {
            return CaptureBlock::kNonPlayable;
        }
        // L3: CEF_NoCapture opt-out keyword (static keyword-array read).
        if (a_armo->HasKeywordString(kNoCaptureKeyword)) {
            return CaptureBlock::kKeyword;
        }
        // L2b: name deny-list (defaults + user). Only file-backed forms reach
        // this read (the hard layer returned above for everything else).
        if (policy::NameDenied(a_policy, a_armo->GetFullName() ? a_armo->GetFullName() : "")) {
            return CaptureBlock::kName;
        }
        // L2c: explicit colon-id deny-list (user; rare - names/plugins cover
        // the common cases). Built from the form only when the list is used;
        // MakeColonId is no-file-safe since r2 anyway.
        if (!a_policy.ids.empty() && policy::IdDenied(a_policy, MakeColonId(a_armo))) {
            return CaptureBlock::kId;
        }
        return CaptureBlock::kNone;
    }

    CaptureBlock CaptureBlockReason(RE::TESObjectARMO* a_armo)
    {
        StoreLock lk;
        const auto pol = CapturePolicySnapshot();
        return CaptureBlockReason(a_armo, *pol);
    }

    bool IsCaptureBlocked(RE::TESObjectARMO* a_armo)
    {
        StoreLock lk;
        return CaptureBlockReason(a_armo) != CaptureBlock::kNone;
    }

    namespace
    {
        const char* BlockReasonText(CaptureBlock a_reason)
        {
            switch (a_reason) {
            case CaptureBlock::kDynamicForm: return "runtime-created (dynamic) form";
            case CaptureBlock::kNoDefiningFile: return "form has no defining plugin file";
            case CaptureBlock::kNonPlayable: return "non-playable armor";
            case CaptureBlock::kPlugin: return "plugin on the deny-list";
            case CaptureBlock::kName: return "name on the deny-list";
            case CaptureBlock::kId: return "id on the deny-list";
            case CaptureBlock::kKeyword: return "carries the CEF_NoCapture keyword";
            default: return "not blocked";
            }
        }
    }

    bool IsContentAdmissible(const std::string& a_id,
        const policy::CapturePolicy& a_policy, std::string* a_why, bool a_log)
    {
        StoreLock lk;
        // Layered admission (review P1-3/P2-1), one policy snapshot for the
        // whole evaluation. Deliberately WITHOUT the resolvability check so
        // the registration boundary can use it on ids whose resolve failure
        // is already fail-soft (ROOT H unresolved-actives). a_log=false is
        // the quiet mode for derived processing (re-review P1-2) - refusals
        // there are expected steady state, not events worth a log line each.
        const auto refuse = [&](CaptureBlock a_reason) {
            if (a_log) {
                SKSE::log::warn("capture: '{}' blocked - {}", a_id, BlockReasonText(a_reason));
            }
            if (a_why) {
                *a_why = std::string("blocked: ") + BlockReasonText(a_reason) +
                         " (capture blacklist)";
            }
            return false;
        };
        // Layer 1: textual id deny - works even when nothing resolves.
        std::string canon = a_id;
        CanonicalizeColonId(canon);
        if (policy::IdDenied(a_policy, canon)) {
            return refuse(CaptureBlock::kId);
        }
        // Layer 2: the GENERIC form, if it resolves. This covers direct ARMA
        // content ids too (review P2-1): hard dynamic/no-file checks and the
        // plugin deny-list apply to any form kind; the ARMO-specific reasons
        // (non-playable / name / keyword) apply when it is an ARMO.
        if (const std::uint32_t fid = ResolveFormId(canon); fid != 0) {
            if (auto* form = RE::TESForm::LookupByID(fid)) {
                if (form->IsDynamicForm()) {
                    return refuse(CaptureBlock::kDynamicForm);
                }
                const auto* file = form->GetFile(0);
                if (!file) {
                    return refuse(CaptureBlock::kNoDefiningFile);
                }
                if (policy::PluginDenied(a_policy, file->GetFilename())) {
                    return refuse(CaptureBlock::kPlugin);
                }
                if (auto* armo = form->As<RE::TESObjectARMO>()) {
                    if (const auto reason = CaptureBlockReason(armo, a_policy);
                        reason != CaptureBlock::kNone) {
                        return refuse(reason);
                    }
                }
            }
        }
        return true;
    }

    bool IsContentAdmissible(const std::string& a_id, std::string* a_why, bool a_log)
    {
        StoreLock lk;
        const auto pol = CapturePolicySnapshot();
        return IsContentAdmissible(a_id, *pol, a_why, a_log);
    }

    std::vector<std::string> StatAdmittedContents(const std::vector<std::string>& a_ids)
    {
        StoreLock lk;
        // ONE policy generation for the whole derived-processing operation, the
        // same contract AdmittedContents follows - but without the model
        // resolution, which is race/sex dependent (see the header).
        const auto pol = CapturePolicySnapshot();
        std::vector<std::string> out;
        out.reserve(a_ids.size());
        for (const auto& id : a_ids) {
            if (IsContentAdmissible(id, *pol, nullptr, false)) {
                out.push_back(id);
            }
        }
        return out;
    }

    namespace
    {
        // M4-J: whether MARA.dll was present at kDataLoaded (plugin.cpp sets
        // it once, before any UI exists - effectively immutable afterwards).
        bool g_maraPresent = false;
    }

    void SetMaraPresent(bool a_present)
    {
        StoreLock lk;
        g_maraPresent = a_present;
    }

    bool CanCaptureContent(const std::string& a_id, std::string* a_why, bool a_physicalCapture)
    {
        StoreLock lk;
        // One policy generation covers the semantic base gate and the selected
        // ARMA/model gate. Unresolved content keeps the pre-1.3.2 UX message.
        const auto pol = CapturePolicySnapshot();
        if (!IsContentAdmissible(a_id, *pol, a_why)) {
            return false;
        }
        // M4-J guard (capture gate ONLY - registration, load of already-
        // captured contents and preset validation are unaffected): while MARA
        // is running, refuse to capture jewelry the player is WEARING. The
        // physical capture strips the worn item (CaptureItemToStore), and
        // MARA - which actively manages worn slot-35/36 jewelry - crashes in
        // its own bookkeeping when a managed item vanishes outside its
        // control (field-proven: crash-2026-07-25-13-03-03.log, all frames
        // MARA.dll; MARA cannot reliably survive even a regular unequip, its
        // bug #1058804, so refusing is the only safe move). Unworn jewelry
        // (inventory capture) is untouched by MARA and stays capturable.
        if (a_physicalCapture && g_maraPresent) {
            std::string canon = a_id;
            CanonicalizeColonId(canon);
            if (const std::uint32_t fid = ResolveFormId(canon); fid != 0) {
                auto* form = RE::TESForm::LookupByID(fid);
                auto* armo = form ? form->As<RE::TESObjectARMO>() : nullptr;
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (armo && player) {
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    // Single-bit HasPartOf calls (the .all() semantics make a
                    // combined mask mean BOTH slots - see the slot-gating
                    // lesson); ArmorJewelry covers keyword-tagged customs.
                    const bool jewelry = armo->HasKeywordString("ArmorJewelry") ||
                                         armo->HasPartOf(Slot::kAmulet) ||
                                         armo->HasPartOf(Slot::kRing);
                    if (jewelry && player->GetWornArmor(armo->GetFormID())) {
                        SKSE::log::warn(
                            "capture: '{}' refused - MARA is running and this jewelry is "
                            "worn (stripping it crashes MARA; unequip it first or capture "
                            "it from inventory)",
                            a_id);
                        if (a_why) {
                            *a_why = "MARA manages worn jewelry - unequip it first, or "
                                     "capture it from inventory";
                        }
                        return false;
                    }
                }
            }
        }
        std::string nif;
        if (!ResolveAdmittedModelPath(
                a_id, EffectiveSexFor(a_id), *pol, nif)) {
            if (a_why) {
                *a_why = "this item's mesh could not be resolved - not captured (see log)";
            }
            return false;
        }
        return true;
    }

    namespace
    {
        // r4 (re-review P1-2): policy mutation is a main-thread transaction.
        // Settings were already persisted WITHOUT a manifest. Use the
        // established reload primitive to detach/re-register configured
        // contents and preserve uncataloged M2 persist actives. Only after all
        // derived state is rebuilt do we emit one admitted-only manifest.
        void ReevaluateContentAdmissions()
        {
            // X-MAN: the manifest emit moved INTO ReloadSettingsFromDisk's tail
            // (same position in the sequence, but now every reload path gets it).
            // Writing it again here would double the per-operation manifest
            // updates that §F5 checks. The box-ability drop that used to sit
            // here moved for the same reason (F05) - it is at the head of
            // ReloadSettingsFromDisk now, so the direct reload lever gets it too.
            ReloadSettingsFromDisk();
        }
    }

    bool SetCaptureBlacklistFlag(const std::string& a_flag, bool a_on)
    {
        StoreLock lk;
        // Copy-and-publish (review P1-5): never mutate the published policy.
        // Note: no "allowDynamic" - the dynamic-form skip is a hard invariant.
        auto next = *CapturePolicySnapshot();
        bool* target = nullptr;
        if (a_flag == "allowNonPlayable") {
            target = &next.allowNonPlayable;
        } else if (a_flag == "disableDefaults") {
            target = &next.disableDefaults;
        }
        if (!target) {
            return false;
        }
        if (*target != a_on) {
            *target = a_on;
            PublishPolicy(std::move(next));
            WriteJson(false);  // r4: settings-only; manifest follows quarantine
            SKSE::log::info("capture: blacklist switch {} = {}", a_flag, a_on);
            ReevaluateContentAdmissions();  // r4: quarantine transaction
        }
        return true;
    }

    bool GetCaptureBlacklistFlag(const std::string& a_flag)
    {
        StoreLock lk;
        const auto pol = CapturePolicySnapshot();
        if (a_flag == "allowNonPlayable") {
            return pol->allowNonPlayable;
        }
        if (a_flag == "disableDefaults") {
            return pol->disableDefaults;
        }
        return false;
    }

    namespace
    {
        std::vector<std::string>* BlacklistBucket(policy::CapturePolicy& a_policy,
            const std::string& a_kind)
        {
            if (a_kind == "name") {
                return &a_policy.names;
            }
            if (a_kind == "plugin") {
                return &a_policy.plugins;
            }
            if (a_kind == "id") {
                return &a_policy.ids;
            }
            return nullptr;
        }

        std::string TrimCopy(const std::string& a_s)
        {
            const auto first = a_s.find_first_not_of(" \t");
            const auto last = a_s.find_last_not_of(" \t");
            return (first == std::string::npos) ? std::string{}
                                                : a_s.substr(first, last - first + 1);
        }
    }

    CaptureBlacklistView GetCaptureBlacklist()
    {
        StoreLock lk;
        const auto pol = CapturePolicySnapshot();
        CaptureBlacklistView view;
        view.defaultNames = policy::DefaultBlockNames();
        view.defaultPlugins = policy::DefaultBlockPlugins();
        view.names = pol->names;
        view.plugins = pol->plugins;
        view.ids = pol->ids;
        view.allowNonPlayable = pol->allowNonPlayable;
        view.disableDefaults = pol->disableDefaults;
        return view;
    }

    bool AddCaptureBlacklistEntry(const std::string& a_kind, const std::string& a_value)
    {
        StoreLock lk;
        // Copy-and-publish (review P1-5).
        auto next = *CapturePolicySnapshot();
        auto* bucket = BlacklistBucket(next, a_kind);
        std::string value = TrimCopy(a_value);
        if (!bucket || value.empty()) {
            return false;
        }
        if (a_kind == "id") {
            CanonicalizeColonId(value);
        }
        for (const auto& existing : *bucket) {
            if (policy::EqualsCI(existing, value)) {
                return false;  // duplicate
            }
        }
        bucket->push_back(value);
        PublishPolicy(std::move(next));
        WriteJson(false);  // r4: settings-only; manifest follows quarantine
        SKSE::log::info("capture: blacklist {} entry added '{}'", a_kind, value);
        ReevaluateContentAdmissions();  // r4: quarantine transaction
        return true;
    }

    bool RemoveCaptureBlacklistEntry(const std::string& a_kind, const std::string& a_value)
    {
        StoreLock lk;
        // Copy-and-publish (review P1-5).
        auto next = *CapturePolicySnapshot();
        auto* bucket = BlacklistBucket(next, a_kind);
        if (!bucket) {
            return false;
        }
        const auto it = std::find_if(bucket->begin(), bucket->end(),
            [&](const std::string& a_e) { return policy::EqualsCI(a_e, a_value); });
        if (it == bucket->end()) {
            return false;
        }
        bucket->erase(it);
        PublishPolicy(std::move(next));
        WriteJson(false);  // r4: settings-only; manifest follows quarantine
        SKSE::log::info("capture: blacklist {} entry removed '{}'", a_kind, a_value);
        ReevaluateContentAdmissions();  // r4: quarantine transaction
        return true;
    }

    std::vector<WornItem> WornArmors()
    {
        StoreLock lk;
        std::vector<WornItem> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }
        // Blocked forms are rejected INSIDE the GetInventory filter (review
        // P1-1): GetInventory copies each passing entry's InventoryEntryData
        // (including its extraLists) before returning, so a loop-side skip
        // would come AFTER the copy - the filter is the last point before a
        // hostile entry is touched (MARA #1059563; CEF report 2026-07-22).
        // One policy snapshot covers the whole enumeration (review P1-5).
        const auto pol = CapturePolicySnapshot();
        auto inv = player->GetInventory([&pol](RE::TESBoundObject& a_obj) {
            if (!a_obj.Is(RE::FormType::Armor)) {
                return false;
            }
            auto* armo = a_obj.As<RE::TESObjectARMO>();
            if (!armo || CaptureBlockReason(armo, *pol) != CaptureBlock::kNone) {
                return false;
            }
            // Exclude our own box tokens (base pool esp + the carrier patch).
            return !IsTokenPluginFile(armo->GetFile(0));
        });
        for (auto& [obj, data] : inv) {
            auto* armo = obj ? obj->As<RE::TESObjectARMO>() : nullptr;
            if (!armo) {
                continue;
            }
            const auto& [count, entry] = data;
            if (count <= 0 || !entry || !entry->IsWorn()) {
                continue;
            }
            const char* nm = armo->GetFullName();
            out.push_back({ (nm && *nm) ? EnsureUtf8(std::string(nm)) : MakeColonId(armo),
                MakeColonId(armo) });
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::vector<WornItem> InventoryArmors(const std::string& a_filter)
    {
        StoreLock lk;
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
        // Same filter-boundary contract as WornArmors (review P1-1): blocked
        // forms never have their InventoryEntryData copied.
        const auto pol = CapturePolicySnapshot();
        auto inv = player->GetInventory([&pol](RE::TESBoundObject& a_obj) {
            if (!a_obj.Is(RE::FormType::Armor)) {
                return false;
            }
            auto* armo = a_obj.As<RE::TESObjectARMO>();
            if (!armo || CaptureBlockReason(armo, *pol) != CaptureBlock::kNone) {
                return false;
            }
            // Exclude our own box tokens.
            return !IsTokenPluginFile(armo->GetFile(0));
        });
        for (auto& [obj, data] : inv) {
            auto* armo = obj ? obj->As<RE::TESObjectARMO>() : nullptr;
            if (!armo) {
                continue;
            }
            const auto& [count, entry] = data;
            if (count <= 0) {
                continue;
            }
            const char* nm = armo->GetFullName();
            std::string name = (nm && *nm) ? EnsureUtf8(std::string(nm)) : MakeColonId(armo);
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
        StoreLock lk;
        std::vector<std::pair<int, std::string>> pairs;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return {};
        }
        auto* marker = BoxTokenMarkerKeyword();
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo) {
                continue;
            }
            // GetFile(0) is the plugin that DEFINED the record, not the load-order
            // winner, so a patch that overrides a token still answers CostumeFW.esp
            // and a translated copy of the plugin changes nothing here.
            if (!tokenid::IsBoxTokenPlugin(FilenameOf(armo->GetFile(0)))) {
                continue;
            }
            // The marker, not the display name. A token's FULL is overwritten with
            // the box's label while it is in use, and an xTranslator'd copy of the
            // plugin translates it - so the old "name starts with Costume Box" test
            // dropped every renamed token out of the pool (F10) and would have
            // emptied the pool entirely for anyone running a translation.
            if (marker && !armo->HasKeyword(marker)) {
                continue;
            }
            pairs.emplace_back(SlotNumberOf(armo), MakeColonId(armo));
        }
        // (slot, colon-id): the slot orders the list for the pickers, and the
        // colon-id breaks ties. From 1.6.4 a slot can hold several tokens, so
        // without the tie-break the order would depend on form-array order.
        std::sort(pairs.begin(), pairs.end());
        std::vector<std::string> out;
        out.reserve(pairs.size());
        for (auto& p : pairs) {
            out.push_back(std::move(p.second));
        }
        return out;
    }

    std::vector<std::string> FreeTokens()
    {
        StoreLock lk;
        std::vector<std::string> out;
        for (const auto& t : TokenPool()) {
            if (FindBox(t) >= 0) {
                continue;
            }
            // Publishing a box does not RELEASE its token, it RESERVES it
            // (PLAN §5.3). Offering it here is what used to let a new box take
            // the slot a published costume came from, after which unpublish had
            // nowhere to put the costume back and landed it on whatever token
            // the allocator happened to reach - a different biped slot, hiding
            // different things (test run 2026-09-10).
            if (TokenReservedByPublish(t)) {
                continue;
            }
            out.push_back(t);
        }
        return out;
    }

    // Slots an AUTOMATIC pick must not reach for first. 30 (Head) and 31
    // (Hair/LongHair) hide the head and hair the moment the token is worn, so
    // handing one out unasked reads as "CEF deleted my head" (test run
    // 2026-09-10: a publish/unpublish slot collision landed a restored box on
    // Costume Box 30). The pool is sorted by slot number, so these WERE the
    // first two candidates for every automatic allocation.
    //
    // Deliberately a preference, not a ban: the user can still pick them in the
    // "+ New box" slot picker (that list stays in plain slot order), and an
    // automatic pick still takes one rather than fail when nothing else is free.
    static bool IsRiskyAutoSlot(int a_slot)
    {
        return a_slot == 30 || a_slot == 31;
    }

    std::string NextFreeToken()
    {
        StoreLock lk;
        const auto free = FreeTokens();
        if (free.empty()) {
            return {};
        }
        // How many boxes each biped slot already holds. Asked for every
        // candidate below, and a box's slot costs a form lookup, so once.
        std::unordered_map<int, int> boxesPerSlot;
        for (const auto& b : g_boxes) {
            ++boxesPerSlot[SlotNumberOf(ResolveArmo(b.token))];
        }
        // PLAN §5.2 / review A07. The pool is offered in slot order, so before
        // BoxPool1 the first free token WAS the lowest free slot. With four
        // tokens per slot that stopped being true: slot 32's three pool tokens
        // all sort ahead of slot 33's, and a run of NewBox calls would have put
        // every box on one biped slot - where they fight over the same
        // equipment slot and hide the same thing.
        //
        //   1. a slot no box is on yet          (spread out first)
        //   2. a slot that already has one      (generation ascending)
        //   3. head and hair                    (see IsRiskyAutoSlot)
        //
        // Within a tier: generation ascending, then local FormID, which is the
        // candidate order §5.2 defines and FreeTokenOnSlot also implements.
        std::vector<std::tuple<int, int, int, std::uint32_t, std::string>> ranked;
        for (const auto& token : free) {
            const int slot = TokenSlot(token);
            std::uint32_t local = 0;
            std::string plugin;
            if (!policy::ParseColonId(token, local, plugin)) {
                continue;
            }
            ranked.emplace_back(IsRiskyAutoSlot(slot) ? 1 : 0,
                boxesPerSlot[slot] > 0 ? 1 : 0, tokenid::BoxPoolGeneration(plugin), local, token);
        }
        if (ranked.empty()) {
            return free.front();  // nothing parsed: keep the old answer
        }
        std::sort(ranked.begin(), ranked.end());
        const auto& pick = ranked.front();
        if (std::get<0>(pick) == 1) {
            SKSE::log::warn(
                "boxes: only head/hair slots are free - auto-picking '{}' (slot {}); wearing it "
                "hides that body part",
                std::get<4>(pick), TokenSlot(std::get<4>(pick)));
        }
        return std::get<4>(pick);
    }

    int TokenSlot(const std::string& a_token)
    {
        StoreLock lk;
        return SlotNumberOf(ResolveArmo(a_token));
    }

    // What the current load order can say about a box's token. Cheap and
    // stateless: it reads the live data handler, so it re-derives correctly
    // after a plugin is added or removed without anything to invalidate.
    //
    // NOTE the ordering. The plugin question is asked BEFORE the form is
    // resolved, because "resolves to an ARMO" is not the same as "is a box
    // token": a publish token in CostumeFW_NPC.esp resolves to an ARMO perfectly
    // well, and letting it through is the whole defect this release exists to
    // close.
    std::uint32_t TokenSlotMask(const std::string& a_token)
    {
        StoreLock lk;
        auto* armo = ResolveArmo(a_token);
        return armo ? static_cast<std::uint32_t>(armo->GetSlotMask()) : 0u;
    }

    std::string CarrierKeyForToken(const std::string& a_token)
    {
        StoreLock lk;
        return tokenid::CarrierKeyFor(a_token, TokenSlot(a_token));
    }

    const RE::TESFile* LoadedFile(std::string_view a_name);  // fwd (defined below)

    // The master files a loaded plugin declares, in order. Empty when the
    // plugin is not loaded - which the caller has already established, so an
    // empty answer here means "no masters", the thing the ability pools are
    // supposed to have none of beyond Skyrim.esm.
    std::vector<std::string> MastersOf(std::string_view a_plugin)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return {};
        }
        const RE::TESFile* file = LoadedFile(a_plugin);
        if (!file) {
            return {};
        }
        // masterPtrs / masterCount rather than the `masters` name list: the
        // name list is a BSSimpleList whose const iterator does not compile
        // here, and the resolved pointers carry the same names in the same
        // order once the load order is built (which it is, at kDataLoaded).
        std::vector<std::string> out;
        for (std::uint32_t i = 0; file->masterPtrs && i < file->masterCount; ++i) {
            if (const auto* master = file->masterPtrs[i]) {
                out.emplace_back(master->GetFilename());
            }
        }
        return out;
    }

    // The loaded TESFile for a plugin name, or nullptr.
    //
    // NOT LookupLoadedModByName/LookupLoadedLightModByName, which is what this
    // used to be. Those take their loop bound from GetLoadedModCount() and
    // GetLoadedLightModCount(), and BOTH of those return a std::uint8_t. The
    // regular list tops out at 255 so its cast is harmless, but the light list
    // holds up to 4096 - so with 256+ ESLs installed the light count wraps and
    // the lookup scans only (count & 0xFF) of them, answering "not loaded" for
    // every light plugin past that point. Measured on the owner's load order:
    // 523 ESLs, 523 & 0xFF = 11 scanned, and CostumeFW.esp sits at light index
    // 426. CEF's own plugins are all ESL, so the whole store went inert (the
    // CoreMissing guard read "core is missing" and left every box unread) while
    // CostumeFW_Abilities.esp - the one full plugin - resolved fine.
    //
    // LookupModByName walks the data handler's file list, which is neither split
    // nor counted through a byte, and compileIndex == 0xFF is how a file that is
    // present but not loaded says so. This is the same pair CommonLibSSE itself
    // uses in TESDataHandler::LookupFormID, VR branch included, which is why
    // form resolution by colon-id kept working throughout.
    const RE::TESFile* LoadedFile(std::string_view a_name)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return nullptr;
        }
        const RE::TESFile* file = dh->LookupModByName(a_name);
        return (file && file->compileIndex != 0xFF) ? file : nullptr;
    }

    bool PluginIsLoaded(std::string_view a_name)
    {
        return LoadedFile(a_name) != nullptr;
    }

    TokenState ClassifyBoxToken(const std::string& a_colonId)
    {
        StoreLock lk;
        std::uint32_t local = 0;
        std::string plugin;
        if (!policy::ParseColonId(a_colonId, local, plugin)) {
            return TokenState::ParseError;
        }
        if (!tokenid::IsBoxTokenPlugin(plugin)) {
            return TokenState::ForeignPlugin;
        }
        if (!PluginIsLoaded(plugin)) {
            // A pool generation that is simply not installed right now. The box
            // is dormant, not broken: put the plugin back and it returns with
            // the same boxId, token and carrier.
            return TokenState::PoolMissing;
        }
        const std::uint32_t formId = ResolveFormId(a_colonId);
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        if (!form) {
            return TokenState::UnresolvedForm;
        }
        auto* armo = form->As<RE::TESObjectARMO>();
        if (!armo) {
            return TokenState::NotArmo;
        }
        // Condition 3 of the role test. LoadBoxes refuses to run at all when the
        // marker's own record is missing (CoreOutdated), so reaching here with a
        // null marker means a caller outside that path - treat "cannot check" as
        // "do not reject", the same way TokenPool does.
        if (auto* marker = BoxTokenMarkerKeyword(); marker && !armo->HasKeyword(marker)) {
            return TokenState::NoMarker;
        }
        return TokenState::Resolved;
    }

    std::string Gen0TokenForSlot(int a_slot)
    {
        StoreLock lk;
        if (a_slot < 30 || a_slot > 61) {
            return {};
        }
        for (const auto& token : TokenPool()) {
            std::uint32_t local = 0;
            std::string plugin;
            if (!policy::ParseColonId(token, local, plugin)) {
                continue;
            }
            if (policy::EqualsCI(plugin, tokenid::kCorePlugin) && TokenSlot(token) == a_slot) {
                return token;
            }
        }
        return {};
    }

    std::vector<std::string> TokenDiagLines()
    {
        StoreLock lk;
        std::vector<std::string> out;
        const auto pool = TokenPool();

        // Per generation. "not installed" is a normal answer for the ones above
        // what is here, and the interesting case is a generation that IS
        // installed but holds nothing.
        std::map<int, int> perGeneration;
        for (const auto& token : pool) {
            std::uint32_t local = 0;
            std::string plugin;
            if (policy::ParseColonId(token, local, plugin)) {
                ++perGeneration[policy::EqualsCI(plugin, tokenid::kCorePlugin)
                        ? 0 : tokenid::BoxPoolGeneration(plugin)];
            }
        }
        out.push_back(std::format("core legacy: {} resolved", perGeneration[0]));
        for (int generation = 1; generation <= 9; ++generation) {
            const auto plugin = tokenid::BoxPoolPluginName(generation);
            if (PluginIsLoaded(plugin)) {
                out.push_back(std::format("BoxPool{}: {} resolved", generation,
                    perGeneration[generation]));
            } else if (generation == 1 || perGeneration.contains(generation)) {
                out.push_back(std::format("BoxPool{}: not installed", generation));
            }
        }

        // Per biped slot. Several tokens on one slot is the NORMAL state from
        // v1.6.4 and is not called out as a problem.
        std::map<int, std::array<int, 4>> perSlot;  // slot -> {total, active, reserved, free}
        int dormant = 0;
        for (const auto& token : pool) {
            const int slot = TokenSlot(token);
            auto& row = perSlot[slot];
            ++row[0];
            if (FindBox(token) >= 0) {
                ++row[1];
            } else if (TokenReservedByPublish(token)) {
                ++row[2];
            } else {
                ++row[3];
            }
        }
        for (const auto& b : g_boxes) {
            dormant += b.tokenState != TokenState::Resolved ? 1 : 0;
        }
        for (const auto& [slot, row] : perSlot) {
            out.push_back(std::format("slot {}: total {} / active {} / reserved {} / free {}", slot,
                row[0], row[1], row[2], row[3]));
        }

        out.push_back(std::format("non-pool CEF tokens: publish 8, npc-persist 8 ({}){}",
            tokenid::kNpcPlugin, NpcEspLoaded() ? "" : " - NOT LOADED, so 0 of each"));
        int invalid = 0;
        for (const auto& b : g_boxes) {
            invalid += (b.tokenState != TokenState::Resolved &&
                        b.tokenState != TokenState::PoolMissing) ? 1 : 0;
        }
        out.push_back(std::format("box definitions: {} (resolved {} / dormant {} / invalid {})",
            g_boxes.size(), g_boxes.size() - dormant, dormant - invalid, invalid));

        // One row per token. `now` is what the inventory shows; `pre-CEF` is the
        // name before a box label was stamped on it - NOT "the ESP default",
        // because a translated copy of the plugin is the load-order winner and
        // its name is what CEF saw first.
        out.push_back("token / carrierKey / slot / state / boxId / gen / now / pre-CEF");
        for (const auto& token : pool) {
            auto* armo = ResolveArmo(token);
            const int slot = SlotNumberOf(armo);
            std::uint32_t local = 0;
            std::string plugin;
            // Cannot fail - TokenPool built every one of these ids itself, with
            // MakeColonId. Discarded explicitly rather than silently ([[nodiscard]]).
            (void)policy::ParseColonId(token, local, plugin);
            const int generation = policy::EqualsCI(plugin, tokenid::kCorePlugin)
                ? 0 : tokenid::BoxPoolGeneration(plugin);
            const int box = FindBox(token);
            const char* state = box >= 0 ? "active"
                : (TokenReservedByPublish(token) ? "reserved" : "free");
            const std::string boxId = box >= 0 ? g_boxes[static_cast<std::size_t>(box)].boxId : "-";
            const char* nowName = armo ? armo->GetFullName() : nullptr;
            std::string pre = nowName ? nowName : "?";
            if (armo) {
                if (const auto it = g_tokenDefaultNames.find(armo->GetFormID());
                    it != g_tokenDefaultNames.end() && !it->second.empty()) {
                    pre = it->second;
                }
            }
            out.push_back(std::format("  {} / {} / {} / {} / {} / gen{} / {} / {}", token,
                tokenid::CarrierKeyFor(token, slot), slot, state, boxId, generation,
                nowName ? nowName : "?", pre));
        }
        return out;
    }

    void AuditTokenPools()
    {
        StoreLock lk;
        int errors = 0;
        int warnings = 0;
        const auto err = [&](const std::string& a_line) {
            ++errors;
            SKSE::log::error("audit: {}", a_line);
        };
        const auto warn = [&](const std::string& a_line) {
            ++warnings;
            SKSE::log::warn("audit: {}", a_line);
        };
        SKSE::log::info("audit: reading the box token pools");

        // 1. The predicates, before anything that rests on them. An allowlist
        //    edited in the wrong direction is invisible in normal play and
        //    turns a publish token into a box token, which is the defect this
        //    whole release exists to close (old plan C4 / M05).
        for (const auto name : { tokenid::kNpcPlugin, tokenid::kAbilityGen1Plugin }) {
            if (tokenid::IsBoxTokenPlugin(name)) {
                err(std::format("IsBoxTokenPlugin('{}') is TRUE - that plugin does NOT define box "
                                "tokens, and treating its records as box tokens is how a published "
                                "costume's token becomes a box", name));
            }
            if (!tokenid::IsCefPlugin(name)) {
                err(std::format("IsCefPlugin('{}') is FALSE - the broad test has been narrowed, "
                                "and CEF's own records can now be captured as box CONTENT", name));
            }
        }
        if (!tokenid::IsBoxTokenPlugin(tokenid::kCorePlugin) ||
            !tokenid::IsBoxTokenPlugin(tokenid::BoxPoolPluginName(1))) {
            err("IsBoxTokenPlugin is FALSE for the core plugin or BoxPool1 - no token can be "
                "recognised at all");
        }

        // 2. Which generations are here, and is the run unbroken? A generation
        //    above a gap is not used (PLAN §4.2), so the gap has to be named
        //    rather than left as "my new pool did nothing".
        int highestInstalled = 0;
        std::vector<int> installed;
        for (int generation = 1; generation <= 99; ++generation) {
            const auto plugin = tokenid::BoxPoolPluginName(generation);
            if (plugin.empty() || !PluginIsLoaded(plugin)) {
                continue;
            }
            installed.push_back(generation);
            highestInstalled = generation;
        }
        for (int generation = 1; generation <= highestInstalled; ++generation) {
            if (std::find(installed.begin(), installed.end(), generation) == installed.end()) {
                err(std::format("{} is loaded but {} is missing - a generation cannot be skipped, "
                                "so nothing is allocated from the later one and any box on it is "
                                "dormant",
                    tokenid::BoxPoolPluginName(highestInstalled),
                    tokenid::BoxPoolPluginName(generation)));
            }
        }

        // 3. Masters, per plugin. BoxPoolN = Skyrim.esm + the core plugin, and
        //    nothing else: a stray master is a plugin the user can remove and
        //    take the whole pool down with (PLAN §1.3).
        const auto checkMasters = [&](std::string_view a_plugin,
                                      const std::vector<std::string>& a_want) {
            const auto have = MastersOf(a_plugin);
            if (have.size() != a_want.size()) {
                err(std::format("{} has {} master(s), expected {}", a_plugin, have.size(),
                    a_want.size()));
                return;
            }
            for (std::size_t i = 0; i < have.size(); ++i) {
                if (!policy::EqualsCI(have[i], a_want[i])) {
                    err(std::format("{} master {} is '{}', expected '{}'", a_plugin, i, have[i],
                        a_want[i]));
                }
            }
        };
        if (PluginIsLoaded(tokenid::kCorePlugin)) {
            checkMasters(tokenid::kCorePlugin, { "Skyrim.esm" });
        }
        for (const int generation : installed) {
            checkMasters(tokenid::BoxPoolPluginName(generation),
                { "Skyrim.esm", std::string(tokenid::kCorePlugin) });
        }

        // 4. The tokens themselves. Every pool token is checked against the
        //    generation-0 token on its slot - the BOD2 mask is inherited, and
        //    slot 31's is 31|41 rather than one bit.
        std::unordered_map<int, std::uint32_t> gen0Mask;
        std::unordered_map<std::string, std::string> byCarrierKey;  // key -> token
        int gen0Count = 0;
        std::unordered_map<int, int> perGeneration;
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* marker = BoxTokenMarkerKeyword();
        const auto pool = TokenPool();
        // The whole generation-0 map BEFORE any pool token is judged against it.
        // Filling it in the same pass made the check depend on the pool's order,
        // and TokenPool sorts by (slot, colon-id) - so within a slot the local id
        // decides, and generation 0 holds the LOW slots under its HIGH ids
        // (slot 34 is 000A07, 37 is 000A0D, 38 is 000A0F) while BoxPool1 numbers
        // those same slots from 000800 up. Every pool token on slots 30-43 was
        // therefore read before its generation-0 counterpart existed in the map
        // and reported as sitting on a slot "which no generation-0 token
        // occupies" - 36 of 81 tokens, all of them false, including the slot
        // 31|41 wig mask this check exists to protect.
        for (const auto& token : pool) {
            std::uint32_t local = 0;
            std::string plugin;
            if (!policy::ParseColonId(token, local, plugin) ||
                !policy::EqualsCI(plugin, tokenid::kCorePlugin)) {
                continue;
            }
            if (auto* armo = ResolveArmo(token)) {
                gen0Mask[SlotNumberOf(armo)] = static_cast<std::uint32_t>(armo->GetSlotMask());
            }
        }
        for (const auto& token : pool) {
            std::uint32_t local = 0;
            std::string plugin;
            if (!policy::ParseColonId(token, local, plugin)) {
                err(std::format("token '{}' does not parse", token));
                continue;
            }
            const int generation = policy::EqualsCI(plugin, tokenid::kCorePlugin) ?
                0 : tokenid::BoxPoolGeneration(plugin);
            ++perGeneration[generation];
            gen0Count += generation == 0 ? 1 : 0;

            auto* armo = ResolveArmo(token);
            if (!armo) {
                err(std::format("token {} is in the pool but does not resolve", token));
                continue;
            }
            if (marker && !armo->HasKeyword(marker)) {
                err(std::format("token {} has no {}", token, kBoxTokenMarkerEdid));
                continue;
            }
            const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
            const int slot = SlotNumberOf(armo);
            if (mask == 0) {
                err(std::format("token {} has an empty BOD2 (mask {:08X})", token, mask));
            } else if (slot < 30 || slot > 61) {
                err(std::format("token {} sits on biped slot {} (mask {:08X}), outside 30-61",
                    token, slot, mask));
            }
            if (armo->armorAddons.size() != 1) {
                err(std::format("token {} references {} ARMA (want exactly 1, mask {:08X})", token,
                    armo->armorAddons.size(), mask));
            } else if (auto* addon = armo->armorAddons.front(); addon) {
                const auto addonMask =
                    static_cast<std::uint32_t>(addon->bipedModelData.bipedObjectSlots.underlying());
                if (addonMask != mask) {
                    err(std::format("token {} BOD2 {:08X} does not match its ARMA's {:08X}", token,
                        mask, addonMask));
                }
            }
            if (generation == 0) {
                // Already in gen0Mask - the pre-pass above put it there.
            } else if (const auto it = gen0Mask.find(slot); it == gen0Mask.end()) {
                err(std::format("token {} is on biped slot {} (mask {:08X}), which no generation-0 "
                                "token occupies", token, slot, mask));
            } else if (it->second != mask) {
                err(std::format("token {} BOD2 {:08X} differs from generation 0's {:08X} for biped "
                                "slot {}", token, mask, it->second, slot));
            }

            const auto carrierKey = tokenid::CarrierKeyFor(token, slot);
            if (carrierKey.empty()) {
                err(std::format("token {} has no carrier key (mask {:08X})", token, mask));
            } else if (const auto [it, fresh] = byCarrierKey.emplace(carrierKey, token); !fresh) {
                err(std::format("tokens {} and {} both claim the carrier key {} - they would "
                                "overwrite each other's meshes", it->second, token, carrierKey));
            }

            // The tuple, not the bare local id (gaps P6). Every pool plugin and
            // CostumeFW_NPC.esp number their records from 0x800, so comparing
            // local ids alone reports a collision between every token and a
            // publish token that is simply a different record in another file.
            if (dh && armo->GetFile(0) &&
                policy::EqualsCI(FilenameOf(armo->GetFile(0)), tokenid::kNpcPlugin)) {
                err(std::format("token {} is DEFINED by {} - a publish or NPC-persist token has "
                                "been admitted to the box pool", token, tokenid::kNpcPlugin));
            }
        }

        // The other direction of the same question.
        if (NpcEspLoaded()) {
            for (int slot = 0; slot < 8; ++slot) {
                for (auto* armo : { PubTokenArmo(slot), NprTokenArmo(slot) }) {
                    if (!armo || !armo->GetFile(0)) {
                        continue;
                    }
                    if (tokenid::IsBoxTokenPlugin(FilenameOf(armo->GetFile(0)))) {
                        err(std::format("{} is treated as a box token plugin - its publish and "
                                        "NPC-persist tokens would be handed out as boxes",
                            FilenameOf(armo->GetFile(0))));
                    }
                }
            }
        }

        // 5. Counts. Generation 0 is a fixed 27 and a drift there means the core
        //    plugin is not the one that shipped; a pool generation's size is
        //    whatever it holds, so it is reported rather than judged.
        constexpr int kGen0TokenCount = 27;
        if (gen0Count != kGen0TokenCount) {
            warn(std::format("generation 0 holds {} token(s), expected {} - is {} the one that "
                             "shipped with this version?",
                gen0Count, kGen0TokenCount, tokenid::kCorePlugin));
        }
        for (const auto& [generation, count] : std::map<int, int>(perGeneration.begin(),
                 perGeneration.end())) {
            SKSE::log::info("audit: {} - {} token(s)",
                generation == 0 ? std::string(tokenid::kCorePlugin)
                                : tokenid::BoxPoolPluginName(generation),
                count);
        }

        // 6. Ownership. A box holding a token that a published costume also
        //    names is legal (PLAN §5.1 rule 3 - the box wins) but it is also the
        //    one state where unpublish refuses, so it is worth a line before the
        //    user finds out by clicking.
        for (const auto& b : g_boxes) {
            if (TokenReservedByPublish(b.token) && !b.token.empty()) {
                warn(std::format("box '{}' holds {}, which a published costume also names - that "
                                 "costume cannot be unpublished until this box frees it",
                    b.label.empty() ? b.boxId : b.label, b.token));
            }
            if (b.tokenState != TokenState::Resolved) {
                warn(std::format("box '{}' token {} is {} - the definition is kept and quarantined",
                    b.label.empty() ? b.boxId : b.label, b.token,
                    TokenStateReason(b.tokenState)));
            }
        }

        const auto stats = BoxTokenPoolStats();
        SKSE::log::info("audit: {} token(s) total, {} in boxes, {} reserved by publish, {} free; "
                        "{} box definition(s)",
            stats.total, stats.inBoxes, stats.reserved, stats.free, stats.definitions);
        SKSE::log::info("audit: enchantment passthrough {}",
            abilities::Ready() ? "ready" : ("OFF - " + abilities::DisabledReason()));
        if (errors == 0 && warnings == 0) {
            SKSE::log::info("audit: no problems found");
        } else {
            SKSE::log::warn("audit: {} error(s), {} warning(s) - see the lines above", errors,
                warnings);
        }
    }

    TokenPoolStats BoxTokenPoolStats()
    {
        StoreLock lk;
        TokenPoolStats out;
        out.definitions = static_cast<int>(g_boxes.size());
        for (const auto& token : TokenPool()) {
            ++out.total;
            if (FindBox(token) >= 0) {
                ++out.inBoxes;
            } else if (TokenReservedByPublish(token)) {
                ++out.reserved;
            } else {
                ++out.free;
            }
        }
        return out;
    }

    std::string FreeTokenOnSlot(int a_slot)
    {
        StoreLock lk;
        if (a_slot < 30 || a_slot > 61) {
            return {};
        }
        // (generation, local id) rather than the pool's own (slot, colon-id)
        // order: the colon-id sorts on the local id FIRST, so with two pool
        // generations installed it would offer BoxPool2's 0x800 ahead of
        // BoxPool1's 0x801 and the generations would interleave.
        std::vector<std::pair<std::pair<int, std::uint32_t>, std::string>> ranked;
        for (const auto& token : FreeTokens()) {
            if (TokenSlot(token) != a_slot) {
                continue;
            }
            std::uint32_t local = 0;
            std::string plugin;
            if (!policy::ParseColonId(token, local, plugin)) {
                continue;
            }
            // Generation 0 is the core plugin, whose generation number is 0 -
            // so it sorts first without a special case.
            ranked.push_back({ { tokenid::BoxPoolGeneration(plugin), local }, token });
        }
        if (ranked.empty()) {
            return {};
        }
        std::sort(ranked.begin(), ranked.end());
        return ranked.front().second;
    }

    int BoxesOnSlot(int a_slot)
    {
        StoreLock lk;
        int n = 0;
        for (const auto& b : g_boxes) {
            if (SlotNumberOf(ResolveArmo(b.token)) == a_slot) {
                ++n;
            }
        }
        return n;
    }

    // I1 (v1.6.4 MCM interlock). A biped slot stopped naming one box the moment
    // BoxPool1 existed, so "the box on slot 55" is a question with no answer when
    // that slot holds two. This used to return the FIRST match, which is how the
    // MCM - whose pages are keyed by slot - would have deleted, renamed or
    // re-packed the wrong box without a word.
    //
    // -1 for an ambiguous slot rather than a guess. The MCM already handles a
    // negative index gracefully (OnPageReset falls through to ResetDeletedBoxPage,
    // a page with no controls on it), so refusing to answer is enough to make the
    // damage impossible. Callers that want every box on a slot ask BoxesOnSlot /
    // BoxesForSlot instead.
    //
    // This is the interlock, NOT support: operating a shared slot from the MCM is
    // 1.6.4.1, which keys its pages by boxId. The guard goes when the MCM does
    // (1.6.5), together with this function.
    int BoxIndexForSlot(int a_slot)
    {
        StoreLock lk;
        int found = -1;
        for (std::size_t i = 0; i < g_boxes.size(); ++i) {
            if (SlotNumberOf(ResolveArmo(g_boxes[i].token)) != a_slot) {
                continue;
            }
            if (found >= 0) {
                return -1;  // ambiguous: two or more boxes share this slot
            }
            found = static_cast<int>(i);
        }
        return found;
    }

    std::string LoreBoxContentsForCarrierKey(const std::string& a_carrierKey)
    {
        StoreLock lk;
        if (a_carrierKey.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            auto* armo = ResolveArmo(b.token);
            if (!armo) {
                continue;
            }
            if (!policy::EqualsCI(tokenid::CarrierKeyFor(b.token, SlotNumberOf(armo)),
                    a_carrierKey)) {
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
        return {};  // no box holds that token
    }

    bool BoxEnabled(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx >= 0 && g_boxes[idx].enabled;
    }

    // The flag, the json and the token's stats - without converging the pool.
    // Split out so the convergence pass itself can flip the flag (below) without
    // calling back into the pass that is already running.
    bool SetBoxEnabledNoSync(const std::string& a_token, bool a_enabled)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].enabled = a_enabled;
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // disabled -> token stats cleared, enabled -> applied
        return true;
    }

    bool SetBoxEnabled(const std::string& a_token, bool a_enabled)
    {
        if (!SetBoxEnabledNoSync(a_token, a_enabled)) {
            return false;
        }
        // This switch decides whether a worn box pays its enchantments through,
        // so flipping it CHANGES what should be granted right now. It used to
        // write the flag and stop: with the token already on, nothing re-ran the
        // convergence, and the box went on paying nothing until the next equip
        // event happened to trigger one. Measured 2026-09-14: five minutes
        // between ticking the box back on (01:28:46) and the ability actually
        // landing, which took re-equipping the token (01:33:59).
        ApplyBoxAbilities();
        return true;
    }

    int BoxArmorType(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx < 0 ? 0 : g_boxes[idx].armorType;
    }

    bool SetBoxArmorType(const std::string& a_token, int a_type)
    {
        StoreLock lk;
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
        StoreLock lk;
        const std::string token = NextFreeToken();
        if (token.empty()) {
            SKSE::log::warn("boxes: NewBox - token pool exhausted");
            return false;
        }
        return AddBox(a_label, token, {});  // definition only
    }

    namespace
    {
        // Strict UTF-8 validity scan (no decode, no allocation).
        bool IsValidUtf8(std::string_view a_s)
        {
            std::size_t i = 0;
            const auto n = a_s.size();
            while (i < n) {
                const auto c = static_cast<unsigned char>(a_s[i]);
                std::size_t need = 0;
                if (c < 0x80) {
                    ++i;
                    continue;
                } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
                    need = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    need = 2;
                } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
                    need = 3;
                } else {
                    return false;
                }
                if (i + need >= n + 1 && need > n - i - 1) {
                    return false;
                }
                for (std::size_t k = 1; k <= need; ++k) {
                    if (i + k >= n ||
                        (static_cast<unsigned char>(a_s[i + k]) & 0xC0) != 0x80) {
                        return false;
                    }
                }
                i += need + 1;
            }
            return true;
        }
    }

    std::string EnsureUtf8(const std::string& a_text)
    {
        // ImGui expects UTF-8; a byte sequence it cannot decode renders as one
        // '?' per bad byte. Modern JP/CN/KR plugins ship UTF-8 strings, but
        // legacy ESPs carry raw ANSI-codepage bytes (cp932/cp936/cp1252...).
        // If the text is not valid UTF-8, reinterpret it in the SYSTEM codepage
        // - on the machine playing a cp932 mod that is almost always cp932 -
        // and convert. Valid UTF-8 (the common case) passes through untouched.
        if (a_text.empty() || IsValidUtf8(a_text)) {
            return a_text;
        }
        const int wlen = ::MultiByteToWideChar(CP_ACP, 0, a_text.data(),
            static_cast<int>(a_text.size()), nullptr, 0);
        if (wlen <= 0) {
            return a_text;
        }
        std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, a_text.data(),
            static_cast<int>(a_text.size()), wide.data(), wlen);
        const int ulen = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen,
            nullptr, 0, nullptr, nullptr);
        if (ulen <= 0) {
            return a_text;
        }
        std::string out(static_cast<std::size_t>(ulen), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, out.data(), ulen,
            nullptr, nullptr);
        return out;
    }

    std::string ItemDisplayName(const std::string& a_colonId)
    {
        StoreLock lk;
        const std::uint32_t formId = ResolveFormId(a_colonId);
        if (formId == 0) {
            return a_colonId;
        }
        auto* form = RE::TESForm::LookupByID(formId);
        auto* full = form ? form->As<RE::TESFullName>() : nullptr;
        const char* nm = full ? full->GetFullName() : nullptr;
        return (nm && *nm) ? EnsureUtf8(std::string(nm)) : a_colonId;
    }

    namespace
    {
        // "Lisa (00000014)" - enough to tell the player from an NPC, and two NPCs
        // of the same name apart, in a log line.
        std::string ActorLabel(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return "(null)";
            }
            const char* nm = a_actor->GetName();
            const std::string name = (nm && *nm) ? EnsureUtf8(std::string(nm)) : std::string("?");
            return std::format("{} ({:08X})", name, a_actor->GetFormID());
        }
    }

    bool GrantAbility(RE::Actor* a_actor, RE::SpellItem* a_spell, std::string_view a_key)
    {
        if (!a_actor || !a_spell) {
            return false;
        }
        if (a_actor->HasSpell(a_spell)) {
            return true;  // already granted - not a second AddSpell
        }
        const bool ok = a_actor->AddSpell(a_spell);
        if (ok) {
            SKSE::log::info("ability: granted '{}' to {} ({} effect(s), form {:08X})", a_key,
                ActorLabel(a_actor), a_spell->effects.size(), a_spell->GetFormID());
        } else {
            SKSE::log::warn("ability: AddSpell REFUSED '{}' for {} - nothing was granted", a_key,
                ActorLabel(a_actor));
        }
        return ok;
    }

    bool RevokeAbility(RE::Actor* a_actor, RE::SpellItem* a_spell, std::string_view a_key)
    {
        if (!a_actor || !a_spell) {
            return false;
        }
        if (!a_actor->HasSpell(a_spell)) {
            return true;  // nothing to take off
        }
        const bool ok = a_actor->RemoveSpell(a_spell);
        const bool gone = !a_actor->HasSpell(a_spell);
        if (ok && gone) {
            SKSE::log::info("ability: removed '{}' from {}", a_key, ActorLabel(a_actor));
        } else {
            // The shape that strands an actor-value modifier: we believe the
            // ability is off, nobody re-runs the removal, and the fortify it
            // applied has nothing left to take it back off.
            SKSE::log::warn(
                "ability: RemoveSpell did NOT take '{}' off {} (returned {}, still held {}) - "
                "its actor-value modifier may be stranded",
                a_key, ActorLabel(a_actor), ok, !gone);
        }
        return ok && gone;
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






        // Whether N copies of this effect add up, the way N real pieces carrying
        // it would. Value-modifier archetypes do. For anything else - waterbreathing,
        // invisibility, a script effect - magnitude either is not an amount or does
        // not compose, so the group keeps the largest and says so. That is what the
        // engine's collapse already gave us, so it cannot be a regression, and the
        // log line is what would justify promoting an archetype later.
        // What to call an MGEF in a fold line: its display name if it has one,
        // else its colon-id. Both are useful - the name is what the user sees in
        // the active-effect list, the id is what a bug report can be traced with.
        std::string MgefLabel(RE::EffectSetting* a_mgef)
        {
            if (!a_mgef) {
                return "(null)";
            }
            const char* nm = a_mgef->GetFullName();
            const std::string id = MakeColonId(a_mgef);
            return (nm && *nm) ? EnsureUtf8(std::string(nm)) + " [" + id + "]" : id;
        }


        // True when a flat capture snapshot is just a copy of the base
        // enchantment - same MGEFs, same magnitudes - rather than a player
        // enchantment that cannot be re-derived from the form. CaptureEnchant
        // reads the EFFECTIVE enchantment, so an unenchanted-by-the-player item
        // snapshots its base; that snapshot then shadows the live form even
        // though the live form is the same thing WITH its conditions.
        bool SnapshotMatchesEnchant(const std::vector<EnchEffect>& a_snap,
            const RE::EnchantmentItem* a_ench)
        {
            if (!a_ench) {
                return false;
            }
            std::size_t live = 0;
            for (const auto* e : a_ench->effects) {
                if (e && e->baseEffect) {
                    ++live;
                }
            }
            if (live == 0 || live != a_snap.size()) {
                return false;
            }
            std::vector<bool> used(a_snap.size(), false);
            for (const auto* e : a_ench->effects) {
                if (!e || !e->baseEffect) {
                    continue;
                }
                const std::string id = MakeColonId(e->baseEffect);
                bool found = false;
                for (std::size_t i = 0; i < a_snap.size(); ++i) {
                    const float d = a_snap[i].magnitude - e->effectItem.magnitude;
                    if (!used[i] && a_snap[i].mgef == id && d < 0.01f && d > -0.01f) {
                        used[i] = true;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return false;
                }
            }
            return true;
        }

        // Whether the hidden store still holds a content's captured original,
        // and that original's player/instance enchantment if it carries one.
        // The live form has the full Effect data (conditions, durations) the
        // flat snapshot cannot represent.
        struct StoredEnchantLookup
        {
            bool inStore{ false };
            RE::EnchantmentItem* instance{ nullptr };
        };

        StoredEnchantLookup FindStoredEnchant(const std::string& a_id)
        {
            StoredEnchantLookup out;
            const std::uint32_t storeId = StoreFormId();
            const std::uint32_t baseId = ResolveFormId(a_id);
            if (!storeId || !baseId) {
                return out;
            }
            auto* form = RE::TESForm::LookupByID(storeId);
            auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!store) {
                return out;
            }
            auto inv = store->GetInventory([baseId](RE::TESBoundObject& a_obj) {
                return a_obj.GetFormID() == baseId;
            });
            for (auto& [obj, data] : inv) {
                const auto& [count, entry] = data;
                if (count <= 0 || !entry) {
                    continue;
                }
                out.inStore = true;
                if (!entry->extraLists) {
                    continue;
                }
                for (auto* x : *entry->extraLists) {
                    if (!x) {
                        continue;
                    }
                    if (const auto* xe = x->GetByType<RE::ExtraEnchantment>();
                        xe && xe->enchantment) {
                        out.instance = xe->enchantment;
                        return out;
                    }
                }
            }
            return out;
        }

        RE::TESObjectARMO* ResolveArmo(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            return form ? form->As<RE::TESObjectARMO>() : nullptr;
        }

        // The defining plugin's file name, or "" when a form has none (a runtime
        // form). string_view over TESFile's own buffer; valid for the call.
        std::string_view FilenameOf(const RE::TESFile* a_file)
        {
            return a_file ? a_file->GetFilename() : std::string_view{};
        }

        // CFW_BoxTokenMarker, resolved once per process. Null means the DLL is
        // 1.6.4 but CostumeFW.esp is still the 1.6.3 file (CoreOutdated): the
        // keyword's own record does not exist, so no token can carry it and
        // testing for it would empty the pool. Callers treat null as "cannot
        // check" rather than "nothing qualifies".
        RE::BGSKeyword* BoxTokenMarkerKeyword()
        {
            // Latches only on SUCCESS. A plain "compute once" static would cache
            // whatever the first call saw - and a call that happens to land
            // before the data handler is up would then pin null for the rest of
            // the process, which reads exactly like "the plugin is out of date".
            // Caching the miss is the initialisation-order hazard this codebase
            // has been bitten by before; not caching it costs one form-array
            // walk on the paths that run before kDataLoaded.
            static RE::BGSKeyword* s_marker = nullptr;
            if (s_marker) {
                return s_marker;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return nullptr;
            }
            for (auto* kw : dh->GetFormArray<RE::BGSKeyword>()) {
                if (kw && kw->formEditorID == kBoxTokenMarkerEdid) {
                    s_marker = kw;
                    return s_marker;
                }
            }
            return nullptr;
        }

        // Is the plugin that defines generation 0 present at all? Everything the
        // box store does assumes it, so when it is absent the answer is not
        // "every box is broken" - it is "CEF cannot judge anything right now",
        // and the settings must be left exactly as they are.
        bool CorePluginLoaded()
        {
            return PluginIsLoaded(tokenid::kCorePlugin);
        }

        RE::EffectSetting* ResolveMgef(const std::string& a_colonId)
        {
            const std::uint32_t formId = ResolveFormId(a_colonId);
            auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
            return form ? form->As<RE::EffectSetting>() : nullptr;
        }


        // Per effect: either a LIVE source Effect (full fidelity: magnitude
        // + duration + conditions) or a flat {mgef, magnitude} snapshot.
        // The pool's type, so a content's effects go straight there with no
        // conversion step to get wrong.
        using PendingEffect = abilities::SourceEffect;

        // What ONE content's enchantment is worth right now.
        //
        // Lifted out of FillEnchantSpell unchanged. The fixed ability pool
        // allocates per CONTENT and needs exactly this answer, and this priority
        // is not something to re-derive: it is the 2game.info fix (2026-08-20)
        // plus the 2026-09-10 field report. Reading the base form alone would
        // quietly drop player enchantments, tempering, and the conditions that
        // gate an effect - which is the bug that fix was for.
        std::vector<PendingEffect> ContentEffects(const std::string& c,
            const FrozenEnchantLookup& a_frozen, const char* a_name)
        {
            std::vector<PendingEffect> effs;
        const auto pushLive = [&effs](const RE::EnchantmentItem* a_ench) {
            for (auto* e : a_ench->effects) {
                if (e && e->baseEffect) {
                    effs.push_back({ nullptr, 0.0f, e });
                }
            }
        };
            if (g_statEnchantOff.contains(c)) {
                return effs;  // item-data toggle: enchant passthrough OFF
            }
            // Source priority (2026-08-20 conditions fix): the stored
            // original's instance enchantment, else - when the store
            // verifiably holds the original, so no re-enchant replaced the
            // base - the base form's enchantment, both at full fidelity.
            // Then the flat snapshot (original unreachable: other-character
            // persist, cross-save copy), and last the bare base form.
            auto* armo = ResolveArmo(c);
            const auto stored = FindStoredEnchant(c);
            if (stored.instance) {
                pushLive(stored.instance);
                return effs;  // this content is done
            }
            if (stored.inStore && armo && armo->formEnchanting) {
                pushLive(armo->formEnchanting);
                return effs;  // this content is done
            }
            const auto snap = g_contentEnchants.find(c);
            if (snap != g_contentEnchants.end()) {
                if (!armo) {
                    // The form does not resolve at all, so the plugin that
                    // defines this piece is not loaded and the piece is not in
                    // the game. The flat snapshot is for an original that is
                    // UNREACHABLE - another character's store, a copy carried
                    // in from another save - while the form itself is still
                    // there. It is not for a piece that has gone.
                    //
                    // Paying from it here hands out a FLATTENED enchantment,
                    // because a snapshot cannot carry conditions. Measured
                    // 2026-09-14 (7.2 #23a): disabling the source plugin turned
                    // "+250 while sneaking" into "+250, always" and "+200 while
                    // sneaking AND in combat" into "+200, always", and those
                    // became the live generations - a costume that no longer
                    // exists paying MORE than it ever did.
                    //
                    // SnapshotMatchesEnchant cannot catch this one: it decides
                    // by comparing against the base enchantment, and with the
                    // plugin gone there is no base enchantment to compare to.
                    return effs;
                }
                // The snapshot is FLAT: it cannot carry the conditions or
                // duration that gate an effect. When it is merely a copy of
                // the base enchantment, the live form is the SAME effects at
                // full fidelity - take that instead, or a conditional
                // enchant silently becomes always-on the moment the stored
                // original goes missing. Field-reported 2026-09-10:
                // disabling a costume's plugin for one session makes the
                // engine strip the captured item out of the hidden store,
                // which drops this content from source 2 to here, and a
                // "while sneaking" bonus started applying while standing.
                if (armo && armo->formEnchanting &&
                    SnapshotMatchesEnchant(snap->second, armo->formEnchanting)) {
                    pushLive(armo->formEnchanting);
                    return effs;  // this content is done
                }
                for (const auto& e : snap->second) {
                    if (auto* mgef = ResolveMgef(e.mgef)) {
                        effs.push_back({ mgef, e.magnitude, nullptr });
                    }
                }
            } else if (armo && armo->formEnchanting) {
                pushLive(armo->formEnchanting);
                return effs;  // this content is done
            }
            // 5th and last, per content: a frozen copy the HOLDER carries -
            // today only a published costume, which froze what each piece
            // was worth at publish time. Flat by construction, so it is
            // reached only when all four live sources came up empty for
            // THIS content, and never in place of one of them (2026-09-11
            // F01/F02). A published piece whose live sources are gone is
            // worth its frozen value; a piece whose sources are fine is
            // unaffected by any other piece's state.
            if (a_frozen && effs.empty()) {
                int recovered = 0;
                for (const auto& e : a_frozen(c)) {
                    if (auto* mgef = ResolveMgef(e.mgef)) {
                        effs.push_back({ mgef, e.magnitude, nullptr });
                        ++recovered;
                    }
                }
                if (recovered > 0) {
                    SKSE::log::info(
                        "boxes: '{}' fell back to its frozen snapshot for '{}' ({} effect(s), "
                        "flat - any conditions it had are not in that copy)",
                        c, a_name, recovered);
                }
            }
            return effs;
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
                for (const auto& c : StatAdmittedContents(a_box.contents)) {  // blacklist only
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
                for (const auto& c : StatAdmittedContents(a_box.contents)) {  // blacklist only
                    if (auto* armo = ResolveArmo(c)) {
                        if (!g_statArmorOff.contains(c)) {
                            // Captured temper multiplier scales the base rating the
                            // way the engine scales a worn tempered piece.
                            armorSum += armo->GetArmorRating() * TemperMultOf(c);
                        }
                        if (!g_statWeightOff.contains(c)) {
                            weightSum += armo->weight;
                        }
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
            g_stampedTokens.insert(a_box.token);  // so a vanished box can be undone
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
            RestoreTokenDefaultName(a_token);    // F10: put it back in TokenPool
            g_stampedTokens.erase(a_token);
        }

        // Undo the stamp on every token no box claims any more. Driven by what
        // we stamped rather than by the current definitions, because the whole
        // point is the case where the definition is gone (F06). TokenPool()
        // cannot stand in for this: it identifies a free token by its ESP name,
        // and a RENAMED box's token does not carry that name until it is freed.
        void ResetUnclaimedTokenStats()
        {
            const std::vector<std::string> stamped(g_stampedTokens.begin(), g_stampedTokens.end());
            for (const auto& token : stamped) {
                if (FindBox(token) < 0) {
                    ResetTokenStats(token);  // erases from g_stampedTokens
                    SKSE::log::info("boxes: cleared the token stats of vanished box '{}'", token);
                }
            }
        }

    }

    std::vector<WornItem> AbilityCatalog()
    {
        StoreLock lk;
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
                out.push_back({ EnsureUtf8(std::string(nm)), MakeColonId(spell) });
            }
        }
        std::sort(out.begin(), out.end(),
            [](const WornItem& a, const WornItem& b) { return a.name < b.name; });
        return out;
    }

    std::string BoxAbility(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].ability;
    }

    bool SetBoxAbility(const std::string& a_token, const std::string& a_ability)
    {
        StoreLock lk;
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
        StoreLock lk;
        if (a_ability.empty()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell = ResolveSpell(a_ability);
        if (player && spell && player->HasSpell(spell)) {
            RevokeAbility(player, spell, "manual:" + a_ability);
        }
    }

    std::vector<EnchantEffectInfo> ContentEnchantSnapshot(const std::string& a_content)
    {
        std::vector<EnchantEffectInfo> out;
        if (const auto it = g_contentEnchants.find(a_content); it != g_contentEnchants.end()) {
            for (const auto& effect : it->second)
                out.push_back({ effect.mgef, effect.magnitude });
            return out;
        }
        if (auto* armor = ResolveArmo(a_content); armor && armor->formEnchanting) {
            for (auto* effect : armor->formEnchanting->effects) {
                if (effect && effect->baseEffect) {
                    out.push_back({ MakeColonId(effect->baseEffect), effect->effectItem.magnitude });
                }
            }
        }
        return out;
    }

    namespace
    {
        // The instance-data extra list riding an inventory entry: the worn -
        // else first - list, i.e. the SAME one CaptureItemToStore moves into
        // the hidden store.
        RE::ExtraDataList* EntryInstanceList(const RE::InventoryEntryData* a_entry)
        {
            if (!a_entry || !a_entry->extraLists) {
                return nullptr;
            }
            for (auto* x : *a_entry->extraLists) {
                if (x && (x->HasType<RE::ExtraWorn>() || x->HasType<RE::ExtraWornLeft>())) {
                    return x;
                }
            }
            return a_entry->extraLists->empty() ? nullptr : a_entry->extraLists->front();
        }

        // Engine-measured temper multiplier for one extra list on a base armor:
        // GetArmorValue(with list) / GetArmorValue(bare). The item-card math is
        // NOT armorRating*health - a Fine (1.10) 40-rated cuirass displays +2ish,
        // not +4 (owner test 2026-08-18: real piece 45 vs naive token 47) - and
        // mods retune it via GMSTs, so let the engine do the arithmetic. The
        // ratio divides out the skill/perk multiplier, leaving exactly the factor
        // the token's base rating must carry for the token card to match the
        // real item's. Falls back to the raw health multiplier on VR (the
        // GetArmorValue thunk is unverified against the VR address map) and
        // wherever the engine value is unusable (zero-rated base).
        float MeasureTemperMult(RE::TESObjectARMO* a_armo, RE::ExtraDataList* a_xlist, float a_health)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!a_armo || !a_xlist || !player || REL::Module::IsVR()) {
                return a_health;
            }
            // Stack entries only borrow a_xlist; ~InventoryEntryData deletes the
            // list CONTAINER it allocated, never the ExtraDataList itself (the
            // moreHUD GetActualArmorRating pattern).
            RE::InventoryEntryData bare{ a_armo, 0 };
            const float base = player->GetArmorValue(&bare);
            RE::InventoryEntryData rated{ a_armo, 0 };
            rated.AddExtraList(a_xlist);
            const float withTemper = player->GetArmorValue(&rated);
            if (base <= 0.01f || withTemper <= base) {
                return a_health;
            }
            return withTemper / base;
        }

        // The temper multiplier to store for an inventory entry: 1.0 when the
        // instance is untempered, else the engine-measured armor ratio.
        float EntryTemperMult(const RE::InventoryEntryData* a_entry)
        {
            auto* xlist = EntryInstanceList(a_entry);
            const auto* xh = xlist ? xlist->GetByType<RE::ExtraHealth>() : nullptr;
            const float health = xh ? xh->health : 1.0f;
            if (health <= 1.0001f) {
                return 1.0f;
            }
            auto* armo = a_entry->object ? a_entry->object->As<RE::TESObjectARMO>() : nullptr;
            return MeasureTemperMult(armo, xlist, health);
        }
    }

    bool CaptureEnchant(const std::string& a_content)
    {
        StoreLock lk;
        // v1.3.2 r3 (re-review P2-2): function-boundary admission. The UIs
        // pre-gate for UX, but this native is public to any mod - without
        // this line an external caller could point it at a blocked static
        // armor and have its worn entry (extra lists, enchantment) read.
        {
            std::string why;
            if (!IsContentAdmissible(a_content, &why)) {
                SKSE::log::warn("boxes: CaptureEnchant refuses '{}' - {}", a_content, why);
                return false;
            }
        }
        // Stage marker: shared by the box and persist capture flows, and it reads
        // the player's inventory entry - the step the MARA class of crash lives in.
        SKSE::log::info("capture[enchant] '{}'", a_content);
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
        float temper = 1.0f;
        float carriedTemper = 1.0f;
        bool sawWorn = false;
        // Target-only filter (review P1-2): the old Armor-wide filter made
        // GetInventory copy EVERY armor's InventoryEntryData - capturing a
        // perfectly safe item still copied a foreign runtime item's hostile
        // entry (the MARA CTD face) as a side effect. Only the captured
        // form's entry is ever touched now.
        auto inv = player->GetInventory([baseId](RE::TESBoundObject& a_obj) {
            return a_obj.GetFormID() == baseId;
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
                temper = EntryTemperMult(entry.get());
                sawWorn = true;
                break;
            }
            if (!carried) {
                carried = entry->GetEnchantment();
                carriedTemper = EntryTemperMult(entry.get());
            }
        }
        if (!ench) {
            ench = carried;
        }
        if (!sawWorn) {
            temper = carriedTemper;
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
        // Tempering snapshot at the SAME spot (2game.info 2026-08-18): the token
        // sums base armorRating, so the smithing improvement must ride a captured
        // multiplier exactly like the player enchantment does.
        if (temper > 1.0001f) {
            SKSE::log::info("boxes: captured temper x{:.2f} for '{}'", temper, a_content);
            g_contentTemper[a_content] = temper;
        } else {
            g_contentTemper.erase(a_content);
        }
        WriteJson();
        // The holder's synthesized ability / token stats may ALREADY be built:
        // the persist aggregate builds at load, and the MCM flow runs the add
        // native (which queues its rebuild) BEFORE this capture - a snapshot
        // stored now would silently sit out until the next game load (07-21
        // finding; the reporter's "enchant not reflected"). Rebuild + re-apply
        // here so the caller's ordering can't matter.
        ReapplyStatsForContent(a_content);
        return has;
    }

    namespace
    {
        void SyncSpell(RE::Actor* a_player, RE::SpellItem* a_spell, bool a_worn,
            std::string_view a_key)
        {
            if (!a_spell) {
                return;
            }
            if (a_worn) {
                GrantAbility(a_player, a_spell, a_key);
            } else {
                RevokeAbility(a_player, a_spell, a_key);
            }
        }

    }

    void ApplyBoxAbilities()
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const bool cefOn = CefEnabled();  // ROOT G: master switch gates box abilities too

        // Which CONTENTS should be paying out right now. One ability per
        // content, not one per box: two pieces carrying the same enchantment
        // are two abilities and are worth two, where a single spell holding
        // both had them collapsed into one by the engine (eb6feb3). The folding
        // that worked around that is not needed on this path and is not done.
        std::vector<std::string> wanted;
        const auto admit = [&wanted](const std::vector<std::string>& a_ids, const char* a_what) {
            const auto admitted = StatAdmittedContents(a_ids);
            // Name what got dropped. This gate is the one place a content can
            // stop contributing stats with nothing said anywhere - the reason
            // "my enchantment stopped applying" had no log line to look at
            // (test run 2026-09-10). A drop here is normal after a plugin is
            // disabled or blacklisted; it is the SILENCE that is the problem.
            if (admitted.size() != a_ids.size()) {
                for (const auto& c : a_ids) {
                    if (std::find(admitted.begin(), admitted.end(), c) == admitted.end()) {
                        SKSE::log::warn(
                            "boxes: '{}' contributes no stats to {} - not admitted right now "
                            "(unresolved plugin, or blocked by the capture blacklist)",
                            c, a_what);
                    }
                }
            }
            wanted.insert(wanted.end(), admitted.begin(), admitted.end());
        };

        // A token that is WORN while its box's distribution is off is the state
        // the two switches must never leave behind: "Distribute token" off takes
        // the token away and zeroes its stats, yet here it is, on the player,
        // showing its costume and paying nothing. Wearing a box is a request to
        // use it, so the wearing wins and distribution goes back on.
        //
        // This sits here rather than in WearBoxToken because WearBoxToken is only
        // CEF's own "Wear (show contents)" checkbox. A token equipped from the
        // INVENTORY - which is how a box is normally worn - never goes through
        // it, so hooking it covered the one path nobody uses (2026-09-14: the
        // fix shipped, the test still measured nothing). This function sees the
        // token worn no matter what put it on, so it is the only place the rule
        // can be stated once.
        if (cefOn) {
            for (const auto& b : g_boxes) {
                if (b.enabled || b.contents.empty() || !TokenWorn(b.token)) {
                    continue;
                }
                SKSE::log::info("boxes: '{}'{} is WORN, so its token distribution is turned back "
                                "ON - a box cannot be worn with its token withheld",
                    b.token, b.label.empty() ? "" : " (" + b.label + ")");
                SetBoxEnabledNoSync(b.token, true);  // no converge: we ARE the converge
            }
        }

        for (const auto& b : g_boxes) {
            // With CEF disabled nothing is injected, so a worn token must not still
            // grant its contents' enchant/armor effects (only the persist spell was
            // gated before - border audit [2161]).
            // b.enabled is the SMF "Distribute token" switch. SetTokenStats
            // already zeroes an off box's armor, weight and keywords, but the
            // ability only asked whether the token was worn - so a token put
            // on by any other means still granted the enchantments of a box
            // the user had turned off (review 2026-09-11 N2).
            const bool worn = cefOn && b.enabled && TokenWorn(b.token);
            if (worn) {
                admit(b.contents, "a worn box");
            }
            // Optional manual extra ability (dormant unless set in json). Still
            // an ESP-defined spell the user named, so it is not pool business.
            if (!b.ability.empty()) {
                SyncSpell(player, ResolveSpell(b.ability), worn, "manual:" + b.ability);
            }
        }
        // Persist class: no token, always shown while CEF is enabled. Built from
        // THIS SAVE'S ACTIVE set, not the shared catalog (M2) - a non-active
        // entry another character cataloged must not grant effects here.
        if (cefOn) {
            admit(ActivePersistIds(), "persist");
        }

        // The player can be wearing a published costume as well as boxes, and
        // SyncToActor converges the WHOLE pool for an actor - so calling it once
        // per source would have the second call revoke what the first granted.
        // Every source an actor draws from has to be in ONE list.
        admit(PublishStatsFor(player), "a worn published costume");

        // One convergence over the whole pool. A content dropped from a box, a
        // deleted box, a box whose token was handed to a publish slot, a
        // costume recalled, the master switch, a load: all of them are "not in
        // wanted", and none needs a path of its own to avoid stranding.
        abilities::SyncToActor(player, wanted);

        // NPC wearers converge one actor at a time, for the same reason and by
        // the same rule (7.6): a master toggle can never strand spells on them.
        SyncNpcAbilities();
    }

    std::vector<abilities::SourceEffect> ContentEffectsFor(const std::string& a_contentId)
    {
        StoreLock lk;
        // The frozen fallback is looked up BY CONTENT rather than passed in. It
        // belongs to whichever published costume holds the piece, a piece has
        // exactly one holder, and asking that way means one entry point answers
        // for a box content and a published one alike.
        return ContentEffects(a_contentId,
            [](const std::string& a_id) { return FrozenEffectsForContent(a_id); }, "pool");
    }

    void RebuildBoxAbility(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return;
        }
        // Re-derive each content's recipe. A content whose enchantment actually
        // changed gets a NEW slot at the next generation rather than having its
        // recipe rewritten, because a save may hold an active effect built from
        // the old one. Nothing is granted or removed here: the next
        // ApplyBoxAbilities converges the whole pool anyway.
        for (const auto& c : g_boxes[static_cast<std::size_t>(idx)].contents) {
            abilities::RefreshContent(c);
        }
    }


    void RebuildPersistAbility()
    {
        StoreLock lk;
        for (const auto& c : ActivePersistIds()) {
            abilities::RefreshContent(c);
        }
    }

    void InvalidateStatAbilities()
    {
        StoreLock lk;
        // A load brings in ANOTHER save's boxes, so every ability's effects are
        // stale. Take ours back off the player and mark them for a refill - but
        // KEEP the forms. Forgetting them here is what stacked a "Costume Stats"
        // per in-process reload: the save restores the ability by form id, and a
        // forgotten form is one nobody can ever remove again (v1.6.1.1).
        // Nothing to take off any more: the pool's abilities are static forms,
        // and the save being loaded brings its OWN spell list, so a previous
        // save's grants are not carried into this one.
        //
        // What does need re-deriving is the VALUE. A content's effects are read
        // from this save's hidden store, so the same piece can be worth
        // something different here than it was in the save before - a captured,
        // tempered original in one character's store and a bare base form in
        // another's. RefreshContent re-points the content at a slot holding the
        // right value, reusing one it has already had rather than taking a new
        // one each time a character is switched.
        int refreshed = 0;
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                abilities::RefreshContent(c);
                ++refreshed;
            }
        }
        for (const auto& c : ActivePersistIds()) {
            abilities::RefreshContent(c);
            ++refreshed;
        }
        const int dropped = refreshed;
        // Published costumes too. Their contents are global, but the effects
        // built from them read THIS save's hidden store, so another save's build
        // must not be carried over - and until now nothing took them back
        // (review 2026-09-11 F08).
        InvalidatePublishAbilities();
        SKSE::log::info("boxes: stat abilities re-derived for this save ({} content(s))", dropped);
    }

    std::string BoxStatsSummary(int a_index)
    {
        StoreLock lk;
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        const auto& box = g_boxes[a_index];
        float armorSum = 0.0f;
        float weightSum = 0.0f;
        std::vector<std::string> effs;
        for (const auto& c : StatAdmittedContents(box.contents)) {  // blacklist only
            auto* armo = ResolveArmo(c);
            if (!armo) {
                continue;
            }
            // Item-data toggles: an OFF channel leaves the summary too, so the
            // readout matches what actually reaches the token/ability. Armor is
            // shown at item-card scale (CardArmorOf) so it agrees with the
            // token's own card.
            if (!g_statArmorOff.contains(c)) {
                armorSum += CardArmorOf(armo) * TemperMultOf(c);
            }
            if (!g_statWeightOff.contains(c)) {
                weightSum += armo->weight;
            }
            if (g_statEnchantOff.contains(c)) {
                continue;  // enchant OFF: skip the effect listing below
            }
            // Same priority as the synthesized ability (FillEnchantSpell):
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
                        (nm && *nm) ? EnsureUtf8(nm).c_str() : "effect", e.magnitude);
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
                        (nm && *nm) ? EnsureUtf8(nm).c_str() : "effect", e->effectItem.magnitude);
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
        StoreLock lk;
        if (!a_store) {
            g_storeFormId = 0;
            return;
        }
        // First resolving store wins per save (P2): once the native layer owns a
        // live store (co-save restored or self-created), a LATER MCM handoff of a
        // DIFFERENT container must not re-point custody mid-save - items already
        // captured would strand in the first one. The legacy-adoption path (save
        // predates the co-save 'STOR' record; native has nothing yet) still
        // adopts the MCM's container as before.
        if (g_storeFormId && g_storeFormId != a_store->GetFormID() &&
            RE::TESForm::LookupByID(g_storeFormId)) {
            SKSE::log::warn("custody: MCM handed store {:08X} but native owns {:08X} - keeping "
                            "native's (one custody home per save)",
                a_store->GetFormID(), g_storeFormId);
            return;
        }
        g_storeFormId = a_store->GetFormID();
    }

    std::uint32_t StoreFormId()
    {
        StoreLock lk;
        return g_storeFormId;
    }

    void RestoreStoreFormId(std::uint32_t a_id)
    {
        StoreLock lk;
        g_storeFormId = a_id;
    }

    RE::TESObjectREFR* EnsureStoreRef()
    {
        StoreLock lk;
        if (g_storeFormId) {
            auto* f = RE::TESForm::LookupByID(g_storeFormId);
            if (auto* r = f ? f->As<RE::TESObjectREFR>() : nullptr) {
                return r;
            }
            SKSE::log::warn("custody: recorded store {:08X} no longer resolves - creating a new one",
                g_storeFormId);
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* base = dh ? dh->LookupForm<RE::TESObjectCONT>(0x00080D, "CostumeFW.esp") : nullptr;
        if (!player || !base) {
            SKSE::log::warn("custody: cannot create hidden store (player or CostumeFW.esp 0x80D missing)");
            return nullptr;
        }
        // Force-persist: unlike the MCM's container (pinned by a persistent
        // Papyrus ObjectReference property), nothing else holds this ref.
        auto ref = player->PlaceObjectAtMe(base, true);
        if (!ref) {
            SKSE::log::warn("custody: PlaceObjectAtMe failed for the hidden store");
            return nullptr;
        }
        ref->Disable();
        g_storeFormId = ref->GetFormID();
        SKSE::log::info("custody: created hidden store {:08X}", g_storeFormId);
        return ref.get();
    }

    bool CaptureItemToStore(const std::string& a_id)
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t formId = ResolveFormId(a_id);
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        auto* store = EnsureStoreRef();
        if (!player || !obj || !store) {
            return false;
        }
        auto inv = player->GetInventory([&](RE::TESBoundObject& o) { return &o == obj; });
        const auto it = inv.find(obj);
        if (it == inv.end() || it->second.first <= 0) {
            SKSE::log::warn("custody: capture '{}' - player has none", a_id);
            return false;
        }
        // Prefer the worn / any instance-data list so tempering + player enchants
        // travel with the stored copy (the engine unequips a worn list on remove).
        RE::ExtraDataList* xlist = nullptr;
        if (const auto& entry = it->second.second; entry && entry->extraLists) {
            for (auto* x : *entry->extraLists) {
                if (x && (x->HasType<RE::ExtraWorn>() || x->HasType<RE::ExtraWornLeft>())) {
                    xlist = x;
                    break;
                }
            }
            if (!xlist && !entry->extraLists->empty()) {
                xlist = entry->extraLists->front();
            }
        }
        player->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, xlist, store);
        SKSE::log::info("custody: captured 1x '{}' into the hidden store", a_id);
        // The custody row starts here. This is the ONLY moment the id is
        // guaranteed to be recorded somewhere the user can reach later: if the
        // box entry is rolled back away, the row is all that is left of it.
        RecordCustody(a_id, "captured");
        return true;
    }

    bool WearBoxToken(const std::string& a_token, bool a_wear)
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t formId = ResolveFormId(a_token);
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!player || !obj || !em) {
            return false;
        }
        if (a_wear) {
            // A box whose distribution is off is put back on by the convergence
            // in ApplyBoxAbilities, which the equip below triggers - one rule in
            // one place, reached whether the token was put on from here or from
            // the inventory.
            const auto counts = player->GetInventoryCounts();
            const auto it = counts.find(obj);
            if (it == counts.end() || it->second <= 0) {
                player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            }
            em->EquipObject(player, obj, nullptr, 1, nullptr, true, false, false);
        } else {
            em->UnequipObject(player, obj, nullptr, 1, nullptr, true, false, false);
        }
        return true;
    }

    void GiveOrRemoveToken(const std::string& a_token, bool a_give)
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t formId = ResolveFormId(a_token);
        auto* form = formId ? RE::TESForm::LookupByID(formId) : nullptr;
        auto* obj = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!player || !obj) {
            return;
        }
        if (a_give) {
            const auto counts = player->GetInventoryCounts();
            const auto it = counts.find(obj);
            if (it == counts.end() || it->second <= 0) {
                player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            }
        } else {
            WearBoxToken(a_token, false);
            player->RemoveItem(obj, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
    }

    bool ReturnStoredItem(const std::string& a_id, bool a_fabricate, bool* a_wasRecreated)
    {
        StoreLock lk;
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
                    RecordCustody(a_id, "returned");
                    if (a_wasRecreated) {
                        *a_wasRecreated = false;
                    }
                    return true;
                }
            }
        }
        if (a_fabricate) {
            player->AddObjectToContainer(obj, nullptr, 1, nullptr);
            SKSE::log::info("custody: fabricated 1x '{}' (none stored on this save)", a_id);
            RecordCustody(a_id, "recreated");
            if (a_wasRecreated) {
                *a_wasRecreated = true;
            }
            return true;
        }
        return false;
    }

    bool RecoverContentItem(const std::string& a_id)
    {
        StoreLock lk;
        // ROOT A ([2279]): drain the store first, fabricate only on a store miss -
        // so recovering a still-stored item no longer mints a second copy.
        bool recreated = false;
        if (ReturnStoredItem(a_id, true, &recreated)) {
            SKSE::log::info("recover: granted 1x '{}' ({}, {})", ItemDisplayName(a_id), a_id,
                recreated ? "plain copy - the original was not in storage" : "the captured original");
            // Say WHICH it was. A recreated copy has no tempering and no player
            // enchantment, and the difference is invisible in the inventory - the
            // user has to be told at the moment they ask for it, not left to find
            // out when the numbers are wrong.
            RE::DebugNotification(
                (recreated
                        ? "CostumeFW: the original was not in storage - you got a plain copy"
                        : "CostumeFW: returned your captured original")
            );
            WriteJson(false);  // console / Recovery UI callers do not persist otherwise
            return true;
        }
        SKSE::log::warn("recover: '{}' does not resolve to an inventory item", a_id);
        return false;
    }

    bool HealStoredInstanceData()
    {
        StoreLock lk;
        // v1.6.1 (2game.info 2026-08-18): contents captured before tempering was
        // snapshotted - or whose player-enchant snapshot lost the old capture
        // race - are not data losses: the hidden store still holds the original
        // item WITH its ExtraDataList. Recover the missing channels from it, so
        // existing boxes heal on load with no re-capture. Post-load main thread
        // (the co-save has restored this save's store id by then).
        if (!g_storeFormId) {
            return false;
        }
        auto* form = RE::TESForm::LookupByID(g_storeFormId);
        auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!store) {
            return false;
        }
        std::unordered_map<std::uint32_t, std::string> wanted;
        // Every held content is scanned: a missing snapshot heals, and an
        // existing temper entry RE-measures against the stored original (the
        // capture model changed once - raw health -> engine ratio - and the
        // engine's arithmetic itself can change under the user's GMST mods).
        const auto want = [&wanted](const std::string& a_id) {
            if (const std::uint32_t formId = ResolveFormId(a_id)) {
                wanted.emplace(formId, a_id);
            }
        };
        for (const auto& b : g_boxes) {
            for (const auto& c : b.contents) {
                want(c);
            }
        }
        for (const auto& c : PersistContents()) {
            want(c);
        }
        if (wanted.empty()) {
            return false;
        }
        bool changed = false;
        // Target-only filter (the P1-2 lesson): only our own captured originals'
        // entries are ever copied out of the store.
        auto inv = store->GetInventory([&wanted](RE::TESBoundObject& a_obj) {
            return wanted.contains(a_obj.GetFormID());
        });
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (!obj || count <= 0 || !entry || !entry->extraLists) {
                continue;
            }
            const auto found = wanted.find(obj->GetFormID());
            if (found == wanted.end()) {
                continue;
            }
            const std::string& id = found->second;
            {
                const float mult = EntryTemperMult(entry.get());
                const auto prev = g_contentTemper.find(id);
                if (mult > 1.0001f) {
                    if (prev == g_contentTemper.end() ||
                        std::abs(prev->second - mult) > 0.005f) {
                        SKSE::log::info(
                            "heal: '{}' temper x{:.3f} measured from the stored original", id, mult);
                        g_contentTemper[id] = mult;
                        changed = true;
                    }
                } else if (prev != g_contentTemper.end()) {
                    SKSE::log::info(
                        "heal: '{}' stored original is untempered - dropping stale multiplier", id);
                    g_contentTemper.erase(prev);
                    changed = true;
                }
            }
            if (!g_contentEnchants.contains(id)) {
                RE::EnchantmentItem* ench = nullptr;
                for (auto* x : *entry->extraLists) {
                    if (!x) {
                        continue;
                    }
                    // Instance (player) enchantment only - a base enchantment
                    // already flows through the FillEnchantSpell fallback.
                    if (const auto* xe = x->GetByType<RE::ExtraEnchantment>();
                        xe && xe->enchantment) {
                        ench = xe->enchantment;
                        break;
                    }
                }
                std::vector<EnchEffect> effs;
                if (ench) {
                    for (auto* e : ench->effects) {
                        if (e && e->baseEffect) {
                            effs.push_back({ MakeColonId(e->baseEffect), e->effectItem.magnitude });
                        }
                    }
                }
                if (!effs.empty()) {
                    SKSE::log::info(
                        "heal: '{}' player enchant recovered from the stored original ({} effect(s))",
                        id, effs.size());
                    g_contentEnchants[id] = std::move(effs);
                    changed = true;
                }
            }
        }
        if (changed) {
            WriteJson();
        }
        return changed;
    }

    namespace
    {
        std::string NowStamp()
        {
            const auto now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            char buf[32]{};
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            return buf;
        }

        // Overwrites the id's row: what matters is what happened LAST. The name is
        // re-read each time so a row keeps working after the item's plugin moved.
        // Does not write the json itself - every custody flow already persists, and
        // the paths that don't write once at their own end.
        void RecordCustody(const std::string& a_id, const char* a_event)
        {
            if (a_id.empty()) {
                return;
            }
            auto& row = g_custody[a_id];
            row.id = a_id;
            row.name = ItemDisplayName(a_id);
            row.event = a_event;
            row.when = NowStamp();
            if (g_custody.size() <= kCustodyMax) {
                return;
            }
            // Over the cap: drop the oldest row. Stamps are sortable strings.
            auto oldest = g_custody.begin();
            for (auto it = g_custody.begin(); it != g_custody.end(); ++it) {
                if (it->second.when < oldest->second.when) {
                    oldest = it;
                }
            }
            g_custody.erase(oldest);
        }

        // Every content id something still holds. The sweep returns what is NOT
        // in here, so a forgotten source means handing back a costume that is
        // still in use - all four are load-bearing:
        //   box contents / persist catalog / PUBLISHED snapshots / NPC-persist.
        // The last two are the easy ones to miss: their items sit in the same
        // hidden store while an NPC wears them. NPC assignments must include the
        // UNRESOLVED ones - at load the actor's cell usually is not loaded yet -
        // which is why this reads NprAssignmentsForSave() and not the live list.
        // The persist CATALOG (not this save's active set) is deliberate: it is
        // the wider set, and a wider held set can only mean returning less.
        //
        // Keyed by resolved FormID rather than colon-id string, so no spelling
        // difference between sources can make a held item look orphaned.
        std::unordered_set<std::uint32_t> BuildHeldFormIds()
        {
            std::unordered_set<std::uint32_t> held;
            const auto add = [&held](const std::string& a_id) {
                if (const std::uint32_t formId = ResolveFormId(a_id)) {
                    held.insert(formId);
                }
            };
            for (const auto& b : g_boxes) {
                for (const auto& c : b.contents) {
                    add(c);
                }
            }
            for (const auto& c : PersistContents()) {
                add(c);
            }
            for (const auto& snap : PublishedSnapshot()) {
                for (const auto& c : snap.contents) {
                    add(c);
                }
            }
            for (const auto& npr : NprAssignmentsForSave()) {
                for (const auto& c : npr.contents) {
                    add(c);
                }
            }
            return held;
        }

        // One load may hand back at most this many items. A bug that made the
        // held set look empty would otherwise empty the whole store in one go;
        // the rest simply waits for the next load.
        constexpr int kSweepCap = 64;
    }

    namespace
    {
        // Who holds each content id, by resolved FormID - the same four sources
        // the sweep judges by, but keeping WHO rather than yes/no. One builder so
        // the store listing, the sweep and the Recovery list can never disagree
        // about what "held" means.
        std::unordered_map<std::uint32_t, std::string> BuildHolderLabels()
        {
            std::unordered_map<std::uint32_t, std::string> holder;
            const auto mark = [&holder](const std::string& a_id, const std::string& a_who) {
                if (const std::uint32_t formId = ResolveFormId(a_id)) {
                    holder.emplace(formId, a_who);
                }
            };
            for (const auto& b : g_boxes) {
                for (const auto& c : b.contents) {
                    mark(c, "box '" + (b.label.empty() ? b.token : b.label) + "'");
                }
            }
            for (const auto& c : PersistContents()) {
                mark(c, "persist");
            }
            for (const auto& snap : PublishedSnapshot()) {
                for (const auto& c : snap.contents) {
                    mark(c, "published '" + snap.label + "'");
                }
            }
            for (const auto& npr : NprAssignmentsForSave()) {
                for (const auto& c : npr.contents) {
                    mark(c, "NPC persist");
                }
            }
            return holder;
        }
    }

    std::vector<CustodyRow> CustodyRows()
    {
        StoreLock lk;
        // ONE holder scan for the whole list. The per-id form of this used to be
        // called once per row from the render callback, which rebuilt this map -
        // every box, persist, published and NPC-persist content, each resolved
        // and each label freshly allocated - for every row, every frame.
        const auto holder = BuildHolderLabels();
        // ONE store read for the whole list, same reasoning as the holder scan.
        // Counts, not GetInventory: presence is all this needs, and GetInventory
        // copies every stack's entry data (the MARA CTD's shape).
        std::unordered_set<std::uint32_t> inStore;
        {
            auto* sform = g_storeFormId ? RE::TESForm::LookupByID(g_storeFormId) : nullptr;
            if (auto* store = sform ? sform->As<RE::TESObjectREFR>() : nullptr) {
                for (const auto& [obj, count] : store->GetInventoryCounts()) {
                    if (obj && count > 0) {
                        inStore.insert(obj->GetFormID());
                    }
                }
            }
        }
        std::vector<CustodyRow> out;
        out.reserve(g_custody.size());
        for (const auto& [id, e] : g_custody) {
            CustodyRow row;
            row.entry = e;
            if (const std::uint32_t formId = ResolveFormId(id)) {
                row.resolves = true;
                row.inStore = inStore.contains(formId);
                if (const auto it = holder.find(formId); it != holder.end()) {
                    row.holder = it->second;
                }
            }
            out.push_back(std::move(row));
        }
        std::sort(out.begin(), out.end(), [](const CustodyRow& a, const CustodyRow& b) {
            return a.entry.when > b.entry.when;
        });
        return out;
    }

    std::vector<std::string> StoreContentIdsForSave()
    {
        StoreLock lk;
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        auto* form = g_storeFormId ? RE::TESForm::LookupByID(g_storeFormId) : nullptr;
        auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (store) {
            // Counts, not GetInventory: this only needs to know WHICH objects are
            // in there, and GetInventory copies each stack's InventoryEntryData -
            // the broad-filter copy that is the MARA CTD's shape (see the
            // target-only filter note in CaptureEnchant).
            for (const auto& [obj, count] : store->GetInventoryCounts()) {
                if (!obj || count <= 0 || IsCefToken(obj->GetFormID())) {
                    continue;  // CEF's own tokens legitimately live here
                }
                std::string id = MakeColonId(obj);
                if (!id.empty() && seen.insert(id).second) {
                    out.push_back(std::move(id));
                }
            }
        }
        // Carry forward what we cannot SEE this session. With the content's
        // plugin disabled the item is not in the container to be listed, so
        // writing only what we found would erase the very record that lets the
        // next load tell "gone" from "never here" (ROOT H does the same for
        // unresolved persist actives).
        int carried = 0;
        for (const auto& id : g_storeManifest) {
            if (ResolveFormId(id) == 0 && seen.insert(id).second) {
                out.push_back(id);
                ++carried;
            }
        }
        if (carried > 0) {
            SKSE::log::info(
                "store manifest: carrying {} entry(ies) whose plugin is not loaded", carried);
        }
        return out;
    }

    void RestoreStoreManifest(std::vector<std::string> a_ids)
    {
        StoreLock lk;
        for (auto& id : a_ids) {
            CanonicalizeColonId(id);  // ROOT D
        }
        g_storeManifest = std::move(a_ids);
    }

    int LastStoreLossCount()
    {
        return g_lastStoreLoss.load(std::memory_order_relaxed);
    }
    int ReportLostStoreItems()
    {
        StoreLock lk;
        g_lastStoreLoss.store(0, std::memory_order_relaxed);
        if (g_storeManifest.empty()) {
            return 0;  // no manifest yet (pre-1.6.2.2 save, or a new game)
        }
        auto* form = g_storeFormId ? RE::TESForm::LookupByID(g_storeFormId) : nullptr;
        auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!store) {
            // Without the store there is nothing to compare against, and every
            // entry would look lost. Say nothing rather than cry wolf.
            SKSE::log::info("store manifest: no store on this save - comparison skipped");
            return 0;
        }
        std::unordered_set<std::uint32_t> present;
        for (const auto& [obj, count] : store->GetInventoryCounts()) {
            if (obj && count > 0) {
                present.insert(obj->GetFormID());
            }
        }
        int lost = 0;
        int unreadable = 0;
        for (const auto& id : g_storeManifest) {
            const std::uint32_t formId = ResolveFormId(id);
            if (formId == 0) {
                // The plugin is not loaded THIS session. The item is unreadable,
                // not proven gone - the save may still hold it. Carried forward
                // by StoreContentIdsForSave; nothing is reported.
                ++unreadable;
                continue;
            }
            if (present.contains(formId)) {
                continue;
            }
            // The form resolves and the store does not have it: the engine took
            // it out. Its tempering and player enchantment went with it; a
            // recovery from here can only mint a plain copy.
            SKSE::log::warn(
                "store manifest: '{}' ({}) was in this save's storage and is gone - "
                "recovering it now can only make a plain copy",
                ItemDisplayName(id), id);
            RecordCustody(id, "lost");
            ++lost;
        }
        if (unreadable > 0) {
            SKSE::log::info(
                "store manifest: {} entry(ies) unreadable this session (plugin not loaded)",
                unreadable);
        }
        g_lastStoreLoss.store(lost, std::memory_order_relaxed);
        if (lost > 0) {
            WriteJson(false);
            SKSE::log::warn("store manifest: {} captured item(s) no longer in storage", lost);
            RE::DebugNotification(std::format(
                "CostumeFW: {} captured item(s) are no longer in storage - see Recovery",
                lost).c_str());
        } else {
            SKSE::log::info("store manifest: {} entry(ies) checked, none missing",
                g_storeManifest.size());
        }
        return lost;
    }
    int BackfillCustodyRows()
    {
        StoreLock lk;
        // The history only started recording at v1.6.2, so an existing user's
        // Recovery list would show nothing but the handful of items they have
        // moved since - useless for the one job it has, naming a piece that went
        // missing. Everything CEF holds today is something it took custody of at
        // some point, so give each one a row now, while it is still reachable.
        // A row cannot be created after the item is lost; that is the whole point.
        if (!g_settingsLoadOk) {
            return 0;
        }
        int added = 0;
        for (const auto& [formId, who] : BuildHolderLabels()) {
            auto* form = RE::TESForm::LookupByID(formId);
            if (!form) {
                continue;
            }
            const std::string id = MakeColonId(form);
            if (id.empty() || g_custody.contains(id)) {
                continue;  // already recorded - never overwrite a real event
            }
            RecordCustody(id, "held");
            ++added;
        }
        if (added > 0) {
            SKSE::log::info("custody: recorded {} item(s) already held (history backfill)", added);
            WriteJson(false);
        }
        return added;
    }

    std::vector<std::string> StoreDiagLines()
    {
        StoreLock lk;
        std::vector<std::string> out;
        if (!g_storeFormId) {
            out.push_back("[CEF] no hidden store on this save (nothing captured yet)");
            return out;
        }
        auto* form = RE::TESForm::LookupByID(g_storeFormId);
        auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!store) {
            out.push_back(std::format(
                "[CEF] hidden store {:08X} does not resolve on this save", g_storeFormId));
            return out;
        }
        const auto holder = BuildHolderLabels();
        auto inv = store->GetInventory();
        int items = 0;
        int orphans = 0;
        std::vector<std::string> rows;
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (!obj || count <= 0) {
                continue;
            }
            ++items;
            const std::string id = MakeColonId(obj);
            std::string row = std::format("  {} x{}  {}", id, count, ItemDisplayName(id));
            if (const auto it = holder.find(obj->GetFormID()); it != holder.end()) {
                row += "  [held by " + it->second + "]";
            } else if (IsCefToken(obj->GetFormID())) {
                row += "  [CEF token - not a content]";
            } else {
                row += "  [ORPHAN - nothing holds this]";
                ++orphans;
            }
            // The two channels a returned original carries and a recreated copy
            // does not - the difference the Recovery confirmation warns about.
            if (entry) {
                if (const float mult = EntryTemperMult(entry.get()); mult > 1.0001f) {
                    row += std::format("  temper x{:.2f}", mult);
                }
                if (entry->GetEnchantment()) {
                    row += "  enchanted";
                }
            }
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end());
        out.push_back(std::format("[CEF] hidden store {:08X}: {} item stack(s), {} orphaned",
            g_storeFormId, items, orphans));
        out.insert(out.end(), rows.begin(), rows.end());
        if (items == 0) {
            out.push_back("  (empty)");
        }
        return out;
    }

    int SweepOrphanedStoredItems()
    {
        StoreLock lk;
        // The loss direction of the two-layer rollback (CEF_STATE_SCOPE.md §4):
        // box definitions live in a GLOBAL json written immediately, the captured
        // items live in a hidden store INSIDE the save. Take a piece out of a box,
        // quit without saving, load the older save, and the json says "nobody owns
        // it" while the item is still sitting in the store - reachable only by
        // `cef recover <id>`, which needs an id the user has no way to know.
        //
        // This hands those back. It only ever RETURNS: it never deletes from the
        // store and never edits the json. After a rollback the json can legitimately
        // be the NEWER of the two, so treating it as the truth and destroying items
        // to match would turn a recoverable mismatch into real data loss. Returning
        // a duplicate is the worst this can do.
        if (!g_settingsLoadOk) {
            SKSE::log::warn("sweep: skipped - the settings load did not complete cleanly");
            return 0;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_storeFormId) {
            // Every exit reports, at info: this feature exists because the user
            // cannot see what happened to their items, and a support log that
            // says nothing about the sweep cannot tell "decided to do nothing"
            // from "never ran". One line per save load is worth that.
            SKSE::log::info("sweep: no {} - nothing to sweep",
                player ? "hidden store on this save" : "player");
            return 0;
        }
        auto* form = RE::TESForm::LookupByID(g_storeFormId);
        auto* store = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!store) {
            SKSE::log::info("sweep: this save's store ref {:08X} no longer resolves",
                g_storeFormId);
            return 0;
        }
        const auto held = BuildHeldFormIds();

        struct Orphan
        {
            RE::TESBoundObject* obj{ nullptr };
            std::string id;
            std::int32_t count{ 0 };
        };
        std::vector<Orphan> orphans;
        auto inv = store->GetInventory([&held](RE::TESBoundObject& a_obj) {
            // CEF's own tokens are never contents; they can legitimately sit here.
            return !held.contains(a_obj.GetFormID()) && !IsCefToken(a_obj.GetFormID());
        });
        for (auto& [obj, data] : inv) {
            const auto& [count, entry] = data;
            if (!obj || count <= 0) {
                continue;
            }
            orphans.push_back({ obj, MakeColonId(obj), count });
        }
        if (orphans.empty()) {
            SKSE::log::info("sweep: nothing orphaned ({} held id(s))", held.size());
            return 0;
        }
        int returned = 0;
        int items = 0;
        for (const auto& o : orphans) {
            if (returned >= kSweepCap) {
                SKSE::log::warn("sweep: cap reached - {} more item(s) wait for the next load",
                    static_cast<int>(orphans.size()) - items);
                break;
            }
            // Container move, not fabricate: the original travels back WITH its
            // instance data (tempering, a player enchantment).
            store->RemoveItem(o.obj, o.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                nullptr, player);
            SKSE::log::info("sweep: returned {}x '{}' ({}) - held by nothing",
                o.count, ItemDisplayName(o.id), o.id);
            RecordCustody(o.id, "returned-orphan");
            returned += o.count;
            ++items;
        }
        WriteJson(false);  // persist the custody log; no box changed, so no manifest
        if (items == 1) {
            RE::DebugNotification(
                ("CostumeFW: returned " + ItemDisplayName(orphans.front().id) +
                    " - it was left in storage")
                    .c_str());
        } else if (items > 1) {
            RE::DebugNotification(
                ("CostumeFW: returned " + std::to_string(items) +
                    " items left in storage")
                    .c_str());
        }
        return returned;
    }

    int BoxCount()
    {
        StoreLock lk;
        return static_cast<int>(g_boxes.size());
    }

    int FindBoxById(const std::string& a_boxId)
    {
        StoreLock lk;
        if (a_boxId.empty()) {
            return -1;
        }
        for (std::size_t i = 0; i < g_boxes.size(); ++i) {
            if (g_boxes[i].boxId == a_boxId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int FindBoxByToken(const std::string& a_token)
    {
        StoreLock lk;
        return FindBox(a_token);
    }

    BoxDefInfo BoxAt(int a_index)
    {
        StoreLock lk;
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return {};
        }
        return g_boxes[a_index];
    }

    bool BoxWornAt(int a_index)
    {
        StoreLock lk;
        const std::uint32_t form = BoxTokenFormAt(a_index);
        if (form == 0) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && player->GetWornArmor(form) != nullptr;
    }

    std::uint32_t BoxTokenFormAt(int a_index)
    {
        StoreLock lk;
        if (a_index < 0 || a_index >= static_cast<int>(g_boxes.size())) {
            return 0;
        }
        return ResolveFormId(g_boxes[a_index].token);
    }

    std::vector<std::string> BoxContents(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx < 0 ? std::vector<std::string>{} : g_boxes[idx].contents;
    }

    bool IsTokenColonId(const std::string& a_id)
    {
        StoreLock lk;
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

    std::string PublishHolderId(int a_pubSlot)
    {
        return "publish:" + std::to_string(a_pubSlot);
    }

    std::string NpcPersistHolderId(int a_poolSlot)
    {
        return "npr:" + std::to_string(a_poolSlot);
    }

    bool IsSentinelHolder(const std::string& a_holder)
    {
        return a_holder == "persist" || a_holder.starts_with("publish:") ||
               a_holder.starts_with("npr:");
    }

    int PublishSlotOfHolder(const std::string& a_holder)
    {
        constexpr std::string_view kPrefix{ "publish:" };
        if (!a_holder.starts_with(kPrefix)) {
            return -1;
        }
        return std::atoi(a_holder.c_str() + kPrefix.size());
    }

    std::string ContentHolder(const std::string& a_content)
    {
        StoreLock lk;
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
        // Published costumes and NPC-persist assignments are OWNERS too (review
        // 2026-09-09 F03/F06/F08). Publish DELETES the source box, so before this
        // a published content answered "held by nobody" and the one-content-one-
        // owner invariant collapsed: it could be re-captured into a second box
        // (then either owner's unequip wiped the other's injection, and Unpublish
        // silently dropped contents), and every ROOT E side-map guard below
        // refused to store its hide/gender/morph settings.
        //
        // Same four sources as BuildHeldFormIds / BuildHolderLabels - the sweep,
        // the Recovery list and this admission gate must agree on "held".
        // Sentinels, like "persist": not a token colon-id, so the existing
        // `holder != "persist"` / `holder != token` rejections all fire correctly.
        if (const int slot = PublishedSlotHolding(a_content); slot >= 0) {
            return PublishHolderId(slot);
        }
        if (const int slot = NpcPersistSlotHolding(a_content); slot >= 0) {
            return NpcPersistHolderId(slot);
        }
        return {};
    }

    std::string CarrierModelForContent(const std::string& a_content)
    {
        StoreLock lk;
        const std::string holder = ContentHolder(a_content);
        if (holder.empty() || holder == "persist") {
            return {};  // persist rides the HDPT pool, not a token ARMA
        }
        auto* armo = ResolveArmo(holder);
        if (!armo || armo->armorAddons.empty()) {
            return {};
        }
        const auto* arma = armo->armorAddons.front();
        // Both sexes are repointed together (RepointCarrier), so either reads
        // the live revision; female is the one that revision-compare uses.
        const char* model = arma ? arma->bipedModels[RE::SEXES::kFemale].model.c_str() : nullptr;
        return model ? std::string(model) : std::string{};
    }

    bool AddBox(const std::string& a_label, const std::string& a_token, const std::string& a_content)
    {
        StoreLock lk;
        if (a_token.empty()) {
            return false;
        }
        // ROOT C/D border quarantine: canonicalize the ids, and refuse a CEF-own id
        // (a box token / pool part) or an id already held elsewhere as content. The
        // MCM pre-checks both (IsTokenPluginFile list filter + FindContentHolder),
        // but the native / preset / hand-edited-JSON routes reach here unchecked.
        std::string token = a_token;
        CanonicalizeColonId(token);
        tokenid::CanonicalizeCefColonId(token);  // v1.6.4: fold the plugin's casing
        // The token must be a record of a plugin that DEFINES box tokens, and it
        // must resolve to an ARMO there. The old test was "any CostumeFW* plugin",
        // which accepted 000800:CostumeFW_NPC.esp - a publish token - and built a
        // box on it, with both machineries then stamping the same record. It also
        // accepted the quest and the container in CostumeFW.esp, and ids naming
        // records that do not exist.
        if (const TokenState state = ClassifyBoxToken(token); state != TokenState::Resolved) {
            SKSE::log::warn("boxes: AddBox rejects token '{}' ({})", token,
                TokenStateReason(state));
            return false;
        }
        std::string content = a_content;
        if (!content.empty()) {
            CanonicalizeColonId(content);
            if (IsTokenColonId(content)) {
                SKSE::log::warn("boxes: AddBox rejects CEF-own id '{}' as content", content);
                return false;
            }
            // v1.3.2 capture gate: the native / preset / hand-edited-JSON routes
            // reach here unchecked (the pickers pre-filter, the UIs pre-gate).
            std::string why;
            if (!CanCaptureContent(content, &why)) {
                SKSE::log::warn("boxes: AddBox rejects '{}' - {}", content, why);
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
            BoxDefInfo fresh;
            fresh.boxId = NewBoxId();  // issued once; survives rename and publish
            fresh.label = a_label;
            fresh.token = token;
            g_boxes.push_back(std::move(fresh));
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

        // The settings are the record of what just happened. If they did not
        // reach disk, say so: the capture flow reads this result to decide
        // whether to move the physical item into the hidden store, and taking
        // the item while failing to record where it went is the one outcome
        // worth refusing (F05).
        const bool saved = WriteJson();
        SetTokenStats(g_boxes[idx]);  // write armor/weight onto the token now
        if (!saved) {
            SKSE::log::error(
                "boxes: AddBox label='{}' token='{}' content='{}' applied in memory but the "
                "settings could NOT be saved - reporting failure so nothing is moved into "
                "storage on the strength of it",
                a_label, token, content);
            return false;
        }
        SKSE::log::info("boxes: AddBox label='{}' token='{}' content='{}'", a_label, token, content);
        return true;
    }

    bool AdoptBoxId(const std::string& a_token, const std::string& a_boxId)
    {
        StoreLock lk;
        if (a_boxId.empty()) {
            return false;
        }
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        if (g_boxes[idx].boxId == a_boxId) {
            return true;
        }
        if (const int other = FindBoxById(a_boxId); other >= 0 && other != idx) {
            // One id, one box - the rule FindBoxById depends on. Keep the id
            // AddBox just issued rather than making two boxes answer to one.
            SKSE::log::warn("boxes: box '{}' cannot take id '{}' - box '{}' already carries it",
                a_token, a_boxId, g_boxes[other].token);
            return false;
        }
        SKSE::log::info("boxes: box '{}' takes back its id '{}' (was '{}')", a_token, a_boxId,
            g_boxes[idx].boxId);
        g_boxes[idx].boxId = a_boxId;
        return WriteJson();
    }

    bool RemoveBoxContent(const std::string& a_token, const std::string& a_content)
    {
        StoreLock lk;
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
        g_statEnchantOff.erase(a_content);  // and its item-data opt-outs
        g_statWeightOff.erase(a_content);
        g_statArmorOff.erase(a_content);
        g_hideShapes.erase(a_content);      // and its per-shape hide choices
        g_contentShapes.erase(a_content);   // and its cached shape list
        g_showRealBody.erase(a_content);    // and its show-real-body opt-in
        g_contentEnchants.erase(a_content);  // and its captured enchantment
        g_contentTemper.erase(a_content);    // and its captured temper multiplier
        WriteJson();
        SetTokenStats(box);  // recompute token armor/weight
        return true;
    }

    bool RemoveBox(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes.erase(g_boxes.begin() + idx);  // caller detaches each content node
        WriteJson();
        ResetTokenStats(a_token);  // freed token: clear its stat fields
        return true;
    }

    std::string ContentStatsSummary(const std::string& a_id)
    {
        // Captured values (pre-toggle) so the user can see what each switch is
        // worth: "Fortify Destruction 25 | Weight 8.0 | Armor 45". Armor rides
        // the item-card scale (see CardArmorOf); enchant/weight are raw.
        StoreLock lk;
        auto* armo = ResolveArmo(a_id);
        if (!armo) {
            return "(unresolved)";
        }
        std::string enchants;
        const auto appendEffect = [&enchants](RE::EffectSetting* a_mgef, float a_mag) {
            if (!a_mgef) {
                return;
            }
            auto* full = a_mgef->As<RE::TESFullName>();
            const char* nm = full ? full->GetFullName() : nullptr;
            if (!enchants.empty()) {
                enchants += ", ";
            }
            enchants += std::format("{} {:.0f}",
                (nm && *nm) ? EnsureUtf8(nm).c_str() : "effect", a_mag);
        };
        if (const auto snap = g_contentEnchants.find(a_id);
            snap != g_contentEnchants.end() && !snap->second.empty()) {
            for (const auto& e : snap->second) {
                appendEffect(ResolveMgef(e.mgef), e.magnitude);
            }
        } else if (auto* ench = armo->formEnchanting) {
            for (auto* e : ench->effects) {
                if (e) {
                    appendEffect(e->baseEffect, e->effectItem.magnitude);
                }
            }
        }
        // Armor at item-card scale x temper ratio = exactly what the real
        // piece's card shows, so the row is comparable at a glance.
        return std::format("{} | Weight {:.1f} | Armor {:.0f}",
            enchants.empty() ? "No enchantment" : enchants,
            armo->weight, CardArmorOf(armo) * TemperMultOf(a_id));
    }

    namespace
    {
        // Stamp the box label onto the token's inventory name (same volatile
        // form-edit lifetime as armorRating/keyword passthrough: re-applied on
        // every settings load). Empty label falls back to the ESP default.
        // The ESP name each token had before a label was ever stamped over it,
        // by FormID. Process-lifetime, exactly like the fullName edit it undoes.
        //
        // NO LONGER LOAD-BEARING (v1.6.4). This restore used to be the only thing
        // keeping a renamed token in the pool: TokenPool found a free box by its
        // ESP name, so renaming a box and then freeing it dropped the token out
        // of the pool until the next game start, and the rename feature ate boxes
        // (review 2026-09-09 F10). The name test could not simply go at the time,
        // because every CEF plugin passed IsTokenPluginFile and dropping it would
        // have swept in CostumeFW_NPC.esp's publish tokens.
        //
        // Membership is the CFW_BoxTokenMarker keyword now, which a rename cannot
        // touch, so F10 cannot recur and a miss here costs nothing but a stale
        // name in the inventory. Kept because that name SHOULD go back - not
        // because anything depends on it.
        std::unordered_map<std::uint32_t, std::string> g_tokenDefaultNames;  // fwd above

        void ApplyBoxLabelToToken(const BoxDefInfo& a_box)
        {
            auto* token = ResolveArmo(a_box.token);
            if (!token) {
                return;
            }
            if (!a_box.label.empty()) {
                // Remember the pre-stamp name once, before it is overwritten.
                // Every defined box passes here on each settings load, so a token
                // that can ever be freed has been recorded by then.
                if (const auto formId = token->GetFormID()) {
                    const char* current = token->GetFullName();
                    g_tokenDefaultNames.try_emplace(formId, current ? current : "");
                }
                token->fullName = a_box.label.c_str();
            }
        }

        // Undo the label stamp: put the ESP name back so TokenPool sees the token
        // again. No-op for a token that was never renamed.
        void RestoreTokenDefaultName(const std::string& a_token)
        {
            auto* token = ResolveArmo(a_token);
            if (!token) {
                return;
            }
            const auto it = g_tokenDefaultNames.find(token->GetFormID());
            if (it == g_tokenDefaultNames.end() || it->second.empty()) {
                return;
            }
            const std::string original = it->second;  // outlive the erase
            g_tokenDefaultNames.erase(it);
            token->fullName = original.c_str();
        }

        // Re-flow a changed item-data toggle through the existing contents-change
        // machinery: ability rebuild for the holder + token stats + re-apply.
        void ReapplyStatsForContent(const std::string& a_id)
        {
            const std::string holder = ContentHolder(a_id);
            if (holder.empty()) {
                return;
            }
            if (holder == "persist") {
                RebuildPersistAbility();
            } else if (const int pubSlot = PublishSlotOfHolder(holder); pubSlot >= 0) {
                // A published costume's stat passthrough is read LIVE on every
                // ability rebuild (FillContentEnchantSpell consults the toggles),
                // but nothing marked that ability stale outside unpublish and the
                // settings load - so the toggle did not take until a reload.
                // ApplyBoxAbilities below re-grants it through SyncNpcAbilities.
                // This restamps the token's armor/weight too: those are ARMO
                // fields, not ability effects, and the stale flag never reached
                // them (F03).
                RefreshPublishedStats(pubSlot);
            } else if (IsSentinelHolder(holder)) {
                // NPC persist: no player token to restat, and its contents carry
                // no synthesized stat ability of their own.
            } else {
                RebuildBoxAbility(holder);
                const int idx = FindBox(holder);
                if (idx >= 0) {
                    SetTokenStats(g_boxes[idx]);
                }
            }
            ApplyBoxAbilities();
        }

        bool SetStatToggle(std::unordered_set<std::string>& a_offSet,
            const char* a_what, const std::string& a_id, bool a_on)
        {
            StoreLock lk;
            if (a_id.empty()) {
                return false;
            }
            std::string id = a_id;
            CanonicalizeColonId(id);  // ROOT D
            if (!a_on) {
                if (ContentHolder(id).empty()) {  // ROOT E: no orphan entries
                    SKSE::log::warn("itemdata: '{}' is held by no box/persist - ignoring", id);
                    return false;
                }
                a_offSet.insert(id);
            } else {
                a_offSet.erase(id);
            }
            SKSE::log::info("itemdata: {} passthrough {} for '{}'", a_what,
                a_on ? "ON" : "OFF", id);
            WriteJson();
            ReapplyStatsForContent(id);
            return true;
        }
    }

    bool StatEnchantOn(const std::string& a_id)
    {
        StoreLock lk;
        return !g_statEnchantOff.contains(a_id);
    }

    bool SetStatEnchantOn(const std::string& a_id, bool a_on)
    {
        return SetStatToggle(g_statEnchantOff, "enchant", a_id, a_on);
    }

    bool StatWeightOn(const std::string& a_id)
    {
        StoreLock lk;
        return !g_statWeightOff.contains(a_id);
    }

    bool SetStatWeightOn(const std::string& a_id, bool a_on)
    {
        return SetStatToggle(g_statWeightOff, "weight", a_id, a_on);
    }

    bool StatArmorOn(const std::string& a_id)
    {
        StoreLock lk;
        return !g_statArmorOff.contains(a_id);
    }

    float ContentTemperMult(const std::string& a_id)
    {
        StoreLock lk;
        return TemperMultOf(a_id);
    }

    bool SetStatArmorOn(const std::string& a_id, bool a_on)
    {
        return SetStatToggle(g_statArmorOff, "armor", a_id, a_on);
    }

    bool SetBoxLabel(const std::string& a_token, const std::string& a_label)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].label = a_label;
        ApplyBoxLabelToToken(g_boxes[idx]);  // inventory name follows the label
        WriteJson();
        return true;
    }

    std::string PresetAssignedTo(const std::string& a_file)
    {
        StoreLock lk;
        if (a_file.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            if (b.preset == a_file) {
                return b.token;
            }
        }
        if (g_persistPreset == a_file) {
            return "persist";  // sentinel: held by the persist class
        }
        return {};
    }

    std::string PresetAssignedToName(const std::string& a_presetName)
    {
        StoreLock lk;
        if (a_presetName.empty()) {
            return {};
        }
        for (const auto& b : g_boxes) {
            if (b.presetName == a_presetName) {
                return b.token;
            }
        }
        if (g_persistPresetName == a_presetName) {
            return "persist";
        }
        return {};
    }

    std::string PersistPreset()
    {
        StoreLock lk;
        return g_persistPreset;
    }

    std::string PersistPresetName()
    {
        StoreLock lk;
        return g_persistPresetName;
    }

    std::vector<std::string> UnresolvedPresetNames()
    {
        StoreLock lk;
        std::vector<std::string> out;
        for (const auto& b : g_boxes) {
            if (b.preset.empty() && !b.presetName.empty()) {
                out.push_back(b.presetName);
            }
        }
        if (g_persistPreset.empty() && !g_persistPresetName.empty()) {
            out.push_back(g_persistPresetName);
        }
        return out;
    }

    void ResolvePresetFiles(const std::unordered_map<std::string, std::string>& a_nameToFile)
    {
        StoreLock lk;
        bool changed = false;
        const auto adopt = [&](const std::string& name, std::string& file) {
            if (!file.empty() || name.empty()) {
                return;
            }
            const auto it = a_nameToFile.find(name);
            if (it == a_nameToFile.end()) {
                // Missing or ambiguous name: preserve the contents and display
                // name, but do not claim a file in the exclusivity pool.
                SKSE::log::warn("preset: '{}' assignment unresolved (missing or ambiguous file); "
                                "current contents retained", name);
                return;
            }
            file = it->second;
            changed = true;
            SKSE::log::info("preset: migrated assignment '{}' -> {}", name, file);
        };
        for (auto& b : g_boxes) {
            adopt(b.presetName, b.preset);
        }
        adopt(g_persistPresetName, g_persistPreset);
        if (changed) {
            WriteJson();
        }
    }

    bool AssignPresetToPersist(const std::string& a_file, const std::string& a_presetName,
        const std::vector<std::string>& a_contents)
    {
        StoreLock lk;
        if (a_file.empty()) {
            return false;
        }
        // Shared exclusivity pool with boxes, keyed by FILE: reject if a box
        // already holds this preset.
        const std::string holder = PresetAssignedTo(a_file);
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
        g_persistPreset = a_file;
        g_persistPresetName = a_presetName;
        WriteJson();
        SKSE::log::info("preset: assigned '{}' to persist ({} content)",
            a_presetName, a_contents.size());
        return true;
    }

    bool ClearPersistPreset()
    {
        StoreLock lk;
        g_persistPreset.clear();  // contents remain (now manual)
        g_persistPresetName.clear();
        WriteJson();
        return true;
    }

    std::string BoxPreset(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].preset;
    }

    std::string BoxPresetName(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        return idx < 0 ? std::string{} : g_boxes[idx].presetName;
    }

    bool AssignPreset(const std::string& a_token, const std::string& a_file,
        const std::string& a_presetName, const std::vector<std::string>& a_contents)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0 || a_file.empty()) {
            return false;
        }
        // Exclusivity: a preset FILE may be assigned to only one box at a time.
        // Two files that happen to share a display name are separate presets and
        // do not collide (they did until v1.6.2.1, which made a same-name
        // re-export unassignable).
        const std::string holder = PresetAssignedTo(a_file);
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
        g_boxes[idx].preset = a_file;
        g_boxes[idx].presetName = a_presetName;
        g_boxes[idx].contents = a_contents;  // box mirrors the preset's contents
        WriteJson();
        SetTokenStats(g_boxes[idx]);  // recompute token armor/weight/keywords for new contents
        SKSE::log::info("preset: assigned '{}' to box {} ({} content)",
            a_presetName, a_token, a_contents.size());
        return true;
    }

    bool ClearPreset(const std::string& a_token)
    {
        StoreLock lk;
        const int idx = FindBox(a_token);
        if (idx < 0) {
            return false;
        }
        g_boxes[idx].preset.clear();  // contents remain (now manual)
        g_boxes[idx].presetName.clear();
        WriteJson();
        return true;
    }

    void SaveGlobalSettings()
    {
        WriteJson();
    }
}
