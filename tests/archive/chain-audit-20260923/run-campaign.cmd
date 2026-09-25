@echo off
rem 测试项目：历史深链分组扫描入口
rem 参数 model 扫描有界模型到 512 层; release 或 asan 调用 task 深链扫描运行器。
rem 预期: 脚本传播运行器退出码, 同时保留每场景与失败边界证据。
rem 限制: release/asan 沿用历史扫描参数, 可能触发已知递归栈极限; 不是当前压力入口, 本次未运行。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/chain-audit-20260923; 非 CTest 自动入口。
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL exit /b 1
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b !errorlevel!
if /i "%~1" == "model" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run-chain-stress.ps1 -Executable build/chain-audit-20260923/model-asan.exe -Label model-asan -Model -MaxDepth 512 -TimeoutSeconds 120
    exit /b !errorlevel!
)
if /i "%~1" == "release" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run-chain-stress.ps1 -Executable build/chain-audit-20260923/task-release.exe -Label task-release
    exit /b !errorlevel!
)
if /i "%~1" == "asan" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run-chain-stress.ps1 -Executable build/chain-audit-20260923/task-asan.exe -Label task-asan
    exit /b !errorlevel!
)
exit /b 2
