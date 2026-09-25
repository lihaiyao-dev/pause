@echo off
rem 测试项目：release-extreme-extra-asan/run-with-msvc-environment.cmd
rem 初始化 VS x64 环境并调用同目录 run.ps1 -SkipBuild, 执行 sender/resource 各 1000 次.
rem 预期: run.ps1 校验退出码/输出计数/stderr 通过; 需要 build/release-extreme-extra-asan 内已构建 ASan 产物.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "tests\archive\release-extreme-extra-asan\run.ps1" -SkipBuild
exit /b %errorlevel%
