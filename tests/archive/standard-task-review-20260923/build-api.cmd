@echo off
rem 测试项目：历史新 stdexec::task API ASan 构建
rem 编译正式 API/resource 探针, 覆盖返回值、错误停止、环境、token 与 frame 分配释放。
rem 预期: 编译生成 api-asan.exe 并零退出; 本文件不运行测试。
rem 入口: 直接运行本文件, 随后可由同目录 run-api.cmd 执行。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standard-task-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/standard-task-review-20260923/api-asan.exe /Fobuild/standard-task-review-20260923/api-asan.obj /Fdbuild/standard-task-review-20260923/api-asan.pdb tests/stdexec_standard_task_api_test.cpp
exit /b %errorlevel%
