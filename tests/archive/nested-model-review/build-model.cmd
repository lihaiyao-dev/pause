@echo off
rem 测试项目：历史嵌套分支状态模型 ASan 构建
rem 编译 tests/pause_state_model_test.cpp 到独立嵌套模型审计目录。
rem 预期: 编译成功并生成程序、对象及 PDB; 本步骤没有运行期通过结论。
rem 入口: 直接运行本文件, 运行参数由同目录 run-model.cmd 给出。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/nested-model-review; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /EHsc /MD /O2 /Zi /fsanitize=address /Febuild/nested-model-review/pause_state_model_test.exe /Fobuild/nested-model-review/pause_state_model_test.obj /Fdbuild/nested-model-review/pause_state_model_test.pdb tests/pause_state_model_test.cpp /link /INCREMENTAL:NO
exit /b %errorlevel%
