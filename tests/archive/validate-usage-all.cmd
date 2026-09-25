@echo off
rem 测试项目：validate-usage-all.cmd
rem 依次配置/构建 Debug 和 Release, 开启 examples 并运行对应完整 CTest preset.
rem 预期: 每阶段退出 0, 首次失败即返回; 需要 VS 2022/CMake 和两个 MSVC preset.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal EnableDelayedExpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
for %%C in (debug release) do (
    cmake --preset msvc-%%C -DPAUSE_BUILD_EXAMPLES=ON
    if errorlevel 1 exit /b !errorlevel!
    cmake --build --preset msvc-%%C
    if errorlevel 1 exit /b !errorlevel!
    ctest --preset msvc-%%C
    if errorlevel 1 exit /b !errorlevel!
)
exit /b 0
