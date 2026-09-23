#!/usr/bin/env bash
# Linux/WSL test harness only. The public namespace mandated by the design
# collides with POSIX ::pause(), so a preinclude shim isolates that declaration.
# Passing these tests does not make the unmodified header a portable Linux API.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
iterations=1000
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
        --iterations|--cxx|--build-dir|--stdexec-source|--timeout|--suite)
            if (($# < 2)); then printf 'Missing value for %s\n' "$1" >&2; exit 2; fi
            case "$1" in
                --iterations) iterations=$2 ;;
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
                'Usage: bash scripts/run-pause-tsan.sh [--iterations 1000] [--suite all|sender|integration|hook|lifetime|batch|extreme]' \
                '       [--release] [--cxx g++] [--build-dir PATH] [--stdexec-source PATH] [--timeout 300]' \
                '--release uses -O2 -DNDEBUG; otherwise keeps -O1 with internal debug invariants.' \
                'Requires Linux C++23 compiler with TSan, CMake, timeout, and setarch.' \
                'Uses a test-only POSIX declaration shim; it does not change the public namespace.' \
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
if [[ ! "$test_timeout" =~ ^[1-9][0-9]*$ ]]; then
    printf 'timeout must be positive seconds\n' >&2
    exit 2
fi
case "$suite" in
    all) tests=(pause_sender_test pause_integration_test pause_sender_hook_test pause_task_lifetime_test pause_batch_extreme_test pause_extreme_test) ;;
    sender) tests=(pause_sender_test) ;;
    integration) tests=(pause_integration_test) ;;
    hook) tests=(pause_sender_hook_test) ;;
    lifetime) tests=(pause_task_lifetime_test) ;;
    batch) tests=(pause_batch_extreme_test) ;;
    extreme) tests=(pause_extreme_test) ;;
    *) printf 'suite must be all, sender, integration, hook, lifetime, batch, or extreme\n' >&2; exit 2 ;;
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

cat > "$build_dir/posix_pause_shim.hpp" <<'EOF'
// Test-only: no test calls the renamed POSIX function. Its header guard prevents
// a later stdlib include from redeclaring ::pause beside namespace pause.
#define pause pause_posix_declaration_for_tsan_only
#include <unistd.h>
#undef pause
EOF

if [[ "$suite" == all || "$suite" == hook ]]; then
    # Reuse the same pinned hash checks and insertion-count checks as CMake's
    # Windows hook tests. Only this build directory receives instrumented files.
    (cd -- "$build_dir" && cmake "-Dstdexec_SOURCE_DIR=$stdexec_source" \
        -P "$repo_dir/tests/instrument_pause_stdexec.cmake")
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
        -include "$build_dir/posix_pause_shim.hpp" "${includes[@]}" \
        "$repo_dir/tests/$test_name.cpp" -o "$build_dir/$test_name" \
        > "$build_dir/$test_name-compile.log" 2>&1; then
        cat "$build_dir/$test_name-compile.log" >&2
        exit 1
    fi
    printf 'Running %s: %s iterations\n' "$test_name" "$iterations"
    if timeout "${test_timeout}s" setarch "$(uname -m)" -R \
        "$build_dir/$test_name" --iterations "$iterations" \
        > "$build_dir/$test_name-run.log" 2>&1; then
        status=0
    else
        status=$?
    fi
    printf '%s\n' "$status" > "$build_dir/$test_name-exit.txt"
    cat "$build_dir/$test_name-run.log"
    if ((status != 0)); then exit "$status"; fi
done
