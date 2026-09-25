@echo off
rem 测试项目：pause-acceptance-asan/run-lifetime-final.cmd
rem 在 VS x64 环境调用正式 ASan runner, 只执行 pause_task_lifetime_test, 每项 10000 次.
rem 预期: 任务帧/分支退役边界通过且无 ASan 诊断; 需要 VS 2022 ASan/依赖, 结果保留 build/pause-acceptance-extra-asan.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\scripts\run-pause-stress.ps1 -OutputDirectory build/pause-acceptance-extra-asan -OnlyTest pause_task_lifetime_test -Iterations 10000
exit /b %errorlevel%
