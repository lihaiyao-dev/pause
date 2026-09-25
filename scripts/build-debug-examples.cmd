@echo off
setlocal EnableExtensions

rem VS Code/Cursor may start without the MSVC INCLUDE/LIB environment.
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
cmake --preset msvc-debug -DPAUSE_BUILD_EXAMPLES=ON
if errorlevel 1 goto failed
cmake --build --preset msvc-debug --target pause_simple_workflow pause_controlled_workflow pause_workflows pause_independent_jobs
if errorlevel 1 goto failed
popd
exit /b 0

:failed
set "PAUSE_EXIT=%errorlevel%"
popd
exit /b %PAUSE_EXIT%
