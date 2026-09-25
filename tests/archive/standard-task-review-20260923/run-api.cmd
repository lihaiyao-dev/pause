@echo off
rem 测试项目：历史新 stdexec::task API ASan 重复验证
rem 运行已有 api-asan.exe 的 5000 轮, 保存 stdout/stderr, 30 秒超时。
rem 预期: 子进程零退出且未超时; stdout/stderr 供复核固定版本环境和错误载荷行为。
rem 入口: 先运行同目录 build-api.cmd; 不含 pause.hpp, 不替代真实 Pause API 验证。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standard-task-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; $taskApiDirectory = 'build/standard-task-review-20260923'; $taskApiProcess = Start-Process -FilePath (Resolve-Path ($taskApiDirectory + '/api-asan.exe')) -ArgumentList '--iterations','5000' -WindowStyle Hidden -PassThru -RedirectStandardOutput ($taskApiDirectory + '/api-asan.stdout.log') -RedirectStandardError ($taskApiDirectory + '/api-asan.stderr.log'); $taskApiHandle = $taskApiProcess.Handle; if (!$taskApiProcess.WaitForExit(30000)) { $taskApiProcess.Kill(); throw 'API probe exceeded 30 seconds' }; $taskApiProcess.Refresh(); Get-Content -LiteralPath ($taskApiDirectory + '/api-asan.stdout.log'); Get-Content -LiteralPath ($taskApiDirectory + '/api-asan.stderr.log'); if ($taskApiProcess.ExitCode -ne 0) { throw ('API probe exit code: ' + $taskApiProcess.ExitCode) }"
exit /b %errorlevel%
