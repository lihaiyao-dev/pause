@echo off
rem 测试项目：历史 ASan 深链失败边界细化入口
rem 设置 MSVC 环境并调用 refine-asan.ps1, 根据既有 boundaries.json 重现已观察失败。
rem 预期: 保留失败与诊断日志, 不把崩溃解释为压力通过。
rem 入口: 需历史边界 JSON 与 task-asan.exe; 这是边界取证脚本, 不是当前压力入口, 本次未运行。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/chain-audit-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/chain-audit-20260923/refine-asan.ps1
exit /b %errorlevel%
