@echo off
rem 测试项目：validate-usage-examples.cmd
rem 配置 Release 并开启 examples, 构建 pause_workflows/pause_independent_jobs, 执行 pause_example_* CTest.
rem 预期: 各阶段退出 0; 需要 VS 2022/CMake/msvc-release preset, 未显式构建的其他示例目标需已存在.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cmake --preset msvc-release -DPAUSE_BUILD_EXAMPLES=ON
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset msvc-release --target pause_workflows pause_independent_jobs
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build/msvc-release --output-on-failure -R "^pause_example_"
exit /b %errorlevel%
