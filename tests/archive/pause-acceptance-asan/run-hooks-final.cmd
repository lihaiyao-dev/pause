@echo off
rem 测试项目：pause-acceptance-asan/run-hooks-final.cmd
rem 构建 Pause sender hook/stdexec stop-callback hook 两目标, 只运行对应两个 CTest.
rem 预期: 精确回调/启动交错测试通过; 需要已配置 build/msvc-acceptance-debug 及插桩依赖, 日志保留 build.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cmake --build build/msvc-acceptance-debug --target pause_sender_hook_test stdexec_stop_callback_hook_test --parallel 2
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build/msvc-acceptance-debug -R "(pause_sender_hook_test|stdexec_stop_callback_hook_test)" --output-on-failure --output-log D:/workspace/pause/build/msvc-acceptance-debug/ctest-hook-final.log
exit /b %errorlevel%
