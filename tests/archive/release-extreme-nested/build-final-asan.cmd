@echo off
rem 测试项目：release-extreme-nested/build-final-asan.cmd
rem 以 Release + ASan 构建正式 pause_extreme_test, 然后执行 --iterations 1000.
rem 预期: 嵌套结构/取消/业务异常路径通过且 ASan 无诊断; 需要 VS 2022 ASan/stdexec, 输出保留 build.
rem 历史压力入口只增加重复次数, 不提高正式测试固定递归深度.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj /EHsc /MD /O2 /Ob2 /DNDEBUG /Zi /fsanitize=address /I. /Ibuild\msvc-debug\_deps\stdexec-src\include /Febuild\release-extreme-nested\asan\pause_extreme_test.exe /Fobuild\release-extreme-nested\asan\pause_extreme_test.obj /Fdbuild\release-extreme-nested\asan\pause_extreme_test.pdb tests\pause_extreme_test.cpp /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
build\release-extreme-nested\asan\pause_extreme_test.exe --iterations 1000 > build\release-extreme-nested\asan\final-1000.stdout.log 2> build\release-extreme-nested\asan\final-1000.stderr.log
exit /b %errorlevel%
