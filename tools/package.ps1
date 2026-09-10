# package.ps1 - stage + build the Nexus 7z (THIRD-PARTY-NOTICES.md packaging checklist).
# Stages from the DEPLOYED MOD FOLDER only (never build/ or dist/), with an explicit
# include list mirroring the v1.2.1 archive manifest. Run AFTER:
#   1. CMakeLists VERSION bump + build.cmd release (DLL deployed with the new banner)
#   2. psc compile (pex deployed - HANDOVER 8.6d recipe)
#   3. tools/espmerge fold of any patch esp into CostumeFW.esp  <-- v1.3.0 blocker:
#      CostumeFW_VanillaSlots_001.esp must be folded (HANDOVER 9.2 F1 release note)
# CostumeFW_NPC.esp is a permanent separate add-on: NEVER fold or stage it here.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    # Mod folder to stage the deployed dll/esp/pex from. Default = the live MO2
    # mod. A hotfix cut from a release branch stages from a CLEAN folder
    # assembled out of the previous release archive + the freshly built DLL,
    # so a beta-line deployment in the live folder cannot leak into a stable
    # package (introduced for v1.3.2).
    [string]$ModFolder = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW'
)
$ErrorActionPreference = 'Stop'
$mod = $ModFolder
$repo = Split-Path $PSScriptRoot -Parent
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_stage_$Version"
$out = Join-Path $repo "dist\CostumeExpansionFW-$Version.7z"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- from the deployed mod folder (explicit manifest) ---
Copy-Item "$mod\CostumeFW.esp" $stage
Copy-Item "$mod\CostumeFW_KID.ini" $stage
# v1.3.2 L3 opt-out template: a static repo file (never modified at runtime),
# staged from the repo like the licenses.
Copy-Item "$repo\CostumeFW_NoCapture_KID.ini" $stage
New-Item -ItemType Directory -Force "$stage\SEQ" | Out-Null
Copy-Item "$mod\SEQ\CostumeFW.seq" "$stage\SEQ\"
# Carrier placeholders come from the PRISTINE repo tree, NEVER the live mod
# folder: the in-game auto-sync rewrites the deployed pool with meshes built
# from the USER'S OWN costume content (third-party assets) - shipping those
# would redistribute them (caught 2026-07-13 staging v1.3.0: 85MB vs 234-byte
# placeholders). package_assets/meshes = the 1.2.1 pool + vanilla-slot boxes.
Copy-Item "$repo\package_assets\meshes" "$stage\meshes" -Recurse
# The Pub*/NpcPersist* rotation pools are ADDON-ONLY (CostumeFW_NPC zip via
# package_npc_addon.ps1). Shipping them here too would put the same virtual
# paths in two mods, making the in-proc sync's VFS write-through target depend
# on MO2 priority. Prune them from the core staging and fail loud on leaks.
Remove-Item "$stage\meshes\CostumeFW\Pub*_carrier_*.nif"
Remove-Item "$stage\meshes\CostumeFW\NpcPersist*_carrier_*.nif"
Remove-Item "$stage\meshes\CostumeFW\XML\Pub*_physics_*.xml"
Remove-Item "$stage\meshes\CostumeFW\XML\NpcPersist*_physics_*.xml"
$npcLeak = Get-ChildItem "$stage\meshes" -Recurse -File |
    Where-Object { $_.Name -like 'Pub*' -or $_.Name -like 'NpcPersist*' }
if ($npcLeak) { throw "addon-only pool leaked into core staging ($($npcLeak.Count) file(s))" }
New-Item -ItemType Directory -Force "$stage\Scripts" | Out-Null
Copy-Item "$mod\Scripts\CFW_Native.pex" "$stage\Scripts\"
Copy-Item "$mod\Scripts\CostumeFW_MCM.pex" "$stage\Scripts\"
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null
Copy-Item "$mod\SKSE\Plugins\CostumeExpansionFW.dll" "$stage\SKSE\Plugins\"
# The documented ini ships from the REPO (distribution defaults: bDebugMode=0),
# never from the live mod folder (the owner's copy carries diagnostic toggles).
# Missing from the package once (test.3, 2026-07-30) - the ini had to be sent
# separately. Update note for users who edited it: reinstalling resets it.
Copy-Item "$repo\CostumeExpansionFW.ini" "$stage\SKSE\Plugins\"

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

# --- sanity: the repo mirrors of the two mod-folder-staged files must match ---
# CostumeFW.esp and CostumeFW_KID.ini ship from the DEPLOYED folder, so their
# tracked copies are mirrors, not sources. Silent drift is exactly what left the
# repo's KID ini pointing at CostumeFW_VanillaSlots_001.esp for every release
# after the v1.3.0 fold (found 2026-08-17: the shipped archives were correct,
# the repo copy was not). Fail loud and refresh the mirror instead of shipping
# blind. Text is compared CRLF-normalized so core.autocrlf cannot fake a diff.
$espMirror = "$repo\package_assets\CostumeFW.esp"
if (-not (Test-Path $espMirror)) { throw "repo mirror missing: $espMirror" }
if ((Get-FileHash "$stage\CostumeFW.esp" -Algorithm SHA256).Hash -ne
    (Get-FileHash $espMirror -Algorithm SHA256).Hash) {
    throw "repo mirror is stale: $espMirror does not match the deployed esp. Copy the deployed CostumeFW.esp over it and commit, then re-run."
}
$kidMirror = "$repo\CostumeFW_KID.ini"
if (-not (Test-Path $kidMirror)) { throw "repo mirror missing: $kidMirror" }
$norm = { (Get-Content -Raw -LiteralPath $args[0]) -replace "`r`n", "`n" }
if ((& $norm "$stage\CostumeFW_KID.ini") -ne (& $norm $kidMirror)) {
    throw "repo mirror is stale: $kidMirror does not match the deployed KID ini. Copy the deployed CostumeFW_KID.ini over it and commit, then re-run."
}
Write-Host "repo mirrors: CostumeFW.esp + CostumeFW_KID.ini OK"

# Build into a temp name, CHECK the exit code, TEST the archive, and only then
# replace the last good one (adversarial review 2026-09-09 F22): $ErrorActionPreference
# does NOT stop on a native non-zero exit, so deleting $out first and trusting 7z
# published a truncated archive as a release and threw the stage away with it.
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
