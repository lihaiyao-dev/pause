@echo off
rem 测试项目：transition-extreme-asan/build-run.cmd
rem 编译正式状态转换测试的优化 ASan 版, 执行 11 类转换各 --iterations 1000.
rem 预期: 编译/测试退出 0 且无 ASan 诊断; 需要 VS 2022 ASan/stdexec, 日志留在 build/transition-extreme-asan.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /DNDEBUG /Zi /fsanitize=address /I. /Ibuild/msvc-debug/_deps/stdexec-src/include tests/pause_transition_extreme_test.cpp /Febuild/transition-extreme-asan/pause_transition_extreme_test.exe /Fobuild/transition-extreme-asan/pause_transition_extreme_test.obj /Fdbuild/transition-extreme-asan/pause_transition_extreme_test.pdb /link /INCREMENTAL:NO >build/transition-extreme-asan/build.log 2>&1
if errorlevel 1 exit /b %errorlevel%
build\transition-extreme-asan\pause_transition_extreme_test.exe --iterations 1000 >build/transition-extreme-asan/transition-1000.stdout.log 2>build/transition-extreme-asan/transition-1000.stderr.log
exit /b %errorlevel%
