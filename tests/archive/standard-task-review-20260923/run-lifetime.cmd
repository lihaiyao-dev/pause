@echo off
rem 测试项目：历史新 stdexec::task 生命周期重复验证
rem 按参数 debug 或 asan 选已有程序, 第二参数为正整数轮数; 最长 300 秒并保存输出和计时。
rem 预期: 参数有效、进程零退出且未超时; 检查 frame 退役、兄弟取消和 start/completion 握手。
rem 入口: run-lifetime.cmd debug-or-asan iterations, 需先执行同目录 build-lifetime.cmd。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standard-task-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set "PAUSE_TEST_VARIANT=%~1"
set "PAUSE_TEST_ITERATIONS=%~2"
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; $directory = 'build/standard-task-review-20260923'; $variant = $env:PAUSE_TEST_VARIANT; $iterations = $env:PAUSE_TEST_ITERATIONS; if ($variant -notin @('debug','asan') -or $iterations -notmatch '^[1-9][0-9]*$') { throw 'Usage: run-lifetime.cmd debug-or-asan iterations' }; $label = $variant + '-' + $iterations; $watch = [System.Diagnostics.Stopwatch]::StartNew(); $process = Start-Process -FilePath (Resolve-Path ($directory + '/standard_task_lifetime_' + $variant + '.exe')) -ArgumentList @('--iterations', $iterations) -WindowStyle Hidden -PassThru -RedirectStandardOutput ($directory + '/' + $label + '.out') -RedirectStandardError ($directory + '/' + $label + '.err'); $processHandle = $process.Handle; while (!$process.WaitForExit(30000)) { if ($watch.Elapsed.TotalSeconds -ge 300) { $process.Kill(); throw 'Standard task run exceeded 300 seconds' }; Write-Output ('Still running: ' + [int]$watch.Elapsed.TotalSeconds + 's') }; $process.Refresh(); $watch.Stop(); Get-Content -LiteralPath ($directory + '/' + $label + '.out'); Get-Content -LiteralPath ($directory + '/' + $label + '.err'); $summary = 'Elapsed: ' + $watch.Elapsed.TotalSeconds + 's; Exit: ' + $process.ExitCode; $summary | Set-Content -LiteralPath ($directory + '/' + $label + '.result'); Write-Output $summary; if ($process.ExitCode -ne 0) { throw ('Standard task exit code: ' + $process.ExitCode) }"
exit /b %errorlevel%
