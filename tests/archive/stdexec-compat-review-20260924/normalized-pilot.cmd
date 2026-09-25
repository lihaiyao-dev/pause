@echo off
rem 测试项目：stdexec-compat-review-20260924/normalized-pilot.cmd
rem 初始化 VS x64 环境并调用 normalized-pilot.ps1, 比较归一化 request_stop 返回值后的两个历史快照.
rem 前提: VS 2022 与 august-main/latest-main 离线快照; 不修改正式源码, 结果留在 build.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal EnableExtensions
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\workspace\pause\tests\archive\stdexec-compat-review-20260924\normalized-pilot.ps1"
exit /b %errorlevel%
