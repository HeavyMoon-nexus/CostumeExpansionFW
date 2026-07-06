// DLL-side seam for nifcarrier_core (NIFCARRIER_INPROC.md). nifly headers are
// NOT C++20/23-clean, so they stay behind the core's C++17 firewall - this TU
// only sees the engine-free interface in NifCarrierCore.h. That also settles
// I-1(d): RE::NiNode and nifly::NiNode can never meet in one translation unit.

#include "nifcarrier/NifCarrierCore.h"

namespace CEF::nifcarrier_bridge {

    // Smoke probe for the MCM Diagnostics page (wired up in Phase 5): shape
    // count of a NIF as nifly sees it, -1 on load failure.
    int CountShapes(const std::filesystem::path& a_path)
    {
        return ::nifcarrier::CountShapes(a_path);
    }

}  // namespace CEF::nifcarrier_bridge
