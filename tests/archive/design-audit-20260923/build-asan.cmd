@echo off
rem 测试项目：历史七程序设计审计 ASan 构建
rem 编译状态模型、stop callback、callback hook、旧 task 以及新 task API/生命周期/调度程序。
rem 预期: 每个目标编译成功, 首个失败中止; 需要固定依赖和生成的 stop-token 插桩头。
rem 入口: 直接运行本文件, 随后由同目录 run-asan.cmd 执行; 不含真实 Pause 全量验收。
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
for %%T in (pause_state_model_test stdexec_stop_callback_test stdexec_stop_callback_hook_test stdexec_task_lifetime_test stdexec_standard_task_api_test stdexec_standard_task_lifetime_test stdexec_standard_task_scheduler_test) do (
    cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Ibuild/msvc-debug/_deps/stdexec-src/include/stdexec /Ibuild/msvc-debug/pause-test-hooks /Febuild/design-audit-20260923/%%T.exe /Fobuild/design-audit-20260923/%%T.obj /Fdbuild/design-audit-20260923/%%T.pdb tests/%%T.cpp /link /INCREMENTAL:NO
    if errorlevel 1 exit /b !errorlevel!
)
exit /b 0
