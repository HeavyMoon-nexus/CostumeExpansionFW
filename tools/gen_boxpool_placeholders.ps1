# Carrier placeholder pool for a BOX pool generation (v1.6.4).
#
# Every token needs its own carrier namespace: the in-game sync REWRITES a
# pre-created revision file in place, because usvfs shows external rewrites of
# existing files but never externally-created new ones. Slot-keyed names were
# fine while a slot held one box; from 1.6.4 several tokens share a slot and
# would overwrite each other's meshes, so the key is the token
# (BP<gen>_<localid>) and these files have to exist before the game starts.
#
# The templates are the same tiny stand-ins the other pools use - a 234-byte NIF
# and a 42-byte XML. Do NOT source them from a deployed mod folder: the in-game
# auto-sync rewrites those with meshes built from the user's own costumes, and
# staging that would redistribute third-party assets (caught at v1.3.0).
#
# Usage:
#   tools\gen_boxpool_placeholders.ps1 -Generation 1 [-PerSlot 3] [-DeployRoot <dir>]
param(
    [Parameter(Mandatory = $true)][int]$Generation,
    [int]$PerSlot = 3,
    [string]$DeployRoot = 'K:\Mo2_SkyrimSE1170\mods\CostumeFW_BoxPool',
    [switch]$NoDeploy
)
$ErrorActionPreference = 'Stop'
if ($Generation -lt 1) { throw "Generation must be >= 1 (generation 0 is CostumeFW.esp)." }

$repo = Split-Path $PSScriptRoot -Parent
$meshRoot = Join-Path $repo 'package_assets\meshes\CostumeFW'
$xmlRoot = Join-Path $meshRoot 'XML'
$nifTemplate = Join-Path $meshRoot 'boxtoken.nif'
$xmlTemplate = Join-Path $xmlRoot 'Box44_physics_r0.xml'
foreach ($t in @($nifTemplate, $xmlTemplate)) {
    if (-not (Test-Path -LiteralPath $t)) { throw "missing template: $t" }
}

# The token local ids the generator assigns: ARMO at 0x800 + index*2, one index
# per (slot, ordinal). 27 slots is generation 0's token count and the number of
# slots every generation covers. Keep this in step with BuildBoxPool.
$slotCount = 27
$revisions = 0..7
$gg = '{0:D2}' -f $Generation

$made = 0
for ($i = 0; $i -lt ($slotCount * $PerSlot); ++$i) {
    $armoId = 0x800 + $i * 2
    if (($armoId + 1) -gt 0xFFF) { throw "generation $Generation does not fit the ESL range at -PerSlot $PerSlot" }
    $key = 'BP{0}_{1:X6}' -f $gg, $armoId
    Copy-Item -LiteralPath $nifTemplate -Destination (Join-Path $meshRoot "${key}_carrier.nif") -Force
    foreach ($rev in $revisions) {
        Copy-Item -LiteralPath $nifTemplate -Destination (Join-Path $meshRoot "${key}_carrier_r${rev}.nif") -Force
        Copy-Item -LiteralPath $xmlTemplate -Destination (Join-Path $xmlRoot "${key}_physics_r${rev}.xml") -Force
    }
    $made += 1 + ($revisions.Count * 2)
}
Write-Host "box pool $Generation placeholders: $made file(s) for $($slotCount * $PerSlot) token(s)"

if ($DeployRoot -and -not $NoDeploy) {
    $deployMeshes = Join-Path $DeployRoot 'meshes\CostumeFW'
    New-Item -ItemType Directory -Force (Join-Path $deployMeshes 'XML') | Out-Null
    Copy-Item -Path (Join-Path $meshRoot "BP${gg}_*") -Destination $deployMeshes -Force
    Copy-Item -Path (Join-Path $xmlRoot "BP${gg}_*") -Destination (Join-Path $deployMeshes 'XML') -Force
    Write-Host "deployed -> $deployMeshes"
}
