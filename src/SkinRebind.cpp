#include "SkinRebind.h"
#include "BodyMorph.h"
#include "BoxStore.h"
#include "PublishStore.h"
#include "StoreLock.h"
#include "Config.h"  // PersistHeadRebuildEnabled (F2 diagnostic lever)
#include "Diag.h"    // two-tier logging + thread-contract guard
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
#include "RE/A/ActorValues.h"
#include "RE/B/BSFadeNode.h"
#include "RE/B/BipedAnim.h"
#include "RE/P/ProcessLists.h"
#include "RE/S/ShaderReferenceEffect.h"
#include "RE/T/TESEffectShader.h"
#include "RE/B/BSDismemberSkinInstance.h"
#include "RE/M/Misc.h"  // RE::DebugNotification (quarantine parking notice)
#include "RE/N/NiNode.h"
#include "RE/N/NiSkinData.h"
#include "RE/N/NiSkinInstance.h"
#include "RE/P/PlayerCharacter.h"

#include <cstdio>  // std::snprintf (CanonicalizeColonId)
#include <unordered_map>
#include <unordered_set>
#include "RE/S/Sexes.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESModelTextureSwap.h"
#include "RE/T/TESNPC.h"
#include "RE/T/TESObjectARMA.h"
#include "RE/T/TESObjectARMO.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>  // the one delayed-call timer thread
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Windows.h>  // GetModuleHandleA (Bone Limit Extender presence check)

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

        // --- reference-shadow visual sync (invisibility & effect shaders) -------
        // The engine's visual effects (invisibility, cloaks, any EFSH) sweep the
        // geometry that exists WHEN THE EFFECT STARTS. CEF holders attached after
        // that moment never receive the state, so a costume shown mid-invisibility
        // floats visibly on an invisible body (investigation 2026-08-05, §1/§6).
        //
        // CEF does not try to detect, enumerate or reproduce those effects. It
        // reads the state the engine ALREADY wrote onto a normal equipped biped
        // part of the same actor - the two channels the diagnostics found in use,
        // the kTempRefraction shader flag and the BSEffectShaderData pointer - and
        // shadows it onto the geometry CEF injected. Whatever the body shows, the
        // costume shows; when the body's state clears, the costume's clears with
        // it. No effect-type analysis, no end-of-effect bookkeeping, no alpha or
        // material writes (§11).
        struct VisualSyncRef
        {
            RE::NiPointer<RE::BSGeometry> geom;
            // The shape's OWN kTempRefraction at injection time, OR-ed in below so
            // an effect can never take away a bit the shape already had.
            //
            // Captured as false whenever an effect was already live on the actor
            // (see ReferenceEffectActive): a bit-2 seen at that moment is far more
            // likely the engine's than the author's, and mistaking it for authored
            // would LATCH the shape refracted forever - the one way this design
            // could leave a permanent artifact. Erring the other way is transient:
            // the next injection taken while the actor is clear re-reads it.
            // (Note that the flag a NIF author actually sets for refraction is
            // kRefraction, bit 15, which this code never touches at all - as with
            // alpha and material, §11. So this is a guard on the runtime bit only.)
            bool authoredTmpRefr{ false };
            // True once CEF has copied an effectData onto this shape - and the ONLY
            // condition under which CEF will later clear one. An effectData CEF did
            // not write belongs to whatever effect reached the shape on its own
            // (in-game 2026-08-05: a mod's persistent EFSH reached two costume
            // 'Hands' shapes CEF had injected); nulling that would be CEF fighting
            // another mod's live effect.
            bool cefEffectData{ false };
        };

        // The engine-side state being shadowed. effectData is held as void* on
        // purpose: it is an IDENTITY for change detection and is never
        // dereferenced from here (the live smart pointer is re-read from the
        // reference property in the frame it is applied).
        struct VisualRefState
        {
            bool        valid{ false };
            bool        tmpRefr{ false };
            const void* effectData{ nullptr };
            bool operator==(const VisualRefState&) const = default;
        };

        // Per-skeleton reference: one normal equipped biped part, reached through
        // the biped RECORDS - never a skeleton search (§10). The biped index +
        // partClone pointer are kept so staleness is an O(1) compare instead of a
        // re-walk: the engine swaps partClone whenever that slot is re-equipped or
        // the 3D is rebuilt, and that is exactly when the reference must be
        // re-taken.
        struct VisualRefSlot
        {
            RE::NiPointer<RE::BSGeometry> geom;
            RE::NiPointer<RE::NiAVObject> part;
            std::uint32_t  bipedIndex{ RE::BIPED_OBJECTS::kTotal };
            VisualRefState last;
            std::uint8_t   missTicks{ 0 };  // back-off after a failed acquisition
        };

        struct ActiveItem
        {
            std::string id;
            ModelRef m3p;               // 3rd-person worn model (bipedModels[sex])
            ModelRef m1p;               // 1st-person model (bipedModel1stPersons[sex])
            std::string tokenId;        // box token colon-form id; empty = persist class
            RE::FormID tokenForm{ 0 };  // resolved token FormID (0 if persist)
            RE::SEX resolvedSex{ RE::SEXES::kFemale };  // sex m3p/m1p were resolved for
            // Last 3p injection's bind outcome, for the bone-budget readout. A
            // content whose bones all land in staticBones gets no SMP sway - and
            // when that is the WHOLE registry, CEF lost the actor's bone budget to
            // another SMP mod (measured 2026-07-28: with SOFTBODY enabled, none of
            // CEF's carrier bones were merged at all).
            std::uint32_t fsmpBones{ 0 };    // bound to an FSMP physics node
            std::uint32_t staticBones{ 0 };  // fell back to the static ancestor remap
            std::uint32_t maxShapeBones{ 0 };  // worst single shape, vs the 80 ceiling
            // Attachment record per skeleton: the holder CEF created and the node
            // it hung it on. Detach goes through these - CEF never searches the
            // scene graph for its own nodes (see DetachRecorded).
            RE::NiPointer<RE::NiNode> holder3p, parent3p;
            RE::NiPointer<RE::NiNode> holder1p, parent1p;
            // Frozen per-content settings (published snapshots). Null = follow
            // the live global content settings.
            std::shared_ptr<const ContentSettings> settings;
            // The geometry this item put on each skeleton, for the visual shadow.
            // Same lifetime as the holder records above: every path that detaches
            // through DetachRecorded clears the matching list.
            std::vector<VisualSyncRef> visual3p, visual1p;
        };
        constexpr int kRebindRetryBudget = 4;
        struct ActorState
        {
            RE::ActorHandle handle;
            bool isPlayer{ false };
            std::vector<ActiveItem> items;
            std::unordered_map<std::string, std::vector<RE::NiPointer<RE::NiAVObject>>> bonePins;
            // Real-body (substitute body) attachment record for THIS actor. Not a
            // registry item, so it gets its own record pair per skeleton - detach
            // goes through these, never a scene search (see DetachRecorded).
            RE::NiPointer<RE::NiNode> realBodyHolder3p, realBodyParent3p;
            RE::NiPointer<RE::NiNode> realBodyHolder1p, realBodyParent1p;
            std::vector<std::string> rebindRetryIds;
            int rebindRetryBudget{ kRebindRetryBudget };
            bool rebindRetryQueued{ false };
            bool inRebindRetry{ false };
            bool realBodyShown{ false };
            // Visual shadow (see VisualSyncRef): the real body's injected geometry
            // per skeleton, and the engine-side reference each skeleton follows.
            std::vector<VisualSyncRef> visualRealBody3p, visualRealBody1p;
            VisualRefSlot visualRef3p, visualRef1p;
        };
        std::vector<ActorState> g_actors;

        // Lock-free mirror of "any non-player state tracks items" for the
        // Character::Load3D thunk (background loading thread; M11). The queued
        // task re-verifies through FindState on the main thread.
        std::atomic<bool> g_anyNpcBindings{ false };

        void RefreshNpcBindingsGate()
        {
            bool any = false;
            for (const auto& state : g_actors) {
                if (!state.isPlayer && !state.items.empty()) {
                    any = true;
                    break;
                }
            }
            g_anyNpcBindings.store(any, std::memory_order_relaxed);
        }

        ActorState& PlayerState()
        {
            if (g_actors.empty()) {
                g_actors.push_back({ {}, true });
            }
            return g_actors.front();
        }

        RE::Actor* ResolveActor(ActorState& a_state)
        {
            if (a_state.isPlayer) {
                return RE::PlayerCharacter::GetSingleton();
            }
            auto ref = a_state.handle.get();
            return ref ? ref.get()->As<RE::Actor>() : nullptr;
        }

        ActorState* FindState(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return nullptr;
            }
            for (auto& state : g_actors) {
                if (ResolveActor(state) == a_actor) {
                    return &state;
                }
            }
            return nullptr;
        }

        ActorState* FindState(RE::ActorHandle a_handle)
        {
            auto ref = a_handle.get();
            return ref ? FindState(ref.get()->As<RE::Actor>()) : nullptr;
        }

        void Register(ActorState& a_state, const std::string& a_id,
            const ModelRef& a_m3p, const ModelRef& a_m1p,
            const std::string& a_tokenId = {}, RE::FormID a_tokenForm = 0,
            RE::SEX a_sex = RE::SEXES::kFemale)
        {
            for (auto& it : a_state.items) {
                if (it.id == a_id) {
                    it.m3p = a_m3p;
                    it.m1p = a_m1p;
                    it.tokenId = a_tokenId;
                    it.tokenForm = a_tokenForm;
                    it.resolvedSex = a_sex;
                    return;
                }
            }
            a_state.items.push_back({ a_id, a_m3p, a_m1p, a_tokenId, a_tokenForm, a_sex });
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
        // Transient sink: during a bind pass RebindGeometry drops a NiPointer for
        // every bone it binds; the injector stores the collected pins into the
        // owning ActorState::bonePins[id] (per-actor storage, NPC-generalized).
        std::vector<RE::NiPointer<RE::NiAVObject>>* g_boneRefSink = nullptr;

        // Baseline: the children-buffer address each recorded holder is SUPPOSED
        // to have, captured when CEF itself finished attaching (InjectOnRoot).
        // Catches the stomp variants a value-plausibility check cannot: an
        // 8-aligned uint16 run with a zero low word (e.g. 0x0002000000010000)
        // passes PlausibleObjectPtr, but nothing legitimate moves the buffer
        // while the record holds the node - CEF pre-sizes the array and never
        // grows it afterwards. Keyed by node address; erased in DetachRecorded.
        std::unordered_map<const void*, std::uint64_t> g_holderArrayBaseline;

        // Repeat-offender parking: a holder the environment stomps once tends to
        // be stomped again right after the re-inject (same allocator, same
        // writer). Re-arming the CTD forever helps nobody; after
        // kQuarantineParkThreshold strikes the id sits out until the next
        // registry reset (save load), loudly.
        constexpr int kQuarantineParkThreshold = 3;
        std::unordered_map<std::string, int> g_quarantineStrikes;
        std::unordered_set<std::string> g_poisonParked;

        // --- CEF never searches the skeleton for its own nodes -----------------
        // Two crash generations taught this, both in DetachRealBody:
        //
        //  1. The original code did GetObjectByName then read the result's
        //     ->parent and called DetachChild through it. That parent could be a
        //     non-object; the fault was reading its vtable.
        //  2. Replacing that with a hand-rolled children walk moved the crash
        //     rather than removing it - and made it WORSE, reaching New Game.
        //     The walk died on the children of "NPC Root [Root]", reading a slot
        //     that held a pointer to some node's children array (the faulting
        //     value unpacked to NiTArray's four uint16 fields: capacity, freeIdx,
        //     size, growthSize). Proven from SkyrimSE.exe's RTTI afterwards:
        //     that node is a BSFlattenedBoneTree, whose vtable slot +0x150
        //     (GetObjectByName) is SkyrimSE.exe+0xD30380 - a DIFFERENT function
        //     from NiNode's +0xD1D9A0. The engine does not find bones there by
        //     walking children, so what those slots hold is nobody's contract.
        //     A hand walk of that node was never valid; it just happened to be
        //     survivable on some skeletons, which is why it never reproduced
        //     locally.
        //
        // So: no searching at all. CEF records a NiPointer to the holder it
        // created and to the node it hung it on, and detaches through the pair.
        // Neither end can dangle - NiPointer keeps both alive - and neither
        // ->parent nor any children array is ever touched.
        //
        // a_visual is that skeleton's visual-shadow list (VisualSyncRef). It is
        // released HERE, with the holder, so the two can never diverge: every
        // detach path in the plugin goes through this function, and a stale
        // NiPointer to geometry CEF no longer owns is exactly the extra reference
        // the containment work forbids (§10).
        void DetachRecorded(RE::NiPointer<RE::NiNode>& a_parent, RE::NiPointer<RE::NiNode>& a_holder,
            std::vector<VisualSyncRef>* a_visual = nullptr)
        {
            if (a_parent && a_holder) {
                a_parent->DetachChild(a_holder.get());
            }
            if (a_holder) {
                g_holderArrayBaseline.erase(a_holder.get());
            }
            if (a_visual) {
                a_visual->clear();
            }
            a_parent.reset();
            a_holder.reset();
        }

        // Pointer-VALUE plausibility for anything that claims to be an object
        // pointer (a child slot, an array buffer). Value checks only - never
        // dereferences - so it is safe on arbitrary bits. The alignment test is
        // the load-bearing one: every corruption value observed in this hunt
        // (0x1, 0x0001000200020002, 0x0001000500050005 - little-endian uint16
        // runs, bone-index-shaped; BUGREPORT 2026-07-30) is misaligned, while a
        // real NiAVObject* is always 8-aligned. FSMP guards the same phenomenon
        // by reading vtables (castNiNode), but that faults on unmapped junk;
        // a value check cannot.
        bool PlausibleObjectPtr(const void* a_p)
        {
            const auto v = reinterpret_cast<std::uintptr_t>(a_p);
            return v >= 0x10000 && (v & 7) == 0 && v <= 0x00007FFF'FFFFFFFFull;
        }

        // A child SLOT holding an implausible pointer is the same corruption
        // family as a broken _data, caught one dereference earlier. The value
        // itself is writer evidence, so the first few go to the log loudly;
        // after that keep counting quietly (a watchdog revisits every 2.5s and
        // must not flood).
        void NoteBadSlot(const char* a_where, RE::NiNode* a_parent, const void* a_val)
        {
            static std::atomic<std::uint32_t> s_seen{ 0 };
            const auto n = ++s_seen;
            if (n <= 8) {
                SKSE::log::error(
                    "SCENE CORRUPTION: child slot of '{}' holds implausible pointer "
                    "{:#x} ({}, hit #{}) - slot skipped",
                    a_parent->name.c_str(), reinterpret_cast<std::uintptr_t>(a_val),
                    a_where, n);
            } else {
                SKSE::log::debug("bad child slot {:#x} under '{}' ({}, hit #{})",
                    reinterpret_cast<std::uintptr_t>(a_val), a_parent->name.c_str(),
                    a_where, n);
            }
        }

        // Still used by the diagnostics walks (nodediag / headdiag / the dead-bind
        // sweep), which enumerate FSMP's merged bones and have no record to go on.
        // Those are user-initiated or already scoped to our own holder; the hot
        // paths no longer walk anything.
        bool ChildrenWalkable(RE::NiNode* a_node)
        {
            const auto& kids = a_node->GetChildren();
            if (kids.size() > kids.capacity()) {
                return false;  // more elements than storage
            }
            // Emptiness is CAPACITY, not size: NiTArray::end() is _data + _capacity,
            // so a range-for iterates every capacity slot. The old size()==0 early
            // true let "size=0, cap>0, data=0x1" through the guard straight into
            // the iteration it was supposed to prevent (INVESTIGATION 2026-07-30).
            if (kids.capacity() == 0) {
                return true;  // nothing will be iterated
            }
            // A non-empty array must point at a real, aligned allocation. The
            // observed 0x1 fails the floor; the uint16-run values (0x0001000500050005)
            // pass a bare floor check and are caught by the alignment test.
            return PlausibleObjectPtr(kids.begin());
        }

        // --- holder-array health + quarantine (persist-CTD primary corruption) --
        // The observed corruption (children._data == 0x1 with the size fields
        // intact, INVESTIGATION 2026-07-29/30) is detected from the attachment
        // RECORDS: metadata reads only - no dereference, no walking - so the
        // check is safe at any moment, on any state.
        struct HolderArrayState
        {
            std::uint64_t data{ 0 };
            std::uint16_t cap{ 0 }, freeIdx{ 0 }, size{ 0 }, growth{ 0 };
        };

        HolderArrayState ReadHolderArray(RE::NiNode* a_node)
        {
            HolderArrayState s{};
            const auto* raw = reinterpret_cast<const unsigned char*>(&a_node->GetChildren());
            std::memcpy(&s.data, raw + 0x08, sizeof(s.data));
            std::memcpy(&s.cap, raw + 0x10, sizeof(s.cap));
            std::memcpy(&s.freeIdx, raw + 0x12, sizeof(s.freeIdx));
            std::memcpy(&s.size, raw + 0x14, sizeof(s.size));
            std::memcpy(&s.growth, raw + 0x16, sizeof(s.growth));
            return s;
        }

        void RecordHolderBaseline(RE::NiNode* a_holder)
        {
            if (a_holder) {
                g_holderArrayBaseline[a_holder] = ReadHolderArray(a_holder).data;
            }
        }

        bool HolderArrayBroken(const HolderArrayState& s)
        {
            if (s.size > s.cap) {
                return true;
            }
            if (s.cap == 0) {
                return false;  // empty array - nothing iterates, dtor frees nothing
            }
            // Non-empty must point at a real, aligned allocation. The bare
            // < 0x10000 floor caught the observed 0x1 but would wave through the
            // uint16-run family (0x0001000500050005 is far above the floor and
            // still poison); the alignment test in PlausibleObjectPtr rejects
            // every member of that family (BUGREPORT 2026-07-30).
            return !PlausibleObjectPtr(reinterpret_cast<const void*>(s.data));
        }

        // The writer's fingerprint: dump the whole node (NiNode is 0x128 bytes)
        // the moment corruption is detected. Whether +0x110 (the embedded array's
        // vtable) survived and what exactly sits at +0x118 is question-A evidence
        // no crash log has been able to give. Safe: the NiPointer record keeps
        // the node's memory alive.
        void HexDumpNode(RE::NiNode* a_node)
        {
            const auto* raw = reinterpret_cast<const unsigned char*>(a_node);
            for (std::size_t off = 0; off < 0x128; off += 16) {
                std::string line;
                for (std::size_t i = 0; i < 16 && off + i < 0x128; ++i) {
                    line += std::format("{:02X} ", raw[off + i]);
                }
                SKSE::log::error("  +{:03X}: {}", off, line);
            }
        }

        // A broken holder cannot be released as-is: ~NiNode's array destructor
        // walks _data (slot release + deallocate) and dies on 0x1 - "Active OFF
        // on the broken item still crashes" was a residual path of the record
        // redesign. Rewrite the embedded array to a valid EMPTY state first; the
        // children leak on purpose (a few geometries vs a CTD).
        void RepairHolderArray(RE::NiNode* a_node)
        {
            auto* raw = reinterpret_cast<unsigned char*>(&a_node->GetChildren());
            static constexpr unsigned char zeros[8]{};
            std::memcpy(raw + 0x08, zeros, 8);  // _data = nullptr
            std::memcpy(raw + 0x10, zeros, 6);  // capacity / freeIdx / size = 0
        }

        // True (and the record is cleared) when the recorded holder's child array
        // is corrupted: log loudly, dump the node, repair, detach through the
        // record. The item then re-injects like any detached item. This is the
        // containment that turns the primary corruption from a CTD into a log
        // line, whoever the writer turns out to be.
        bool QuarantineIfBroken(const char* a_what,
            RE::NiPointer<RE::NiNode>& a_parent, RE::NiPointer<RE::NiNode>& a_holder,
            std::vector<VisualSyncRef>* a_visual = nullptr)
        {
            if (!a_holder) {
                return false;
            }
            const auto st = ReadHolderArray(a_holder.get());
            const char* how = nullptr;
            if (HolderArrayBroken(st)) {
                how = "implausible children array";
            } else if (const auto itB = g_holderArrayBaseline.find(a_holder.get());
                       itB != g_holderArrayBaseline.end() && itB->second != st.data) {
                // Plausible-looking value, wrong buffer: either an aligned
                // stomp (uint16 run with a zero low word) or something foreign
                // grew OUR array. Neither is a state to keep rendering from.
                how = "children buffer moved from its attach-time baseline";
            }
            if (!how) {
                return false;
            }
            SKSE::log::error(
                "SCENE CORRUPTION on holder '{}' ({}): {} - children size={} cap={} freeIdx={} "
                "data={:#x} - dumping node, then quarantining",
                a_holder->name.c_str(), a_what, how, st.size, st.cap, st.freeIdx, st.data);
            HexDumpNode(a_holder.get());
            RepairHolderArray(a_holder.get());
            DetachRecorded(a_parent, a_holder, a_visual);
            const int strikes = ++g_quarantineStrikes[a_what];
            if (strikes == kQuarantineParkThreshold) {
                g_poisonParked.insert(a_what);
                SKSE::log::error(
                    "'{}' has been quarantined {} times this session - PARKED (no more "
                    "re-injects until the next save load). Something else is repeatedly "
                    "overwriting live scene nodes on this actor", a_what, strikes);
                RE::DebugNotification(
                    "CostumeFW: a costume keeps getting damaged by another mod - "
                    "parked it (see the CEF log)");
            }
            SKSE::log::error(
                "quarantined '{}': array repaired to empty, holder detached (its geometry "
                "leaks by design), the item will re-inject (strike {})", a_what, strikes);
            return true;
        }

        // Registry-wide containment pass over EVERY actor's items (the real-body
        // pair is file-scope state declared further down; its callers add it
        // explicitly).
        int QuarantineSweep()
        {
            int hit = 0;
            for (auto& state : g_actors) {
                for (auto& it : state.items) {
                    hit += QuarantineIfBroken(it.id.c_str(), it.parent3p, it.holder3p,
                               &it.visual3p) ? 1 : 0;
                    hit += QuarantineIfBroken(it.id.c_str(), it.parent1p, it.holder1p,
                               &it.visual1p) ? 1 : 0;
                }
            }
            return hit;
        }

        void DetachNodes(ActorState& a_state, const std::string& a_id)
        {
            for (auto& it : a_state.items) {
                if (it.id != a_id) {
                    continue;
                }
                const bool had3p = static_cast<bool>(it.holder3p);
                const bool had1p = static_cast<bool>(it.holder1p);
                DetachRecorded(it.parent3p, it.holder3p, &it.visual3p);
                DetachRecorded(it.parent1p, it.holder1p, &it.visual1p);
                // Pins go AFTER the detach: skin->bones[] are raw pointers, so
                // releasing the pins first opens a window where a retired FSMP
                // bone is freed while its geometry is still attached and
                // renderable (the 2026-07-04 freed-bone CTD class).
                a_state.bonePins.erase(a_id);
                SKSE::log::debug("  DetachNodes '{}' 3p={} 1p={}", a_id, had3p, had1p);
                return;
            }
            a_state.bonePins.erase(a_id);  // no registry entry: nothing attached to protect
            // No registry entry means no attachment record, which means anything
            // this id has on the player can no longer be reached - it is orphaned
            // until the next 3D rebuild, and a re-injection will sit beside it.
            // Say so loudly: the only way here is detaching in the wrong order
            // (see DetachSkinned), and that bug is invisible otherwise.
            SKSE::log::warn(
                "DetachNodes '{}': no registry entry - nothing to detach through. If this "
                "id was attached, its holder is now orphaned (detach BEFORE unregistering)",
                a_id);
        }

        void Unregister(ActorState& a_state, const std::string& a_id)
        {
            std::erase_if(a_state.items, [&](const ActiveItem& it) { return it.id == a_id; });
        }

        // --- visual shadow: read the body, write the costume --------------------
        // See VisualSyncRef for the why. Everything below touches ONLY geometry
        // CEF itself injected and recorded (never a skeleton walk, never the
        // actor root), and writes only the two effect channels - no alpha, no
        // material (§10, §11).

        RE::BSShaderProperty* ShaderPropOf(RE::BSGeometry* a_geom)
        {
            if (!a_geom) {
                return nullptr;
            }
            auto& rt = a_geom->GetGeometryRuntimeData();
            return ::netimmerse_cast<RE::BSShaderProperty*>(
                rt.properties[RE::BSGeometry::States::kEffect].get());
        }

        bool HasTempRefraction(RE::BSShaderProperty* a_prop)
        {
            return a_prop &&
                   a_prop->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction);
        }

        // First shader-carrying geometry in ONE biped part's own subtree. The
        // subtree is the engine's equip clone, not the actor skeleton, and this
        // runs only when the reference is (re)taken - not per frame.
        RE::BSGeometry* FirstShadedGeometry(RE::NiAVObject* a_part)
        {
            RE::BSGeometry* found = nullptr;
            if (!a_part) {
                return nullptr;
            }
            RE::BSVisit::TraverseScenegraphGeometries(a_part, [&](RE::BSGeometry* a_geom) {
                if (ShaderPropOf(a_geom)) {
                    found = a_geom;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return found;
        }

        // Take a fresh reference for one skeleton. Slot 32 (kBody) is tried first:
        // it is the one biped slot that is filled even on a naked actor (the skin
        // body), and it is never one of CEF's own invisible carriers. The scan of
        // the remaining slots is the fallback for an actor whose body slot is
        // genuinely empty.
        bool AcquireVisualRef(RE::Actor* a_actor, bool a_firstPerson, VisualRefSlot& a_slot)
        {
            a_slot.geom.reset();
            a_slot.part.reset();
            a_slot.bipedIndex = RE::BIPED_OBJECTS::kTotal;
            a_slot.last = {};
            if (!a_actor) {
                return false;
            }
            const auto& biped = a_actor->GetBiped1(a_firstPerson);
            if (!biped) {
                return false;
            }
            const auto take = [&](std::uint32_t a_i) {
                auto& part = biped->objects[a_i].partClone;
                auto* geom = FirstShadedGeometry(part.get());
                if (!geom) {
                    return false;
                }
                a_slot.geom.reset(geom);
                a_slot.part = part;
                a_slot.bipedIndex = a_i;
                return true;
            };
            if (take(RE::BIPED_OBJECTS::kBody)) {
                return true;
            }
            for (std::uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
                if (i != RE::BIPED_OBJECTS::kBody && take(i)) {
                    return true;
                }
            }
            return false;
        }

        // Still the same equipped part in the same slot? The engine replaces
        // partClone on re-equip and on every 3D rebuild, which is exactly when a
        // reference goes stale. O(1) - no walking.
        bool VisualRefStillValid(RE::Actor* a_actor, bool a_firstPerson, const VisualRefSlot& a_slot)
        {
            if (!a_slot.geom || !a_slot.part || a_slot.bipedIndex >= RE::BIPED_OBJECTS::kTotal) {
                return false;
            }
            const auto& biped = a_actor->GetBiped1(a_firstPerson);
            return biped &&
                   biped->objects[a_slot.bipedIndex].partClone.get() == a_slot.part.get();
        }

        void ApplyVisualState(std::vector<VisualSyncRef>& a_list, const VisualRefState& a_state,
            const RE::BSTSmartPointer<RE::BSEffectShaderData>& a_data, int& a_geomCount)
        {
            for (auto& ref : a_list) {
                auto* prop = ShaderPropOf(ref.geom.get());
                if (!prop) {
                    continue;
                }
                // OR with the shape's own flag: an effect can add refraction to a
                // shape, never take away one it already had.
                const bool want = a_state.tmpRefr || ref.authoredTmpRefr;
                if (HasTempRefraction(prop) != want) {
                    prop->SetFlags(
                        RE::BSShaderProperty::EShaderPropertyFlag8::kTempRefraction, want);
                }
                // effectData is mirrored, not managed: CEF copies what the
                // reference carries, and releases ONLY what CEF itself put there
                // (see VisualSyncRef::cefEffectData). AcceptsEffectData is the
                // engine's own gate for property types with nowhere to put it. The
                // smart pointer carries the lifetime; nothing here owns the data.
                if (prop->AcceptsEffectData()) {
                    if (a_data) {
                        if (prop->effectData != a_data) {
                            prop->SetEffectShaderData(a_data);
                        }
                        ref.cefEffectData = true;
                    } else if (ref.cefEffectData) {
                        if (prop->effectData) {
                            prop->SetEffectShaderData(a_data);  // null: release ours
                        }
                        ref.cefEffectData = false;
                    }
                }
                ++a_geomCount;
            }
        }

        // Is a visual effect live on the actor's OWN equipped geometry right now?
        // Asked at injection time so a shape attached mid-effect is not credited
        // with a kTempRefraction that belongs to the effect (VisualSyncRef). Uses
        // the cached reference, so it is O(1) once taken - and refreshing it here
        // is work the sync that follows would do anyway.
        bool ReferenceEffectActive(ActorState& a_state, RE::Actor* a_actor, bool a_firstPerson)
        {
            auto& slot = a_firstPerson ? a_state.visualRef1p : a_state.visualRef3p;
            if (!VisualRefStillValid(a_actor, a_firstPerson, slot) &&
                !AcquireVisualRef(a_actor, a_firstPerson, slot)) {
                return false;  // no reference: treat the actor as clear
            }
            auto* prop = ShaderPropOf(slot.geom.get());
            return prop && (HasTempRefraction(prop) || prop->effectData);
        }

        bool AnyVisualRefs(const ActorState& a_state, bool a_firstPerson)
        {
            for (const auto& it : a_state.items) {
                if (!(a_firstPerson ? it.visual1p : it.visual3p).empty()) {
                    return true;
                }
            }
            return !(a_firstPerson ? a_state.visualRealBody1p : a_state.visualRealBody3p).empty();
        }

        // One skeleton of one actor. a_force writes unconditionally (used right
        // after an injection, where the reference may not have CHANGED but the
        // new geometry has never been written); otherwise the whole frame cost is
        // one pointer compare plus a (bit, pointer) compare, and nothing is
        // written unless the body's state actually moved.
        void SyncSkeletonVisualState(ActorState& a_state, RE::Actor* a_actor,
            bool a_firstPerson, bool a_force)
        {
            auto& slot = a_firstPerson ? a_state.visualRef1p : a_state.visualRef3p;
            if (!AnyVisualRefs(a_state, a_firstPerson)) {
                slot = {};  // nothing of ours on this skeleton - drop the reference
                return;
            }
            if (!VisualRefStillValid(a_actor, a_firstPerson, slot)) {
                // Back off between failed acquisitions (during a 3D rebuild the
                // biped is briefly empty) so a bipedless actor cannot turn this
                // into a per-frame scan.
                if (slot.missTicks > 0 && !a_force) {
                    --slot.missTicks;
                    return;
                }
                if (!AcquireVisualRef(a_actor, a_firstPerson, slot)) {
                    slot.missTicks = 30;
                    return;
                }
                slot.missTicks = 0;
            }
            auto* prop = ShaderPropOf(slot.geom.get());
            if (!prop) {
                slot = {};  // the reference lost its shader property - retake next frame
                return;
            }
            VisualRefState now{};
            now.valid = true;
            now.tmpRefr = HasTempRefraction(prop);
            now.effectData = static_cast<const void*>(prop->effectData.get());
            if (!a_force && now == slot.last) {
                return;
            }
            const RE::BSTSmartPointer<RE::BSEffectShaderData> data = prop->effectData;
            int geoms = 0;
            for (auto& it : a_state.items) {
                ApplyVisualState(a_firstPerson ? it.visual1p : it.visual3p, now, data, geoms);
            }
            ApplyVisualState(
                a_firstPerson ? a_state.visualRealBody1p : a_state.visualRealBody3p,
                now, data, geoms);
            const bool changed = !(now == slot.last);
            slot.last = now;
            // Only on an actual transition - never per frame (§10). A forced
            // post-injection pass that found the body in its resting state has
            // nothing to report.
            if (geoms > 0 && (changed || now.tmpRefr || now.effectData)) {
                SKSE::log::debug(
                    "visual sync {} ({}): tmpRefr={} effectData={} -> {} injected shape(s){}",
                    a_state.isPlayer ? "player" : "npc", a_firstPerson ? "1p" : "3p",
                    now.tmpRefr ? 1 : 0, now.effectData, geoms, a_force ? " [inject]" : "");
            }
        }

        void SyncActorVisualState(ActorState& a_state, bool a_force)
        {
            if (!AnyVisualRefs(a_state, false) && !AnyVisualRefs(a_state, true)) {
                return;  // cheapest possible exit for an actor with nothing shown
            }
            auto* actor = ResolveActor(a_state);
            if (!actor) {
                return;
            }
            SyncSkeletonVisualState(a_state, actor, false, a_force);
            if (a_state.isPlayer) {
                SyncSkeletonVisualState(a_state, actor, true, a_force);
            }
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
                // Walkability guard (2026-07-30): this walk had NONE while the
                // guarded sweeps did - it was the widest-open residual path of
                // the §7 B-2 audit (every unresolved bone, up to twice).
                if (auto* node = obj->AsNode(); node && ChildrenWalkable(node)) {
                    for (auto& child : node->GetChildren()) {
                        auto* c = child.get();
                        if (c && !PlausibleObjectPtr(c)) {
                            NoteBadSlot("FindFsmpRenamedBone", node, c);
                            continue;
                        }
                        stack.push_back(c);
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
        constexpr auto kRebindRetryDelay = std::chrono::milliseconds(1000);

        // --- X-DIAG: "still static after the retries" ---------------------------
        // "the carrier attached but carries none of this content's bones" and
        // "the carrier is still attaching" look IDENTICAL from the rebind - both
        // are a 3p static fallback. The retry budget is what tells them apart:
        // once it is spent the item is still static, so the file the token points
        // at is either not the carrier that was built (a higher-priority mod
        // masking it - the 2026-07-25 X-SMP fault was a 234-byte pristine stub
        // winning over a 1MB built carrier) or was built without this content.
        // Name the file and the bone counts once per episode instead of going
        // quiet, which cost a whole night of triage.
        std::uint32_t g_rebind3pFsmp = 0;   // per-injection, 3p skeleton only
        std::uint32_t g_rebind3pRemap = 0;
        // Highest bone count on any SINGLE skinned shape of this injection. This
        // is the number the vanilla 80-bone ceiling applies to: SSE skins on the
        // GPU and passes the shape's bones in a ~3840-byte DX11 constant buffer,
        // i.e. 80 bones per DRAW - a shape over that crashed the game when it was
        // copied into the buffer, which is precisely what Bone Limit Extender
        // (Nexus 177636) lifts. Per shape, NOT per actor: the FSMP merge totals
        // reported next to it are a different axis entirely.
        std::uint32_t g_maxShapeBones = 0;
        std::unordered_set<std::string> g_staticDiagReported;
        // Contents already warned about the 80-bone ceiling this session. The
        // Diagnostics page has reported this all along, but a user only looks
        // there once something has already gone wrong - and what goes wrong here
        // is a crash. Say it on screen, once per item, when it is injected.
        std::unordered_set<std::string> g_boneLimitWarned;

        std::atomic<int> g_rebindRetryBudget{ 0 };
        std::atomic<bool> g_rebindRetryQueued{ false };
        bool g_inRebindRetry = false;          // main-thread only (tasks + Reconcile)
        bool g_injectStatic3p = false;         // set by RebindGeometry, read by InjectFor
        std::vector<std::string> g_rebindRetryIds;  // items to detach+re-inject on retry

        // Per-content carrier bone prefix of the item currently being injected
        // (v1.2.1 multi-content namespace isolation, nifcarrier IsolateContent):
        // a multi-content carrier's copy of this content's custom bones is
        // "C<fnv1a32>_<bone>", so the FSMP-renamed lookup must try the prefixed
        // name first, then the plain one (single-content carriers stay plain).
        // Set by InjectFor, read by RebindGeometry. Main thread only.
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

        // Reconcile is declared in the public header and is safe to call from delayed retries.

        // One pending delayed call. std::priority_queue hands out only a CONST
        // top(), so the payload sits behind a shared_ptr to be movable out of it.
        struct DelayedCall
        {
            std::chrono::steady_clock::time_point due;
            std::shared_ptr<std::function<void()>> fn;
            // priority_queue is a MAX heap and we want the SOONEST deadline on
            // top, so this compares backwards on purpose.
            bool operator<(const DelayedCall& a_rhs) const { return due > a_rhs.due; }
        };
        std::priority_queue<DelayedCall> g_delayQueue;
        std::mutex g_delayMutex;
        std::condition_variable g_delayCv;

        // Wait until due, then hand the payload to the SKSE task queue ONCE. It
        // must NOT re-queue itself through the task pump until due: the pump is
        // frame-bound in gameplay (the rebind retry ran that way for days), but
        // the EARLY loading-screen pump drains tasks INCLUSIVELY, so an un-due
        // requeue re-ran forever and froze the load at the exact moment a chain
        // started there (in-game 2026-07-04 17:35, the bind watchdog's first tick
        // scheduled from the kDataLoaded Reconcile). AddTask is thread-safe.
        //
        // The waiting is done by ONE timer thread, not one thread per call.
        // Load3D schedules a 4000ms retry for every NPC that builds 3D, so a busy
        // cell was creating and tearing down an OS thread several times a second
        // (measured 2026-09-09: 544 created, 546 exited in 3 minutes). They cost
        // almost no CPU - they sleep - but a thread apiece to hold a deadline is
        // a stack and a kernel object for nothing.
        //
        // A heap rather than a queue because a call made later can be due EARLIER
        // (a 500ms rebind retry queued behind a 4000ms Load3D settle), so the wait
        // target is re-decided on every insert - which is what notifying the
        // condition variable makes wait_until do.
        //
        // Lock order: a caller may hold StoreLock while pushing here (RunAfterDelayMs
        // takes it), and the timer thread takes ONLY g_delayMutex and dispatches
        // outside it, so the two can never wait on each other.
        void RunAfterDelay(std::chrono::steady_clock::time_point a_due, std::function<void()> a_fn)
        {
            // Started by the first caller, then runs for the life of the process -
            // same contract as the bind watchdog, and detached for the same reason:
            // there is no shutdown hook to join it from.
            static std::once_flag s_timerStarted;
            std::call_once(s_timerStarted, [] {
                std::thread([] {
                    for (;;) {
                        std::function<void()> ready;
                        {
                            std::unique_lock lk(g_delayMutex);
                            g_delayCv.wait(lk, [] { return !g_delayQueue.empty(); });
                            const auto next = g_delayQueue.top().due;
                            if (std::chrono::steady_clock::now() < next) {
                                // A sooner call arriving wakes this; the top is
                                // then re-read. A spurious wake costs one loop.
                                g_delayCv.wait_until(lk, next);
                                continue;
                            }
                            ready = std::move(*g_delayQueue.top().fn);
                            g_delayQueue.pop();
                        }
                        // Outside the lock: no caller should ever block behind a
                        // dispatch, and AddTask is thread-safe by design.
                        if (auto* tasks = SKSE::GetTaskInterface()) {
                            tasks->AddTask(std::move(ready));
                        }
                    }
                }).detach();
            });
            {
                std::scoped_lock lk(g_delayMutex);
                g_delayQueue.push(
                    { a_due, std::make_shared<std::function<void()>>(std::move(a_fn)) });
            }
            g_delayCv.notify_one();
        }

        // X-DIAG: the budget is spent and this item is STILL binding static.
        // One line per content per episode (re-armed the moment it binds a
        // physics node again, so a later regression reports afresh). The counts
        // are THIS content's custom bones - the ones that exist only while its
        // carrier is live - split into "bound to physics" vs "total".
        void ReportStaticCarrier(const std::string& a_id)
        {
            if (!g_staticDiagReported.insert(a_id).second) {
                return;
            }
            const std::string carrier = CarrierModelForContent(a_id);
            const std::uint32_t expected = g_rebind3pFsmp + g_rebind3pRemap;
            // Ordered by measured frequency (2026-07-31): the MERGE RACE is the
            // common case now and heals itself - say so first, or this line
            // sends every reader off to CEF_sync.log for a non-problem.
            SKSE::log::warn(
                "  carrier diagnostic '{}': {} of {} custom bone(s) bound to an FSMP "
                "physics node after {} retries - carrier = '{}'. MOST COMMON and "
                "self-healing: the carrier was just (re)built and FSMP had not finished "
                "merging its bones when the retries ran - CEF re-arms automatically "
                "(watch for \"re-arming ... for rebind\" within ~15s; if the item binds "
                "then, this message was the race, not a fault). If it stays static: "
                "check CEF_sync.log - a content with no inline HDT xml is SKIPPED when "
                "the carrier is built (\"skipped for the carrier\") and can never bind. "
                "Otherwise: the wrong FILE (another mod overriding meshes\\CostumeFW, "
                "or a pristine stub from a release archive) - check it, then re-equip "
                "the token.",
                a_id, g_rebind3pFsmp, expected, kRebindRetryBudget,
                carrier.empty() ? "<none: persist head-carrier or unheld>" : carrier);
            SKSE::log::info("  '{}': parked - no more rebind retries until it binds "
                            "physics again (a carrier rebuild or a 3D rebuild re-arms it)",
                a_id);
        }

        void RequestRebindRetry(ActorState& a_state, const std::string& a_id)
        {
            // Already diagnosed as permanently static? Do not queue it again.
            //
            // In-game 2026-07-28: a content nifcarrier EXCLUDED from the carrier
            // ("skipped for the carrier - no inline HDT xml") can never bind, but
            // it still fell back to static on every pass, and every externally
            // triggered Reconcile re-armed the retry budget - so the retry loop
            // never ended. That run logged 35 retry rounds over 2.5 minutes,
            // growing to 7 items, each round a full detach + NIF load + clone +
            // rebind + reattach. Permanent scene-graph churn for work that cannot
            // succeed. g_staticDiagReported is cleared the moment the item binds
            // physics again (InjectFor), so a carrier rebuild or a 3D rebuild
            // still re-arms it - this parks the hopeless case, it does not give up
            // on a recoverable one.
            if (g_staticDiagReported.contains(a_id)) {
                return;
            }
            if (std::find(a_state.rebindRetryIds.begin(), a_state.rebindRetryIds.end(), a_id) ==
                a_state.rebindRetryIds.end()) {
                a_state.rebindRetryIds.push_back(a_id);
            }
            if (a_state.rebindRetryQueued) {
                return;
            }
            if (a_state.rebindRetryBudget <= 0) {
                ReportStaticCarrier(a_id);  // X-DIAG: out of retries, say why
                return;
            }
            --a_state.rebindRetryBudget;
            a_state.rebindRetryQueued = true;
            const auto handle = a_state.handle;
            const bool isPlayer = a_state.isPlayer;
            SKSE::log::info("  rebind retry queued (+{}ms): FSMP carrier may still be attaching",
                std::chrono::duration_cast<std::chrono::milliseconds>(kRebindRetryDelay).count());
            RunAfterDelay(std::chrono::steady_clock::now() + kRebindRetryDelay, [handle, isPlayer]() {
                // Locked explicitly: this payload calls the anonymous-namespace
                // DetachNodes and touches the retry ids directly, so it is the
                // one main-thread path into the registry that does not arrive
                // through a locked exported function.
                StoreLock lk;
                ActorState* state = isPlayer ? &PlayerState() : FindState(handle);
                if (!state) {
                    return;
                }
                state->rebindRetryQueued = false;
                ++g_persistDiag.rebindRetries;
                const auto ids = std::move(state->rebindRetryIds);
                state->rebindRetryIds.clear();
                SKSE::log::info("rebind retry: re-injecting {} static item(s)", ids.size());
                for (const auto& id : ids) {
                    DetachNodes(*state, id);
                }
                state->inRebindRetry = true;
                Reconcile();
                state->inRebindRetry = false;
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
        bool HasDeadPhysicsBind(const std::string& a_id, RE::NiAVObject* a_root3p,
            RE::NiNode* a_holder)
        {
            // The holder comes from the attachment RECORD, never from a name
            // search: a not-found GetObjectByName descends into EVERY holder's
            // children (§7 B-2 residual path), and the watchdog used to run
            // exactly that every 2.5s for every not-injected item.
            (void)a_id;
            if (!a_holder || !a_root3p) {
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
                    if (auto* node = obj->AsNode(); node && ChildrenWalkable(node)) {
                        for (auto& child : node->GetChildren()) {
                            auto* c = child.get();
                            if (c && !PlausibleObjectPtr(c)) {
                                NoteBadSlot("dead-bind sweep", node, c);
                                continue;
                            }
                            stack.push_back(c);
                        }
                    }
                }
            }

            bool dead = false;
            RE::BSVisit::TraverseScenegraphGeometries(a_holder,
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
            if (n > g_maxShapeBones) {
                g_maxShapeBones = n;  // vs the 80-bone DX11 skinning ceiling
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
            // Only a 3rd-person static fallback can mean "carrier still
            // attaching" - FSMP never builds physics on the 1st-person
            // skeleton, so a 1p remap is permanent and no reason to retry.
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const bool is3p = pc && a_root != pc->Get3D(true);
            if (is3p) {
                g_rebind3pFsmp += fsmpCount;    // X-DIAG: per-injection tallies
                g_rebind3pRemap += remapCount;
            }
            if (remapCount) {
                SKSE::log::warn("  remapped {} unresolved bone(s) to nearest ancestor "
                                "(static, no SMP sway; e.g. {})", remapCount, firstRemap);
                if (is3p) {
                    g_injectStatic3p = true;  // the injector turns this into a retry
                }
            }

            const bool haveWorldXf = (a_skin->boneWorldTransforms != nullptr);
            for (std::uint32_t i = 0; i < n; ++i) {
                a_skin->bones[i] = resolved[i];
                if (haveWorldXf) {
                    a_skin->boneWorldTransforms[i] = &resolved[i]->world;
                }
                if (g_boneRefSink) {
                    g_boneRefSink->emplace_back(resolved[i]);
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
        // virtuals - NOT the community-REL InitializeShader (ids 99866/106432) or
        // InvalidateTextures (99865/106431): those RESOLVE on 1.6.1170 but crash
        // with an access violation in BSLightingShader when called on a cloned
        // envmap material (and neither id exists in the VR address library).
        // (skee/po3 pattern, minus the material clone we don't need because the
        // material is private.)
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

        // True if this skin is a costume's bundled body/hands/feet shape - a
        // BSDismemberSkinInstance with a partition on biped slot 32/33/37. MUST be
        // read BEFORE RebindGeometry neutralizes the partition slots to 61. Used
        // to drop the shipped body so it does not double the player's real one.
        // The first dismember biped slot of a skin (32 body / 33 hands / 37 feet /
        // ...), or -1 if not a BSDismemberSkinInstance. Read BEFORE RebindGeometry
        // neutralizes the partition slots to 61. Annotates a shape in the MCM list.
        int ShapeSlot(RE::NiSkinInstance* a_skin)
        {
            // Same detection as RebindGeometry's neutralize path: only a
            // BSDismemberSkinInstance carries biped-slot partitions.
            const char* rtti = a_skin && a_skin->GetRTTI() ? a_skin->GetRTTI()->name : "";
            if (std::string_view(rtti) != "BSDismemberSkinInstance") {
                return -1;
            }
            auto* dsi = static_cast<RE::BSDismemberSkinInstance*>(a_skin);
            auto& rd = dsi->GetRuntimeData();
            return rd.numPartitions > 0 ? static_cast<int>(rd.partitions[0].slot) : -1;
        }

        // Find a non-empty "BODYTRI" NiStringExtraData anywhere in a subtree.
        // BodySlide puts it on the NIF root, but some outfits carry it on a child
        // node - and the caller also retries on the PRE-CLONE original, since
        // extra data may not survive Clone(). (Codex 2026-07-11 morph root cause.)
        RE::NiStringExtraData* FindBodyTri(RE::NiAVObject* a_root)
        {
            RE::NiStringExtraData* found = nullptr;
            if (!a_root) {
                return found;
            }
            RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_obj) {
                if (auto* sed = a_obj->GetExtraData<RE::NiStringExtraData>("BODYTRI");
                    sed && sed->value && *sed->value) {
                    found = sed;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return found;
        }

        // Inject onto one skeleton root (3D root). Returns true on attach.
        // a_applyMorph gates skee body morph (off for hair/head content, which
        // must not receive body-slider deformation). a_hideShapes = the NIF shape
        // names to DROP (per-content pick). a_cacheShapes: record this NIF's shapes
        // into the MCM cache (true only for the primary/3p pass so 1p doesn't
        // overwrite the full list with its partial one).
        // a_holder / a_parent: the attachment RECORD for this root. CEF keeps a
        // NiPointer to the node it created and to the node it hung it on, and
        // detaches through those - it never searches the skeleton for its own
        // work. See DetachRecorded for why.
        // a_visualOut: filled with the geometry this attach put on the skeleton,
        // for the visual shadow (see VisualSyncRef). Written only on a real
        // attach - an idempotent skip leaves the caller's existing list intact.
        // a_effectActive: whether an effect is already live on this actor, so the
        // shapes' own refraction baseline is not credited with the effect's bit.
        bool InjectOnRoot(RE::NiAVObject* a_root3D, const std::string& a_relPath,
            const std::string& a_nodeName, const RE::TESModelTextureSwap* a_swap,
            bool a_applyMorph, const std::unordered_set<std::string>& a_hideShapes,
            const std::string& a_id, bool a_cacheShapes, RE::Actor* a_morphActor,
            RE::NiPointer<RE::NiNode>& a_holder, RE::NiPointer<RE::NiNode>& a_parent,
            std::vector<VisualSyncRef>* a_visualOut = nullptr, bool a_effectActive = false)
        {
            if (!a_root3D) {
                return false;
            }
            // Resolve the attach point FIRST - it is also the idempotency key.
            // GetObjectByName here is the ENGINE's own lookup, which dispatches
            // per node type (BSFlattenedBoneTree overrides it); that is exactly
            // why CEF must not hand-roll a children walk of its own.
            RE::NiAVObject* skelRootObj = a_root3D->GetObjectByName(kSkeletonRootName);
            RE::NiNode* attachRoot = skelRootObj ? skelRootObj->AsNode() : a_root3D->AsNode();
            if (!attachRoot) {
                SKSE::log::error("  no attach root node");
                return false;
            }
            // One line per session per distinct runtime type: whether the attach
            // root is a BSFlattenedBoneTree (whose child slots are nobody's
            // contract, see DetachRecorded) is load-bearing - and UNKNOWN for
            // custom-race skeletons (the reporter runs BD Ungulates).
            {
                const char* rt = (skelRootObj && skelRootObj->GetRTTI())
                                   ? skelRootObj->GetRTTI()->name
                                   : "<no NPC Root - using root3D>";
                static std::unordered_set<std::string> s_seenRootTypes;
                if (s_seenRootTypes.insert(rt).second) {
                    SKSE::log::info("attach root '{}' runtime type: {}",
                        attachRoot->name.c_str(), rt);
                }
            }
            // Idempotency without searching: our own record says whether this
            // holder is already hanging on THIS attach root. After a 3D rebuild
            // attachRoot is a different node, so the record no longer matches and
            // we re-inject, which is the old behaviour minus the search.
            if (a_holder && a_parent.get() == attachRoot) {
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
            SKSE::log::debug("  root3D='{}' attachRoot='{}'",
                a_root3D->name.c_str(), attachRoot->name.c_str());

            // Collect the skinned geometry and rebind each to the live skeleton.
            // Bind/resolve against the live actor root (a_root3D), as skee does.
            std::vector<RE::NiPointer<RE::BSGeometry>> geoms;
            std::vector<std::pair<std::string, int>> shapeList;  // every skinned shape, for the MCM cache
            bool ok = true;
            RE::BSVisit::TraverseScenegraphGeometries(clone.get(),
                [&](RE::BSGeometry* a_geom) {
                    auto skin = a_geom->GetGeometryRuntimeData().skinInstance;
                    if (skin) {
                        const std::string sname = a_geom->name.c_str();
                        // Record the shape (name + dismember slot, BEFORE
                        // RebindGeometry neutralizes it) so the MCM can list it.
                        shapeList.emplace_back(sname, ShapeSlot(skin.get()));
                        // Per-shape hide: drop the shapes the user picked by name
                        // (e.g. a costume's bundled body doubling the real body).
                        if (!a_hideShapes.empty() && a_hideShapes.contains(sname)) {
                            SKSE::log::info("  hideshape: skipping '{}' (id={})", sname, a_id);
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        if (!RebindGeometry(skin.get(), a_root3D)) {
                            ok = false;
                            return RE::BSVisit::BSVisitControl::kStop;
                        }
                        geoms.emplace_back(RE::NiPointer<RE::BSGeometry>(a_geom));
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            if (a_cacheShapes && !shapeList.empty()) {
                SetContentShapes(a_id, shapeList);  // let the MCM list this content's shapes
            }

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
            //
            // Size the child array UP FRONT. This used to be Create(0), and that
            // WAS the leading suspect for the persist CTD until `cef arraytest`
            // measured it in-game (2026-07-28): Create(0) + AttachChild grows
            // correctly every time - capacity 1,2,3,4 with a valid buffer at each
            // step - so it does NOT produce the crashed state. Suspect refuted.
            //
            // Kept purely as an efficiency change, which the same measurement
            // justifies: growing from zero reallocates and copies on EVERY attach
            // (capacity tracked size exactly), so a 20-shape costume did 20
            // reallocations. geoms.size() is known here; one allocation covers it.
            RE::NiNode* holder =
                RE::NiNode::Create(static_cast<std::uint16_t>(std::min<std::size_t>(geoms.size(), 0xFFFF)));
            holder->name = a_nodeName.c_str();
            for (auto& g : geoms) {
                if (auto* p = g->parent) {
                    p->DetachChild(g.get());
                }
                holder->AttachChild(g.get(), true);
            }

            // Preserve the BodySlide morph reference: RaceMenu's ApplyVertexDiff
            // searches the node it is given for a "BODYTRI" NiStringExtraData and
            // applies NOTHING if it's missing. Our bare-geometry holder drops the
            // NIF's own nodes - so the injected mesh (body AND garments in the same
            // .tri) never got morphed. Search the whole clone subtree; if cloning
            // dropped the extra data, fall back to the pre-clone original (sharing
            // the ref-counted object with the holder is safe - skee only reads it).
            auto* bodyTri = FindBodyTri(clone.get());
            if (!bodyTri) {
                bodyTri = FindBodyTri(loaded.get());
            }
            if (bodyTri) {
                const bool added = holder->AddExtraData(bodyTri);
                SKSE::log::info("  BODYTRI '{}' -> holder ({})", bodyTri->value,
                    added ? "carried" : "ADD FAILED");
            } else {
                // Diagnostic: list what extras the loader kept - if even the
                // original root has none, the model DB stripped them (args issue).
                const auto dumpExtras = [](const char* a_tag, RE::NiObjectNET* a_net) {
                    if (!a_net) {
                        return;
                    }
                    std::string names;
                    const std::uint16_t n = a_net->GetExtraDataSize();
                    for (std::uint16_t i = 0; i < n; ++i) {
                        if (auto* e = a_net->GetExtraDataAt(i)) {
                            names += e->name.c_str();
                            names += ' ';
                        }
                    }
                    SKSE::log::info("  BODYTRI: none on {} (root extras: {})", a_tag,
                        names.empty() ? std::string("<none>") : names);
                };
                dumpExtras("clone", clone.get());
                dumpExtras("loaded", loaded.get());
            }

            attachRoot->AttachChild(holder, true);
            // Record the attachment. Both ends are held by NiPointer, so neither
            // can be freed while we still intend to detach through them.
            a_holder.reset(holder);
            a_parent.reset(attachRoot);

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
                BodyMorph::ApplyToNode(a_morphActor, holder);
            }

            // Baseline AFTER every in-inject mutation (alt textures, body morph):
            // from here on nothing legitimate touches this array, so any change
            // the sweep sees is a stomp.
            RecordHolderBaseline(holder);

            // Visual-shadow record: `geoms` IS what now hangs on the skeleton
            // (hidden shapes never got here, and unskinned nodes were left behind
            // in the clone), so no second traversal is needed. Read the flags
            // AFTER the texture/morph passes: whatever the shapes look like at
            // this point is the authored baseline an effect must never destroy.
            if (a_visualOut) {
                a_visualOut->clear();
                a_visualOut->reserve(geoms.size());
                for (auto& g : geoms) {
                    const bool ownRefr =
                        !a_effectActive && HasTempRefraction(ShaderPropOf(g.get()));
                    a_visualOut->push_back({ g, ownRefr, false });
                }
            }

            SKSE::log::debug("  attached {} ({} skinned shape(s))", a_nodeName, geoms.size());
            return true;
        }

        // Inject on player 3D: the 3P model on the 3rd-person skeleton, the 1P
        // model on the 1st-person skeleton. Each carries its own alt textures.
        bool InjectFor(ActorState& a_state, const ActiveItem& a_item)
        {
            auto* actor = ResolveActor(a_state);
            if (!actor) {
                return false;
            }
            const std::string nodeName = NodeName(a_item.id);
            const bool applyMorph = a_item.settings ? a_item.settings->bodyMorph :
                ShouldApplyBodyMorph(a_item.id);
            SKSE::log::info("  bodymorph gate '{}' -> {}",
                a_item.id, applyMorph ? "apply (opted in)" : "SKIP (not opted in)");
            std::unordered_set<std::string> hideShapes;
            if (a_item.settings) {
                hideShapes = a_item.settings->hideShapes;
            } else {
                const auto hideList = HideShapesFor(a_item.id);
                hideShapes.insert(hideList.begin(), hideList.end());
            }
            bool any = false;
            g_injectStatic3p = false;
            g_rebind3pFsmp = 0;   // X-DIAG: tallies belong to THIS injection
            g_rebind3pRemap = 0;
            g_maxShapeBones = 0;
            // Multi-content carriers prefix this content's custom bones
            // (nifcarrier namespace isolation) - same id, same prefix.
            g_rebindPrefix = nifcarrier::ContentNamePrefix(a_item.id);
            // Collect the bones this injection binds to, then APPEND them to the
            // state's pin set (append, not replace: an idempotent skip on one
            // skeleton must not drop the pins the other skeleton still uses).
            std::vector<RE::NiPointer<RE::NiAVObject>> collected;
            g_boneRefSink = &collected;
            // The attachment record lives on the registry entry; hand the right
            // slot to each root so InjectOnRoot can both skip an existing attach
            // and record a new one without searching the skeleton.
            ActiveItem* slot = nullptr;
            for (auto& it : a_state.items) {
                if (it.id == a_item.id) {
                    slot = &it;
                    break;
                }
            }
            // Separate scratch pairs per skeleton: an id that reaches here without
            // a registry entry should not exist (every caller registers first),
            // but sharing one pair would make the 1p pass see the 3p record and
            // skip itself.
            static RE::NiPointer<RE::NiNode> s_scratchHolder3p, s_scratchParent3p;
            static RE::NiPointer<RE::NiNode> s_scratchHolder1p, s_scratchParent1p;
            auto& holder3p = slot ? slot->holder3p : s_scratchHolder3p;
            auto& parent3p = slot ? slot->parent3p : s_scratchParent3p;
            auto& holder1p = slot ? slot->holder1p : s_scratchHolder1p;
            auto& parent1p = slot ? slot->parent1p : s_scratchParent1p;
            // No registry entry -> nothing owns a visual list either (the scratch
            // holders above are already the "should never happen" branch).
            auto* visual3p = slot ? &slot->visual3p : nullptr;
            auto* visual1p = slot ? &slot->visual1p : nullptr;
            if (auto* root3p = actor->Get3D(false); root3p && !a_item.m3p.nifPath.empty()) {
                any |= InjectOnRoot(root3p, StripMeshesPrefix(a_item.m3p.nifPath), nodeName,
                    a_item.m3p.swap, applyMorph, hideShapes, a_item.id, true, actor,
                    holder3p, parent3p, visual3p,
                    ReferenceEffectActive(a_state, actor, false));
            }
            if (g_rebind3pFsmp) {
                // Bound to physics again - re-arm the X-DIAG report so a later
                // regression is not swallowed by the once-per-episode guard.
                g_staticDiagReported.erase(a_item.id);
            }
            // Record this injection's 3p bind outcome for the bone-budget readout
            // (Diagnostics page). Written before the retry so the numbers reflect
            // the pass the user is looking at. An idempotent SKIP pass (already
            // attached - nothing rebound, all three tallies still 0) keeps the
            // previous REAL numbers instead: without the condition, any routine
            // Reconcile wiped the whole readout to zeros ("CFW content needs: 0 /
            // Heaviest shape: 0" with 1113 bones live - 2026-07-31 screenshot).
            if ((g_rebind3pFsmp || g_rebind3pRemap || g_maxShapeBones) && slot) {
                slot->fsmpBones = g_rebind3pFsmp;
                slot->staticBones = g_rebind3pRemap;
                slot->maxShapeBones = g_maxShapeBones;
            }
            // Bone Limit Extender guard: this shape asks for more bones than the
            // vanilla GPU skinning buffer holds, and nothing is lifting the limit.
            // The count is already measured here, so the check is free - and this
            // is the moment the item goes on, which is the moment before the
            // crash. Once per content per session, never mid-load spam.
            if (g_maxShapeBones > kVanillaShapeBoneLimit &&
                !GetModuleHandleA("skyrimbonelimitfix.dll") &&
                g_boneLimitWarned.insert(a_item.id).second) {
                SKSE::log::warn(
                    "bone limit: '{}' has a {}-bone shape and Bone Limit Extender is not "
                    "loaded (vanilla ceiling {}) - this crashes when the shape is skinned",
                    a_item.id, g_maxShapeBones, kVanillaShapeBoneLimit);
                RE::DebugNotification(
                    ("CostumeFW: " + ItemDisplayName(a_item.id) +
                        " needs Bone Limit Extender (Nexus 177636) - it can crash without it")
                        .c_str());
            }
            if (g_injectStatic3p) {
                RequestRebindRetry(a_state, a_item.id);
            }
            if (a_state.isPlayer) {
                if (auto* root1p = actor->Get3D(true); root1p && !a_item.m1p.nifPath.empty()) {
                    any |= InjectOnRoot(root1p, StripMeshesPrefix(a_item.m1p.nifPath), nodeName,
                        a_item.m1p.swap, applyMorph, hideShapes, a_item.id, false, actor,
                        holder1p, parent1p, visual1p,
                        ReferenceEffectActive(a_state, actor, true));
                }
            }
            g_boneRefSink = nullptr;
            if (!collected.empty()) {
                auto& refs = a_state.bonePins[a_item.id];
                refs.insert(refs.end(), collected.begin(), collected.end());
            }
            if (!any) {
                SKSE::log::warn("InjectFor: nothing attached for id='{}'", a_item.id);
            }
            // Immediate visual shadow. This is the fix for the reported bug: a
            // costume shown DURING an invisibility (or any effect shader) never
            // gets the engine's start-of-effect pass, so it must be handed the
            // body's current state the moment it lands. Every injection path -
            // Reconcile, box token equip, persist toggle, rebind retry, Load3D
            // re-attach - funnels through InjectFor, so this one call covers all
            // of them (§9.1's "one initial sync for every caller").
            SyncActorVisualState(a_state, true);
            return any;
        }

        // --- Substitute body (dual injection, HANDOVER §8.4 strategy 2) ----------
        bool ResolveArmaModels(std::uint32_t a_localID, const std::string& a_plugin, RE::SEX a_sex,
            const policy::CapturePolicy& a_policy, ModelRef& a_out3p, ModelRef& a_out1p,
            bool a_log = true);  // fwd (defined below)

        constexpr const char* kRealBodyNode = "CEF_RealBody";

        // The real body is not a registry item, so its attachment record lives on
        // the ActorState (per-actor: published snapshots can show a real body on
        // NPCs too). Both crash generations happened in DetachRealBody - see
        // DetachRecorded for why neither searching nor walking is allowed.
        void DetachRealBody(ActorState& a_state)
        {
            DetachRecorded(a_state.realBodyParent3p, a_state.realBodyHolder3p,
                &a_state.visualRealBody3p);
            DetachRecorded(a_state.realBodyParent1p, a_state.realBodyHolder1p,
                &a_state.visualRealBody1p);
        }

        // Pick the BODY (slot-32) addon from a skin ARMO's armature for the player's
        // race - not the first race-matching addon (which may be hands/feet/genital;
        // 2026-07-10 TNG's skin gave femalehands). Prefers an exact race match among
        // body addons, else any body addon. nullptr if the skin has no body addon.
        // a_raceMatched (optional) reports whether the returned addon actually
        // covers a_race (RNAM/additionalRaces) - the engine only RENDERS a skin
        // addon whose race matches, so callers adopting a non-default skin gate
        // on it (the anyBody fallback is leniency the engine does not share).
        RE::TESObjectARMA* PickBodyAddon(RE::TESObjectARMO* a_armo, RE::TESRace* a_race,
            bool* a_raceMatched = nullptr)
        {
            if (a_raceMatched) {
                *a_raceMatched = false;
            }
            if (!a_armo) {
                return nullptr;
            }
            const auto isBody = [](RE::TESObjectARMA* aa) {
                return (static_cast<std::uint32_t>(aa->GetSlotMask()) &
                        static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) != 0;
            };
            RE::TESObjectARMA* anyBody = nullptr;
            for (auto* aa : a_armo->armorAddons) {
                if (!aa || !isBody(aa)) {
                    continue;
                }
                if (!anyBody) {
                    anyBody = aa;
                }
                if (aa->race == a_race) {
                    if (a_raceMatched) {
                        *a_raceMatched = true;
                    }
                    return aa;
                }
                for (auto* extra : aa->additionalRaces) {
                    if (extra == a_race) {
                        if (a_raceMatched) {
                            *a_raceMatched = true;
                        }
                        return aa;
                    }
                }
            }
            return anyBody;
        }

        // A worn box token whose ARMAs cover neither the player's race nor its
        // ArmorRace proxy is a silent brick: the engine refuses to render the
        // (invisible) carrier, FSMP never sees its mesh, and every content in
        // the box stays permanently static - with nothing in any log. Proven
        // in-game 2026-08-02 on BD Ungulates (race ArmorRace=ImperialRace vs
        // the carrier's DefaultRace-only ARMA; headdiag showed zero Armor_
        // merge groups while the token was worn and the carrier file held).
        // The data fix puts the standard 23-race list on every CEF ARMA, so
        // this tripwire should stay silent - it exists for the next exotic
        // race (ArmorRace = itself, a custom race, or null).
        bool ArmaCoversRace(const RE::TESObjectARMA* a_aa, const RE::TESRace* a_race)
        {
            if (!a_aa || !a_race) {
                return false;
            }
            if (a_aa->race == a_race) {
                return true;
            }
            for (const auto* extra : a_aa->additionalRaces) {
                if (extra == a_race) {
                    return true;
                }
            }
            return false;
        }

        void WarnIfTokenRaceGap(RE::FormID a_tokenForm, RE::Actor* a_player)
        {
            if (a_tokenForm == 0 || !a_player) {
                return;
            }
            auto* race = a_player->GetRace();
            if (!race) {
                return;
            }
            // Once per (token, race) pair per session - a race swap mid-session
            // (ShowRaceMenu) re-arms the check for the new race.
            static std::unordered_set<std::uint64_t> s_checked;
            const std::uint64_t key =
                (static_cast<std::uint64_t>(a_tokenForm) << 32) | race->GetFormID();
            if (!s_checked.insert(key).second) {
                return;
            }
            auto* armo = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_tokenForm);
            if (!armo) {
                return;
            }
            const RE::TESRace* proxy = race->armorParentRace;
            for (auto* aa : armo->armorAddons) {
                if (ArmaCoversRace(aa, race) || (proxy && ArmaCoversRace(aa, proxy))) {
                    return;  // covered - the normal case
                }
            }
            const auto edid = [](const RE::TESForm* f) {
                const char* e = f ? f->GetFormEditorID() : nullptr;
                return (e && *e) ? e : "<no-edid>";
            };
            SKSE::log::warn(
                "box token '{}' ({:08X}): no armor addon covers the player's race '{}' "
                "(ArmorRace '{}') - the engine will not render the box carrier, so this "
                "box CANNOT get SMP physics on this race. The token/carrier ARMA needs "
                "the race (or its ArmorRace) in its race list",
                armo->GetName(), a_tokenForm, edid(race), edid(proxy));
            RE::DebugNotification(
                "CostumeFW: this box cannot get physics on your race (see the CEF log)");
        }

        // True if the addon carries ANY sex's 3P biped model path (the injection
        // falls back across sexes, so one non-empty model makes it usable).
        bool HasBodyModel(RE::TESObjectARMA* a_aa)
        {
            if (!a_aa) {
                return false;
            }
            for (int s = 0; s < static_cast<int>(RE::SEXES::kTotal); ++s) {
                if (const char* m = a_aa->bipedModels[s].model.c_str(); m && *m) {
                    return true;
                }
            }
            return false;
        }

        // Apply a skin TXST to EVERY shape under an injected holder node (a naked
        // body NIF's shapes are all skin). The engine layers the ARMA's per-sex
        // skin texture set (NAM0/NAM1) over whatever the body NIF references -
        // TXST-only alternate skins (RaceMenu Selector of Skins' skin02-04 share
        // the vanilla mesh and differ ONLY by texture set) rely on that. Without
        // this the injected copy keeps the NIF's baked-in texture paths. Also runs
        // on the idempotent already-attached path, so a TXST-only skin change
        // converges without waiting for a 3D rebuild.
        void ApplySkinTextures(RE::NiAVObject* a_root3D, const char* a_nodeName,
            RE::BGSTextureSet* a_txst)
        {
            auto* holder = a_root3D ? a_root3D->GetObjectByName(a_nodeName) : nullptr;
            if (!holder || !a_txst) {
                return;
            }
            int applied = 0;
            RE::BSVisit::TraverseScenegraphGeometries(holder,
                [&](RE::BSGeometry* a_geom) {
                    if (ApplyTextureSet(a_geom, a_txst)) {
                        ++applied;
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            if (applied > 0) {
                SKSE::log::info("realbody: skin TXST {:08X} -> {} shape(s)",
                    a_txst->GetFormID(), applied);
            }
        }

        // Inject the player's real (naked) skin body (slot 32) under a dedicated,
        // idempotent node so a costume whose own body shape is hidden shows the
        // player's morphed body instead of nothing. Morph ON. No-op if the skin
        // can't be resolved. Main thread only (called from Reconcile).
        bool InjectRealBody(ActorState& a_state)
        {
            auto* actor = ResolveActor(a_state);
            if (!actor) {
                return false;
            }
            // Engine-faithful skin resolution with a CONSERVATIVE adoption guard.
            // The engine renders the actor-base skin (WNAM - what SetSkin-style
            // mods write) over the race skin, so prefer it here - but adopt it
            // ONLY when the engine itself would render it on the naked player:
            // it must yield a slot-32 body addon that (a) race-matches the player
            // (the engine skips race-mismatched skin ARMAs; RNAM/additionalRaces)
            // and (b) carries a real model. Anything less - keyword-only TNG/SOS
            // tag skins with no body ARMA (2026-07-10: TNG_Skin_B08 injected an
            // empty body), race-mismatched ARMAs, empty models - falls through to
            // the race skin, which is byte-identical to the pre-2026-07-12 path
            // (anyBody leniency included). The addon's model is resolved directly
            // below (it may be a runtime form with no round-trippable lid/plugin).
            auto* base = actor->GetActorBase();
            auto* race = actor->GetRace();
            RE::TESObjectARMO* skin = nullptr;
            RE::TESObjectARMA* bodyAA = nullptr;
            const char* skinSrc = "race";  // logged: WHICH chain the skin came from
                                           // (EDIDs are usually empty at runtime)
            if (RE::TESObjectARMO* baseSkin = base ? base->skin : nullptr) {
                bool raceMatched = false;
                RE::TESObjectARMA* baseAA = PickBodyAddon(baseSkin, race, &raceMatched);
                if (baseAA && raceMatched && HasBodyModel(baseAA)) {
                    skin = baseSkin;
                    bodyAA = baseAA;
                    skinSrc = "base";
                } else {
                    const char* why = !baseAA          ? "no slot-32 body addon"
                                      : !raceMatched   ? "no race-matched body addon"
                                                       : "body addon has no model";
                    SKSE::log::info("realbody: base skin {:08X} not adopted ({}) - using race skin",
                        baseSkin->GetFormID(), why);
                }
            }
            if (!bodyAA) {
                skin = race ? race->skin : nullptr;
                bodyAA = PickBodyAddon(skin, race);
            }
            if (!skin) {
                SKSE::log::warn("realbody: no skin ARMO on player");
                return false;
            }
            if (!bodyAA) {
                SKSE::log::warn("realbody: skin '{}' has no slot-32 body addon",
                    skin->GetFormEditorID() ? skin->GetFormEditorID() : "?");
                return false;
            }
            const RE::SEX reqSex = base ? base->GetSex() : RE::SEX::kMale;
            RE::SEX sex = reqSex;
            RE::TESModelTextureSwap* mm3 = &bodyAA->bipedModels[sex];
            RE::TESModelTextureSwap* mm1 = &bodyAA->bipedModel1stPersons[sex];
            if (!mm3->model.c_str() || !*mm3->model.c_str()) {  // requested sex has no model
                const RE::SEX other = (sex == RE::SEXES::kMale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
                if (const char* on = bodyAA->bipedModels[other].model.c_str(); on && *on) {
                    sex = other;
                    mm3 = &bodyAA->bipedModels[other];
                    mm1 = &bodyAA->bipedModel1stPersons[other];
                }
            }
            const char* nif3 = mm3->model.c_str();
            if (!nif3 || !*nif3) {
                SKSE::log::warn("realbody: body addon {:08X} has no model", bodyAA->GetFormID());
                return false;
            }
            const char* nif1 = mm1->model.c_str();
            ModelRef m3p{ nif3, mm3 };
            ModelRef m1p = (nif1 && *nif1) ? ModelRef{ nif1, mm1 } : m3p;
            SKSE::log::info("realbody: skin '{}' ({:08X}, {}) body addon {:08X} sex={} model3p='{}'",
                skin->GetFormEditorID() ? skin->GetFormEditorID() : "?", skin->GetFormID(),
                skinSrc, bodyAA->GetFormID(), sex == RE::SEXES::kFemale ? "F" : "M", m3p.nifPath);
            // The addon's per-sex skin texture set - follow the (possibly
            // fallen-back) model sex so the TXST matches the mesh's UVs. Null =
            // the NIF's own textures stand.
            RE::BGSTextureSet* skinTx = bodyAA->skinTextures[sex];
            static const std::unordered_set<std::string> kNoHide;
            bool any = false;
            if (auto* root3p = actor->Get3D(false); root3p && !m3p.nifPath.empty()) {
                any |= InjectOnRoot(root3p, StripMeshesPrefix(m3p.nifPath), kRealBodyNode,
                    m3p.swap, true, kNoHide, "realbody", false, actor,
                    a_state.realBodyHolder3p, a_state.realBodyParent3p,
                    &a_state.visualRealBody3p, ReferenceEffectActive(a_state, actor, false));
                ApplySkinTextures(root3p, kRealBodyNode, skinTx);
            }
            if (a_state.isPlayer) {
                if (auto* root1p = actor->Get3D(true); root1p && !m1p.nifPath.empty()) {
                    any |= InjectOnRoot(root1p, StripMeshesPrefix(m1p.nifPath), kRealBodyNode,
                        m1p.swap, true, kNoHide, "realbody", false, actor,
                        a_state.realBodyHolder1p, a_state.realBodyParent1p,
                        &a_state.visualRealBody1p, ReferenceEffectActive(a_state, actor, true));
                    ApplySkinTextures(root1p, kRealBodyNode, skinTx);
                }
            }
            if (any) {
                SKSE::log::info("realbody: injected player body addon {:08X}", bodyAA->GetFormID());
            }
            // Same immediate shadow as InjectFor: a substitute body attached
            // mid-effect must not be the one shape still visible.
            SyncActorVisualState(a_state, true);
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

        RE::SEX ActorSexOf(RE::Actor* a_actor)
        {
            auto* base = a_actor ? a_actor->GetActorBase() : nullptr;
            const RE::SEX sex = base ? base->GetSex() : RE::SEXES::kFemale;
            return sex == RE::SEXES::kMale ? RE::SEXES::kMale : RE::SEXES::kFemale;
        }

        RE::SEX PlayerSex()
        {
            return ActorSexOf(RE::PlayerCharacter::GetSingleton());
        }

        RE::SEX EffectiveSexOf(RE::Actor* a_actor, const std::string& a_id)
        {
            switch (GenderModeFor(a_id)) {
            case 1:
                return RE::SEXES::kMale;
            case 2:
                return RE::SEXES::kFemale;
            default:
                return ActorSexOf(a_actor);
            }
        }

        RE::SEX EffectiveSex(const std::string& a_id)
        {
            return EffectiveSexOf(RE::PlayerCharacter::GetSingleton(), a_id);
        }

        // Parse a colon-form id "XXXXXX:Plugin.esp" into local FormID + plugin.
        // False if it isn't a colon-form (e.g. the raw-NIF "test" id).
        // Delegates to the pure policy module (r2: single source of truth,
        // host-tested in tests/policy_tests.cpp).
        bool ParseColonId(const std::string& a_id, std::uint32_t& a_localID, std::string& a_plugin)
        {
            return policy::ParseColonId(a_id, a_localID, a_plugin);
        }

        // Candidate-level hard admission. Nothing beyond formID/sourceFiles is
        // read until this returns a defining file. This is the r4 safety border
        // for foreign runtime/half-built ARMA pointers.
        const RE::TESFile* AdmitArmaSource(RE::TESObjectARMA* a_arma,
            const policy::CapturePolicy& a_policy, std::uint32_t a_localID,
            const std::string& a_plugin, bool a_log)
        {
            if (!a_arma) {
                return nullptr;
            }
            if (a_arma->IsDynamicForm()) {
                if (a_log) {
                    SKSE::log::warn("ResolveArma: {:X}:{} skips runtime ARMA {:08X}",
                        a_localID, a_plugin, a_arma->GetFormID());
                }
                return nullptr;
            }
            const auto* file = a_arma->GetFile(0);
            if (!file) {
                if (a_log) {
                    SKSE::log::warn("ResolveArma: {:X}:{} skips no-file ARMA {:08X}",
                        a_localID, a_plugin, a_arma->GetFormID());
                }
                return nullptr;
            }
            if (policy::PluginDenied(a_policy, file->GetFilename())) {
                if (a_log) {
                    SKSE::log::warn(
                        "ResolveArma: {:X}:{} skips ARMA {:08X} from deny-listed plugin '{}'",
                        a_localID, a_plugin, a_arma->GetFormID(), file->GetFilename());
                }
                return nullptr;
            }
            return file;
        }

        // Exact race, additional race, then data-order fallback - identical to
        // the old picker, but only among candidates that passed hard admission.
        // The race is a PARAMETER (NPC generalization): pass the wearer's race.
        RE::TESObjectARMA* PickAdmittedAddonForRace(RE::TESObjectARMO* a_armo,
            const policy::CapturePolicy& a_policy, std::uint32_t a_localID,
            const std::string& a_plugin, bool a_log, RE::TESRace* a_race,
            bool* a_allRefused = nullptr)
        {
            if (!a_armo || a_armo->armorAddons.empty()) {
                return nullptr;
            }
            std::vector<RE::TESObjectARMA*> admitted;
            admitted.reserve(a_armo->armorAddons.size());
            for (auto* addon : a_armo->armorAddons) {
                if (AdmitArmaSource(addon, a_policy, a_localID, a_plugin, a_log)) {
                    admitted.push_back(addon);
                }
            }
            if (admitted.empty()) {
                // X-LOG1: candidates existed and the policy turned every one of
                // them away - that is a DECISION, not a failure to resolve.
                if (a_allRefused) {
                    *a_allRefused = true;
                }
                return nullptr;
            }
            auto* race = a_race;

            // SLOT FIRST, race as the tiebreaker. PickBodyAddon learned this on
            // 2026-07-10 ("not the first race-matching addon (which may be
            // hands/feet/genital) - TNG's skin gave femalehands") but the CONTENT
            // path never got the same treatment: it took the first race match out
            // of the armature regardless of which body part that addon draws.
            //
            // An ARMO whose armature holds more than one addon is common - a
            // helmet plus its hair-hiding piece, a costume plus a bundled base -
            // and picking the wrong member injects the wrong MESH. That is the
            // shape of the 2026-07-27 report: "the Helms would Persist but the
            // Horns would turn into Hide helmets", two items from one mod where
            // one resolved right and the other picked a sibling addon.
            //
            // The ARMO's own slot mask says which part the user captured, so
            // prefer an addon that actually covers one of those slots.
            const std::uint32_t want = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            const auto covers = [want](RE::TESObjectARMA* a_addon) {
                return want == 0 ||
                       (static_cast<std::uint32_t>(a_addon->GetSlotMask()) & want) != 0;
            };
            const auto raceExact = [race](RE::TESObjectARMA* a_addon) {
                return race && a_addon->race == race;
            };
            const auto raceExtra = [race](RE::TESObjectARMA* a_addon) {
                if (!race) {
                    return false;
                }
                for (auto* extra : a_addon->additionalRaces) {
                    if (extra == race) {
                        return true;
                    }
                }
                return false;
            };
            // Best to worst. Slot-matching tiers come first because an addon that
            // draws the wrong part is wrong even when its race is right; the
            // race-only tiers preserve the old behaviour as a floor.
            const std::pair<const char*, std::function<bool(RE::TESObjectARMA*)>> tiers[] = {
                { "slot+race", [&](RE::TESObjectARMA* a) { return covers(a) && raceExact(a); } },
                { "slot+altrace", [&](RE::TESObjectARMA* a) { return covers(a) && raceExtra(a); } },
                { "slot", [&](RE::TESObjectARMA* a) { return covers(a); } },
                { "race", [&](RE::TESObjectARMA* a) { return raceExact(a); } },
                { "altrace", [&](RE::TESObjectARMA* a) { return raceExtra(a); } },
            };
            for (const auto& [why, match] : tiers) {
                for (auto* addon : admitted) {
                    if (match(addon)) {
                        if (a_log && admitted.size() > 1) {
                            SKSE::log::info(
                                "  addon pick {:X}:{}: '{}' by {} (armo slots {:08X}, addon "
                                "slots {:08X}, {} candidate(s))",
                                a_localID, a_plugin,
                                addon->GetFormEditorID() ? addon->GetFormEditorID() : "?", why,
                                want, static_cast<std::uint32_t>(addon->GetSlotMask()),
                                admitted.size());
                        }
                        return addon;
                    }
                }
            }
            if (a_log && admitted.size() > 1) {
                SKSE::log::warn(
                    "  addon pick {:X}:{}: no addon covers the item's slots ({:08X}) or the "
                    "player's race - falling back to the first of {} candidate(s); the injected "
                    "mesh may be the wrong body part",
                    a_localID, a_plugin, want, admitted.size());
            }
            return admitted.front();
        }

        // Resolve an ARMA (or ARMO -> its race-matched admitted ARMA) to the
        // 3P + 1P models for the given wearer race. Every caller supplies one
        // immutable policy generation. If the requested sex has no model, falls
        // back to the other sex so a single-sex-authored accessory still shows.
        bool ResolveArmaModelsFor(std::uint32_t a_localID, const std::string& a_plugin,
            RE::SEX a_sex, RE::TESRace* a_race,
            const policy::CapturePolicy& a_policy, ModelRef& a_out3p, ModelRef& a_out1p,
            bool a_log)

        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return false;
            }
            RE::TESObjectARMA* arma = dh->LookupForm<RE::TESObjectARMA>(a_localID, a_plugin);
            const RE::TESFile* armaFile = nullptr;
            bool policyRefused = false;  // X-LOG1: refused by the deny-list, not unresolvable
            if (arma) {
                armaFile = AdmitArmaSource(arma, a_policy, a_localID, a_plugin, a_log);
                if (!armaFile) {
                    return false;
                }
            } else if (auto* armo = dh->LookupForm<RE::TESObjectARMO>(a_localID, a_plugin)) {
                arma = PickAdmittedAddonForRace(
                    armo, a_policy, a_localID, a_plugin, a_log, a_race, &policyRefused);
                if (arma) {
                    // The candidate picker returned it only after this succeeded.
                    armaFile = arma->GetFile(0);
                }
            }
            if (!arma || !armaFile) {
                if (a_log) {
                    // X-LOG1: an explicit deny is policy working as designed, so it
                    // logs at warn - error stays for "this really cannot resolve".
                    // (In the 2026-07-26 run every single error in the log was this
                    // one line, firing on deliberate fixture denials.)
                    if (policyRefused) {
                        SKSE::log::warn(
                            "ResolveArma: {:X}:{} has no admitted ARMA "
                            "(every candidate refused by the capture policy)",
                            a_localID, a_plugin);
                    } else {
                        SKSE::log::error(
                            "ResolveArma: {:X}:{} has no admitted ARMA", a_localID, a_plugin);
                    }
                }
                return false;
            }

            // r4 final identity layer: a wrapper ARMO may be allowed while its
            // selected addon is explicitly denied. GetLocalFormID is safe only
            // after the defining-file guard above.
            if (!a_policy.ids.empty()) {
                const std::string armaId = policy::FormatColonId(
                    arma->GetLocalFormID(), armaFile->GetFilename());
                if (policy::IdDenied(a_policy, armaId)) {
                    if (a_log) {
                        SKSE::log::warn(
                            "ResolveArma: {:X}:{} selects deny-listed ARMA '{}' - refused",
                            a_localID, a_plugin, armaId);
                    }
                    return false;
                }
            }

            const auto sexName = [](RE::SEX s) {
                return s == RE::SEXES::kMale ? "male" : "female";
            };
            RE::SEX sex = a_sex;
            RE::TESModelTextureSwap* m3 = &arma->bipedModels[sex];
            RE::TESModelTextureSwap* m1 = &arma->bipedModel1stPersons[sex];
            const char* nif3p = m3->model.c_str();
            if (!nif3p || !*nif3p) {
                const RE::SEX other =
                    (sex == RE::SEXES::kMale) ? RE::SEXES::kFemale : RE::SEXES::kMale;
                RE::TESModelTextureSwap* o3 = &arma->bipedModels[other];
                const char* on = o3->model.c_str();
                if (on && *on) {
                    if (a_log) {
                        SKSE::log::warn(
                            "ResolveArma: {:X}:{} has no {} 3P model, using {} model",
                            a_localID, a_plugin, sexName(sex), sexName(other));
                    }
                    sex = other;
                    m3 = o3;
                    m1 = &arma->bipedModel1stPersons[other];
                    nif3p = m3->model.c_str();
                } else {
                    if (a_log) {
                        SKSE::log::error(
                            "ResolveArma: {:X}:{} has no 3P model for either sex",
                            a_localID, a_plugin);
                    }
                    return false;
                }
            }
            a_out3p = ModelRef{ nif3p, m3 };
            const char* nif1p = m1->model.c_str();
            a_out1p = (nif1p && *nif1p) ? ModelRef{ nif1p, m1 } : a_out3p;
            return true;
        }

        // Player-race convenience wrapper (the fwd-declared name main's player
        // paths call). NPC paths call ResolveArmaModelsFor with the wearer race.
        bool ResolveArmaModels(std::uint32_t a_localID, const std::string& a_plugin, RE::SEX a_sex,
            const policy::CapturePolicy& a_policy, ModelRef& a_out3p, ModelRef& a_out1p,
            bool a_log)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            return ResolveArmaModelsFor(a_localID, a_plugin, a_sex,
                player ? player->GetRace() : nullptr, a_policy, a_out3p, a_out1p, a_log);
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
        StoreLock lk;
        const ModelRef m{ a_nifPath, nullptr };
        auto& state = PlayerState();
        Register(state, a_id, m, m);
        Reconcile();
        return true;
    }

    void RunAfterDelayMs(int a_ms, std::function<void()> a_fn)
    {
        StoreLock lk;
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
        StoreLock lk;
        if (Diag::Debug()) {
            static int s_tick = 0;
            if (++s_tick % 12 == 0) {  // ~every 30s at the 2.5s cadence
                Diag::LogMemoryUsageDebugLine();
            }
        }
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
        // Containment between Reconciles: this tick is the always-on detector
        // that timestamps WHEN a holder went bad - the corruption predates the
        // crash it used to cause (§7 B-1's corollary), and only a periodic
        // record-based check turns that from inference into a measurement.
        {
            int q = QuarantineSweep();
            for (auto& state : g_actors) {
                q += QuarantineIfBroken("realbody-3p", state.realBodyParent3p,
                         state.realBodyHolder3p, &state.visualRealBody3p) ? 1 : 0;
                q += QuarantineIfBroken("realbody-1p", state.realBodyParent1p,
                         state.realBodyHolder1p, &state.visualRealBody1p) ? 1 : 0;
            }
            if (q > 0) {
                Reconcile();  // re-inject what was contained
                return;
            }
        }
        for (auto& state : g_actors) {
            auto* actor = ResolveActor(state);
            auto* root = actor ? actor->Get3D(false) : nullptr;
            if (!root || (state.isPlayer &&
                std::chrono::steady_clock::now() < g_headRebuildGraceUntil)) {
                continue;
            }
            for (const auto& it : state.items) {
                if (Diag::Debug() && it.holder3p) {
                    const auto st = ReadHolderArray(it.holder3p.get());
                    SKSE::log::debug("healthpoll '{}': size={} cap={} freeIdx={} data={:#x}",
                        it.id, st.size, st.cap, st.freeIdx, st.data);
                }
                if (!it.holder3p) {
                    continue;  // not injected: record-based skip (no name search)
                }
                if (HasDeadPhysicsBind(it.id, root, it.holder3p.get())) {
                    ++g_persistDiag.watchdogReconciles;
                    SKSE::log::info(
                        "bind watchdog: '{}' holds a dead merge generation - reconciling", it.id);
                    Reconcile();  // the sweep inside detaches + re-injects
                    return;
                }
            }
        }
    }

    void StartBindWatchdogOnce()
    {
        StoreLock lk;
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

    // Attachment census, straight from the registry - no scene-graph walk, so it
    // is safe to run automatically and costs nothing.
    //
    // It exists because the console cannot be used for this. The crash lands
    // during a persist operation with the menu open, and you can neither type a
    // command while a menu is up nor react to a CTD that has already happened
    // (reporter, 2026-07-29: "the game crashes without the ability to load the
    // console"). Anything a bug report needs has to reach the log by itself.
    //
    // Logged only when the numbers CHANGE, so a busy session leaves a short
    // legible trail instead of one line per Reconcile.
    void LogAttachmentCensus(const char* a_tag)
    {
        std::size_t contents = 0, on3p = 0, on1p = 0, npcStates = 0;
        bool realBody = false;
        for (const auto& state : g_actors) {
            if (!state.isPlayer && !state.items.empty()) {
                ++npcStates;
            }
            for (const auto& it : state.items) {
                ++contents;
                if (it.holder3p) {
                    ++on3p;
                }
                if (it.holder1p) {
                    ++on1p;
                }
            }
            realBody |= static_cast<bool>(state.realBodyHolder3p);
        }
        static std::size_t s_lastContents = SIZE_MAX, s_last3p = 0, s_last1p = 0, s_lastNpc = 0;
        static bool s_lastRealBody = false;
        if (contents == s_lastContents && on3p == s_last3p && on1p == s_last1p &&
            realBody == s_lastRealBody && npcStates == s_lastNpc) {
            return;
        }
        s_lastContents = contents;
        s_last3p = on3p;
        s_last1p = on1p;
        s_lastNpc = npcStates;
        s_lastRealBody = realBody;
        SKSE::log::info(
            "attached: {} content(s) registered, {} on 3p, {} on 1p, {} NPC state(s), "
            "real body {} ({})",
            contents, on3p, on1p, npcStates, realBody ? "ON" : "off", a_tag);
    }

    namespace
    {
        // One actor's pass of Reconcile. Callers hold the StoreLock and supply
        // one immutable policy generation + the master-switch state for the
        // whole sweep.
        void ReconcileActorImpl(ActorState& a_state, const policy::CapturePolicy& a_pol,
            const bool a_cefOn)
        {
            // Every externally-triggered pass re-arms the rebind-retry budget; the
            // retry passes themselves must not, or persistently-static contents
            // (custom bones with no carrier) would retry forever.
            if (!a_state.inRebindRetry) {
                a_state.rebindRetryBudget = kRebindRetryBudget;
            }
            auto* actor = ResolveActor(a_state);
            if (!actor) {
                a_state.bonePins.clear();
                return;
            }
            bool anyRealBody = false;  // set if a shown content wants the real body under it
            for (auto& it : a_state.items) {
                // master off -> hide; else persist (tokenForm 0) always shows, a
                // box/publish item shows only while its token is worn by THIS actor.
                bool show = false;
                if (a_cefOn) {
                    show = (it.tokenForm == 0);
                    if (!show) {
                        show = (actor->GetWornArmor(it.tokenForm) != nullptr);
                        if (show) {
                            WarnIfTokenRaceGap(it.tokenForm, actor);
                        }
                    }
                }
                // Hide-when-worn (8.10): hide this content while any of its
                // configured vanilla slots is occupied by NON-CEF real equipment
                // (boots over foot nails, helmet over a wig, ...). Auto-reshows
                // when the slot frees. Published snapshots carry FROZEN settings;
                // live settings otherwise.
                if (show) {
                    const auto hideSlots =
                        it.settings ? it.settings->hideSlots : HideSlotsFor(it.id);
                    for (const int slot : hideSlots) {
                        if (slot < 30 || slot > 61) {
                            continue;
                        }
                        auto* worn = actor->GetWornArmor(
                            static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << (slot - 30)));
                        if (worn && !IsCefToken(worn->GetFormID())) {
                            show = false;  // a real (non-token) item holds the slot
                            break;
                        }
                    }
                }
                // Sex-aware models: re-resolve for the wearer's current effective
                // sex (frozen genderMode wins) and RACE before injecting.
                if (show) {
                    RE::SEX sex = ActorSexOf(actor);
                    const int mode = it.settings ? it.settings->genderMode : GenderModeFor(it.id);
                    if (mode == 1) {
                        sex = RE::SEXES::kMale;
                    }
                    if (mode == 2) {
                        sex = RE::SEXES::kFemale;
                    }
                    if (it.resolvedSex != sex) {
                        std::uint32_t lid = 0;
                        std::string plg;
                        ModelRef m3p, m1p;
                        if (ParseColonId(it.id, lid, plg) &&
                            ResolveArmaModelsFor(lid, plg, sex, actor->GetRace(), a_pol,
                                m3p, m1p, true)) {
                            it.m3p = m3p;
                            it.m1p = m1p;
                        }
                        it.resolvedSex = sex;  // mark resolved (raw-NIF items: nothing to redo)
                    }
                }
                // Parked after repeated quarantines: injecting again just re-arms
                // the CTD the containment defused. Sits out until the next save load.
                if (show && g_poisonParked.contains(it.id)) {
                    show = false;
                }
                SKSE::log::debug("  item '{}' tokenForm={:08X} show={}", it.id, it.tokenForm, show);
                if (show) {
                    // Dead-bind sweep: if this item's injected mesh holds FSMP bones of
                    // a dead merge generation (armor unequipped / head rebuilt), detach
                    // it first so the injection below rebinds to the CURRENT generation.
                    if (auto* r3 = actor->Get3D(false);
                        r3 && it.holder3p && HasDeadPhysicsBind(it.id, r3, it.holder3p.get())) {
                        ++g_persistDiag.deadBindReinjects;
                        SKSE::log::info("  '{}': bound FSMP bones are DETACHED (dead merge "
                                        "generation) - re-injecting", it.id);
                        DetachNodes(a_state, it.id);
                    }
                    InjectFor(a_state, it);
                    anyRealBody |= it.settings ? it.settings->showRealBody : ShowRealBodyOn(it.id);
                } else {
                    DetachNodes(a_state, it.id);
                }
            }
            // Substitute body (dual injection, HANDOVER 8.4 strategy 2): when a
            // shown content opted in, inject the wearer's real skin body (paired
            // with hideShapes dropping the costume's own body); else remove it.
            if (anyRealBody && !g_poisonParked.contains("realbody-3p") &&
                !g_poisonParked.contains("realbody-1p")) {
                a_state.realBodyShown = InjectRealBody(a_state);
            } else {
                DetachRealBody(a_state);
                a_state.realBodyShown = false;
            }
        }
    }

    // Frame containment: called from the PlayerCharacter::Update prologue
    // (plugin.cpp, vfunc 0xAD) EVERY frame. The 2026-07-31 reporter crash
    // measured the failure mode this closes: a holder was attached at
    // 15:47:52.760 and the engine's light-registration walk (SkyrimSE ids
    // 106350/106353, inside Update) read its stomped children array within the
    // same second - the 2.5s watchdog cadence never got a turn. Prologue
    // position matters: contain first, then let the engine walk. Residual
    // window: a stomp landing mid-frame, between this sweep and the walk.
    // Covers EVERY actor state (NPC holders live in the same scene and stomp
    // the same way); cost when healthy is a handful of field reads per
    // attached holder, no allocation, no logging.
    void ContainmentSweepFrame()
    {
        StoreLock lk;
        bool anything = false;
        for (const auto& state : g_actors) {
            if (!state.items.empty() || state.realBodyHolder3p || state.realBodyHolder1p) {
                anything = true;
                break;
            }
        }
        if (!anything) {
            return;
        }
        int q = QuarantineSweep();
        for (auto& state : g_actors) {
            q += QuarantineIfBroken("realbody-3p", state.realBodyParent3p,
                     state.realBodyHolder3p, &state.visualRealBody3p) ? 1 : 0;
            q += QuarantineIfBroken("realbody-1p", state.realBodyParent1p,
                     state.realBodyHolder1p, &state.visualRealBody1p) ? 1 : 0;
        }
        if (q > 0) {
            SKSE::log::error(
                "frame containment: {} holder(s) contained before this frame's scene "
                "pass - re-inject queued", q);
            SKSE::GetTaskInterface()->AddTask([] { Reconcile(); });
        }
    }

    void SyncInjectedVisualState()
    {
        StoreLock lk;
        // Maintenance half of the visual shadow (the injection half runs from
        // InjectFor / InjectRealBody). Runs AFTER the engine's own Update, so
        // whatever a visual effect wrote onto the actor this frame is already
        // there to be read.
        //
        // This is deliberately NOT a per-frame material writer: per actor and
        // skeleton it costs one pointer compare (is the reference part still the
        // equipped one) plus one (bit, pointer) compare, and it writes only in
        // the frames where the body's own state moved - effect start, effect end,
        // effect swap. An actor with no CEF geometry exits on the first line.
        for (auto& state : g_actors) {
            SyncActorVisualState(state, false);
        }
    }

    // WP0.4: drop NPC states whose actor object is gone (freed on cell unload /
    // deleted) or that track nothing - without this g_actors grows for the whole
    // session. A swept state is reconstructible: the actor's next equip event
    // (publish) or OnNpcActorLoaded (npc-persist) re-registers it, and erasing
    // the state releases its bone pins. The player slot is never swept.
    static void SweepDeadActors()
    {
        std::erase_if(g_actors, [](ActorState& a_state) {
            if (a_state.isPlayer) return false;
            if (a_state.items.empty()) return true;
            const auto ref = a_state.handle.get();
            return !ref || !ref.get()->As<RE::Actor>();
        });
        RefreshNpcBindingsGate();
    }

    void Reconcile()
    {
        StoreLock lk;
        Diag::NoteExecutionThread("Reconcile");
        StartBindWatchdogOnce();
        ++g_persistDiag.reconcileCalls;
        PlayerState();  // keep the player slot 0 invariant
        // Containment first: a corrupted holder must not be reused through the
        // idempotency record, walked by the engine searches below, or released
        // raw by a detach (its destructor dies on the broken array).
        {
            int q = QuarantineSweep();
            for (auto& state : g_actors) {
                q += QuarantineIfBroken("realbody-3p", state.realBodyParent3p,
                         state.realBodyHolder3p, &state.visualRealBody3p) ? 1 : 0;
                q += QuarantineIfBroken("realbody-1p", state.realBodyParent1p,
                         state.realBodyHolder1p, &state.visualRealBody1p) ? 1 : 0;
            }
            if (q > 0) {
                SKSE::log::error("quarantine: {} holder(s) contained this pass", q);
            }
        }
        const auto pol = CapturePolicySnapshot();  // one generation for the whole sweep
        const bool cefOn = CefEnabled();  // master off (Main page) -> hide everything
        SKSE::log::info("Reconcile: {} actor state(s) (cef enabled={})", g_actors.size(), cefOn);
        for (auto& state : g_actors) {
            ReconcileActorImpl(state, *pol, cefOn);
        }
        SweepDeadActors();
        LogAttachmentCensus("reconcile");
    }

    void ReconcileActorByHandle(RE::ActorHandle a_handle)
    {
        StoreLock lk;
        if (auto* state = FindState(a_handle)) {
            const auto pol = CapturePolicySnapshot();
            ReconcileActorImpl(*state, *pol, CefEnabled());
        }
    }

    bool HasActorBindings(RE::Actor* a_actor)
    {
        auto* state = FindState(a_actor);
        return state && !state->items.empty();
    }

    bool AnyActorBindings()
    {
        return g_anyNpcBindings.load(std::memory_order_relaxed);
    }

    bool RegisterActorContent(RE::Actor* a_actor, const std::string& a_contentId,
        const std::string& a_tokenId, std::uint32_t a_tokenForm,
        std::shared_ptr<const ContentSettings> a_settings)
    {
        if (!a_actor) return false;
        ActorState* state = FindState(a_actor);
        if (!state) {
            g_actors.push_back({ a_actor->GetHandle(), false });
            state = &g_actors.back();
        }
        std::string id = a_contentId;
        CanonicalizeColonId(id);
        std::uint32_t local = 0;
        std::string plugin;
        if (!ParseColonId(id, local, plugin)) return false;
        const int mode = a_settings ? a_settings->genderMode : GenderModeFor(id);
        RE::SEX sex = ActorSexOf(a_actor);
        if (mode == 1) sex = RE::SEXES::kMale;
        if (mode == 2) sex = RE::SEXES::kFemale;
        ModelRef m3p, m1p;
        const auto pol = CapturePolicySnapshot();
        if (!ResolveArmaModelsFor(local, plugin, sex, a_actor->GetRace(), *pol, m3p, m1p, true)) {
            return false;
        }
        Register(*state, id, m3p, m1p, a_tokenId, a_tokenForm, sex);
        for (auto& item : state->items) {
            if (item.id == id) item.settings = std::move(a_settings);
        }
        RefreshNpcBindingsGate();
        return true;
    }

    void RemoveActorToken(RE::Actor* a_actor, std::uint32_t a_tokenForm)
    {
        auto* state = FindState(a_actor);
        if (!state) return;
        std::vector<std::string> ids;
        for (const auto& item : state->items) {
            if (item.tokenForm == a_tokenForm) ids.push_back(item.id);
        }
        for (const auto& id : ids) {
            DetachNodes(*state, id);
            Unregister(*state, id);
        }
        RefreshNpcBindingsGate();
    }

    void RemoveActorContent(RE::Actor* a_actor, const std::string& a_contentId)
    {
        auto* state = FindState(a_actor);
        if (!state) return;
        DetachNodes(*state, a_contentId);
        Unregister(*state, a_contentId);
        RefreshNpcBindingsGate();
    }

    std::size_t InjectedNpcCount()
    {
        // Count only states whose actor currently HAS a 3D: a state whose actor
        // unloaded (or died holding items) still occupied a cap slot, so with
        // maxNpcInjected=8 the 9th NPC silently showed nothing while the screen
        // had zero dressed NPCs (NPC_AUDIT_2026-08-03 H4). Unloaded states cost
        // no injection budget - they re-count the moment their 3D loads.
        return static_cast<std::size_t>(std::count_if(g_actors.begin(), g_actors.end(),
            [](const ActorState& state) {
                if (state.isPlayer || state.items.empty()) {
                    return false;
                }
                const auto ref = state.handle.get();
                auto* actor = ref ? ref.get()->As<RE::Actor>() : nullptr;
                return actor && actor->Get3D(false) != nullptr;
            }));
    }

    std::uint32_t ResolveFormId(const std::string& a_colonId)
    {
        StoreLock lk;
        return ResolveFormID(a_colonId);
    }

    bool IsTrackedToken(RE::FormID a_form)
    {
        StoreLock lk;
        if (a_form == 0) {
            return false;
        }
        for (const auto& state : g_actors) {
            for (const auto& it : state.items) {
                if (it.tokenForm == a_form) return true;
            }
        }
        return false;
    }

    bool CanonicalizeColonId(std::string& a_id)
    {
        StoreLock lk;
        // Delegates to the pure policy module (r2: single source of truth for
        // parse/format, incl. the never-truncate formatter - host-tested).
        return policy::CanonicalizeColonIdStr(a_id);
    }

    bool ResolveAdmittedModelPath(const std::string& a_contentId, RE::SEX a_sex,
        const policy::CapturePolicy& a_policy, std::string& a_nifOut, bool a_log)
    {
        StoreLock lk;
        // One policy generation covers both the content's base form and the
        // selected addon. This is the only seam carrier-manifest code may use.
        if (!IsContentAdmissible(a_contentId, a_policy, nullptr, a_log)) {
            return false;
        }
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(a_contentId, localID, plugin)) {
            return false;
        }
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(
                localID, plugin, a_sex, a_policy, m3p, m1p, a_log)) {
            return false;
        }
        a_nifOut = m3p.nifPath;
        return true;
    }

    bool CanResolveContent(const std::string& a_contentId)
    {
        StoreLock lk;
        const auto pol = CapturePolicySnapshot();
        std::string nif;
        return ResolveAdmittedModelPath(
            a_contentId, EffectiveSex(a_contentId), *pol, nif);
    }

    std::vector<std::pair<std::string, int>> EnumerateContentShapes(const std::string& a_id)
    {
        StoreLock lk;
        // Main thread only: resolve the content's ARMA model, load the NIF, list its
        // skinned shapes (name + first dismember biped slot), and cache the result so
        // the MCM's GetContentShapes can read it without a VM-thread NIF load. Empty
        // on failure. LoadNif returns the shared cached model - read-only traversal.
        std::vector<std::pair<std::string, int>> out;
        const auto pol = CapturePolicySnapshot();
        std::string nif;
        if (!ResolveAdmittedModelPath(a_id, EffectiveSex(a_id), *pol, nif)) {
            return out;
        }
        auto loaded = LoadNif(StripMeshesPrefix(nif));
        if (!loaded) {
            return out;
        }
        RE::BSVisit::TraverseScenegraphGeometries(loaded.get(),
            [&](RE::BSGeometry* a_geom) {
                auto skin = a_geom->GetGeometryRuntimeData().skinInstance;
                if (skin) {
                    out.emplace_back(std::string(a_geom->name.c_str()), ShapeSlot(skin.get()));
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        SetContentShapes(a_id, out);
        return out;
    }

    RE::SEX EffectiveSexFor(const std::string& a_id)
    {
        StoreLock lk;
        return EffectiveSex(a_id);
    }

    bool RegisterBoxById(const std::string& a_contentId, const std::string& a_tokenId)
    {
        StoreLock lk;
        // ROOT D: register under canonical ids so the registry key, the gender-map
        // lookup, and active-vs-catalog matching agree with the (canonical) config
        // side regardless of how this id was spelled at its source.
        std::string cid = a_contentId;
        CanonicalizeColonId(cid);
        std::string tid = a_tokenId;
        if (!tid.empty()) {
            CanonicalizeColonId(tid);
        }
        // v1.3.2 r2 hard admission (review P1-3): every registration path
        // funnels through here / RegisterArmaById / InjectArmaById - settings
        // load, co-save restore, natives, console, persist re-enable. The
        // upstream capture gates are UX; this is the enforcement line. On
        // refusal the configured id is KEPT (quarantine-lite): it stays in
        // settings/co-save, is simply never registered, one log line.
        const auto pol = CapturePolicySnapshot();
        std::string why;
        if (!IsContentAdmissible(cid, *pol, &why)) {
            SKSE::log::warn(
                "register: box content '{}' not admitted - {} (config kept, not registered)",
                cid, why);
            return false;
        }
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(cid, localID, plugin)) {
            return false;
        }
        const RE::SEX sex = EffectiveSex(cid);
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(localID, plugin, sex, *pol, m3p, m1p)) {
            return false;
        }
        const RE::FormID tokenForm = ResolveFormID(tid);
        Register(PlayerState(), cid, m3p, m1p, tid, tokenForm, sex);
        return true;
    }

    bool DefineBox(const std::string& a_contentId, const std::string& a_tokenId)
    {
        StoreLock lk;
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
        StoreLock lk;
        // Copy ids first - DetachSkinned mutates the registry via Unregister.
        auto& playerState = PlayerState();
        std::vector<std::string> ids;
        ids.reserve(playerState.items.size());
        for (const auto& it : playerState.items) {
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
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0;
        }
        // Record-driven, not a sweep. This used to walk both skeletons looking
        // for CostumeFW_* nodes; walking the actor's skeleton is what the second
        // crash generation proved unsafe (see DetachRecorded). Everything CEF
        // attached is in the registry with a NiPointer to it and to its parent,
        // so there is nothing a sweep could find that this misses.
        int total = 0;
        for (auto& state : g_actors) {
            for (auto& it : state.items) {
                if (it.holder3p) {
                    ++total;
                }
                if (it.holder1p) {
                    ++total;
                }
                DetachRecorded(it.parent3p, it.holder3p, &it.visual3p);
                DetachRecorded(it.parent1p, it.holder1p, &it.visual1p);
            }
            DetachRealBody(state);
        }
        SKSE::log::info("DetachAllInjected: removed {} recorded CostumeFW node(s)", total);
        return total;
    }

    void ListActive()
    {
        StoreLock lk;
        std::size_t count = 0;
        for (const auto& state : g_actors) count += state.items.size();
        SKSE::log::info("active: {} item(s), {} actor state(s)", count, g_actors.size());
        if (auto* c = RE::ConsoleLog::GetSingleton()) {
            c->Print("[CEF] active items:");
        }
        for (auto& state : g_actors) {
            auto* actor = ResolveActor(state);
            const char* name = actor ? actor->GetName() : "<unloaded>";
            for (const auto& it : state.items) {
                SKSE::log::info("  {}: {}", name, it.id);
                if (auto* c = RE::ConsoleLog::GetSingleton()) c->Print(it.id.c_str());
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

        // Misc-type HDPT whose model path OR editorID names a mouth.
        bool IsMouthPart(RE::BGSHeadPart* a_part)
        {
            if (!a_part || a_part->type.get() != RE::BGSHeadPart::HeadPartType::kMisc) {
                return false;
            }
            const auto lower = [](const char* a_s) {
                std::string m = a_s ? a_s : "";
                for (auto& ch : m) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch + 32);
                    }
                }
                return m;
            };
            if (lower(a_part->model.c_str()).find("mouth") != std::string::npos) {
                return true;
            }
            return lower(a_part->GetFormEditorID()).find("mouth") != std::string::npos;
        }

        // The player's mouth/teeth head part. The vanilla mouth usually is NOT a
        // top-level entry of the NPC's headParts array - it rides as an extraPart
        // of the face HDPT, or comes from the RACE's chargen defaults (2026-07-17
        // in-game: the watchdog stayed silently inert on a real teeth drop because
        // only top-level NPC entries were scanned). Scan all three tiers. nullptr
        // if none (a custom head can bake the mouth in) - watchdog stays inert.
        RE::BGSHeadPart* FindPlayerMouth(RE::TESNPC* a_base)
        {
            if (!a_base) {
                return nullptr;
            }
            const auto scan = [](RE::BGSHeadPart* a_part) -> RE::BGSHeadPart* {
                if (!a_part) {
                    return nullptr;
                }
                if (IsMouthPart(a_part)) {
                    return a_part;
                }
                for (auto* extra : a_part->extraParts) {
                    if (IsMouthPart(extra)) {
                        return extra;
                    }
                }
                return nullptr;
            };
            if (a_base->headParts) {
                for (std::int8_t i = 0; i < a_base->numHeadParts; ++i) {
                    if (auto* hit = scan(a_base->headParts[i])) {
                        return hit;
                    }
                }
            }
            auto* race = a_base->race;
            const int si = (a_base->GetSex() == RE::SEXES::kFemale) ? 1 : 0;
            if (race && race->faceRelatedData[si] && race->faceRelatedData[si]->headParts) {
                for (auto* part : *race->faceRelatedData[si]->headParts) {
                    if (auto* hit = scan(part)) {
                        return hit;
                    }
                }
            }
            return nullptr;
        }

        // True if any registered head part is a CEF pool part (CFW_*). The teeth-drop
        // bug only manifests while a CFW Misc head part shares the facegen head.
        bool AnyCfwHeadPart(RE::TESNPC* a_base)
        {
            if (!a_base || !a_base->headParts) {
                return false;
            }
            for (std::int8_t i = 0; i < a_base->numHeadParts; ++i) {
                auto* part = a_base->headParts[i];
                const char* ed = part ? part->GetFormEditorID() : nullptr;
                if (ed && std::string_view(ed).starts_with("CFW_")) {
                    return true;
                }
            }
            return false;
        }

        // True if a node named a_name exists anywhere under a_root (the facegen head
        // renames each head part's geometry to its editorID - HANDOVER §9-18).
        bool NodeNamePresent(RE::NiAVObject* a_root, const char* a_name)
        {
            if (!a_root || !a_name || !*a_name) {
                return false;
            }
            bool found = false;
            RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_obj) {
                if (a_obj->name == a_name) {
                    found = true;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return found;
        }
    }

    bool ReconcilePersistHeadParts(const std::vector<RE::BGSHeadPart*>& a_desired,
        const std::vector<RE::BGSHeadPart*>& a_pool)
    {
        StoreLock lk;
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

    // Rebuild count guard so a persistent failure can't loop DoReset3D (each retains
    // an FSMP physics arena). Reset once the mouth is confirmed present.
    std::atomic<int> g_mouthRestoreRetries{ 0 };

    // Teeth-drop watchdog. The engine's load-time facegen build can drop the mouth
    // from the assembled head (GetFaceNodeSkinned) when a persist head-carrier (a
    // Misc HDPT) shares it - the mouth stays in the headParts ARRAY but is absent
    // from the built head, so racemenu / a persist re-toggle (a clean rebuild after
    // the engine settled) brings it back. This does that automatically: if a CFW
    // head part is registered and the mouth's geometry is missing from the facegen
    // head, one clean DoReset3D restores it. Main thread only. (HANDOVER teeth bug.)
    // The race loser's rescue (field-proven 2026-07-31 on the owner's rig,
    // the "newest persist entry never gets physics" mechanism): an item
    // injected BEFORE its carrier bones finish merging binds fully static,
    // burns its 4 retries in ~4s, gets parked - and then the merge lands
    // with nobody left to try again. The dead-bind watchdog only watches
    // bonds that EXISTED, Reconcile idempotent-skips the intact holder, and
    // RequestRebindRetry refuses parked ids; measured end state: bones
    // merged as Head_0000000F, prefix verified inside the carrier NIF, item
    // still 0-of-237 static forever. The park always promised "a carrier
    // rebuild or a 3D rebuild re-arms it" - this IS that re-arm, called from
    // the settle points where new bones can actually appear. One shot per
    // trigger: a hopeless item (carrier truly boneless) re-parks after its 4
    // tries, so this cannot recreate the 2026-07-28 endless-retry churn
    // (35 rounds/2.5min) that the park exists to prevent.
    std::vector<std::string> InvisDiag()
    {
        // One-shot comparison for the invisibility-propagation work
        // (CEF_Invisibility_Propagation_Audit.md section 6): the user runs this
        // BEFORE invisibility, DURING the fade, at FULL invisibility and AFTER
        // dispel; diffing the reference (engine-equipped biped parts, reached
        // through the biped RECORDS - no skeleton search) against the CEF
        // holders tells us which channel the engine actually uses (shader
        // alpha / material alpha / refraction flags / fade node) and therefore
        // which sync strategy to implement. Read-only; user-initiated.
        StoreLock lk;
        std::vector<std::string> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }
        {
            int q = QuarantineSweep();  // containment first, as everywhere
            if (q > 0) {
                out.push_back(std::format("(quarantined {} broken holder(s) first)", q));
            }
        }
        const float invisAV = player->AsActorValueOwner()->GetActorValue(
            RE::ActorValue::kInvisibility);
        out.push_back(std::format("# Actor  invisibilityAV={:.2f}", invisAV));
        for (int fp = 0; fp <= 1; ++fp) {
            auto* root = player->Get3D(fp != 0);
            auto* fade = root ? root->AsFadeNode() : nullptr;
            out.push_back(std::format("root {} : fadeNode={} currentFade={:.3f}",
                fp ? "1p" : "3p", fade ? "yes" : "no",
                fade ? fade->GetRuntimeData().currentFade : -1.0f));
        }
        const auto describeGeoms = [&out](RE::NiAVObject* a_sub, const char* a_tag, int a_max) {
            if (!a_sub) {
                return;
            }
            int n = 0;
            RE::BSVisit::TraverseScenegraphGeometries(a_sub,
                [&](RE::BSGeometry* a_geom) {
                    if (n >= a_max) {
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    auto& rt = a_geom->GetGeometryRuntimeData();
                    auto* prop = ::netimmerse_cast<RE::BSShaderProperty*>(
                        rt.properties[RE::BSGeometry::States::kEffect].get());
                    float propAlpha = -1.0f;
                    float matAlpha = -1.0f;
                    std::uint64_t flags = 0;
                    const char* type = "none";
                    if (prop) {
                        propAlpha = prop->alpha;
                        flags = prop->flags.underlying();
                        if (auto* ls = ::netimmerse_cast<RE::BSLightingShaderProperty*>(prop)) {
                            type = "lighting";
                            if (ls->material) {
                                matAlpha = static_cast<RE::BSLightingShaderMaterialBase*>(
                                    ls->material)->materialAlpha;
                            }
                        } else {
                            type = "other";
                        }
                    }
                    const bool refr = (flags & (1ull << 15)) != 0;      // kRefraction
                    const bool tmpRefr = (flags & (1ull << 2)) != 0;    // kTempRefraction
                    // fadeNode + effectData are the audit's two unverified
                    // channels: SetEffectShaderData is the engine's own
                    // per-geometry link to an active TESEffectShader, and the
                    // first capture proved alpha/flags never move.
                    const void* fadeNodePtr = prop ? prop->fadeNode : nullptr;
                    const void* effectDataPtr = prop ? prop->effectData.get() : nullptr;
                    out.push_back(std::format(
                        "  [{}] '{}' shader={} propAlpha={:.3f} matAlpha={:.3f} "
                        "refr={} tmpRefr={} fadeNode={} effectData={} flags={:#018x}",
                        a_tag, a_geom->name.c_str(), type, propAlpha, matAlpha,
                        refr ? 1 : 0, tmpRefr ? 1 : 0, fadeNodePtr, effectDataPtr, flags));
                    ++n;
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        };
        // Reference: engine-equipped parts via the biped RECORDS (partClone),
        // never a skeleton search.
        constexpr std::size_t kBipedTotal = 42;  // BIPED_OBJECTS::kTotal
        const auto& biped = player->GetBiped1(false);
        int shown = 0;
        if (biped) {
            out.push_back("# Normal equip (biped partClone, reference)");
            for (std::size_t i = 0; i < kBipedTotal && shown < 3; ++i) {
                const auto& part = biped->objects[i].partClone;
                if (!part) {
                    continue;
                }
                out.push_back(std::format(" biped[{}] '{}':", i, part->name.c_str()));
                describeGeoms(part.get(), "equip", 4);
                ++shown;
            }
        }
        if (!shown) {
            out.push_back(
                "# Normal equip: NOTHING worn - equip one normal armor piece so the "
                "diag has an engine-side reference");
        }
        // Active shader effects targeting the player: the suspected channel.
        // Correlate each effect's BSEffectShaderData pointer with the
        // per-geometry effectData column above.
        out.push_back("# Active ShaderReferenceEffects on the player");
        if (auto* lists = RE::ProcessLists::GetSingleton()) {
            int found = 0;
            lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect& a_effect) {
                auto targetRef = a_effect.target.get();
                if (targetRef && targetRef.get() == player) {
                    const auto* shader = a_effect.effectData;
                    out.push_back(std::format(
                        " effect: efsh={:08X} shaderData={} finished={} targetRoot={}",
                        shader ? shader->GetFormID() : 0,
                        static_cast<const void*>(a_effect.effectShaderData),
                        a_effect.finished ? 1 : 0,
                        static_cast<const void*>(a_effect.targetRoot.get())));
                    ++found;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (!found) {
                out.push_back(" (none)");
            }
        }
        // Visual shadow: which biped part each skeleton is following, and the
        // state last copied from it. A late-injected costume is correct when its
        // geometry below shows the SAME (tmpRefr, effectData) this line reports.
        out.push_back("# CEF visual shadow (reference-follow state)");
        {
            bool anyRef = false;
            for (auto& state : g_actors) {
                auto* actor = ResolveActor(state);
                const char* who = state.isPlayer ? "player" :
                    (actor && actor->GetName() && *actor->GetName() ? actor->GetName() : "npc");
                for (int fp = 0; fp <= 1; ++fp) {
                    const auto& slot = fp ? state.visualRef1p : state.visualRef3p;
                    if (!slot.geom) {
                        continue;
                    }
                    anyRef = true;
                    out.push_back(std::format(
                        " {} {}: ref=biped[{}] '{}' applied tmpRefr={} effectData={}",
                        who, fp ? "1p" : "3p", slot.bipedIndex, slot.geom->name.c_str(),
                        slot.last.tmpRefr ? 1 : 0, slot.last.effectData));
                }
            }
            if (!anyRef) {
                out.push_back(
                    " (no reference taken - nothing injected, or no equipped biped part "
                    "to follow)");
            }
        }
        out.push_back("# CEF injected holders");
        bool any = false;
        for (auto& state : g_actors) {
            auto* actor = ResolveActor(state);
            const char* who = state.isPlayer ? "player" :
                (actor && actor->GetName() && *actor->GetName() ? actor->GetName() : "npc");
            for (auto& it : state.items) {
                if (!it.holder3p) {
                    continue;
                }
                any = true;
                out.push_back(std::format(" holder '{}' ({}):", it.id, who));
                describeGeoms(it.holder3p.get(), "cef", 6);
            }
            if (state.realBodyHolder3p) {
                any = true;
                out.push_back(std::format(" holder realbody ({}):", who));
                describeGeoms(state.realBodyHolder3p.get(), "cef", 4);
            }
        }
        if (!any) {
            out.push_back(" (no CEF holders attached - show a box/persist item first)");
        }
        return out;
    }

    void RearmStaticBinds(const char* a_reason)
    {
        StoreLock lk;
        int rearmed = 0;
        for (auto& state : g_actors) {
            std::vector<std::string> ids;
            for (const auto& it : state.items) {
                if (!it.holder3p && !it.holder1p) {
                    continue;  // not shown - nothing to rebind
                }
                const bool parked = g_staticDiagReported.contains(it.id);
                const bool allStatic = it.staticBones > 0 && it.fsmpBones == 0;
                if (parked || allStatic) {
                    ids.push_back(it.id);
                }
            }
            if (ids.empty()) {
                continue;
            }
            state.rebindRetryBudget = kRebindRetryBudget;
            for (const auto& id : ids) {
                g_staticDiagReported.erase(id);
                RequestRebindRetry(state, id);
                ++rearmed;
            }
        }
        if (rearmed) {
            SKSE::log::info("re-arming {} static item(s) for rebind ({})", rearmed, a_reason);
        }
    }

    void RestoreMouthIfDropped(const char* a_reason)
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = player ? player->GetActorBase() : nullptr;
        if (!player || !base) {
            return;
        }
        if (!AnyCfwHeadPart(base)) {
            g_mouthRestoreRetries = 0;
            return;  // no CFW head part registered -> the drop bug isn't in play
        }
        auto* mouth = FindPlayerMouth(base);
        if (!mouth) {
            // Loud, not silent: a CFW head part IS registered, so the drop bug is
            // in play and an unidentifiable mouth means the watchdog cannot help.
            SKSE::log::warn("mouth: no mouth head part identifiable (npc + extras + race "
                            "chargen scanned) - watchdog inert ({})",
                a_reason ? a_reason : "");
            return;
        }
        auto* faceNode = player->GetFaceNodeSkinned();
        if (!faceNode) {
            return;
        }
        const char* eid = mouth->GetFormEditorID();
        // Present = exact editorID match OR any node whose name contains "mouth"
        // (ci). A vanilla extra-part mouth's facegen geometry is NOT reliably
        // named by its HDPT editorID (2026-07-17 in-game: two rebuilds judged
        // "still dropped" - HANDOVER branch (b), presence-check false negative).
        // On a miss, collect the node names so the log shows what IS there.
        bool present = false;
        std::string seen;
        RE::BSVisit::TraverseScenegraphObjects(faceNode, [&](RE::NiAVObject* a_obj) {
            const char* n = a_obj->name.c_str();
            if (n && *n) {
                if (eid && a_obj->name == eid) {
                    present = true;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                std::string m = n;
                for (auto& ch : m) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch + 32);
                    }
                }
                if (m.find("mouth") != std::string::npos) {
                    present = true;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                if (seen.size() < 700) {
                    seen += n;
                    seen += " | ";
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        if (present) {
            g_mouthRestoreRetries = 0;
            return;  // mouth geometry present in the assembled head -> nothing to do
        }
        SKSE::log::info("mouth: facegen head nodes = {}", seen.empty() ? "<none>" : seen);
        if (g_mouthRestoreRetries.load() >= 2) {
            SKSE::log::warn("mouth: '{}' still dropped after {} rebuild(s) - giving up ({})",
                eid ? eid : "?", g_mouthRestoreRetries.load(), a_reason ? a_reason : "");
            return;
        }
        ++g_mouthRestoreRetries;
        // Escalation (2026-07-17 in-game: a plain DoReset3D reassembles the head
        // the SAME way and drops the mouth again - the node diag showed it
        // genuinely absent after every rebuild): from the 2nd attempt, PROMOTE
        // the extra-part/race-default mouth to an EXPLICIT NPC head part so the
        // assembly cannot lose it in the Misc-type race. Save-persistent like
        // RaceMenu; ChangeHeadPart APPENDS - it never replaces a same-type part
        // (C 9-16), so the CFW carrier parts stay.
        if (g_mouthRestoreRetries.load() >= 2 && !HasHeadPart(base, mouth)) {
            base->ChangeHeadPart(mouth);
            base->AddChange(RE::TESNPC::ChangeFlags::kFace);
            SKSE::log::info("mouth: promoted '{}' to an explicit head part", eid ? eid : "?");
        }
        SKSE::log::info("mouth: '{}' registered but ABSENT from facegen head ({}) - clean "
                        "rebuild #{} (persist head-carrier teeth drop)",
            eid ? eid : "?", a_reason ? a_reason : "", g_mouthRestoreRetries.load());
        if (!PersistHeadRebuildEnabled()) {  // same lever: no facegen rebuild at all
            SKSE::log::warn("mouth: rebuild SUPPRESSED (bPersistHeadRebuild=0)");
            return;
        }
        player->DoReset3D(false);
        g_headRebuildGraceUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        RunAfterDelayMs(1500, [] { Reconcile(); });
        RunAfterDelayMs(2600, [] { RestoreMouthIfDropped("recheck"); });
        RunAfterDelayMs(4000, [] { RearmStaticBinds("mouth-rebuild settle"); });
    }

    bool PlayerHasHeadPart(RE::BGSHeadPart* a_part)
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = player ? player->GetActorBase() : nullptr;
        return base && a_part && HasHeadPart(base, a_part);
    }

    bool SweepLegacyCfwHeadParts(const std::vector<RE::BGSHeadPart*>& a_currentPool)
    {
        StoreLock lk;
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

    // --- persist-CTD test harness (2026-07-28) -----------------------------
    // The crash needs a CEF holder node whose children array has a size and no
    // usable buffer. Rather than trying to rebuild the reporter's load order,
    // these two probe the hypothesis directly.

    // `cef arraytest` - synthetic, needs NO mods and no costume. Builds holder
    // nodes exactly the way InjectOnRoot does and reports the child array after
    // every attach, for the OLD capacity (0, engine-grown) and for pre-sized
    // ones. If Create(0) + AttachChild ever leaves size > 0 with an unusable
    // buffer, it shows up here in one line and the hypothesis is proven in the
    // console. If all rows stay walkable, the cause is elsewhere and this says
    // so just as clearly.
    std::vector<std::string> ChildArrayProbe()
    {
        StoreLock lk;
        std::vector<std::string> out;
        const auto snap = [](RE::NiNode* a_n) {
            const auto& k = a_n->GetChildren();
            return std::format("size={} cap={} data={}", k.size(), k.capacity(),
                static_cast<const void*>(k.begin()));
        };
        for (const std::uint16_t cap : { std::uint16_t{ 0 }, std::uint16_t{ 1 }, std::uint16_t{ 3 } }) {
            RE::NiPointer<RE::NiNode> holder{ RE::NiNode::Create(cap) };
            if (!holder) {
                out.push_back(std::format("Create({}) returned null", cap));
                continue;
            }
            holder->name = "CEF_ArrayProbe";
            out.push_back(std::format("Create({}) -> {}", cap, snap(holder.get())));
            for (int i = 0; i < 4; ++i) {
                RE::NiPointer<RE::NiNode> child{ RE::NiNode::Create(0) };
                if (!child) {
                    break;
                }
                child->name = std::format("probe_child_{}", i).c_str();
                holder->AttachChild(child.get(), true);  // firstAvail, as InjectOnRoot does
                const bool ok = ChildrenWalkable(holder.get());
                out.push_back(std::format("  +child{} -> {} walkable={}", i, snap(holder.get()),
                    ok ? "yes" : "NO  <-- REPRODUCED"));
            }
            // The exact read the crash took, but only when it is safe to try.
            if (ChildrenWalkable(holder.get())) {
                const RE::BSFixedString probe{ "probe_child_2" };
                auto* found = holder->GetObjectByName(probe);
                out.push_back(std::format("  GetObjectByName('probe_child_2') -> {}",
                    static_cast<const void*>(found)));
            } else {
                out.push_back("  GetObjectByName SKIPPED - walking this array is the crash");
            }
        }
        return out;
    }

    // `cef slottest` - synthetic probe for the SLOT corruption family (the
    // bone-index-shaped values; BUGREPORT 2026-07-30, uint16-pattern section).
    // Builds a holder + children the way InjectOnRoot does, plants the observed
    // poison value 0x0001000500050005 into one child slot by raw write (the
    // real child's refcount is held externally the whole time), and verifies:
    // (a) PlausibleObjectPtr rejects the value, (b) the guarded walk skips the
    // slot without dereferencing it, (c) ChildrenWalkable still accepts the
    // array itself (only a slot is poisoned, the buffer is fine), then
    // (d) restores the real pointer BEFORE teardown so no destructor ever sees
    // the poison (deliberate-corruption rules, INVESTIGATION 2026-07-29 §A-5).
    std::vector<std::string> SlotCorruptionProbe()
    {
        StoreLock lk;
        std::vector<std::string> out;
        constexpr std::uint64_t kPoison = 0x0001000500050005ull;

        // Boundary matrix first - pure value calls, nothing is planted anywhere.
        // The PASS row is deliberate: an aligned, canonical junk address is the
        // guard's DESIGNED limit (it must not be planted into a live slot - the
        // walk would pass it and the dereference behind it would fault).
        struct BoundaryCase
        {
            std::uint64_t v;
            bool expectPlausible;
            const char* why;
        };
        static constexpr BoundaryCase kCases[] = {
            { 0x1, false, "observed poison (holder _data)" },
            { 0x0001000200020002ull, false, "observed poison (mid-save hit)" },
            { 0x0001000500050005ull, false, "observed poison (old-walk hit)" },
            { 0x8000, false, "aligned but below the null-page floor" },
            { 0xFFFF800000000000ull, false, "aligned but non-canonical" },
            { 0x10000, true, "aligned canonical floor - the guard's PASS limit" },
        };
        bool matrixOk = true;
        for (const auto& c : kCases) {
            const bool got = PlausibleObjectPtr(reinterpret_cast<const void*>(c.v));
            if (got != c.expectPlausible) {
                matrixOk = false;
            }
            out.push_back(std::format("  {:#018x} -> {} (expect {}) {} [{}]", c.v,
                got ? "pass" : "reject", c.expectPlausible ? "pass" : "reject",
                got == c.expectPlausible ? "ok" : "<-- FAIL", c.why));
        }
        out.push_back(std::format("boundary matrix: {}", matrixOk ? "ok" : "FAILED"));

        RE::NiPointer<RE::NiNode> holder{ RE::NiNode::Create(0) };
        if (!holder) {
            out.push_back("Create(0) returned null");
            return out;
        }
        holder->name = "CEF_SlotProbe";
        std::vector<RE::NiPointer<RE::NiNode>> kids;  // external refs: teardown safety
        for (int i = 0; i < 4; ++i) {
            RE::NiPointer<RE::NiNode> child{ RE::NiNode::Create(0) };
            if (!child) {
                out.push_back(std::format("child Create #{} returned null", i));
                return out;
            }
            child->name = std::format("slotprobe_child_{}", i).c_str();
            holder->AttachChild(child.get(), true);
            kids.push_back(child);
        }
        auto& arr = holder->GetChildren();
        out.push_back(std::format("built: size={} cap={} data={}", arr.size(),
            arr.capacity(), static_cast<const void*>(arr.begin())));
        if (arr.size() < 3 || !ChildrenWalkable(holder.get())) {
            out.push_back("unexpected build state - aborting before any poison");
            return out;
        }

        auto** slots = reinterpret_cast<RE::NiAVObject**>(arr.begin());
        RE::NiAVObject* real = slots[2];
        slots[2] = reinterpret_cast<RE::NiAVObject*>(kPoison);
        out.push_back(std::format("slot[2] poisoned with {:#x}", kPoison));

        out.push_back(std::format("  PlausibleObjectPtr -> {}",
            PlausibleObjectPtr(slots[2]) ? "ACCEPTED  <-- FAIL" : "rejected (ok)"));
        out.push_back(std::format("  ChildrenWalkable(holder) -> {}",
            ChildrenWalkable(holder.get()) ? "yes (ok - buffer itself is fine)"
                                           : "NO  <-- unexpected"));

        int visited = 0, skipped = 0;
        for (auto& child : holder->GetChildren()) {
            auto* c = child.get();
            if (c && !PlausibleObjectPtr(c)) {
                NoteBadSlot("slottest", holder.get(), c);
                ++skipped;
                continue;
            }
            if (c) {
                ++visited;
            }
        }
        out.push_back(std::format("  guarded walk: {} visited, {} skipped -> {}",
            visited, skipped, (skipped == 1 && visited == 3) ? "ok" : "UNEXPECTED"));

        slots[2] = real;  // restore BEFORE any release path can run
        out.push_back("slot[2] restored - teardown is safe");
        return out;
    }

    // `cef nodediag` - live side. Reports the child array of every CEF node on
    // the player, plus ANY node that fails the walkability guard. Run it right
    // after the operation that crashes: if a holder is already bad here, we have
    // caught the corruption without needing the crash.
    std::vector<std::string> ChildArrayScan()
    {
        StoreLock lk;
        std::vector<std::string> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            out.push_back("no player");
            return out;
        }
        int bad = 0, cef = 0;
        for (int fp = 0; fp <= 1; ++fp) {
            auto* root = player->Get3D(fp != 0);
            auto* rootNode = root ? root->AsNode() : nullptr;
            if (!rootNode) {
                continue;
            }
            const char* tag = fp ? "1p" : "3p";
            std::vector<RE::NiNode*> stack{ rootNode };
            while (!stack.empty()) {
                auto* node = stack.back();
                stack.pop_back();
                const std::string_view nm{ node->name.c_str() };
                const bool ours = nm.starts_with(kNodePrefix) || nm == kRealBodyNode;
                const bool walkable = ChildrenWalkable(node);
                if (!walkable || ours) {
                    const auto& k = node->GetChildren();
                    out.push_back(std::format("[{}] {}{} size={} cap={} data={}", tag, nm,
                        walkable ? "" : "  <<< UNWALKABLE", k.size(), k.capacity(),
                        static_cast<const void*>(k.begin())));
                    if (ours) {
                        ++cef;
                    }
                    if (!walkable) {
                        ++bad;
                    }
                }
                if (!walkable) {
                    continue;  // never descend into the thing that crashes
                }
                for (auto& child : node->GetChildren()) {
                    if (auto* c = child.get(); c) {
                        if (!PlausibleObjectPtr(c)) {
                            NoteBadSlot("nodediag", node, c);
                            ++bad;
                            continue;
                        }
                        if (auto* cn = c->AsNode()) {
                            stack.push_back(cn);
                        }
                    }
                }
            }
        }
        out.push_back(std::format("-- {} CEF node(s), {} unwalkable array(s)", cef, bad));
        return out;
    }

    // --- bone budget readout (Diagnostics page) -----------------------------
    // Measured, never guessed. There is no number we can honestly print as "the
    // limit": Bone Limit Extender (Nexus 177636, skyrimbonelimitfix.dll) ships
    // no config and states no constant, nifcarrier has no bone ceiling of its
    // own, and the ceiling that actually bites is FSMP's per-actor merge budget,
    // which depends on the whole load order. So report what IS measurable - what
    // CEF asked for, what it got, and who else is on the actor - and let the gap
    // be the signal.
    //
    // In-game 2026-07-28: with SOFTBODY enabled, FSMP merged 8+94 bones and NONE
    // of them were CEF's; with it disabled the same character merged 2226+1227
    // CEF carrier bones. All-or-nothing per carrier, so "asked 3453 / merged 0"
    // is exactly the line a user in that state needs to see.
    BoneBudgetInfo BoneBudget()
    {
        StoreLock lk;
        BoneBudgetInfo out{};
        for (const auto& it : PlayerState().items) {
            out.askedBones += it.fsmpBones + it.staticBones;
            out.boundBones += it.fsmpBones;
            out.staticBones += it.staticBones;
            if (it.maxShapeBones > out.worstShapeBones) {
                out.worstShapeBones = it.maxShapeBones;
                out.worstShapeContent = it.id;
            }
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player ? player->Get3D(false) : nullptr;
        auto* rootNode = root ? root->AsNode() : nullptr;
        if (!rootNode) {
            return out;
        }
        // nifcarrier stamps every CEF content bone with "C<8hex>_"
        // (NifCarrierCore ContentNamePrefix) - that is how we tell our merged
        // bones from another mod's inside the same skeleton.
        const auto isCefBone = [](std::string_view a_suffix) {
            if (a_suffix.size() < 10 || a_suffix.front() != 'C' || a_suffix[9] != '_') {
                return false;
            }
            for (std::size_t i = 1; i < 9; ++i) {
                if (std::isxdigit(static_cast<unsigned char>(a_suffix[i])) == 0) {
                    return false;
                }
            }
            return true;
        };
        std::set<std::string> groups, cefGroups;
        std::vector<RE::NiNode*> stack{ rootNode };
        while (!stack.empty()) {
            auto* node = stack.back();
            stack.pop_back();
            bool head = false;
            std::uint32_t id = 0;
            std::string_view suffix;
            if (ParseRenamedBone(std::string_view(node->name.c_str()), head, id, suffix)) {
                const std::string grp = std::format("{}{:08X}", head ? "Head_" : "Armor_", id);
                groups.insert(grp);
                ++out.mergedTotal;
                if (isCefBone(suffix)) {
                    ++out.mergedCef;
                    cefGroups.insert(grp);
                }
            }
            if (!ChildrenWalkable(node)) {
                continue;
            }
            for (auto& child : node->GetChildren()) {
                if (auto* c = child.get(); c) {
                    if (!PlausibleObjectPtr(c)) {
                        NoteBadSlot("bone census", node, c);
                        continue;
                    }
                    if (auto* cn = c->AsNode()) {
                        stack.push_back(cn);
                    }
                }
            }
        }
        out.mergeGroups = static_cast<std::uint32_t>(groups.size());
        out.cefMergeGroups = static_cast<std::uint32_t>(cefGroups.size());
        return out;
    }

    void RebuildPlayerHead()
    {
        StoreLock lk;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D(false)) {
            return;  // no 3D yet - the engine builds the head with the current set
        }
        if (!PersistHeadRebuildEnabled()) {  // F2 diagnostic lever (ini)
            SKSE::log::warn("persist head: DoReset3D SUPPRESSED (bPersistHeadRebuild=0) - "
                            "diagnostic mode, persist SMP physics will not attach");
            return;
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
        // ...then check the mouth survived this rebuild (belt-and-suspenders; the
        // runtime re-toggle path is usually clean, but the load path is not).
        RunAfterDelayMs(2600, [] { RestoreMouthIfDropped("post-rebuild"); });
        // ...then rescue the race losers: the 1500ms Reconcile above can still
        // run BEFORE FSMP finishes merging the new carrier bones (measured
        // 2026-07-31), leaving items fully static with their retries burned.
        // Two settle points because merge latency scales with the load order -
        // the second is silent when the first already converged.
        RunAfterDelayMs(4000, [] { RearmStaticBinds("head-rebuild settle"); });
        RunAfterDelayMs(12000, [] { RearmStaticBinds("head-rebuild late settle"); });
    }

    void RequestPersistHeadRebuild(const char* a_reason)
    {
        StoreLock lk;
        // Debounce: coalesce a burst of ApplyPersistCarrier-driven rebuild
        // requests into ONE DoReset3D ~500ms after the LAST request, so FSMP
        // rebuilds the wig physics once (not per settings-write / sync / pass).
        ++g_persistDiag.headRebuildRequested;
        // Info, not debug: this is the entry to the F2 chain and has to be visible
        // in a user-supplied log (debug lines are filtered out - logger.h).
        SKSE::log::info("persist head: rebuild requested ({}) - DoReset3D in ~500ms",
            a_reason ? a_reason : "");
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
        StoreLock lk;
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
        StoreLock lk;
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
        // ROOT E [1496]: ChangeHeadPart REPLACES the same-type part and is save-
        // persistent (like RaceMenu). Restrict this debug lever to Hair so it can't
        // irreversibly swap the player's face/eyes, and log the displaced hair's
        // FormID so it can be re-applied to revert.
        if (hdpt->type.get() != RE::BGSHeadPart::HeadPartType::kHair) {
            SKSE::log::error("hair PoC: refusing - {:X}:{} is head-part type {} (Hair only)",
                localID, plugin, static_cast<int>(hdpt->type.get()));
            if (auto* console = RE::ConsoleLog::GetSingleton()) {
                console->Print("CostumeFW: cef hair refused - not a Hair head part");
            }
            return false;
        }
        for (std::int32_t i = 0; i < base->numHeadParts; ++i) {
            auto* cur = base->headParts ? base->headParts[i] : nullptr;
            if (cur && cur->type.get() == RE::BGSHeadPart::HeadPartType::kHair) {
                SKSE::log::info("hair PoC: displacing current hair '{}' ({:08X}) - re-apply it or reload to revert",
                    cur->GetFormEditorID(), cur->GetFormID());
            }
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
        StoreLock lk;
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
            // Carry the parent NAME down the walk instead of reading obj->parent:
            // these are exactly the FSMP-renamed nodes whose ->parent can point at
            // a freed node of a retired merge generation (the confirmed CTD class -
            // see DetachNamedFrom). A diagnostic must not be the thing that crashes.
            std::vector<std::pair<RE::NiAVObject*, std::string>> stack{ { a_root, "<root>" } };
            while (!stack.empty()) {
                auto [obj, parentName] = std::move(stack.back());
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
                    // World position: a merged-but-sane bone sits near its anchor
                    // (head ~ pelvis height); one at the origin / thousands of units
                    // away is the "stretched everywhere" failure signature.
                    const auto& wp = obj->world.translate;
                    SKSE::log::info("  headdiag[{}] {} (parent '{}') world=({:.1f},{:.1f},{:.1f})",
                        a_tag, nm, parentName, wp.x, wp.y, wp.z);
                }
                if (auto* node = obj->AsNode(); node && ChildrenWalkable(node)) {
                    const std::string myName{ node->name.c_str() };
                    for (auto& child : node->GetChildren()) {
                        auto* c = child.get();
                        if (c && !PlausibleObjectPtr(c)) {
                            NoteBadSlot("headdiag", node, c);
                            continue;
                        }
                        stack.emplace_back(c, myName);
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

    namespace
    {
        // Unchecked primitive (re-review P2-1): base admission is owned by the
        // wrapper, while selected-ARMA admission remains mandatory inside the
        // resolver. The SAME immutable policy generation is passed through.
        bool InjectArmaUnchecked(std::uint32_t a_localID, const std::string& a_plugin,
            const std::string& a_id, const policy::CapturePolicy& a_policy)
        {
            const RE::SEX sex = EffectiveSex(a_id);
            ModelRef m3p, m1p;
            if (!ResolveArmaModels(
                    a_localID, a_plugin, sex, a_policy, m3p, m1p)) {
                return false;
            }
            SKSE::log::info("InjectArma {:X}:{} 3p='{}' 1p='{}'",
                a_localID, a_plugin, m3p.nifPath, m1p.nifPath);
            auto& state = PlayerState();
            Register(state, a_id, m3p, m1p, {}, 0, sex);
            for (auto& it : state.items) {
                if (it.id == a_id) {
                    return InjectFor(state, it);
                }
            }
            return false;
        }
    }

    bool InjectArma(std::uint32_t a_localID, const std::string& a_plugin, const std::string& a_id)
    {
        StoreLock lk;
        // Public console/self-test entrance: admit the actual form identity,
        // not the caller's optional registry label.
        const auto pol = CapturePolicySnapshot();
        const std::string cid = policy::FormatColonId(a_localID, a_plugin);
        std::string why;
        if (!IsContentAdmissible(cid, *pol, &why)) {
            SKSE::log::warn(
                "register: inject '{}' not admitted - {} (not registered)", cid, why);
            return false;
        }
        return InjectArmaUnchecked(a_localID, a_plugin, a_id, *pol);
    }

    bool RegisterArmaById(const std::string& a_id)
    {
        StoreLock lk;
        // Co-save restore keeps unresolved/blocked ids via ROOT H. Canonicalize
        // the registry key, but never delete the configured/co-save entry.
        std::string cid = a_id;
        CanonicalizeColonId(cid);
        const auto pol = CapturePolicySnapshot();
        std::string why;
        if (!IsContentAdmissible(cid, *pol, &why)) {
            SKSE::log::warn(
                "register: persist content '{}' not admitted - {} (config kept, not registered)",
                cid, why);
            return false;
        }
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(cid, localID, plugin)) {
            return false;
        }
        const RE::SEX sex = EffectiveSex(cid);
        ModelRef m3p, m1p;
        if (!ResolveArmaModels(
                localID, plugin, sex, *pol, m3p, m1p)) {
            return false;
        }
        Register(PlayerState(), cid, m3p, m1p, {}, 0, sex);
        return true;
    }

    bool InjectArmaById(const std::string& a_id)
    {
        StoreLock lk;
        // Papyrus RegisterPersist entrance: one policy generation covers base
        // admission and the selected addon all the way into the primitive.
        std::string cid = a_id;
        CanonicalizeColonId(cid);
        const auto pol = CapturePolicySnapshot();
        std::string why;
        if (!IsContentAdmissible(cid, *pol, &why)) {
            SKSE::log::warn(
                "register: inject '{}' not admitted - {} (not registered)", cid, why);
            return false;
        }
        std::uint32_t localID = 0;
        std::string plugin;
        if (!ParseColonId(cid, localID, plugin)) {
            return false;
        }
        return InjectArmaUnchecked(localID, plugin, cid, *pol);
    }
    std::vector<ActiveItemInfo> ActiveSnapshot()
    {
        StoreLock lk;
        std::vector<ActiveItemInfo> v;
        const auto& items = PlayerState().items;
        v.reserve(items.size());
        for (const auto& it : items) {
            v.push_back({ it.id, it.tokenId });
        }
        return v;
    }

    void ClearRegistry()
    {
        StoreLock lk;
        // Detach before dropping the entries. The attachment record IS the only
        // handle CEF has on its nodes now that nothing searches the scene graph,
        // so clearing the registry without detaching would strand whatever is
        // currently attached and let the next injection add a duplicate beside
        // it. (Reachable from the co-save revert, where the 3D is not always
        // rebuilt underneath us.)
        for (auto& state : g_actors) {
            for (auto& it : state.items) {
                DetachRecorded(it.parent3p, it.holder3p, &it.visual3p);
                DetachRecorded(it.parent1p, it.holder1p, &it.visual1p);
            }
            DetachRealBody(state);
            state.items.clear();
            state.bonePins.clear();
            state.rebindRetryIds.clear();
        }
        g_actors.resize(1);
        // resize(1) on an EMPTY vector default-constructs slot 0 as a non-player
        // state, breaking the "[0] is ALWAYS the player" invariant for the rest
        // of the session (ResolveActor on an empty handle returns null, so the
        // player would never reconcile again). Force the invariant either way.
        g_actors[0].isPlayer = true;
        g_actors[0].handle = {};
        RefreshNpcBindingsGate();
        // Fresh scene, fresh slate: stale baselines can't match anything, and a
        // poison-park is a per-scene verdict (the stomping neighbor may be gone).
        g_holderArrayBaseline.clear();
        g_quarantineStrikes.clear();
        g_poisonParked.clear();
    }

    void DetachSkinned(const std::string& a_id)
    {
        StoreLock lk;
        Diag::NoteExecutionThread("DetachSkinned");
        // ORDER IS LOAD-BEARING. DetachNodes reads the attachment record, and the
        // record lives ON the registry entry, so unregistering first destroys the
        // only handle CEF has and leaves the holder orphaned on the skeleton.
        // It did not matter while DetachNodes searched the scene graph by name;
        // it does now. Caught in-game 2026-07-29: toggling a persist entry
        // off -> on -> off left TWO holders per skeleton (nodediag went 40 -> 42,
        // with 000D6F:Aether Outfit.esp appearing twice on each root).
        auto& state = PlayerState();
        DetachNodes(state, a_id);
        Unregister(state, a_id);
        SKSE::log::debug("  detached {}", a_id);
    }

    void HideInjectedNodes(const std::string& a_id)
    {
        StoreLock lk;
        DetachNodes(PlayerState(), a_id);  // registry untouched: Reconcile re-injects
    }

    void RefreshGender(const std::string& a_id)
    {
        StoreLock lk;
        auto& state = PlayerState();
        for (auto& it : state.items) {
            if (it.id == a_id) {
                DetachNodes(state, a_id);             // drop old-sex node, keep registered
                it.resolvedSex = RE::SEXES::kNone;  // force Reconcile to re-resolve
                break;
            }
        }
        Reconcile();  // re-resolves on the sex mismatch and re-attaches
    }

    void InjectTestFromFile()
    {
        StoreLock lk;
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
            if (hex && !left.empty() && left.size() <= 8 && plugin.size() > 4) {
                std::uint32_t localID = 0;
                try {
                    localID = static_cast<std::uint32_t>(std::stoul(left, nullptr, 16));
                } catch (...) {
                    // ROOT E [1699]: an over-long hex prefix threw std::out_of_range
                    // inside this main-thread task and hard-crashed the game.
                    SKSE::log::error("test inject: bad FormID hex '{}'", left);
                    if (auto* console = RE::ConsoleLog::GetSingleton()) {
                        console->Print("CostumeFW: bad FormID in test txt");
                    }
                    return;
                }
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
        StoreLock lk;
        DetachSkinned("test");
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("CostumeFW: detached test NIF");
        }
    }
}
