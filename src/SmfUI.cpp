#include "SmfUI.h"

#include "BodyMorph.h"
#include "BoxStore.h"
#include "Diag.h"
#include "Preset.h"
#include "PublishStore.h"
#include "SkinRebind.h"
#include "UiOps.h"

#include "RE/T/TESDataHandler.h"
#include "RE/C/CrosshairPickData.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// SKSE Menu Framework v3 consumer header (vendored; pinned commit recorded in
// SKSEMenuFramework.h.commit.txt). Every call resolves the SMF DLL's exports at
// runtime (GetProcAddress) - no link dependency, and with SMF absent every
// wrapper is a safe no-op. Render callbacks run on the game's UI-render path
// with the game frozen (SMF default FreezeTimeOnMenu) - reads follow the same
// "read-only snapshot tolerated" contract as the Papyrus VM thread
// (Papyrus.cpp:27), and every mutation is deferred via AddTask exactly like the
// CFW_Native mutators.
#pragma warning(push)
#pragma warning(disable : 4996)  // <codecvt> deprecation (header's UnicodeToUtf8)
#pragma warning(disable : 4099)  // ImGuiTextFilter struct/class mismatch
#pragma warning(disable : 5054)  // ImGui flag enums OR'd across enum types
#include "external/SKSEMenuFramework.h"
#pragma warning(pop)

// The v3 header exports the ImGui wrappers under "ImGuiMCP" (renamed so a
// consumer's own ImGui can't collide). CEF embeds no ImGui - alias it back.
namespace ImGui = ImGuiMCP;

namespace CostumeFW::SmfUI
{
    namespace
    {
        // --- render-thread-only UI state (tasks never touch these) -----------
        std::string s_selContent;         // content id shown in the detail block
        char s_invFilter[64] = "";        // "+ Add from inventory" name filter
        char s_catFilter[64] = "";        // Persist page: catalog row filter (X-SCROLL)
        char s_recFilter[64] = "";        // Recovery page: row filter (X-SCROLL)
        char s_hideSlots[64] = "";        // hide-when-worn slot list edit buffer
        std::string s_hideSlotsFor;       // which content the buffer was loaded for
        char s_exportName[64] = "";       // "Export as preset" name buffer
        char s_blkValue[128] = "";        // "Blocked" page: add-entry value buffer
        int s_blkKind = 0;                // "Blocked" page: 0=name, 1=plugin, 2=id
        std::string s_status;             // last guard/op feedback line
        // Optimistic Wear state: the engine's equip QUEUE does not process while
        // SMF pauses the game, so an honest per-frame GetWornArmor re-read
        // snapped the checkbox straight back (in-game 2026-07-12). Show the
        // user's intent until the live state catches up (menu close), then drop
        // the override. The MCM did the same via SetToggleOptionValue.
        std::unordered_map<std::string, bool> s_pendingWear;  // token -> desired

        // --- scrollable list regions (X-SCROLL) ------------------------------
        // Nexus report 2026-07-27 (recorded against v1.3.0): a persist catalog
        // that outgrew the page height had NO way to reach its newest entries -
        // the reporter had to DELETE old entries to get at the latest one. Every
        // unbounded list on these pages now lives in its own child region: the
        // list scrolls INSIDE a fixed frame while the page's controls (pickers,
        // filters, add rows, destructive buttons) stay pinned OUTSIDE it, so a
        // long list can never push them off-screen either (the X-UI1 failure
        // mode, one axis over).
        //
        // The region takes whatever vertical space the host page has left. SMF
        // owns the page window, so if it ever auto-sizes to content the avail
        // height is small or negative - the floor keeps the region usable (and
        // scrollable) in that case instead of collapsing to nothing.
        constexpr float kListMinHeight = 180.0f;

        // a_reserveBelow: height to leave for the controls drawn AFTER the list.
        // BeginChild's return is deliberately ignored - content submission stays
        // unconditional (a clipped region must not drop TreeNode/selection state),
        // and EndChild is mandatory either way.
        void BeginScrollList(const char* a_id, float a_reserveBelow = 0.0f)
        {
            float h = ImGui::GetContentRegionAvail().y - a_reserveBelow;
            if (h < kListMinHeight) {
                h = kListMinHeight;
            }
            ImGui::BeginChild(a_id, ImGui::ImVec2(0.0f, h), ImGui::ImGuiChildFlags_Border,
                ImGui::ImGuiWindowFlags_AlwaysVerticalScrollbar);
        }

        void EndScrollList()
        {
            ImGui::EndChild();
        }

        // Case-insensitive substring match; an empty needle passes everything.
        // Pairs with the scroll regions: scrolling makes a long list reachable,
        // filtering makes it navigable.
        bool RowMatches(const std::string& a_hay, const char* a_needle)
        {
            if (!a_needle || a_needle[0] == '\0') {
                return true;
            }
            const auto lower = [](std::string s) {
                for (auto& c : s) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return s;
            };
            return lower(a_hay).find(lower(a_needle)) != std::string::npos;
        }

        // Biped-slot display names. C++ twin of the MCM's SlotName (psc) while
        // the MCM lives; SMF is the owning copy once the MCM retires.
        const char* SlotName(int a_slot)
        {
            switch (a_slot) {
            case 30: return "Head";
            case 31: return "Hair (Wig)";
            case 32: return "Body";
            case 33: return "Hands";
            case 34: return "Forearms";
            case 35: return "Amulet";
            case 36: return "Ring";
            case 37: return "Feet";
            case 38: return "Calves";
            case 40: return "Tail";
            case 42: return "Circlet";
            case 43: return "Ears";
            case 44: return "Face/Eyes";
            case 45: return "Neck";
            case 46: return "Chest";
            case 47: return "Cloak";
            case 48: return "Belly/Garter";
            case 49: return "Skirts/Pants";
            case 52: return "Underwear";
            case 53: return "Leg Upper/R";
            case 54: return "Leg Lower/L";
            case 55: return "Face Mask";
            case 56: return "Bra/Chest2";
            case 57: return "Shoulder";
            case 58: return "Arm Upper/R";
            case 59: return "Arm Lower/L";
            case 60: return "Misc";
            default: return "?";
            }
        }

        // Human label for a FindContentHolder() result (MCM HolderLabel twin).
        std::string HolderLabel(const std::string& a_holder)
        {
            if (a_holder == "persist") {
                return "Persist";
            }
            return std::format("{} ({})", ItemDisplayName(a_holder), SlotName(TokenSlot(a_holder)));
        }

        // Capture one item into a box - guards on the render thread (immediate
        // feedback), then the MCM capture order on the main thread: register
        // FIRST -> enchant snapshot -> move item to store -> auto-wear token
        // (CostumeFW_MCM.psc box-capture flow, reviews A-2 / P1-3 / item 2).
        void QueueCapture(const std::string& a_token, const std::string& a_id)
        {
            const std::string holder = UiOps::FindContentHolder(a_id);
            if (!holder.empty() && holder != a_token) {
                s_status = std::format("already captured in {} - item not moved", HolderLabel(holder));
                return;
            }
            std::string why;
            if (!CanCaptureContent(a_id, &why)) {  // v1.3.2: blacklist + resolvability
                s_status = why;
                // X-UI2 (test run 2026-07-26): s_status alone reads as a SILENT
                // refusal - it renders at the TOP of the Boxes page while the
                // picker sits deep inside a box's tree node, so the reason is
                // off-screen at the moment of the click. The MCM shows a modal
                // here; match that with a notification.
                RE::DebugNotification(("CostumeFW: " + why).c_str());
                return;
            }
            s_status = UiOps::ContentHasScript(a_id)
                ? "note: item has attached scripts - script-driven behavior won't run under CEF"
                : "";
            const std::string token = a_token;
            const std::string id = a_id;
            SKSE::GetTaskInterface()->AddTask([token, id] {
                if (!UiOps::AddBox("", token, id)) {
                    RE::DebugNotification("CostumeFW: already in this box - item not moved");
                    return;
                }
                CaptureEnchant(id);       // before the move (worn instance data)
                CaptureItemToStore(id);
                auto* player = RE::PlayerCharacter::GetSingleton();
                const std::uint32_t tf = ResolveFormId(token);
                if (player && tf && !player->GetWornArmor(tf)) {
                    WearBoxToken(token, true);
                }
                RE::DebugNotification(("CostumeFW: captured " + ItemDisplayName(id)).c_str());
            });
        }

        // Persist capture - MCM persist-capture order: holder guard -> resolve
        // guard -> register (AddPersist = catalog + activate) -> enchant
        // snapshot -> move item to store.
        void QueueCapturePersist(const std::string& a_id)
        {
            const std::string holder = UiOps::FindContentHolder(a_id);
            if (!holder.empty() && holder != "persist") {
                s_status = std::format("already captured in {} - item not moved", HolderLabel(holder));
                return;
            }
            std::string why;
            if (!CanCaptureContent(a_id, &why)) {  // v1.3.2: blacklist + resolvability
                s_status = why;
                RE::DebugNotification(("CostumeFW: " + why).c_str());  // X-UI2
                return;
            }
            s_status = UiOps::ContentHasScript(a_id)
                ? "note: item has attached scripts - script-driven behavior won't run under CEF"
                : "";
            const std::string id = a_id;
            SKSE::GetTaskInterface()->AddTask([id] {
                if (!UiOps::AddPersist(id)) {
                    RE::DebugNotification("CostumeFW: already in persist - item not moved");
                    return;
                }
                CaptureEnchant(id);
                CaptureItemToStore(id);
                RE::DebugNotification(("CostumeFW: persist added " + ItemDisplayName(id)).c_str());
            });
        }

        // Box preset assign + A-4 custody: preset-dropped BOX contents return
        // STORE-ONLY (a preset lists references; fabricating would mint a free
        // item on every swap - MCM ReturnDroppedContents, aPersist=false).
        // Two presets can carry the same display name: a re-export writes
        // CEFP_X_1.json but leaves the json's "name" alone, and distributed
        // presets collide freely. The file is the identity, so a name shared by
        // more than one entry gets its file appended - otherwise the list shows
        // two identical rows and there is no way to tell which one you picked.
        std::string PresetPickLabel(const std::vector<Preset::PresetInfo>& a_list,
            const Preset::PresetInfo& a_p)
        {
            std::size_t same = 0;
            for (const auto& other : a_list) {
                if (other.name == a_p.name && ++same > 1) {
                    return std::format("{}  ({})", a_p.name, a_p.file);
                }
            }
            return a_p.name;
        }

        void QueueAssignPreset(const std::string& a_token, const std::string& a_file)
        {
            const std::string token = a_token;
            const std::string file = a_file;
            const auto oldContents = BoxContents(token);
            SKSE::GetTaskInterface()->AddTask([token, file, oldContents] {
                if (!UiOps::AssignPreset(token, file)) {
                    RE::DebugNotification("CostumeFW: preset not assigned - already used elsewhere, "
                                          "or items captured elsewhere (see log)");
                    return;
                }
                const auto now = BoxContents(token);
                for (const auto& c : oldContents) {
                    if (std::find(now.begin(), now.end(), c) == now.end()) {
                        ReturnStoredItem(c, false);
                    }
                }
            });
        }

        // Persist preset assign: dropped entries fabricate ONLY if they were
        // ACTIVE here pre-assign (MCM ReturnDroppedContents, aPersist=true).
        void QueueAssignPersistPreset(const std::string& a_file)
        {
            const std::string file = a_file;
            const auto oldContents = PersistContents();
            const auto preActive = PersistActiveIds();
            SKSE::GetTaskInterface()->AddTask([file, oldContents, preActive] {
                if (!UiOps::AssignPersistPreset(file)) {
                    RE::DebugNotification("CostumeFW: preset not assigned - already used by a box, or "
                                          "it contains items captured in a box (see log)");
                    return;
                }
                const auto now = PersistContents();
                for (const auto& c : oldContents) {
                    if (std::find(now.begin(), now.end(), c) == now.end()) {
                        const bool wasActive =
                            std::find(preActive.begin(), preActive.end(), c) != preActive.end();
                        ReturnStoredItem(c, wasActive);
                    }
                }
            });
        }

        // Selected-content editor (right-panel twin of the MCM box page).
        // "Item data" fold: per-content passthrough toggles. a_full = box
        // contents (all three channels); persist gets enchant only - weight and
        // armor ride the TOKEN, which the persist class does not have.
        void RenderItemDataFold(const std::string& a_id, const std::string& a_uiKey, bool a_full)
        {
            if (!ImGui::CollapsingHeader(std::format("Item data##itd{}", a_uiKey).c_str())) {
                return;
            }
            ImGui::TextDisabled("%s", ContentStatsSummary(a_id).c_str());
            bool en = StatEnchantOn(a_id);
            if (ImGui::Checkbox(std::format("Enchantments##ien{}", a_uiKey).c_str(), &en)) {
                const std::string id = a_id;
                const bool v = en;
                SKSE::GetTaskInterface()->AddTask([id, v] { SetStatEnchantOn(id, v); });
            }
            if (a_full) {
                bool wt = StatWeightOn(a_id);
                if (ImGui::Checkbox(std::format("Weight##iwt{}", a_uiKey).c_str(), &wt)) {
                    const std::string id = a_id;
                    const bool v = wt;
                    SKSE::GetTaskInterface()->AddTask([id, v] { SetStatWeightOn(id, v); });
                }
                bool ar = StatArmorOn(a_id);
                if (ImGui::Checkbox(std::format("Armor rating##iar{}", a_uiKey).c_str(), &ar)) {
                    const std::string id = a_id;
                    const bool v = ar;
                    SKSE::GetTaskInterface()->AddTask([id, v] { SetStatArmorOn(id, v); });
                }
            } else {
                ImGui::TextDisabled(
                    "Weight/armor don't apply: persist has no token to carry them.");
            }
        }

        void RenderContentDetail(const std::string& a_token, int a_slot, const std::string& a_id)
        {
            ImGui::SeparatorText(ItemDisplayName(a_id).c_str());

            bool morph = BodyMorphOn(a_id);
            if (ImGui::Checkbox(std::format("Body morph (BodySlide mesh)##m{}", a_slot).c_str(), &morph)) {
                UiOps::SetBodyMorph(a_id, morph);
                if (morph) {
                    s_status = "Body morph ON - only for body-conforming meshes; keep OFF for hair/jewelry (memory cost)";
                }
            }

            static const char* kGender[3] = { "Auto (player gender)", "Force Male NIF", "Force Female NIF" };
            int gm = GenderModeFor(a_id);
            if (gm < 0 || gm > 2) {
                gm = 0;
            }
            if (ImGui::BeginCombo(std::format("Body (forced NIF)##g{}", a_slot).c_str(), kGender[gm])) {
                for (int t = 0; t < 3; ++t) {
                    if (ImGui::Selectable(kGender[t], t == gm) && t != gm) {
                        UiOps::SetContentGender(a_id, t);  // re-resolves + re-injects
                    }
                }
                ImGui::EndCombo();
            }

            if (s_hideSlotsFor != a_id) {  // (re)load the edit buffer on selection change
                std::snprintf(s_hideSlots, sizeof(s_hideSlots), "%s", UiOps::GetHideSlotsStr(a_id).c_str());
                s_hideSlotsFor = a_id;
            }
            ImGui::InputText(std::format("Hide when worn (slots)##hs{}", a_slot).c_str(),
                s_hideSlots, sizeof(s_hideSlots));
            ImGui::SameLine();
            if (ImGui::Button(std::format("Apply##hsb{}", a_slot).c_str())) {
                UiOps::SetHideSlotsStr(a_id, s_hideSlots);
                s_status = "hide-when-worn rule updated";
            }

            bool rb = ShowRealBodyOn(a_id);
            if (ImGui::Checkbox(std::format("Show real body under##rb{}", a_slot).c_str(), &rb)) {
                UiOps::SetShowRealBody(a_id, rb);
                if (rb) {
                    s_status = "Show real body ON - pair with Hide shapes on the costume's body (doubles if your body already shows)";
                }
            }

            RenderItemDataFold(a_id, std::format("b{}", a_slot), true);

            if (ImGui::Button(std::format("Remove from box##rm{}", a_slot).c_str())) {
                const std::string token = a_token;
                const std::string id = a_id;
                SKSE::GetTaskInterface()->AddTask([token, id] {
                    UiOps::RemoveBoxContent(token, id, true);  // returns the captured item (store-first)
                });
                s_selContent.clear();
            }

            const auto shapes = ContentShapesFor(a_id);
            ImGui::SeparatorText(std::format("Hide shapes ({})", shapes.size()).c_str());
            if (shapes.empty()) {
                if (ImGui::Button(std::format("Scan shapes##sc{}", a_slot).c_str())) {
                    UiOps::ScanContentShapes(a_id);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(wear the box once, or scan)");
            } else {
                for (const auto& [sname, sslot] : shapes) {
                    bool hidden = IsHideShape(a_id, sname);
                    const std::string lbl = (sslot >= 0)
                        ? std::format("{} [slot {}]##sh{}{}", sname, sslot, a_slot, sname)
                        : std::format("{}##sh{}{}", sname, a_slot, sname);
                    if (ImGui::Checkbox(lbl.c_str(), &hidden)) {
                        UiOps::SetHideShape(a_id, sname, hidden);  // queues a re-inject
                    }
                }
            }
        }

        void __stdcall RenderMain()
        {
            bool on = CefEnabled();
            if (ImGui::Checkbox("Enable CEF##cfw", &on)) {
                const bool want = on;
                SKSE::GetTaskInterface()->AddTask([want] {
                    SetCefEnabled(want);  // def + json
                    Reconcile();          // master off hides everything; on re-shows
                    ApplyBoxAbilities();
                    // Persist head carriers follow the master switch (same trio
                    // as the Papyrus SetEnabled native - keep them in lockstep).
                    ApplyCarrierOverrides(false);
                });
            }
            if (ImGui::Button("Reload settings from disk##cfw")) {
                SKSE::GetTaskInterface()->AddTask([] { ReloadSettingsFromDisk(); });
            }

            ImGui::SeparatorText("Dependencies");
            auto* dh = RE::TESDataHandler::GetSingleton();
            const bool espOk = dh && dh->LookupModByName("CostumeFW.esp") != nullptr;
            ImGui::Text("RaceMenu / skee (body morph): %s",
                BodyMorph::Available() ? "OK" : "MISSING");
            ImGui::Text("CostumeFW.esp: %s", espOk ? "OK" : "MISSING");

            ImGui::SeparatorText("Maintenance");
            if (ImGui::Button("Prepare for uninstall##cfwun")) {
                ImGui::OpenPopup("Prepare for uninstall?###cfwunp");
            }
            ImGui::SetNextWindowSize(ImGui::ImVec2(460, 0), ImGui::ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Prepare for uninstall?###cfwunp")) {
                ImGui::TextWrapped(
                    "Return all captured items, remove all box tokens, detach everything and "
                    "disable CEF (re-enable from this page to restore). Do this before removing "
                    "the mod.");
                if (ImGui::Button("Proceed")) {
                    SKSE::GetTaskInterface()->AddTask([] {
                        // MCM UninstallCleanup twin: box contents + ACTIVE persist
                        // return with the fabricate fallback; defs stay in the json.
                        const int n = BoxCount();
                        for (int i = 0; i < n; ++i) {
                            const BoxDefInfo b = BoxAt(i);
                            for (const auto& c : b.contents) {
                                ReturnStoredItem(c, true);
                            }
                            GiveOrRemoveToken(b.token, false);  // unequip + remove all copies
                        }
                        for (const auto& id : PersistActiveIds()) {
                            ReturnStoredItem(id, true);
                        }
                        UninstallNpcCleanup();  // NPC arm: recall published + strip persist
                        DetachAll();
                        SetCefEnabled(false);  // persist the OFF state (no re-apply on reload)
                        Reconcile();
                        ApplyBoxAbilities();
                        ApplyCarrierOverrides(false);
                        RE::DebugNotification(
                            "CostumeFW: items returned, tokens removed, CEF disabled - safe to uninstall");
                    });
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::SeparatorText("About");
            const auto* decl = SKSE::PluginDeclaration::GetSingleton();
            ImGui::Text("Costume Expansion FW %s",
                decl ? decl->GetVersion().string().c_str() : "?");
            ImGui::TextDisabled(
                "SKSE Menu Framework UI (phase 2b: full management)."
                " The SkyUI MCM remains only for the transition;"
                " 'cef' console commands stay available.");
        }

        void __stdcall RenderBoxes()
        {
            // + New box (free slot picker)
            if (ImGui::BeginCombo("+ New box##cfwnew", "(pick a free slot)")) {
                for (const auto& t : FreeTokens()) {
                    const int slot = TokenSlot(t);
                    const std::string lbl =
                        std::format("{}: {}##nb{}", ItemDisplayName(t), SlotName(slot), t);
                    if (ImGui::Selectable(lbl.c_str())) {
                        UiOps::AddBox(ItemDisplayName(t), t, "");
                        s_status = std::format("box created for {}", SlotName(slot));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("Inventory filter##cfwif", s_invFilter, sizeof(s_invFilter));
            if (!s_status.empty()) {
                ImGui::TextWrapped("%s", s_status.c_str());
            }
            ImGui::Separator();

            BeginScrollList("##cfwboxlist");
            const int n = BoxCount();
            for (int i = 0; i < n; ++i) {
                const BoxDefInfo b = BoxAt(i);
                const std::string token = b.token;
                const int slot = TokenSlot(token);
                const bool worn = BoxWornAt(i);
                // "###" id keyed by SLOT (stable): a deletion shifting box indices
                // must not re-target open tree nodes (the MCM's ROOT I analog).
                const std::string headTitle = b.label.empty() ? SlotName(slot) : b.label;
                const std::string header = std::format("Box {}: {} - {} item(s){}###cfwbox{}",
                    slot, headTitle, b.contents.size(), worn ? "  [WORN]" : "", slot);
                if (!ImGui::TreeNode(header.c_str())) {
                    continue;
                }

                bool w = worn;
                if (auto pend = s_pendingWear.find(token); pend != s_pendingWear.end()) {
                    if (pend->second == worn) {
                        s_pendingWear.erase(pend);  // live state caught up
                    } else {
                        w = pend->second;           // show the queued intent
                    }
                }
                if (ImGui::Checkbox(std::format("Wear (show contents)##w{}", slot).c_str(), &w)) {
                    const bool want = w;
                    s_pendingWear[token] = want;
                    SKSE::GetTaskInterface()->AddTask([token, want] { WearBoxToken(token, want); });
                }
                ImGui::SameLine();
                bool dist = b.enabled;
                if (ImGui::Checkbox(std::format("Distribute token##d{}", slot).c_str(), &dist)) {
                    const bool want = dist;
                    UiOps::SetBoxEnabled(token, want);  // flag + json (sync, like the MCM)
                    SKSE::GetTaskInterface()->AddTask([token, want] { GiveOrRemoveToken(token, want); });
                }

                static const char* kTypes[3] = { "Clothing", "Light Armor", "Heavy Armor" };
                int at = b.armorType;
                if (at < 0 || at > 2) {
                    at = 0;
                }
                if (ImGui::BeginCombo(std::format("Armor type##at{}", slot).c_str(), kTypes[at])) {
                    for (int t = 0; t < 3; ++t) {
                        if (ImGui::Selectable(kTypes[t], t == at) && t != at) {
                            UiOps::SetBoxArmorType(token, t);
                            if (worn) {  // re-equip so the armor-class change lands (MCM twin)
                                SKSE::GetTaskInterface()->AddTask([token] {
                                    WearBoxToken(token, false);
                                    WearBoxToken(token, true);
                                });
                            }
                        }
                    }
                    ImGui::EndCombo();
                }

                const std::string preset = BoxPreset(token);          // file = identity
                const std::string presetName = BoxPresetName(token);  // what to show
                if (ImGui::BeginCombo(std::format("Preset##pr{}", slot).c_str(),
                        presetName.empty() ? "(manual)" : presetName.c_str())) {
                    // Snapshot on combo OPEN, not per frame (List() scans the
                    // presets folder; a combo renders every frame while open).
                    static std::vector<Preset::PresetInfo> s_presets;
                    if (ImGui::IsWindowAppearing()) {
                        s_presets = Preset::List();
                    }
                    // Keyed on the NAME, not the file: an assignment whose
                    // preset file was deleted keeps the name and no file, and
                    // it still has to be clearable back to manual.
                    if (ImGui::Selectable("(manual)", presetName.empty()) &&
                        !presetName.empty()) {
                        UiOps::ClearPreset(token);
                    }
                    for (const auto& p : s_presets) {
                        if (ImGui::Selectable(
                                std::format("{}##pf{}", PresetPickLabel(s_presets, p), p.file)
                                    .c_str(),
                                p.file == preset) &&
                            p.file != preset) {
                            QueueAssignPreset(token, p.file);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::InputText(std::format("##expn{}", slot).c_str(), s_exportName, sizeof(s_exportName));
                ImGui::SameLine();
                if (ImGui::Button(std::format("Export as preset##expb{}", slot).c_str()) &&
                    s_exportName[0] != '\0') {
                    const std::string file = UiOps::ExportPreset(token, s_exportName);
                    s_status = file.empty() ? "export failed (see log)" : ("exported " + file);
                }

                // Rename (Nexus request): the label follows into the token's
                // inventory name, so the item reads as the outfit it holds.
                static char s_boxLabel[64] = {};
                static std::string s_boxLabelFor;
                if (s_boxLabelFor != token) {
                    std::snprintf(s_boxLabel, sizeof(s_boxLabel), "%s", b.label.c_str());
                    s_boxLabelFor = token;
                }
                ImGui::InputText(std::format("##bxl{}", slot).c_str(), s_boxLabel,
                    sizeof(s_boxLabel));
                ImGui::SameLine();
                if (ImGui::Button(std::format("Rename##bxlb{}", slot).c_str()) &&
                    s_boxLabel[0] != '\0') {
                    const std::string label = s_boxLabel;
                    SKSE::GetTaskInterface()->AddTask([token, label] {
                        SetBoxLabel(token, label);
                    });
                    s_status = "box renamed (inventory name follows)";
                }

                ImGui::Text("Stats: %s", BoxStatsSummary(i).c_str());

                if (ImGui::BeginCombo(std::format("+ Add worn item##aw{}", slot).c_str(), "(pick)")) {
                    static std::vector<WornItem> s_worn;  // snapshot on combo open
                    if (ImGui::IsWindowAppearing()) {
                        s_worn = WornArmors();
                    }
                    for (const auto& wi : s_worn) {
                        if (ImGui::Selectable(std::format("{}##aw{}{}", wi.name, slot, wi.id).c_str())) {
                            QueueCapture(token, wi.id);
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::BeginCombo(std::format("+ Add from inventory##ai{}", slot).c_str(), "(pick)")) {
                    static std::vector<WornItem> s_inv;  // snapshot on combo open
                    if (ImGui::IsWindowAppearing()) {
                        s_inv = InventoryArmors(s_invFilter);
                    }
                    for (const auto& wi : s_inv) {
                        if (ImGui::Selectable(std::format("{}##ai{}{}", wi.name, slot, wi.id).c_str())) {
                            QueueCapture(token, wi.id);
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SeparatorText(std::format("Contents ({})", b.contents.size()).c_str());
                for (const auto& c : b.contents) {
                    const bool sel = (c == s_selContent);
                    if (ImGui::Selectable(
                            std::format("{}##sel{}{}", ItemDisplayName(c), slot, c).c_str(), sel)) {
                        s_selContent = c;
                    }
                }
                if (!s_selContent.empty() &&
                    std::find(b.contents.begin(), b.contents.end(), s_selContent) != b.contents.end()) {
                    RenderContentDetail(token, slot, s_selContent);
                }

                ImGui::Spacing();
                const bool npcReady = NpcEspLoaded();
                const auto publishPopup = std::format("Publish box for NPC?###pubp{}", slot);
                ImGui::BeginDisabled(!npcReady);
                if (ImGui::Button(std::format("Publish for NPC##pub{}", slot).c_str()))
                    ImGui::OpenPopup(publishPopup.c_str());
                ImGui::EndDisabled();
                if (!npcReady && ImGui::IsItemHovered(ImGui::ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Requires the CostumeFW_NPC.esp add-on.");
                ImGui::SetNextWindowSize(ImGui::ImVec2(460, 0), ImGui::ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal(publishPopup.c_str())) {
                    ImGui::TextWrapped(
                        "Freeze this box's contents and settings into a distributable NPC token? "
                        "The source box will be removed and its normal token slot freed.");
                    if (ImGui::Button("Publish")) {
                        const int index = i;
                        SKSE::GetTaskInterface()->AddTask([index] { PublishBox(index); });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::Spacing();
                const std::string popupId = std::format("Delete box?###delp{}", slot);
                if (ImGui::Button(std::format("Delete box##delb{}", slot).c_str())) {
                    ImGui::OpenPopup(popupId.c_str());
                }
                // Width pinned: TextWrapped inside an auto-sizing modal wraps at a
                // tiny default width -> a skinny vertical window (in-game 2026-07-12).
                ImGui::SetNextWindowSize(ImGui::ImVec2(460, 0), ImGui::ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal(popupId.c_str())) {
                    ImGui::TextWrapped(
                        "Delete this box? Captured items are returned to you; the slot token frees up.");
                    if (ImGui::Button("Delete")) {
                        SKSE::GetTaskInterface()->AddTask([token] {
                            WearBoxToken(token, false);
                            UiOps::RemoveBox(token, true);  // returns each captured content (store-first)
                        });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::TreePop();
            }
            EndScrollList();
        }

        void __stdcall RenderPersist()
        {
            if (ImGui::BeginCombo("+ Add worn item##pw", "(pick)")) {
                static std::vector<WornItem> s_worn;
                if (ImGui::IsWindowAppearing()) {
                    s_worn = WornArmors();
                }
                for (const auto& wi : s_worn) {
                    if (ImGui::Selectable(std::format("{}##pw{}", wi.name, wi.id).c_str())) {
                        QueueCapturePersist(wi.id);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("+ Add from inventory##pi", "(pick)")) {
                static std::vector<WornItem> s_inv;
                if (ImGui::IsWindowAppearing()) {
                    s_inv = InventoryArmors(s_invFilter);
                }
                for (const auto& wi : s_inv) {
                    if (ImGui::Selectable(std::format("{}##pi{}", wi.name, wi.id).c_str())) {
                        QueueCapturePersist(wi.id);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("Inventory filter##pif", s_invFilter, sizeof(s_invFilter));

            const std::string ppreset = PersistPreset();          // file = identity
            const std::string ppresetName = PersistPresetName();  // what to show
            if (ImGui::BeginCombo("Preset##ppr",
                    ppresetName.empty() ? "(manual)" : ppresetName.c_str())) {
                static std::vector<Preset::PresetInfo> s_presets;
                if (ImGui::IsWindowAppearing()) {
                    s_presets = Preset::List();
                }
                if (ImGui::Selectable("(manual)", ppresetName.empty()) && !ppresetName.empty()) {
                    UiOps::ClearPersistPreset();
                }
                for (const auto& p : s_presets) {
                    if (ImGui::Selectable(
                            std::format("{}##ppf{}", PresetPickLabel(s_presets, p), p.file).c_str(),
                            p.file == ppreset) &&
                        p.file != ppreset) {
                        QueueAssignPersistPreset(p.file);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("##pexn", s_exportName, sizeof(s_exportName));
            ImGui::SameLine();
            if (ImGui::Button("Export as preset##pexb") && s_exportName[0] != '\0') {
                const std::string file = UiOps::ExportPersist(s_exportName);
                s_status = file.empty() ? "export failed (see log)" : ("exported " + file);
            }
            if (!s_status.empty()) {
                ImGui::TextWrapped("%s", s_status.c_str());
            }

            const auto contents = PersistContents();
            const auto actives = PersistActiveIds();
            const auto isActive = [&](const std::string& id) {
                return std::find(actives.begin(), actives.end(), id) != actives.end();
            };

            // Active on this save but no longer in the shared catalog. Computed
            // BEFORE the scroll region so it can ride inside it (one scrollable
            // area for everything list-shaped on this page).
            std::vector<std::string> uncat;
            for (const auto& id : actives) {
                if (std::find(contents.begin(), contents.end(), id) == contents.end()) {
                    uncat.push_back(id);
                }
            }

            // X-SCROLL: the catalog is the list that grows without bound (one row
            // per captured costume piece, shared across saves). Filter + scroll.
            ImGui::InputText("Catalog filter##pcf", s_catFilter, sizeof(s_catFilter));
            // One name resolve per entry per frame (the header, the filter and the
            // count all read it) - this callback runs every frame.
            std::vector<std::string> names;
            names.reserve(contents.size());
            std::size_t shown = 0;
            for (const auto& id : contents) {
                names.push_back(ItemDisplayName(id));
                if (RowMatches(names.back(), s_catFilter)) {
                    ++shown;
                }
            }
            ImGui::SeparatorText(
                (shown == contents.size()
                        ? std::format("Catalog ({}) - shared across saves", contents.size())
                        : std::format("Catalog ({} of {} shown) - shared across saves", shown,
                              contents.size()))
                    .c_str());

            // Reserve the row "Remove all persist" occupies below the region, so a
            // long catalog can never push that button (or the uncataloged rows)
            // out of reach - the exact failure the report describes.
            BeginScrollList("##pclist",
                contents.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing());
            for (std::size_t i = 0; i < contents.size(); ++i) {
                const std::string& id = contents[i];
                if (!RowMatches(names[i], s_catFilter)) {
                    continue;
                }
                const bool act = isActive(id);
                const std::string header =
                    std::format("{}{}###pc{}", names[i], act ? "  [ON]" : "", id);
                if (!ImGui::TreeNode(header.c_str())) {
                    continue;
                }
                bool a = act;
                if (ImGui::Checkbox(std::format("Active on this save##pa{}", id).c_str(), &a)) {
                    // Visual-only toggle (M2): items move only on capture/remove.
                    if (!UiOps::SetPersistActive(id, a)) {
                        s_status = "could not change the active state (see log)";
                    }
                }
                bool morph = BodyMorphOn(id);
                if (ImGui::Checkbox(std::format("Body morph (BodySlide mesh)##pm{}", id).c_str(),
                        &morph)) {
                    UiOps::SetBodyMorph(id, morph);
                }
                static const char* kGender[3] = { "Auto (player gender)", "Force Male NIF",
                    "Force Female NIF" };
                int gm = GenderModeFor(id);
                if (gm < 0 || gm > 2) {
                    gm = 0;
                }
                if (ImGui::BeginCombo(std::format("Body (forced NIF)##pg{}", id).c_str(),
                        kGender[gm])) {
                    for (int t = 0; t < 3; ++t) {
                        if (ImGui::Selectable(kGender[t], t == gm) && t != gm) {
                            UiOps::SetContentGender(id, t);
                        }
                    }
                    ImGui::EndCombo();
                }
                if (s_hideSlotsFor != id) {
                    std::snprintf(s_hideSlots, sizeof(s_hideSlots), "%s",
                        UiOps::GetHideSlotsStr(id).c_str());
                    s_hideSlotsFor = id;
                }
                ImGui::InputText(std::format("Hide when worn (slots)##ph{}", id).c_str(),
                    s_hideSlots, sizeof(s_hideSlots));
                ImGui::SameLine();
                if (ImGui::Button(std::format("Apply##phb{}", id).c_str())) {
                    UiOps::SetHideSlotsStr(id, s_hideSlots);
                }
                RenderItemDataFold(id, std::format("p{}", id), false);
                if (ImGui::Button(std::format("Remove from catalog##prm{}", id).c_str())) {
                    // MCM custody (P1 2026-07-07): fabricate fallback only for an
                    // entry THIS save displays; pair the return with deactivation.
                    const std::string rid = id;
                    const bool wasActive = act;
                    SKSE::GetTaskInterface()->AddTask([rid, wasActive] {
                        ReturnStoredItem(rid, wasActive);
                        if (wasActive) {
                            UiOps::SetPersistActive(rid, false);
                        }
                        UiOps::RemovePersist(rid, false);
                    });
                }
                ImGui::TreePop();
            }
            if (shown == 0 && !contents.empty()) {
                ImGui::TextDisabled("(no catalog entry matches the filter)");
            }
            if (!uncat.empty()) {
                ImGui::SeparatorText(
                    std::format("Active but not in catalog ({})", uncat.size()).c_str());
                for (const auto& id : uncat) {
                    ImGui::Text("%s", ItemDisplayName(id).c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(std::format("Deactivate##pu{}", id).c_str())) {
                        const std::string rid = id;
                        SKSE::GetTaskInterface()->AddTask([rid] {
                            // Stale-row guard (review round 4): return only if
                            // still active at execution time.
                            const auto now = PersistActiveIds();
                            if (std::find(now.begin(), now.end(), rid) != now.end()) {
                                ReturnStoredItem(rid, true);
                                UiOps::SetPersistActive(rid, false);
                            }
                        });
                    }
                }
            }
            EndScrollList();

            // Pinned below the scroll region (see the reserve above).
            if (!contents.empty() && ImGui::Button("Remove all persist##prall")) {
                SKSE::GetTaskInterface()->AddTask([] {
                    const auto all = PersistContents();
                    const auto act = PersistActiveIds();
                    for (const auto& id : all) {
                        // Return only what THIS save shows (P1-4).
                        if (std::find(act.begin(), act.end(), id) != act.end()) {
                            ReturnStoredItem(id, true);
                            UiOps::SetPersistActive(id, false);
                        }
                        UiOps::RemovePersist(id, false);
                    }
                    RE::DebugNotification("CostumeFW: removed all persist");
                });
            }
        }

        void __stdcall RenderPresets()
        {
            static std::vector<Preset::PresetInfo> s_list;
            static bool s_loaded = false;
            if (ImGui::Button("Refresh##cfwprl") || !s_loaded) {
                s_list = Preset::List();
                s_loaded = true;
            }
            ImGui::SeparatorText(std::format("Presets ({})", s_list.size()).c_str());
            if (s_list.empty()) {
                ImGui::TextDisabled("(none - export a box as a preset, or install a CEFP_*.json)");
                return;
            }
            BeginScrollList("##cfwpresetlist");
            for (const auto& p : s_list) {
                const std::string assigned = PresetAssignedTo(p.file);
                const std::string stat = assigned.empty()
                    ? "free"
                    : (assigned == "persist"
                              ? std::string("assigned: Persist")
                              : std::format("assigned: Box {}", TokenSlot(assigned)));
                if (!ImGui::TreeNode(std::format("{} - {}###pp{}", p.name, stat, p.file).c_str())) {
                    continue;
                }
                if (!p.description.empty()) {
                    ImGui::TextWrapped("%s", p.description.c_str());
                }
                if (!p.author.empty()) {
                    ImGui::TextDisabled("by %s", p.author.c_str());
                }
                ImGui::Text("File: %s | %d item(s)", p.file.c_str(),
                    static_cast<int>(p.contents.size()));
                if (ImGui::BeginCombo(std::format("Assign to box##ab{}", p.file).c_str(), "(pick)")) {
                    const int n = BoxCount();
                    for (int i = 0; i < n; ++i) {
                        const BoxDefInfo b = BoxAt(i);
                        const int slot = TokenSlot(b.token);
                        if (ImGui::Selectable(
                                std::format("Box {}: {}##abx{}{}", slot, SlotName(slot), p.file, slot)
                                    .c_str())) {
                            QueueAssignPreset(b.token, p.file);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TreePop();
            }
            EndScrollList();
        }

        // H3 (NPC_AUDIT_2026-08-03): the SMF render callback runs on the D3D
        // present thread, and the old code DEEP-COPIED the live publish/npr
        // stores (vector<shared_ptr> + strings + maps) every frame while the
        // main-thread task pump erases/push_backs them - a vector reallocation
        // or string destruction mid-copy is a use-after-free. The snapshots are
        // now taken ON THE MAIN THREAD via a queued task and handed to the
        // renderer through a mutex-guarded cache (refreshed every ~500ms while
        // the page is visible). Buttons already mutate via AddTask, so a
        // half-second-stale VIEW is harmless.
        struct NpcPageCache
        {
            std::vector<PubSnapshot> published;
            std::vector<PubBindingInfo> bindings;
            std::vector<NprAssignmentInfo> assignments;
            // Parallel to assignments (same index): resolved actor name and
            // whether the carrier is currently worn. Resolved in the refresh
            // task (main thread) so the renderer never touches live forms.
            std::vector<std::string> assignmentNames;
            std::vector<std::uint8_t> assignmentWorn;
            // Display names for every content id on the page (assignments +
            // catalog), same main-thread rule.
            std::unordered_map<std::string, std::string> names;
            std::size_t unresolvedNpr{ 0 };
            std::vector<std::string> catalog;
            std::unordered_map<int, bool> hidden;
            std::size_t injected{ 0 };
            int maxInjected{ 8 };
            std::size_t dormant{ 0 };
            bool espLoaded{ false };
            bool valid{ false };
        };
        // Per-assignment checkbox edit buffers (render-thread only). baseline =
        // the assignment contents the checks were initialized from; when the
        // cache shows different contents and the user has no pending edits, the
        // buffer re-syncs. dirty = user changed something; Apply/Revert clear it.
        struct NprEditBuffer
        {
            std::string baseline;
            std::unordered_map<std::string, bool> checks;
            bool dirty{ false };
        };
        std::unordered_map<int, NprEditBuffer> g_nprEdit;
        std::unordered_map<std::string, bool> g_newAssignChecks;
        char g_npcInvFilter[64] = {};

        // "+ Add from inventory" (reference-only): picks a carried ARMO's colon
        // id straight into a checklist. The item is NOT captured and never
        // leaves the player's inventory - npc-persist bakes from the ARMA
        // records, so an id reference is all an assignment needs (same
        // semantics as 'cef npcpersist add'). Snapshot on combo open, the
        // accepted pattern for inventory enumeration in this UI.
        template <class TOnPick>
        void NpcInventoryAddCombo(const std::string& a_uiKey, TOnPick&& a_onPick)
        {
            if (!ImGui::BeginCombo(
                    std::format("+ Add from inventory##npinv{}", a_uiKey).c_str(), "(pick)")) {
                return;
            }
            static std::vector<WornItem> s_inv;
            if (ImGui::IsWindowAppearing()) {
                s_inv = InventoryArmors(g_npcInvFilter);
            }
            for (const auto& wi : s_inv) {
                if (ImGui::Selectable(
                        std::format("{}##npinv{}_{}", wi.name, a_uiKey, wi.id).c_str())) {
                    a_onPick(wi.id);
                }
            }
            ImGui::EndCombo();
        }

        NpcPageCache g_npcPageCache;
        std::mutex g_npcPageCacheMutex;
        std::atomic<bool> g_npcPageRefreshQueued{ false };
        std::chrono::steady_clock::time_point g_npcPageCacheStamp{};

        void QueueNpcPageRefresh()
        {
            if (g_npcPageRefreshQueued.exchange(true)) {
                return;
            }
            SKSE::GetTaskInterface()->AddTask([] {
                NpcPageCache fresh;
                fresh.espLoaded = NpcEspLoaded();
                fresh.published = PublishedSnapshot();
                fresh.dormant = fresh.published.size();
                if (fresh.espLoaded) {
                    fresh.bindings = PubBindingsSnapshot();
                    fresh.assignments = NprAssignmentsSnapshot();
                    fresh.catalog = PersistContents();
                    fresh.injected = InjectedNpcCount();
                    fresh.maxInjected = MaxNpcInjected();
                    fresh.unresolvedNpr = UnresolvedNprAssignmentCount();
                    for (const auto& snap : fresh.published) {
                        fresh.hidden[snap.pubSlot] = PubHidden(snap.pubSlot);
                    }
                    for (const auto& item : fresh.assignments) {
                        auto* form = RE::TESForm::LookupByID(item.actorFormID);
                        auto* actor = form ? form->As<RE::Actor>() : nullptr;
                        const char* name = actor ? actor->GetName() : nullptr;
                        fresh.assignmentNames.push_back(
                            name && *name ? EnsureUtf8(name) : "(unloaded)");
                        auto* token = NprTokenArmo(item.poolSlot);
                        fresh.assignmentWorn.push_back(
                            actor && token && actor->GetWornArmor(token->GetFormID()) ? 1 : 0);
                        for (const auto& id : item.contents) {
                            if (!fresh.names.contains(id)) fresh.names[id] = ItemDisplayName(id);
                        }
                    }
                    for (const auto& id : fresh.catalog) {
                        if (!fresh.names.contains(id)) fresh.names[id] = ItemDisplayName(id);
                    }
                }
                fresh.valid = true;
                {
                    std::scoped_lock lk(g_npcPageCacheMutex);
                    g_npcPageCache = std::move(fresh);
                    g_npcPageCacheStamp = std::chrono::steady_clock::now();
                }
                g_npcPageRefreshQueued.store(false);
            });
        }

        void __stdcall RenderNpc()
        {
            NpcPageCache view;
            {
                std::scoped_lock lk(g_npcPageCacheMutex);
                view = g_npcPageCache;
                const auto age = std::chrono::steady_clock::now() - g_npcPageCacheStamp;
                if (!view.valid || age > std::chrono::milliseconds(500)) {
                    QueueNpcPageRefresh();
                }
            }
            if (!view.valid) {
                ImGui::TextDisabled("(loading...)");
                return;
            }
            if (!view.espLoaded) {
                ImGui::TextWrapped("The NPC token add-on plugin (CostumeFW_NPC.esp) is not installed. Install it to use NPC distribution.");
                if (view.dormant) ImGui::Text("%d published definition(s) are dormant.",
                    static_cast<int>(view.dormant));
                return;
            }
            const auto& published = view.published;
            const auto& bindings = view.bindings;
            ImGui::Text("Injected NPCs: %d / %d", static_cast<int>(view.injected),
                view.maxInjected);
            ImGui::SeparatorText("NPC persist");
            ImGui::Text("Pool slots: %d / 8 used", static_cast<int>(
                view.assignments.size() + view.unresolvedNpr));
            for (std::size_t i = 0; i < view.assignments.size(); ++i) {
                const auto& item = view.assignments[i];
                const auto& actorName =
                    i < view.assignmentNames.size() ? view.assignmentNames[i] : std::string("?");
                const bool worn = i < view.assignmentWorn.size() && view.assignmentWorn[i] != 0;
                const auto title = std::format("Pool {:02}  {}  ({:08X})  {} item(s){}{}{}###nprslot{}",
                    item.poolSlot + 1, actorName, item.actorFormID,
                    static_cast<int>(item.contents.size()),
                    worn ? "  [worn]" : "", item.unresolved ? "  (unresolved)" : "",
                    item.restoreSuspended ? "  (restore suspended)" : "", item.poolSlot);
                if (!ImGui::TreeNode(title.c_str())) {
                    continue;
                }
                auto& buf = g_nprEdit[item.poolSlot];
                std::string current;
                for (const auto& id : item.contents) {
                    current += id;
                    current += '\n';
                }
                if (!buf.dirty && buf.baseline != current) {
                    buf.baseline = current;
                    buf.checks.clear();
                    for (const auto& id : item.contents) {
                        buf.checks[id] = true;
                    }
                }
                // Union rows: the assignment's contents first (their order),
                // then catalog entries not yet assigned.
                std::vector<std::string> rows = item.contents;
                for (const auto& id : view.catalog) {
                    if (std::find(rows.begin(), rows.end(), id) == rows.end()) {
                        rows.push_back(id);
                    }
                }
                for (const auto& [cid, on] : buf.checks) {
                    (void)on;  // keep the row visible even if unchecked again
                    if (std::find(rows.begin(), rows.end(), cid) == rows.end()) {
                        rows.push_back(cid);
                    }
                }
                int checkedCount = 0;
                for (const auto& id : rows) {
                    auto it = buf.checks.find(id);
                    bool checked = it != buf.checks.end() && it->second;
                    const auto nameIt = view.names.find(id);
                    const auto label = std::format("{}##npchk{}_{}",
                        nameIt != view.names.end() ? nameIt->second : ItemDisplayName(id),
                        item.poolSlot, id);
                    if (ImGui::Checkbox(label.c_str(), &checked)) {
                        buf.checks[id] = checked;
                        buf.dirty = true;
                    }
                    if (checked) {
                        ++checkedCount;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", id.c_str());
                }
                NpcInventoryAddCombo(std::format("a{}", item.poolSlot),
                    [&buf](const std::string& a_id) {
                        buf.checks[a_id] = true;
                        buf.dirty = true;
                    });
                ImGui::BeginDisabled(!buf.dirty || checkedCount == 0);
                if (ImGui::Button(std::format("Apply changes##npap{}", item.poolSlot).c_str())) {
                    std::vector<std::string> sel;
                    for (const auto& id : rows) {
                        auto it = buf.checks.find(id);
                        if (it != buf.checks.end() && it->second) {
                            sel.push_back(id);
                        }
                    }
                    const RE::FormID actorID = item.actorFormID;
                    SKSE::GetTaskInterface()->AddTask([actorID, sel] {
                        auto* form = RE::TESForm::LookupByID(actorID);
                        UpdateNpcPersist(form ? form->As<RE::Actor>() : nullptr, sel);
                    });
                    buf.dirty = false;
                    buf.baseline.clear();  // resync from the next cache refresh
                }
                ImGui::EndDisabled();
                if (buf.dirty) {
                    ImGui::SameLine();
                    if (ImGui::Button(std::format("Revert##nprev{}", item.poolSlot).c_str())) {
                        buf.dirty = false;
                        buf.baseline.clear();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(std::format("Refresh##npfr{}", item.poolSlot).c_str())) {
                    const RE::FormID actorID = item.actorFormID;
                    SKSE::GetTaskInterface()->AddTask([actorID] {
                        auto* form = RE::TESForm::LookupByID(actorID);
                        RefreshNpcPersist(form ? form->As<RE::Actor>() : nullptr);
                    });
                }
                ImGui::SameLine();
                if (ImGui::Button(std::format("Remove assignment##nprm{}", item.poolSlot).c_str())) {
                    const RE::FormID actorID = item.actorFormID;
                    SKSE::GetTaskInterface()->AddTask([actorID] {
                        auto* form = RE::TESForm::LookupByID(actorID);
                        RemoveNpcPersist(form ? form->As<RE::Actor>() : nullptr);
                    });
                }
                ImGui::TreePop();
            }
            ImGui::SeparatorText("New assignment");
            // Crosshair name readout: short-lived NiPointer read, same pattern
            // as the existing crosshair buttons (accepted in the merge review).
            RE::Actor* crosshairActor = nullptr;
            {
                auto* pick = RE::CrosshairPickData::GetSingleton();
                auto ref = pick ? pick->targetActor.get() : RE::NiPointer<RE::TESObjectREFR>{};
                if (!ref && pick) ref = pick->target.get();
                auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
                if (actor && actor != RE::PlayerCharacter::GetSingleton()) {
                    crosshairActor = actor;
                }
            }
            const std::string crosshairName =
                crosshairActor && crosshairActor->GetName() && *crosshairActor->GetName() ?
                    EnsureUtf8(crosshairActor->GetName()) : "(no NPC under crosshair)";
            ImGui::Text("Crosshair target: %s", crosshairName.c_str());
            {
                std::vector<std::string> newRows = view.catalog;
                for (const auto& [cid, on] : g_newAssignChecks) {
                    (void)on;
                    if (std::find(newRows.begin(), newRows.end(), cid) == newRows.end()) {
                        newRows.push_back(cid);
                    }
                }
                if (newRows.empty()) {
                    ImGui::TextDisabled(
                        "Pick from the shared Persist catalog below or add straight from "
                        "your inventory.");
                }
                int newChecked = 0;
                for (const auto& id : newRows) {
                    bool checked = g_newAssignChecks[id];
                    const auto nameIt = view.names.find(id);
                    const auto label = std::format("{}##npnew_{}",
                        nameIt != view.names.end() ? nameIt->second : ItemDisplayName(id), id);
                    if (ImGui::Checkbox(label.c_str(), &checked)) {
                        g_newAssignChecks[id] = checked;
                    }
                    if (checked) {
                        ++newChecked;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", id.c_str());
                }
                NpcInventoryAddCombo("new",
                    [](const std::string& a_id) { g_newAssignChecks[a_id] = true; });
                ImGui::InputText("Inventory filter##npif", g_npcInvFilter,
                    sizeof(g_npcInvFilter));
                ImGui::BeginDisabled(newChecked == 0 || !crosshairActor);
                if (ImGui::Button("Assign selected to crosshair NPC##npnewgo")) {
                    std::vector<std::string> sel;
                    for (const auto& id : newRows) {
                        if (g_newAssignChecks[id]) {
                            sel.push_back(id);
                        }
                    }
                    const auto handle = crosshairActor->GetHandle();
                    SKSE::GetTaskInterface()->AddTask([handle, sel] {
                        auto resolved = handle.get();
                        AssignNpcPersist(resolved ? resolved.get()->As<RE::Actor>() : nullptr, sel);
                    });
                    g_newAssignChecks.clear();
                }
                ImGui::EndDisabled();
            }
            ImGui::TextDisabled(
                "Refresh re-equips the carrier so FSMP physics converge (repeat until the outfit sways).");
            ImGui::SeparatorText("Published costumes");
            if (published.empty()) {
                ImGui::TextDisabled("(nothing published)");
                return;
            }
            auto* playerRef = RE::PlayerCharacter::GetSingleton();
            const RE::FormID playerID = playerRef ? playerRef->GetFormID() : 0x14;
            for (const auto& snap : published) {
                int holders = 0, wearers = 0, unresolved = 0, npcHolders = 0;
                for (const auto& binding : bindings) {
                    if (binding.pubSlot != snap.pubSlot) continue;
                    holders += binding.holder;
                    // The player's own copies never block unpublish (the flow
                    // reclaims them itself) - gate on NON-player holders only.
                    npcHolders += (binding.holder && binding.actorFormID != playerID) ? 1 : 0;
                    wearers += binding.wearer;
                    unresolved += binding.unresolved;
                }
                const auto title = std::format("Pub {:02}: {} (slot {}, {} item(s), {}/{} worn/held, {} unresolved)###npc{}",
                    snap.pubSlot + 1, snap.label, snap.sourceSlot, snap.contents.size(), wearers,
                    holders, unresolved, snap.pubSlot);
                const auto hiddenIt = view.hidden.find(snap.pubSlot);
                const bool hidden = hiddenIt != view.hidden.end() && hiddenIt->second;
                if (ImGui::Button(std::format("{}##npv{}", hidden ? "Show" : "Hide",
                        snap.pubSlot).c_str())) {
                    const int slot = snap.pubSlot;
                    SKSE::GetTaskInterface()->AddTask([slot, hidden] { SetPubHidden(slot, !hidden); });
                }
                ImGui::SameLine();
                if (!ImGui::TreeNode(title.c_str())) continue;
                if (ImGui::Button(std::format("Refresh##npref{}", snap.pubSlot).c_str())) {
                    const int slot = snap.pubSlot;
                    SKSE::GetTaskInterface()->AddTask([slot] { RefreshPubWearers(slot); });
                }
                ImGui::SameLine();
                const auto recallPopup = std::format("Recall published costume?###npr{}", snap.pubSlot);
                if (ImGui::Button(std::format("Recall##nprec{}", snap.pubSlot).c_str()))
                    ImGui::OpenPopup(recallPopup.c_str());
                ImGui::SameLine();
                const bool canUnpublish = npcHolders == 0 && unresolved == 0;
                const auto unpublishPopup = std::format("Unpublish costume?###npu{}", snap.pubSlot);
                ImGui::BeginDisabled(!canUnpublish);
                if (ImGui::Button(std::format("Unpublish##npunp{}", snap.pubSlot).c_str()))
                    ImGui::OpenPopup(unpublishPopup.c_str());
                ImGui::EndDisabled();
                if (!canUnpublish)
                    ImGui::TextDisabled("Recall all known holders before unpublishing.");

                ImGui::SetNextWindowSize(ImGui::ImVec2(480, 0), ImGui::ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal(recallPopup.c_str())) {
                    ImGui::TextWrapped("Recall every tracked copy to the player and clear this slot's binding table?");
                    if (unresolved > 0)
                        ImGui::TextWrapped("Warning: %d unresolved binding(s) are outside the recovery net and will be dropped.", unresolved);
                    if (ImGui::Button("Recall")) {
                        const int slot = snap.pubSlot;
                        SKSE::GetTaskInterface()->AddTask([slot] { RecallPublished(slot); });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                ImGui::SetNextWindowSize(ImGui::ImVec2(480, 0), ImGui::ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal(unpublishPopup.c_str())) {
                    ImGui::TextWrapped("Restore this frozen snapshot as a normal box and free the published slot?");
                    if (ImGui::Button("Unpublish")) {
                        const int slot = snap.pubSlot;
                        SKSE::GetTaskInterface()->AddTask([slot] { UnpublishToBox(slot); });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                ImGui::SeparatorText("Contents");
                for (const auto& id : snap.contents)
                    ImGui::BulletText("%s", ItemDisplayName(id).c_str());

                ImGui::SeparatorText("Tracked holders");
                bool anyBinding = false;
                const auto* player = RE::PlayerCharacter::GetSingleton();
                const auto playerId = player ? player->GetFormID() : 0;
                for (const auto& binding : bindings) {
                    if (binding.pubSlot != snap.pubSlot) continue;
                    anyBinding = true;
                    const auto name = binding.actorName.empty() ?
                        std::format("FormID {:08X}", binding.actorFormID) : binding.actorName;
                    const auto state = binding.unresolved ? "unresolved" :
                        (binding.loaded ? "loaded" : "unloaded");
                    ImGui::BulletText("%s (%s%s%s)", name.c_str(), state,
                        binding.holder ? ", held" : "", binding.wearer ? ", worn" : "");
                    if (binding.loaded && binding.holder && binding.actorFormID != playerId) {
                        ImGui::SameLine();
                        const auto button = std::format("{}##npwear{}{}",
                            binding.wearer ? "Unequip" : "Equip", snap.pubSlot,
                            binding.actorFormID);
                        if (ImGui::Button(button.c_str())) {
                            const auto handle = binding.handle;
                            const int slot = snap.pubSlot;
                            const bool worn = !binding.wearer;
                            SKSE::GetTaskInterface()->AddTask(
                                [handle, slot, worn] { SetNpcTokenWorn(handle, slot, worn); });
                        }
                    }
                }
                if (!anyBinding) ImGui::TextDisabled("(no tracked holders)");
                ImGui::TreePop();
            }
        }

        // --- Recovery (v1.6.2) ------------------------------------------------
        // Its own page, not a fold on Main: this lists EVERY item CEF has ever
        // taken into storage, which on a real setup is dozens of rows - enough to
        // push the page's own controls out of reach if it shared one.
        //
        // The net under the orphan sweep. The sweep hands back what the hidden
        // store still holds; this covers what it cannot reach - the store ref lost
        // with a save, a piece captured on another character, a box entry rolled
        // back away. One row per item, because once the box entry is gone the id
        // is the only handle left and the user has no way to know it.
        // Row cache, same contract as the NPC page and Diagnostics: CustodyRows()
        // resolves live forms and reads the lock-free PublishStore vectors, so it
        // runs ON THE MAIN THREAD via the task pump and reaches the renderer as a
        // mutex-guarded copy. Building it on the render thread was the same
        // use-after-free class those two already fixed - and rebuilding it per
        // frame was pure waste on a page that only changes when a button is
        // pressed. Half a second of staleness is invisible here.
        std::vector<CustodyRow> g_recoveryRows;
        bool g_recoveryValid = false;
        std::mutex g_recoveryMutex;
        std::atomic<bool> g_recoveryRefreshQueued{ false };
        std::chrono::steady_clock::time_point g_recoveryStamp{};

        void QueueRecoveryRefresh()
        {
            if (g_recoveryRefreshQueued.exchange(true)) {
                return;
            }
            SKSE::GetTaskInterface()->AddTask([] {
                auto fresh = CustodyRows();
                {
                    std::scoped_lock lk(g_recoveryMutex);
                    g_recoveryRows = std::move(fresh);
                    g_recoveryValid = true;
                    g_recoveryStamp = std::chrono::steady_clock::now();
                }
                g_recoveryRefreshQueued.store(false);
            });
        }

        void __stdcall RenderRecovery()
        {
            std::vector<CustodyRow> log;
            bool valid = false;
            {
                std::scoped_lock lk(g_recoveryMutex);
                log = g_recoveryRows;
                valid = g_recoveryValid;
                const auto age = std::chrono::steady_clock::now() - g_recoveryStamp;
                if (!valid || age > std::chrono::milliseconds(500)) {
                    QueueRecoveryRefresh();
                }
            }
            ImGui::TextWrapped(
                "Every item CEF has taken into storage, newest first. Use this when a piece "
                "went missing. An item a box still holds is not missing - take it out of the "
                "box instead; those rows say where they are. For the rest, if the item is "
                "still in storage you get the original back with its tempering and "
                "enchantment, and if it is not, CEF recreates a plain copy - so recovering "
                "something you already have gives you two.");
            if (!valid) {
                ImGui::TextDisabled("(loading...)");
                return;
            }
            if (log.empty()) {
                ImGui::TextDisabled("Nothing captured yet.");
                return;
            }
            ImGui::InputText("Filter##recf", s_recFilter, sizeof(s_recFilter));
            // Rows that can be acted on come first: on a full setup almost every
            // row is held and has no button, and a handful of recoverable ones at
            // the bottom of eighty is the same as not showing them. Pointers into
            // the snapshot above - it outlives the loop.
            std::vector<const CustodyRow*> rows;
            rows.reserve(log.size());
            std::size_t loose = 0;
            for (const auto& r : log) {
                if (!RowMatches(r.entry.name, s_recFilter) &&
                    !RowMatches(r.entry.id, s_recFilter)) {
                    continue;
                }
                if (r.holder.empty()) {
                    ++loose;
                }
                rows.push_back(&r);
            }
            std::stable_sort(rows.begin(), rows.end(),
                [](const CustodyRow* a, const CustodyRow* b) {
                    return a->holder.empty() && !b->holder.empty();
                });
            ImGui::Text("%d shown, %d not held by anything",
                static_cast<int>(rows.size()), static_cast<int>(loose));
            BeginScrollList("##reclist");
            for (const auto* row : rows) {
                const auto& e = row->entry;
                const auto& held = row->holder;
                ImGui::PushID(e.id.c_str());
                // A held item is reachable the normal way, and recovering it
                // drains storage while the box goes on claiming it - which then
                // hands out a plain copy on the next remove. Don't offer the
                // footgun; say where the item is.
                if (!held.empty()) {
                    ImGui::TextDisabled("in %s", held.c_str());
                    ImGui::SameLine();
                    ImGui::Text("%s", e.name.c_str());
                    ImGui::PopID();
                    continue;
                }
                if (ImGui::Button("Recover")) {
                    ImGui::OpenPopup("Recover this item?###cfwrec");
                }
                ImGui::SetNextWindowSize(ImGui::ImVec2(460, 0), ImGui::ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal("Recover this item?###cfwrec")) {
                    ImGui::TextWrapped("%s", e.name.c_str());
                    ImGui::TextDisabled("%s", e.id.c_str());
                    ImGui::TextWrapped(
                        "If this item is no longer in storage, CEF recreates it without its "
                        "tempering or player enchantment. Only do this for a piece you have "
                        "actually lost.");
                    if (ImGui::Button("Recover##go")) {
                        const std::string id = e.id;
                        SKSE::GetTaskInterface()->AddTask([id] {
                            if (RecoverContentItem(id)) {
                                RE::DebugNotification(
                                    ("CostumeFW: recovered " + ItemDisplayName(id)).c_str());
                            } else {
                                RE::DebugNotification(
                                    "CostumeFW: that item could not be resolved - see the log");
                            }
                        });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel##no")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::Text("%s", e.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s  %s", e.event.c_str(), e.when.c_str());
                ImGui::PopID();
            }
            EndScrollList();
        }

        void __stdcall RenderDiagnostics()
        {
            // Two-tier logging switch (persist-CTD instrumentation). Session-only
            // here; [Diagnostics] bDebugMode=1 in CostumeExpansionFW.ini is the
            // persistent form. SetDebugMode is atomics + spdlog (thread-safe), so
            // no AddTask hop is needed - this toggles LOGGING ONLY, never behavior.
            bool dbg = Diag::Debug();
            if (ImGui::Checkbox("Verbose diagnostic logging (debug mode)##cfwdbg", &dbg)) {
                Diag::SetDebugMode(dbg, "SMF Diagnostics toggle");
            }
            if (dbg) {
                ImGui::TextWrapped(
                    "Debug logging is ON: larger log file, small overhead. Play normally; "
                    "after the next crash send CostumeExpansionFW.log + the crash log. "
                    "Session-only - restarts revert to the ini setting.");
            }
            ImGui::Separator();

            // Snapshot once per open/click, not per frame - and ON THE MAIN
            // THREAD: DiagLines() now includes NpcDiagLines(), which walks the
            // lock-free PublishStore vectors, so running it on the render thread
            // raced the task pump (merge review 2026-08-04; same class the NPC
            // page cache fixed).
            static std::mutex s_linesMutex;
            static std::vector<std::string> s_lines;
            static std::atomic<bool> s_linesPending{ false };
            static bool s_requested = false;
            if (ImGui::Button("Refresh##cfwdiag") || !s_requested) {
                s_requested = true;
                if (!s_linesPending.exchange(true)) {
                    SKSE::GetTaskInterface()->AddTask([] {
                        auto lines = DiagLines();
                        {
                            std::scoped_lock lk(s_linesMutex);
                            s_lines = std::move(lines);
                        }
                        s_linesPending.store(false);
                    });
                }
            }
            std::vector<std::string> linesView;
            {
                std::scoped_lock lk(s_linesMutex);
                linesView = s_lines;
            }
            // Grows with the store (one line per box content / persist entry) -
            // the report's "can't reach the bottom" applies here too.
            BeginScrollList("##cfwdiaglist");
            for (const auto& l : linesView) {
                if (l.rfind("# ", 0) == 0) {
                    ImGui::SeparatorText(l.c_str() + 2);
                } else {
                    ImGui::TextUnformatted(l.c_str());
                }
            }
            EndScrollList();
        }
    }

    namespace
    {
        // --- Blocked items (v1.3.2 capture blacklist, MARA_COMPAT_PLAN.md §3) ---
        // Pure config UI: reads are per-frame snapshots (GetCaptureBlacklist
        // copies), mutations go through AddTask like every other mutator.
        void __stdcall RenderBlocked()
        {
            ImGui::TextWrapped(
                "Items matched here are hidden from the capture pickers and refused by "
                "the capture gate. The structural skips (runtime/dynamic forms, "
                "non-playable armors) protect against MARA-class runtime items whose "
                "inventory data crashes on touch; the deny-list names known offenders.");
            const auto view = GetCaptureBlacklist();

            bool allowNp = view.allowNonPlayable;
            if (ImGui::Checkbox("Show non-playable armors in pickers##blkNp", &allowNp)) {
                SKSE::GetTaskInterface()->AddTask(
                    [allowNp] { SetCaptureBlacklistFlag("allowNonPlayable", allowNp); });
            }
            // No "show runtime forms" switch (review r2, P1-4): the dynamic /
            // no-file skip is a hard invariant - reading such a form's data
            // (GetLocalFormID null-derefs) is the original CTD, and capture
            // could never restore it across a load anyway.
            bool noDefaults = view.disableDefaults;
            if (ImGui::Checkbox("Disable shipped default entries##blkDef", &noDefaults)) {
                SKSE::GetTaskInterface()->AddTask(
                    [noDefaults] { SetCaptureBlacklistFlag("disableDefaults", noDefaults); });
            }

            // X-SCROLL: the deny-list is user-grown; keep the add row (X-UI1) and
            // its help text below the region so they stay reachable at any size.
            BeginScrollList("##blklist",
                ImGui::GetFrameHeightWithSpacing() + 4.0f * ImGui::GetTextLineHeightWithSpacing());
            ImGui::SeparatorText("Shipped defaults");
            for (const auto& name : view.defaultNames) {
                ImGui::Text(view.disableDefaults ? "name: %s (off)" : "name: %s", name.c_str());
            }
            for (const auto& plugin : view.defaultPlugins) {
                ImGui::Text(view.disableDefaults ? "plugin: %s* (off)" : "plugin: %s*",
                    plugin.c_str());
            }

            ImGui::SeparatorText(std::format("User entries ({})",
                view.names.size() + view.plugins.size() + view.ids.size()).c_str());
            const auto renderRows = [](const char* a_kind, const std::vector<std::string>& a_rows) {
                for (const auto& value : a_rows) {
                    ImGui::Text("%s: %s", a_kind, value.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(std::format("Remove##blk{}{}", a_kind, value).c_str())) {
                        const std::string kind = a_kind;
                        const std::string entry = value;
                        SKSE::GetTaskInterface()->AddTask(
                            [kind, entry] { RemoveCaptureBlacklistEntry(kind, entry); });
                    }
                }
            };
            renderRows("name", view.names);
            renderRows("plugin", view.plugins);
            renderRows("id", view.ids);
            EndScrollList();

            ImGui::Spacing();
            static const char* kKinds[] = { "name", "plugin", "id" };
            // X-UI1 (test run 2026-07-26): this row used to be combo + input +
            // button chained with SameLine and NO width hints, so a narrow SMF
            // window let the input eat the line and pushed "Add" off-screen -
            // the add path was unusable in practice (that run's log carries
            // blacklist removals and switch flips, never a single "entry
            // added"). Widths are pinned, Enter commits, and an empty commit
            // says so instead of being a silent no-op.
            constexpr float kKindW = 90.0f;
            constexpr float kAddW = 60.0f;
            constexpr float kMinValueW = 120.0f;
            ImGui::SetNextItemWidth(kKindW);
            if (ImGui::BeginCombo("##blkKind", kKinds[s_blkKind])) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(kKinds[i], i == s_blkKind)) {
                        s_blkKind = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const float rowAvail = ImGui::GetContentRegionAvail().x;
            const float valueW = rowAvail - kAddW - 24.0f;  // 24 = two SameLine gaps
            ImGui::SetNextItemWidth(valueW > kMinValueW ? valueW : kMinValueW);
            const bool committed = ImGui::InputText("##blkVal", s_blkValue, sizeof(s_blkValue),
                ImGui::ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Add##blkAdd") || committed) {
                if (s_blkValue[0] == '\0') {
                    s_status = "nothing to add - type a value first "
                               "(name / plugin prefix / LOCALID:Plugin.esp)";
                } else {
                    const std::string kind = kKinds[s_blkKind];
                    const std::string value = s_blkValue;
                    SKSE::GetTaskInterface()->AddTask(
                        [kind, value] { AddCaptureBlacklistEntry(kind, value); });
                    s_status = std::format("queued: block {} '{}'", kind, value);
                    s_blkValue[0] = '\0';
                }
            }
            ImGui::TextWrapped(
                "name = exact display name, or 'prefix*'. plugin = source-plugin "
                "filename prefix (e.g. MARA). id = colon-id LOCALID:Plugin.esp.");
            if (!s_status.empty()) {
                ImGui::TextUnformatted(s_status.c_str());
            }
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            SKSE::log::info("SMF: SKSEMenuFramework.dll not installed - SMF UI skipped "
                            "(MCM / console remain available)");
            return;
        }
        SKSEMenuFramework::SetSection("Costume Expansion FW");
        SKSEMenuFramework::AddSectionItem("Main", RenderMain);
        SKSEMenuFramework::AddSectionItem("Boxes", RenderBoxes);
        SKSEMenuFramework::AddSectionItem("Persist", RenderPersist);
        SKSEMenuFramework::AddSectionItem("NPC", RenderNpc);
        SKSEMenuFramework::AddSectionItem("Presets", RenderPresets);
        SKSEMenuFramework::AddSectionItem("Blocked", RenderBlocked);  // v1.3.2 capture blacklist
        SKSEMenuFramework::AddSectionItem("Recovery", RenderRecovery);
        SKSEMenuFramework::AddSectionItem("Diagnostics", RenderDiagnostics);
        SKSE::log::info("SMF: registered section 'Costume Expansion FW' (6 pages)");
    }
}
