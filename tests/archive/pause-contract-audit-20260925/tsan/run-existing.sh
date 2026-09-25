#!/usr/bin/env bash
# 测试项目：十一套件 TSan 批次与检测器阳性对照
# 性质: 调用同目录 run-suite.sh, 每套件 10000 轮, 记录 WSL/编译器/依赖证据.
# 项目: 覆盖 sender、integration、hook、lifetime、batch、extreme、concurrency、resource、transition、
# exception、notification; integration/batch/extreme 为 release, 其余 debug.
# 预期: 全套件成功且阳性对照产生 data race 诊断与退出 66; 任一条件不符最终返回失败.
# 运行: bash tests/archive/pause-contract-audit-20260925/tsan/run-existing.sh.
# 边界: 阳性对照故意有竞争, 不计入 Pause 通过数; outer/host 由补充脚本另行执行.
set -u -o pipefail
repo=/workspace/pause
audit="$repo/build/pause-contract-audit-20260925/tsan"
cd "$repo" || exit 1
mkdir -p "$audit"
exec > >(tee "$audit/existing-audit.log") 2>&1
{
  date --iso-8601=seconds
  uname -a
  cat /etc/os-release
  g++-14 --version
  cmake --version
  ldd --version
  printf 'Compiler: '; command -v g++-14
  printf 'Runtime: '; g++-14 -print-file-name=libtsan.so
  sha256sum "$(readlink -f "$(g++-14 -print-file-name=libtsan.so)")"
  git -c safe.directory="$repo" rev-parse HEAD
  git -C build/msvc-debug/_deps/stdexec-src rev-parse HEAD
  printf 'TSAN_OPTIONS=halt_on_error=1:exitcode=66\n'
} > "$audit/environment.txt" 2>&1
find build/msvc-debug/_deps/stdexec-src/include -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$audit/stdexec-include-sha256.txt"
overall=0
for suite in sender integration hook lifetime batch extreme concurrency resource transition exception notification; do
  config=debug
  if [[ "$suite" == integration || "$suite" == batch || "$suite" == extreme ]]; then config=release; fi
  bash "$repo/tests/archive/pause-contract-audit-20260925/tsan/run-suite.sh" "$suite" 10000 "$config"
  status=$?
  if (( status != 0 )); then overall=1; fi
done
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
g++-14 -std=c++23 -pthread -g -O1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie \
  "$repo/tests/archive/pause-contract-audit-20260925/tsan/tsan-positive-control.cpp" -o "$audit/tsan-positive-control" > "$audit/positive-control-compile.log" 2>&1
positive_compile=$?
timeout 30s setarch "$(uname -m)" -R "$audit/tsan-positive-control" > "$audit/positive-control-run.log" 2>&1
positive_status=$?
printf '%s\n' "$positive_status" > "$audit/positive-control-exit.txt"
if (( positive_compile != 0 || positive_status != 66 )) || ! grep -q 'WARNING: ThreadSanitizer: data race' "$audit/positive-control-run.log"; then overall=1; fi
printf 'Existing suites finished: '; date --iso-8601=seconds
exit "$overall"
