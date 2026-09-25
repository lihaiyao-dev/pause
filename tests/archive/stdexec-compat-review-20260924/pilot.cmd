@echo off
rem 测试项目：stdexec-compat-review-20260924/pilot.cmd
rem 初始化 VS x64 环境并调用同目录 pilot.ps1, 比较四个历史 stdexec 快照.
rem 前提: VS 2022 及 build/stdexec-compat-review-20260924 离线快照; 是否兼容需审阅 pilot 结果 JSON, 不要求所有快照通过.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal EnableExtensions
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\workspace\pause\tests\archive\stdexec-compat-review-20260924\pilot.ps1"
exit /b %errorlevel%
