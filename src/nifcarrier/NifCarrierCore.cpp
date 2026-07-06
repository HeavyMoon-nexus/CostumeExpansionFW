#include "nifcarrier/NifCarrierCore.h"

#include <NifFile.hpp>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

#include <windows.h>

#include <bcrypt.h>

namespace nifcarrier {

    namespace {

        constexpr const char* kHdtExtra = "HDT Skinned Mesh Physics Object";
        // blend enable | src=SRC_ALPHA | dst=INV_SRC_ALPHA (community-standard
        // invisible-but-equipped mesh; FSMP_REINVESTIGATION.md §3-6)
        constexpr uint16_t kAlphaBlendFlags = 237;

        void Log(std::string& log, const char* fmt, ...)
        {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            log += buf;
            log += '\n';
        }

        bool HasHdtExtra(const nifly::NifFile& nif)
        {
            const auto& hdr = nif.GetHeader();
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* sed = hdr.GetBlock<nifly::NiStringExtraData>(i)) {
                    if (sed->name.get() == kHdtExtra) {
                        return true;
                    }
                }
            }
            return false;
        }

        // Exact-type NiNode (not BSFadeNode etc.), matching the C# tool's
        // `GetType().Name == "NiNode"` filter.
        bool IsExactNiNode(nifly::NiObject* block)
        {
            return std::strcmp(block->GetBlockName(), nifly::NiNode::BlockName) == 0;
        }

        int CountNamedNodes(const nifly::NifFile& nif)
        {
            const auto& hdr = nif.GetHeader();
            int n = 0;
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* node = hdr.GetBlock<nifly::NiNode>(i)) {
                    if (IsExactNiNode(node) && !node->name.get().empty()) {
                        ++n;
                    }
                }
            }
            return n;
        }

        void Report(std::string& log, const nifly::NifFile& nif, const char* tag)
        {
            Log(log, "--- %s: blocks=%u shapes=%zu namedNiNodes=%d hdtExtra=%s", tag,
                nif.GetHeader().GetNumBlocks(), nif.GetShapes().size(), CountNamedNodes(nif),
                HasHdtExtra(nif) ? "True" : "False");
        }

        // Points at the shader's mutable alpha. Only BSLightingShaderProperty
        // carries a writable one in nifly (NiMaterialProperty's is protected,
        // LE-only; BSEffectShaderProperty has none) - every other type warns
        // and skips, matching the C# tool's missing-`_alpha` reflection path.
        float* ShaderAlpha(nifly::NiShader* shader)
        {
            if (auto* l = dynamic_cast<nifly::BSLightingShaderProperty*>(shader)) {
                return &l->alpha;
            }
            return nullptr;
        }

        // Shared reload-side invariant check (ZeroAlpha self-verify + the
        // standalone VerifyZeroAlpha verb).
        bool CheckZeroAlpha(std::string& log, const nifly::NifFile& nif, const char* tag)
        {
            bool allZero = true;
            bool allBlend = true;
            bool skinned = false;
            const auto shapes = nif.GetShapes();
            for (auto* shape : shapes) {
                if (shape->HasSkinInstance()) {
                    skinned = true;
                }
                auto* shader = nif.GetShader(shape);
                if (!shader) {
                    continue;  // non-rendered collision proxy - invisible by construction
                }
                if (shader->GetAlpha() != 0.0f) {
                    allZero = false;
                }
                auto* ap = nif.GetAlphaProperty(shape);
                if (!ap || ap->flags != kAlphaBlendFlags) {
                    allBlend = false;
                }
            }
            const bool hdt = HasHdtExtra(nif);
            Log(log, "[%s] VERDICT shaderAlphaZero=%s blendFlags=%s shapes=%zu skinned=%s hdtExtra=%s",
                tag, allZero ? "True" : "False", allBlend ? "True" : "False", shapes.size(),
                skinned ? "True" : "False", hdt ? "True" : "False");
            return allZero && allBlend && !shapes.empty() && skinned && hdt;
        }

        // --- merge internals (C# MergeBranch/EnsureBone/MergeBones ports) ---

        // Merge srcNode's bone branch into dst: reuse an existing same-named
        // bone or clone it under the name-matched parent (else dst root), then
        // recurse children. Same algorithm as FSMP's doSkeletonMerge, so the
        // union stays FSMP-consistent.
        void MergeBranch(nifly::NifFile& dst, nifly::NiNode* srcNode, nifly::NiNode* dstRoot, nifly::NifFile& src)
        {
            const std::string boneName = srcNode->name.get();
            if (boneName.empty()) {
                return;
            }

            nifly::NiNode* nodeParent = dstRoot;
            if (auto* srcParent = src.GetParentNode(srcNode)) {
                const std::string pn = srcParent->name.get();
                if (!pn.empty()) {
                    if (auto* p = dst.FindBlockByName<nifly::NiNode>(pn)) {
                        nodeParent = p;
                    }
                }
            }

            auto* existing = dst.FindBlockByName<nifly::NiNode>(boneName);
            if (!existing || dst.GetBlockID(existing) == nifly::NIF_NPOS) {
                const uint32_t boneID = dst.CloneNamedNode(boneName, &src);
                nodeParent->childRefs.AddBlockRef(boneID);
            }

            for (auto& childRef : srcNode->childRefs) {
                if (auto* childNode = src.GetHeader().GetBlock<nifly::NiNode>(childRef)) {
                    MergeBranch(dst, childNode, dstRoot, src);
                }
            }
        }

        // Ensure a source bone (and its ancestor chain) exists in dst - the
        // rescue for physics bones referenced ONLY by a shape's skin and absent
        // from the node hierarchy (Pharaoh_Veil's PhSVeil_*). Idempotent.
        nifly::NiNode* EnsureBone(nifly::NifFile& dst, nifly::NifFile& src, nifly::NiNode* srcBone,
            nifly::NiNode* dstRoot)
        {
            const std::string bn = srcBone->name.get();
            if (bn.empty()) {
                return dstRoot;
            }
            if (auto* existing = dst.FindBlockByName<nifly::NiNode>(bn)) {
                return existing;
            }
            nifly::NiNode* dparent = dstRoot;
            auto* sp = src.GetParentNode(srcBone);
            if (sp && !sp->name.get().empty() && sp != src.GetRootNode()) {
                dparent = EnsureBone(dst, src, sp, dstRoot);  // parent first (chain-safe order)
            }
            const uint32_t bid = dst.CloneNamedNode(bn, &src);
            dparent->childRefs.AddBlockRef(bid);
            auto* created = dst.FindBlockByName<nifly::NiNode>(bn);
            return created ? created : dstRoot;
        }

        // Bone union of one source into dst: hierarchy-reachable branch first,
        // then EVERY remaining named (exact-type) NiNode. Returns bones added.
        int MergeBones(nifly::NifFile& dst, nifly::NifFile& src, nifly::NiNode* dstRoot)
        {
            const int before = CountNamedNodes(dst);
            auto* srcRoot = src.GetRootNode();
            for (auto& childRef : srcRoot->childRefs) {
                if (auto* childNode = src.GetHeader().GetBlock<nifly::NiNode>(childRef)) {
                    MergeBranch(dst, childNode, dstRoot, src);
                }
            }
            const auto& hdr = src.GetHeader();
            std::vector<nifly::NiNode*> named;
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* node = hdr.GetBlock<nifly::NiNode>(i)) {
                    if (IsExactNiNode(node) && !node->name.get().empty()) {
                        named.push_back(node);
                    }
                }
            }
            for (auto* b : named) {
                EnsureBone(dst, src, b, dstRoot);
            }
            return CountNamedNodes(dst) - before;
        }

        // Geometry block types that must NOT appear in an SSE carrier: a
        // skinned NiTriShape (LE/Oldrim) carrying an SSE NiSkinPartition is
        // the classic EXCEPTION_INT_DIVIDE_BY_ZERO in the engine's
        // skin-partition loader (Box44 CTD root cause, 2026-07-03).
        constexpr const char* kNonSseGeo[] = {
            "NiTriShape", "NiTriShapeData", "NiTriStrips", "NiTriStripsData"
        };

        bool ValidateNifSkinnable(const nifly::NifFile& nif, std::string& reason)
        {
            const auto& hdr = nif.GetHeader();
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                auto* b = hdr.GetBlock<nifly::NiObject>(i);
                if (!b) {
                    continue;
                }
                for (const char* bad : kNonSseGeo) {
                    if (std::strcmp(b->GetBlockName(), bad) == 0) {
                        reason = std::string("non-SSE geometry block ") + bad +
                                 " (run SSE NIF Optimizer on the source)";
                        return false;
                    }
                }
            }
            for (auto* s : nif.GetShapes()) {
                if (s->HasSkinInstance() && s->GetNumVertices() == 0) {
                    reason = "skinned shape '" + s->name.get() + "' has 0 vertices";
                    return false;
                }
            }
            reason.clear();
            return true;
        }

        // True if nif saves to path AND reloads valid - the gate that keeps a
        // corrupting content bones-only WITHOUT losing the others' shapes.
        bool SaveReloadOk(nifly::NifFile& nif, const std::filesystem::path& path)
        {
            try {
                if (nif.Save(path) != 0) {
                    return false;
                }
                nifly::NifFile chk;
                return chk.Load(path) == 0 && chk.IsValid();
            } catch (...) {
                return false;
            }
        }

        bool ValidateNifPathImpl(const std::filesystem::path& path, std::string& reason)
        {
            nifly::NifFile nif;
            if (nif.Load(path) != 0 || !nif.IsValid()) {
                reason = "unreadable / invalid NIF";
                return false;
            }
            return ValidateNifSkinnable(nif, reason);
        }

        // --- head-path internals (C# approach-C helpers) ---------------------

        // Vanilla / XPMSSE skeleton nodes follow well-known naming; custom
        // physics bones don't. In-game `cef headdiag` remains the ground truth.
        constexpr const char* kLiveBonePrefixes[] = {
            "NPC ", "CME ", "MOV ", "AnimObject", "Camera", "Weapon", "Shield",
            "Quiver", "SaddleBone", "HorseSpine", "Bip01"
        };
        constexpr const char* kLiveBoneExact[] = {
            "NPC", "Root", "MagicEffectsNode", "BSFaceGenNiNodeSkinned"
        };

        bool CiStartsWith(const std::string& s, const char* prefix)
        {
            const std::size_t n = std::strlen(prefix);
            if (s.size() < n) {
                return false;
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (std::tolower(static_cast<unsigned char>(s[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    return false;
                }
            }
            return true;
        }

        bool CiEquals(const std::string& s, const char* other)
        {
            return s.size() == std::strlen(other) && CiStartsWith(s, other);
        }

        bool LooksLiveSkeletonBone(const std::string& name)
        {
            if (name.empty()) {
                return true;
            }
            for (const char* p : kLiveBonePrefixes) {
                if (CiStartsWith(name, p)) {
                    return true;
                }
            }
            for (const char* e : kLiveBoneExact) {
                if (CiEquals(name, e)) {
                    return true;
                }
            }
            return false;
        }

        // First HDT extra data in header order (the C# Blocks.OfType pattern).
        nifly::NiStringExtraData* FirstHdtExtra(const nifly::NifFile& nif)
        {
            const auto& hdr = nif.GetHeader();
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* sed = hdr.GetBlock<nifly::NiStringExtraData>(i)) {
                    if (sed->name.get() == kHdtExtra) {
                        return sed;
                    }
                }
            }
            return nullptr;
        }

        // The root-attached HDT extra data (or nullptr): facegen R1.
        nifly::NiStringExtraData* RootHdtExtra(const nifly::NifFile& nif)
        {
            auto* root = nif.GetRootNode();
            if (!root) {
                return nullptr;
            }
            for (auto& ref : root->extraDataRefs) {
                if (auto* sed = nif.GetHeader().GetBlock<nifly::NiStringExtraData>(ref)) {
                    if (sed->name.get() == kHdtExtra) {
                        return sed;
                    }
                }
            }
            return nullptr;
        }

        // Re-attach a stray HDT extra to the root's extra-data list (R1 fix-up).
        bool EnsureRootHdtExtra(nifly::NifFile& nif, std::string& log)
        {
            if (RootHdtExtra(nif)) {
                return true;
            }
            auto* stray = FirstHdtExtra(nif);
            if (!stray) {
                return false;
            }
            const uint32_t id = nif.GetBlockID(stray);
            if (id == nifly::NIF_NPOS) {
                return false;
            }
            nif.GetRootNode()->extraDataRefs.AddBlockRef(id);
            Log(log, "[headcarrier] HDT extra data was not root-attached - re-attached to root");
            return RootHdtExtra(nif) != nullptr;
        }

        // All node names reachable from the root via children (the tree
        // doSkeletonMerge actually walks). Root's own name included.
        void TreeNodeNamesWalk(const nifly::NifFile& nif, nifly::NiNode* n, std::set<std::string>& names)
        {
            if (!n) {
                return;
            }
            const std::string nm = n->name.get();
            if (!nm.empty() && !names.insert(nm).second) {
                return;  // cycle/dup guard
            }
            for (auto& ref : n->childRefs) {
                if (auto* c = nif.GetHeader().GetBlock<nifly::NiNode>(ref)) {
                    TreeNodeNamesWalk(nif, c, names);
                }
            }
        }

        std::set<std::string> TreeNodeNames(const nifly::NifFile& nif)
        {
            std::set<std::string> names;
            TreeNodeNamesWalk(nif, nif.GetRootNode(), names);
            return names;
        }

        // R3 fix-up: parent every orphaned named NiNode (referenced only from a
        // skin instance, not the scene graph) under the root.
        int CompleteBoneTree(nifly::NifFile& nif, std::string& log)
        {
            auto* root = nif.GetRootNode();
            int reparented = 0;
            const auto& hdr = nif.GetHeader();
            std::vector<nifly::NiNode*> named;
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* node = hdr.GetBlock<nifly::NiNode>(i)) {
                    if (IsExactNiNode(node) && !node->name.get().empty()) {
                        named.push_back(node);
                    }
                }
            }
            for (auto* node : named) {
                if (node == root || nif.GetParentNode(node) != nullptr) {
                    continue;
                }
                const uint32_t id = nif.GetBlockID(node);
                if (id == nifly::NIF_NPOS) {
                    continue;
                }
                root->childRefs.AddBlockRef(id);
                ++reparented;
                Log(log, "[headcarrier] orphan bone '%s' re-parented under root (was skin-only)",
                    node->name.get().c_str());
            }
            return reparented;
        }

        // Anchor re-route that reuses an existing same-named node and leaves
        // live-named branches at root (APPROACH_C §9-10 recipe).
        std::pair<int, int> AnchorInMemory(nifly::NifFile& nif, const std::string& anchorName)
        {
            auto* root = nif.GetRootNode();
            auto* anchor = nif.FindBlockByName<nifly::NiNode>(anchorName);
            if (!anchor) {
                auto node = std::make_unique<nifly::NiNode>();
                node->name.get() = anchorName;
                anchor = node.get();
                const uint32_t anchorId = nif.GetHeader().AddBlock(std::move(node));
                root->childRefs.AddBlockRef(anchorId);
            }
            int moved = 0;
            int kept = 0;
            // Snapshot the original count: AddBlockRef during the loop appends
            // to anchor's array only, but the anchor ref itself may sit in
            // root's array - the identity check skips it either way.
            const uint32_t count = root->childRefs.GetSize();
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t idx = root->childRefs.GetBlockRef(i);
                auto* n = nif.GetHeader().GetBlock<nifly::NiNode>(idx);
                if (!n || n == anchor) {
                    continue;
                }
                const std::string nm = n->name.get();
                if (!nm.empty() && LooksLiveSkeletonBone(nm)) {
                    ++kept;
                    continue;
                }
                anchor->childRefs.AddBlockRef(idx);
                root->childRefs.SetBlockRef(i, nifly::NIF_NPOS);
                ++moved;
            }
            return { moved, kept };
        }

        std::vector<std::string> ShapeBoneNames(const nifly::NifFile& nif, nifly::NiShape* shape)
        {
            std::vector<std::string> names;
            nif.GetShapeBoneList(shape, names);
            return names;
        }

    }  // namespace

    RoundTripResult RoundTrip(const std::filesystem::path& in, const std::filesystem::path& out)
    {
        RoundTripResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(in) != 0 || !nif.IsValid()) {
                r.error = "load failed: " + in.string();
                return r;
            }
            r.shapesBefore = static_cast<int>(nif.GetShapes().size());
            if (nif.Save(out) != 0) {
                r.error = "save failed: " + out.string();
                return r;
            }
            nifly::NifFile chk;
            if (chk.Load(out) != 0 || !chk.IsValid()) {
                r.error = "reload failed: " + out.string();
                return r;
            }
            r.shapesAfter = static_cast<int>(chk.GetShapes().size());
            if (r.shapesAfter != r.shapesBefore) {
                r.error = "shape count changed across round-trip";
                return r;
            }
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = std::string("exception: ") + e.what();
        } catch (...) {
            r.error = "unknown exception";
        }
        return r;
    }

    int CountShapes(const std::filesystem::path& path)
    {
        try {
            nifly::NifFile nif;
            if (nif.Load(path) != 0 || !nif.IsValid()) {
                return -1;
            }
            return static_cast<int>(nif.GetShapes().size());
        } catch (...) {
            return -1;
        }
    }

    VerbResult ZeroAlpha(const std::filesystem::path& in, const std::filesystem::path& out)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(in) != 0 || !nif.IsValid()) {
                Log(r.log, "[zeroalpha] FAILED to load %s", in.string().c_str());
                return r;
            }
            Report(r.log, nif, "BEFORE");

            int done = 0;
            for (auto* shape : nif.GetShapes()) {
                const std::string shapeName = shape->name.get();

                // 1. shader alpha -> 0
                auto* shader = nif.GetShader(shape);
                if (!shader) {
                    // No shader = the engine never renders it (collision-proxy
                    // shapes like 'VirtualHead'). Already invisible.
                    Log(r.log, "[zeroalpha] shape '%s': no shader (non-rendered collision proxy), left as-is",
                        shapeName.c_str());
                    ++done;
                    continue;
                }
                float* alpha = ShaderAlpha(shader);
                if (!alpha) {
                    Log(r.log, "[zeroalpha] WARNING: shader %s has no alpha field, skipping",
                        shader->GetBlockName());
                    continue;
                }
                *alpha = 0.0f;

                // 2. ensure a NiAlphaProperty with blending enabled so alpha is honored
                auto* alphaProp = nif.GetAlphaProperty(shape);
                if (!alphaProp) {
                    nif.AssignAlphaProperty(shape, std::make_unique<nifly::NiAlphaProperty>());
                    alphaProp = nif.GetAlphaProperty(shape);
                }
                if (!alphaProp) {
                    Log(r.log, "[zeroalpha] WARNING: failed to assign NiAlphaProperty on '%s', skipping",
                        shapeName.c_str());
                    continue;
                }
                alphaProp->flags = kAlphaBlendFlags;
                alphaProp->threshold = 0;
                Log(r.log, "[zeroalpha] shape '%s': shader alpha=0, NiAlphaProperty flags=%u",
                    shapeName.c_str(), kAlphaBlendFlags);
                ++done;
            }
            if (done == 0) {
                Log(r.log, "[zeroalpha] FAILED: no shape processed");
                return r;
            }

            if (nif.Save(out) != 0) {
                Log(r.log, "[zeroalpha] FAILED to save %s", out.string().c_str());
                return r;
            }
            Log(r.log, "[zeroalpha] wrote %s", out.string().c_str());

            // Reload and verify
            nifly::NifFile chk;
            if (chk.Load(out) != 0 || !chk.IsValid()) {
                Log(r.log, "[zeroalpha] FAILED: output does not reload");
                return r;
            }
            Report(r.log, chk, "RELOAD");
            r.ok = CheckZeroAlpha(r.log, chk, "zeroalpha");
        } catch (const std::exception& e) {
            Log(r.log, "[zeroalpha] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[zeroalpha] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    std::string GetHdtXmlPath(const std::filesystem::path& nifPath)
    {
        try {
            nifly::NifFile nif;
            if (nif.Load(nifPath) != 0 || !nif.IsValid()) {
                return {};
            }
            const auto& hdr = nif.GetHeader();
            for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                if (auto* sed = hdr.GetBlock<nifly::NiStringExtraData>(i)) {
                    if (sed->name.get() == kHdtExtra) {
                        return sed->stringData.get();
                    }
                }
            }
            return {};
        } catch (...) {
            return {};
        }
    }

    VerbResult MergeXml(const std::filesystem::path& out, const std::vector<std::filesystem::path>& inputs)
    {
        // FSMP parser semantics that shape this (hdtSkyrimSystem.cpp, verified
        // on dev): anonymous <bone-default> overwrites the "" template used by
        // every following un-templated <bone>, so each document boundary
        // re-stores the pristine template via the cef_factory snapshot; the
        // parser skips duplicate <bone name> entries and we drop them too so
        // shared anchors (NPC Pelvis...) merge cleanly.
        static constexpr const char* kDefaultKinds[] = {
            "bone-default", "generic-constraint-default",
            "stiffspring-constraint-default", "conetwist-constraint-default"
        };
        VerbResult r;
        try {
            if (inputs.size() < 2) {
                Log(r.log, "[mergexml] need at least 2 input xml files");
                return r;
            }
            std::deque<pugi::xml_document> docs;
            for (const auto& in : inputs) {
                auto& d = docs.emplace_back();
                // parse_comments/parse_pi: XDocument (the C# oracle) preserves
                // these; pugixml's parse_default silently drops them.
                const auto res = d.load_file(in.c_str(),
                    pugi::parse_default | pugi::parse_comments | pugi::parse_pi);
                if (!res) {
                    Log(r.log, "[mergexml] FAILED to load %s: %s", in.string().c_str(), res.description());
                    return r;
                }
                if (std::string(d.document_element().name()) != "system") {
                    Log(r.log, "[mergexml] FAILED: root of %s is not <system>", in.string().c_str());
                    return r;
                }
            }

            pugi::xml_document outDoc;
            auto decl = outDoc.append_child(pugi::node_declaration);
            decl.append_attribute("version") = "1.0";
            decl.append_attribute("encoding") = "UTF-8";
            auto outRoot = outDoc.append_child(docs[0].document_element().name());
            for (auto attr : docs[0].document_element().attributes()) {
                outRoot.append_attribute(attr.name()) = attr.value();
            }
            for (const char* kind : kDefaultKinds) {
                outRoot.append_child(kind).append_attribute("name") = "cef_factory";
            }

            std::set<std::string> seenBones;
            std::set<std::string> seenShapes;
            int dupBones = 0;
            for (std::size_t i = 0; i < docs.size(); ++i) {
                if (i > 0) {
                    for (const char* kind : kDefaultKinds) {
                        outRoot.append_child(kind).append_attribute("extends") = "cef_factory";
                    }
                }
                for (auto el : docs[i].document_element().children()) {
                    if (el.type() != pugi::node_element) {
                        continue;
                    }
                    const std::string tag = el.name();
                    const auto nmAttr = el.attribute("name");
                    const std::string nm = nmAttr.value();
                    if (tag == "bone" && !nmAttr.empty()) {
                        if (!seenBones.insert(nm).second) {
                            ++dupBones;
                            Log(r.log, "[mergexml] duplicate bone '%s' from %s skipped (first wins)",
                                nm.c_str(), inputs[i].string().c_str());
                            continue;
                        }
                    }
                    if ((tag == "per-vertex-shape" || tag == "per-triangle-shape") && !nmAttr.empty() &&
                        !seenShapes.insert(nm).second) {
                        Log(r.log,
                            "[mergexml] WARNING: duplicate collision shape '%s' from %s - NIF shape names must be unique across merged contents",
                            nm.c_str(), inputs[i].string().c_str());
                    }
                    outRoot.append_copy(el);
                }
                Log(r.log, "[mergexml] appended %s", inputs[i].string().c_str());
            }

            if (!outDoc.save_file(out.c_str(), "  ")) {
                Log(r.log, "[mergexml] FAILED to save %s", out.string().c_str());
                return r;
            }
            Log(r.log, "[mergexml] wrote %s  (bones=%zu dupSkipped=%d shapes=%zu)", out.string().c_str(),
                seenBones.size(), dupBones, seenShapes.size());
            r.ok = true;
        } catch (const std::exception& e) {
            Log(r.log, "[mergexml] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[mergexml] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult SetXml(const std::filesystem::path& in, const std::filesystem::path& out,
        const std::string& xmlPath)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(in) != 0 || !nif.IsValid()) {
                Log(r.log, "[setxml] FAILED to load %s", in.string().c_str());
                return r;
            }
            nifly::NiStringExtraData* extra = nullptr;
            {
                const auto& hdr = nif.GetHeader();
                for (uint32_t i = 0; i < hdr.GetNumBlocks() && !extra; ++i) {
                    if (auto* sed = hdr.GetBlock<nifly::NiStringExtraData>(i)) {
                        if (sed->name.get() == kHdtExtra) {
                            extra = sed;
                        }
                    }
                }
            }
            if (!extra) {
                Log(r.log, "[setxml] FAILED: no '%s' NiStringExtraData in %s", kHdtExtra, in.string().c_str());
                return r;
            }
            const std::string old = extra->stringData.get();
            extra->stringData.get() = xmlPath;
            Log(r.log, "[setxml] '%s' -> '%s'", old.c_str(), xmlPath.c_str());

            if (nif.Save(out) != 0) {
                Log(r.log, "[setxml] FAILED to save %s", out.string().c_str());
                return r;
            }
            nifly::NifFile chk;
            std::string back;
            if (chk.Load(out) == 0) {
                const auto& hdr = chk.GetHeader();
                for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                    if (auto* sed = hdr.GetBlock<nifly::NiStringExtraData>(i)) {
                        if (sed->name.get() == kHdtExtra) {
                            back = sed->stringData.get();
                            break;
                        }
                    }
                }
            }
            Log(r.log, "[setxml] VERDICT readback='%s' (want '%s')", back.c_str(), xmlPath.c_str());
            r.ok = (back == xmlPath);
        } catch (const std::exception& e) {
            Log(r.log, "[setxml] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[setxml] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult RenameShapesInXml(const std::filesystem::path& in, const std::filesystem::path& out,
        const std::map<std::string, std::string>& renames)
    {
        VerbResult r;
        try {
            pugi::xml_document doc;
            const auto res = doc.load_file(in.c_str(),
                pugi::parse_default | pugi::parse_comments | pugi::parse_pi);
            if (!res || !doc.document_element()) {
                Log(r.log, "[persist] FAILED xml load '%s'", in.string().c_str());
                return r;
            }
            int n = 0;
            for (auto el : doc.document_element().children()) {
                if (el.type() != pugi::node_element) {
                    continue;
                }
                const std::string tag = el.name();
                if (tag != "per-vertex-shape" && tag != "per-triangle-shape") {
                    continue;
                }
                auto nmAttr = el.attribute("name");
                if (nmAttr.empty()) {
                    continue;
                }
                const auto it = renames.find(nmAttr.value());
                if (it != renames.end()) {
                    nmAttr.set_value(it->second.c_str());
                    ++n;
                }
            }
            if (!doc.save_file(out.c_str(), "  ")) {
                Log(r.log, "[persist] FAILED xml save '%s'", out.string().c_str());
                return r;
            }
            Log(r.log, "[persist] xml '%s': renamed %d per-*-shape name(s)", in.filename().string().c_str(), n);
            r.ok = true;
        } catch (const std::exception& e) {
            Log(r.log, "[persist] FAILED xml transform: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[persist] FAILED xml transform: unknown exception");
            r.ok = false;
        }
        return r;
    }

    std::string FragmentWithCount(const std::string& fragment, int count)
    {
        if (fragment.empty()) {
            return fragment;
        }
        try {
            auto node = nlohmann::json::parse(fragment);
            node["count"] = count;
            return node.dump();
        } catch (...) {
            return fragment;
        }
    }

    VerbResult Validate(const std::filesystem::path& path)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(path) != 0 || !nif.IsValid()) {
                Log(r.log, "[validate] %s: FAILED to load (unreadable / invalid NIF)", path.string().c_str());
                return r;
            }
            Report(r.log, nif, ("VALIDATE " + path.string()).c_str());
            std::string reason;
            r.ok = ValidateNifSkinnable(nif, reason);
            if (r.ok) {
                Log(r.log, "[validate] VERDICT: OK (no divide-by-zero risk found)");
            } else {
                Log(r.log, "[validate] VERDICT: REJECT - %s", reason.c_str());
            }
        } catch (const std::exception& e) {
            Log(r.log, "[validate] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[validate] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult ValidateHead(const std::filesystem::path& path)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(path) != 0 || !nif.IsValid()) {
                Log(r.log, "[validatehead] %s: FAILED to load (unreadable / invalid NIF)", path.string().c_str());
                return r;
            }
            Report(r.log, nif, ("VALIDATEHEAD " + path.string()).c_str());

            std::string ctdReason;
            const bool ctdOk = ValidateNifSkinnable(nif, ctdReason);
            auto* rootExtra = RootHdtExtra(nif);
            const bool anyExtra = FirstHdtExtra(nif) != nullptr;
            const int bones = CountNamedNodes(nif);
            const auto tree = TreeNodeNames(nif);

            int triggers = 0;
            int shaderless = 0;
            std::vector<std::string> unreachable;  // insertion-ordered unique
            for (auto* s : nif.GetShapes()) {
                const std::string sn = s->name.get();
                const bool hasShader = nif.GetShader(s) != nullptr;
                if (!hasShader) {
                    ++shaderless;
                }
                if (!s->HasSkinInstance()) {
                    Log(r.log, "    shape '%s': UNSKINNED (no merge trigger)%s", sn.c_str(),
                        hasShader ? "" : "  NO-SHADER (facegen-fatal)");
                    continue;
                }
                const auto names = ShapeBoneNames(nif, s);
                std::vector<std::string> custom;
                for (const auto& n : names) {
                    if (!LooksLiveSkeletonBone(n)) {
                        custom.push_back(n);
                    }
                }
                const bool trig = !custom.empty();
                if (trig) {
                    ++triggers;
                }
                for (const auto& c : custom) {
                    if (tree.find(c) == tree.end() &&
                        std::find(unreachable.begin(), unreachable.end(), c) == unreachable.end()) {
                        unreachable.push_back(c);
                    }
                }
                Log(r.log, "    shape '%s' verts=%d: skinBones=%zu custom=%zu%s%s", sn.c_str(),
                    static_cast<int>(s->GetNumVertices()), names.size(), custom.size(),
                    trig ? "  TRIGGER" : "  (vanilla-only)", hasShader ? "" : "  NO-SHADER (facegen-fatal)");
            }

            // §9-10 anchor report: chain tops and their effective merge anchors.
            auto* rootNode = nif.GetRootNode();
            std::vector<std::pair<std::string, std::string>> anchors;  // (chainTop, anchor)
            {
                const auto& hdr = nif.GetHeader();
                for (uint32_t i = 0; i < hdr.GetNumBlocks(); ++i) {
                    auto* node = hdr.GetBlock<nifly::NiNode>(i);
                    if (!node || !IsExactNiNode(node) || node->name.get().empty() || node == rootNode) {
                        continue;
                    }
                    const std::string nm = node->name.get();
                    if (LooksLiveSkeletonBone(nm)) {
                        continue;
                    }
                    // nearest NAMED ancestor (unnamed nodes are transparent)
                    auto* p = nif.GetParentNode(node);
                    while (p && p->name.get().empty() && p != rootNode) {
                        p = nif.GetParentNode(p);
                    }
                    std::string pn = !p ? "(orphan)"
                                        : (p == rootNode ? "(model root)"
                                                         : (p->name.get().empty() ? "(model root)" : p->name.get()));
                    if (pn != "(orphan)" && pn != "(model root)" && !LooksLiveSkeletonBone(pn)) {
                        continue;  // parent is custom too -> not a chain top
                    }
                    // dedupe by chain-top name, first wins (C# dict assignment
                    // per header order ends with the LAST value; mirror that)
                    bool found = false;
                    for (auto& kv : anchors) {
                        if (kv.first == nm) {
                            kv.second = pn;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        anchors.emplace_back(nm, pn);
                    }
                }
            }
            std::stable_sort(anchors.begin(), anchors.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second < b.second : a.first < b.first;
            });
            for (const auto& kv : anchors) {
                Log(r.log, "    chain top '%s' -> anchors under '%s'", kv.first.c_str(), kv.second.c_str());
            }

            std::string r1 = std::string("[validatehead] R1 rootHdtExtra=") + (rootExtra ? "True" : "False");
            if (!rootExtra && anyExtra) {
                r1 += "  (extra data EXISTS but NOT on root - scanBBP misses it)";
            }
            if (rootExtra) {
                r1 += "  xml='" + rootExtra->stringData.get() + "'";
            }
            Log(r.log, "%s", r1.c_str());
            Log(r.log, "[validatehead] R2 mergeTriggerShapes=%d (skinned shape referencing a non-live bone)",
                triggers);
            std::string r3 = "[validatehead] R3 unreachableCustomBones=" + std::to_string(unreachable.size());
            if (!unreachable.empty()) {
                r3 += "  (";
                for (std::size_t i = 0; i < unreachable.size() && i < 5; ++i) {
                    if (i) {
                        r3 += ", ";
                    }
                    r3 += unreachable[i];
                }
                if (unreachable.size() > 5) {
                    r3 += ", ...";
                }
                r3 += ") - doSkeletonMerge would never create these";
            }
            Log(r.log, "%s", r3.c_str());
            Log(r.log, "[validatehead] R4 shaderlessShapes=%d%s", shaderless,
                shaderless > 0
                    ? "  - facegen-fatal: the head build breaks on shader-less geometry (use headcarrier --keep1)"
                    : "");
            Log(r.log, "[validatehead] bones=%d  sseSkinnable=%s%s%s", bones, ctdOk ? "True" : "False",
                ctdOk ? "" : " - ", ctdOk ? "" : ctdReason.c_str());
            const bool pass = ctdOk && rootExtra && bones > 0 && triggers > 0 && unreachable.empty() &&
                              shaderless == 0;
            Log(r.log, "[validatehead] VERDICT: %s", pass ? "OK (should fire the facegen path)" : "REJECT");
            r.ok = pass;
        } catch (const std::exception& e) {
            Log(r.log, "[validatehead] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[validatehead] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult HeadCarrier(const std::filesystem::path& out,
        const std::vector<std::filesystem::path>& contents, const HeadCarrierOptions& opts)
    {
        VerbResult r;
        const std::filesystem::path tMerged = out.string() + ".hc1.nif";
        const std::filesystem::path tZa = out.string() + ".hc2.nif";
        const auto cleanup = [&]() {
            std::error_code ec;
            std::filesystem::remove(tMerged, ec);
            std::filesystem::remove(tZa, ec);
        };
        try {
            if (contents.empty()) {
                Log(r.log, "[headcarrier] no content given");
                return r;
            }
            // CTD gate per content - loud fail (exclusion policy lives in sync)
            for (const auto& c : contents) {
                std::string why;
                if (!ValidateNifPathImpl(c, why)) {
                    Log(r.log, "[headcarrier] REJECT content '%s' - %s", c.string().c_str(), why.c_str());
                    return r;
                }
            }

            // R2: base = first content owning a merge-trigger shape
            int baseIdx = -1;
            for (std::size_t i = 0; i < contents.size() && baseIdx < 0; ++i) {
                nifly::NifFile n;
                if (n.Load(contents[i]) != 0 || !n.IsValid()) {
                    continue;
                }
                for (auto* s : n.GetShapes()) {
                    if (!s->HasSkinInstance()) {
                        continue;
                    }
                    const auto names = ShapeBoneNames(n, s);
                    if (std::any_of(names.begin(), names.end(),
                            [](const std::string& b) { return !LooksLiveSkeletonBone(b); })) {
                        baseIdx = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (baseIdx < 0) {
                Log(r.log, "[headcarrier] FAILED: no content has a skinned shape referencing a custom bone -");
                Log(r.log, "[headcarrier]         the facegen path would never call doSkeletonMerge (R2, FSMP_REINVESTIGATION.md \xC2\xA7" "2-2)");
                return r;
            }
            if (baseIdx != 0) {
                Log(r.log, "[headcarrier] base reordered to '%s' (owns a merge-trigger shape)",
                    contents[baseIdx].string().c_str());
            }
            std::vector<std::filesystem::path> ordered{ contents[baseIdx] };
            for (std::size_t i = 0; i < contents.size(); ++i) {
                if (static_cast<int>(i) != baseIdx) {
                    ordered.push_back(contents[i]);
                }
            }

            std::filesystem::path cur = ordered[0];
            if (ordered.size() > 1) {
                const auto mg = Merge(tMerged, ordered);
                r.log += mg.log;
                if (!mg.ok) {
                    cleanup();
                    return r;
                }
                if (opts.xmlRel.empty()) {
                    Log(r.log,
                        "[headcarrier] WARNING: 2+ contents but no --xml - carrier keeps the base's xml; the other contents' physics configs are NOT merged");
                }
                cur = tMerged;
            }
            {
                const auto za = ZeroAlpha(cur, tZa);
                r.log += za.log;
                if (!za.ok) {
                    cleanup();
                    return r;
                }
            }

            // Final pass (in-memory): R3 completion -> keep1 -> anchor -> xml
            // -> R1 -> save
            nifly::NifFile fin;
            if (fin.Load(tZa) != 0 || !fin.IsValid()) {
                Log(r.log, "[headcarrier] FAILED to reload assembled carrier");
                cleanup();
                return r;
            }
            const int orphans = CompleteBoneTree(fin, r.log);
            if (orphans > 0) {
                Log(r.log, "[headcarrier] R3: re-parented %d skin-only bone(s) into the tree", orphans);
            }
            if (opts.keep1) {
                const auto all = fin.GetShapes();
                std::vector<nifly::NiShape*> candidates;
                for (auto* s : all) {
                    if (!s->HasSkinInstance() || fin.GetShader(s) == nullptr) {
                        continue;
                    }
                    const auto names = ShapeBoneNames(fin, s);
                    if (std::any_of(names.begin(), names.end(),
                            [](const std::string& b) { return !LooksLiveSkeletonBone(b); })) {
                        candidates.push_back(s);
                    }
                }
                std::stable_sort(candidates.begin(), candidates.end(),
                    [](nifly::NiShape* a, nifly::NiShape* b) { return a->GetNumVertices() < b->GetNumVertices(); });
                nifly::NiShape* keep = candidates.empty() ? nullptr : candidates.front();
                if (!keep) {
                    Log(r.log, "[headcarrier] FAILED: --keep1 found no shader-bearing trigger shape");
                    cleanup();
                    return r;
                }
                std::vector<nifly::NiShape*> kept{ keep };
                if (opts.proxyShader) {
                    auto* keptRef = keep->ShaderPropertyRef();
                    const uint32_t shaderId = keptRef ? keptRef->index : nifly::NIF_NPOS;
                    for (auto* s : all) {
                        if (s == keep || !s->HasSkinInstance() || fin.GetShader(s) != nullptr) {
                            continue;
                        }
                        auto* prop = s->ShaderPropertyRef();
                        if (shaderId != nifly::NIF_NPOS && prop) {
                            prop->index = shaderId;
                            kept.push_back(s);
                            Log(r.log,
                                "[headcarrier] proxyshader: kept collision proxy '%s' sharing '%s' shader (zero-alpha)",
                                s->name.get().c_str(), keep->name.get().c_str());
                        } else {
                            Log(r.log,
                                "[headcarrier] proxyshader: WARNING cannot set shader on '%s' (%s) - dropped",
                                s->name.get().c_str(), s->GetBlockName());
                        }
                    }
                }
                int dropped = 0;
                for (auto* s : all) {
                    if (std::find(kept.begin(), kept.end(), s) == kept.end()) {
                        fin.GetHeader().DeleteBlock(fin.GetBlockID(s));
                        ++dropped;
                    }
                }
                fin.DeleteUnreferencedBlocks();
                Log(r.log, "[headcarrier] keep1: kept %zu shape(s) ('%s' trigger%s), dropped %d", kept.size(),
                    keep->name.get().c_str(), kept.size() > 1 ? " + collision proxies" : "", dropped);
            }
            if (!opts.anchorName.empty()) {
                const auto [moved, kept] = AnchorInMemory(fin, opts.anchorName);
                Log(r.log, "[headcarrier] anchor '%s': moved %d branch(es), kept %d live-named at root",
                    opts.anchorName.c_str(), moved, kept);
            }
            if (!opts.xmlRel.empty()) {
                auto* ex = FirstHdtExtra(fin);
                if (!ex) {
                    Log(r.log, "[headcarrier] FAILED: no '%s' extra data to re-point", kHdtExtra);
                    cleanup();
                    return r;
                }
                Log(r.log, "[headcarrier] xml '%s' -> '%s'", ex->stringData.get().c_str(), opts.xmlRel.c_str());
                ex->stringData.get() = opts.xmlRel;
            }
            if (!EnsureRootHdtExtra(fin, r.log)) {
                Log(r.log, "[headcarrier] FAILED: no '%s' extra data anywhere - not an SMP content set", kHdtExtra);
                cleanup();
                return r;
            }
            if (fin.Save(out) != 0) {
                Log(r.log, "[headcarrier] FAILED to save %s", out.string().c_str());
                cleanup();
                return r;
            }
            Log(r.log, "[headcarrier] wrote %s", out.string().c_str());
            cleanup();

            const auto vh = ValidateHead(out);
            r.log += vh.log;
            r.ok = vh.ok;
        } catch (const std::exception& e) {
            Log(r.log, "[headcarrier] FAILED: exception: %s", e.what());
            cleanup();
            r.ok = false;
        } catch (...) {
            Log(r.log, "[headcarrier] FAILED: unknown exception");
            cleanup();
            r.ok = false;
        }
        return r;
    }

    VerbResult ProxyNif(const std::filesystem::path& in, const std::filesystem::path& out)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(in) != 0 || !nif.IsValid()) {
                Log(r.log, "[proxynif] FAILED to load %s", in.string().c_str());
                return r;
            }

            const auto all = nif.GetShapes();
            std::vector<nifly::NiShape*> donors;
            std::vector<nifly::NiShape*> proxies;
            for (auto* s : all) {
                if (!s->HasSkinInstance()) {
                    continue;
                }
                if (nif.GetShader(s) != nullptr) {
                    donors.push_back(s);
                } else {
                    proxies.push_back(s);
                }
            }
            std::stable_sort(donors.begin(), donors.end(),
                [](nifly::NiShape* a, nifly::NiShape* b) { return a->GetNumVertices() < b->GetNumVertices(); });
            nifly::NiShape* donor = donors.empty() ? nullptr : donors.front();
            if (!donor || proxies.empty()) {
                Log(r.log,
                    "[proxynif] FAILED: need a shader-bearing donor and >=1 shader-less skinned proxy (donor=%s, proxies=%zu)",
                    donor ? "True" : "False", proxies.size());
                return r;
            }

            auto* donorRef = donor->ShaderPropertyRef();
            const uint32_t shaderId = donorRef ? donorRef->index : nifly::NIF_NPOS;
            if (shaderId == nifly::NIF_NPOS) {
                Log(r.log, "[proxynif] FAILED: cannot read donor shader ref");
                return r;
            }
            for (auto* p : proxies) {
                auto* prop = p->ShaderPropertyRef();
                if (!prop) {
                    Log(r.log, "[proxynif] FAILED: cannot set shader on '%s' (%s)", p->name.get().c_str(),
                        p->GetBlockName());
                    return r;
                }
                prop->index = shaderId;
                Log(r.log, "[proxynif] proxy '%s' borrows '%s' shader", p->name.get().c_str(),
                    donor->name.get().c_str());
            }
            int dropped = 0;
            for (auto* s : all) {
                if (std::find(proxies.begin(), proxies.end(), s) == proxies.end()) {
                    nif.GetHeader().DeleteBlock(nif.GetBlockID(s));
                    ++dropped;
                }
            }
            // Strip the inherited HDT extra data: the proxy must NOT declare
            // its own physics file (FSMP would build a SECOND system).
            while (auto* sed = FirstHdtExtra(nif)) {
                nif.GetHeader().DeleteBlock(nif.GetBlockID(sed));
                Log(r.log, "[proxynif] stripped inherited HDT physics extra data");
            }
            nif.DeleteUnreferencedBlocks();

            const std::filesystem::path tmp = out.string() + ".px.nif";
            if (nif.Save(tmp) != 0) {
                Log(r.log, "[proxynif] FAILED to save %s", tmp.string().c_str());
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return r;
            }
            const auto za = ZeroAlpha(tmp, out);  // invisible + NiAlphaProperty
            r.log += za.log;
            {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
            }
            if (!za.ok) {
                return r;
            }

            nifly::NifFile chk;
            chk.Load(out);
            const auto shapes = chk.GetShapes();
            const bool allShader = !shapes.empty() &&
                std::all_of(shapes.begin(), shapes.end(),
                    [&](nifly::NiShape* s) { return chk.GetShader(s) != nullptr; });
            Log(r.log, "[proxynif] VERDICT shapes=%zu (want %zu) allShader=%s dropped=%d", shapes.size(),
                proxies.size(), allShader ? "True" : "False", dropped);
            r.ok = (shapes.size() == proxies.size() && allShader);
        } catch (const std::exception& e) {
            Log(r.log, "[proxynif] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[proxynif] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult Merge(const std::filesystem::path& out, const std::vector<std::filesystem::path>& inputs)
    {
        VerbResult r;
        try {
            if (inputs.size() < 2) {
                Log(r.log, "[merge] need a base NIF + at least one more");
                return r;
            }
            nifly::NifFile dst;
            if (dst.Load(inputs[0]) != 0 || !dst.IsValid()) {
                Log(r.log, "[merge] FAILED to load base %s", inputs[0].string().c_str());
                return r;
            }
            Report(r.log, dst, ("BASE " + inputs[0].string()).c_str());
            auto* dstRoot = dst.GetRootNode();

            for (std::size_t i = 1; i < inputs.size(); ++i) {
                nifly::NifFile src;
                if (src.Load(inputs[i]) != 0 || !src.IsValid()) {
                    Log(r.log, "[merge] FAILED to load %s", inputs[i].string().c_str());
                    return r;
                }

                const int addedBones = MergeBones(dst, src, dstRoot);  // bones ALWAYS union

                // Clone this content's shapes, remembering the new ones so we
                // can drop JUST them if they corrupt the NIF.
                std::vector<nifly::NiShape*> addedShapes;
                for (auto* srcShape : src.GetShapes()) {
                    const std::string nm = srcShape->name.get();
                    bool dup = false;
                    for (auto* s : dst.GetShapes()) {
                        if (s->name.get() == nm) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) {
                        Log(r.log, "[merge] WARNING: duplicate shape name '%s' from %s - first wins",
                            nm.c_str(), inputs[i].string().c_str());
                        continue;
                    }
                    if (auto* ns = dst.CloneShape(srcShape, nm, &src)) {
                        addedShapes.push_back(ns);
                    }
                }
                dst.DeleteUnreferencedBlocks();

                // Validate to a throwaway temp (unique per content). Unloadable
                // -> remove just this content's shapes (its bones stay).
                auto vtmp = out;
                vtmp += ".v" + std::to_string(i);
                const bool ok = SaveReloadOk(dst, vtmp);
                std::error_code ec;
                std::filesystem::remove(vtmp, ec);
                if (ok) {
                    Log(r.log, "[merge] %s: +%d bone(s), +%zu shape(s)", inputs[i].string().c_str(),
                        addedBones, addedShapes.size());
                } else {
                    for (auto* s : addedShapes) {
                        dst.DeleteShape(s);
                    }
                    dst.DeleteUnreferencedBlocks();
                    Log(r.log, "[merge] %s: +%d bone(s), shapes unloadable (nifly SSE clone) - kept BONES-ONLY",
                        inputs[i].string().c_str(), addedBones);
                }
            }

            if (dst.Save(out) != 0) {
                Log(r.log, "[merge] FAILED to save %s", out.string().c_str());
                return r;
            }
            nifly::NifFile chk;
            if (chk.Load(out) != 0 || !chk.IsValid()) {
                Log(r.log, "[merge] merged NIF invalid on reload");
                return r;
            }
            Report(r.log, chk, ("MERGED " + out.string()).c_str());
            Log(r.log, "[merge] hdtExtra=%s (root extra inherited from base '%s').",
                HasHdtExtra(chk) ? "True" : "False", inputs[0].string().c_str());
            r.ok = true;
        } catch (const std::exception& e) {
            Log(r.log, "[merge] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[merge] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    VerbResult VerifyZeroAlpha(const std::filesystem::path& path)
    {
        VerbResult r;
        try {
            nifly::NifFile nif;
            if (nif.Load(path) != 0 || !nif.IsValid()) {
                Log(r.log, "[verifyzeroalpha] FAILED to load %s", path.string().c_str());
                return r;
            }
            Report(r.log, nif, "VERIFY");
            r.ok = CheckZeroAlpha(r.log, nif, "verifyzeroalpha");
        } catch (const std::exception& e) {
            Log(r.log, "[verifyzeroalpha] FAILED: exception: %s", e.what());
            r.ok = false;
        } catch (...) {
            Log(r.log, "[verifyzeroalpha] FAILED: unknown exception");
            r.ok = false;
        }
        return r;
    }

    // ---------------------------------------------------------------------
    // sync driver (C# Sync/SyncPersistBuild ports)

    namespace {

        constexpr const char* kEmptyXmlPlaceholder = "<?xml version=\"1.0\"?>\n<system></system>\n";
        constexpr int kSlots = 8;
        constexpr int kMaxPersistProxies = 8;
        constexpr const char* kPersistCarrierEditorId = "CFW_PersistCarrier";

        std::string PersistProxyEditorId(int i)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "CFW_PersistProxy%02d", i);
            return buf;
        }

        std::string Sha256HexUpper(const std::string& data)
        {
            std::string out;
            BCRYPT_ALG_HANDLE alg = nullptr;
            if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
                return out;
            }
            BCRYPT_HASH_HANDLE h = nullptr;
            if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
                unsigned char digest[32];
                BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                    static_cast<ULONG>(data.size()), 0);
                if (BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
                    char hex[3];
                    for (unsigned char b : digest) {
                        std::snprintf(hex, sizeof(hex), "%02X", b);
                        out += hex;
                    }
                }
                BCryptDestroyHash(h);
            }
            BCryptCloseAlgorithmProvider(alg, 0);
            return out;
        }

        std::string ReadTextFile(const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        bool WriteTextFile(const std::filesystem::path& p, const std::string& data)
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << data;
            return static_cast<bool>(f);
        }

        bool CopyOverwrite(const std::filesystem::path& from, const std::filesystem::path& to)
        {
            std::error_code ec;
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
            return !ec;
        }

        std::filesystem::path ResolveAgainstRoots(const std::string& rel,
            const std::vector<std::filesystem::path>& roots, bool meshesRel)
        {
            std::string fixed = rel;
            std::replace(fixed.begin(), fixed.end(), '/', '\\');
            for (const auto& r : roots) {
                if (meshesRel) {
                    const auto p1 = r / "meshes" / fixed;
                    if (std::filesystem::exists(p1)) {
                        return p1;
                    }
                }
                const auto p2 = r / fixed;
                if (std::filesystem::exists(p2)) {
                    return p2;
                }
            }
            return {};
        }

        struct SmpContent
        {
            std::filesystem::path nif;
            std::filesystem::path xmlDisk;
            std::string xmlRel;
        };

        // Hash inputs by path+size+mtime (the same skip-when-unchanged contract
        // as C#; the digest VALUE differs from C#'s - timestamps use the C++
        // filesystem epoch - so the first in-proc run after a tool switch
        // rebuilds everything once).
        std::string HashContents(const char* prefix, std::vector<SmpContent> smp)
        {
            std::sort(smp.begin(), smp.end(),
                [](const SmpContent& a, const SmpContent& b) { return a.nif.string() < b.nif.string(); });
            std::string h = prefix;
            for (const auto& s : smp) {
                std::error_code ec;
                const auto nlen = std::filesystem::file_size(s.nif, ec);
                const auto ntime = std::filesystem::last_write_time(s.nif, ec).time_since_epoch().count();
                const auto xlen = std::filesystem::file_size(s.xmlDisk, ec);
                const auto xtime = std::filesystem::last_write_time(s.xmlDisk, ec).time_since_epoch().count();
                h += s.nif.string() + "|" + std::to_string(nlen) + "|" + std::to_string(ntime) + "|" +
                     s.xmlDisk.string() + "|" + std::to_string(xlen) + "|" + std::to_string(xtime) + "|";
            }
            return Sha256HexUpper(h);
        }

        // Resolve the manifest's content list into validated SMP contents.
        // transientMiss counts path-resolution misses (may heal next run);
        // non-SMP and validate rejects are decisive. `tag` = log prefix.
        std::vector<SmpContent> ResolveSmpContents(const nlohmann::json& contentsArr,
            const std::vector<std::filesystem::path>& dataRoots, const std::string& tag,
            const char* excludedSuffix, int* transientMiss, std::string& log)
        {
            std::vector<SmpContent> smp;
            for (const auto& c : contentsArr) {
                const std::string rel = c.value("nif", "");
                const auto nifDisk = ResolveAgainstRoots(rel, dataRoots, true);
                if (nifDisk.empty()) {
                    Log(log, "%s WARNING content '%s' EXCLUDED - cannot resolve under data roots%s", tag.c_str(),
                        rel.c_str(), excludedSuffix);
                    if (transientMiss) {
                        ++*transientMiss;
                    }
                    continue;
                }
                const std::string xmlRel = GetHdtXmlPath(nifDisk);
                if (xmlRel.empty()) {
                    continue;  // not an SMP content
                }
                std::string reason;
                if (!ValidateNifPathImpl(nifDisk, reason)) {
                    if (*excludedSuffix) {
                        Log(log, "%s WARNING content '%s' EXCLUDED - %s (would crash the SSE skin-partition loader)%s",
                            tag.c_str(), rel.c_str(), reason.c_str(), excludedSuffix);
                    } else {
                        Log(log, "%s WARNING content '%s' EXCLUDED - %s", tag.c_str(), rel.c_str(), reason.c_str());
                    }
                    continue;
                }
                const auto xmlDisk = ResolveAgainstRoots(xmlRel, dataRoots, false);
                if (xmlDisk.empty()) {
                    Log(log, "%s WARNING content '%s' EXCLUDED - physics xml '%s' not found%s", tag.c_str(),
                        rel.c_str(), xmlRel.c_str(), excludedSuffix);
                    if (transientMiss) {
                        ++*transientMiss;
                    }
                    continue;
                }
                smp.push_back({ nifDisk, xmlDisk, xmlRel });
            }
            return smp;
        }

        // First content owning a merge-trigger shape (R2). requireShader: the
        // persist variant also demands a shader-bearing trigger.
        int FindTriggerBaseIndex(const std::vector<SmpContent>& smp, bool requireShader)
        {
            for (std::size_t i = 0; i < smp.size(); ++i) {
                nifly::NifFile n;
                if (n.Load(smp[i].nif) != 0 || !n.IsValid()) {
                    continue;
                }
                for (auto* s : n.GetShapes()) {
                    if (!s->HasSkinInstance()) {
                        continue;
                    }
                    if (requireShader && n.GetShader(s) == nullptr) {
                        continue;
                    }
                    const auto names = ShapeBoneNames(n, s);
                    if (std::any_of(names.begin(), names.end(),
                            [](const std::string& b) { return !LooksLiveSkeletonBone(b); })) {
                        return static_cast<int>(i);
                    }
                }
            }
            return -1;
        }

        std::optional<std::string> SyncPersistBuild(const nlohmann::json& persistEl,
            const std::vector<std::filesystem::path>& dataRoots, const std::filesystem::path& carrierDir,
            const std::filesystem::path& xmlDir, const std::filesystem::path& tmpDir,
            const std::filesystem::path& emptyNif, std::optional<std::string> oldFragment, int& built,
            int& skipped, int& failed, int& poolCreated, std::string& log)
        {
            const auto basePath = carrierDir / "Persist_carrier.nif";
            const auto hashPath = carrierDir / "Persist_carrier.hash";

            const auto ensurePool = [&]() {
                const std::filesystem::path seed =
                    std::filesystem::exists(basePath) ? basePath : emptyNif;
                const bool haveSeed = !seed.empty() && std::filesystem::exists(seed);
                for (int k = 0; k < kSlots; ++k) {
                    const auto cn = carrierDir / ("Persist_carrier_r" + std::to_string(k) + ".nif");
                    if (!std::filesystem::exists(cn) && haveSeed && CopyOverwrite(seed, cn)) {
                        ++poolCreated;
                    }
                    for (int i = 1; i <= kMaxPersistProxies; ++i) {
                        char nm[48];
                        std::snprintf(nm, sizeof(nm), "Persist_part%02d_r%d.nif", i, k);
                        const auto pn = carrierDir / nm;
                        if (!std::filesystem::exists(pn) && haveSeed && CopyOverwrite(seed, pn)) {
                            ++poolCreated;
                        }
                    }
                    const auto sx = xmlDir / ("Persist_physics_r" + std::to_string(k) + ".xml");
                    if (!std::filesystem::exists(sx) && WriteTextFile(sx, kEmptyXmlPlaceholder)) {
                        ++poolCreated;
                    }
                }
            };

            // Resolve contents - EXCLUDE (warn + drop), never fail the set.
            int declared = 0;
            int transientMiss = 0;
            std::vector<SmpContent> smp;
            if (persistEl.is_object() && persistEl.contains("contents")) {
                declared = static_cast<int>(persistEl["contents"].size());
                smp = ResolveSmpContents(persistEl["contents"], dataRoots, "[persist]", "", &transientMiss, log);
            }

            if (smp.empty()) {
                ensurePool();
                if (transientMiss > 0) {
                    Log(log, "[persist] %d content(s) unresolved (transient?) - keeping previous artifacts",
                        transientMiss);
                    return oldFragment;
                }
                if (declared > 0) {
                    Log(log, "[persist] no SMP content among %d declared - count=0 (CEF deregisters)", declared);
                }
                return oldFragment ? std::optional<std::string>(FragmentWithCount(*oldFragment, 0))
                                   : std::nullopt;
            }

            const std::string hash = HashContents("p1|", smp);
            if (std::filesystem::exists(hashPath) && std::filesystem::exists(basePath) &&
                ReadTextFile(hashPath) == hash && oldFragment) {
                ensurePool();
                ++skipped;
                return FragmentWithCount(*oldFragment, static_cast<int>(smp.size()));
            }

            Log(log, "[persist] %zu SMP content(s)", smp.size());

            const auto failKeepOld = [&]() {
                ++failed;
                ensurePool();
                return oldFragment;
            };

            // Base ordering: trigger-owning content first.
            const int baseIdx = FindTriggerBaseIndex(smp, true);
            if (baseIdx < 0) {
                Log(log, "[persist] FAILED: no content owns a merge-trigger shape (R2)");
                return failKeepOld();
            }
            std::vector<SmpContent> ordered{ smp[baseIdx] };
            for (std::size_t i = 0; i < smp.size(); ++i) {
                if (static_cast<int>(i) != baseIdx) {
                    ordered.push_back(smp[i]);
                }
            }

            // Master merged nif: bones union + every content's shapes
            // (zero-alpha'd), R3-completed, root HDT extra ensured.
            const auto tMg = tmpDir / "persist_mg.nif";
            const auto tZa = tmpDir / "persist_za.nif";
            const auto tBase = tmpDir / "persist_base.nif";
            {
                std::filesystem::path cur = ordered[0].nif;
                if (ordered.size() > 1) {
                    std::vector<std::filesystem::path> nifs;
                    for (const auto& s : ordered) {
                        nifs.push_back(s.nif);
                    }
                    const auto mg = Merge(tMg, nifs);
                    log += mg.log;
                    if (!mg.ok) {
                        Log(log, "[persist] FAILED building the master merge");
                        return failKeepOld();
                    }
                    cur = tMg;
                }
                const auto za = ZeroAlpha(cur, tZa);
                log += za.log;
                if (!za.ok) {
                    Log(log, "[persist] FAILED building the master merge");
                    return failKeepOld();
                }
                nifly::NifFile fin;
                if (fin.Load(tZa) != 0 || !fin.IsValid()) {
                    Log(log, "[persist] FAILED to reload master merge");
                    return failKeepOld();
                }
                CompleteBoneTree(fin, log);
                if (!EnsureRootHdtExtra(fin, log)) {
                    Log(log, "[persist] FAILED: no HDT extra data in the merged set");
                    return failKeepOld();
                }
                if (fin.Save(tBase) != 0) {
                    Log(log, "[persist] FAILED to save master merge");
                    return failKeepOld();
                }
            }

            // Needed head meshes = every per-*-shape name across the XMLs.
            std::vector<std::filesystem::path> xmlDisks;  // distinct, first-seen order
            for (const auto& s : ordered) {
                if (std::find(xmlDisks.begin(), xmlDisks.end(), s.xmlDisk) == xmlDisks.end()) {
                    xmlDisks.push_back(s.xmlDisk);
                }
            }
            std::vector<std::string> needed;
            for (const auto& xmlDisk : xmlDisks) {
                pugi::xml_document xdoc;
                const auto pres = xdoc.load_file(xmlDisk.c_str(),
                    pugi::parse_default | pugi::parse_comments | pugi::parse_pi);
                if (!pres) {
                    Log(log, "[persist] WARNING cannot parse xml '%s': %s", xmlDisk.string().c_str(),
                        pres.description());
                    continue;
                }
                for (auto el : xdoc.document_element().children()) {
                    if (el.type() != pugi::node_element) {
                        continue;
                    }
                    const std::string tag = el.name();
                    if (tag != "per-vertex-shape" && tag != "per-triangle-shape") {
                        continue;
                    }
                    const auto nmAttr = el.attribute("name");
                    if (!nmAttr.empty()) {
                        const std::string nm = nmAttr.value();
                        if (std::find(needed.begin(), needed.end(), nm) == needed.end()) {
                            needed.push_back(nm);
                        }
                    }
                }
            }

            // Shape inventory of the master merge (insertion-ordered, C# dict).
            struct ShapeInfo
            {
                bool skinned = false;
                bool shader = false;
                bool custom = false;
            };
            std::vector<std::pair<std::string, ShapeInfo>> shapeInfo;
            {
                nifly::NifFile inv;
                inv.Load(tBase);
                for (auto* s : inv.GetShapes()) {
                    ShapeInfo si;
                    si.skinned = s->HasSkinInstance();
                    si.shader = inv.GetShader(s) != nullptr;
                    if (si.skinned) {
                        const auto names = ShapeBoneNames(inv, s);
                        si.custom = std::any_of(names.begin(), names.end(),
                            [](const std::string& b) { return !LooksLiveSkeletonBone(b); });
                    }
                    const std::string nm = s->name.get();
                    bool replaced = false;
                    for (auto& kv : shapeInfo) {
                        if (kv.first == nm) {
                            kv.second = si;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        shapeInfo.emplace_back(nm, si);
                    }
                }
            }
            const auto findInfo = [&](const std::string& nm) -> const ShapeInfo* {
                for (const auto& kv : shapeInfo) {
                    if (kv.first == nm) {
                        return &kv.second;
                    }
                }
                return nullptr;
            };

            // Assignment: carrier gets a trigger-qualifying mesh; remaining
            // needed meshes go to the proxy pool in order.
            std::string carrierMesh;
            for (const auto& nm : needed) {
                const auto* si = findInfo(nm);
                if (si && si->skinned && si->custom) {
                    carrierMesh = nm;
                    break;
                }
            }
            if (carrierMesh.empty()) {
                for (const auto& kv : shapeInfo) {
                    if (kv.second.skinned && kv.second.custom) {
                        carrierMesh = kv.first;
                        break;
                    }
                }
            }
            if (carrierMesh.empty()) {
                Log(log, "[persist] FAILED: master merge has no trigger shape (R2)");
                return failKeepOld();
            }

            std::vector<std::pair<std::string, std::string>> assign{ { carrierMesh, kPersistCarrierEditorId } };
            int pi = 1;
            for (const auto& nm : needed) {
                if (nm == carrierMesh) {
                    continue;
                }
                const auto* si = findInfo(nm);
                if (!si || !si->skinned) {
                    Log(log,
                        "[persist] WARNING collision mesh '%s' has no skinned shape in the merge - its collision goes inert",
                        nm.c_str());
                    continue;
                }
                if (pi > kMaxPersistProxies) {
                    Log(log, "[persist] WARNING proxy pool exhausted (%d) - collision mesh '%s' goes inert",
                        kMaxPersistProxies, nm.c_str());
                    continue;
                }
                assign.emplace_back(nm, PersistProxyEditorId(pi));
                ++pi;
            }
            for (const auto& [mesh, eid] : assign) {
                Log(log, "[persist] assign '%s' -> %s", mesh.c_str(), eid.c_str());
            }

            // XML: per-content rename -> merge -> temp
            std::map<std::string, std::string> renames(assign.begin(), assign.end());
            std::vector<std::filesystem::path> txmls;
            int ti = 0;
            for (const auto& xmlDisk : xmlDisks) {
                const auto t = tmpDir / ("persist_x" + std::to_string(ti++) + ".xml");
                const auto rn = RenameShapesInXml(xmlDisk, t, renames);
                log += rn.log;
                if (!rn.ok) {
                    Log(log, "[persist] FAILED xml transform '%s'", xmlDisk.string().c_str());
                    return failKeepOld();
                }
                txmls.push_back(t);
            }
            const auto tXml = tmpDir / "persist_physics.xml";
            if (txmls.size() == 1) {
                CopyOverwrite(txmls[0], tXml);
            } else {
                const auto mx = MergeXml(tXml, txmls);
                log += mx.log;
                if (!mx.ok) {
                    Log(log, "[persist] FAILED xml merge");
                    return failKeepOld();
                }
            }

            // Revision + slot paths
            int rev = 1;
            if (oldFragment) {
                try {
                    rev = nlohmann::json::parse(*oldFragment).at("rev").get<int>() + 1;
                } catch (...) {
                }
            }
            const int slotIdx = rev % kSlots;
            const auto slotXmlDisk = xmlDir / ("Persist_physics_r" + std::to_string(slotIdx) + ".xml");
            const std::string slotXmlRel =
                "meshes\\CostumeFW\\XML\\Persist_physics_r" + std::to_string(slotIdx) + ".xml";

            // Build every part into tmp, validate, then publish the whole set.
            struct PartFile
            {
                std::string editorId;
                std::filesystem::path tmp;
                std::filesystem::path slotDisk;
                std::string slotRel;
            };
            std::vector<PartFile> partFiles;
            for (const auto& [mesh, editorId] : assign) {
                const bool isCarrier = (editorId == kPersistCarrierEditorId);
                nifly::NifFile part;
                if (part.Load(tBase) != 0 || !part.IsValid()) {
                    Log(log, "[persist] FAILED to reload master for part build");
                    return failKeepOld();
                }
                const auto all = part.GetShapes();
                nifly::NiShape* target = nullptr;
                for (auto* s : all) {
                    if (s->name.get() == mesh) {
                        target = s;
                        break;
                    }
                }
                if (!target) {
                    Log(log, "[persist] FAILED: shape '%s' missing from master", mesh.c_str());
                    return failKeepOld();
                }
                // Borrow a shader BEFORE dropping the donor (R4).
                if (part.GetShader(target) == nullptr) {
                    nifly::NiShape* donor = nullptr;
                    for (auto* s : all) {
                        if (part.GetShader(s) != nullptr) {
                            donor = s;
                            break;
                        }
                    }
                    auto* dref = donor ? donor->ShaderPropertyRef() : nullptr;
                    auto* tref = target->ShaderPropertyRef();
                    if (!dref || dref->index == nifly::NIF_NPOS || !tref) {
                        Log(log, "[persist] FAILED: cannot shader-fix proxy '%s'", mesh.c_str());
                        return failKeepOld();
                    }
                    tref->index = dref->index;
                }
                for (auto* s : all) {
                    if (s != target) {
                        part.GetHeader().DeleteBlock(part.GetBlockID(s));
                    }
                }
                if (isCarrier) {
                    auto* ex = FirstHdtExtra(part);
                    if (!ex) {
                        Log(log, "[persist] FAILED: carrier lost its HDT extra data");
                        return failKeepOld();
                    }
                    ex->stringData.get() = slotXmlRel;
                } else {
                    while (auto* sed = FirstHdtExtra(part)) {
                        part.GetHeader().DeleteBlock(part.GetBlockID(sed));
                    }
                }
                part.DeleteUnreferencedBlocks();

                const auto tOut = tmpDir / ("persist_" + editorId + ".nif");
                if (part.Save(tOut) != 0) {
                    Log(log, "[persist] FAILED to save part '%s'", editorId.c_str());
                    return failKeepOld();
                }
                std::string preason;
                if (!ValidateNifPathImpl(tOut, preason)) {
                    Log(log, "[persist] FAILED: part '%s' rejected - %s", editorId.c_str(), preason.c_str());
                    return failKeepOld();
                }

                PartFile pf;
                pf.editorId = editorId;
                pf.tmp = tOut;
                if (isCarrier) {
                    pf.slotDisk = carrierDir / ("Persist_carrier_r" + std::to_string(slotIdx) + ".nif");
                    pf.slotRel = "CostumeFW/Persist_carrier_r" + std::to_string(slotIdx) + ".nif";
                } else {
                    const std::string idx = editorId.substr(editorId.size() - 2);
                    pf.slotDisk = carrierDir / ("Persist_part" + idx + "_r" + std::to_string(slotIdx) + ".nif");
                    pf.slotRel = "CostumeFW/Persist_part" + idx + "_r" + std::to_string(slotIdx) + ".nif";
                }
                partFiles.push_back(std::move(pf));
            }

            // Final gate on the carrier part (R1-R4), then publish atomically.
            const auto& carrierPart = *std::find_if(partFiles.begin(), partFiles.end(),
                [](const PartFile& p) { return p.editorId == kPersistCarrierEditorId; });
            {
                const auto vh = ValidateHead(carrierPart.tmp);
                log += vh.log;
                if (!vh.ok) {
                    Log(log, "[persist] FAILED: carrier part rejected by validatehead - previous artifacts kept");
                    return failKeepOld();
                }
            }

            // Failed copies must surface (review P1-3). Old slot files (rev-1)
            // are untouched, so keeping the old fragment stays consistent even
            // after a partial publish of the NEW slot.
            bool pubOk = CopyOverwrite(tXml, slotXmlDisk);
            for (const auto& p : partFiles) {
                pubOk = CopyOverwrite(p.tmp, p.slotDisk) && pubOk;
            }
            pubOk = CopyOverwrite(carrierPart.tmp, basePath) && pubOk;
            if (!pubOk) {
                Log(log, "[persist] FAILED to publish part files - previous artifacts kept");
                return failKeepOld();
            }
            if (!WriteTextFile(hashPath, hash)) {
                Log(log, "[persist] WARNING failed to write hash file - persist rebuilds next run");
            }
            ++built;
            ensurePool();

            // Fragment: rev/file mirror the box entry shape, plus the
            // persist-specific xml + parts assignment (exact C# text shape).
            std::string xmlEsc;
            for (char c : slotXmlRel) {
                if (c == '\\') {
                    xmlEsc += "\\\\";
                } else {
                    xmlEsc += c;
                }
            }
            std::string frag = "{ \"rev\": " + std::to_string(rev) +
                               ", \"count\": " + std::to_string(smp.size()) +
                               ", \"file\": \"" + carrierPart.slotRel +
                               "\", \"xml\": \"" + xmlEsc + "\", \"parts\": [";
            bool pf1 = true;
            for (const auto& p : partFiles) {
                if (p.editorId == kPersistCarrierEditorId) {
                    continue;
                }
                if (!pf1) {
                    frag += ", ";
                }
                pf1 = false;
                frag += "{ \"editorid\": \"" + p.editorId + "\", \"file\": \"" + p.slotRel + "\" }";
            }
            frag += "] }";
            Log(log, "[persist] rev=%d -> %zu part(s), xml=%s", rev, partFiles.size(), slotXmlRel.c_str());
            return frag;
        }

    }  // namespace

    SyncResult Sync(const SyncOptions& opts)
    {
        SyncResult res;
        try {
            if (opts.manifestPath.empty() || opts.outRoot.empty() || opts.dataRoots.empty()) {
                Log(res.log,
                    "[sync] usage: sync <manifest.json> --data <root> [--data ...] --out <cefModRoot> [--empty <emptyToken.nif>]");
                return res;
            }

            const auto doc = nlohmann::json::parse(ReadTextFile(opts.manifestPath));
            const auto carrierDir = opts.outRoot / "meshes" / "CostumeFW";
            const auto xmlDir = carrierDir / "XML";
            const auto tmpDir = opts.outRoot / ".cef_tmp";
            std::filesystem::create_directories(carrierDir);
            std::filesystem::create_directories(xmlDir);
            std::filesystem::create_directories(tmpDir);

            const auto carriersJsonPath = carrierDir / "carriers.json";
            std::map<std::string, std::pair<int, std::string>> carriers;  // slot -> (rev, file)
            std::optional<std::string> persistFragment;
            if (std::filesystem::exists(carriersJsonPath)) {
                try {
                    const auto cj = nlohmann::json::parse(ReadTextFile(carriersJsonPath));
                    for (auto it = cj.begin(); it != cj.end(); ++it) {
                        if (it.key() == "persist") {
                            persistFragment = it.value().dump();
                            continue;
                        }
                        carriers[it.key()] = { it.value().at("rev").get<int>(),
                            it.value().at("file").get<std::string>() };
                    }
                } catch (...) {
                    Log(res.log, "[sync] WARNING: carriers.json unreadable, starting fresh");
                    carriers.clear();
                    persistFragment.reset();
                }
            }

            int poolCreated = 0;
            for (const auto& box : doc.at("boxes")) {
                const int slot = box.at("slot").get<int>();
                const std::string slotStr = std::to_string(slot);
                const std::string tag = "[sync] box" + slotStr + ":";
                const auto carrierPath = carrierDir / ("Box" + slotStr + "_carrier.nif");
                const auto mergedXmlDisk = xmlDir / ("Box" + slotStr + "_physics.xml");
                const std::string mergedXmlRel = "meshes\\CostumeFW\\XML\\Box" + slotStr + "_physics.xml";
                const auto hashPath = carrierDir / ("Box" + slotStr + "_carrier.hash");

                const int declared = box.contains("contents") ? static_cast<int>(box["contents"].size()) : 0;
                const auto smp = ResolveSmpContents(box["contents"], opts.dataRoots, tag,
                    "; box built from the rest", nullptr, res.log);

                const auto ensurePool = [&]() {
                    const std::filesystem::path seed =
                        std::filesystem::exists(carrierPath) ? carrierPath : opts.emptyNif;
                    const bool haveSeed = !seed.empty() && std::filesystem::exists(seed);
                    for (int i = 0; i < kSlots; ++i) {
                        const auto sn = carrierDir / ("Box" + slotStr + "_carrier_r" + std::to_string(i) + ".nif");
                        const auto sx = xmlDir / ("Box" + slotStr + "_physics_r" + std::to_string(i) + ".xml");
                        if (!std::filesystem::exists(sn) && haveSeed && CopyOverwrite(seed, sn)) {
                            ++poolCreated;
                        }
                        if (!std::filesystem::exists(sx) && WriteTextFile(sx, kEmptyXmlPlaceholder)) {
                            ++poolCreated;
                        }
                    }
                    if (!carriers.count(slotStr)) {
                        carriers[slotStr] = { 0, "CostumeFW/Box" + slotStr + "_carrier.nif" };
                    }
                };

                // A box that DECLARED contents but resolved none must not
                // clobber a good carrier down to empty (transient path miss).
                if (declared > 0 && smp.empty() && std::filesystem::exists(carrierPath)) {
                    Log(res.log, "%s all %d declared content(s) unresolved/excluded - keeping previous carrier",
                        tag.c_str(), declared);
                    ensurePool();
                    ++res.skipped;
                    continue;
                }

                const std::string hash = HashContents("v1|", smp);
                if (std::filesystem::exists(hashPath) && std::filesystem::exists(carrierPath) &&
                    ReadTextFile(hashPath) == hash) {
                    ensurePool();
                    ++res.skipped;
                    continue;
                }

                Log(res.log, "%s %zu SMP content(s)", tag.c_str(), smp.size());
                int rc = 0;
                // Build into a temp, VALIDATE, then publish to the base
                // carrier - a bad build never clobbers the last good carrier.
                const auto outTmp = tmpDir / ("box" + slotStr + "_out.nif");
                if (smp.empty()) {
                    if (opts.emptyNif.empty() || !std::filesystem::exists(opts.emptyNif)) {
                        Log(res.log, "%s no SMP contents and no --empty template - skipped", tag.c_str());
                        ++res.failed;
                        continue;
                    }
                    CopyOverwrite(opts.emptyNif, outTmp);
                } else if (smp.size() == 1) {
                    const auto za = ZeroAlpha(smp[0].nif, outTmp);
                    res.log += za.log;
                    rc = za.ok ? 0 : 1;
                } else {
                    // merge FIRST, then zero-alpha the whole merged NIF, then
                    // point the extra data at the unified XML.
                    const auto t1 = tmpDir / ("box" + slotStr + "_mg.nif");
                    const auto t2 = tmpDir / ("box" + slotStr + "_za.nif");
                    std::vector<std::filesystem::path> nifs;
                    std::vector<std::filesystem::path> xmls;
                    for (const auto& s : smp) {
                        nifs.push_back(s.nif);
                        xmls.push_back(s.xmlDisk);
                    }
                    const auto mg = Merge(t1, nifs);
                    res.log += mg.log;
                    rc = mg.ok ? 0 : 1;
                    if (rc == 0) {
                        const auto za = ZeroAlpha(t1, t2);
                        res.log += za.log;
                        rc = za.ok ? 0 : 1;
                    }
                    if (rc == 0) {
                        const auto mx = MergeXml(mergedXmlDisk, xmls);
                        res.log += mx.log;
                        rc = mx.ok ? 0 : 1;
                    }
                    if (rc == 0) {
                        const auto sx = SetXml(t2, outTmp, mergedXmlRel);
                        res.log += sx.log;
                        rc = sx.ok ? 0 : 1;
                    }
                }
                // Defense A: final gate. Never publish a carrier the SSE
                // loader would divide-by-zero on.
                std::string builtReason;
                if (rc == 0 && !ValidateNifPathImpl(outTmp, builtReason)) {
                    Log(res.log, "%s BUILT CARRIER REJECTED - %s; keeping previous carrier, revision NOT bumped",
                        tag.c_str(), builtReason.c_str());
                    rc = 3;
                }
                if (rc != 0) {
                    ++res.failed;
                    Log(res.log, "%s FAILED (rc=%d)", tag.c_str(), rc);
                    continue;
                }

                // Publish the validated build, rotate slots, then record the
                // hash LAST - a failed copy (usvfs, locks, AV) must surface as
                // a failure and must NOT leave a hash claiming success, or the
                // box would never rebuild (review P1-3).
                if (!CopyOverwrite(outTmp, carrierPath)) {
                    ++res.failed;
                    Log(res.log, "%s FAILED to publish carrier to %s", tag.c_str(), carrierPath.string().c_str());
                    continue;
                }
                ensurePool();

                const int rev = carriers.count(slotStr) ? carriers[slotStr].first + 1 : 1;
                const int slotIdx = rev % kSlots;
                const auto slotNifDisk = carrierDir / ("Box" + slotStr + "_carrier_r" + std::to_string(slotIdx) + ".nif");
                const std::string slotNifRel = "CostumeFW/Box" + slotStr + "_carrier_r" + std::to_string(slotIdx) + ".nif";
                if (smp.size() >= 2) {
                    // Slot carrier must reference the slot XML (FSMP may cache
                    // XML by path).
                    const auto slotXmlDisk = xmlDir / ("Box" + slotStr + "_physics_r" + std::to_string(slotIdx) + ".xml");
                    const std::string slotXmlRel =
                        "meshes\\CostumeFW\\XML\\Box" + slotStr + "_physics_r" + std::to_string(slotIdx) + ".xml";
                    if (!CopyOverwrite(mergedXmlDisk, slotXmlDisk)) {
                        ++res.failed;
                        Log(res.log, "%s FAILED to publish slot xml - revision NOT bumped", tag.c_str());
                        continue;
                    }
                    const auto sx = SetXml(carrierPath, slotNifDisk, slotXmlRel);
                    res.log += sx.log;
                    if (!sx.ok) {
                        Log(res.log, "%s WARNING slot carrier setxml failed", tag.c_str());
                    }
                } else {
                    if (!CopyOverwrite(carrierPath, slotNifDisk)) {
                        ++res.failed;
                        Log(res.log, "%s FAILED to publish slot carrier - revision NOT bumped", tag.c_str());
                        continue;
                    }
                }
                if (!WriteTextFile(hashPath, hash)) {
                    Log(res.log, "%s WARNING failed to write hash file - box rebuilds next run", tag.c_str());
                }
                ++res.built;
                carriers[slotStr] = { rev, slotNifRel };
                Log(res.log, "%s rev=%d -> %s", tag.c_str(), rev, slotNifRel.c_str());
            }

            // approach-C persist pipeline (after boxes: shares tmp + pools)
            if (doc.contains("persist")) {
                persistFragment = SyncPersistBuild(doc["persist"], opts.dataRoots, carrierDir, xmlDir,
                    tmpDir, opts.emptyNif, persistFragment, res.built, res.skipped, res.failed,
                    poolCreated, res.log);
            }

            // carriers.json: always rewrite (pre-existing file = externally
            // visible swap signal).
            std::string jsonOut = "{\n";
            bool first = true;
            for (const auto& kv : carriers) {
                if (!first) {
                    jsonOut += ",\n";
                }
                first = false;
                jsonOut += " \"" + kv.first + "\": { \"rev\": " + std::to_string(kv.second.first) +
                           ", \"file\": \"" + kv.second.second + "\" }";
            }
            if (persistFragment) {
                if (!first) {
                    jsonOut += ",\n";
                }
                first = false;
                jsonOut += " \"persist\": " + *persistFragment;
            }
            jsonOut += "\n}\n";
            if (!WriteTextFile(carriersJsonPath, jsonOut)) {
                Log(res.log, "[sync] FAILED to write carriers.json - new revisions are invisible to CEF");
                ++res.failed;
            }

            {
                std::error_code ec;
                std::filesystem::remove_all(tmpDir, ec);
            }
            if (poolCreated > 0) {
                Log(res.log,
                    "[sync] NOTE: created %d slot placeholder file(s) - a running game cannot see NEW files; they register on the next launch",
                    poolCreated);
            }
            Log(res.log, "[sync] done: built=%d skipped(unchanged)=%d failed=%d", res.built, res.skipped,
                res.failed);
            res.ok = (res.failed == 0);
        } catch (const std::exception& e) {
            Log(res.log, "[sync] FAILED: exception: %s", e.what());
            res.ok = false;
        } catch (...) {
            Log(res.log, "[sync] FAILED: unknown exception");
            res.ok = false;
        }
        return res;
    }

}  // namespace nifcarrier
