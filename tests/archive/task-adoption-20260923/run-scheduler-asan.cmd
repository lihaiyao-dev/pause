@echo off
rem 测试项目：历史新 task 调度协议 ASan 重复验证
rem 运行 scheduler-asan.exe 的 1000 轮, 保存 stdout/stderr, 设置 30 秒超时。
rem 预期: 程序零退出且未超时, 调度次数、资源归属和排队取消符合源内断言。
rem 入口: 先执行同目录 build-scheduler-asan.cmd; 无真实 Pause 屏障或分支树。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/task-adoption-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; $taskAdoptionDirectory = 'build/task-adoption-20260923'; $taskAdoptionProcess = Start-Process -FilePath (Resolve-Path ($taskAdoptionDirectory + '/scheduler-asan.exe')) -ArgumentList '--iterations','1000' -WindowStyle Hidden -PassThru -RedirectStandardOutput ($taskAdoptionDirectory + '/scheduler-asan.stdout.log') -RedirectStandardError ($taskAdoptionDirectory + '/scheduler-asan.stderr.log'); $taskAdoptionHandle = $taskAdoptionProcess.Handle; if (!$taskAdoptionProcess.WaitForExit(30000)) { $taskAdoptionProcess.Kill(); throw 'Scheduler test exceeded 30 seconds' }; $taskAdoptionProcess.Refresh(); Get-Content -LiteralPath ($taskAdoptionDirectory + '/scheduler-asan.stdout.log'); Get-Content -LiteralPath ($taskAdoptionDirectory + '/scheduler-asan.stderr.log'); if ($taskAdoptionProcess.ExitCode -ne 0) { throw ('Scheduler test exit code: ' + $taskAdoptionProcess.ExitCode) }"
exit /b %errorlevel%
