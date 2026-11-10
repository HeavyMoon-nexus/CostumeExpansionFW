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

rem English compiler messages. Ninja learns which headers each .cpp includes by
rem reading cl's /showIncludes lines, and it only recognizes the prefix CMake
rem recorded at configure time ("Note: including file:"). A Japanese-locale cl
rem prints that note in Japanese instead, Ninja records no header at all, and
rem editing a header then rebuilds only the .cpp files that changed too -
rem the rest link against code compiled from the OLD header (found 2026-09-23 as
rem an unresolved ReturnStoredItem; a changed inline function would not even
rem fail to link).
set "VSLANG=1033"

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

rem Release builds are what ships, so the host tests gate them: a failure here
rem fails the build instead of waiting for someone to remember to run them
rem (review 2026-09-23 M13). The DLL is already deployed by this point - the
rem exit code is what tells the caller not to trust it.
if /i "%PRESET%"=="release" (
    echo === HOST TESTS ===
    build\release\policy_tests.exe || exit /b 1
)
