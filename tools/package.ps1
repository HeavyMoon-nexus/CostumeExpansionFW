# package.ps1 - stage + build the Nexus 7z (THIRD-PARTY-NOTICES.md packaging checklist).
# Stages from the DEPLOYED MOD FOLDER only (never build/ or dist/), with an explicit
# include list mirroring the v1.2.1 archive manifest. Run AFTER:
#   1. CMakeLists VERSION bump + build.cmd release (DLL deployed with the new banner)
#   2. psc compile (pex deployed - HANDOVER 8.6d recipe)
#   3. tools/espmerge fold of any patch esp into CostumeFW.esp  <-- v1.3.0 blocker:
#      CostumeFW_VanillaSlots_001.esp must be folded (HANDOVER 9.2 F1 release note)
param(
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
$mod = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW'
$repo = Split-Path $PSScriptRoot -Parent
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_stage_$Version"
$out = Join-Path $repo "dist\CostumeExpansionFW-$Version.7z"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- from the deployed mod folder (explicit manifest) ---
Copy-Item "$mod\CostumeFW.esp" $stage
Copy-Item "$mod\CostumeFW_KID.ini" $stage
New-Item -ItemType Directory -Force "$stage\SEQ" | Out-Null
Copy-Item "$mod\SEQ\CostumeFW.seq" "$stage\SEQ\"
# Carrier placeholders come from the PRISTINE repo tree, NEVER the live mod
# folder: the in-game auto-sync rewrites the deployed pool with meshes built
# from the USER'S OWN costume content (third-party assets) - shipping those
# would redistribute them (caught 2026-07-13 staging v1.3.0: 85MB vs 234-byte
# placeholders). package_assets/meshes = the 1.2.1 pool + vanilla-slot boxes.
Copy-Item "$repo\package_assets\meshes" "$stage\meshes" -Recurse
New-Item -ItemType Directory -Force "$stage\Scripts" | Out-Null
Copy-Item "$mod\Scripts\CFW_Native.pex" "$stage\Scripts\"
Copy-Item "$mod\Scripts\CostumeFW_MCM.pex" "$stage\Scripts\"
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null
Copy-Item "$mod\SKSE\Plugins\CostumeExpansionFW.dll" "$stage\SKSE\Plugins\"

# --- from the repo (licenses + script sources) ---
Copy-Item "$repo\LICENSE" $stage
Copy-Item "$repo\LICENSE.GPL-3.0.txt" $stage
Copy-Item "$repo\THIRD-PARTY-NOTICES.md" $stage
New-Item -ItemType Directory -Force "$stage\Source\Scripts" | Out-Null
Copy-Item "$repo\papyrus\CFW_Native.psc" "$stage\Source\Scripts\"
Copy-Item "$repo\papyrus\CostumeFW_MCM.psc" "$stage\Source\Scripts\"

# --- sanity: DLL banner file-stamp must be today-ish (stale-deploy guard) ---
$dll = Get-Item "$stage\SKSE\Plugins\CostumeExpansionFW.dll"
Write-Host "DLL: $($dll.LastWriteTime)  $($dll.Length) bytes"
$esp = Get-Item "$stage\CostumeFW.esp"
Write-Host "ESP: $($esp.LastWriteTime)  $($esp.Length) bytes"

if (Test-Path $out) { Remove-Item $out -Force }
Push-Location $stage
& $sevenZip a -t7z $out * | Select-Object -Last 3
Pop-Location
Write-Host "packaged: $out"
Remove-Item $stage -Recurse -Force
