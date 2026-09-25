@echo off
rem 测试项目：release-extreme-batch-asan/build-and-test.cmd
rem 编译批量恢复/取消/回调重入测试的 ASan 版, 运行 --iterations 1000 --suite all.
rem 预期: 批次矩阵通过且 ASan 无诊断; 需要 VS 2022 ASan/stdexec, 结果留在 build/release-extreme-batch-asan.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /Zi /fsanitize=address /I"." /I"build\msvc-debug\_deps\stdexec-src\include" tests\pause_batch_extreme_test.cpp /Fo"build\release-extreme-batch-asan\pause_batch_extreme_test.obj" /Fd"build\release-extreme-batch-asan\pause_batch_extreme_test.pdb" /Fe"build\release-extreme-batch-asan\pause_batch_extreme_test.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
build\release-extreme-batch-asan\pause_batch_extreme_test.exe --iterations 1000 --suite all 1>build\release-extreme-batch-asan\stdout.log 2>build\release-extreme-batch-asan\stderr.log
exit /b %errorlevel%
