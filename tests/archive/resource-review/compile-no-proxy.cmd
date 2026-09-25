@echo off
rem 测试项目：resource-review/compile-no-proxy.cmd
rem 以 /MDd /Od /D_ITERATOR_DEBUG_LEVEL=0 编译资源测试, 执行 --iterations 1.
rem 预期: 去除迭代器代理后资源错误测试通过; 不代替默认 Debug 代理行为, 需要 VS 2022 和既有 stdexec.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MDd /Od /D_ITERATOR_DEBUG_LEVEL=0 /Zi /I. /Ibuild/msvc-debug/_deps/stdexec-src/include tests/pause_resource_test.cpp /Febuild/resource-review/pause_resource_test.exe /Fobuild/resource-review/pause_resource_test.obj /Fdbuild/resource-review/pause_resource_test.pdb /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
build\resource-review\pause_resource_test.exe --iterations 1
