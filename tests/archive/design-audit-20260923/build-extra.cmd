@echo off
rem 测试项目：历史 callback affinity 补充 ASan 验证
rem 编译正式 callback affinity 测试后, 通过 run-suite.ps1 重复 10000 轮, 超时 120 秒。
rem 预期: 编译与进程均零退出且未超时, 日志由运行器保存。
rem 入口: 直接运行本文件; 仅真实依赖组合的生命周期证据, 不替代 Pause 状态机测试。
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
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/design-audit-20260923/stdexec_callback_affine_lifetime_test.exe /Fobuild/design-audit-20260923/stdexec_callback_affine_lifetime_test.obj /Fdbuild/design-audit-20260923/stdexec_callback_affine_lifetime_test.pdb tests/stdexec_callback_affine_lifetime_test.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b !errorlevel!
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& .\tests\archive\design-audit-20260923\run-suite.ps1 -Name 'callback-affine-asan' -Executable '.\build\design-audit-20260923\stdexec_callback_affine_lifetime_test.exe' -TestArguments '--iterations','10000' -TimeoutSeconds 120"
exit /b !errorlevel!
