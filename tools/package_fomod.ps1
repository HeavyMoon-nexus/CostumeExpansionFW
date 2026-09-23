# package_fomod.ps1 - stage + build the single FOMOD 7z that replaces the three
# separate Nexus downloads (main / NPC add-on / VR build).
#
# Run AFTER the same prerequisites as package.ps1:
#   1. CMakeLists VERSION bump + build.cmd release (SE/AE DLL deployed)
#   2. psc compile (pex deployed)
#   3. any patch esp folded into CostumeFW.esp
# The script checks what it can of that itself and stops before archiving if
# anything is off (review 2026-09-23 W23): the host tests, that the DLL is this
# repo's release build and says -Version to SKSE, the plugin verifiers in
# tools/espmerge, the installer XML, pristine carriers and diagnostics off.
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
#   ench/             CostumeFW_Abilities.esp + CostumeFW_Abilities2.esp
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ModFolder = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW',
    # The ability pool. Non-ESL on purpose: saves point at fixed form IDs in it,
    # and the light range cannot hold enough of them.
    [string]$AbilitiesEsp = 'K:\dev\CostumeExpansionFW\package_assets\CostumeFW_Abilities.esp',
    [int]$AbilityCount = 1024,
    # Generation 2. ESL on purpose - see make_ability_pool.py's header for why
    # generation 1 is the one exception and every later one is light.
    [string]$Abilities2Esp = 'K:\dev\CostumeExpansionFW\package_assets\CostumeFW_Abilities2.esp',
    [int]$Ability2Count = 2048,
    # Where the archive goes (default: dist\ in the repo). A dry run points this
    # at a temp folder so the last good archive stays where it is.
    [string]$OutDir = '',
    # info.xml + ModuleConfig.xml (default: fomod\ in the repo). A test points
    # this at a deliberately broken copy to watch the XML check stop the run.
    [string]$FomodDir = ''
)
$ErrorActionPreference = 'Stop'
$mod = $ModFolder
$repo = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }
if (-not $FomodDir) { $FomodDir = Join-Path $repo 'fomod' }
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_fomod_$Version"
$out = Join-Path $OutDir "CostumeExpansionFW-$Version-FOMOD.7z"

# --- preflight: is this the build that should ship? -------------------------
# Every check here used to be the packager's memory, and each mistake it catches
# sails through every other step: a DLL from a worktree or a debug build is
# still a DLL, and a version nobody bumped still loads.
if ($Version -notmatch '^\d+(\.\d+){1,3}$') {
    throw "-Version '$Version' is not a dotted version number like 1.6.4.1."
}
$buildDir = Join-Path $repo 'build\release'

# The host-side tests, run on the build that is about to ship.
$policyTests = Join-Path $buildDir 'policy_tests.exe'
if (-not (Test-Path -LiteralPath $policyTests)) {
    throw "missing $policyTests - run build.cmd release first."
}
& $policyTests | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "policy_tests.exe failed (exit $LASTEXITCODE) - nothing was packaged." }

# The DLL must be THIS repo's release build. What gets staged is the deployed
# copy, and every build.cmd overwrites that one - a worktree's, or a
# `build.cmd debug`, whose DLL links the debug CRT (/MDd) that no player has
# installed, so it would not load for anyone.
$cache = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache) -or
    -not (Select-String -LiteralPath $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=Release$' -Quiet)) {
    throw "$buildDir is not a Release build directory - run build.cmd release."
}
$builtDll = Join-Path $buildDir 'CostumeExpansionFW.dll'
$deployedDll = Join-Path $mod 'SKSE\Plugins\CostumeExpansionFW.dll'
if (-not (Test-Path -LiteralPath $builtDll)) { throw "missing $builtDll - run build.cmd release." }
if ((Get-FileHash -LiteralPath $builtDll -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $deployedDll -Algorithm SHA256).Hash) {
    throw "the deployed DLL is not this repo's release build: $deployedDll differs from $builtDll. Another build deployed over it (a worktree, build.cmd debug) - run build.cmd release here, then re-run."
}
# Latin-1 turns each byte into exactly one char, so byte patterns can be
# searched for as text.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$dllText = $latin1.GetString([System.IO.File]::ReadAllBytes($deployedDll))
foreach ($crt in 'ucrtbased.dll', 'vcruntime140d.dll', 'msvcp140d.dll') {
    if ($dllText.IndexOf($crt, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "the DLL imports $crt - it is a debug (/MDd) build and would not load on any player's machine."
    }
}
# The version SKSE reports is the SKSEPlugin_Version block that SKSEPluginInfo
# generates from CMakeLists' VERSION: dataVersion 1, REL::Version::pack(), then
# the plugin name. The DLL has no VERSIONINFO resource, so this block is the
# only place the number lives - and the VERSION line is easy to leave behind
# (the 1.6.4.1 work began with a DLL that still told SKSE 1.6.4).
$parts = @($Version.Split('.') | ForEach-Object { [int]$_ }) + @(0, 0, 0)
$packed = [uint32]((($parts[0] -band 0xFF) * 0x1000000) + (($parts[1] -band 0xFF) * 0x10000) +
                   (($parts[2] -band 0xFFF) * 0x10) + ($parts[3] -band 0xF))
[byte[]]$versionBlock = @(1, 0, 0, 0) + [System.BitConverter]::GetBytes($packed) +
    [System.Text.Encoding]::ASCII.GetBytes('CostumeExpansionFW') + @(0)
if ($dllText.IndexOf($latin1.GetString($versionBlock), [System.StringComparison]::Ordinal) -lt 0) {
    throw "the DLL does not tell SKSE it is $Version. Set project(CostumeExpansionFW VERSION $Version) in CMakeLists.txt, run build.cmd release, then re-run."
}
Write-Host "DLL: this repo's release build, tells SKSE $Version"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$dirs = 'fomod', 'core', 'vr', 'npc', 'ench'
foreach ($d in $dirs) { New-Item -ItemType Directory -Force (Join-Path $stage $d) | Out-Null }

# --- fomod/ ---------------------------------------------------------------
# info.xml's <Version> is STAMPED from -Version rather than copied, because the
# tracked copy had said 1.6.3 since the FOMOD was written and nothing made it
# move: 1.6.3 shipped correct by coincidence and 1.6.4 would have gone out
# labelled 1.6.3 in every mod manager. The archive name already comes from
# -Version, so this makes the two agree by construction instead of by anyone
# remembering. Fails loud if the tag is not where it is expected - a silent
# no-op replace would ship the wrong number just as quietly.
$infoXml = Get-Content -Raw -LiteralPath "$FomodDir\info.xml"
$stamped = $infoXml -replace '<Version\b[^>]*>[^<]*</Version>',
    "<Version MachineVersion=`"$Version`">$Version</Version>"
if ($stamped -eq $infoXml) {
    throw "fomod\info.xml: no <Version> element to stamp - refusing to package an unlabelled FOMOD."
}
# .NET rather than Set-Content: PowerShell 5.1's -Encoding UTF8 always writes a
# BOM, and the tracked info.xml has none. Stamping a version is no reason to
# change the file's encoding underneath a FOMOD installer.
[System.IO.File]::WriteAllText("$stage\fomod\info.xml", $stamped,
    (New-Object System.Text.UTF8Encoding $false))
Copy-Item "$FomodDir\ModuleConfig.xml" "$stage\fomod\"

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
# BOTH generations ship together. Generation 2 was built, verified and supported
# at runtime for 1.6.4 but never staged, so the "the pool is full, install
# CostumeFW_Abilities2.esp" guidance - which the DLL logs, the SMF Main page
# prints and the installer's own description repeats - named a file nobody could
# get. Shipping it costs nothing: it is ESL (no load-order slot), it holds empty
# spells until something is allocated into it, and MeasureGenerations finds it by
# itself. It moves the wall from 1,024 abilities to 3,072.
if (-not (Test-Path $AbilitiesEsp)) {
    throw "ability pool not found: $AbilitiesEsp. 1.6.3 cannot ship without it - enchantment passthrough is on by default and has nowhere to put its abilities."
}
Copy-Item $AbilitiesEsp "$stage\ench\CostumeFW_Abilities.esp"
if (-not (Test-Path $Abilities2Esp)) {
    throw "generation 2 pool not found: $Abilities2Esp. Generate it with 'python tools/make_ability_pool.py --generation 2 --esl', or the full-pool guidance keeps naming a file that is not in the archive."
}
Copy-Item $Abilities2Esp "$stage\ench\CostumeFW_Abilities2.esp"

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

# The pool MUST NOT be ESL-flagged. Its form IDs are what saves point at, and
# the light range is both too small and addressed differently - flagging it
# later would break every save that already used it. TES4 record flags are at
# offset 0x08; the light-master bit is 0x200.
$flags = [BitConverter]::ToUInt32($bytes, 8)
if ($flags -band 0x200) {
    throw "CostumeFW_Abilities.esp is ESL-flagged. It has to be a full plugin - see the design doc, section 2.2."
}

# Generation 2: the same two checks with the ESL one INVERTED. Generation 1 is a
# full plugin because saves already point into it and the flag can never change;
# every later generation must be light, or adding capacity would cost a
# load-order slot each time and the whole "just ship the next one" plan fails.
$bytes2 = [System.IO.File]::ReadAllBytes("$stage\ench\CostumeFW_Abilities2.esp")
$spel2 = ([regex]::Matches([System.Text.Encoding]::ASCII.GetString($bytes2), 'SPEL')).Count - 1
if ($spel2 -lt $Ability2Count) {
    throw "generation 2 pool looks short: found about $spel2 SPEL records, expected $Ability2Count."
}
if (-not ([BitConverter]::ToUInt32($bytes2, 8) -band 0x200)) {
    throw "CostumeFW_Abilities2.esp is NOT ESL-flagged. Generation 2 and up have to be light - regenerate it with --esl."
}
Write-Host "ability pools: about $spel (gen1, full) + $spel2 (gen2, ESL) SPEL records"

# There is no canceled/ any more. The installer's first question used to be
# able to end the install by staging one text file instead of the mod, and a
# mod manager told to REPLACE turned that into deleting the install the user
# was being sent back to. ModuleConfig.xml's header note has the rest.

# --- verify what was staged -------------------------------------------------
# The checks run on the staged copies rather than the sources, so what passes is
# exactly what goes into the archive - a Copy-Item above that picked up the
# wrong file cannot slip past them.

# The installer's two files. A mod manager parses them as XML, and a single bare
# '&' in a description makes the whole installer fail to open.
foreach ($x in 'info.xml', 'ModuleConfig.xml') {
    $doc = New-Object System.Xml.XmlDocument
    try { $doc.Load((Join-Path $stage "fomod\$x")) }
    catch { throw "fomod\$x is not well-formed XML: $($_.Exception.Message)" }
    if ($x -eq 'ModuleConfig.xml') {
        foreach ($node in $doc.SelectNodes('//*[@source]')) {
            $src = $node.GetAttribute('source')
            if (-not (Test-Path -LiteralPath (Join-Path $stage $src))) {
                throw "ModuleConfig.xml installs '$src', which is not in the package."
            }
        }
    }
}

# The plugin verifiers in tools/espmerge. Each asserts what a mistake would
# otherwise hide until saves point into the file: a pool whose masters are wrong
# is a startup CTD in SE, and a token that lost its race list is a box that does
# nothing on a custom race without saying so.
$espmerge = Join-Path $PSScriptRoot 'espmerge\espmerge.csproj'
& dotnet build $espmerge --no-restore -nologo -v q | Out-Null
if ($LASTEXITCODE -ne 0) { throw "espmerge does not build: dotnet build $espmerge" }
$verifiers = @(
    @('--verify-core', "$core\CostumeFW.esp"),
    @('--verify-pool', "$core\CostumeFW_BoxPool1.esp", "$core\CostumeFW.esp", "$core\meshes"),
    @('--verify-npc', "$stage\npc\CostumeFW_NPC.esp"),
    @('--verify-abilities', '1', "$stage\ench\CostumeFW_Abilities.esp"),
    @('--verify-abilities', '2', "$stage\ench\CostumeFW_Abilities2.esp")
)
foreach ($v in $verifiers) {
    & dotnet run --project $espmerge --no-build -- @v
    if ($LASTEXITCODE -ne 0) { throw "espmerge $($v[0]) failed (see VERIFY FAIL above) - nothing was archived." }
}

# Carriers ship as the pristine placeholders and nothing else: 234-byte NIFs and
# physics XMLs of 64 bytes or less. Anything bigger was built from somebody's
# costume - the in-game auto-sync writes those into the deployed folder, and
# 85MB of them reached the v1.3.0 archive.
$carrierFiles = @(Get-ChildItem -LiteralPath "$core\meshes", "$stage\npc\meshes" -Recurse -File)
$notPristine = @($carrierFiles | Where-Object {
    -not (($_.Extension -eq '.nif' -and $_.Length -eq 234) -or
          ($_.Extension -eq '.xml' -and $_.Length -le 64))
})
if ($notPristine.Count -ne 0) {
    throw "$($notPristine.Count) carrier file(s) are not pristine placeholders (NIF 234 bytes, XML 64 or less), first: $($notPristine[0].FullName) ($($notPristine[0].Length) bytes)"
}
Write-Host "carriers: $($carrierFiles.Count) pristine placeholder file(s)"

# Diagnostic mode stays off in the shipped ini: on, every player's log grows by
# the debug volume and is flushed on every line.
$shippedIni = "$core\SKSE\Plugins\CostumeExpansionFW.ini"
$debugLines = @(Get-Content -LiteralPath $shippedIni | Where-Object { $_ -match '^\s*bDebugMode\s*=' })
$debugValues = @($debugLines | ForEach-Object { (($_ -split '=', 2)[1] -replace '[;#].*$', '').Trim() })
if ($debugValues.Count -ne 1 -or $debugValues[0] -ne '0') {
    throw "the shipped ini must set bDebugMode=0 exactly once, found: $($debugLines -join ' | ')"
}

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
