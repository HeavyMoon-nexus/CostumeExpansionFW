#pragma once

#include "CapturePolicy.h"

#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace CostumeFW
{
    // Build the project's colon-form id "XXXXXX:Plugin.esp" from a form: its
    // plugin-local FormID (ESL-masked) + defining plugin filename.
    //
    // Lived in BoxStore.cpp until the ability pool needed it too. It is here
    // rather than copied because the no-file guard below is the load-bearing
    // part, and a copy of a guard is a guard that will be fixed in one place
    // only. The FORMAT itself has been shared (policy::FormatColonId) since r2
    // for the same reason.
    //
    // No-file safety (review P1-4): TESForm::GetLocalFormID() dereferences
    // GetFile(0) UNCHECKED - calling it on a runtime/no-file form is the
    // null-deref behind the original "+ Add worn item" CTD. Such forms get
    // their raw 8-digit FormID and an empty plugin: the same textual shape as
    // before, produced without touching the missing file, and unresolvable by
    // design (the formatter never truncates - the old char[8] bug).
    [[nodiscard]] inline std::string MakeColonId(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return policy::FormatColonId(0, {});
        }
        const auto* file = a_form->GetFile(0);
        const std::uint32_t local =
            file ? a_form->GetLocalFormID() : a_form->GetFormID();
        return policy::FormatColonId(
            local, file ? std::string_view(file->GetFilename()) : std::string_view{});
    }
}
