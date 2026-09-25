#!/usr/bin/env bash
# 测试项目：补充并发责任计数 TSan 压测
# 性质: 调用正式 TSan 运行器, 默认固定 1000 轮 concurrency, 单项超时 300 秒.
# 项目: 运行真实 Pause 并发套件, 保存命令、退出码、耗时、源码前后 SHA-256 和 TSV 记录.
# 预期: 测试运行器退出 0, 无 TSan 竞争诊断, 源文件校验通过.
# 运行: bash tests/archive/pause-audit-20260925/tsan/run-concurrency.sh.
# 边界: 本版返回测试退出码, 哈希校验仅记录; 仍须检查 source-sha256-after-check.txt.
set -u -o pipefail
repo=/workspace/pause
audit="$repo/build/pause-audit-20260925/tsan"
out="$audit/concurrency-debug"
mkdir -p "$out"
cd "$repo" || exit 1
exec > >(tee "$out/audit.log") 2>&1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
sha256sum pause.hpp pause-branch-design.md tests/scripts/run-pause-tsan.sh tests/pause_concurrency_stress_test.cpp \
  tests/pause_sender_test_support.hpp > "$out/source-sha256-before.txt"
command=(bash -x tests/scripts/run-pause-tsan.sh --suite concurrency --cxx g++-14 --iterations 1000 \
  --timeout 300 --build-dir "$out")
printf '%q ' "${command[@]}" > "$out/command.txt"
printf '\n' >> "$out/command.txt"
printf 'Supplement began: '; date --iso-8601=seconds
start=$(date +%s)
"${command[@]}" > "$out/driver.log" 2>&1
status=$?
elapsed=$(( $(date +%s) - start ))
printf '%s\n' "$status" > "$out/driver-exit.txt"
printf '%s\n' "$elapsed" > "$out/elapsed-seconds.txt"
printf 'concurrency\tdebug\t1000\t%s\t%s\n' "$status" "$elapsed" >> "$audit/results.tsv"
tail -n 20 "$out/driver.log"
sha256sum -c "$out/source-sha256-before.txt" > "$out/source-sha256-after-check.txt"
printf 'Hash check exit=%s\n' "$?"
printf 'Supplement ended: '; date --iso-8601=seconds
exit "$status"
