#!/bin/bash
# 测试项目：tsan-tests/run-1000.sh
# 并行运行 sender 非 PIE/integration 两个 TSan 产物, 使用 setarch -R, --iterations 1000 和 300 秒超时.
# 预期: 两个 exit.txt 均为 0 且日志无 TSan 诊断; 末尾裸 wait 不聚合各子进程退出码, 不能只看脚本退出码.
# 前提: 固定 WSL 工作区/允许关闭 ASLR/两个历史二进制; 不编译, 结果保留 build/tsan-tests.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
cd /workspace/pause || exit 90
for suite in sender integration; do
  (
    binary="pause_${suite}_test"
    if [ "$suite" = sender ]; then binary="pause_sender_test_nopie"; fi
    timeout 300s setarch x86_64 -R "./build/tsan-tests/$binary" --iterations 1000 > "build/tsan-tests/${suite}-1000-run.log" 2>&1
    test_status=$?
    printf '%s\n' "$test_status" > "build/tsan-tests/${suite}-1000-exit.txt"
    cat "build/tsan-tests/${suite}-1000-run.log"
    exit "$test_status"
  ) &
done
wait