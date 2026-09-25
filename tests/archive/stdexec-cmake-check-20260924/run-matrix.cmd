@echo off
rem 测试项目：stdexec-cmake-check-20260924/run-matrix.cmd
rem 初始化 VS x64 环境并调用同目录 run-matrix.ps1 的离线依赖兼容矩阵.
rem 预期: 旧依赖配置拒绝, 新依赖烟雾目标通过, 同一构建目录切回旧依赖仍拒绝; 需要 VS 2022/CMake/Ninja/离线快照.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\workspace\pause\tests\archive\stdexec-cmake-check-20260924\run-matrix.ps1"
exit /b %errorlevel%
