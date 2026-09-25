@echo off
rem 测试项目：历史深链模型与真实 task 构建
rem 参数 model 编译 ASan 状态模型; 参数 task 编译 Release/ASan 深链探针并用 dumpbin 保存 PE 头。
rem 预期: 指定程序编译成功; PE 头供审查栈配置, 本脚本不运行递归边界探测。
rem 入口: build-tests.cmd model 或 build-tests.cmd task; 深链程序不含 pause.hpp。
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
    cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Febuild/chain-audit-20260923/model-asan.exe /Fobuild/chain-audit-20260923/model-asan.obj /Fdbuild/chain-audit-20260923/model-asan.pdb tests/pause_state_model_test.cpp /link /INCREMENTAL:NO
    exit /b !errorlevel!
)
if /i not "%~1" == "task" exit /b 2
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/chain-audit-20260923/task-release.exe /Fobuild/chain-audit-20260923/task-release.obj /Fdbuild/chain-audit-20260923/task-release.pdb tests/stdexec_deep_chain_test.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b !errorlevel!
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/chain-audit-20260923/task-asan.exe /Fobuild/chain-audit-20260923/task-asan.obj /Fdbuild/chain-audit-20260923/task-asan.pdb tests/stdexec_deep_chain_test.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b !errorlevel!
dumpbin /headers build/chain-audit-20260923/task-release.exe > build/chain-audit-20260923/task-release-headers.log
dumpbin /headers build/chain-audit-20260923/task-asan.exe > build/chain-audit-20260923/task-asan-headers.log
exit /b 0
