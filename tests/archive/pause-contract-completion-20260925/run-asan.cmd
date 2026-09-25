@echo off
rem 测试项目：ASan 覆盖补齐的 Windows 环境入口
rem 性质: 初始化固定 VS2022 x64 开发环境, 调用同目录 run-asan.ps1.
rem 项目: 按 PowerShell 驱动顺序执行极端、集成、异常与两个终止套件, 返回驱动退出码.
rem 预期: MSVC 初始化及下游运行成功, 退出 0; 任一步失败保留非零状态.
rem 运行: 从仓库根调用 tests\archive\pause-contract-completion-20260925\run-asan.cmd.
rem 边界: 实际场景/次数在 PowerShell 驱动中定义, 仍使用固定机器路径与原 build 输出.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cd /d D:\workspace\pause
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/archive/pause-contract-completion-20260925/run-asan.ps1
exit /b %errorlevel%
