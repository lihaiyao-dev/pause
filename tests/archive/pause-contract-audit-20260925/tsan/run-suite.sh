#!/usr/bin/env bash
# 测试项目：契约审计单套件 TSan 运行
# 性质: 包装正式 TSan 脚本, 保存可复核命令、退出码、耗时、哈希及动态链接证据.
# 项目: 支持 sender/integration/hook/lifetime/batch/extreme/concurrency/exception/notification/resource/transition;
# integration 的固定深度链重复次数跟随 iterations, 每项超时 1800 秒.
# 预期: 运行退出 0, 输入哈希不变; 哈希异常返回 90, 测试异常保留原退出码.
# 运行: bash 本脚本 SUITE ITERATIONS debug|release [LABEL].
# 边界: 依赖 WSL g++-14 与固定 build 依赖树, 输出保持原审计目录; 不自动无限重试失败.
set -u -o pipefail
repo=/workspace/pause
audit="$repo/build/pause-contract-audit-20260925/tsan"
suite=${1:?suite}
iterations=${2:?iterations}
config=${3:?debug or release}
label=${4:-"$suite-$config-$iterations"}
case "$suite" in
  sender) test_name=pause_sender_test ;;
  integration) test_name=pause_integration_test ;;
  hook) test_name=pause_sender_hook_test ;;
  lifetime) test_name=pause_task_lifetime_test ;;
  batch) test_name=pause_batch_extreme_test ;;
  extreme) test_name=pause_extreme_test ;;
  concurrency) test_name=pause_concurrency_stress_test ;;
  exception) test_name=pause_exception_matrix_test ;;
  notification) test_name=pause_notification_matrix_test ;;
  resource) test_name=pause_resource_test ;;
  transition) test_name=pause_transition_extreme_test ;;
  *) exit 2 ;;
esac
out="$audit/$label"
mkdir -p "$out"
cd "$repo" || exit 1
exec > >(tee "$out/audit.log") 2>&1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
extra=()
if [[ "$config" == release ]]; then extra+=(--release); fi
if [[ "$suite" == integration ]]; then extra+=(--deep-iterations "$iterations"); fi
sha256sum pause.hpp pause-branch-design.md tests/scripts/run-pause-tsan.sh \
  "tests/$test_name.cpp" tests/pause_*support.hpp tests/instrument_pause_stdexec.cmake \
  > "$out/source-sha256-before.txt"
command=(bash -x tests/scripts/run-pause-tsan.sh --suite "$suite" --cxx g++-14 --iterations "$iterations" \
  --timeout 1800 --build-dir "$out" "${extra[@]}")
printf '%q ' "${command[@]}" > "$out/command.txt"
printf '\n' >> "$out/command.txt"
printf 'Starting %s: ' "$label"; date --iso-8601=seconds
start=$(date +%s)
"${command[@]}" > "$out/driver.log" 2>&1
status=$?
elapsed=$(( $(date +%s) - start ))
printf '%s\n' "$status" > "$out/driver-exit.txt"
printf '%s\n' "$elapsed" > "$out/elapsed-seconds.txt"
printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$suite" "$config" "$iterations" "$status" "$elapsed" >> "$audit/results.tsv"
tail -n 15 "$out/driver.log"
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
