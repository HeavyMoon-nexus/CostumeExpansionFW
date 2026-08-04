param(
    [string]$CoreEsp = 'K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW\CostumeFW.esp',
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $repo 'package_assets' }
$project = Join-Path $PSScriptRoot 'espmerge\espmerge.csproj'

if (-not (Test-Path -LiteralPath $CoreEsp -PathType Leaf)) {
    throw "Core template ESP was not found: $CoreEsp"
}

& dotnet run --project $project --no-restore -- --build-npc $CoreEsp $OutDir
if ($LASTEXITCODE -ne 0) {
    throw "CostumeFW_NPC.esp generation or verification failed (exit $LASTEXITCODE)"
}

Write-Host "Generated and verified: $(Join-Path $OutDir 'CostumeFW_NPC.esp')"
