@echo off
rem 测试项目：task_scheduler 布局探针构建
rem 性质: MSVC Debug 编译期布局检查, 只构建同目录 layout.cpp.
rem 项目: 启用 __value_model 类布局报告, 检查内部调度状态尺寸、对齐与 noexcept 静态断言.
rem 预期: 编译链接成功, layout.exe、对象和调试信息仍写到原 build/layout-probe 目录.
rem 运行: 从仓库根调用 tests\archive\pause-audit-20260925\layout-probe\build.cmd.
rem 边界: 保留固定 MSVC 和 stdexec 依赖路径, 脚本不运行生成的观测程序.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /TP /d1reportSingleClassLayout__value_model /DSTDEXEC_ENABLE_WINDOWS_THREAD_POOL /I D:\workspace\pause /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-src\include /external:I D:\workspace\pause\build\msvc-debug\_deps\stdexec-build\include /external:W0 /EHsc /Zi /Ob0 /Od /RTC1 /std:c++latest /MDd /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /FoD:\workspace\pause\build\pause-audit-20260925\layout-probe\probe.obj /FdD:\workspace\pause\build\pause-audit-20260925\layout-probe\probe.pdb D:\workspace\pause\tests\archive\pause-audit-20260925\layout-probe\layout.cpp /FeD:\workspace\pause\build\pause-audit-20260925\layout-probe\layout.exe /link /DEBUG:FULL /INCREMENTAL:NO
exit /b %errorlevel%
