#!/usr/bin/env bash
# 测试项目：Linux/WSL 下当前 Pause sender、并发、异常、通知、资源和生命周期的 TSan 检查。
# --suite 选择测试组；--iterations 增加完整重复数，--deep-iterations 仅重复固定深度。
# 默认 -O1，--release 使用 -O2 -DNDEBUG；hook 组生成经哈希校验的独立依赖插桩副本。
# 需要支持 C++23/TSan 的编译器、cmake、timeout、setarch；默认使用 build 中固定 stdexec。
# 预期各测试退出 0、无数据竞争报告且不超时；日志和退出码保存在 --build-dir。
# setarch -R 只影响子进程；此脚本无栈极限扫描，环境不能启动 TSan 时不算通过。
# Linux/WSL test harness. coop_pause coexists with POSIX ::pause(), so the
# production header is compiled directly without a declaration-renaming shim.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
iterations=1000
deep_iterations=1
compiler=${CXX:-g++}
build_dir="$repo_dir/build/tsan-tests"
stdexec_source="$repo_dir/build/msvc-debug/_deps/stdexec-src"
test_timeout=300
suite=all
optimization=(-O1)

while (($#)); do
    case "$1" in
        --release)
            optimization=(-O2 -DNDEBUG)
            shift
            ;;
        --iterations|--deep-iterations|--cxx|--build-dir|--stdexec-source|--timeout|--suite)
            if (($# < 2)); then printf 'Missing value for %s\n' "$1" >&2; exit 2; fi
            case "$1" in
                --iterations) iterations=$2 ;;
                --deep-iterations) deep_iterations=$2 ;;
                --cxx) compiler=$2 ;;
                --build-dir) build_dir=$2 ;;
                --stdexec-source) stdexec_source=$2 ;;
                --timeout) test_timeout=$2 ;;
                --suite) suite=$2 ;;
            esac
            shift 2
            ;;
        --help)
            printf '%s\n' \
                'Usage: bash tests/scripts/run-pause-tsan.sh [--iterations 1000] [--deep-iterations 1]' \
                '       [--suite all|sender|integration|hook|lifetime|batch|extreme|concurrency|exception|notification|resource|transition|outer|host|scheduler|phase]' \
                '       [--release] [--cxx g++] [--build-dir PATH] [--stdexec-source PATH] [--timeout 300]' \
                '--release uses -O2 -DNDEBUG; otherwise keeps -O1 with internal debug invariants.' \
                'Requires Linux C++23 compiler with TSan, CMake, timeout, and setarch.' \
                'Compiles the production header directly without a POSIX declaration shim.' \
                'setarch -R disables ASLR only in each test process, never system-wide.'
            exit 0
            ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

if [[ ! "$iterations" =~ ^[1-9][0-9]*$ ]] || ((iterations > 1000000)); then
    printf 'iterations must be between 1 and 1000000\n' >&2
    exit 2
fi
if [[ ! "$deep_iterations" =~ ^[1-9][0-9]*$ ]] || ((deep_iterations > 1000000)); then
    printf 'deep-iterations must be between 1 and 1000000\n' >&2
    exit 2
fi
if [[ ! "$test_timeout" =~ ^[1-9][0-9]*$ ]]; then
    printf 'timeout must be positive seconds\n' >&2
    exit 2
fi
case "$suite" in
    all) tests=(pause_sender_test pause_integration_test pause_sender_hook_test pause_task_lifetime_test pause_batch_extreme_test pause_extreme_test pause_concurrency_stress_test pause_exception_matrix_test pause_notification_matrix_test pause_resource_test pause_transition_extreme_test pause_outer_lifetime_test pause_host_failure_test pause_scheduler_test pause_phase_test) ;;
    sender) tests=(pause_sender_test) ;;
    integration) tests=(pause_integration_test) ;;
    hook) tests=(pause_sender_hook_test) ;;
    lifetime) tests=(pause_task_lifetime_test) ;;
    batch) tests=(pause_batch_extreme_test) ;;
    extreme) tests=(pause_extreme_test) ;;
    concurrency) tests=(pause_concurrency_stress_test) ;;
    exception) tests=(pause_exception_matrix_test) ;;
    notification) tests=(pause_notification_matrix_test) ;;
    resource) tests=(pause_resource_test) ;;
    transition) tests=(pause_transition_extreme_test) ;;
    outer) tests=(pause_outer_lifetime_test) ;;
    host) tests=(pause_host_failure_test) ;;
    scheduler) tests=(pause_scheduler_test) ;;
    phase) tests=(pause_phase_test) ;;
    *) printf 'unknown suite: %s\n' "$suite" >&2; exit 2 ;;
esac

mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
stdexec_source=$(cd -- "$stdexec_source" && pwd)
if [[ ! -f "$stdexec_source/include/stdexec/execution.hpp" ]]; then
    printf 'Missing pinned stdexec headers: %s\n' "$stdexec_source" >&2
    exit 2
fi
for tool in "$compiler" cmake timeout setarch; do command -v "$tool" >/dev/null; done
"$compiler" --version | head -n 1

if [[ "$suite" == all || "$suite" == hook ]]; then
    # Reuse the same pinned hash checks and insertion-count checks as CMake's
    # Windows hook tests. Only this build directory receives instrumented files.
    (cd -- "$build_dir" && cmake "-Dpause_stdexec_include_dir=$stdexec_source/include" \
        -DPAUSE_BUILD_TESTS=ON \
        -P "$repo_dir/tests/scripts/instrument_pause_stdexec.cmake")
fi

for test_name in "${tests[@]}"; do
    includes=(-I"$repo_dir")
    if [[ "$test_name" == pause_sender_hook_test ]]; then
        includes+=(-I"$build_dir/pause-instrumented-stdexec")
    fi
    includes+=(-I"$stdexec_source/include")
    printf 'Building %s with ThreadSanitizer\n' "$test_name"
    if ! "$compiler" -std=c++23 -pthread -g "${optimization[@]}" -fsanitize=thread \
        -fno-omit-frame-pointer -fno-pie -no-pie \
        "${includes[@]}" \
        "$repo_dir/tests/runtime/$test_name.cpp" -o "$build_dir/$test_name" \
        > "$build_dir/$test_name-compile.log" 2>&1; then
        cat "$build_dir/$test_name-compile.log" >&2
        exit 1
    fi
    printf 'Running %s: %s iterations\n' "$test_name" "$iterations"
    arguments=(--iterations "$iterations")
    if [[ "$test_name" == pause_integration_test ]]; then
        arguments+=(--deep-iterations "$deep_iterations")
    fi
    if timeout "${test_timeout}s" setarch "$(uname -m)" -R \
        "$build_dir/$test_name" "${arguments[@]}" \
        > "$build_dir/$test_name-run.log" 2>&1; then
        status=0
    else
        status=$?
    fi
    printf '%s\n' "$status" > "$build_dir/$test_name-exit.txt"
    cat "$build_dir/$test_name-run.log"
    if ((status != 0)); then exit "$status"; fi
done
