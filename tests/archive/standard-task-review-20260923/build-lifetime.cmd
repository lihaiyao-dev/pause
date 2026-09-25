@echo off
rem 测试项目：历史新 stdexec::task 生命周期双配置构建
rem 参数 asan 生成优化 ASan 程序, 其他参数生成 Debug 程序, 源码为正式 lifetime 测试。
rem 预期: 编译成功并保存对象和调试符号; 本文件不执行线程交错。
rem 入口: build-lifetime.cmd asan 或 build-lifetime.cmd debug, 随后配合 run-lifetime.cmd。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standard-task-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set "OUT=build/standard-task-review-20260923"
if /i "%~1" == "asan" (
    cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Fe%OUT%/standard_task_lifetime_asan.exe /Fo%OUT%/standard_task_lifetime_asan.obj /Fd%OUT%/standard_task_lifetime_asan.pdb tests/stdexec_standard_task_lifetime_test.cpp /link /INCREMENTAL:NO
) else (
    cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MDd /Od /Zi /Ibuild/msvc-debug/_deps/stdexec-src/include /Fe%OUT%/standard_task_lifetime_debug.exe /Fo%OUT%/standard_task_lifetime_debug.obj /Fd%OUT%/standard_task_lifetime_debug.pdb tests/stdexec_standard_task_lifetime_test.cpp
)
exit /b %errorlevel%
