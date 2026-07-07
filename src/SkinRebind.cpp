#include "SkinRebind.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "Offsets.h"
#include "nifcarrier/NifCarrierCore.h"  // ContentNamePrefix (engine-free header)

#include "RE/B/BGSBipedObjectForm.h"
#include "RE/B/BGSHeadPart.h"
#include "RE/B/BGSTextureSet.h"
#include "RE/B/BSGeometry.h"
#include "RE/B/BSLightingShaderMaterialBase.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/B/BSModelDB.h"
#include "RE/B/BSShaderMaterial.h"
#include "RE/B/BSShaderProperty.h"
#include "RE/B/BSTextureSet.h"
#include "RE/B/BSVisit.h"
#include "RE/B/BSDismemberSkinInstance.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiSkinData.h"
#include "RE/N/NiSkinInstance.h"
#include "RE/P/PlayerCharacter.h"

#include <unordered_set>
#include "RE/S/Sexes.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESModelTextureSwap.h"
#include "RE/T/TESNPC.h"
#include "RE/T/TESObjectARMA.h"
#include "RE/T/TESObjectARMO.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        constexpr const char* kNodePrefix = "CostumeFW_";
        constexpr const char* kSkeletonRootName = "NPC Root [Root]";

        // Scene-node name for a logical id. The id may be a colon-form FormKey
        // ("XXXXXX:Plugin.esp"); ':'/space/etc. are sanitized for the node name
        // (spec 8-1). The registry still keys on the original id.
        std::string NodeName(const std::string& a_id)
        {
            std::string s = std::string(kNodePrefix) + a_id;
            for (auto& c : s) {
                if (c == ':' || c == ' ' || c == '[' || c == ']' ||
                    c == '\\' || c == '/' || c == '.') {
                    c = '_';
                }
            }
            return s;
        }

        // In-memory registry of active items so the reattach hook can re-inject
        // after a player 3D rebuild (cell change / load / transform). Co-save
        // persistence across game restarts comes later. Main-thread access only.
        // One model + its alternate-texture set. The swap pointer is stable
        // (lives on the ARMA form). 3rd- and 1st-person models differ.
        struct ModelRef
        {
            std::string nifPath;                  // raw; meshes\ prefix stripped at use
            const RE::TESModelTextureSwap* swap;  // alternate textures (or nullptr)
        };

        struct ActiveItem
        {
            std::string id;
            ModelRef m3p;               // 3rd-person worn model (bipedModels[sex])
            ModelRef m1p;               // 1st-person model (bipedModel1stPersons[sex])
            std::string tokenId;        // box token colon-form id; empty = persist class
            RE::FormID tokenForm{ 0 };  // resolved token FormID (0 if persist)
            RE::SEX resolvedSex{ RE::SEXES::kFemale };  // sex m3p/m1p were resolved for
        };
        std::vector<ActiveItem> g_active;

        void Register(const std::string& a_id, const ModelRef& a_m3p, const ModelRef& a_m1p,
            const std::string& a_tokenId = {}, RE::FormID a_tokenForm = 0,
            RE::SEX a_sex = RE::SEXES::kFemale)
        {
            for (auto& it : g_active) {
                if (it.id == a_id) {
                    it.m3p = a_m3p;
                    it.m1p = a_m1p;
                    it.tokenId = a_tokenId;
                    it.tokenForm = a_tokenForm;
                    it.resolvedSex = a_sex;
                    return;
                }
            }
            g_active.push_back({ a_id, a_m3p, a_m1p, a_tokenId, a_tokenForm, a_sex });
        }

        // Remove the CostumeFW_<id> nodes from both skeletons WITHOUT touching the
        // registry (used to hide a box item whose token is unequipped). Detaches
        // EVERY same-named node on each root (not just the first) so a duplicate
        // holder can't survive and keep rendering.
        // --- bound-bone pinning --------------------------------------------------
        // The engine's skin instance holds bones as RAW pointers; normally that's
        // fine (skeleton bones outlive meshes), but CEF binds to FSMP's transient
        // merge nodes, and once FSMP releases a retired generation those raw
        // pointers dangle - the renderer reads freed world matrices, and the
        // dead-bind sweep crashed reading a freed bone's name (CTD 2026-07-04
        // 18:23, ucrtbase strlen on 0x2A6, BSDismemberSkinInstance in RBX). Pin
        // every bone an item is bound to with a NiPointer for as long as the
        // item is attached; DetachNodes releases the pins.
        std::unordered_map<std::string, std::vector<RE::NiPointer<RE::NiAVObject>>> g_boundBoneRefs;
        std::vector<RE::NiPointer<RE::NiAVObject>>* g_boneRefSink = nullptr;

        void DetachNodes(const std::string& a_id)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            g_boundBoneRefs.erase(a_id);  // release the bone pins with the attach
            const std::string nodeName = NodeName(a_id);
            auto detachFrom = [&](RE::NiAVObject* a_root, const char* a_tag) {
                if (!a_root) {
                    return;
                }
                int removed = 0;
                // GetObjectByName returns the FIRST match only; loop until none.
                while (auto* node = a_root->GetObjectByName(nodeName)) {
                    auto* parent = node->parent;
                    if (!parent) {
                        break;  // can't detach a parentless match; avoid infinite loop
                    }
                    parent->DetachChild(node);
                    ++removed;
                }
                SKSE::log::debug("  DetachNodes[{}] '{}' removed {}", a_tag, nodeName, removed);
            };
            detachFrom(player->Get3D(false), "3p");
            detachFrom(player->Get3D(true), "1p");
        }

        // Recursively collect every node whose name starts with kNodePrefix. Does
        // not descend INTO a matched holder (its children are the bare geometry).
        void CollectInjected(RE::NiAVObject* a_obj, std::vector<RE::NiAVObject*>& a_out)
        {
            if (!a_obj) {
                return;
            }
            if (std::string_view(a_obj->name.c_str()).starts_with(kNodePrefix)) {
                a_out.push_back(a_obj);
                return;
            }
            if (auto* node = a_obj->AsNode()) {
                for (auto& child : node->GetChildren()) {
                    CollectInjected(child.get(), a_out);
                }
            }
        }

        void Unregister(const std::string& a_id)
        {
            std::erase_if(g_active, [&](const ActiveItem& it) { return it.id == a_id; });
        }

        // BSModelDB::Demand expects a Data\Meshes-relative path: strip a leading
        // "meshes\" (or "meshes/") prefix, case-insensitive (v2 spec 3-1).
        std::string StripMeshesPrefix(std::string a_path)
        {
            std::replace(a_path.begin(), a_path.end(), '/', '\\');
            constexpr std::string_view kPrefix = "meshes\\";
            if (a_path.size() >= kPrefix.size()) {
                std::string head = a_path.substr(0, kPrefix.size());
                std::transform(head.begin(), head.end(), head.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (head == kPrefix) {
                    a_path.erase(0, kPrefix.size());
                }
            }
            return a_path;
        }

        RE::NiPointer<RE::NiNode> LoadNif(const std::string& a_relPath)
        {
            RE::NiPointer<RE::NiNode> out;
            RE::BSModelDB::DBTraits::ArgsType args{};  // defaults preserve skin instances
            auto err = RE::BSModelDB::Demand(a_relPath.c_str(), out, args);
            if (err != RE::BSResource::ErrorCode::kNone) {
                SKSE::log::error("LoadNif failed (err={}) for '{}'",
                    static_cast<std::uint32_t>(err), a_relPath);
                return nullptr;
            }
            return out;
        }

        // FSMP merges the physics bones it builds into the LIVE skeleton under a
        // renamed node: "hdtSSEPhysics_AutoRename_Armor_<8hex> <origName>" (equip
        // armor-attach path) or "hdtSSEPhysics_AutoRename_Head_<8hex> <origName>"
        // (facegen head path). The <8hex> id is a per-skeleton SEQUENTIAL counter
        // (confirmed in hdtSMP64 ActorManager.cpp:856 - it is NOT a formID), so we
        // can't construct the name up-front. Search the skeleton for a node whose
        // name is exactly <known-prefix><8 hex><space><bone>. Returns first BFS
        // match, or null. If found, the injected mesh can bind to the SIMULATED
        // bone and get real SMP sway (instead of the static ancestor remap).
        // Parse "hdtSSEPhysics_AutoRename_(Armor|Head|External)_<8hex> <bone>".
        // FSMP merge ids are per-skeleton sequential counters, so a HIGHER id is
        // a NEWER generation of the same class.
        bool ParseRenamedBone(std::string_view a_nm, bool& a_head, std::uint32_t& a_id,
            std::string_view& a_suffix)
        {
            static constexpr std::string_view kArmor = "hdtSSEPhysics_AutoRename_Armor_";
            static constexpr std::string_view kHead = "hdtSSEPhysics_AutoRename_Head_";
            // Future FSMP external-node API (fsmp_patches/): merges would land
            // under an "External" prefix. Provisionally classed with Armor
            // (per-item lifecycle, explicit detach) until that API ships - inert
            // today, no released FSMP emits this prefix.
            static constexpr std::string_view kExternal = "hdtSSEPhysics_AutoRename_External_";
            std::string_view pfx;
            if (a_nm.starts_with(kHead)) {
                a_head = true;
                pfx = kHead;
            } else if (a_nm.starts_with(kArmor)) {
                a_head = false;
                pfx = kArmor;
            } else if (a_nm.starts_with(kExternal)) {
                a_head = false;
                pfx = kExternal;
            } else {
                return false;
            }
            if (a_nm.size() < pfx.size() + 8 + 2) {
                return false;
            }
            const std::string_view hex = a_nm.substr(pfx.size(), 8);
            if (!std::all_of(hex.begin(), hex.end(),
                    [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                return false;
            }
            if (a_nm[pfx.size() + 8] != ' ') {
                return false;
            }
            a_id = static_cast<std::uint32_t>(std::strtoul(std::string(hex).c_str(), nullptr, 16));
            a_suffix = a_nm.substr(pfx.size() + 8 + 1);
            return true;
        }

        // Selection policy (learned the hard way, 2026-07-04):
        // - PREFER Head over Armor: an Armor merge comes from a WORN item and
        //   dies on unequip (a mesh bound there stretches to garbage); box
        //   contents are hidden while their token is unworn, so Head-first is
        //   correct for both classes.
        // - Within a class, PREFER THE HIGHEST id: FSMP keeps the RETIRING
        //   generation attached for a few seconds after a head rebuild /
        //   re-equip, so "attached" does not mean "current" - a bind taken in
        //   that overlap window latches onto never-again-simulated nodes
        //   (slightly stretched, half-swaying veil at load). The newest id is
        //   the generation FSMP actually simulates.
        RE::NiAVObject* FindFsmpRenamedBone(RE::NiAVObject* a_root, const char* a_bone)
        {
            if (!a_root || !a_bone || !*a_bone) {
                return nullptr;
            }
            const std::string_view bone{ a_bone };
            RE::NiAVObject* bestHead = nullptr;
            RE::NiAVObject* bestArmor = nullptr;
            std::uint32_t bestHeadId = 0, bestArmorId = 0;
            std::vector<RE::NiAVObject*> stack{ a_root };
            while (!stack.empty()) {
                auto* obj = stack.back();
                stack.pop_back();
                if (!obj) {
                    continue;
                }
                bool head = false;
                std::uint32_t id = 0;
                std::string_view suffix;
                if (ParseRenamedBone(std::string_view(obj->name.c_str()), head, id, suffix) &&
                    suffix == bone) {
                    if (head && (!bestHead || id > bestHeadId)) {
                        bestHead = obj;
                        bestHeadId = id;
                    } else if (!head && (!bestArmor || id > bestArmorId)) {
                        bestArmor = obj;
                        bestArmorId = id;
                    }
                }
                if (auto* node = obj->AsNode()) {
                    for (auto& child : node->GetChildren()) {
                        stack.push_back(child.get());
                    }
                }
            }
            return bestHead ? bestHead : bestArmor;
        }

        // --- rebind retry (FSMP carrier attach race) ---------------------------
        // An injection that runs in the same beat as a token (re)equip / player 3D
        // rebuild can rebind BEFORE the engine finishes the (async) load+attach of
        // the carrier NIF and FSMP grows its renamed bones - heavier carriers
        // widen the window (observed in-game 2026-07-02 with the 1.9MB box44
        // carrier). Injection is idempotent by node name, so a plain Reconcile
        // can NOT fix a static-bound mesh: the retry must DETACH the items whose
        // 3rd-person rebind fell to the static fallback and re-inject them once
        // the carrier had time to attach. Budgeted per external trigger so
        // contents whose custom bones genuinely have no carrier (persist items)
        // cost at most kRebindRetryBudget extra passes, never a loop.
        constexpr int kRebindRetryBudget = 4;  // heavy carriers load async for seconds
        constexpr auto kRebindRetryDelay = std::chrono::milliseconds(1000);

        std::atomic<int> g_rebindRetryBudget{ 0 };
        std::atomic<bool> g_rebindRetryQueued{ false };
        bool g_inRebindRetry = false;          // main-thread only (tasks + Reconcile)
        bool g_injectStatic3p = false;         // set by RebindGeometry, read by InjectInternal
        std::vector<std::string> g_rebindRetryIds;  // items to detach+re-inject on retry

        // Per-content carrier bone prefix of the item currently being injected
        // (v1.2.1 multi-content namespace isolation, nifcarrier IsolateContent):
        // a multi-content carrier's copy of this content's custom bones is
        // "C<fnv1a32>_<bone>", so the FSMP-renamed lookup must try the prefixed
        // name first, then the plain one (single-content carriers stay plain).
        // Set by InjectInternal, read by RebindGeometry. Main thread only.
        std::string g_rebindPrefix;

        // --- persist head-rebuild debounce + diagnostics (Codex Phase 2) -------
        // ApplyPersistCarrier can fire in bursts (settings writes, sync
        // completion, load passes); each DoReset3D makes FSMP rebuild the wig
        // physics and SSE Engine Fixes commits (and RETAINS) a ~2.5GB arena per
        // rebuild. Coalesce a burst into ONE DoReset3D, and give the dead-bind
        // watchdog a grace window afterwards so it doesn't judge FSMP's
        // generation-overlap as "dead" and pile on re-injects (each of which can
        // rebuild again).
        std::atomic<bool> g_headRebuildQueued{ false };
        std::atomic<std::uint64_t> g_headRebuildRev{ 0 };
        std::chrono::steady_clock::time_point g_headRebuildGraceUntil{};  // main thread only

        struct PersistDiag
        {
            std::uint64_t headRebuildRequested{ 0 };
            std::uint64_t headRebuildExecuted{ 0 };
            std::uint64_t reconcileCalls{ 0 };
            std::uint64_t watchdogReconciles{ 0 };
            std::uint64_t deadBindReinjects{ 0 };
            std::uint64_t rebindRetries{ 0 };
            std::uint64_t bodyMorphApplies{ 0 };
        };
        PersistDiag g_persistDiag;

        void DetachNodes(const std::string& a_id);  // fwd (defined below)

        void RunAfterDelay(std::chrono::steady_clock::time_point a_due, std::function<void()> a_fn)
        {
            // Sleep on a background thread, then hand the payload to the SKSE
            // task queue ONCE. The previous implementation re-queued itself
            // through the task pump every cycle until due - safe while the
            // pump is frame-bound (rebind retry ran that way for days), but
            // the EARLY loading-screen pump drains tasks INCLUSIVELY, so an
            // un-due requeue re-ran forever and froze the load at the exact
            // moment a chain started there (in-game 2026-07-04 17:35, the
            // bind watchdog's first tick scheduled from the kDataLoaded
            // Reconcile). AddTask is thread-safe by design.
            std::thread([a_due, fn = std::move(a_fn)]() mutable {
                std::this_thread::sleep_until(a_due);
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(fn));
                }
            }).detach();
        }

        void RequestRebindRetry(const std::string& a_id)
        {
            if (std::find(g_rebindRetryIds.begin(), g_rebindRetryIds.end(), a_id) ==
                g_rebindRetryIds.end()) {
                g_rebindRetryIds.push_back(a_id);
            }
            if (g_rebindRetryQueued.load()) {
                return;
            }
            if (g_rebindRetryBudget.fetch_sub(1) <= 0) {
                g_rebindRetryBudget.fetch_add(1);  // keep at 0
                return;
            }
            g_rebindRetryQueued = true;
            SKSE::log::info("  rebind retry queued (+{}ms): FSMP carrier may still be attaching",
                std::chrono::duration_cast<std::chrono::milliseconds>(kRebindRetryDelay).count());
            RunAfterDelay(std::chrono::steady_clock::now() + kRebindRetryDelay, []() {
                g_rebindRetryQueued = false;
                ++g_persistDiag.rebindRetries;
                const auto ids = std::move(g_rebindRetryIds);
                g_rebindRetryIds.clear();
                SKSE::log::info("rebind retry: re-injecting {} static item(s)", ids.size());
                for (const auto& id : ids) {
                    DetachNodes(id);  // clear the static attach so injection re-runs
                }
                g_inRebindRetry = true;
                Reconcile();  // re-injects the detached items (others skip, idempotent)
                g_inRebindRetry = false;
            });
        }

        // --- dead-bind sweep (FSMP merge generations) --------------------------
        // BOTH FSMP merge classes are GENERATIONAL: an Armor merge dies when its
        // worn item is removed, and a Head merge dies on every facegen head
        // rebuild - FSMP detaches the old renamed nodes and re-merges under a
        // new sequential id. Observed in-game 2026-07-04: the load sequence
        // builds the head twice; a persist veil bound to Head_00000001 kept the
        // DETACHED (world-frozen) nodes after the rebuild to Head_00000002 and
        // stretched between them and its live bones. An injected mesh cannot
        // heal itself (injection is idempotent by node name), so every
        // Reconcile sweeps for bones that no longer hang under the live
        // skeleton and detaches the item for re-injection into the CURRENT
        // generation. Only FSMP-renamed bones can die this way; plain skeleton
        // bones live until the full 3D rebuild, which re-triggers injection
        // anyway (Load3D hook).
        bool HasDeadPhysicsBind(const std::string& a_id, RE::NiAVObject* a_root3p)
        {
            auto* holder = a_root3p ? a_root3p->GetObjectByName(NodeName(a_id)) : nullptr;
            if (!holder) {
                return false;  // not injected on the 3p skeleton - nothing to sweep
            }

            // One BFS over the LIVE tree: the set of attached renamed nodes (by
            // POINTER - liveness is membership, never a parent-chain walk: a
            // retired node's ->parent can dangle even while the node itself is
            // pinned alive) and the newest attached generation per (class, bone).
            // "Attached" alone is NOT enough: FSMP keeps the retiring generation
            // on the skeleton for a few seconds, and a bind taken in that overlap
            // window sways wrong without ever being detached.
            std::unordered_set<const RE::NiAVObject*> attachedRenamed;
            std::unordered_map<std::string, std::uint32_t> maxHead, maxArmor;
            {
                std::vector<RE::NiAVObject*> stack{ a_root3p };
                while (!stack.empty()) {
                    auto* obj = stack.back();
                    stack.pop_back();
                    if (!obj) {
                        continue;
                    }
                    bool head = false;
                    std::uint32_t id = 0;
                    std::string_view suffix;
                    if (ParseRenamedBone(std::string_view(obj->name.c_str()), head, id, suffix)) {
                        attachedRenamed.insert(obj);
                        auto& m = head ? maxHead : maxArmor;
                        auto [it, inserted] = m.try_emplace(std::string(suffix), id);
                        if (!inserted && id > it->second) {
                            it->second = id;
                        }
                    }
                    if (auto* node = obj->AsNode()) {
                        for (auto& child : node->GetChildren()) {
                            stack.push_back(child.get());
                        }
                    }
                }
            }

            bool dead = false;
            RE::BSVisit::TraverseScenegraphGeometries(holder,
                [&](RE::BSGeometry* a_geom) {
                    auto skin = a_geom->GetGeometryRuntimeData().skinInstance;
                    if (!skin || !skin->bones || !skin->skinData) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const std::uint32_t n = skin->skinData->bones;
                    for (std::uint32_t i = 0; i < n; ++i) {
                        auto* bone = skin->bones[i];
                        if (!bone) {
                            continue;
                        }
                        // Safe to read: every bound bone is pinned via
                        // g_boundBoneRefs for as long as the item is attached.
                        bool head = false;
                        std::uint32_t id = 0;
                        std::string_view suffix;
                        if (!ParseRenamedBone(std::string_view(bone->name.c_str()), head, id, suffix)) {
                            continue;
                        }
                        // Dead: no longer attached (generation retired).
                        if (!attachedRenamed.contains(bone)) {
                            dead = true;
                            return RE::BSVisit::BSVisitControl::kStop;
                        }
                        // Stale: a NEWER generation of the same class exists - only
                        // the newest is simulated by FSMP.
                        const auto& m = head ? maxHead : maxArmor;
                        if (auto it = m.find(std::string(suffix)); it != m.end() && id < it->second) {
                            dead = true;
                            return RE::BSVisit::BSVisitControl::kStop;
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return dead;
        }

        // Resolve every bone of one geometry's skin against the live skeleton.
        // Two-pass gate: resolve ALL first; only commit if every bone resolved
        // (avoids the "missing skeleton root node" fatal / partial rebind).
        // Rebind one geometry's skin to the LIVE actor skeleton: resolve each
        // bone by name against a_root, repoint bones[] AND boneWorldTransforms[]
        // to the live bone, set rootParent = a_root. (skee AttachMesh.)
        bool RebindGeometry(RE::NiSkinInstance* a_skin, RE::NiAVObject* a_root)
        {
            if (!a_skin->skinData || !a_skin->bones) {
                SKSE::log::warn("  skin missing skinData/bones - cannot rebind");
                return true;
            }
            // AUTHORED bone count from skinData. numMatrices (the runtime matrix
            // count) is 0 on a freshly-loaded/unattached skin - using it skipped
            // the entire rebind. THIS was the bug. skee uses skinData bone count.
            const std::uint32_t n = a_skin->skinData->bones;
            const char* rttiName = a_skin->GetRTTI() ? a_skin->GetRTTI()->name : "<null>";
            SKSE::log::debug("  skin rtti='{}' boneCount={} numMatrices={} worldXf={}",
                rttiName, n, a_skin->numMatrices, a_skin->boneWorldTransforms != nullptr);
            if (n == 0) {
                return true;
            }

            std::uint32_t remapCount = 0;
            std::string firstRemap;
            std::uint32_t fsmpCount = 0;
            std::string firstFsmp;
            std::vector<RE::NiAVObject*> resolved(n, nullptr);
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::NiAVObject* src = a_skin->bones[i];
                if (!src) {
                    SKSE::log::warn("  bone[{}] is null in source skin", i);
                    return false;
                }
                RE::NiAVObject* tgt = a_root->GetObjectByName(src->name);
                if (!tgt) {
                    // FSMP physics-driven bone: if this outfit's custom SMP bone was
                    // built by FSMP (via a physics carrier / box token / head part),
                    // it lives in the skeleton under a renamed node
                    // "hdtSSEPhysics_AutoRename_(Armor|Head)_<id> <bone>". Bind THERE
                    // so the injected mesh follows the simulated bone = real SMP sway,
                    // instead of falling through to the static ancestor remap below.
                    // Multi-content carriers prefix this content's custom bones
                    // (namespace isolation) - try the prefixed name first so the
                    // mesh binds ITS OWN chain, not a same-named chain of another
                    // content (or of the plain worn outfit).
                    if (!g_rebindPrefix.empty()) {
                        const std::string prefixed = g_rebindPrefix + src->name.c_str();
                        tgt = FindFsmpRenamedBone(a_root, prefixed.c_str());
                    }
                    if (!tgt) {
                        tgt = FindFsmpRenamedBone(a_root, src->name.c_str());
                    }
                    if (tgt) {
                        ++fsmpCount;
                        if (firstFsmp.empty()) {
                            firstFsmp = std::string(src->name.c_str()) + "->" + tgt->name.c_str();
                        }
                    }
                }
                if (!tgt) {
                    // Ancestor remap: a bone the live skeleton lacks - typically an
                    // outfit-specific SMP/physics bone (e.g. 'SeraPantyL_A 1') that
                    // only exists when that outfit is equipped+processed. Walk the
                    // content NIF's OWN bone hierarchy (still intact at this point)
                    // to the nearest ancestor that DOES exist on the live skeleton
                    // and bind there. The geometry then SHOWS (that part static - no
                    // SMP sway, which an injected mesh can't get anyway) instead of
                    // the whole shape vanishing via the old bone gate.
                    for (RE::NiAVObject* anc = src->parent; anc; anc = anc->parent) {
                        if (auto* r = a_root->GetObjectByName(anc->name)) {
                            tgt = r;
                            break;
                        }
                    }
                    if (!tgt) {
                        tgt = a_root;  // last resort: no resolvable ancestor at all
                    }
                    ++remapCount;
                    if (firstRemap.empty()) {
                        firstRemap = std::string(src->name.c_str()) + "->" + tgt->name.c_str();
                    }
                }
                resolved[i] = tgt;
            }
            if (fsmpCount) {
                SKSE::log::info("  bound {} bone(s) to FSMP physics-driven node(s) "
                                "(SMP sway; e.g. {})", fsmpCount, firstFsmp);
            }
            if (remapCount) {
                SKSE::log::warn("  remapped {} unresolved bone(s) to nearest ancestor "
                                "(static, no SMP sway; e.g. {})", remapCount, firstRemap);
                // Only a 3rd-person static fallback can mean "carrier still
                // attaching" - FSMP never builds physics on the 1st-person
                // skeleton, so a 1p remap is permanent and no reason to retry.
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (pc && a_root != pc->Get3D(true)) {
                    g_injectStatic3p = true;  // InjectInternal turns this into a retry
                }
            }

            const bool haveWorldXf = (a_skin->boneWorldTransforms != nullptr);
            for (std::uint32_t i = 0; i < n; ++i) {
                a_skin->bones[i] = resolved[i];
                if (haveWorldXf) {
                    a_skin->boneWorldTransforms[i] = &resolved[i]->world;
                }
                if (g_boneRefSink) {
                    g_boneRefSink->emplace_back(resolved[i]);  // pin (see g_boundBoneRefs)
                }
            }
            a_skin->rootParent = a_root;  // bind to live actor root (skee)
            a_skin->numMatrices = n;      // runtime matrix count (was 0 when loaded)
            if (!haveWorldXf) {
                SKSE::log::warn("  boneWorldTransforms is null - mesh may not deform");
            }

            // Worn-armor NIFs carry a BSDismemberSkinInstance whose partitions
            // claim biped slots (32 body / 33 hands). Once bound to the actor,
            // those partitions enter the body-part arbitration and can HIDE the
            // body. Neutralize them: keep visible, re-slot to an unused slot.
            if (std::string_view(rttiName) == "BSDismemberSkinInstance") {
                auto* dsi = static_cast<RE::BSDismemberSkinInstance*>(a_skin);
                auto& rd = dsi->GetRuntimeData();
                for (std::int32_t i = 0; i < rd.numPartitions; ++i) {
                    rd.partitions[i].editorVisible = true;
                    rd.partitions[i].startNetBoneSet = (i == 0);
                    rd.partitions[i].slot = 61;  // unused biped slot - claims nothing
                }
                SKSE::log::debug("  neutralized {} dismember partition(s)", rd.numPartitions);
            }
            return true;
        }

        // Apply one BGSTextureSet to a geometry's lighting-shader material IN
        // PLACE (safe: our cloned NIF owns this material). Uses only type-correct
        // virtuals - NOT the community-REL InitializeShader that crashed on the
        // envmap material. (skee/po3 pattern, minus the material clone we don't
        // need because the material is private.)
        bool ApplyTextureSet(RE::BSGeometry* a_geom, RE::BGSTextureSet* a_txst)
        {
            if (!a_geom || !a_txst) {
                return false;
            }
            auto& rt = a_geom->GetGeometryRuntimeData();
            auto* effect = rt.properties[RE::BSGeometry::States::kEffect].get();
            auto* ls = ::netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
            if (!ls || !ls->material) {
                return false;
            }
            auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(ls->material);
            material->ClearTextures();                                       // vfunc 0x09
            material->OnLoadTextureSet(0, static_cast<RE::BSTextureSet*>(a_txst));  // vfunc 0x08
            ls->SetupGeometry(a_geom);        // vfunc 0x27
            ls->FinishSetupGeometry(a_geom);  // vfunc 0x28
            return true;
        }

        // For each ARMA alternate-texture entry, find the matching shape by name
        // under the holder and apply its TXST (the ESP's color variant).
        void ApplyAltTextures(RE::NiAVObject* a_holder, const RE::TESModelTextureSwap* a_swap)
        {
            if (!a_swap || a_swap->numAlternateTextures == 0 || !a_swap->alternateTextures) {
                return;
            }
            RE::BSVisit::TraverseScenegraphGeometries(a_holder,
                [&](RE::BSGeometry* a_geom) {
                    for (std::uint32_t i = 0; i < a_swap->numAlternateTextures; ++i) {
                        const auto& alt = a_swap->alternateTextures[i];
                        if (alt.textureSet && a_geom->name == alt.name3D) {
                            if (ApplyTextureSet(a_geom, alt.textureSet)) {
                                SKSE::log::debug("  applied TXST to shape '{}'", a_geom->name.c_str());
                            }
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        // Body morph (skee body-slider vertex diff) is OPT-IN per content, default
        // OFF (BodyMorphOn). It is only needed for BodySlide/body-conforming meshes;
        // applying it to accessories (hair/nails/piercings/veil) is unnecessary and
        // drove a severe memory balloon - skee ApplyVertexDiff makes huge
        // allocations that SSE Engine Fixes' allocator commits into 2.5GB arenas
        // and retains (memory dump 2026-07-05: 6x2.5GB EngineFixes arenas). CEF's
        // custom-slot content can't be auto-classified (nails/piercings use
        // arbitrary modder-chosen slots), so the user turns it on per content when
        // a mesh actually looks wrong (`cef morph <id> on` / MCM checkbox).
        bool ShouldApplyBodyMorph(const std::string& a_id)
        {
            return BodyMorphOn(a_id);
        }

        // Inject onto one skeleton root (3D root). Returns true on attach.
        // a_applyMorph gates skee body morph (off for hair/head content, which
        // must not receive body-slider deformation).
        bool InjectOnRoot(RE::NiAVObject* a_root3D, const std::string& a_relPath,
            const std::string& a_nodeName, const RE::TESModelTextureSwap* a_swap,
            bool a_applyMorph)
        {
            if (!a_root3D) {
                return false;
            }
            // Idempotency: already present on this skeleton?
            if (a_root3D->GetObjectByName(a_nodeName)) {
                SKSE::log::debug("  already attached: {}", a_nodeName);
                return true;
            }

            auto loaded = LoadNif(a_relPath);
            if (!loaded) {
                return false;
            }

            // Private copy so we never mutate the shared cached model.
            RE::NiPointer<RE::NiNode> clone;
            if (auto* c = loaded->Clone()) {
                clone = RE::NiPointer<RE::NiNode>(c->AsNode());
            }
            if (!clone) {
                SKSE::log::error("  Clone() failed");
                return false;
            }

            RE::NiAVObject* skelRootObj = a_root3D->GetObjectByName(kSkeletonRootName);
            RE::NiNode* attachRoot = skelRootObj ? skelRootObj->AsNode() : a_root3D->AsNode();
            if (!attachRoot) {
                SKSE::log::error("  no attach root node");
                return false;
            }
            SKSE::log::debug("  root3D='{}' attachRoot='{}'",
                a_root3D->name.c_str(), attachRoot->name.c_str());

            // Collect the skinned geometry and rebind each to the live skeleton.
            // Bind/resolve against the live actor root (a_root3D), as skee does.
            std::vector<RE::NiPointer<RE::BSGeometry>> geoms;
            bool ok = true;
            RE::BSVisit::TraverseScenegraphGeometries(clone.get(),
                [&](RE::BSGeometry* a_geom) {
                    auto skin = a_geom->GetGeometryRuntimeData().skinInstance;
                    if (skin) {
                        if (!RebindGeometry(skin.get(), a_root3D)) {
                            ok = false;
                            return RE::BSVisit::BSVisitControl::kStop;
                        }
                        geoms.emplace_back(RE::NiPointer<RE::BSGeometry>(a_geom));
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });

            // Unresolved bones are now ancestor-remapped (not gated), so RebindGeometry
            // only fails on a corrupt skin (null source bone). NOT-SHOWN summaries:
            if (!ok) {
                SKSE::log::warn("NOT SHOWN '{}' on '{}': skin rebind failed (corrupt source bone)",
                    a_nodeName, a_root3D->name.c_str());
                return false;
            }
            if (geoms.empty()) {
                SKSE::log::warn("NOT SHOWN '{}' on '{}': no skinned geometry in NIF",
                    a_nodeName, a_root3D->name.c_str());
                return false;
            }

            // Reparent the BARE geometry into a fresh holder, leaving the NIF's
            // own internal bone nodes behind in 'clone' (destroyed at scope end).
            // Attaching the internal skeleton was the static/float cause.
            RE::NiNode* holder = RE::NiNode::Create(0);
            holder->name = a_nodeName.c_str();
            for (auto& g : geoms) {
                if (auto* p = g->parent) {
                    p->DetachChild(g.get());
                }
                holder->AttachChild(g.get(), true);
            }

            attachRoot->AttachChild(holder, true);

            RE::NiUpdateData updateData{};
            updateData.flags.set(RE::NiUpdateData::Flag::kDirty);
            holder->Update(updateData);

            // Apply the ARMA's alternate texture set (color variant) per shape.
            ApplyAltTextures(holder, a_swap);

            // Match the morphed body: apply the player's skee BodyMorph (RaceMenu
            // sliders) vertex diff to the injected shapes. The equip system never
            // sees this mesh, so without this it keeps its un-morphed shape and
            // clips through a 3BA/CBBE body. No-op if skee is absent. Skipped for
            // hair/head content (a wig must not get body-slider deformation).
            if (a_applyMorph) {
                ++g_persistDiag.bodyMorphApplies;
                BodyMorph::ApplyToNode(RE::PlayerCharacter::GetSingleton(), holder);
            }

            SKSE::log::debug("  attached {} ({} skinned shape(s))", a_nodeName, geoms.size());
            return true;
        }

        // Inject on player 3D: the 3P model on the 3rd-person skeleton, the 1P
        // model on the 1st-person skeleton. Each carries its own alt textures.
        bool InjectInternal(const std::string& a_id, const ModelRef& a_m3p, const ModelRef& a_m1p)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            const std::string nodeName = NodeName(a_id);
            const bool applyMorph = ShouldApplyBodyMorph(a_id);
            SKSE::log::info("  bodymorph gate '{}' -> {}",
                a_id, applyMorph ? "apply (opted in)" : "SKIP (not opted in)");

            bool any = false;
            g_injectStatic3p = false;
            // Multi-content carriers prefix this content's custom bones
            // (nifcarrier namespace isolation) - same id, same prefix.
            g_rebindPrefix = nifcarrier::ContentNamePrefix(a_id);
            // Collect the bones this injection binds to, then APPEND them to the
            // item's pin set (append, not replace: an idempotent skip on one
            // skeleton must not drop the pins the other skeleton still uses).
            std::vector<RE::NiPointer<RE::NiAVObject>> collected;
            g_boneRefSink = &collected;
            if (auto* root3p = player->Get3D(false); root3p && !a_m3p.nifPath.empty()) {
                any |= InjectOnRoot(root3p, StripMeshesPrefix(a_m3p.nifPath), nodeName, a_m3p.swap, applyMorph);
            }
            if (g_injectStatic3p) {
                // This item's 3p rebind fell to the static fallback - the carrier
                // may still be attaching. Queue a detach+re-inject for it.
                RequestRebindRetry(a_id);
            }
            if (auto* root1p = player->Get3D(true); root1p && !a_m1p.nifPath.empty()) {
                any |= InjectOnRoot(root1p, StripMeshesPrefix(a_m1p.nifPath), nodeName, a_m1p.swap, applyMorph);
            }
            g_boneRefSink = nullptr;
            if (!collected.empty()) {
                auto& refs = g_boundBoneRefs[a_id];
                refs.insert(refs.end(), collected.begin(), collected.end());
            }
            if (!any) {
                SKSE::log::warn("InjectInternal: nothing attached for id='{}'", a_id);
            }
            return any;
        }

        std::string ReadTestPath()
        {
            // Resolved through MO2's VFS as Data\SKSE\Plugins\...
            std::ifstream f("Data\\SKSE\\Plugins\\CostumeExpansionFW_test.txt");
            if (!f) {
                return {};
            }
            std::string line;
            std::getline(f, line);
            // trim whitespace/CR
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            return line;
        }

        // The player's body sex (kMale/kFemale), used to pick which ARMA model to
        // inject. kNone (no actor base) falls back to female (v1's assumption).
        RE::SEX PlayerSex()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* base = player ? player->GetActorBase() : nullptr;
            const RE::SEX sex = base ? base->GetSex() : RE::SEXES::kFemale;
            return (sex == RE::SEXES::kMale) ? RE::SEXES::kMale : RE::SEXES::kFemale;
        }

        // The body sex to inject a_id's model for: a forced gender mode (1=male,
        // 2=female) overrides the player's sex; mode 0 follows the player.
        RE::SEX EffectiveSex(const std::string& a_id)
        {
            switch (GenderModeFor(a_id)) {
            case 1:
                return RE::SEXES::kMale;
            case 2:
                return RE::SEXES::kFemale;
            default:
                return PlayerSex();
            }
        }

        // Parse a colon-form id "XXXXXX:Plugin.esp" into local FormID + plugin.
        // False if it isn't a colon-form (e.g. the raw-NIF "test" id).
        bool ParseColonId(const std::string& a_id, std::uint32_t& a_localID, std::string& a_plugin)
        {
            const auto colon = a_id.find(':');
            if (colon == std::string::npos) {
                return false;
            }
            try {
                a_localID = static_cast<std::uint32_t>(std::stoul(a_id.substr(0, colon), nullptr, 16));
            } catch (...) {
                return false;
            }
            a_plugin = a_id.substr(colon + 1);
            return true;
        }

        // Resolve an ARMA (or ARMO -> its race-matched ARMA) to the 3P + 1P models
        // (NIF path + alternate-texture swap) for the requested body sex. If that
        // sex has no model, falls back to the other sex so a single-sex-authored
        // accessory still shows (e.g. a female-only nail mesh on a male PC). False
        // if neither sex has a 3P model / the form isn't ARMA/ARMO.
        bool ResolveArmaModels(std::uint32_t a_localID, const std::string& a_plugin, RE::SEX a_sex,
            ModelRef& a_out3p, ModelRef& a_out1p)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return false;
            }
            RE::TESObjectARMA* arma = dh->LookupForm<RE::TESObjectARMA>(a_localID, a_plugin);
            if (!arma) {
                if (auto* armo = dh->LookupForm<RE::TESObjectARMO>(a_localID, a_plugin)) {
                    arma = PickAddonForPlayer(armo);
                }
            }
            if (!arma) {
                SKSE::log::error("ResolveArma: {:X}:{} is not ARMA/ARMO", a_localID, a_plugin);
                return false;
            }
            const auto sexName = [](RE::SEX s) { return s == RE::SEXES::kMale ? "male" : "female"; };

            RE::SEX sex = a_sex;
            RE::TESModelTextureSwap* m3 = &arma->bipedModels[sex];
            RE::TESModelTextureSwap* m1 = &arma->bipedModel1stPersons[sex];
            const char* nif3p = m3->model.c_str();
            if (!nif3p || !*nif3p) {
                // Requested sex has no 3P model - fall back to the other sex.
                const RE::SEX other = (sex == RE::SEXES::kMale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
                RE::TESModelTextureSwap* o3 = &arma->bipedModels[other];
                const char* on = o3->model.c_str();
                if (on && *on) {
                    SKSE::log::warn("ResolveArma: {:X}:{} has no {} 3P model, using {} model",
                        a_localID, a_plugin, sexName(sex), sexName(other));
                    sex = other;
                    m3 = o3;
                    m1 = &arma->bipedModel1stPersons[other];
                    nif3p = m3->model.c_str();
                } else {
                    SKSE::log::error("ResolveArma: {:X}:{} has no 3P model for either sex",
                        a_localID, a_plugin);
                    return false;
                }
            }
            a_out3p = ModelRef{ nif3p, m3 };
            const char* nif1p = m1->model.c_str();
            a_out1p = (nif1p && *nif1p) ? ModelRef{ nif1p, m1 } : a_out3p;
            return true;
        }

        // Resolve a colon-form id "XXXXXX:Plugin.esp" to its full runtime FormID
        // (0 on failure). Used for the box token's worn-state check.
        RE::FormID ResolveFormID(const std::string& a_colonId)
        {
            const auto colon = a_colonId.find(':');
            if (colon == std::string::npos) {
                return 0;
            }
            std::uint32_t localID = 0;
            try {
                localID = static_cast<std::uint32_t>(std::stoul(a_colonId.substr(0, colon), nullptr, 16));
            } catch (...) {
                return 0;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return 0;
            }
            return dh->LookupFormID(localID, a_colonId.substr(colon + 1));
        }
    }

    bool InjectSkinned(const std::string& a_nifPath, const std::string& a_id)
    {
        const ModelRef m{ a_nifPath, nullptr };
        Register(a_id, m, m);
        return InjectInternal(a_id, m, m);
    }

    void RunAfterDelayMs(int a_ms, std::function<void()> a_fn)
    {
        RunAfterDelay(std::chrono::steady_clock::now() + std::chrono::milliseconds(a_ms),
            std::move(a_fn));
    }

    // --- bind watchdog ------------------------------------------------------
    // A one-shot delayed pass raced the engine's SECOND facegen head build
    // (observed up to ~13s after load) and lost - the merge generation died
    // AFTER the pass and the stretched mesh stayed until a manual CEF
    // off->on. Timing guesses are the approach this project keeps re-learning
    // to avoid; instead a permanent lightweight tick (a few parent-chain
    // walks every 2.5s) detects dead-generation binds WHENEVER they happen
    // (load sequence, RaceMenu, any mod rebuilding the head) and runs the
    // Reconcile sweep to re-inject into the current generation.
    constexpr int kBindWatchdogMs = 2500;
    std::atomic<bool> g_watchdogTickPending{ false };

    void BindWatchdogTick()
    {
        // Grace after a head rebuild: FSMP's old/new generations overlap for a few
        // seconds, and judging that as "dead" here would pile on re-injects that
        // each can trigger another FSMP rebuild (Engine Fixes arena per rebuild).
        if (std::chrono::steady_clock::now() < g_headRebuildGraceUntil) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        RE::NiAVObject* r3 = player ? player->Get3D(false) : nullptr;
        if (!r3) {
            return;
        }
        for (const auto& it : g_active) {
            if (HasDeadPhysicsBind(it.id, r3)) {
                ++g_persistDiag.watchdogReconciles;
                SKSE::log::info("bind watchdog: '{}' holds a dead merge generation - reconciling", it.id);
                Reconcile();  // the sweep inside detaches + re-injects
                break;
            }
        }
    }

    void StartBindWatchdogOnce()
    {
        static std::atomic<bool> started{ false };
        if (started.exchange(true)) {
            return;
        }
        SKSE::log::info("bind watchdog started ({}ms tick)", kBindWatchdogMs);
        // Cadence lives on a dedicated thread; the CHECK runs on the main
        // thread via one AddTask per tick. The pending flag keeps ticks from
        // piling up in the queue while the pump is busy (loading screens).
        std::thread([]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kBindWatchdogMs));
                if (g_watchdogTickPending.exchange(true)) {
                    continue;  // previous tick not consumed yet
                }
                auto* tasks = SKSE::GetTaskInterface();
                if (!tasks) {
                    g_watchdogTickPending = false;
                    continue;
                }
                tasks->AddTask([]() {
                    g_watchdogTickPending = false;
                    BindWatchdogTick();
                });
            }
        }).detach();
    }

    void Reconcile()
    {
        StartBindWatchdogOnce();
        ++g_persistDiag.reconcileCalls;
        if (g_active.empty()) {
            return;
        }
        // Every externally-triggered pass re-arms the rebind-retry budget; the
        // retry passes themselves must not, or persistently-static contents
        // (custom bones with no carrier) would retry forever.
        if (!g_inRebindRetry) {
            g_rebindRetryBudget = kRebindRetryBudget;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        const bool cefOn = CefEnabled();  // master off (Main page) -> hide everything
        SKSE::log::info("Reconcile: {} active item(s) (cef enabled={})", g_active.size(), cefOn);
        for (auto& it : g_active) {
            // master off -> hide; else persist (tokenForm 0) always shows, a box
            // item shows only while its token is worn.
            bool show = false;
            if (cefOn) {
                // persist (tokenForm 0) always shows; a box item shows only while its
                // token is worn.
                show = (it.tokenForm == 0);
                if (!show && player) {
                    show = (player->GetWornArmor(it.tokenForm) != nullptr);
                }
            }
            // §8.10 hide-when-worn: hide this content while any of its configured
            // vanilla slots is occupied by NON-CEF real equipment (boots over foot
            // nails, helmet over a wig, ...). Auto-reshows when the slot frees.
            if (show && player) {
                for (const int slot : HideSlotsFor(it.id)) {
                    if (slot < 30 || slot > 61) {
                        continue;
                    }
                    auto* worn = player->GetWornArmor(
                        static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (slot - 30)));
                    if (worn && !IsBoxToken(worn->GetFormID())) {
                        show = false;  // a real (non-token) item holds the slot
                        break;
                    }
                }
            }
            // Sex-aware models: if the player's body sex changed since this item
            // was resolved (e.g. ShowRaceMenu), re-resolve the ARMA models for the
            // current sex before injecting. The 3D rebuild that follows a sex change
            // already dropped the old nodes, so this re-injects the right mesh.
            if (show && player) {
                const RE::SEX sex = EffectiveSex(it.id);
                if (it.resolvedSex != sex) {
                    std::uint32_t lid = 0;
                    std::string plg;
                    ModelRef m3p, m1p;
                    if (ParseColonId(it.id, lid, plg) &&
                        ResolveArmaModels(lid, plg, sex, m3p, m1p)) {
                        it.m3p = m3p;
                        it.m1p = m1p;
                    }
                    it.resolvedSex = sex;  // mark resolved (raw-NIF items: nothing to redo)
                }
            }
            SKSE::log::debug("  item '{}' tokenForm={:08X} show={}", it.id, it.tokenForm, show);
            if (show) {
                // Dead-bind sweep: if this item's injected mesh holds FSMP bones of a
                // dead merge generation (armor unequipped / head rebuilt), detach it
                // first so the injection below rebinds to the CURRENT generation.
                if (player) {
                    if (auto* r3 = player->Get3D(false); r3 && HasDeadPhysicsBind(it.id, r3)) {
                        ++g_persistDiag.deadBindReinjects;
                        SKSE::log::info("  '{}': bound FSMP bones are DETACHED (dead merge "
                                        "generation) - re-injecting", it.id);
                        DetachNodes(it.id);
                    }
                }
                InjectInternal(it.id, it.m3p, it.m1p);
            } else {
                DetachNodes(it.id);
            }
        }
    }

    std::uint32_t ResolveFormId(const std::string& a_colonId)
    {
        return ResolveFormID(a_colonId);
    }

    bool IsTrackedToken(RE::FormID a_form)
    {
        if (a_form == 0) {
            return false;
        }
        for (const auto& it : g_active) {
            if (it.tokenForm == a_form) {
                return true;
            }
        }
        return false;
    }

    RE::TESObjectARMA* PickAddonForPlayer(RE::TESObjectARMO* a_armo)
    {
        if (!a_armo || a_armo->armorAddons.empty()) {
            return nullptr;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* race = player ? player->GetRace() : nullptr;
        if (race) {
            for (auto* aa : a_armo->armorAddons) {
                if (aa && aa->race == race) {
                    return aa;
                }
            }
            for (auto* aa : a_armo->armorAddons) {
                if (!aa) {
                    continue;
                }
                for (auto* extra : aa->additionalRaces) {
                    if (extra == race) {
                        return aa;
                    }
                }
            }
        }
        return a_armo->armorAddons.front();
    }

    bool CanResolveContent(const std::string& a_contentId)
    {
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(a_contentId, localID, plugin)) {
            return false;
        }
        ModelRef m3p, m1p;
        return ResolveArmaModels(localID, plugin, EffectiveSex(a_contentId), m3p, m1p);
    }

    bool RegisterBoxById(const std::string& a_contentId, const std::string& a_tokenId)
    {
        const auto colon = a_contentId.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::uint32_t localID = 0;
        try {
            localID = static_cast<std::uint32_t>(std::stoul(a_contentId.substr(0, colon), nullptr, 16));
        } catch (...) {
            return false;
        }
        const RE::SEX sex = EffectiveSex(a_contentId);
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(localID, a_contentId.substr(colon + 1), sex, m3p, m1p)) {
            return false;
        }
        const RE::FormID tokenForm = ResolveFormID(a_tokenId);
        Register(a_contentId, m3p, m1p, a_tokenId, tokenForm, sex);
        return true;
    }

    bool DefineBox(const std::string& a_contentId, const std::string& a_tokenId)
    {
        if (!RegisterBoxById(a_contentId, a_tokenId)) {
            SKSE::log::error("DefineBox: failed content='{}' token='{}'", a_contentId, a_tokenId);
            return false;
        }
        SKSE::log::info("DefineBox content='{}' token='{}'", a_contentId, a_tokenId);
        Reconcile();
        return true;
    }

    void DetachAll()
    {
        // Copy ids first - DetachSkinned mutates g_active via Unregister.
        std::vector<std::string> ids;
        ids.reserve(g_active.size());
        for (const auto& it : g_active) {
            ids.push_back(it.id);
        }
        for (const auto& id : ids) {
            DetachSkinned(id);
        }
        // Belt-and-suspenders: also nuke any orphaned/duplicate holders the
        // registry no longer knows about.
        DetachAllInjected();
    }

    int DetachAllInjected()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0;
        }
        int total = 0;
        auto sweep = [&](RE::NiAVObject* a_root, const char* a_tag) {
            if (!a_root) {
                return;
            }
            std::vector<RE::NiAVObject*> found;
            CollectInjected(a_root, found);
            for (auto* node : found) {
                if (auto* parent = node->parent) {
                    parent->DetachChild(node);
                    ++total;
                }
            }
            SKSE::log::info("DetachAllInjected[{}]: removed {} CostumeFW_* node(s)", a_tag, found.size());
        };
        sweep(player->Get3D(false), "3p");
        sweep(player->Get3D(true), "1p");
        return total;
    }

    void ListActive()
    {
        SKSE::log::info("active: {} item(s)", g_active.size());
        if (auto* c = RE::ConsoleLog::GetSingleton()) {
            c->Print("[CEF] active items:");
        }
        for (const auto& it : g_active) {
            SKSE::log::info("  {}", it.id);
            if (auto* c = RE::ConsoleLog::GetSingleton()) {
                c->Print(it.id.c_str());
            }
        }
    }

    namespace
    {
        bool HasHeadPart(RE::TESNPC* a_base, RE::BGSHeadPart* a_part)
        {
            if (!a_base->headParts) {
                return false;
            }
            for (std::int8_t i = 0; i < a_base->numHeadParts; ++i) {
                if (a_base->headParts[i] == a_part) {
                    return true;
                }
            }
            return false;
        }

        // Remove ONE occurrence of a_part from the base's headParts by shifting
        // the array in place (the engine offers no removal counterpart to
        // ChangeHeadPart; the slack slot stays allocated, which is harmless -
        // the next ChangeHeadPart add reallocates anyway).
        bool RemoveHeadPartDirect(RE::TESNPC* a_base, RE::BGSHeadPart* a_part)
        {
            if (!a_base->headParts) {
                return false;
            }
            const std::int8_t n = a_base->numHeadParts;
            std::int8_t idx = -1;
            for (std::int8_t i = 0; i < n; ++i) {
                if (a_base->headParts[i] == a_part) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                return false;
            }
            for (std::int8_t i = idx; i + 1 < n; ++i) {
                a_base->headParts[i] = a_base->headParts[i + 1];
            }
            a_base->headParts[n - 1] = nullptr;
            a_base->numHeadParts = static_cast<std::int8_t>(n - 1);
            return true;
        }
    }

    bool ReconcilePersistHeadParts(const std::vector<RE::BGSHeadPart*>& a_desired,
        const std::vector<RE::BGSHeadPart*>& a_pool)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = player ? player->GetActorBase() : nullptr;
        if (!base) {
            return false;
        }
        bool changed = false;
        for (auto* part : a_pool) {
            if (!part) {
                continue;
            }
            const bool want =
                std::find(a_desired.begin(), a_desired.end(), part) != a_desired.end();
            if (want) {
                if (!HasHeadPart(base, part)) {
                    base->ChangeHeadPart(part);
                    SKSE::log::info("persist head: + '{}' ({:08X})",
                        part->GetFormEditorID(), part->GetFormID());
                    changed = true;
                }
            } else {
                // Loop: duplicates could exist from older sessions / PoC leftovers.
                while (RemoveHeadPartDirect(base, part)) {
                    SKSE::log::info("persist head: - '{}' ({:08X})",
                        part->GetFormEditorID(), part->GetFormID());
                    changed = true;
                }
            }
        }
        if (changed) {
            // ChangeHeadPart flags this itself; the direct removals need it so
            // the save serializes the edited array.
            base->AddChange(RE::TESNPC::ChangeFlags::kFace);
        }
        return changed;
    }

    bool PlayerHasHeadPart(RE::BGSHeadPart* a_part)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = player ? player->GetActorBase() : nullptr;
        return base && a_part && HasHeadPart(base, a_part);
    }

    bool SweepLegacyCfwHeadParts(const std::vector<RE::BGSHeadPart*>& a_currentPool)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = player ? player->GetActorBase() : nullptr;
        if (!base || !base->headParts) {
            return false;
        }
        // Collect first (removal shifts the array). HDPT retains its editorID at
        // runtime (facegen addresses parts by editorID), so prefix matching works.
        std::vector<RE::BGSHeadPart*> legacy;
        for (std::int8_t i = 0; i < base->numHeadParts; ++i) {
            auto* part = base->headParts[i];
            if (!part) {
                continue;
            }
            const char* ed = part->GetFormEditorID();
            if (!ed || !std::string_view(ed).starts_with("CFW_")) {
                continue;
            }
            if (std::find(a_currentPool.begin(), a_currentPool.end(), part) !=
                a_currentPool.end()) {
                continue;  // merged-plugin pool member - the reconcile owns it
            }
            if (std::find(legacy.begin(), legacy.end(), part) == legacy.end()) {
                legacy.push_back(part);
            }
        }
        bool changed = false;
        for (auto* part : legacy) {
            while (RemoveHeadPartDirect(base, part)) {
                SKSE::log::info("persist head: - legacy '{}' ({:08X}) [pre-merge sweep]",
                    part->GetFormEditorID(), part->GetFormID());
                changed = true;
            }
        }
        if (changed) {
            base->AddChange(RE::TESNPC::ChangeFlags::kFace);
        }
        return changed;
    }

    void RebuildPlayerHead()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D(false)) {
            return;  // no 3D yet - the engine builds the head with the current set
        }
        ++g_persistDiag.headRebuildExecuted;
        SKSE::log::info("persist head: DoReset3D (facegen rebuild)");
        player->DoReset3D(false);
        // Grace window: FSMP keeps the retiring head generation for a few seconds
        // (§9-19); suppress the dead-bind watchdog so it doesn't judge the overlap
        // as dead and pile on re-injects (each can trigger another FSMP rebuild).
        g_headRebuildGraceUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        // Load3D hook does not fire on DoReset3D (C §9-11(ii)): re-inject
        // explicitly once the rebuild settles; the watchdog converges the rest.
        RunAfterDelayMs(1500, [] { Reconcile(); });
    }

    void RequestPersistHeadRebuild(const char* a_reason)
    {
        // Debounce: coalesce a burst of ApplyPersistCarrier-driven rebuild
        // requests into ONE DoReset3D ~500ms after the LAST request, so FSMP
        // rebuilds the wig physics once (not per settings-write / sync / pass).
        ++g_persistDiag.headRebuildRequested;
        const auto rev = ++g_headRebuildRev;
        if (g_headRebuildQueued.exchange(true)) {
            SKSE::log::debug("persist head: rebuild coalesced ({})", a_reason ? a_reason : "");
            return;
        }
        RunAfterDelayMs(500, [rev]() {
            g_headRebuildQueued = false;
            if (rev < g_headRebuildRev.load()) {
                RequestPersistHeadRebuild("coalesced newer revision");
                return;  // a newer request superseded this one
            }
            RebuildPlayerHead();
        });
    }

    std::string PersistDiagString()
    {
        const auto& d = g_persistDiag;
        return "headRebuild(req/exec)=" + std::to_string(d.headRebuildRequested) + "/" +
               std::to_string(d.headRebuildExecuted) +
               " reconcile=" + std::to_string(d.reconcileCalls) +
               " watchdog=" + std::to_string(d.watchdogReconciles) +
               " deadbind=" + std::to_string(d.deadBindReinjects) +
               " rebindRetry=" + std::to_string(d.rebindRetries) +
               " morphApply=" + std::to_string(d.bodyMorphApplies);
    }

    bool ChangeHeadPartPoC(const std::string& a_id)
    {
        // FSMP approach-C active PoC (stage 1): drive a head-part change from CEF
        // code (NOT RaceMenu UI) and force a facegen head rebuild, to test whether
        // FSMP's facegen path (2) enumerates a CODE-changed head part - i.e. does
        // skee's GetBaseOverlays() (which FSMP iterates on AE when HasOverlays())
        // include our vanilla TESNPC::ChangeHeadPart? Pass a known SMP-hair HDPT
        // FormID; then run `cef headdiag` to see if its "_Head_" bones appear.
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!player || !dh) {
            return false;
        }
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(a_id, localID, plugin)) {
            SKSE::log::error("hair PoC: bad id '{}' (want XXXXXX:Plugin.esp)", a_id);
            return false;
        }
        auto* hdpt = dh->LookupForm<RE::BGSHeadPart>(localID, plugin);
        if (!hdpt) {
            SKSE::log::error("hair PoC: {:X}:{} is not a HeadPart (HDPT)", localID, plugin);
            return false;
        }
        auto* base = player->GetActorBase();
        if (!base) {
            SKSE::log::error("hair PoC: player has no actor base");
            return false;
        }
        SKSE::log::info("hair PoC: ChangeHeadPart -> editorID='{}' type={} model='{}'",
            hdpt->GetFormEditorID(), static_cast<int>(hdpt->type.get()),
            hdpt->model.c_str());
        base->ChangeHeadPart(hdpt);
        player->DoReset3D(false);  // rebuilds actor 3D incl. facegen head -> fires (2)
        SKSE::log::info("hair PoC: DoReset3D issued - now run 'cef headdiag'");
        return true;
    }

    void HeadDiag()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        auto* console = RE::ConsoleLog::GetSingleton();
        const auto say = [&](const std::string& s) {
            SKSE::log::info("{}", s);
            if (console) {
                console->Print(s.c_str());
            }
        };

        static constexpr std::string_view kMarker = "hdtSSEPhysics_AutoRename_";

        // Walk one skeleton root: enumerate every FSMP-renamed physics bone,
        // grouped by its "<prefix>_<8hex>" merge id. Full per-bone list -> log;
        // summary -> console. Returns {armorBones, headBones}.
        const auto diagRoot = [&](RE::NiAVObject* a_root, const char* a_tag) {
            if (!a_root) {
                say(std::string("[CEF] headdiag ") + a_tag + ": no 3D");
                return std::pair<std::uint32_t, std::uint32_t>{ 0, 0 };
            }
            std::map<std::string, std::uint32_t> groups;  // "<prefix>_<8hex>" -> bone count
            std::uint32_t armorTotal = 0, headTotal = 0;
            std::vector<RE::NiAVObject*> stack{ a_root };
            while (!stack.empty()) {
                auto* obj = stack.back();
                stack.pop_back();
                if (!obj) {
                    continue;
                }
                const std::string_view nm{ obj->name.c_str() };
                if (nm.starts_with(kMarker)) {
                    const auto sp = nm.find(' ');
                    const std::string group{ sp == std::string_view::npos ? nm : nm.substr(0, sp) };
                    ++groups[group];
                    if (group.find("_Head_") != std::string::npos) {
                        ++headTotal;
                    } else {
                        ++armorTotal;
                    }
                    const char* parent = obj->parent ? obj->parent->name.c_str() : "<null>";
                    // World position: a merged-but-sane bone sits near its anchor
                    // (head ~ pelvis height); one at the origin / thousands of units
                    // away is the "stretched everywhere" failure signature.
                    const auto& wp = obj->world.translate;
                    SKSE::log::info("  headdiag[{}] {} (parent '{}') world=({:.1f},{:.1f},{:.1f})",
                        a_tag, nm, parent, wp.x, wp.y, wp.z);
                }
                if (auto* node = obj->AsNode()) {
                    for (auto& child : node->GetChildren()) {
                        stack.push_back(child.get());
                    }
                }
            }
            say("[CEF] headdiag " + std::string(a_tag) + ": " + std::to_string(armorTotal) +
                " Armor + " + std::to_string(headTotal) + " Head physics bone(s), " +
                std::to_string(groups.size()) + " merge group(s)");
            for (const auto& [g, cnt] : groups) {
                say("  " + g + " : " + std::to_string(cnt) + " bone(s)");
            }
            return std::pair<std::uint32_t, std::uint32_t>{ armorTotal, headTotal };
        };

        const auto [a3, h3] = diagRoot(player->Get3D(false), "3p");
        const auto [a1, h1] = diagRoot(player->Get3D(true), "1p");
        if (h3 == 0 && h1 == 0) {
            say("[CEF] headdiag: NO _Head_ bones found - facegen head path (2) not "
                "firing here (apply an SMP hair/head part and retry)");
        }
        // Live anchor reference for reading the world= values above.
        if (auto* third = player->Get3D(false)) {
            if (auto* headObj = third->GetObjectByName("NPC Head [Head]")) {
                const auto& hp = headObj->world.translate;
                SKSE::log::info("  headdiag ref: live 'NPC Head [Head]' world=({:.1f},{:.1f},{:.1f})",
                    hp.x, hp.y, hp.z);
            }
        }
    }

    bool InjectArma(std::uint32_t a_localID, const std::string& a_plugin, const std::string& a_id)
    {
        const RE::SEX sex = EffectiveSex(a_id);
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(a_localID, a_plugin, sex, m3p, m1p)) {
            return false;
        }
        SKSE::log::info("InjectArma {:X}:{} 3p='{}' 1p='{}'",
            a_localID, a_plugin, m3p.nifPath, m1p.nifPath);
        Register(a_id, m3p, m1p, {}, 0, sex);
        return InjectInternal(a_id, m3p, m1p);
    }

    bool RegisterArmaById(const std::string& a_id)
    {
        // Parse the colon-form id "XXXXXX:Plugin.esp", resolve + register WITHOUT
        // injecting (used by the co-save load callback; ReattachAll injects later).
        const auto colon = a_id.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::uint32_t localID = 0;
        try {
            localID = static_cast<std::uint32_t>(std::stoul(a_id.substr(0, colon), nullptr, 16));
        } catch (...) {
            return false;
        }
        const std::string plugin = a_id.substr(colon + 1);
        const RE::SEX sex = PlayerSex();
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(localID, plugin, sex, m3p, m1p)) {
            return false;
        }
        Register(a_id, m3p, m1p, {}, 0, sex);
        return true;
    }

    bool InjectArmaById(const std::string& a_id)
    {
        // Parse the colon-form id "XXXXXX:Plugin.esp", resolve + register + inject
        // as a persist item (no box token). The Papyrus RegisterPersist() path.
        const auto colon = a_id.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::uint32_t localID = 0;
        try {
            localID = static_cast<std::uint32_t>(std::stoul(a_id.substr(0, colon), nullptr, 16));
        } catch (...) {
            return false;
        }
        return InjectArma(localID, a_id.substr(colon + 1), a_id);
    }

    std::vector<ActiveItemInfo> ActiveSnapshot()
    {
        std::vector<ActiveItemInfo> v;
        v.reserve(g_active.size());
        for (const auto& it : g_active) {
            v.push_back({ it.id, it.tokenId });
        }
        return v;
    }

    void ClearRegistry()
    {
        g_active.clear();
    }

    void DetachSkinned(const std::string& a_id)
    {
        Unregister(a_id);
        DetachNodes(a_id);
        SKSE::log::debug("  detached {}", a_id);
    }

    void HideInjectedNodes(const std::string& a_id)
    {
        DetachNodes(a_id);  // registry untouched: Reconcile re-injects
    }

    void RefreshGender(const std::string& a_id)
    {
        for (auto& it : g_active) {
            if (it.id == a_id) {
                DetachNodes(a_id);                  // drop old-sex node, keep registered
                it.resolvedSex = RE::SEXES::kNone;  // force Reconcile to re-resolve
                break;
            }
        }
        Reconcile();  // re-resolves on the sex mismatch and re-attaches
    }

    void InjectTestFromFile()
    {
        const std::string line = ReadTestPath();
        if (line.empty()) {
            SKSE::log::error("test inject: empty Data\\SKSE\\Plugins\\CostumeExpansionFW_test.txt");
            if (auto* console = RE::ConsoleLog::GetSingleton()) {
                console->Print("CostumeFW: put 'XXXXXX:Plugin.esp' (ARMA/ARMO FormID) in the test txt");
            }
            return;
        }

        // Colon-form FormID "XXXXXX:Plugin.esp" -> resolve ARMA (NIF + variant
        // textures). Anything else is treated as a raw NIF path (no variant).
        const auto colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            const std::string left = line.substr(0, colon);
            const std::string plugin = line.substr(colon + 1);
            const bool hex = left.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            if (hex && plugin.size() > 4) {
                const auto localID = static_cast<std::uint32_t>(std::stoul(left, nullptr, 16));
                const bool ok = InjectArma(localID, plugin, "test");
                if (auto* console = RE::ConsoleLog::GetSingleton()) {
                    console->Print(ok ? "CostumeFW: injected (ARMA)" : "CostumeFW: inject FAILED (see log)");
                }
                return;
            }
        }

        const bool ok = InjectSkinned(line, "test");  // fallback: raw NIF path
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print(ok ? "CostumeFW: injected (NIF)" : "CostumeFW: inject FAILED (see log)");
        }
    }

    void DetachTest()
    {
        DetachSkinned("test");
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("CostumeFW: detached test NIF");
        }
    }
}
