@echo off
rem 测试项目：历史设计审计 ASan 模型或依赖压力
rem 参数 model 运行 100 种子各 100000 模型事件; 否则 stop callback 两程序各 10000 轮、task 四程序各 5000 轮。
rem 预期: 运行器检查退出码和超时, 首个失败停止; 模型默认超时 1800 秒, 依赖程序 300 秒。
rem 入口: 先运行同目录 build-asan.cmd; 历史参数留作审计, 不把事件数等同真实 Pause 场景。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/design-audit-20260923; 非 CTest 自动入口。
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
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& .\tests\archive\design-audit-20260923\run-suite.ps1 -Name 'model-10m-asan' -Executable '.\build\design-audit-20260923\pause_state_model_test.exe' -TestArguments '--depth','10','--random-steps','100000','--seeds','100','--max-records','24'"
    exit /b !errorlevel!
)
for %%T in (stdexec_stop_callback_test stdexec_stop_callback_hook_test stdexec_task_lifetime_test stdexec_standard_task_api_test stdexec_standard_task_lifetime_test stdexec_standard_task_scheduler_test) do (
    set "ITERATIONS=5000"
    if "%%T" == "stdexec_stop_callback_test" set "ITERATIONS=10000"
    if "%%T" == "stdexec_stop_callback_hook_test" set "ITERATIONS=10000"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& .\tests\archive\design-audit-20260923\run-suite.ps1 -Name '%%T-asan' -Executable '.\build\design-audit-20260923\%%T.exe' -TestArguments '--iterations','!ITERATIONS!' -TimeoutSeconds 300"
    if errorlevel 1 exit /b !errorlevel!
)
exit /b 0
