#!/usr/bin/env bash
# 测试项目：固定 stdexec 依赖来源核验
# 性质: 历史审计辅助脚本, 不执行 Pause 测试.
# 项目: 忽略文件模式及行尾空白比较依赖 HEAD, 记录 diff 退出码、WSL autocrlf 配置及所有 include 文件 SHA-256.
# 预期: diff 退出码为 0, 依赖源码与固定版本一致; 本脚本只保存证据, 须检查生成的退出码文件.
# 运行: bash tests/archive/pause-audit-20260925/tsan/verify-dependency.sh.
# 边界: 保留固定 WSL 工作区与 build 证据目录, 不证明二进制运行正确.
set -u
repo=/workspace/pause
audit="$repo/build/pause-audit-20260925/tsan"
cd "$repo" || exit 1
git -C build/msvc-debug/_deps/stdexec-src -c core.filemode=false diff --ignore-space-at-eol --exit-code HEAD -- \
  > "$audit/stdexec-normalized-diff.txt" 2>&1
result=$?
printf '%s\n' "$result" > "$audit/stdexec-normalized-diff-exit.txt"
printf 'stdexec diff ignoring filemode and end-of-line whitespace: exit=%s\n' "$result"
git -C build/msvc-debug/_deps/stdexec-src config --show-origin --get core.autocrlf \
  > "$audit/stdexec-wsl-autocrlf.txt"
find build/msvc-debug/_deps/stdexec-src/include -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$audit/stdexec-include-sha256.txt"
printf 'Pinned dependency include hashes saved.\n'
