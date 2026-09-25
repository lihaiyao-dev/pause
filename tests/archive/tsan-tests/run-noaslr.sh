#!/bin/bash
# 测试项目：tsan-tests/run-noaslr.sh
# 以 setarch x86_64 -R 关闭子进程 ASLR, 运行非 PIE sender TSan 产物 --iterations 20, 超时 60 秒.
# 预期: 退出 0 且无 TSan 诊断; setarch 或运行时启动失败属于环境失败, 需要 WSL/Linux 权限及历史二进制.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
cd /workspace/pause || exit 90
timeout 60s setarch x86_64 -R ./build/tsan-tests/pause_sender_test_nopie --iterations 20 > build/tsan-tests/sender-noaslr-run.log 2>&1
test_status=$?
printf '%s\n' "$test_status" > build/tsan-tests/sender-noaslr-exit.txt
cat build/tsan-tests/sender-noaslr-run.log
exit "$test_status"