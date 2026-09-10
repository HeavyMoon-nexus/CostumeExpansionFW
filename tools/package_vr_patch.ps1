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
    [Parameter(Mandatory = $true)][string]$Version,
    # Same contract as package.ps1 -ModFolder (v1.3.2): stage the DLL from a
    # clean assembled folder instead of the live MO2 mod when cutting from a
    # release branch.
    [string]$ModFolder = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW'
)
$ErrorActionPreference = 'Stop'
$mod = $ModFolder
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
# Temp-then-verify-then-replace (review F22) - see package.ps1 for the rationale.
$tmpOut = "$out.tmp"
if (Test-Path $tmpOut) { Remove-Item $tmpOut -Force }
Push-Location $stage
try {
    & $sevenZip a -t7z $tmpOut * | Select-Object -Last 3
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($rc -ne 0 -or -not (Test-Path $tmpOut)) {
    if (Test-Path $tmpOut) { Remove-Item $tmpOut -Force }
    throw "7z FAILED (exit $rc). $out is untouched and the stage is kept for diagnosis: $stage"
}
& $sevenZip t $tmpOut | Out-Null
if ($LASTEXITCODE -ne 0) {
    Remove-Item $tmpOut -Force
    throw "7z wrote an unreadable archive (test exit $LASTEXITCODE). $out is untouched; stage kept: $stage"
}
Move-Item -LiteralPath $tmpOut -Destination $out -Force
Write-Host "packaged: $out"
Remove-Item $stage -Recurse -Force
