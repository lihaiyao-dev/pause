#!/usr/bin/env bash
# 测试项目：八套件补充 TSan 证据复核
# 性质: 只读取已有日志、哈希清单及二进制, 不重新执行产品压力.
# 项目: 检查 run-tsan.sh 语法、八组驱动/测试退出码、源码和二进制 SHA-256,
# 并用 ldd 确認实际链接 libtsan.so.
# 预期: 全部检查成功才逐套件输出 PASS, 任一校验失败立即停止.
# 运行: bash tests/archive/pause-contract-completion-20260925/check-tsan-evidence.sh.
# 边界: 只验证当前文件与历史哈希一致; 注释改变后不改写旧清单来伪造一致性.
set -euo pipefail
cd /workspace/pause
root=build/pause-contract-completion-20260925/tsan
bash -n tests/archive/pause-contract-completion-20260925/run-tsan.sh
for suite in extreme concurrency exception notification resource transition outer host; do
    out="$root/$suite"
    [[ $(cat "$out/driver-exit.txt") == 0 ]]
    for status in "$out"/pause_*-exit.txt; do [[ $(cat "$status") == 0 ]]; done
    sha256sum -c "$out/inputs.sha256" > "$out/final-input-check.log"
    sha256sum -c "$out/binaries.sha256" > "$out/final-binary-check.log"
    for binary in "$out"/pause_*_test; do
        linkage=$(ldd "$binary")
        printf '%s\n' "$linkage" > "$out/binary-ldd.txt"
        [[ "$linkage" == *libtsan.so* ]]
    done
    printf 'PASS completed TSan suite: %s (exit, source/binary hashes, libtsan linkage)\n' "$suite"
done
