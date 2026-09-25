@echo off
rem 测试项目：release-extreme-extra-asan/build.cmd
rem 以 MSVC Release + ASan 编译 pause_sender_test/pause_resource_test, 本脚本不运行.
rem 预期: 两次编译成功; 需要 VS 2022 ASan/stdexec 和既有输出目录, 产物留在 build/release-extreme-extra-asan.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /Zi /fsanitize=address /I"." /I"build\msvc-debug\_deps\stdexec-src\include" tests\pause_sender_test.cpp /Fo"build\release-extreme-extra-asan\pause_sender_test.obj" /Fd"build\release-extreme-extra-asan\pause_sender_test.pdb" /Fe"build\release-extreme-extra-asan\pause_sender_test.exe" /link /INCREMENTAL:NO 1>build\release-extreme-extra-asan\pause_sender_test.build.log 2>&1
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /Zi /fsanitize=address /I"." /I"build\msvc-debug\_deps\stdexec-src\include" tests\pause_resource_test.cpp /Fo"build\release-extreme-extra-asan\pause_resource_test.obj" /Fd"build\release-extreme-extra-asan\pause_resource_test.pdb" /Fe"build\release-extreme-extra-asan\pause_resource_test.exe" /link /INCREMENTAL:NO 1>build\release-extreme-extra-asan\pause_resource_test.build.log 2>&1
exit /b %errorlevel%
