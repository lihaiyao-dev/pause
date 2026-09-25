#!/usr/bin/env bash
# 测试项目：TSan 运行时链接与插桩依赖证据采集
# 性质: 只读取已生成二进制及插桩头文件, 不执行产品测试.
# 项目: 对七套件存在的二进制保存 ldd 结果, 对 hook 插桩 include 树排序后计算 SHA-256.
# 预期: 证据可用于核对 libtsan 链接与实际插桩依赖版本.
# 运行: bash tests/archive/pause-audit-20260925/tsan/collect-runtime.sh.
# 边界: 缺失二进制会跳过, 脚本不自行判断 ldd 内容是否符合要求, 需结合汇总检查.
set -euo pipefail
audit=/workspace/pause/build/pause-audit-20260925/tsan
for pair in \
  sender-debug/pause_sender_test \
  integration-debug/pause_integration_test \
  hook-debug/pause_sender_hook_test \
  lifetime-debug/pause_task_lifetime_test \
  batch-release/pause_batch_extreme_test \
  extreme-release/pause_extreme_test \
  concurrency-debug/pause_concurrency_stress_test; do
  if [[ -f "$audit/$pair" ]]; then
    ldd "$audit/$pair" > "$audit/$pair-ldd.txt"
  fi
done
find "$audit/hook-debug/pause-instrumented-stdexec" -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$audit/instrumented-stdexec-sha256.txt"
