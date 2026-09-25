@echo off
rem 测试项目：release-extreme-nested/run-asan.cmd
rem 运行既有 pause_extreme_test ASan 二进制, 固定 --iterations 1000, 保存 stdout/stderr.
rem 预期: 子进程退出 0 且 ASan 无诊断; 只返回退出码, 不构建或核验哈希, 日志需另审.
rem 需要 VS 2022 ASan 和 build/release-extreme-nested/asan 既有匹配二进制.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
build\release-extreme-nested\asan\pause_extreme_test.exe --iterations 1000 > build\release-extreme-nested\asan\stress-1000.stdout.log 2> build\release-extreme-nested\asan\stress-1000.stderr.log
exit /b %errorlevel%
