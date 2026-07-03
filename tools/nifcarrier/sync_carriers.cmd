@echo off
REM Rebuild FSMP physics carriers from CEF's manifest (approach B, merged carriers).
REM Run AFTER changing box contents in-game (MCM), BEFORE the next game start.
REM
REM Path resolution: content NIFs are resolved against the --data roots below.
REM Best run through MO2 (add as an MO2 executable) so the VFS presents every
REM mod under one Data dir - then a single --data "<Game>\Data" suffices.
REM Standalone: list every mod dir that ships SMP contents you use in boxes.
setlocal
set DLL=%~dp0bin\Release\net9.0\nifcarrier.dll
if not exist "%DLL%" (
  echo nifcarrier is not built. Run: dotnet build -c Release
  exit /b 1
)
REM --mo2 reads the profile modlist.txt (enabled mods, winners-first) so NO
REM per-mod --data maintenance is needed when boxes gain contents from new mods.
dotnet "%DLL%" sync "K:\Mo2_SkyrimSE1170\overwrite\SKSE\plugins\CEF_carrier_manifest.json" ^
  --mo2 "K:\Mo2_SkyrimSE1170" --profile Default ^
  --out "K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW" ^
  --empty "K:\Mo2_SkyrimSE1170\mods\CostumeExpansionFW\meshes\CostumeFW\boxtoken.nif"
pause
