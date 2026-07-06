// nifcarrier_cli - offline test host for nifcarrier_core (NIFCARRIER_INPROC.md
// I-7). Runs the same code the CEF DLL runs, outside the game, so T0-T3 stay
// verifiable without Skyrim. Verbs grow with the port; C# nifcarrier remains
// the golden oracle.

#include "nifcarrier/NifCarrierCore.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    const std::string verb = (argc >= 2) ? argv[1] : "";
    if (argc >= 4 && verb == "roundtrip") {
        const auto r = nifcarrier::RoundTrip(argv[2], argv[3]);
        std::printf("[roundtrip] %s shapes=%d->%d%s%s\n",
            r.ok ? "OK" : "FAILED", r.shapesBefore, r.shapesAfter,
            r.error.empty() ? "" : " ", r.error.c_str());
        return r.ok ? 0 : 1;
    }
    if (argc >= 4 && verb == "zeroalpha") {
        const auto r = nifcarrier::ZeroAlpha(argv[2], argv[3]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 3 && verb == "verifyzeroalpha") {
        const auto r = nifcarrier::VerifyZeroAlpha(argv[2]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 3 && verb == "hdtxml") {
        const auto p = nifcarrier::GetHdtXmlPath(argv[2]);
        std::printf("[hdtxml] '%s'\n", p.c_str());
        return p.empty() ? 1 : 0;
    }
    if (argc >= 5 && verb == "mergexml") {
        std::vector<std::filesystem::path> inputs;
        for (int i = 3; i < argc; ++i) {
            inputs.emplace_back(argv[i]);
        }
        const auto r = nifcarrier::MergeXml(argv[2], inputs);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 5 && verb == "setxml") {
        const auto r = nifcarrier::SetXml(argv[2], argv[3], argv[4]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 3 && verb == "validate") {
        const auto r = nifcarrier::Validate(argv[2]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 5 && verb == "merge") {
        std::vector<std::filesystem::path> inputs;
        for (int i = 3; i < argc; ++i) {
            inputs.emplace_back(argv[i]);
        }
        const auto r = nifcarrier::Merge(argv[2], inputs);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 3 && verb == "validatehead") {
        const auto r = nifcarrier::ValidateHead(argv[2]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 4 && verb == "proxynif") {
        const auto r = nifcarrier::ProxyNif(argv[2], argv[3]);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 4 && verb == "headcarrier") {
        std::filesystem::path out;
        std::vector<std::filesystem::path> contents;
        nifcarrier::HeadCarrierOptions opts;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--xml" && i + 1 < argc) {
                opts.xmlRel = argv[++i];
            } else if (a == "--anchor" && i + 1 < argc) {
                opts.anchorName = argv[++i];
            } else if (a == "--keep1") {
                opts.keep1 = true;
            } else if (a == "--proxyshader") {
                opts.proxyShader = true;
            } else if (out.empty()) {
                out = a;
            } else {
                contents.emplace_back(a);
            }
        }
        if (out.empty() || contents.empty()) {
            std::printf("usage: nifcarrier_cli headcarrier <out.nif> <content.nif> [content2 ...] [--xml <gameRelXml>] [--anchor <liveBoneName>] [--keep1] [--proxyshader]\n");
            return 2;
        }
        const auto r = nifcarrier::HeadCarrier(out, contents, opts);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 1;
    }
    if (argc >= 3 && verb == "sync") {
        nifcarrier::SyncOptions opts;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--data" && i + 1 < argc) {
                opts.dataRoots.emplace_back(argv[++i]);
            } else if (a == "--out" && i + 1 < argc) {
                opts.outRoot = argv[++i];
            } else if (a == "--empty" && i + 1 < argc) {
                opts.emptyNif = argv[++i];
            } else if (opts.manifestPath.empty()) {
                opts.manifestPath = a;
            }
        }
        const auto r = nifcarrier::Sync(opts);
        std::fputs(r.log.c_str(), stdout);
        return r.ok ? 0 : 2;
    }
    std::printf("usage: nifcarrier_cli roundtrip       <in.nif> <out.nif>\n"
                "       nifcarrier_cli zeroalpha       <in.nif> <out.nif>\n"
                "       nifcarrier_cli verifyzeroalpha <nif>\n"
                "       nifcarrier_cli merge           <out.nif> <base.nif> <in2.nif> [...]\n"
                "       nifcarrier_cli validate        <nif>\n"
                "       nifcarrier_cli hdtxml          <nif>\n"
                "       nifcarrier_cli mergexml        <out.xml> <in1.xml> <in2.xml> [...]\n"
                "       nifcarrier_cli setxml          <in.nif> <out.nif> <xmlPath>\n"
                "       nifcarrier_cli validatehead    <nif>\n"
                "       nifcarrier_cli proxynif        <in.nif> <out.nif>\n"
                "       nifcarrier_cli headcarrier     <out.nif> <content.nif> [...] [--xml <x>] [--anchor <a>] [--keep1] [--proxyshader]\n"
                "       nifcarrier_cli sync            <manifest.json> --data <root> [--data ...] --out <cefModRoot> [--empty <emptyToken.nif>]\n");
    return 2;
}
