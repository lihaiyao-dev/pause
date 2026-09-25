@echo off
rem 测试项目：并发压力场景 Debug 编译探针
rem 性质: 只构建 pause_concurrency_stress_test.cpp, 不执行大基数压力.
rem 项目: 初始化固定 MSVC x64, 用 Debug CRT 和运行时检查编译并发测试, 链接调试信息.
rem 预期: 编译退出 0, 输出 exe、obj、pdb 保留在原 build/new-probe 路径.
rem 运行: 从仓库根调用 tests\archive\pause-audit-20260925\new-probe\build.cmd.
rem 边界: 依赖已有 stdexec 源及构建 include, 编译通过不构成并发正确性结论.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /TP /DSTDEXEC_ENABLE_WINDOWS_THREAD_POOL /I D:\workspace\pause /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-src\include /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-build\include /external:W0 /EHsc /Zi /Ob0 /Od /RTC1 /std:c++latest /MDd /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /FoD:\workspace\pause\build\pause-audit-20260925\new-probe\probe.obj /FdD:\workspace\pause\build\pause-audit-20260925\new-probe\probe.pdb D:\workspace\pause\tests\pause_concurrency_stress_test.cpp /FeD:\workspace\pause\build\pause-audit-20260925\new-probe\pause_concurrency_stress_test.exe /link /DEBUG:FULL /INCREMENTAL:NO
exit /b %errorlevel%
