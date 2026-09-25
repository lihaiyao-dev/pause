@echo off
rem 测试项目：pause-acceptance-asan/run-debug-candidate.cmd
rem 离线配置 Debug 候选构建, 关闭 STL 迭代器代理并开启设计测试, 随后构建及执行完整 CTest.
rem 预期: 各阶段退出 0; 此结果不代表默认迭代器检查 Debug 配置. 需要 VS 2022/Ninja 和既有离线依赖.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal EnableExtensions
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cmake -S . -B build/msvc-acceptance-debug -G Ninja -DCMAKE_MAKE_PROGRAM=D:/Ninja/ninja.exe -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=/D_ITERATOR_DEBUG_LEVEL=0 -DPAUSE_BUILD_DESIGN_TESTS=ON -DFETCHCONTENT_SOURCE_DIR_STDEXEC=D:/workspace/pause/build/msvc-debug/_deps/stdexec-src -DFETCHCONTENT_SOURCE_DIR_ICM=D:/workspace/pause/build/msvc-debug/_deps/icm-src -DFETCHCONTENT_SOURCE_DIR_RAPIDS-CMAKE=D:/workspace/pause/build/msvc-debug/_deps/rapids-cmake-src
if errorlevel 1 exit /b %errorlevel%
cmake --build build/msvc-acceptance-debug --parallel 3
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build/msvc-acceptance-debug --output-on-failure --output-log D:/workspace/pause/build/msvc-acceptance-debug/ctest-final.log
exit /b %errorlevel%
