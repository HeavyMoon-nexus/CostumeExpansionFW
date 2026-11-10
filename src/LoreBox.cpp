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
        // Our two LoreBox keyword families. BOTH name a box by its CARRIER KEY:
        //
        //   LoreBox_CEFBox55            generation 0, whose carrier key IS "Box55"
        //   LoreBox_CEFTok_BP01_000800  generation 1 and up
        //
        // The first is the spelling that shipped in CostumeFW_KID.ini and stays
        // exactly as it is - an alias, not a legacy path - because generation
        // 0's carrier key was deliberately kept as "Box<slot>" (PLAN §3.4). The
        // second is what the pool generator writes into
        // CostumeFW_BoxPoolN_KID.ini, one keyword per TOKEN, because a biped
        // slot stopped naming one box in v1.6.4 and a slot-keyed tooltip would
        // have shown whichever of that slot's boxes CEF found first.
        constexpr wchar_t kSlotPrefix[] = L"LoreBox_CEFBox";
        constexpr wchar_t kTokenPrefix[] = L"LoreBox_CEFTok_";
        // Carrier keys are at most 64 characters of [A-Za-z0-9_] - the alphabet
        // the generator and nifcarrier's IsSafeCarrierKey agree on.
        constexpr std::size_t kMaxCarrierKey = 64;

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

        // The carrier key one of our keyword names refers to, or false when the
        // name is not ours. Both families land in the same key space, so the
        // lookup that follows is one lookup rather than two code paths.
        bool CarrierKeyFromKeyword(const wchar_t* a_key, std::string& a_out)
        {
            const auto narrow = [](const wchar_t* a_p, std::string& a_dst) {
                for (; *a_p; ++a_p) {
                    if (a_dst.size() >= kMaxCarrierKey) {
                        return false;
                    }
                    const wchar_t c = *a_p;
                    const bool ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                                    (c >= L'a' && c <= L'z') || c == L'_';
                    if (!ok) {
                        return false;
                    }
                    a_dst.push_back(static_cast<char>(c));
                }
                return true;
            };
            if (const std::size_t plen = std::wcslen(kTokenPrefix);
                std::wcsncmp(a_key, kTokenPrefix, plen) == 0) {
                std::string key;
                if (!narrow(a_key + plen, key) || key.empty()) {
                    return false;
                }
                a_out = std::move(key);
                return true;
            }
            if (const std::size_t plen = std::wcslen(kSlotPrefix);
                std::wcsncmp(a_key, kSlotPrefix, plen) == 0) {
                const wchar_t* p = a_key + plen;
                if (!*p) {
                    return false;
                }
                for (const wchar_t* q = p; *q; ++q) {
                    if (*q < L'0' || *q > L'9') {
                        return false;  // not a pure "<prefix><number>" key
                    }
                }
                // Generation 0's carrier key is literally "Box<slot>", which is
                // what lets one lookup serve both keyword families.
                std::string key = "Box";
                if (!narrow(p, key)) {
                    return false;
                }
                a_out = std::move(key);
                return true;
            }
            return false;
        }

        // If a_key is one of our keyword names AND the box it names has
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
            std::string carrierKey;
            if (!CarrierKeyFromKeyword(a_key, carrierKey)) {
                return false;
            }
            const std::string contents = LoreBoxContentsForCarrierKey(carrierKey);
            if (contents.empty()) {
                return false;  // no box on that token / empty -> leave untranslated
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
