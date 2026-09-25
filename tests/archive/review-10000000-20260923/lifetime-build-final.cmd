@echo off
rem 测试项目：历史 callback affinity 最终版 ASan 构建
rem 编译正式 stdexec_callback_affine_lifetime_test.cpp 为 lifetime-final.exe。
rem 预期: 编译成功并保留调试符号; 本文件不执行四类生命周期交错。
rem 入口: 直接运行本文件, 随后由同目录 lifetime-run-final.cmd 执行。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/review-10000000-20260923/lifetime-final.exe /Fobuild/review-10000000-20260923/lifetime-final.obj /Fdbuild/review-10000000-20260923/lifetime-final.pdb tests/stdexec_callback_affine_lifetime_test.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
exit /b %errorlevel%
