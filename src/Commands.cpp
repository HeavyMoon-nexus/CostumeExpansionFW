#include "Commands.h"
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
            Print("[CEF] inject | box | detach | clear | list");
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
        } else if (sub == "list") {
            SKSE::GetTaskInterface()->AddTask([] { ListActive(); });
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
        } else {
            Print("[CEF] inject | box | detach | clear | list | headdiag | hair");
        }
    }
}
