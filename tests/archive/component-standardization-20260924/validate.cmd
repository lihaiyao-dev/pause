@echo off
rem 测试项目：component-standardization-20260924/validate.cmd
rem 依次构建并执行 msvc-debug/msvc-release 的已有 CTest preset, 不重新配置.
rem 预期: 四阶段均退出 0; 需要固定 D:\workspace\pause 工作区/VS 2022/已配置构建树, 范围以当时 CTest 清单为准.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cd /d D:\workspace\pause
cmake --build --preset msvc-debug
if errorlevel 1 exit /b %errorlevel%
ctest --preset msvc-debug --output-on-failure
if errorlevel 1 exit /b %errorlevel%
cmake --build --preset msvc-release
if errorlevel 1 exit /b %errorlevel%
ctest --preset msvc-release --output-on-failure
exit /b %errorlevel%
