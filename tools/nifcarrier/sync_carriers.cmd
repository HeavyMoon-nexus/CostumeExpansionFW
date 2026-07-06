@echo off
REM LEGACY / FALLBACK (v1.2+): the carrier build runs in-proc inside the CEF DLL
REM by default. This script is only needed when CEF_sync_command.txt exists
REM (external-tool compat mode) or for running the C# oracle outside the game.
REM
REM Rebuild CEF's FSMP physics carriers from the in-game manifest.
REM Run AFTER changing box contents in-game (MCM), BEFORE the next game start -
REM or let CEF's auto-sync run this for you (Data\SKSE\Plugins\CEF_sync_command.txt
REM containing:  "<full path to this script>" auto  ).
REM
REM Local setup: copy this file to sync_carriers.local.cmd (gitignored), edit the
REM two paths below there, and point CEF_sync_command.txt at the .local copy.

REM ==== EDIT THESE TWO LINES FOR YOUR SETUP =================================
set MO2=C:\Modding\MO2
set PROFILE=Default
REM ==========================================================================

setlocal
set CEFMOD=%MO2%\mods\CostumeExpansionFW
set DLL=%~dp0bin\Release\net9.0\nifcarrier.dll
if not exist "%DLL%" (
  echo nifcarrier is not built. Run: dotnet build -c Release
  exit /b 1
)
REM --mo2 reads the profile's modlist.txt (enabled mods, winners-first), so no
REM per-mod path maintenance is needed when boxes gain contents from new mods.
REM Non-MO2 setups: replace --mo2/--profile with one or more --data "<dir>" roots.
dotnet "%DLL%" sync "%MO2%\overwrite\SKSE\plugins\CEF_carrier_manifest.json" ^
  --mo2 "%MO2%" --profile %PROFILE% ^
  --out "%CEFMOD%" ^
  --empty "%CEFMOD%\meshes\CostumeFW\boxtoken.nif"
REM keep the window open only for manual runs (CEF auto-sync passes "auto")
if "%~1"=="" pause
