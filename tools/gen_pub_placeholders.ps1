param(
    [string]$DeployRoot = 'K:\Mo2_SkyrimSE1170\mods\CostumeFW_NPC',
    [switch]$NoDeploy
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$meshRoot = Join-Path $repo 'package_assets\meshes\CostumeFW'
$xmlRoot = Join-Path $meshRoot 'XML'
$nifTemplate = Join-Path $meshRoot 'boxtoken.nif'
$xmlTemplate = Join-Path $xmlRoot 'Box44_physics_r0.xml'

foreach ($slot in 1..8) {
    $nn = '{0:D2}' -f $slot
    foreach ($rev in 0..7) {
        foreach ($sex in @('m','f')) {
            Copy-Item -LiteralPath $nifTemplate -Destination (Join-Path $meshRoot "Pub${nn}_carrier_${sex}_r${rev}.nif") -Force
            Copy-Item -LiteralPath $xmlTemplate -Destination (Join-Path $xmlRoot "Pub${nn}_${sex}_physics_r${rev}.xml") -Force
        }
        Copy-Item -LiteralPath $nifTemplate -Destination (Join-Path $meshRoot "NpcPersist${nn}_carrier_r${rev}.nif") -Force
        Copy-Item -LiteralPath $xmlTemplate -Destination (Join-Path $xmlRoot "NpcPersist${nn}_physics_r${rev}.xml") -Force
    }
}
if ($DeployRoot -and -not $NoDeploy) {
    $deployMeshes = Join-Path $DeployRoot 'meshes\CostumeFW'
    New-Item -ItemType Directory -Force $deployMeshes | Out-Null
    Copy-Item -Path (Join-Path $meshRoot 'Pub*') -Destination $deployMeshes -Force
    Copy-Item -Path (Join-Path $meshRoot 'NpcPersist*') -Destination $deployMeshes -Force
    New-Item -ItemType Directory -Force (Join-Path $deployMeshes 'XML') | Out-Null
    Copy-Item -Path (Join-Path $xmlRoot 'Pub*') -Destination (Join-Path $deployMeshes 'XML') -Force
    Copy-Item -Path (Join-Path $xmlRoot 'NpcPersist*') -Destination (Join-Path $deployMeshes 'XML') -Force
}
Write-Host 'NPC placeholder pool generated.'

