#!/bin/sh
# 测试项目：tsan-check-20260923/check-tsan.sh
# 用 clang++ C++23 -fsanitize=thread 编译 stdexec_stop_callback_test, 执行 --iterations 200, 超时 90 秒.
# 预期: 退出 0 且无 TSan 诊断, halt_on_error=1, 保存工具链/构建/运行日志和退出码.
# 前提: 固定 WSL 工作区/clang++ TSan/build/msvc-debug 对应历史 stdexec; 属依赖停止回调探针.
# 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
set -eu
cd /workspace/pause
audit_dir=build/tsan-check-20260923
clang++ --version > "$audit_dir/toolchain.log"
clang++ -std=c++23 -O1 -g -fsanitize=thread -fno-omit-frame-pointer -pthread \
  -Ibuild/msvc-debug/_deps/stdexec-src/include \
  tests/stdexec_stop_callback_test.cpp -o "$audit_dir/stdexec_stop_callback_test" \
  > "$audit_dir/build.log" 2>&1
set +e
TSAN_OPTIONS=halt_on_error=1 timeout 90s "$audit_dir/stdexec_stop_callback_test" --iterations 200 \
  > "$audit_dir/stdout.log" 2> "$audit_dir/stderr.log"
test_exit=$?
set -e
printf '%s\n' "$test_exit" > "$audit_dir/exit-code.txt"
cat "$audit_dir/stdout.log" "$audit_dir/stderr.log"
printf 'TSAN_TEST_EXIT=%s\n' "$test_exit"
exit "$test_exit"
