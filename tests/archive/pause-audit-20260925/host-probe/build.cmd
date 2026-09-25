@echo off
rem 测试项目：宿主故障恢复 Debug 编译探针
rem 性质: 只构建 pause_host_failure_test.cpp, 使用现有 MSVC x64 和 stdexec.
rem 项目: 以 Debug CRT、运行时检查和调试链接选项验证宿主故障测试可以构建.
rem 预期: 编译成功并返回 0, exe、obj、pdb 保留原 build/host-probe 输出位置.
rem 运行: 从仓库根调用 tests\archive\pause-audit-20260925\host-probe\build.cmd.
rem 边界: 不自动执行测试, 不能由编译成功推断控制器异常场景已验证.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /TP /DSTDEXEC_ENABLE_WINDOWS_THREAD_POOL /I D:\workspace\pause /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-src\include /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-build\include /external:W0 /EHsc /Zi /Ob0 /Od /RTC1 /std:c++latest /MDd /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /FoD:\workspace\pause\build\pause-audit-20260925\host-probe\probe.obj /FdD:\workspace\pause\build\pause-audit-20260925\host-probe\probe.pdb D:\workspace\pause\tests\pause_host_failure_test.cpp /FeD:\workspace\pause\build\pause-audit-20260925\host-probe\pause_host_failure_test.exe /link /DEBUG:FULL /INCREMENTAL:NO
exit /b %errorlevel%
