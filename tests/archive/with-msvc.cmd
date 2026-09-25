@echo off
rem 测试项目：with-msvc.cmd
rem 为调用者初始化 VS 2022 x64 环境, 成功后原样执行传入命令 %%*.
rem 入口: 从仓库根运行本脚本并传入命令; 无独立测试断言或 CTest 注册.
rem 前提: 固定位置的 VS Community 已安装; 测试范围和结果由被调用命令决定.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
%*
