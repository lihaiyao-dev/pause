@echo off
rem 测试项目：外层 operation 生命周期 Debug 编译探针
rem 性质: 调用固定 MSVC x64 环境, 只编译 pause_outer_lifetime_test.cpp.
rem 项目: 使用 Debug CRT、运行时检查和固定 stdexec 头文件构建生命周期测试可执行程序.
rem 预期: cl 编译链接成功并保留原 build 目录下的 exe、obj、pdb, 返回编译退出码.
rem 运行: 从仓库根调用 tests\archive\pause-audit-20260925\outer-probe\build.cmd.
rem 边界: 脚本不运行测试, 编译成功不等于生命周期场景通过; 保留原机器工具链路径.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /TP /DSTDEXEC_ENABLE_WINDOWS_THREAD_POOL /I D:\workspace\pause /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-src\include /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-build\include /external:W0 /EHsc /Zi /Ob0 /Od /RTC1 /std:c++latest /MDd /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /FoD:\workspace\pause\build\pause-audit-20260925\outer-probe\probe.obj /FdD:\workspace\pause\build\pause-audit-20260925\outer-probe\probe.pdb D:\workspace\pause\tests\pause_outer_lifetime_test.cpp /FeD:\workspace\pause\build\pause-audit-20260925\outer-probe\pause_outer_lifetime_test.exe /link /DEBUG:FULL /INCREMENTAL:NO
exit /b %errorlevel%
