@echo off
rem 测试项目：历史同 token 下 scheduler 完成通道对照
rem 运行已有探针的 100 次确定性重复, 保存输出, 30 秒超时。
rem 预期: 新 run_loop/task_scheduler 观察停止, 旧 erased scheduler 与 inline 的既有 value 行为被断言。
rem 入口: 先执行同目录 build-scheduler-stop-probe.cmd; 零退出仅证明此固定依赖探针通过。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/standards-review-20260923; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; $probeDirectory = 'build/standards-review-20260923'; $schedulerProbeProcess = Start-Process -FilePath (Resolve-Path ($probeDirectory + '/scheduler_stop_probe.exe')) -WindowStyle Hidden -PassThru -RedirectStandardOutput ($probeDirectory + '/scheduler_stop_probe.out') -RedirectStandardError ($probeDirectory + '/scheduler_stop_probe.err'); $schedulerProbeHandle = $schedulerProbeProcess.Handle; if (!$schedulerProbeProcess.WaitForExit(30000)) { $schedulerProbeProcess.Kill(); throw 'Scheduler probe exceeded 30 seconds' }; $schedulerProbeProcess.Refresh(); Get-Content -LiteralPath ($probeDirectory + '/scheduler_stop_probe.out'); Get-Content -LiteralPath ($probeDirectory + '/scheduler_stop_probe.err'); if ($schedulerProbeProcess.ExitCode -ne 0) { throw ('Scheduler probe exit code: ' + $schedulerProbeProcess.ExitCode) }"
exit /b %errorlevel%
