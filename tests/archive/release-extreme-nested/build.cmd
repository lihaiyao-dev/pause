@echo off
rem 测试项目：release-extreme-nested/build.cmd
rem 以 MSVC Release /O2 /Ob2 编译正式 pause_extreme_test, 本脚本不执行.
rem 预期: 编译链接成功; 需要 VS 2022 与既有 stdexec, 产物留在 build/release-extreme-nested.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /I. /Ibuild\msvc-debug\_deps\stdexec-src\include /Febuild\release-extreme-nested\pause_extreme_test.exe /Fobuild\release-extreme-nested\pause_extreme_test.obj tests\pause_extreme_test.cpp /link /INCREMENTAL:NO
exit /b %errorlevel%
