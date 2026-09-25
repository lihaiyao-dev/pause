@echo off
rem 测试项目：历史 callback affinity ASan 运行入口
rem 初始化 MSVC 运行环境后调用 lifetime-run-final.ps1, 执行四类交错各 10000 轮。
rem 预期: PowerShell 运行器完成并返回子进程状态; 日志和 JSON 记录保留于原 build 分组。
rem 入口: 先运行同目录 lifetime-build-final.cmd; 这是依赖组合测试, 不是 Pause 实现测试。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/review-10000000-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/review-10000000-20260923/lifetime-run-final.ps1
exit /b %errorlevel%
