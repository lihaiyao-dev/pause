#!/usr/bin/env bash
# 测试项目：外层生命周期或宿主故障 TSan 补充批次
# 性质: 直接用 g++-14/TSan 编译并执行 outer 或 host 真实 Pause 测试.
# 项目: 每项固定 10000 轮、1800 秒超时, 非 PIE 且 setarch -R;
# host 额外纳入 example_support.hpp 的输入哈希, 保存二进制哈希和 ldd 结果.
# 预期: 编译/运行退出 0 且输入哈希不变, 哈希校验失败返回 90.
# 运行: bash 本脚本 outer 或 bash 本脚本 host.
# 边界: 两套件单独执行, 输出仍在 build/pause-contract-audit-20260925/tsan.
set -u -o pipefail
repo=/workspace/pause
audit="$repo/build/pause-contract-audit-20260925/tsan"
suite=${1:?outer or host}
extra_sources=()
case "$suite" in
  outer) test_name=pause_outer_lifetime_test ;;
  host) test_name=pause_host_failure_test; extra_sources=(examples/example_support.hpp) ;;
  *) exit 2 ;;
esac
label="$suite-debug-10000"
out="$audit/$label"
mkdir -p "$out"
cd "$repo" || exit 1
exec > >(tee "$out/audit.log") 2>&1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
sha256sum pause.hpp pause-branch-design.md "tests/$test_name.cpp" tests/pause_*support.hpp \
  "${extra_sources[@]}" "$repo/tests/archive/pause-contract-audit-20260925/tsan/run-supplement.sh" > "$out/source-sha256-before.txt"
compile=(g++-14 -std=c++23 -pthread -g -O1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie \
  -I"$repo" -I"$repo/build/msvc-debug/_deps/stdexec-src/include" \
  "$repo/tests/$test_name.cpp" -o "$out/$test_name")
run=(timeout 1800s setarch x86_64 -R "$out/$test_name" --iterations 10000)
printf '%q ' "${compile[@]}" > "$out/command.txt"
printf '\n' >> "$out/command.txt"
printf '%q ' "${run[@]}" >> "$out/command.txt"
printf '\n' >> "$out/command.txt"
printf 'Starting %s: ' "$label"; date --iso-8601=seconds
start=$(date +%s)
"${compile[@]}" > "$out/$test_name-compile.log" 2>&1
status=$?
if (( status == 0 )); then
  "${run[@]}" > "$out/$test_name-run.log" 2>&1
  status=$?
  printf '%s\n' "$status" > "$out/$test_name-exit.txt"
  cat "$out/$test_name-run.log"
else
  cat "$out/$test_name-compile.log"
fi
elapsed=$(( $(date +%s) - start ))
printf '%s\n' "$status" > "$out/driver-exit.txt"
printf '%s\n' "$elapsed" > "$out/elapsed-seconds.txt"
printf '%s\t%s\tdebug\t10000\t%s\t%s\n' "$label" "$suite" "$status" "$elapsed" >> "$audit/results.tsv"
sha256sum -c "$out/source-sha256-before.txt" > "$out/source-sha256-after-check.txt"
hash_status=$?
printf '%s\n' "$hash_status" > "$out/hash-exit.txt"
if [[ -f "$out/$test_name" ]]; then
  sha256sum "$out/$test_name" > "$out/binary-sha256.txt"
  ldd "$out/$test_name" > "$out/binary-ldd.txt"
fi
printf 'Finished %s: exit=%s hash_exit=%s elapsed=%ss at ' "$label" "$status" "$hash_status" "$elapsed"
date --iso-8601=seconds
if (( hash_status != 0 )); then exit 90; fi
exit "$status"
