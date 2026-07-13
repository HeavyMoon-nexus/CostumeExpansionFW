#include "SmfUI.h"

#include "BodyMorph.h"
#include "BoxStore.h"
#include "Preset.h"
#include "SkinRebind.h"
#include "UiOps.h"

#include "RE/T/TESDataHandler.h"

#include <algorithm>
#include <cstdio>
#include <format>
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
        char s_hideSlots[64] = "";        // hide-when-worn slot list edit buffer
        std::string s_hideSlotsFor;       // which content the buffer was loaded for
        char s_exportName[64] = "";       // "Export as preset" name buffer
        std::string s_status;             // last guard/op feedback line
        // Optimistic Wear state: the engine's equip QUEUE does not process while
        // SMF pauses the game, so an honest per-frame GetWornArmor re-read
        // snapped the checkbox straight back (in-game 2026-07-12). Show the
        // user's intent until the live state catches up (menu close), then drop
        // the override. The MCM did the same via SetToggleOptionValue.
        std::unordered_map<std::string, bool> s_pendingWear;  // token -> desired

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
            if (!CanResolveContent(a_id)) {
                s_status = "this item's mesh could not be resolved - not captured (see log)";
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
            if (!CanResolveContent(a_id)) {
                s_status = "this item's mesh could not be resolved - not captured (see log)";
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

            const int n = BoxCount();
            for (int i = 0; i < n; ++i) {
                const BoxDefInfo b = BoxAt(i);
                const std::string token = b.token;
                const int slot = TokenSlot(token);
                const bool worn = BoxWornAt(i);
                // "###" id keyed by SLOT (stable): a deletion shifting box indices
                // must not re-target open tree nodes (the MCM's ROOT I analog).
                const std::string header = std::format("Box {}: {} - {} item(s){}###cfwbox{}",
                    slot, SlotName(slot), b.contents.size(), worn ? "  [WORN]" : "", slot);
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

                const std::string preset = BoxPreset(token);
                if (ImGui::BeginCombo(std::format("Preset##pr{}", slot).c_str(),
                        preset.empty() ? "(manual)" : preset.c_str())) {
                    // Snapshot on combo OPEN, not per frame (List() scans the
                    // presets folder; a combo renders every frame while open).
                    static std::vector<Preset::PresetInfo> s_presets;
                    if (ImGui::IsWindowAppearing()) {
                        s_presets = Preset::List();
                    }
                    if (ImGui::Selectable("(manual)", preset.empty()) && !preset.empty()) {
                        UiOps::ClearPreset(token);
                    }
                    for (const auto& p : s_presets) {
                        if (ImGui::Selectable(std::format("{}##pf{}", p.name, p.file).c_str(),
                                p.name == preset) &&
                            p.name != preset) {
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

            const std::string ppreset = PersistPreset();
            if (ImGui::BeginCombo("Preset##ppr", ppreset.empty() ? "(manual)" : ppreset.c_str())) {
                static std::vector<Preset::PresetInfo> s_presets;
                if (ImGui::IsWindowAppearing()) {
                    s_presets = Preset::List();
                }
                if (ImGui::Selectable("(manual)", ppreset.empty()) && !ppreset.empty()) {
                    UiOps::ClearPersistPreset();
                }
                for (const auto& p : s_presets) {
                    if (ImGui::Selectable(std::format("{}##ppf{}", p.name, p.file).c_str(),
                            p.name == ppreset) &&
                        p.name != ppreset) {
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

            ImGui::SeparatorText(
                std::format("Catalog ({}) - shared across saves", contents.size()).c_str());
            for (const auto& id : contents) {
                const bool act = isActive(id);
                const std::string header = std::format("{}{}###pc{}", ItemDisplayName(id),
                    act ? "  [ON]" : "", id);
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

            // Active on this save but no longer in the shared catalog.
            std::vector<std::string> uncat;
            for (const auto& id : actives) {
                if (std::find(contents.begin(), contents.end(), id) == contents.end()) {
                    uncat.push_back(id);
                }
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
            for (const auto& p : s_list) {
                const std::string assigned = PresetAssignedTo(p.name);
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
        }

        void __stdcall RenderDiagnostics()
        {
            // Snapshot once per open/click, not per frame - DiagLines() walks the
            // whole store and the render callback fires every frame.
            static std::vector<std::string> s_lines;
            static bool s_loaded = false;
            if (ImGui::Button("Refresh##cfwdiag") || !s_loaded) {
                s_lines = DiagLines();
                s_loaded = true;
            }
            for (const auto& l : s_lines) {
                if (l.rfind("# ", 0) == 0) {
                    ImGui::SeparatorText(l.c_str() + 2);
                } else {
                    ImGui::TextUnformatted(l.c_str());
                }
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
        SKSEMenuFramework::AddSectionItem("Presets", RenderPresets);
        SKSEMenuFramework::AddSectionItem("Diagnostics", RenderDiagnostics);
        SKSE::log::info("SMF: registered section 'Costume Expansion FW' (5 pages)");
    }
}
