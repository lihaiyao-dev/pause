@echo off
rem 测试项目：超时自检的故意超时子驱动
rem 性质: MSVC 环境入口, 调用正式运行器对 contract death 重复 1000 轮但限时 1 秒.
rem 项目: 强制触发运行器超时和进程树终止路径, 将结果保留到原 build timeout-selfcheck.
rem 预期: 非零退出且运行器 JSON 标记 timed_out, 由同目录 PowerShell 父驱动检查清理情况.
rem 运行: 由 run-selfcheck.ps1 启动, 也可从仓库根直接调用本 cmd 观察故意超时.
rem 边界: 此处非零是自检预期, 不应算作产品失败或产品通过.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cd /d D:\workspace\pause
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run-pause-stress.ps1 -OnlyTest pause_contract_death_test -TerminationIterations 1000 -TimeoutSeconds 1 -OutputDirectory build/pause-contract-completion-20260925/timeout-selfcheck
exit /b %errorlevel%
