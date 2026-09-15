# package_fomod.ps1 - stage + build the single FOMOD 7z that replaces the three
# separate Nexus downloads (main / NPC add-on / VR build).
#
# Run AFTER the same prerequisites as package.ps1:
#   1. CMakeLists VERSION bump + build.cmd release (SE/AE DLL deployed)
#   2. psc compile (pex deployed)
#   3. any patch esp folded into CostumeFW.esp
# There is NO separate VR build. CEF is one CommonLibSSE-NG binary that serves
# SE, AE and VR and branches on REL::Module::IsVR(); package_vr_patch.ps1 says so
# and always staged the same deployed DLL, and the 1.6.2.2 main and VR archives
# ship byte-identical DLLs (verified 2026-09-14). The VR download was the same
# DLL plus README_VR.txt, which is all the vr/ folder holds here.
#
# Layout produced (what the FOMOD's ModuleConfig.xml refers to):
#
#   fomod/            info.xml + ModuleConfig.xml
#   core/             everything common to every install, minus the DLL
#   vr/               README_VR.txt (the DLL is NOT VR-specific - see below)
#   npc/              CostumeFW_NPC.esp + its carriers + README_NPC.txt
#   ench/             CostumeFW_Abilities.esp
#   canceled/         install_canceled.txt
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ModFolder = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW',
    # The ability pool. Non-ESL on purpose: saves point at fixed form IDs in it,
    # and the light range cannot hold enough of them.
    [string]$AbilitiesEsp = 'K:\dev\CostumeExpansionFW\package_assets\CostumeFW_Abilities.esp',
    [int]$AbilityCount = 1024
)
$ErrorActionPreference = 'Stop'
$mod = $ModFolder
$repo = Split-Path $PSScriptRoot -Parent
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_fomod_$Version"
$out = Join-Path $repo "dist\CostumeExpansionFW-$Version-FOMOD.7z"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$dirs = 'fomod', 'core', 'vr', 'npc', 'ench', 'canceled'
foreach ($d in $dirs) { New-Item -ItemType Directory -Force (Join-Path $stage $d) | Out-Null }

# --- fomod/ ---------------------------------------------------------------
Copy-Item "$repo\fomod\info.xml" "$stage\fomod\"
Copy-Item "$repo\fomod\ModuleConfig.xml" "$stage\fomod\"

# --- core/ (package.ps1's manifest, minus SKSE/Plugins/*.dll) --------------
$core = "$stage\core"
Copy-Item "$mod\CostumeFW.esp" $core
Copy-Item "$mod\CostumeFW_KID.ini" $core
# BoxPool1 rides in core, not as an option (PLAN 4.3 / A11): a slot that can
# hold only one box is what 1.6.4 exists to fix, and a box made on a pool token
# goes dormant without it. Staged from the REPO, like the other tracked assets -
# never from the deployed mod folder, which auto-sync rewrites.
Copy-Item "$repo\package_assets\CostumeFW_BoxPool1.esp" $core
Copy-Item "$repo\package_assets\CostumeFW_BoxPool1_KID.ini" $core
Copy-Item "$repo\CostumeFW_NoCapture_KID.ini" $core
New-Item -ItemType Directory -Force "$core\SEQ" | Out-Null
Copy-Item "$mod\SEQ\CostumeFW.seq" "$core\SEQ\"

# The PRISTINE pool, never the deployed one: the in-game auto-sync rewrites the
# deployed carriers with meshes built from the owner's own costumes, and that
# leaked 85MB of third-party meshes into a release once (v1.3.0).
Copy-Item "$repo\package_assets\meshes" "$core\meshes" -Recurse
Remove-Item "$core\meshes\CostumeFW\Pub*_carrier_*.nif"
Remove-Item "$core\meshes\CostumeFW\NpcPersist*_carrier_*.nif"
Remove-Item "$core\meshes\CostumeFW\XML\Pub*_physics_*.xml"
Remove-Item "$core\meshes\CostumeFW\XML\NpcPersist*_physics_*.xml"
$npcLeak = Get-ChildItem "$core\meshes" -Recurse -File |
    Where-Object { $_.Name -like 'Pub*' -or $_.Name -like 'NpcPersist*' }
if ($npcLeak) { throw "NPC add-on carriers leaked into core/: $($npcLeak.Name -join ', ')" }

New-Item -ItemType Directory -Force "$core\Scripts" | Out-Null
Copy-Item "$mod\Scripts\CFW_Native.pex" "$core\Scripts\"
Copy-Item "$mod\Scripts\CostumeFW_MCM.pex" "$core\Scripts\"
New-Item -ItemType Directory -Force "$core\SKSE\Plugins" | Out-Null
# The repo's ini, never the deployed one - the owner's copy carries diagnostic
# toggles that must not ship.
Copy-Item "$repo\CostumeExpansionFW.ini" "$core\SKSE\Plugins\"
Copy-Item "$repo\LICENSE" $core
Copy-Item "$repo\LICENSE.GPL-3.0.txt" $core
Copy-Item "$repo\THIRD-PARTY-NOTICES.md" $core
New-Item -ItemType Directory -Force "$core\Source\Scripts" | Out-Null
Copy-Item "$repo\papyrus\CFW_Native.psc" "$core\Source\Scripts\"
Copy-Item "$repo\papyrus\CostumeFW_MCM.psc" "$core\Source\Scripts\"

# --- the DLL (core) and vr/ -----------------------------------------------
# ONE binary for every runtime, so it goes in core/ with everything else. An
# earlier version of this script took a -VrDll and REFUSED a package whose two
# DLLs matched, which is backwards: in this project they have to match, and the
# only way to satisfy that guard was to hand it a DLL from another build. It
# did exactly that once, in a test package, before anyone compared the hashes.
New-Item -ItemType Directory -Force "$core\SKSE\Plugins" | Out-Null
Copy-Item "$mod\SKSE\Plugins\CostumeExpansionFW.dll" "$core\SKSE\Plugins\"
Copy-Item "$repo\README_VR.txt" "$stage\vr\"

# --- npc/ -----------------------------------------------------------------
$assetRoot = "$repo\package_assets"
Copy-Item "$assetRoot\CostumeFW_NPC.esp" "$stage\npc\"
Copy-Item "$repo\README_NPC.txt" "$stage\npc\"
New-Item -ItemType Directory -Force "$stage\npc\meshes\CostumeFW\XML" | Out-Null
Copy-Item "$assetRoot\meshes\CostumeFW\Pub*" "$stage\npc\meshes\CostumeFW"
Copy-Item "$assetRoot\meshes\CostumeFW\NpcPersist*" "$stage\npc\meshes\CostumeFW"
Copy-Item "$assetRoot\meshes\CostumeFW\XML\Pub*" "$stage\npc\meshes\CostumeFW\XML"
Copy-Item "$assetRoot\meshes\CostumeFW\XML\NpcPersist*" "$stage\npc\meshes\CostumeFW\XML"

# --- ench/ ----------------------------------------------------------------
if (-not (Test-Path $AbilitiesEsp)) {
    throw "ability pool not found: $AbilitiesEsp. 1.6.3 cannot ship without it - enchantment passthrough is on by default and has nowhere to put its abilities."
}
Copy-Item $AbilitiesEsp "$stage\ench\CostumeFW_Abilities.esp"

# Count the SPEL records, approximately: scan for the record signature and drop
# one for the GRUP header that carries the same four bytes. It is a "did you
# ship the right file" check, not a validator - the DLL does the real one at
# kDataLoaded, where it can read form IDs instead of guessing from bytes.
$bytes = [System.IO.File]::ReadAllBytes("$stage\ench\CostumeFW_Abilities.esp")
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
$spel = ([regex]::Matches($text, 'SPEL')).Count - 1
if ($spel -lt $AbilityCount) {
    throw "ability pool looks short: found about $spel SPEL records, expected $AbilityCount. A pool smaller than the DLL expects fails closed in game and nobody gets enchantment passthrough."
}
Write-Host "ability pool: about $spel SPEL records"

# The pool MUST NOT be ESL-flagged. Its form IDs are what saves point at, and
# the light range is both too small and addressed differently - flagging it
# later would break every save that already used it. TES4 record flags are at
# offset 0x08; the light-master bit is 0x200.
$flags = [BitConverter]::ToUInt32($bytes, 8)
if ($flags -band 0x200) {
    throw "CostumeFW_Abilities.esp is ESL-flagged. It has to be a full plugin - see the design doc, section 2.2."
}

# --- canceled/ ------------------------------------------------------------
Copy-Item "$repo\package_assets\install_canceled.txt" "$stage\canceled\"

# --- sanity, same as package.ps1 ------------------------------------------
$dll = Get-Item "$core\SKSE\Plugins\CostumeExpansionFW.dll"
Write-Host "DLL (SE/AE/VR): $($dll.LastWriteTime)  $($dll.Length) bytes"

$espMirror = "$assetRoot\CostumeFW.esp"
if (-not (Test-Path $espMirror)) { throw "repo mirror missing: $espMirror" }
if ((Get-FileHash "$core\CostumeFW.esp" -Algorithm SHA256).Hash -ne
    (Get-FileHash $espMirror -Algorithm SHA256).Hash) {
    throw "repo mirror is stale: $espMirror does not match the deployed esp. Copy the deployed CostumeFW.esp over it and commit, then re-run."
}
$kidMirror = "$repo\CostumeFW_KID.ini"
$norm = { (Get-Content -Raw -LiteralPath $args[0]) -replace "`r`n", "`n" }
if ((& $norm "$core\CostumeFW_KID.ini") -ne (& $norm $kidMirror)) {
    throw "repo mirror is stale: $kidMirror does not match the deployed KID ini. Copy the deployed CostumeFW_KID.ini over it and commit, then re-run."
}
Write-Host "repo mirrors: CostumeFW.esp + CostumeFW_KID.ini OK"

# --- archive --------------------------------------------------------------
# Temp name, check the exit code, test the archive, and only then replace the
# last good one: $ErrorActionPreference does not stop on a native non-zero exit,
# and trusting 7z once published a truncated archive as a release.
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$tmpOut = "$out.tmp"
if (Test-Path $tmpOut) { Remove-Item $tmpOut -Force }
Push-Location $stage
try {
    & $sevenZip a -t7z $tmpOut * | Select-Object -Last 3
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($rc -ne 0) { throw "7z failed with exit code $rc - $out was left alone" }
& $sevenZip t $tmpOut | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "7z could not verify $tmpOut - $out was left alone" }
Move-Item -LiteralPath $tmpOut -Destination $out -Force
Write-Host "wrote $out"
