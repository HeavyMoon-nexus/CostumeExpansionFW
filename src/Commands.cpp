#include "Commands.h"
#include "BoxStore.h"
#include "SkinRebind.h"
#include "PublishStore.h"
#include "ConsoleOut.h"  // ConsolePrint - the one console chokepoint (F01)

#include "RE/S/Script.h"
#include "RE/C/Console.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        // Console output for `cef ...`. Forwards to the ONE console chokepoint
        // (ConsoleOut.h) - never call ConsoleLog::Print directly: it is a varargs
        // FORMAT call, and a costume label or shape name containing '%' would be
        // read as the format string (review 2026-09-09 F01).
        void Print(std::string_view a_msg) { ConsolePrint(a_msg); }

        std::string Trim(std::string s)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

        // Split "id1 id2" (two colon-form FormKeys) at the end of id1's plugin
        // extension (.esp/.esm/.esl) - plugin names contain spaces, so a plain
        // space split won't work. Returns false if no extension boundary found.
        bool SplitTwoIds(const std::string& a_rest, std::string& a_first, std::string& a_second)
        {
            const std::string low = Lower(a_rest);
            std::size_t end = std::string::npos;
            for (const char* ext : { ".esp", ".esm", ".esl" }) {
                const auto p = low.find(ext);
                if (p != std::string::npos) {
                    const auto e = p + 4;
                    if (end == std::string::npos || e < end) {
                        end = e;
                    }
                }
            }
            if (end == std::string::npos) {
                return false;
            }
            a_first = Trim(a_rest.substr(0, end));
            a_second = Trim(a_rest.substr(end));
            return !a_first.empty() && !a_second.empty();
        }

        // Hook RE::Script::CompileAndRun: read the raw typed line via GetCommand(),
        // intercept our "cef" prefix, suppress the vanilla compiler for it.
        // (Pattern from KrisV-777/ConsoleUtil-Extended.)
        struct ConsoleHook
        {
            static void thunk(RE::Script* a_script, RE::ScriptCompiler* a_compiler,
                RE::COMPILER_NAME a_name, RE::TESObjectREFR* a_targetRef)
            {
                if (a_script) {
                    const std::string line = a_script->GetCommand();
                    if (line.size() >= 3 && _strnicmp(line.c_str(), "cef", 3) == 0 &&
                        (line.size() == 3 || std::isspace(static_cast<unsigned char>(line[3])))) {
                        HandleConsoleCommand(line, a_targetRef);
                        return;  // suppress vanilla "unknown command"
                    }
                }
                func(a_script, a_compiler, a_name, a_targetRef);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void InstallConsoleHook()
    {
        // Call-site offset into the console runner is variant-specific:
        // SE=0xE2, AE(1.6.x incl 1.6.1170)=0x52, VR=0xE2. Using 0xE2 on AE split
        // an instruction -> ILLEGAL_INSTRUCTION crash. (ConsoleUtil-Extended.)
        std::uintptr_t callSite = 0;
        if (REL::Module::IsVR()) {
            // SkyrimVR 1.4.15.0: the console-runner fn has no address-library id
            // (SE id 52065 is absent from the VR database, and constructing the
            // RelocationID below would be load-fatal), so use the raw offset.
            // Two independent sources agree on 0x90E1F0: ConsoleUtil-Extended's
            // shipping VR branch and the VR address library's auto-diff candidate
            // (sse_vr.csv). VR is frozen at 1.4.15.0, so a raw offset cannot rot.
            callSite = REL::Offset(0x90E1F0 + 0xE2).address();
        } else {
            REL::Relocation<std::uintptr_t> target{
                REL::RelocationID(52065, 52952), REL::VariantOffset(0xE2, 0x52, 0xE2)
            };
            callSite = target.address();
        }
        ConsoleHook::func = SKSE::GetTrampoline().write_call<5>(callSite, ConsoleHook::thunk);
        SKSE::log::info("console hook installed (Script::CompileAndRun)");
    }

    void HandleConsoleCommand(const std::string& a_line, RE::TESObjectREFR* a_target)
    {
        // a_line = "cef <sub> <rest...>". Drop the "cef" token.
        const std::string afterPrefix = Trim(a_line.substr(3));
        if (afterPrefix.empty()) {
            Print("[CEF] inject | box | pub | npcpersist | detach | clear | list | store | repair | persist | "
                  "morph | shapes | hideshape | recover | headdiag | hair | nodediag | arraytest | "
                  "slottest | invisdiag");
            return;
        }

        // sub = first token; rest = everything after (may contain spaces/brackets,
        // e.g. a FormID "000BC1:[Caenarvon] Cosplay Basics.esp").
        const auto sp = afterPrefix.find_first_of(" \t");
        const std::string sub = Lower(sp == std::string::npos ? afterPrefix : afterPrefix.substr(0, sp));
        const std::string rest = sp == std::string::npos ? std::string{} : Trim(afterPrefix.substr(sp + 1));

        if (sub == "inject") {
            const auto colon = rest.find(':');
            if (colon == std::string::npos) {
                Print("[CEF] usage: cef inject <FormID:Plugin.esp>");
                return;
            }
            const std::string left = rest.substr(0, colon);
            const std::string plugin = rest.substr(colon + 1);
            std::uint32_t localID = 0;
            try {
                localID = static_cast<std::uint32_t>(std::stoul(left, nullptr, 16));
            } catch (...) {
                Print("[CEF] bad FormID");
                return;
            }
            SKSE::GetTaskInterface()->AddTask(
                [localID, plugin, rest] { InjectArma(localID, plugin, rest); });
            Print("[CEF] injecting (see log)");
        } else if (sub == "detach") {
            if (rest.empty()) {
                Print("[CEF] usage: cef detach <id>");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([rest] { DetachSkinned(rest); });
            Print("[CEF] detaching");
        } else if (sub == "carriers") {
            // Re-read carriers.json (rewritten by nifcarrier sync while the game
            // runs) and re-equip tokens whose carrier revision changed - the
            // restart-free FSMP carrier swap.
            SKSE::GetTaskInterface()->AddTask([] { ApplyCarrierOverrides(true); });
            Print("[CEF] reloading carrier revisions (see log)");
        } else if (sub == "testnif") {
            // Inject the NIF path written in Data\SKSE\Plugins\CostumeExpansionFW_test.txt
            // (id "test"; remove with `cef detach test`). Reads the txt fresh each call —
            // used e.g. to probe whether files created AFTER launch resolve through the VFS.
            SKSE::GetTaskInterface()->AddTask([] { InjectTestFromFile(); });
            Print("[CEF] test inject from CostumeExpansionFW_test.txt (see log)");
        } else if (sub == "box") {
            // cef box <token FormID:Plugin> <content FormID:Plugin>
            std::string token, content;
            if (!SplitTwoIds(rest, token, content)) {
                Print("[CEF] usage: cef box <token FormID:Plugin> <content FormID:Plugin>");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([content, token] { DefineBox(content, token); });
            Print("[CEF] box defined - equip the token to show it");
        } else if (sub == "clear") {
            SKSE::GetTaskInterface()->AddTask([] { DetachAll(); });
            Print("[CEF] cleared all");
        } else if (sub == "nuke") {
            // Diagnostic: detach EVERY CostumeFW_* node by traversal, leaving the
            // registry intact (so Reconcile would re-add legit items next frame).
            SKSE::GetTaskInterface()->AddTask([] {
                const int n = DetachAllInjected();
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint(("[CEF] nuke removed " + std::to_string(n) + " node(s)").c_str());
                }
            });
        } else if (sub == "repair") {
            // Manual equivalent of the MCM CEF off->on cycle: detach every
            // injected node (registry intact) and re-inject into the CURRENT
            // FSMP merge generations. The bind watchdog does this automatically
            // when it detects a dead generation; this is the on-demand button.
            SKSE::GetTaskInterface()->AddTask([] {
                const int n = DetachAllInjected();
                Reconcile();
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint(("[CEF] repair: re-injected (" + std::to_string(n) +
                              " node(s) detached)").c_str());
                }
            });
        } else if (sub == "list") {
            SKSE::GetTaskInterface()->AddTask([] { ListActive(); });
        } else if (sub == "store") {
            // The hidden store is a disabled container - there is no in-game way
            // to open it, so this is the only view of what CEF is holding.
            SKSE::GetTaskInterface()->AddTask([] {
                for (const auto& line : StoreDiagLines()) {
                    Print(line.c_str());
                }
            });
        } else if (sub == "persist") {
            // Stage 3b persist head-carrier levers:
            //   cef persist          - status (pool registration + carriers.json entry)
            //   cef persist regen    - re-read carriers.json, reconcile + FORCE a head
            //                          rebuild (repair lever; sync-complete runs the
            //                          same pass automatically)
            //   cef persist remove   - deregister the production pool AND purge
            //                          legacy PoC leftovers (000806-808) baked
            //                          into contaminated saves (rescue lever)
            //   cef persist on <id>  - M2: activate a CATALOG id on this save
            //   cef persist off <id> - M2: deactivate on this save (catalog kept)
            const std::string arg = Lower(rest);
            if (arg.empty()) {
                SKSE::GetTaskInterface()->AddTask([] { PersistCarrierStatus(); });
            } else if (arg == "regen") {
                SKSE::GetTaskInterface()->AddTask([] {
                    ApplyCarrierOverrides(true);
                    RebuildPlayerHead();  // force even when nothing changed
                });
                Print("[CEF] persist: reapplying carriers + rebuilding head (see log)");
            } else if (arg == "remove") {
                SKSE::GetTaskInterface()->AddTask([] { PersistCarrierRemove(); });
                Print("[CEF] persist: deregistering head-carrier pool (see log)");
            } else if (arg.rfind("on ", 0) == 0 || arg.rfind("off ", 0) == 0) {
                // M2 per-save activation levers (the MCM catalog UI lands in a
                // later phase). Use the raw rest for the id - plugin names are
                // case-sensitive on some filesystems and Lower() mangles them.
                const bool on = arg.rfind("on ", 0) == 0;
                const std::string id = Trim(rest.substr(on ? 3 : 4));
                SKSE::GetTaskInterface()->AddTask([id, on] {
                    const bool ok = PersistSetActive(id, on);
                    if (ok && !on) {
                        // ROOT A [1324]: an uncataloged-active persist id has no
                        // catalog entry keeping custody, so deactivating it strands
                        // the stored original. Return it store-only (mirrors the MCM
                        // uncataloged deactivate). A CATALOGED id keeps its stored
                        // copy (re-activatable), so leave it.
                        std::string cid = id;
                        CanonicalizeColonId(cid);
                        bool cataloged = false;
                        for (const auto& e : PersistContents()) {
                            if (e == cid) {
                                cataloged = true;
                                break;
                            }
                        }
                        if (!cataloged) {
                            ReturnStoredItem(cid, false);  // store-only, no fabricate
                        }
                    }
                    if (auto* c = RE::ConsoleLog::GetSingleton()) {
                        std::string msg;
                        if (ok) {
                            msg = std::string("[CEF] persist ") +
                                  (on ? "on: active on this save" : "off: deactivated on this save");
                        } else {
                            msg = on ? "[CEF] persist on: failed - not in catalog? (see log)"
                                     : "[CEF] persist off: not active on this save";
                        }
                        ConsolePrint(msg.c_str());
                    }
                });
            } else {
                Print("[CEF] usage: cef persist [regen|remove|on <id>|off <id>]");
            }
        } else if (sub == "morph") {
            // Per-content body-morph opt-in (default OFF). Body morph is only
            // needed for BodySlide/body-conforming meshes; it is off for everything
            // by default (accessories don't need it and it drove a memory balloon
            // via skee ApplyVertexDiff + SSE Engine Fixes arena retention).
            //   cef morph                 - list active items + their state
            //   cef morph <id> on|off     - set, then re-inject the item
            if (rest.empty()) {
                SKSE::GetTaskInterface()->AddTask([] {
                    auto* c = RE::ConsoleLog::GetSingleton();
                    if (c) {
                        ConsolePrint("[CEF] body morph (ON = applied; default off):");
                    }
                    for (const auto& it : ActiveSnapshot()) {
                        const std::string line =
                            std::string("  ") + (BodyMorphOn(it.id) ? "ON  " : "off ") + it.id;
                        SKSE::log::info("{}", line);
                        if (c) {
                            ConsolePrint(line.c_str());
                        }
                    }
                });
                return;
            }
            const std::string low = Lower(rest);
            std::string id;
            bool on = false;
            if (low.size() > 3 && low.compare(low.size() - 3, 3, " on") == 0) {
                on = true;
                id = Trim(rest.substr(0, rest.size() - 3));
            } else if (low.size() > 4 && low.compare(low.size() - 4, 4, " off") == 0) {
                on = false;
                id = Trim(rest.substr(0, rest.size() - 4));
            } else {
                Print("[CEF] usage: cef morph [<FormID:Plugin.esp> on|off]");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([id, on] {
                // Report the refusal (a published costume's look is frozen, an
                // unheld id has nowhere to store the setting) - re-injecting and
                // printing success either way said the opposite of the truth.
                if (!SetBodyMorphOn(id, on)) {
                    ConsolePrint("[CEF] body morph: refused (see log) - published "
                                 "costumes are frozen; unpublish first");
                    return;
                }
                HideInjectedNodes(id);  // drop the node so Reconcile re-injects with the new decision
                Reconcile();
                ConsolePrint(std::string("[CEF] body morph ") + (on ? "ON" : "off") +
                             " for " + id + " (re-injected)");
            });
        } else if (sub == "shapes") {
            // List a content's skinned shapes (name + dismember biped slot) so the
            // user knows what to hide. Loads the NIF on the main thread and caches
            // the result for the MCM.  cef shapes <FormID:Plugin.esp>
            if (rest.empty()) {
                Print("[CEF] usage: cef shapes <FormID:Plugin.esp>");
                return;
            }
            const std::string id = Trim(rest);
            SKSE::GetTaskInterface()->AddTask([id] {
                const auto shapes = EnumerateContentShapes(id);
                auto* c = RE::ConsoleLog::GetSingleton();
                if (shapes.empty()) {
                    SKSE::log::warn("shapes: '{}' - none (unresolved content or empty NIF)", id);
                    if (c) {
                        ConsolePrint(("[CEF] shapes '" + id + "': none (unresolved / empty NIF)").c_str());
                    }
                    return;
                }
                SKSE::log::info("shapes for '{}': {} shape(s)", id, shapes.size());
                if (c) {
                    ConsolePrint(("[CEF] shapes for " + id + " (ON = hidden):").c_str());
                }
                for (const auto& [name, slot] : shapes) {
                    const std::string line = std::string("  ") +
                        (IsHideShape(id, name) ? "ON  " : "off ") + name +
                        " [slot " + std::to_string(slot) + "]";
                    SKSE::log::info("{}", line);
                    if (c) {
                        ConsolePrint(line.c_str());
                    }
                }
            });
        } else if (sub == "hideshape") {
            // Toggle ONE shape of a content in its hide set, then re-inject.
            //   cef hideshape <FormID:Plugin.esp> <shapeName>
            const auto shapeSp = rest.find_last_of(' ');
            if (rest.empty() || shapeSp == std::string::npos) {
                Print("[CEF] usage: cef hideshape <FormID:Plugin.esp> <shapeName>");
                return;
            }
            const std::string id = Trim(rest.substr(0, shapeSp));
            const std::string shape = Trim(rest.substr(shapeSp + 1));
            if (id.empty() || shape.empty()) {
                Print("[CEF] usage: cef hideshape <FormID:Plugin.esp> <shapeName>");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([id, shape] {
                const bool now = !IsHideShape(id, shape);
                if (!SetHideShape(id, shape, now)) {
                    ConsolePrint("[CEF] hideshape: refused (see log) - published "
                                 "costumes are frozen; unpublish first");
                    return;
                }
                HideInjectedNodes(id);  // drop the node so Reconcile re-injects with the new decision
                Reconcile();
                ConsolePrint(std::string("[CEF] hideshape ") + (now ? "ON" : "off") + " '" +
                             shape + "' for " + id + " (re-injected)");
            });
        } else if (sub == "headdiag") {
            // FSMP approach-C passive PoC: enumerate FSMP-renamed physics bones on
            // the live skeleton(s). Apply an SMP hair first to see "_Head_" bones.
            SKSE::GetTaskInterface()->AddTask([] { HeadDiag(); });
        } else if (sub == "hair") {
            // FSMP approach-C active PoC (stage 1): ChangeHeadPart(<SMP hair HDPT>)
            // + DoReset3D from CEF code, then `cef headdiag` to see if (2) fired.
            if (rest.find(':') == std::string::npos) {
                Print("[CEF] usage: cef hair <HeadPart FormID:Plugin.esp>");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([rest] {
                const bool ok = ChangeHeadPartPoC(rest);
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint(ok ? "[CEF] head part changed + reset 3D; run 'cef headdiag'"
                                : "[CEF] hair PoC FAILED (see log)");
                }
            });
        } else if (sub == "arraytest" || sub == "nodediag" || sub == "slottest") {
            // persist-CTD harness (BUGREPORT_2026-07-27 / uint16-pattern
            // 2026-07-30). All run on the task pump and print to console AND
            // log, so a tester can paste either.
            const std::string which = sub;
            SKSE::GetTaskInterface()->AddTask([which] {
                const auto lines = which == "arraytest" ? ChildArrayProbe()
                                 : which == "slottest"  ? SlotCorruptionProbe()
                                                        : ChildArrayScan();
                SKSE::log::info("--- cef {} ---", which);
                for (const auto& l : lines) {
                    SKSE::log::info("  {}", l);
                }
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint(std::format("[CEF] {}:", which).c_str());
                    for (const auto& l : lines) {
                        ConsolePrint(("  " + l).c_str());
                    }
                }
            });
        } else if (sub == "npcpersist") {
            const auto split = rest.find(' ');
            const auto op = Lower(split == std::string::npos ? rest : rest.substr(0, split));
            if (op.empty() || op == "list") {
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    for (const auto& item : NprAssignmentsSnapshot()) {
                        const auto line = std::format("[CEF] npcpersist {} actor={:08X} contents={}{}{}",
                            item.poolSlot + 1, item.actorFormID, item.contents.size(),
                            item.unresolved ? " (unresolved)" : "",
                            item.restoreSuspended ? " (restore suspended)" : "");
                        ConsolePrint(line.c_str());
                    }
                }
                return;
            }
            // The engine hands the console-selected reference straight into
            // CompileAndRun (a_target). RE::Console::GetSelectedRef() is kept
            // only as a fallback - its NG implementation reads a raw offset
            // that misses on 1.6.1170 and returns null (field-hit 2026-08-04).
            auto* actor = a_target ? a_target->As<RE::Actor>() : nullptr;
            if (!actor) {
                auto selected = RE::Console::GetSelectedRef();
                actor = selected ? selected->As<RE::Actor>() : nullptr;
            }
            if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
                Print("[CEF] npcpersist: click an NPC in the console first (its RefID "
                      "shows top-center), then run this with it still selected");
                return;
            }
            if (op == "add") {
                const std::string id = split == std::string::npos ? "" : Trim(rest.substr(split + 1));
                if (id.find(':') == std::string::npos) {
                    Print("[CEF] usage: cef npcpersist add <FormID:Plugin.esp>");
                    return;
                }
                const auto handle = actor->GetHandle();
                SKSE::GetTaskInterface()->AddTask([handle, id] {
                    auto ref = handle.get();
                    auto* target = ref ? ref.get()->As<RE::Actor>() : nullptr;
                    Print(AssignNpcPersist(target, { id }) ?
                        "[CEF] NPC persist assigned" : "[CEF] NPC persist assignment failed");
                });
            } else if (op == "remove") {
                const auto handle = actor->GetHandle();
                SKSE::GetTaskInterface()->AddTask([handle] {
                    auto ref = handle.get();
                    Print(RemoveNpcPersist(ref ? ref.get()->As<RE::Actor>() : nullptr) ?
                        "[CEF] NPC persist removed" : "[CEF] NPC has no persist assignment");
                });
            } else if (op == "refresh") {
                const auto handle = actor->GetHandle();
                SKSE::GetTaskInterface()->AddTask([handle] {
                    auto ref = handle.get();
                    Print(RefreshNpcPersist(ref ? ref.get()->As<RE::Actor>() : nullptr) ?
                        "[CEF] NPC persist refresh queued (re-equip lands in ~1s)" :
                        "[CEF] NPC has no persist assignment");
                });
            }
        } else if (sub == "pub") {
            if (rest.empty() || rest == "list") {
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    const auto bindings = PubBindingsSnapshot();
                    for (const auto& snap : PublishedSnapshot()) {
                        int holders = 0, wearers = 0, unresolved = 0;
                        for (const auto& binding : bindings) {
                            if (binding.pubSlot != snap.pubSlot) continue;
                            holders += binding.holder;
                            wearers += binding.wearer;
                            unresolved += binding.unresolved;
                        }
                        const auto line = std::format(
                            "[CEF] pub {} '{}' slot={} contents={} holders={} wearers={} unresolved={}",
                            snap.pubSlot + 1, snap.label, snap.sourceSlot, snap.contents.size(),
                            holders, wearers, unresolved);
                        ConsolePrint(line.c_str());
                    }
                    const auto cap = std::format("[CEF] injected NPCs: {} / {}",
                        InjectedNpcCount(), MaxNpcInjected());
                    ConsolePrint(cap.c_str());
                }
            } else if (rest == "diag") {
                if (auto* c = RE::ConsoleLog::GetSingleton())
                    for (const auto& line : NpcDiagLines()) ConsolePrint(line.c_str());
            } else {
                const auto split = rest.find(' ');
                const auto op = Lower(split == std::string::npos ? rest : rest.substr(0, split));
                int slot = -1;
                try { slot = std::stoi(split == std::string::npos ? "" : rest.substr(split + 1)) - 1; }
                catch (...) { Print("[CEF] usage: cef pub refresh|recall <1-8>"); return; }
                if (op == "refresh")
                    SKSE::GetTaskInterface()->AddTask([slot] { RefreshPubWearers(slot); });
                else if (op == "recall")
                    SKSE::GetTaskInterface()->AddTask([slot] { RecallPublished(slot); });
            }
        } else if (sub == "invisdiag") {
            // Invisibility-propagation groundwork: run once per stage
            // (before / fading / fully invisible / after dispel), then read the
            // log. Prints to console AND log so either can be pasted.
            SKSE::GetTaskInterface()->AddTask([] {
                const auto lines = InvisDiag();
                SKSE::log::info("--- cef invisdiag ---");
                for (const auto& l : lines) {
                    SKSE::log::info("  {}", l);
                }
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint("[CEF] invisdiag (also in the log):");
                    for (const auto& l : lines) {
                        ConsolePrint(("  " + l).c_str());
                    }
                }
            });
        } else if (sub == "recover") {
            // Deliberate escape hatch for the STORE-ONLY return rule: the MCM
            // return flows never fabricate an item (a store miss on this save
            // means the copy lives on another character - CEF_STATE_SCOPE.md §4).
            if (rest.find(':') == std::string::npos) {
                Print("[CEF] usage: cef recover <FormID:Plugin.esp>");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([rest] {
                const bool ok = RecoverContentItem(rest);
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    ConsolePrint(ok ? "[CEF] recover: granted 1 copy (see log)"
                                : "[CEF] recover: id does not resolve to an item");
                }
            });
        } else {
            Print("[CEF] inject | box | pub | npcpersist | detach | clear | list | store | repair | persist | "
                  "morph | shapes | hideshape | recover | headdiag | hair | nodediag | arraytest | "
                  "slottest | invisdiag");
        }
    }
}
