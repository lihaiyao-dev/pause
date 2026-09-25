#!/usr/bin/env bash
# 测试项目：首轮六套件 TSan 历史压力运行
# 性质: 批量调用正式运行器, 每套件 1000 轮, 记录工具链及固定依赖证据.
# 项目: sender、integration、hook、lifetime 使用 debug 设置; batch、extreme 使用 release,
# 分别采用 300/600 秒超时, 保存各运行结果并检查结束时输入哈希.
# 预期: 六套件均退出 0 且无 TSan 诊断, 源码在运行期间不变.
# 运行: bash tests/archive/pause-audit-20260925/tsan/run-audit.sh.
# 边界: 历史收集器会继续收集后续套件, 总脚本退出码不汇总所有失败, 须检查 results.tsv 与摘要.
set -u -o pipefail
repo=/workspace/pause
audit="$repo/build/pause-audit-20260925/tsan"
cd "$repo" || exit 1
mkdir -p "$audit"
exec > >(tee "$audit/audit.log") 2>&1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
printf 'Audit began: '; date --iso-8601=seconds
{
  uname -a
  cat /etc/os-release
  g++-14 --version
  cmake --version
  ldd --version
  printf 'TSAN_OPTIONS=%s\n' "$TSAN_OPTIONS"
  printf 'Compiler path: '; command -v g++-14
  printf 'TSan runtime: '; g++-14 -print-file-name=libtsan.so
  sha256sum "$(readlink -f "$(g++-14 -print-file-name=libtsan.so)")"
  printf 'Repository commit: '; git -c safe.directory="$repo" rev-parse HEAD
  printf 'stdexec commit: '; git -C build/msvc-debug/_deps/stdexec-src rev-parse HEAD
  printf 'stdexec worktree status:\n'; git -C build/msvc-debug/_deps/stdexec-src status --short
} > "$audit/environment.txt" 2>&1
sha256sum pause.hpp pause-branch-design.md tests/scripts/run-pause-tsan.sh tests/instrument_pause_stdexec.cmake \
  tests/pause_sender_test.cpp tests/pause_integration_test.cpp tests/pause_sender_hook_test.cpp \
  tests/pause_task_lifetime_test.cpp tests/pause_batch_extreme_test.cpp tests/pause_extreme_test.cpp \
  build/msvc-debug/_deps/stdexec-src/include/stdexec/execution.hpp \
  > "$audit/source-sha256-before.txt"
printf 'suite\tconfiguration\titerations\texit\telapsed_seconds\n' > "$audit/results.tsv"
for suite in sender integration hook lifetime batch extreme; do
  config=debug
  extra=()
  limit=300
  if [[ "$suite" == batch || "$suite" == extreme ]]; then
    config=release
    extra=(--release)
    limit=600
  fi
  out="$audit/$suite-$config"
  mkdir -p "$out"
  command=(bash -x tests/scripts/run-pause-tsan.sh --suite "$suite" "${extra[@]}" --cxx g++-14 \
    --iterations 1000 --timeout "$limit" --build-dir "$out")
  printf '%q ' "${command[@]}" > "$out/command.txt"
  printf '\n' >> "$out/command.txt"
  printf '\nStarting %s (%s): ' "$suite" "$config"; date --iso-8601=seconds
  start=$(date +%s)
  "${command[@]}" > "$out/driver.log" 2>&1
  status=$?
  elapsed=$(( $(date +%s) - start ))
  printf '%s\n' "$status" > "$out/driver-exit.txt"
  printf '%s\t%s\t1000\t%s\t%s\n' "$suite" "$config" "$status" "$elapsed" >> "$audit/results.tsv"
  printf 'Finished %s: exit=%s elapsed=%ss\n' "$suite" "$status" "$elapsed"
  tail -n 8 "$out/driver.log"
done
sha256sum -c "$audit/source-sha256-before.txt" > "$audit/source-sha256-after-check.txt"
printf 'Hash check exit=%s\n' "$?"
printf 'Audit ended: '; date --iso-8601=seconds
cat "$audit/results.tsv"
