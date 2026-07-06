#include "LoreBox.h"
#include "BoxStore.h"

#include "RE/B/BSScaleformTranslator.h"
#include "RE/G/GFxTranslator.h"

#include <Windows.h>

#include <cstring>
#include <string>

namespace CostumeFW
{
    namespace
    {
        // Our LoreBox keyword family: "LoreBox_CEFBox<slot>" (see CostumeFW_KID.ini).
        constexpr wchar_t kKeyPrefix[] = L"LoreBox_CEFBox";

        std::wstring Utf8ToWide(const std::string& a_utf8)
        {
            if (a_utf8.empty()) {
                return {};
            }
            const int len = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(),
                static_cast<int>(a_utf8.size()), nullptr, 0);
            if (len <= 0) {
                return {};
            }
            std::wstring out(static_cast<std::size_t>(len), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(),
                static_cast<int>(a_utf8.size()), out.data(), len);
            return out;
        }

        // If a_key is one of our "LoreBox_CEFBox<slot>" keys AND that box has
        // contents, fill a_out with the tooltip HTML and return true. Otherwise
        // return false (the caller falls through to the game translator, leaving
        // the text untranslated so LoreBox simply skips it).
        bool BuildLoreBoxText(const wchar_t* a_key, std::wstring& a_out)
        {
            if (!a_key) {
                return false;
            }
            if (*a_key == L'$') {  // tolerate an unexpected leading translation marker
                ++a_key;
            }
            const std::size_t plen = std::wcslen(kKeyPrefix);
            if (std::wcsncmp(a_key, kKeyPrefix, plen) != 0) {
                return false;
            }
            const wchar_t* p = a_key + plen;
            if (!*p) {
                return false;
            }
            int slot = 0;
            for (; *p; ++p) {
                if (*p < L'0' || *p > L'9') {
                    return false;  // not a pure "<prefix><number>" key
                }
                slot = slot * 10 + (*p - L'0');
            }
            const std::string contents = LoreBoxContentsForSlot(slot);
            if (contents.empty()) {
                return false;  // no box on this slot / empty -> leave untranslated
            }
            a_out = Utf8ToWide(
                "<font face='$EverywhereBoldFont'>Costume box contents:</font><br>" + contents);
            return !a_out.empty();
        }

        struct TranslatorHook
        {
            static void thunk(RE::BSScaleformTranslator* a_this,
                RE::GFxTranslator::TranslateInfo* a_info)
            {
                std::wstring html;
                if (a_info && BuildLoreBoxText(a_info->GetKey(), html)) {
                    a_info->SetResultHTML(html.c_str());
                    return;  // handled; do not call the game translator
                }
                func(a_this, a_info);  // everything else: normal translation
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void InstallLoreBoxHook()
    {
        REL::Relocation<std::uintptr_t> vtbl{ RE::BSScaleformTranslator::VTABLE[0] };
        TranslatorHook::func = vtbl.write_vfunc(0x2, TranslatorHook::thunk);
        SKSE::log::info("LoreBox: BSScaleformTranslator::Translate hook installed (vfunc 0x2)");
    }
}
