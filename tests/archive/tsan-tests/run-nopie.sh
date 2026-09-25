#!/bin/bash
# 测试项目：tsan-tests/run-nopie.sh
# 在 WSL/Linux 运行既有非 PIE sender TSan 产物, --iterations 20, 超时 60 秒.
# 预期: 退出 0 且日志无 TSan 诊断; 需固定 /workspace/pause 工作区及历史二进制, 结果留在 build/tsan-tests.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
cd /workspace/pause || exit 90
timeout 60s ./build/tsan-tests/pause_sender_test_nopie --iterations 20 > build/tsan-tests/sender-nopie-run.log 2>&1
test_status=$?
printf '%s\n' "$test_status" > build/tsan-tests/sender-nopie-exit.txt
cat build/tsan-tests/sender-nopie-run.log
exit "$test_status"