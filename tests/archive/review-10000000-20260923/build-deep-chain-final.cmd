@echo off
rem 测试项目：历史深链 ASan 构建与定深度回归
rem 编译 tests/stdexec_deep_chain_test.cpp 并调用同目录 run-deep-chain-final.ps1。
rem 预期: 编译成功且默认 48 场景和深度 32/64 的 24 场景按运行器检查通过。
rem 入口: 直接运行本文件; 历史宿主依赖审计, 不是当前压力入口, 不扩大深度扫描。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal EnableExtensions
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/review-10000000-20260923/deep-chain-final.exe /Fobuild/review-10000000-20260923/deep-chain-final.obj /Fdbuild/review-10000000-20260923/deep-chain-final.pdb tests/stdexec_deep_chain_test.cpp /link /INCREMENTAL:NO >build/review-10000000-20260923/deep-chain-final-build.log 2>&1
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/review-10000000-20260923/run-deep-chain-final.ps1
exit /b %errorlevel%
