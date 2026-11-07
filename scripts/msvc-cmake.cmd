@echo off
setlocal EnableExtensions

set "PAUSE_ACTION=%~1"
if /i not "%PAUSE_ACTION%" == "configure" if /i not "%PAUSE_ACTION%" == "build" if /i not "%PAUSE_ACTION%" == "test" if /i not "%PAUSE_ACTION%" == "examples" if /i not "%PAUSE_ACTION%" == "release" goto usage
if not "%~3" == "" goto usage
set "PAUSE_CONFIG=%~2"
if not defined PAUSE_CONFIG (
    if /i "%PAUSE_ACTION%" == "test" (
        set "PAUSE_CONFIG=all"
    ) else if /i "%PAUSE_ACTION%" == "release" (
        set "PAUSE_CONFIG=release"
    ) else (
        set "PAUSE_CONFIG=debug"
    )
)
if /i not "%PAUSE_CONFIG%" == "debug" if /i not "%PAUSE_CONFIG%" == "release" if /i not "%PAUSE_CONFIG%" == "all" goto usage
if /i "%PAUSE_CONFIG%" == "debug" set "PAUSE_CONFIG=debug"
if /i "%PAUSE_CONFIG%" == "release" set "PAUSE_CONFIG=release"
if /i "%PAUSE_ACTION%" == "release" if /i not "%PAUSE_CONFIG%" == "release" goto usage

set "PAUSE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%PAUSE_VSWHERE%" (
    echo Visual Studio Installer's vswhere.exe was not found. 1>&2
    exit /b 1
)

set "PAUSE_VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%PAUSE_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PAUSE_VSINSTALL=%%I"

if not defined PAUSE_VSINSTALL (
    echo A Visual Studio installation with the MSVC x64 tools was not found. 1>&2
    exit /b 1
)

call "%PAUSE_VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

pushd "%~dp0.."
if errorlevel 1 exit /b %errorlevel%
set "PAUSE_REPO=%CD:\=/%"
if /i "%PAUSE_CONFIG%" == "all" (
    call :run_config debug
    if errorlevel 1 goto failed
    call :run_config release
) else (
    call :run_config %PAUSE_CONFIG%
)
if errorlevel 1 goto failed
popd
exit /b 0

:run_config
if /i "%PAUSE_ACTION%" == "release" goto release
cmake --preset msvc-%~1
if errorlevel 1 exit /b %errorlevel%
if /i "%PAUSE_ACTION%" == "configure" exit /b 0
if /i "%PAUSE_ACTION%" == "examples" goto examples
cmake --build --preset msvc-%~1 --parallel 2
if errorlevel 1 exit /b %errorlevel%
if /i "%PAUSE_ACTION%" == "build" exit /b 0
ctest --preset msvc-%~1 --output-junit "%PAUSE_REPO%/build/msvc-%~1/ctest.xml"
exit /b %errorlevel%

:examples
cmake --build --preset msvc-%~1 --parallel 2 --target pause_simple_workflow pause_thread_pool pause_cancellation pause_sender_workflow
exit /b %errorlevel%

:release
python scripts/check-repository.py
if errorlevel 1 exit /b %errorlevel%
python -m unittest discover -s tests/repository -p test_repository_checks.py
if errorlevel 1 exit /b %errorlevel%
set "PAUSE_ACTION=test"
call :run_config release
if errorlevel 1 exit /b %errorlevel%
for /f "tokens=1,* delims==" %%A in ('findstr /b /l /c:"STDEXEC_SOURCE_DIR:STATIC=" "build\msvc-release\CMakeCache.txt"') do if not defined PAUSE_STDEXEC_SOURCE set "PAUSE_STDEXEC_SOURCE=%%B"
if not exist "%PAUSE_STDEXEC_SOURCE%/CMakeLists.txt" (
    echo Set PAUSE_STDEXEC_SOURCE to the pinned stdexec checkout for package validation. 1>&2
    exit /b 1
)
set "PAUSE_VALIDATION=%PAUSE_REPO%/build/release-validation"
set "PAUSE_RUN=%PAUSE_VALIDATION%/run-%RANDOM%-%RANDOM%"
set "PAUSE_DEPENDENCY_RUN=%PAUSE_RUN%-dependency"
cmake -S "%PAUSE_STDEXEC_SOURCE%" -B "%PAUSE_DEPENDENCY_RUN%/build" -G Ninja -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=Release -DSTDEXEC_INSTALL=ON -DSTDEXEC_BUILD_TESTS=OFF -DSTDEXEC_BUILD_EXAMPLES=OFF -DSTDEXEC_BUILD_PARALLEL_SCHEDULER=OFF
if errorlevel 1 exit /b %errorlevel%
cmake --build "%PAUSE_DEPENDENCY_RUN%/build" --parallel 2
if errorlevel 1 exit /b %errorlevel%
cmake --install "%PAUSE_DEPENDENCY_RUN%/build" --prefix "%PAUSE_DEPENDENCY_RUN%/prefix"
if errorlevel 1 exit /b %errorlevel%
cmake "-DPAUSE_BINARY_DIR=%PAUSE_REPO%/build/msvc-release" "-DSTDEXEC_PREFIX=%PAUSE_DEPENDENCY_RUN%/prefix" "-DOUTPUT_DIRECTORY=%PAUSE_RUN%" -P tests/packaging/package_integration_test.cmake
if errorlevel 1 exit /b %errorlevel%
echo Release artifacts: %PAUSE_RUN%/archives
exit /b 0

:failed
set "PAUSE_EXIT=%errorlevel%"
popd
exit /b %PAUSE_EXIT%

:usage
echo Usage: scripts\msvc-cmake.cmd configure^|build^|test^|examples [debug^|release^|all] 1>&2
echo        scripts\msvc-cmake.cmd release 1>&2
echo Default: configure/build/examples debug; test all; release validates and packages Release. 1>&2
exit /b 2
