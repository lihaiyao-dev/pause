@echo off
rem 测试项目：历史八场景终止组合构建与验证
rem 以 ASan 编译 terminate-composition-probe.cpp, 由隔离进程验证 body 与宿主错误终止。
rem 预期: 编译成功且八场景均匹配退出 86、TERMINATE_OK 与空 stderr。
rem 入口: 直接运行本文件; 仅组合探针, 不替代真实 Pause 终止测试。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/review-10000000-20260923/terminate-composition-probe.exe /Fobuild/review-10000000-20260923/terminate-composition-probe.obj /Fdbuild/review-10000000-20260923/terminate-composition-probe.pdb tests/archive/review-10000000-20260923/terminate-composition-probe.cpp /link /INCREMENTAL:NO >build/review-10000000-20260923/terminate-probe.build.log 2>&1
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/review-10000000-20260923/run-terminate-probe.ps1 >build/review-10000000-20260923/terminate-probe.run.log 2>&1
exit /b %errorlevel%
