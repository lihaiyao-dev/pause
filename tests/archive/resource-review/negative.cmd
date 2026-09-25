@echo off
rem 测试项目：resource-review/negative.cmd
rem 运行 check_pause_environment.cmake 的正/反向编译检查.
rem 预期: 合法 scheduler/stop-token 环境可编译, 非法环境被拒绝; 需要 VS 2022/stdexec, 中间文件保留 build/resource-review/env-check.
rem 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
rem 入口: 从仓库根运行本 .cmd, 产物/日志目录仍在 build.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cmake -DCXX=cl.exe -DSOURCE_DIR=D:/workspace/pause -DSTDEXEC_INCLUDE_DIR=D:/workspace/pause/build/msvc-debug/_deps/stdexec-src/include -DOUTPUT_DIRECTORY=D:/workspace/pause/build/resource-review/env-check -P tests/check_pause_environment.cmake
