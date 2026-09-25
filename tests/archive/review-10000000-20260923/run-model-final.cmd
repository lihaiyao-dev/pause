@echo off
rem 测试项目：历史最终模型 ASan 定量复核
rem 对已有 state-review-model.exe 执行 20 种子各 10000 事件、搜索深度 10、最多 24 records 和链深度 64。
rem 预期: 300 秒内以零退出结束, 由 run-suite.ps1 保存日志及结果。
rem 入口: 需先准备该历史可执行文件; 本命令不编译, 模型事件不是实际并发或生命周期证明。
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& .\tests\archive\review-10000000-20260923\run-suite.ps1 -Name 'model-final-200k-asan' -Executable '.\build\review-10000000-20260923\state-review-model.exe' -TestArguments '--depth','10','--random-steps','10000','--seeds','20','--max-records','24','--chain-depth','64' -TimeoutSeconds 300"
exit /b !errorlevel!
