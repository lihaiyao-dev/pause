@echo off
rem 测试项目：sender-agent/build.cmd
rem 以 MSVC Debug 编译普通 sender, hook sender 和死亡测试三个目标, 本脚本不运行.
rem 预期: 全部编译成功; hook 使用已生成的 pause-instrumented-stdexec, 需要 VS 2022 和 build/msvc-debug 依赖.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc /MDd /Zi /Od /I"." /I"build\msvc-debug\_deps\stdexec-src\include" tests\pause_sender_test.cpp /Fo"build\sender-agent\pause_sender_test.obj" /Fd"build\sender-agent\sender.pdb" /Fe"build\sender-agent\pause_sender_test.exe"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc /MDd /Zi /Od /I"." /I"build\msvc-debug\pause-instrumented-stdexec" tests\pause_sender_hook_test.cpp /Fo"build\sender-agent\pause_sender_hook_test.obj" /Fd"build\sender-agent\hook.pdb" /Fe"build\sender-agent\pause_sender_hook_test.exe"
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc /MDd /Zi /Od /I"." /I"build\msvc-debug\_deps\stdexec-src\include" tests\pause_sender_death_test.cpp /Fo"build\sender-agent\pause_sender_death_test.obj" /Fd"build\sender-agent\death.pdb" /Fe"build\sender-agent\pause_sender_death_test.exe"
exit /b %errorlevel%
