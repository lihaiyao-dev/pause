@echo off
rem 测试项目：历史 Debug 构建与回归收尾
rem 构建 build/msvc-debug, 执行其 CTest, 再显式运行工具链 smoke 与头文件编译 smoke。
rem 预期: 所有命令零退出, 任一步失败立即传播错误码。
rem 入口: 直接运行本文件, 前提是已有正确配置的 Debug 构建目录; 不包含额外压力扫描。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL exit /b 1
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b !errorlevel!
cmake --build build/msvc-debug
if errorlevel 1 exit /b !errorlevel!
ctest --test-dir build/msvc-debug --output-on-failure
if errorlevel 1 exit /b !errorlevel!
build\msvc-debug\pause_toolchain_smoke.exe
if errorlevel 1 exit /b !errorlevel!
build\msvc-debug\pause_header_compile.exe
exit /b !errorlevel!
