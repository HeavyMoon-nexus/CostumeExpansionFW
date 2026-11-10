#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>
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
    //
    // Atomic is not the same as durable, and a power cut or an OS crash is the
    // difference (review 2026-09-23 M2). NTFS journals metadata, not data: the
    // rename can reach the disk before the bytes it renamed, and what comes back
    // up is a file of the right length full of zeros - which then replaces the
    // .bak at the next clean-looking load. So the bytes are flushed before the
    // rename, the rename itself is written through, and a .tmp that did not make
    // it is deleted rather than left beside the real file.
    //
    // a_path is a narrow path in the ANSI codepage, like every std::ifstream in
    // CEF; the callers all pass ASCII constants. The "close the stream you are
    // reading before writing the same file" rule still holds: the rename needs
    // DELETE access on the destination, which an open MSVC ifstream denies.
    inline bool WriteFileAtomic(const char* a_path, const std::string& a_data)
    {
        const auto widen = [](const std::string& a_narrow) {
            std::wstring out;
            const int n = ::MultiByteToWideChar(CP_ACP, 0, a_narrow.data(),
                static_cast<int>(a_narrow.size()), nullptr, 0);
            if (n > 0) {
                out.resize(static_cast<std::size_t>(n));
                ::MultiByteToWideChar(CP_ACP, 0, a_narrow.data(),
                    static_cast<int>(a_narrow.size()), out.data(), n);
            }
            return out;
        };
        const std::wstring dst = widen(a_path);
        const std::wstring tmp = widen(std::string(a_path) + ".tmp");
        if (dst.empty() || tmp.empty()) {
            SKSE::log::error("atomic write: cannot convert the path {}", a_path);
            return false;
        }

        HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            SKSE::log::error("atomic write: cannot create the temp file ({}) for {}",
                ::GetLastError(), a_path);
            return false;
        }
        bool ok = true;
        DWORD err = 0;
        const char* p = a_data.data();
        std::size_t left = a_data.size();
        while (left > 0) {
            const DWORD chunk =
                static_cast<DWORD>(std::min<std::size_t>(left, std::size_t{ 1 } << 30));
            DWORD written = 0;
            if (!::WriteFile(h, p, chunk, &written, nullptr) || written == 0) {
                ok = false;
                err = ::GetLastError();
                break;
            }
            p += written;
            left -= written;
        }
        if (ok && !::FlushFileBuffers(h)) {
            ok = false;
            err = ::GetLastError();
        }
        ::CloseHandle(h);
        if (!ok) {
            SKSE::log::error("atomic write: writing the temp file failed ({}) for {}", err, a_path);
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        if (!::MoveFileExW(tmp.c_str(), dst.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            SKSE::log::error("atomic write: MoveFileEx failed ({}) for {}", ::GetLastError(),
                a_path);
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }
}
