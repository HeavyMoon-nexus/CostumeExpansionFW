#pragma once

// nifcarrier_core - in-proc port of tools/nifcarrier (NIFCARRIER_INPROC.md).
// Engine-free by design: std + nifly only, no CommonLibSSE/RE types. The same
// static library links into the CEF DLL and into nifcarrier_cli.exe so the
// T0-T3 offline checks keep running outside the game (I-7).

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nifcarrier {

    struct RoundTripResult
    {
        bool ok = false;
        int shapesBefore = 0;
        int shapesAfter = 0;
        std::string error;
    };

    // T0: load -> save -> reload with shape-count parity; the C# SaveReloadOk
    // equivalent. Never throws - nifly parse failures come back in `error`.
    RoundTripResult RoundTrip(const std::filesystem::path& in, const std::filesystem::path& out);

    // Shape count of a NIF as nifly sees it, -1 on load failure. Never throws.
    int CountShapes(const std::filesystem::path& path);

    // Verb outcome + the human-readable trace destined for CEF_sync.log. Lines
    // match the C# tool's [zeroalpha]/[sync] format so diagnostics stay
    // greppable across both implementations.
    struct VerbResult
    {
        bool ok = false;
        std::string log;
    };

    // Single-content carrier build step: shader alpha -> 0 + NiAlphaProperty
    // with blend flags, per shape; shapes without a shader (collision proxies)
    // pass through. Saves to `out`, then reload-verifies. C# ZeroAlpha port.
    VerbResult ZeroAlpha(const std::filesystem::path& in, const std::filesystem::path& out);

    // Reload-side invariant check of a zero-alpha'd NIF (any producer): all
    // shader alphas 0, all blend flags set, skinned, HDT extra present. Used
    // for cross-implementation golden checks against the C# oracle's output.
    VerbResult VerifyZeroAlpha(const std::filesystem::path& path);

    // HDT physics XML path referenced by the NIF's extra data; empty when the
    // NIF is unreadable or carries no HDT extra (= not an SMP content).
    std::string GetHdtXmlPath(const std::filesystem::path& nifPath);

    // Merge several HDT-SMP physics XMLs into one <system> document, isolating
    // each document's <*-default> template scope via cef_factory snapshots and
    // dropping duplicate <bone name> entries (first wins). C# MergeXml port.
    VerbResult MergeXml(const std::filesystem::path& out, const std::vector<std::filesystem::path>& inputs);

    // Re-point the carrier's root HDT extra data at a different physics XML.
    VerbResult SetXml(const std::filesystem::path& in, const std::filesystem::path& out,
        const std::string& xmlPath);

    // Rewrite per-*-shape name attributes per the pool assignment (persist).
    VerbResult RenameShapesInXml(const std::filesystem::path& in, const std::filesystem::path& out,
        const std::map<std::string, std::string>& renames);

    // Set/overwrite "count" in a persist cosave fragment (JSON). Unparseable
    // fragments pass through unchanged; empty stays empty.
    std::string FragmentWithCount(const std::string& fragment, int count);

    // Defense A/B gate: would the SSE engine's skin-partition loader
    // divide-by-zero on this NIF? (non-SSE geometry blocks, or a skinned shape
    // with 0 vertices - the Box44 CTD root cause, 2026-07-03.) C# Validate port.
    VerbResult Validate(const std::filesystem::path& path);

    // Offline gate for a facegen-path carrier: R1 (root HDT extra), R2 (merge
    // trigger shape), R3 (custom bones tree-reachable), R4 (no shader-less
    // geometry) + the SSE divide-by-zero gate. C# ValidateHead port.
    VerbResult ValidateHead(const std::filesystem::path& path);

    struct HeadCarrierOptions
    {
        std::string xmlRel;      // re-point HDT extra ("" = keep base's xml)
        std::string anchorName;  // anchor re-route ("" = none)
        bool keep1 = false;
        bool proxyShader = false;
    };

    // One-shot facegen-path carrier build: base pick -> merge -> zero-alpha ->
    // R3 completion -> (keep1/anchor/xml) -> R1 fix-up -> validatehead gate.
    VerbResult HeadCarrier(const std::filesystem::path& out,
        const std::vector<std::filesystem::path>& contents, const HeadCarrierOptions& opts);

    // Collision-proxy companion NIF for the ExtraParts pattern: keep only the
    // shader-less skinned proxies, give them a borrowed shader, zero-alpha,
    // strip the HDT extra. C# ProxyNif port.
    VerbResult ProxyNif(const std::filesystem::path& in, const std::filesystem::path& out);

    // Level-3 merge carrier: bone union of every input into inputs[0]'s tree
    // (hierarchy walk + skin-only bone rescue), shapes cloned per content and
    // dropped back to bones-only when the clone fails SaveReloadOk - one
    // corrupting content never strips the other contents' collision shapes.
    // C# Merge port; doubles as the I-3 CloneShape corruption testbed.
    VerbResult Merge(const std::filesystem::path& out, const std::vector<std::filesystem::path>& inputs);

    // --- sync driver (production entry point) -------------------------------

    struct SyncOptions
    {
        std::filesystem::path manifestPath;               // CEF_carrier_manifest.json
        std::vector<std::filesystem::path> dataRoots;     // in-game: { "Data" } (usvfs resolves)
        std::filesystem::path outRoot;                    // in-game: "Data"
        std::filesystem::path emptyNif;                   // empty-token template ("" = none)
    };

    struct SyncResult
    {
        bool ok = false;  // failed == 0
        int built = 0;
        int skipped = 0;
        int failed = 0;
        std::string log;
    };

    // Full manifest-driven rebuild: per box (0/1/2+ SMP contents), hash skip,
    // revision slots, carriers.json, then the persist pipeline. C# Sync port
    // minus the --mo2/modlist layer (in-game the VFS resolves "Data\..."; the
    // CLI host passes explicit data roots).
    SyncResult Sync(const SyncOptions& opts);

}  // namespace nifcarrier
