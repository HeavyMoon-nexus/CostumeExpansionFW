#pragma once

#include <Windows.h>

#include <fstream>
#include <string>

namespace CostumeFW
{
    // Write a_data to a_path via a sibling ".tmp" + atomic rename, so a CTD or a
    // process kill mid-write can never leave a truncated file at a_path. The old
    // truncate-then-overwrite could destroy CEF_settings.json (Codex review
    // 2026-07-05 A-1). MoveFileEx(REPLACE_EXISTING) is atomic on NTFS and is
    // hooked by MO2's usvfs like the rest of the Win32 file API.
    //
    // Lived in BoxStore.cpp until the ability registry needed the same
    // guarantee. The registry needs it MORE than the settings did: losing it
    // does not lose a preference, it loses which ability every save is pointing
    // at, and a half-written one cannot be told from a short one.
    inline bool WriteFileAtomic(const char* a_path, const std::string& a_data)
    {
        const std::string tmp = std::string(a_path) + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
            if (!f) {
                return false;
            }
            f << a_data;
            f.flush();
            if (!f.good()) {
                return false;
            }
        }
        if (!MoveFileExA(tmp.c_str(), a_path, MOVEFILE_REPLACE_EXISTING)) {
            SKSE::log::error("atomic write: MoveFileEx failed ({}) for {}", GetLastError(),
                a_path);
            DeleteFileA(tmp.c_str());
            return false;
        }
        return true;
    }
}
