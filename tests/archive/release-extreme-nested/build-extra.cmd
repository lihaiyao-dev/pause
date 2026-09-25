@echo off
rem 测试项目：release-extreme-nested/build-extra.cmd
rem 编译归档 pause_external_probe 及正式 pause_extreme_test 的 ASan 版本, 不执行.
rem 预期: 两次编译成功; 外部探针保留旧 pause:: API, 需匹配历史头文件, 不能直接兼容当前 coop_pause API.
rem 需要 VS 2022 ASan/stdexec; 源码输入迁至 tests, 产物保留 build/release-extreme-nested.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /I. /Ibuild\msvc-debug\_deps\stdexec-src\include /Febuild\release-extreme-nested\pause_external_probe.exe /Fobuild\release-extreme-nested\pause_external_probe.obj tests\archive\release-extreme-nested\pause_external_probe.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /Zi /fsanitize=address /I. /Ibuild\msvc-debug\_deps\stdexec-src\include /Febuild\release-extreme-nested\asan\pause_extreme_test.exe /Fobuild\release-extreme-nested\asan\pause_extreme_test.obj /Fdbuild\release-extreme-nested\asan\pause_extreme_test.pdb tests\pause_extreme_test.cpp /link /INCREMENTAL:NO
exit /b %errorlevel%
