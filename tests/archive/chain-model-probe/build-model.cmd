@echo off
rem 测试项目：历史深链状态模型独立构建
rem 以优化 MSVC 编译 tests/pause_state_model_test.cpp, 输出独立模型可执行文件和 PDB。
rem 预期: 编译零退出; 不执行模型、不验证真实 Pause 对象或线程。
rem 入口: 直接运行本文件, 可执行文件参数见正式模型源码头注释。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/chain-model-probe; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /Febuild/chain-model-probe/pause_state_model_test.exe /Fobuild/chain-model-probe/pause_state_model_test.obj /Fdbuild/chain-model-probe/pause_state_model_test.pdb tests/pause_state_model_test.cpp /link /INCREMENTAL:NO
exit /b %errorlevel%
