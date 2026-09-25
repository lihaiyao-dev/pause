@echo off
rem 测试项目：pause-acceptance-asan/run.cmd
rem 使用 vswhere 初始化 VS x64 环境, 调用正式 run-pause-stress.ps1 -Iterations 10000.
rem 预期: 正式 runner 选中的 ASan 测试通过, 返回其退出码; 需要 VS C++/ASan 与 vswhere, 不定义额外场景.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
setlocal EnableExtensions
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\scripts\run-pause-stress.ps1 -Iterations 10000
exit /b %errorlevel%
