#include "Commands.h"
#include "BoxStore.h"
#include "SkinRebind.h"

#include "RE/S/Script.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        void Print(const char* a_msg)
        {
            if (auto* c = RE::ConsoleLog::GetSingleton()) {
                c->Print(a_msg);
            }
        }

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
                        HandleConsoleCommand(line);
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
        REL::Relocation<std::uintptr_t> target{
            REL::RelocationID(52065, 52952), REL::VariantOffset(0xE2, 0x52, 0xE2)
        };
        ConsoleHook::func = SKSE::GetTrampoline().write_call<5>(target.address(), ConsoleHook::thunk);
        SKSE::log::info("console hook installed (Script::CompileAndRun)");
    }

    void HandleConsoleCommand(const std::string& a_line)
    {
        // a_line = "cef <sub> <rest...>". Drop the "cef" token.
        const std::string afterPrefix = Trim(a_line.substr(3));
        if (afterPrefix.empty()) {
            Print("[CEF] inject | box | detach | clear | list | repair | persist | morph | shapes | hideshape | recover | headdiag | hair");
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
                    c->Print(("[CEF] nuke removed " + std::to_string(n) + " node(s)").c_str());
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
                    c->Print(("[CEF] repair: re-injected (" + std::to_string(n) +
                              " node(s) detached)").c_str());
                }
            });
        } else if (sub == "list") {
            SKSE::GetTaskInterface()->AddTask([] { ListActive(); });
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
                        c->Print(msg.c_str());
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
                        c->Print("[CEF] body morph (ON = applied; default off):");
                    }
                    for (const auto& it : ActiveSnapshot()) {
                        const std::string line =
                            std::string("  ") + (BodyMorphOn(it.id) ? "ON  " : "off ") + it.id;
                        SKSE::log::info("{}", line);
                        if (c) {
                            c->Print(line.c_str());
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
                SetBodyMorphOn(id, on);
                HideInjectedNodes(id);  // drop the node so Reconcile re-injects with the new decision
                Reconcile();
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    c->Print((std::string("[CEF] body morph ") + (on ? "ON" : "off") +
                              " for " + id + " (re-injected)")
                                 .c_str());
                }
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
                        c->Print(("[CEF] shapes '" + id + "': none (unresolved / empty NIF)").c_str());
                    }
                    return;
                }
                SKSE::log::info("shapes for '{}': {} shape(s)", id, shapes.size());
                if (c) {
                    c->Print(("[CEF] shapes for " + id + " (ON = hidden):").c_str());
                }
                for (const auto& [name, slot] : shapes) {
                    const std::string line = std::string("  ") +
                        (IsHideShape(id, name) ? "ON  " : "off ") + name +
                        " [slot " + std::to_string(slot) + "]";
                    SKSE::log::info("{}", line);
                    if (c) {
                        c->Print(line.c_str());
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
                SetHideShape(id, shape, now);
                HideInjectedNodes(id);  // drop the node so Reconcile re-injects with the new decision
                Reconcile();
                if (auto* c = RE::ConsoleLog::GetSingleton()) {
                    c->Print((std::string("[CEF] hideshape ") + (now ? "ON" : "off") + " '" +
                              shape + "' for " + id + " (re-injected)")
                                 .c_str());
                }
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
                    c->Print(ok ? "[CEF] head part changed + reset 3D; run 'cef headdiag'"
                                : "[CEF] hair PoC FAILED (see log)");
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
                    c->Print(ok ? "[CEF] recover: granted 1 copy (see log)"
                                : "[CEF] recover: id does not resolve to an item");
                }
            });
        } else {
            Print("[CEF] inject | box | detach | clear | list | repair | persist | morph | shapes | hideshape | recover | headdiag | hair");
        }
    }
}
