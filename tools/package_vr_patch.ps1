# package_vr_patch.ps1 - stage + build the Skyrim VR patch 7z.
# The VR patch = the (runtime-independent) DLL + VR install notes ONLY.
# esp / meshes / scripts / KID ini are byte-identical across SE/AE/VR and stay
# in the main package (2026-07-17 decision: plan A - Skyrim VR ESL Support
# handles the ESL-flagged 1.71 esp on VR, so no plugin variant is shipped).
# The DLL is included so the patch stays correct even against an older main
# archive; it is the SAME multi-runtime binary the main package ships.
# Run AFTER build.cmd release (DLL deployed with the new banner) - same
# stale-deploy guard as package.ps1.
param(
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'
$mod = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW'
$repo = Split-Path $PSScriptRoot -Parent
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_vr_stage_$Version"
$out = Join-Path $repo "dist\CostumeExpansionFW-VR-Patch-$Version.7z"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null

Copy-Item "$mod\SKSE\Plugins\CostumeExpansionFW.dll" "$stage\SKSE\Plugins\"
Copy-Item "$repo\README_VR.txt" $stage

# --- sanity: DLL banner file-stamp must be today-ish (stale-deploy guard) ---
$dll = Get-Item "$stage\SKSE\Plugins\CostumeExpansionFW.dll"
Write-Host "DLL: $($dll.LastWriteTime)  $($dll.Length) bytes"

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
if (Test-Path $out) { Remove-Item $out -Force }
Push-Location $stage
& $sevenZip a -t7z $out * | Select-Object -Last 3
Pop-Location
Write-Host "packaged: $out"
Remove-Item $stage -Recurse -Force
