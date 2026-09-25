#!/usr/bin/env bash
# 测试项目：编译并验证 TSan 阳性对照
# 性质: g++-14/WSL 检测器自检, 故意触发竞争, 不计入产品通过数.
# 项目: 用 TSan 和非 PIE 编译同目录未同步写入程序, 禁用 ASLR 后限时 30 秒运行.
# 预期: 退出码必须为 66 且日志包含 WARNING: ThreadSanitizer: data race, 否则脚本失败.
# 运行: bash tests/archive/pause-audit-20260925/tsan/run-positive-control.sh.
# 边界: 输出保留到原 build 审计目录, 依赖现有 g++-14 与 setarch -R 权限.
set -u
audit=/workspace/pause/build/pause-audit-20260925/tsan
exec > "$audit/positive-control-driver.log" 2>&1
set -x
export TSAN_OPTIONS=halt_on_error=1:exitcode=66
g++-14 -std=c++23 -pthread -g -O1 -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie \
  "/workspace/pause/tests/archive/pause-audit-20260925/tsan/tsan-positive-control.cpp" -o "$audit/tsan-positive-control" || exit 1
timeout 30s setarch "$(uname -m)" -R "$audit/tsan-positive-control" > "$audit/positive-control-run.log" 2>&1
status=$?
printf '%s\n' "$status" > "$audit/positive-control-exit.txt"
test "$status" -eq 66 && grep -q 'WARNING: ThreadSanitizer: data race' "$audit/positive-control-run.log"
