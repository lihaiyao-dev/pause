@echo off
rem 测试项目：历史新 task 调度协议 ASan 构建
rem 编译正式 stdexec_standard_task_scheduler_test.cpp, 保存 ASan 程序及调试符号。
rem 预期: 编译零退出; 此步不检查运行期调度次数。
rem 入口: 直接运行本文件, 再由 run-scheduler-asan.cmd 执行; 协议探针未实现 Pause terminate 策略。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/task-adoption-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/task-adoption-20260923/scheduler-asan.exe /Fobuild/task-adoption-20260923/scheduler-asan.obj /Fdbuild/task-adoption-20260923/scheduler-asan.pdb tests/stdexec_standard_task_scheduler_test.cpp
exit /b %errorlevel%
