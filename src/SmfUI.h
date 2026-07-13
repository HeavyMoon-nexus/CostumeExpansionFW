#pragma once

namespace CostumeFW::SmfUI
{
    // Register the SKSE Menu Framework section + pages. No-op when SMF is not
    // installed (IsInstalled() file check; every header wrapper also null-checks
    // the resolved export). Call at kDataLoaded - NOT SKSEPluginLoad: SKSE loads
    // plugin DLLs in filename order and CostumeExpansionFW.dll sorts before
    // SKSEMenuFramework.dll, so early registration could touch a not-yet-loaded
    // module. SMF is the MAIN CEF UI going forward; the SkyUI MCM stays only for
    // the transition (decision 2026-07-12, SMF_RMSS_INVESTIGATION §1.3).
    void Register();
}
