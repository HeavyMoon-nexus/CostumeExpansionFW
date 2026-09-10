param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$assetRoot = Join-Path $repo 'package_assets'
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$stage = Join-Path $env:TEMP "cef_npc_stage_$Version"
$out = Join-Path $repo "dist\CostumeExpansionFW-NPC-Addon-$Version.7z"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\meshes\CostumeFW\XML" | Out-Null
$esp = Get-Item (Join-Path $assetRoot 'CostumeFW_NPC.esp')
Copy-Item -LiteralPath $esp.FullName -Destination $stage
Copy-Item -LiteralPath "$repo\README_NPC.txt" -Destination $stage
$asset = "$assetRoot\meshes\CostumeFW"
Copy-Item -Path "$asset\Pub*" -Destination "$stage\meshes\CostumeFW"
Copy-Item -Path "$asset\NpcPersist*" -Destination "$stage\meshes\CostumeFW"
Copy-Item -Path "$asset\XML\Pub*" -Destination "$stage\meshes\CostumeFW\XML"
Copy-Item -Path "$asset\XML\NpcPersist*" -Destination "$stage\meshes\CostumeFW\XML"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
# Temp-then-verify-then-replace (review F22) - see package.ps1 for the rationale.
$tmpOut = "$out.tmp"
if (Test-Path $tmpOut) { Remove-Item -LiteralPath $tmpOut -Force }
Push-Location $stage
try {
    & $sevenZip a -t7z $tmpOut * | Select-Object -Last 3
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($rc -ne 0 -or -not (Test-Path $tmpOut)) {
    if (Test-Path $tmpOut) { Remove-Item -LiteralPath $tmpOut -Force }
    throw "7z FAILED (exit $rc). $out is untouched and the stage is kept for diagnosis: $stage"
}
& $sevenZip t $tmpOut | Out-Null
if ($LASTEXITCODE -ne 0) {
    Remove-Item -LiteralPath $tmpOut -Force
    throw "7z wrote an unreadable archive (test exit $LASTEXITCODE). $out is untouched; stage kept: $stage"
}
Move-Item -LiteralPath $tmpOut -Destination $out -Force
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "packaged: $out"

