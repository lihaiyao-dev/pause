@echo off
rem 测试项目：resource-review/run-no-proxy.cmd
rem 关闭 MSVC Debug 迭代器代理重建资源测试并执行 --iterations 1000.
rem 预期: 编译及资源矩阵通过; 历史压力入口无单项超时, 需要 VS 2022 和既有 stdexec.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MDd /Od /D_ITERATOR_DEBUG_LEVEL=0 /Zi /I. /Ibuild/msvc-debug/_deps/stdexec-src/include tests/pause_resource_test.cpp /Febuild/resource-review/pause_resource_test.exe /Fobuild/resource-review/pause_resource_test.obj /Fdbuild/resource-review/pause_resource_test.pdb /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
build\resource-review\pause_resource_test.exe --iterations 1000
