#!/usr/bin/env bash
# 测试项目：契约补齐 TSan 阳性对照及证据核验
# 性质: 编译 contract-audit 中故意竞争的 C++ 探针, 不属于 Pause 正常场景.
# 项目: g++-14 TSan 非 PIE 构建, setarch -R 下限时 30 秒运行,
# 保存环境/命令/退出码以及源码和二进制前后 SHA-256.
# 预期: 编译成功、哈希校验一致, 运行退出 66 且出现 data race 诊断才输出 PASS.
# 运行: bash tests/archive/pause-contract-completion-20260925/tsan-positive/run-positive.sh.
# 边界: 故意竞争只验证检测器工作, 不计入产品通过数量; 输出仍留在 build/tsan-positive.
set -euo pipefail
cd /workspace/pause
out=build/pause-contract-completion-20260925/tsan-positive
source_file=tests/archive/pause-contract-audit-20260925/tsan/tsan-positive-control.cpp
binary="$out/tsan-positive-control"
mkdir -p "$out"
date -u +%FT%TZ > "$out/started.txt"
cat /etc/os-release > "$out/environment.log"
g++-14 --version >> "$out/environment.log"
uname -a >> "$out/environment.log"
sha256sum "$source_file" > "$out/source-before.sha256"
compile_command=(g++-14 -std=c++23 -pthread -g -O1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie "$source_file" -o "$binary")
run_command=(env TSAN_OPTIONS=halt_on_error=1:exitcode=66 timeout 30s setarch x86_64 -R "$binary")
printf '%q ' "${compile_command[@]}" > "$out/command.log"
printf '\n' >> "$out/command.log"
printf '%q ' "${run_command[@]}" >> "$out/command.log"
printf '\n' >> "$out/command.log"
if "${compile_command[@]}" > "$out/compile.log" 2>&1; then
    compile_status=0
else
    compile_status=$?
fi
printf '%s\n' "$compile_status" > "$out/compile-exit.txt"
if ((compile_status != 0)); then
    cat "$out/compile.log"
    exit "$compile_status"
fi
sha256sum "$binary" > "$out/binary-before.sha256"
if "${run_command[@]}" > "$out/run.log" 2>&1; then
    run_status=0
else
    run_status=$?
fi
printf '%s\n' "$run_status" > "$out/run-exit.txt"
sha256sum "$source_file" > "$out/source-after.sha256"
sha256sum "$binary" > "$out/binary-after.sha256"
sha256sum -c "$out/source-before.sha256" "$out/binary-before.sha256" > "$out/hash-check.log"
date -u +%FT%TZ > "$out/finished.txt"
cat "$out/run.log"
printf 'Positive control exit=%s; expected=66\n' "$run_status"
if ((run_status == 66)) && grep -q 'WARNING: ThreadSanitizer: data race' "$out/run.log"; then
    printf 'PASS: intentional race detected; not a production test pass.\n' | tee "$out/result.txt"
else
    printf 'FAIL: expected data race diagnostic and exit 66.\n' | tee "$out/result.txt"
    exit 1
fi
