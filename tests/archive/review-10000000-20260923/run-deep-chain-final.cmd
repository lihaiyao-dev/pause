@echo off
rem 测试项目：历史深链最终定深度回归入口
rem 设置 MSVC 环境并调用 run-deep-chain-final.ps1, 使用已有 deep-chain-final.exe。
rem 预期: 默认与深度 32/64 的有限场景无超时、非零退出或 stderr。
rem 入口: 先构建对应探针; 依赖中仍有递归路径, 本文件不是当前压力入口, 不承诺任意深度安全。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal EnableExtensions
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/review-10000000-20260923/run-deep-chain-final.ps1
exit /b %errorlevel%
