@echo off
rem 测试项目：stdexec-cmake-check-20260924/run-extra.cmd
rem 启用 STDEXEC_ENABLE_EXTRA_TYPE_CHECKING, 用 latest-main 离线快照构建/运行 toolchain/header 两个烟雾目标.
rem 预期: 配置/构建/两程序都退出 0; 关闭正式运行期/设计测试, 不能代表完整 Pause 验收.
rem 前提: VS 2022, 固定 CMake/Ninja, latest-main/rapids-cmake/icm 缓存; 结果保留 build.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > nul
if errorlevel 1 exit /b %errorlevel%
D:\CMake\bin\cmake.exe -S D:/workspace/pause -B D:/workspace/pause/build/stdexec-cmake-check-20260924/builds/latest-extra -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=D:/Ninja/ninja.exe -DPAUSE_BUILD_TESTS=OFF -DPAUSE_BUILD_DESIGN_TESTS=OFF -DPAUSE_BUILD_TOOLCHAIN_SMOKE_TEST=ON -DSTDEXEC_ENABLE_EXTRA_TYPE_CHECKING=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_STDEXEC=D:/workspace/pause/build/stdexec-cmake-check-20260924/sources/latest-main -DFETCHCONTENT_SOURCE_DIR_RAPIDS-CMAKE=D:/workspace/pause/build/msvc-debug/_deps/rapids-cmake-src -DFETCHCONTENT_SOURCE_DIR_ICM=D:/workspace/pause/build/msvc-debug/_deps/icm-src -DCPM_SOURCE_CACHE=D:/workspace/pause/build/msvc-debug > D:/workspace/pause/build/stdexec-cmake-check-20260924/logs/latest-extra-configure.log 2>&1
if errorlevel 1 exit /b %errorlevel%
D:\CMake\bin\cmake.exe --build D:/workspace/pause/build/stdexec-cmake-check-20260924/builds/latest-extra --target pause_toolchain_smoke pause_header_compile > D:/workspace/pause/build/stdexec-cmake-check-20260924/logs/latest-extra-build.log 2>&1
if errorlevel 1 exit /b %errorlevel%
D:\workspace\pause\build\stdexec-cmake-check-20260924\builds\latest-extra\pause_toolchain_smoke.exe > D:/workspace/pause/build/stdexec-cmake-check-20260924/logs/latest-extra-toolchain-smoke.log 2>&1
if errorlevel 1 exit /b %errorlevel%
D:\workspace\pause\build\stdexec-cmake-check-20260924\builds\latest-extra\pause_header_compile.exe > D:/workspace/pause/build/stdexec-cmake-check-20260924/logs/latest-extra-header-smoke.log 2>&1
exit /b %errorlevel%
