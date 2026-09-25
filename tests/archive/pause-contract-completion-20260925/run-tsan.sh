#!/usr/bin/env bash
# 测试项目：八套件契约补齐 TSan 压力入口
# 性质: 使用正式 TSan 运行器, g++-14 release, 每套件 10000 轮、1800 秒超时.
# 项目: extreme、concurrency、exception、notification、resource、transition、outer、host;
# 每项保存开始/结束时间、退出码、输入与二进制哈希, 输入变化或测试失败立即停止.
# 预期: 所有驱动退出 0, TSan 无报告且输入哈希稳定, 供 check-tsan-evidence.sh 再核对.
# 运行: bash tests/archive/pause-contract-completion-20260925/run-tsan.sh.
# 边界: 参数中的 DeepIterations 只交给下游支持的场景, 不增加递归深度; 日志仍位于 build.
set -euo pipefail
cd /workspace/pause
root=build/pause-contract-completion-20260925/tsan
mkdir -p "$root"
for suite in extreme concurrency exception notification resource transition outer host; do
    out="$root/$suite"
    mkdir -p "$out"
    sha256sum pause.hpp pause-branch-design.md tests/pause_*test.cpp tests/pause_sender_test_support.hpp examples/example_support.hpp tests/scripts/run-pause-tsan.sh > "$out/inputs.sha256"
    date -u +%FT%TZ > "$out/started.txt"
    printf 'Running TSan %s\n' "$suite"
    if bash tests/scripts/run-pause-tsan.sh --suite "$suite" --release --cxx g++-14 --iterations 10000 --deep-iterations 10000 --timeout 1800 --build-dir "$out" > "$out/driver.log" 2>&1; then
        status=0
    else
        status=$?
    fi
    printf '%s\n' "$status" > "$out/driver-exit.txt"
    date -u +%FT%TZ > "$out/finished.txt"
    sha256sum -c "$out/inputs.sha256" > "$out/input-check.log"
    find "$out" -maxdepth 1 -type f -name 'pause_*_test' -exec sha256sum '{}' + > "$out/binaries.sha256"
    tail -n 6 "$out/driver.log"
    if ((status != 0)); then exit "$status"; fi
done
