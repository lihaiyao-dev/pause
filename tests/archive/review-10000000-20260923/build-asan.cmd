@echo off
rem 测试项目：历史九程序 ASan 构建
rem 编译状态模型、stop callback、插桩窗口、callback affinity、新旧 task API/生命周期/调度与深链依赖测试。
rem 预期: 每个编译成功, 首个失败返回非零; 本脚本不执行测试。
rem 入口: 直接运行本文件, 需要 VS C++、已下载的固定 stdexec 与生成的 pause-test-hooks。
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
for %%T in (pause_state_model_test stdexec_stop_callback_test stdexec_stop_callback_hook_test stdexec_callback_affine_lifetime_test stdexec_task_lifetime_test stdexec_standard_task_api_test stdexec_standard_task_lifetime_test stdexec_standard_task_scheduler_test stdexec_deep_chain_test) do (
    cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Ibuild/msvc-debug/_deps/stdexec-src/include/stdexec /Ibuild/msvc-debug/pause-test-hooks /Febuild/review-10000000-20260923/%%T.exe /Fobuild/review-10000000-20260923/%%T.obj /Fdbuild/review-10000000-20260923/%%T.pdb tests/%%T.cpp /link /INCREMENTAL:NO
    if errorlevel 1 exit /b !errorlevel!
)
exit /b 0
