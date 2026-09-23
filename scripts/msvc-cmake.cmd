@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PAUSE_ACTION=%~1"
if /i not "%PAUSE_ACTION%" == "configure" if /i not "%PAUSE_ACTION%" == "build" if /i not "%PAUSE_ACTION%" == "test" goto usage
if not "%~3" == "" goto usage
set "PAUSE_CONFIG=%~2"
if not defined PAUSE_CONFIG (
    if /i "%PAUSE_ACTION%" == "test" (set "PAUSE_CONFIG=all") else (set "PAUSE_CONFIG=debug")
)
if /i "%PAUSE_CONFIG%" == "debug" (
    set "PAUSE_CONFIGS=debug"
) else if /i "%PAUSE_CONFIG%" == "release" (
    set "PAUSE_CONFIGS=release"
) else if /i "%PAUSE_CONFIG%" == "all" (
    set "PAUSE_CONFIGS=debug release"
) else (
    goto usage
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer's vswhere.exe was not found. 1>&2
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"

if not defined VSINSTALL (
    echo A Visual Studio installation with the MSVC x64 tools was not found. 1>&2
    exit /b 1
)

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

pushd "%~dp0.."
if errorlevel 1 exit /b %errorlevel%
for %%C in (%PAUSE_CONFIGS%) do (
    call :run_config %%C
    if errorlevel 1 (
        set "PAUSE_EXIT=!errorlevel!"
        popd
        exit /b !PAUSE_EXIT!
    )
)
popd
exit /b 0

:run_config
if /i "%PAUSE_ACTION%" == "configure" (
    cmake --preset msvc-%~1
    exit /b !errorlevel!
)
if /i "%PAUSE_ACTION%" == "build" (
    cmake --build --preset msvc-%~1
    exit /b !errorlevel!
)
cmake --preset msvc-%~1
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset msvc-%~1
if errorlevel 1 exit /b %errorlevel%
ctest --preset msvc-%~1
exit /b %errorlevel%

:usage
echo Usage: scripts\msvc-cmake.cmd configure^|build^|test [debug^|release^|all] 1>&2
echo Default: configure/build debug; test all. 1>&2
exit /b 2
