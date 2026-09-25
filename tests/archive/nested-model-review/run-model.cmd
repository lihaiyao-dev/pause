@echo off
rem 测试项目：历史嵌套分支模型固定种子复核
rem 运行已有模型: 搜索深度 10、20 种子各 10000 随机事件、最多 24 records。
rem 预期: 模型不变量和故意缺陷检查通过, 进程零退出, stdout/stderr 写入原 build 分组。
rem 入口: 先执行同目录 build-model.cmd; 无脚本级超时, 仅模型审计, 不是当前压力入口。
rem 路径: 以脚本位置定位仓库根, 依赖与输出保留 build/nested-model-review; 非 CTest 自动入口。
setlocal
cd /d "%~dp0..\..\.."
if errorlevel 1 exit /b %errorlevel%
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
build\nested-model-review\pause_state_model_test.exe --depth 10 --random-steps 10000 --seeds 20 --max-records 24 > build\nested-model-review\model.out 2> build\nested-model-review\model.err
exit /b %errorlevel%
