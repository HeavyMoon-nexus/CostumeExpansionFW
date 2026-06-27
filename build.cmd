@echo off
rem Build entry for CostumeExpansionFW (CommonLibSSE-NG / vcpkg / Ninja / MSVC).
rem Run from a plain shell; this sets up the MSVC + CMake/Ninja environment itself.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"

rem Put VS-bundled CMake + Ninja on PATH (sequential batch lines = no parse-time
rem %PATH% capture bug), THEN call vcvars64 which prepends the MSVC toolset.
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
rem NOTE: vcvars64 forces VCPKG_ROOT to the VS-bundled vcpkg. The project was first
rem installed with it, so we keep it for cache reuse. To use a different vcpkg,
rem set VCPKG_ROOT AFTER this line.

rem Auto-deploy target: CMake copies the built .dll to
rem   %SKYRIM_MODS_FOLDER%\CostumeExpansionFW\SKSE\Plugins\
rem so MO2 sees it as a mod. Override by setting SKYRIM_MODS_FOLDER before calling.
if "%SKYRIM_MODS_FOLDER%"=="" set "SKYRIM_MODS_FOLDER=K:\Mo2_SkyrimSE1170\mods"

cd /d "%~dp0"

set "PRESET=%1"
if "%PRESET%"=="" set "PRESET=release"

echo === CONFIGURE (%PRESET%) ===
cmake --preset %PRESET% || exit /b 1
echo === BUILD (%PRESET%) ===
cmake --build build/%PRESET% || exit /b 1
echo === DONE: produced DLL(s) ===
dir /b build\%PRESET%\*.dll
