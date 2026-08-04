#include "Cosave.h"
#include "BoxStore.h"
#include "SkinRebind.h"
#include "PublishStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CostumeFW
{
    namespace
    {
        constexpr std::uint32_t kSignature = 'CEFW';   // co-save unique id
        constexpr std::uint32_t kRecordActive = 'ACTV';
        constexpr std::uint32_t kVersion = 2;          // v2 adds the box token id
        // P2 (SMF): the hidden holding container's FormID, so an SMF-only session
        // can capture/return custody items without the MCM ever opening (the MCM
        // handoff becomes legacy adoption). Runtime refs are save-local - always
        // remapped through ResolveFormID on load.
        constexpr std::uint32_t kRecordStore = 'STOR';
        constexpr std::uint32_t kStoreVersion = 1;
        constexpr std::uint32_t kRecordPubState = 'PUBS';
        constexpr std::uint32_t kRecordPubBind = 'PUBB';
        constexpr std::uint32_t kPubVersion = 1;
        constexpr std::uint32_t kRecordNprState = 'NPRS';
        constexpr std::uint32_t kNprVersion = 1;
        constexpr std::uint32_t kMaxBindCount = 1024;

        void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            a_intfc->WriteRecordData(len);
            if (len > 0) {
                a_intfc->WriteRecordData(a_s.data(), len);
            }
        }

        // Corrupt-cosave guards: a damaged record must fail small, never drive
        // a giant allocation or a near-endless restore loop. Ids are
        // "XXXXXX:Plugin.esp" strings, so these bounds are generous.
        constexpr std::uint32_t kMaxStringLen = 1024;
        constexpr std::uint32_t kMaxItemCount = 4096;

        // ROOT H (border audit 2026-07-09): persist ids that FAILED to resolve at
        // this session's load (a content mod was temporarily disabled). Re-serialized
        // on save so a temporary disable doesn't permanently erase the per-save
        // activation - the next load with the mod back re-registers them. Rebuilt each
        // load; cleared on revert.
        std::vector<std::string> g_unresolvedActives;

        bool ReadStringChecked(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            std::uint32_t len = 0;
            if (a_intfc->ReadRecordData(len) != sizeof(len) || len > kMaxStringLen) {
                return false;
            }
            a_out.assign(len, '\0');
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len) == len;
        }

        void SaveCallback(SKSE::SerializationInterface* a_intfc)
        {
            // Persist ONLY per-save persist items (empty tokenId). Box defs are
            // global config owned by costume_boxes.json (BoxStore), not the
            // co-save - saving them here would double-register on load.
            std::vector<ActiveItemInfo> persist;
            for (const auto& it : ActiveSnapshot()) {
                if (it.tokenId.empty()) {
                    persist.push_back(it);
                }
            }
            if (!a_intfc->OpenRecord(kRecordActive, kVersion)) {
                SKSE::log::error("cosave: OpenRecord failed");
                return;
            }
            const auto count = static_cast<std::uint32_t>(persist.size() + g_unresolvedActives.size());
            a_intfc->WriteRecordData(count);
            for (const auto& it : persist) {
                WriteString(a_intfc, it.id);
                WriteString(a_intfc, it.tokenId);  // always empty here (persist)
            }
            // ROOT H: carry this session's unresolved actives so a temporary content-mod
            // disable does not silently erase them from the save.
            for (const auto& id : g_unresolvedActives) {
                WriteString(a_intfc, id);
                WriteString(a_intfc, "");  // persist (token-less)
            }
            SKSE::log::info("cosave: saved {} persist item(s) ({} unresolved carried)",
                count, g_unresolvedActives.size());

            // Hidden-store FormID (0 = none this save; still written so a revert
            // to "no store" round-trips).
            if (a_intfc->OpenRecord(kRecordStore, kStoreVersion)) {
                const std::uint32_t store = StoreFormId();
                a_intfc->WriteRecordData(store);
            } else {
                SKSE::log::error("cosave: OpenRecord(STOR) failed");
            }

            const auto states = PubStatesForSave();
            if (a_intfc->OpenRecord(kRecordPubState, kPubVersion)) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(states.size()));
                for (const auto& state : states) {
                    a_intfc->WriteRecordData(state.pubSlot);
                    a_intfc->WriteRecordData(static_cast<std::uint8_t>(state.hidden ? 1 : 0));
                }
            }
            const auto bindings = PubBindingsForSave();
            if (a_intfc->OpenRecord(kRecordPubBind, kPubVersion)) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(bindings.size()));
                for (const auto& binding : bindings) {
                    a_intfc->WriteRecordData(binding.pubSlot);
                    a_intfc->WriteRecordData(binding.actorFormID);
                    a_intfc->WriteRecordData(binding.flags);
                }
            }
            const auto npr = NprAssignmentsForSave();
            if (a_intfc->OpenRecord(kRecordNprState, kNprVersion)) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(npr.size()));
                for (const auto& item : npr) {
                    a_intfc->WriteRecordData(item.poolSlot);
                    a_intfc->WriteRecordData(item.actorFormID);
                    a_intfc->WriteRecordData(static_cast<std::uint32_t>(item.contents.size()));
                    for (const auto& id : item.contents) WriteString(a_intfc, id);
                }
            } else {
                SKSE::log::error("cosave: OpenRecord(NPRS) failed");
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_intfc)
        {
            g_unresolvedActives.clear();  // ROOT H: rebuilt from this load's failures
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            int restored = 0;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type == kRecordStore) {
                    std::uint32_t saved = 0;
                    if (a_intfc->ReadRecordData(saved) == sizeof(saved) && saved != 0) {
                        RE::FormID resolved = 0;
                        if (a_intfc->ResolveFormID(saved, resolved) && resolved != 0) {
                            RestoreStoreFormId(resolved);
                            SKSE::log::info("cosave: hidden store {:08X} restored", resolved);
                        } else {
                            SKSE::log::warn("cosave: hidden store {:08X} did not resolve", saved);
                        }
                    }
                    continue;
                }
                if (type == kRecordPubState) {
                    std::uint32_t count = 0;
                    if (a_intfc->ReadRecordData(count) != sizeof(count) || count > kMaxBindCount)
                        continue;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        std::uint8_t slot = 0, flags = 0;
                        if (a_intfc->ReadRecordData(slot) != sizeof(slot) ||
                            a_intfc->ReadRecordData(flags) != sizeof(flags)) break;
                        RestorePubState(slot, (flags & 1) != 0);
                    }
                    continue;
                }
                if (type == kRecordPubBind) {
                    std::uint32_t count = 0;
                    if (a_intfc->ReadRecordData(count) != sizeof(count) || count > kMaxBindCount)
                        continue;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        std::uint8_t slot = 0, flags = 0;
                        RE::FormID saved = 0, resolved = 0;
                        if (a_intfc->ReadRecordData(slot) != sizeof(slot) ||
                            a_intfc->ReadRecordData(saved) != sizeof(saved) ||
                            a_intfc->ReadRecordData(flags) != sizeof(flags)) break;
                        if (a_intfc->ResolveFormID(saved, resolved) && resolved)
                            RestorePubBinding(slot, resolved, flags);
                        else
                            CarryUnresolvedPubBinding(slot, saved, flags);
                    }
                    continue;
                }
                if (type != kRecordActive && type != kRecordNprState) {
                    continue;
                }
                if (type == kRecordNprState) {
                    std::uint32_t count = 0;
                    if (a_intfc->ReadRecordData(count) != sizeof(count) || count > kMaxBindCount)
                        continue;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        std::uint8_t slot = 0;
                        RE::FormID saved = 0, resolved = 0;
                        std::uint32_t contentCount = 0;
                        if (a_intfc->ReadRecordData(slot) != sizeof(slot) ||
                            a_intfc->ReadRecordData(saved) != sizeof(saved) ||
                            a_intfc->ReadRecordData(contentCount) != sizeof(contentCount) ||
                            contentCount > kMaxItemCount) {
                            SKSE::log::error("cosave: corrupt NPRS assignment {}/{}", i, count);
                            break;
                        }
                        std::vector<std::string> contents;
                        bool valid = true;
                        for (std::uint32_t j = 0; j < contentCount; ++j) {
                            std::string id;
                            if (!ReadStringChecked(a_intfc, id)) {
                                valid = false;
                                break;
                            }
                            contents.push_back(std::move(id));
                        }
                        if (!valid) break;
                        if (a_intfc->ResolveFormID(saved, resolved) && resolved) {
                            auto* form = RE::TESForm::LookupByID(resolved);
                            auto* actor = form ? form->As<RE::Actor>() : nullptr;
                            const bool female = actor && actor->GetActorBase() &&
                                actor->GetActorBase()->GetSex() == RE::SEXES::kFemale;
                            RestoreNprAssignment(slot, resolved, std::move(contents), female);
                        } else {
                            CarryUnresolvedNprAssignment(slot, saved, std::move(contents), false);
                        }
                    }
                    continue;
                }
                std::uint32_t count = 0;
                if (a_intfc->ReadRecordData(count) != sizeof(count) || count > kMaxItemCount) {
                    SKSE::log::error("cosave: corrupt record (count={}) - record skipped", count);
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::string id;
                    std::string tokenId;
                    if (!ReadStringChecked(a_intfc, id) ||
                        (version >= 2 && !ReadStringChecked(a_intfc, tokenId))) {
                        SKSE::log::error("cosave: corrupt record at item {}/{} - rest skipped", i, count);
                        break;
                    }
                    const bool ok = tokenId.empty() ? RegisterArmaById(id)
                                                     : RegisterBoxById(id, tokenId);
                    if (ok) {
                        ++restored;
                    } else if (tokenId.empty()) {
                        // ROOT H: keep an unresolved persist id (its content mod may be
                        // temporarily off) so the next save doesn't erase it.
                        g_unresolvedActives.push_back(id);
                        SKSE::log::warn("cosave: '{}' unresolved - carried for next save", id);
                    } else {
                        SKSE::log::warn("cosave: could not restore '{}'", id);
                    }
                }
            }
            SKSE::log::info("cosave: restored {} item(s)", restored);
            // Reconcile once the player 3D is ready (next frame), then bring the
            // carrier manifest's persist fragment in line with THIS save's active
            // set (M2, CEF_STATE_SCOPE.md §3/§5 - a character switch changes the
            // set; the compare-skip keeps same-character reloads free).
            SKSE::GetTaskInterface()->AddTask([] {
                Reconcile();
                SyncPersistManifest();
                ReapplyNpcBindings();
            });
        }

        void RevertCallback(SKSE::SerializationInterface*)
        {
            ClearRegistry();
            ClearNpcBindings();
            g_unresolvedActives.clear();
            // The hidden store is per-save: clear at every load START so a save
            // without a STOR record can never inherit the previous save's store
            // (ROOT A cross-save protection, moved here from the kPostLoadGame
            // task - which ran AFTER LoadCallback and wiped the restored id).
            RestoreStoreFormId(0);
            SKSE::log::info("cosave: reverted (registry cleared)");
        }
    }

    void InstallSerialization()
    {
        auto* ser = SKSE::GetSerializationInterface();
        ser->SetUniqueID(kSignature);
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
        SKSE::log::info("serialization installed");
    }
}
