@echo off
rem 测试项目：已合并生命周期原型的历史构建运行记录
rem 历史上编译 lifetime-terminal-inflight.cpp, 运行两类 terminal 未返回时析构交错各 1000 轮。
rem 预期: 原型当时无重复完成或析构后访问; 该实现后来已合入正式 callback affinity 测试。
rem 限制: 当前原型包含正式源码会重复定义, 本入口只供审计, 不应当作当前构建或压力入口。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/review-10000000-20260923/lifetime-terminal-inflight.exe /Fobuild/review-10000000-20260923/lifetime-terminal-inflight.obj /Fdbuild/review-10000000-20260923/lifetime-terminal-inflight.pdb tests/archive/review-10000000-20260923/lifetime-terminal-inflight.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
build\review-10000000-20260923\lifetime-terminal-inflight.exe --iterations 1000
exit /b %errorlevel%
