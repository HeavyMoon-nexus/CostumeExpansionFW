#include "Cosave.h"
#include "BoxStore.h"
#include "SkinRebind.h"

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
            const auto count = static_cast<std::uint32_t>(persist.size());
            a_intfc->WriteRecordData(count);
            for (const auto& it : persist) {
                WriteString(a_intfc, it.id);
                WriteString(a_intfc, it.tokenId);  // always empty here (persist)
            }
            SKSE::log::info("cosave: saved {} persist item(s)", count);
        }

        void LoadCallback(SKSE::SerializationInterface* a_intfc)
        {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            int restored = 0;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kRecordActive) {
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
            });
        }

        void RevertCallback(SKSE::SerializationInterface*)
        {
            ClearRegistry();
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
