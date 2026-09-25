@echo off
rem 测试项目：历史深链 ASan 崩溃调试入口
rem 配置 MSVC 与符号路径后调用 debug-asan.ps1, 定位历史栈溢出或访问异常。
rem 预期: 保存 CDB 异常和调用栈证据, 调试器退出码不能作为测试通过判据。
rem 入口: 需已有 task-asan.exe、PDB 和 Windows CDB; 仅历史诊断, 不是当前压力入口, 本次未运行。
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
set "_NT_SYMBOL_PATH=%CD%\build\chain-audit-20260923"
set "_NT_ALT_SYMBOL_PATH="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/chain-audit-20260923/debug-asan.ps1
exit /b !errorlevel!
