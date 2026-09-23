// registry_checksum - the DLL's registry checksum, runnable on the build host.
//
//   registry_checksum <in.json> <out.json> <out.compact>
//
// Reads a CEF_abilities.json-shaped file with nlohmann, the way CEF reads the
// registry at kDataLoaded, and writes it back the way SaveRegistry writes it:
// "checksum" recomputed over the compact dump of "abilities", the whole
// document as dump(2). The compact dump goes to <out.compact> and the checksum
// to stdout. A DEV tool driven by tests/registry_checksum_test.py (review
// 2026-09-23 M13), never packaged.

#include "RegistryChecksum.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    bool WriteText(const char* a_path, const std::string& a_text)
    {
        std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
        out << a_text;
        return static_cast<bool>(out);
    }
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: registry_checksum <in.json> <out.json> <out.compact>\n");
        return 2;
    }
    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        std::stringstream buf;
        buf << in.rdbuf();
        auto doc = nlohmann::json::parse(buf.str());
        const auto compact = doc.at("abilities").dump();
        const auto checksum = CostumeFW::abilities::RegistryChecksum(doc.at("abilities"));
        doc["checksum"] = checksum;
        if (!WriteText(argv[2], doc.dump(2)) || !WriteText(argv[3], compact)) {
            std::fprintf(stderr, "cannot write the output files\n");
            return 1;
        }
        std::printf("%s\n", checksum.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "registry_checksum: %s\n", e.what());
        return 1;
    }
}
