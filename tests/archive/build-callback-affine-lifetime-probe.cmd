@echo off
rem 测试项目：build-callback-affine-lifetime-probe.cmd
rem 只编译 stdexec_callback_affine_lifetime_test 的优化版, 验证依赖回调/affine 生命周期探针可构建.
rem 预期: 编译链接成功, 不运行产物; 需要 VS 2022 和 build/msvc-debug 的 stdexec, 输出保留 build.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Ibuild/msvc-debug/_deps/stdexec-src/include /Febuild/stdexec_callback_affine_lifetime_probe.exe /Fobuild/stdexec_callback_affine_lifetime_probe.obj tests/stdexec_callback_affine_lifetime_test.cpp
exit /b %errorlevel%
