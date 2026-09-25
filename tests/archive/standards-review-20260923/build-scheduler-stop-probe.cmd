@echo off
rem 测试项目：历史新旧 scheduler 取消对照构建
rem 编译同目录 scheduler_stop_probe.cpp, 使用固定 stdexec 头文件。
rem 预期: 生成 scheduler_stop_probe.exe 并零退出, 本文件不执行比较。
rem 入口: 直接运行本文件后执行 run-scheduler-stop-probe.cmd; 旧 exec::task 仅作不兼容对照。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standards-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/standards-review-20260923/scheduler_stop_probe.exe /Fobuild/standards-review-20260923/scheduler_stop_probe.obj tests/archive/standards-review-20260923/scheduler_stop_probe.cpp
exit /b %errorlevel%
