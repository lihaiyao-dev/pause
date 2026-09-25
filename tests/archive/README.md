# 历史手写测试归档与路径迁移索引

此目录收纳历次手写探针、审查驱动、调试器命令和编辑器夹具。当前受维护的回归源码在 tests 根目录，压力入口在 tests/scripts；日常回归运行 scripts/msvc-cmake.cmd test all。

归档保留各次实验的测试逻辑和参数。文件头逐项说明目的、判据、运行前提和局限；这些文件不自动加入 CTest。部分探针使用旧版 API、固定依赖目录或 Windows/WSL 调试工具；补全相应历史环境后才能复现。包含 § 游标标记的 Clice 源码是补全夹具，不是可直接编译的程序。

深链边界脚本可能有意制造栈溢出，只作为历史诊断资料保留。本次仅对维护入口做深度 1 自检，没有执行历史极限扫描；当前大基数压力通过固定已验证深度、增加完整重复次数实现。

## 产物与证据

迁移只调整源码/脚本输入及仓库根路径。二进制、生成源码、插桩依赖和日志仍写入 build 中原批次目录。第三方下载源码和真实构建生成的 compile_commands.json 留在 build；这里的两份 Clice compile_commands.json 是手写夹具输入，说明见同目录 README。

共迁移 137 个文件：5 个维护脚本、1 个配置期 C++ 探针、128 个历史程序/脚本/辅助文件，以及 3 个编辑器配置夹具。下表包含所有迁移，便于按历史报告的旧命令定位。

[migration-manifest.json](migration-manifest.json) 中 sha256 是移动前原始字节的哈希，用于关联历史证据；不是添加注释/修路径后的哈希。旧日志、旧执行命令和旧证据哈希不改写。新的普通回归与入口验证另见 [目录整理验证](../test-layout-validation-2026-09-25.md)。

## 完整路径映射

所有路径均相对仓库根。请在仓库根执行脚本；个别脚本的额外前提见文件头。

| 原路径 | 新路径 |
| --- | --- |
| build/build-callback-affine-lifetime-probe.cmd | [tests/archive/build-callback-affine-lifetime-probe.cmd](build-callback-affine-lifetime-probe.cmd) |
| build/chain-audit-20260923/build-tests.cmd | [tests/archive/chain-audit-20260923/build-tests.cmd](chain-audit-20260923/build-tests.cmd) |
| build/chain-audit-20260923/debug-asan.cmd | [tests/archive/chain-audit-20260923/debug-asan.cmd](chain-audit-20260923/debug-asan.cmd) |
| build/chain-audit-20260923/debug-asan.ps1 | [tests/archive/chain-audit-20260923/debug-asan.ps1](chain-audit-20260923/debug-asan.ps1) |
| build/chain-audit-20260923/debug-asan-exceptions.txt | [tests/archive/chain-audit-20260923/debug-asan-exceptions.txt](chain-audit-20260923/debug-asan-exceptions.txt) |
| build/chain-audit-20260923/debug-asan-target-exceptions.txt | [tests/archive/chain-audit-20260923/debug-asan-target-exceptions.txt](chain-audit-20260923/debug-asan-target-exceptions.txt) |
| build/chain-audit-20260923/refine-asan.cmd | [tests/archive/chain-audit-20260923/refine-asan.cmd](chain-audit-20260923/refine-asan.cmd) |
| build/chain-audit-20260923/refine-asan.ps1 | [tests/archive/chain-audit-20260923/refine-asan.ps1](chain-audit-20260923/refine-asan.ps1) |
| build/chain-audit-20260923/run-campaign.cmd | [tests/archive/chain-audit-20260923/run-campaign.cmd](chain-audit-20260923/run-campaign.cmd) |
| build/chain-model-probe/build-model.cmd | [tests/archive/chain-model-probe/build-model.cmd](chain-model-probe/build-model.cmd) |
| build/clice-validation/completion-workspace/clice.toml | [tests/archive/clice-validation/completion-workspace/clice.toml](clice-validation/completion-workspace/clice.toml) |
| build/clice-validation/completion-workspace/completion.cpp | [tests/archive/clice-validation/completion-workspace/completion.cpp](clice-validation/completion-workspace/completion.cpp) |
| build/clice-validation/completion-workspace/debug/compile_commands.json | [tests/archive/clice-validation/completion-workspace/debug/compile_commands.json](clice-validation/completion-workspace/debug/compile_commands.json) |
| build/clice-validation/completion-workspace/release/compile_commands.json | [tests/archive/clice-validation/completion-workspace/release/compile_commands.json](clice-validation/completion-workspace/release/compile_commands.json) |
| build/clice-validation/completion-workspace/simple.cpp | [tests/archive/clice-validation/completion-workspace/simple.cpp](clice-validation/completion-workspace/simple.cpp) |
| build/clice-validation/verify-dot-clice-config.ps1 | [tests/archive/clice-validation/verify-dot-clice-config.ps1](clice-validation/verify-dot-clice-config.ps1) |
| build/clice-validation/verify-lsp.ps1 | [tests/archive/clice-validation/verify-lsp.ps1](clice-validation/verify-lsp.ps1) |
| build/clice-validation/verify-project-config.ps1 | [tests/archive/clice-validation/verify-project-config.ps1](clice-validation/verify-project-config.ps1) |
| build/component-standardization-20260924/validate.cmd | [tests/archive/component-standardization-20260924/validate.cmd](component-standardization-20260924/validate.cmd) |
| build/debug-release-stress/run.ps1 | [tests/archive/debug-release-stress/run.ps1](debug-release-stress/run.ps1) |
| build/design-audit-20260923/build-asan.cmd | [tests/archive/design-audit-20260923/build-asan.cmd](design-audit-20260923/build-asan.cmd) |
| build/design-audit-20260923/build-extra.cmd | [tests/archive/design-audit-20260923/build-extra.cmd](design-audit-20260923/build-extra.cmd) |
| build/design-audit-20260923/run-asan.cmd | [tests/archive/design-audit-20260923/run-asan.cmd](design-audit-20260923/run-asan.cmd) |
| build/design-audit-20260923/run-suite.ps1 | [tests/archive/design-audit-20260923/run-suite.ps1](design-audit-20260923/run-suite.ps1) |
| build/nested-model-review/build-model.cmd | [tests/archive/nested-model-review/build-model.cmd](nested-model-review/build-model.cmd) |
| build/nested-model-review/run-model.cmd | [tests/archive/nested-model-review/run-model.cmd](nested-model-review/run-model.cmd) |
| build/pause_optional_solution_probe.cpp | [tests/archive/pause_optional_solution_probe.cpp](pause_optional_solution_probe.cpp) |
| build/pause-acceptance-asan/run.cmd | [tests/archive/pause-acceptance-asan/run.cmd](pause-acceptance-asan/run.cmd) |
| build/pause-acceptance-asan/run-debug-candidate.cmd | [tests/archive/pause-acceptance-asan/run-debug-candidate.cmd](pause-acceptance-asan/run-debug-candidate.cmd) |
| build/pause-acceptance-asan/run-hooks-final.cmd | [tests/archive/pause-acceptance-asan/run-hooks-final.cmd](pause-acceptance-asan/run-hooks-final.cmd) |
| build/pause-acceptance-asan/run-lifetime-final.cmd | [tests/archive/pause-acceptance-asan/run-lifetime-final.cmd](pause-acceptance-asan/run-lifetime-final.cmd) |
| build/pause-acceptance-asan/run-resource-final.cmd | [tests/archive/pause-acceptance-asan/run-resource-final.cmd](pause-acceptance-asan/run-resource-final.cmd) |
| build/pause-audit-20260925/asan/run-asan-audit.initial.ps1 | [tests/archive/pause-audit-20260925/asan/run-asan-audit.initial.ps1](pause-audit-20260925/asan/run-asan-audit.initial.ps1) |
| build/pause-audit-20260925/asan/run-asan-audit.ps1 | [tests/archive/pause-audit-20260925/asan/run-asan-audit.ps1](pause-audit-20260925/asan/run-asan-audit.ps1) |
| build/pause-audit-20260925/asan/run-extra-asan.ps1 | [tests/archive/pause-audit-20260925/asan/run-extra-asan.ps1](pause-audit-20260925/asan/run-extra-asan.ps1) |
| build/pause-audit-20260925/host-probe/build.cmd | [tests/archive/pause-audit-20260925/host-probe/build.cmd](pause-audit-20260925/host-probe/build.cmd) |
| build/pause-audit-20260925/layout-probe/build.cmd | [tests/archive/pause-audit-20260925/layout-probe/build.cmd](pause-audit-20260925/layout-probe/build.cmd) |
| build/pause-audit-20260925/layout-probe/layout.cpp | [tests/archive/pause-audit-20260925/layout-probe/layout.cpp](pause-audit-20260925/layout-probe/layout.cpp) |
| build/pause-audit-20260925/new-probe/build.cmd | [tests/archive/pause-audit-20260925/new-probe/build.cmd](pause-audit-20260925/new-probe/build.cmd) |
| build/pause-audit-20260925/outer-probe/build.cmd | [tests/archive/pause-audit-20260925/outer-probe/build.cmd](pause-audit-20260925/outer-probe/build.cmd) |
| build/pause-audit-20260925/run-model.ps1 | [tests/archive/pause-audit-20260925/run-model.ps1](pause-audit-20260925/run-model.ps1) |
| build/pause-audit-20260925/tsan/collect-runtime.sh | [tests/archive/pause-audit-20260925/tsan/collect-runtime.sh](pause-audit-20260925/tsan/collect-runtime.sh) |
| build/pause-audit-20260925/tsan/run-audit.sh | [tests/archive/pause-audit-20260925/tsan/run-audit.sh](pause-audit-20260925/tsan/run-audit.sh) |
| build/pause-audit-20260925/tsan/run-concurrency.sh | [tests/archive/pause-audit-20260925/tsan/run-concurrency.sh](pause-audit-20260925/tsan/run-concurrency.sh) |
| build/pause-audit-20260925/tsan/run-positive-control.sh | [tests/archive/pause-audit-20260925/tsan/run-positive-control.sh](pause-audit-20260925/tsan/run-positive-control.sh) |
| build/pause-audit-20260925/tsan/summarize.ps1 | [tests/archive/pause-audit-20260925/tsan/summarize.ps1](pause-audit-20260925/tsan/summarize.ps1) |
| build/pause-audit-20260925/tsan/tsan-positive-control.cpp | [tests/archive/pause-audit-20260925/tsan/tsan-positive-control.cpp](pause-audit-20260925/tsan/tsan-positive-control.cpp) |
| build/pause-audit-20260925/tsan/verify-dependency.sh | [tests/archive/pause-audit-20260925/tsan/verify-dependency.sh](pause-audit-20260925/tsan/verify-dependency.sh) |
| build/pause-contract-audit-20260925/asan/host-failure/run-host-failure.ps1 | [tests/archive/pause-contract-audit-20260925/asan/host-failure/run-host-failure.ps1](pause-contract-audit-20260925/asan/host-failure/run-host-failure.ps1) |
| build/pause-contract-audit-20260925/asan/notification-final/run-final-notification.ps1 | [tests/archive/pause-contract-audit-20260925/asan/notification-final/run-final-notification.ps1](pause-contract-audit-20260925/asan/notification-final/run-final-notification.ps1) |
| build/pause-contract-audit-20260925/asan/outer-lifetime/run-outer-lifetime.ps1 | [tests/archive/pause-contract-audit-20260925/asan/outer-lifetime/run-outer-lifetime.ps1](pause-contract-audit-20260925/asan/outer-lifetime/run-outer-lifetime.ps1) |
| build/pause-contract-audit-20260925/asan/run-contract-asan.ps1 | [tests/archive/pause-contract-audit-20260925/asan/run-contract-asan.ps1](pause-contract-audit-20260925/asan/run-contract-asan.ps1) |
| build/pause-contract-audit-20260925/asan/summarize-contract-asan.ps1 | [tests/archive/pause-contract-audit-20260925/asan/summarize-contract-asan.ps1](pause-contract-audit-20260925/asan/summarize-contract-asan.ps1) |
| build/pause-contract-audit-20260925/run-debug-proxy.ps1 | [tests/archive/pause-contract-audit-20260925/run-debug-proxy.ps1](pause-contract-audit-20260925/run-debug-proxy.ps1) |
| build/pause-contract-audit-20260925/run-model.ps1 | [tests/archive/pause-contract-audit-20260925/run-model.ps1](pause-contract-audit-20260925/run-model.ps1) |
| build/pause-contract-audit-20260925/tsan/run-existing.sh | [tests/archive/pause-contract-audit-20260925/tsan/run-existing.sh](pause-contract-audit-20260925/tsan/run-existing.sh) |
| build/pause-contract-audit-20260925/tsan/run-suite.sh | [tests/archive/pause-contract-audit-20260925/tsan/run-suite.sh](pause-contract-audit-20260925/tsan/run-suite.sh) |
| build/pause-contract-audit-20260925/tsan/run-supplement.sh | [tests/archive/pause-contract-audit-20260925/tsan/run-supplement.sh](pause-contract-audit-20260925/tsan/run-supplement.sh) |
| build/pause-contract-audit-20260925/tsan/summarize.ps1 | [tests/archive/pause-contract-audit-20260925/tsan/summarize.ps1](pause-contract-audit-20260925/tsan/summarize.ps1) |
| build/pause-contract-audit-20260925/tsan/tsan-positive-control.cpp | [tests/archive/pause-contract-audit-20260925/tsan/tsan-positive-control.cpp](pause-contract-audit-20260925/tsan/tsan-positive-control.cpp) |
| build/pause-contract-completion-20260925/check-tsan-evidence.sh | [tests/archive/pause-contract-completion-20260925/check-tsan-evidence.sh](pause-contract-completion-20260925/check-tsan-evidence.sh) |
| build/pause-contract-completion-20260925/run-asan.cmd | [tests/archive/pause-contract-completion-20260925/run-asan.cmd](pause-contract-completion-20260925/run-asan.cmd) |
| build/pause-contract-completion-20260925/run-asan.ps1 | [tests/archive/pause-contract-completion-20260925/run-asan.ps1](pause-contract-completion-20260925/run-asan.ps1) |
| build/pause-contract-completion-20260925/run-tsan.sh | [tests/archive/pause-contract-completion-20260925/run-tsan.sh](pause-contract-completion-20260925/run-tsan.sh) |
| build/pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.cmd | [tests/archive/pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.cmd](pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.cmd) |
| build/pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.ps1 | [tests/archive/pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.ps1](pause-contract-completion-20260925/timeout-selfcheck/run-selfcheck.ps1) |
| build/pause-contract-completion-20260925/tsan-positive/run-positive.sh | [tests/archive/pause-contract-completion-20260925/tsan-positive/run-positive.sh](pause-contract-completion-20260925/tsan-positive/run-positive.sh) |
| build/probe-lldb-server.py | [tests/archive/probe-lldb-server.py](probe-lldb-server.py) |
| build/release-extreme-batch-asan/build-and-test.cmd | [tests/archive/release-extreme-batch-asan/build-and-test.cmd](release-extreme-batch-asan/build-and-test.cmd) |
| build/release-extreme-extra-asan/build.cmd | [tests/archive/release-extreme-extra-asan/build.cmd](release-extreme-extra-asan/build.cmd) |
| build/release-extreme-extra-asan/run.ps1 | [tests/archive/release-extreme-extra-asan/run.ps1](release-extreme-extra-asan/run.ps1) |
| build/release-extreme-extra-asan/run-with-msvc-environment.cmd | [tests/archive/release-extreme-extra-asan/run-with-msvc-environment.cmd](release-extreme-extra-asan/run-with-msvc-environment.cmd) |
| build/release-extreme-nested/build.cmd | [tests/archive/release-extreme-nested/build.cmd](release-extreme-nested/build.cmd) |
| build/release-extreme-nested/build-extra.cmd | [tests/archive/release-extreme-nested/build-extra.cmd](release-extreme-nested/build-extra.cmd) |
| build/release-extreme-nested/build-final-asan.cmd | [tests/archive/release-extreme-nested/build-final-asan.cmd](release-extreme-nested/build-final-asan.cmd) |
| build/release-extreme-nested/pause_external_probe.cpp | [tests/archive/release-extreme-nested/pause_external_probe.cpp](release-extreme-nested/pause_external_probe.cpp) |
| build/release-extreme-nested/run-asan.cmd | [tests/archive/release-extreme-nested/run-asan.cmd](release-extreme-nested/run-asan.cmd) |
| build/release-extreme-tsan/posix_pause_shim.hpp | [tests/archive/release-extreme-tsan/posix_pause_shim.hpp](release-extreme-tsan/posix_pause_shim.hpp) |
| build/release-extreme-tsan-final/posix_pause_shim.hpp | [tests/archive/release-extreme-tsan-final/posix_pause_shim.hpp](release-extreme-tsan-final/posix_pause_shim.hpp) |
| build/release-extreme-tsan-o2/posix_pause_shim.hpp | [tests/archive/release-extreme-tsan-o2/posix_pause_shim.hpp](release-extreme-tsan-o2/posix_pause_shim.hpp) |
| build/resource-review/compile.cmd | [tests/archive/resource-review/compile.cmd](resource-review/compile.cmd) |
| build/resource-review/compile-no-proxy.cmd | [tests/archive/resource-review/compile-no-proxy.cmd](resource-review/compile-no-proxy.cmd) |
| build/resource-review/negative.cmd | [tests/archive/resource-review/negative.cmd](resource-review/negative.cmd) |
| build/resource-review/run-no-proxy.cmd | [tests/archive/resource-review/run-no-proxy.cmd](resource-review/run-no-proxy.cmd) |
| build/resource-review/tsan_probe.cpp | [tests/archive/resource-review/tsan_probe.cpp](resource-review/tsan_probe.cpp) |
| build/review-10000000-20260923/build-asan.cmd | [tests/archive/review-10000000-20260923/build-asan.cmd](review-10000000-20260923/build-asan.cmd) |
| build/review-10000000-20260923/build-deep-chain-final.cmd | [tests/archive/review-10000000-20260923/build-deep-chain-final.cmd](review-10000000-20260923/build-deep-chain-final.cmd) |
| build/review-10000000-20260923/build-run-terminate-probe.cmd | [tests/archive/review-10000000-20260923/build-run-terminate-probe.cmd](review-10000000-20260923/build-run-terminate-probe.cmd) |
| build/review-10000000-20260923/lifetime-build-final.cmd | [tests/archive/review-10000000-20260923/lifetime-build-final.cmd](review-10000000-20260923/lifetime-build-final.cmd) |
| build/review-10000000-20260923/lifetime-run.cmd | [tests/archive/review-10000000-20260923/lifetime-run.cmd](review-10000000-20260923/lifetime-run.cmd) |
| build/review-10000000-20260923/lifetime-run-final.cmd | [tests/archive/review-10000000-20260923/lifetime-run-final.cmd](review-10000000-20260923/lifetime-run-final.cmd) |
| build/review-10000000-20260923/lifetime-run-final.ps1 | [tests/archive/review-10000000-20260923/lifetime-run-final.ps1](review-10000000-20260923/lifetime-run-final.ps1) |
| build/review-10000000-20260923/lifetime-terminal-inflight.cpp | [tests/archive/review-10000000-20260923/lifetime-terminal-inflight.cpp](review-10000000-20260923/lifetime-terminal-inflight.cpp) |
| build/review-10000000-20260923/model-baseline.cpp | [tests/archive/review-10000000-20260923/model-baseline.cpp](review-10000000-20260923/model-baseline.cpp) |
| build/review-10000000-20260923/run-asan.cmd | [tests/archive/review-10000000-20260923/run-asan.cmd](review-10000000-20260923/run-asan.cmd) |
| build/review-10000000-20260923/run-deep-chain-final.cmd | [tests/archive/review-10000000-20260923/run-deep-chain-final.cmd](review-10000000-20260923/run-deep-chain-final.cmd) |
| build/review-10000000-20260923/run-deep-chain-final.ps1 | [tests/archive/review-10000000-20260923/run-deep-chain-final.ps1](review-10000000-20260923/run-deep-chain-final.ps1) |
| build/review-10000000-20260923/run-model-final.cmd | [tests/archive/review-10000000-20260923/run-model-final.cmd](review-10000000-20260923/run-model-final.cmd) |
| build/review-10000000-20260923/run-suite.ps1 | [tests/archive/review-10000000-20260923/run-suite.ps1](review-10000000-20260923/run-suite.ps1) |
| build/review-10000000-20260923/run-terminate-probe.ps1 | [tests/archive/review-10000000-20260923/run-terminate-probe.ps1](review-10000000-20260923/run-terminate-probe.ps1) |
| build/review-10000000-20260923/terminate-composition-probe.cpp | [tests/archive/review-10000000-20260923/terminate-composition-probe.cpp](review-10000000-20260923/terminate-composition-probe.cpp) |
| build/review-10000000-20260923/verify-final.cmd | [tests/archive/review-10000000-20260923/verify-final.cmd](review-10000000-20260923/verify-final.cmd) |
| main.cpp | [tests/archive/root/main.cpp](root/main.cpp) |
| build/sender-agent/build.cmd | [tests/archive/sender-agent/build.cmd](sender-agent/build.cmd) |
| build/standards-review-20260923/build-scheduler-stop-probe.cmd | [tests/archive/standards-review-20260923/build-scheduler-stop-probe.cmd](standards-review-20260923/build-scheduler-stop-probe.cmd) |
| build/standards-review-20260923/run-scheduler-stop-probe.cmd | [tests/archive/standards-review-20260923/run-scheduler-stop-probe.cmd](standards-review-20260923/run-scheduler-stop-probe.cmd) |
| build/standards-review-20260923/scheduler_stop_probe.cpp | [tests/archive/standards-review-20260923/scheduler_stop_probe.cpp](standards-review-20260923/scheduler_stop_probe.cpp) |
| build/standard-task-review-20260923/build-api.cmd | [tests/archive/standard-task-review-20260923/build-api.cmd](standard-task-review-20260923/build-api.cmd) |
| build/standard-task-review-20260923/build-lifetime.cmd | [tests/archive/standard-task-review-20260923/build-lifetime.cmd](standard-task-review-20260923/build-lifetime.cmd) |
| build/standard-task-review-20260923/run-api.cmd | [tests/archive/standard-task-review-20260923/run-api.cmd](standard-task-review-20260923/run-api.cmd) |
| build/standard-task-review-20260923/run-lifetime.cmd | [tests/archive/standard-task-review-20260923/run-lifetime.cmd](standard-task-review-20260923/run-lifetime.cmd) |
| build/stdexec-cmake-check-20260924/run-extra.cmd | [tests/archive/stdexec-cmake-check-20260924/run-extra.cmd](stdexec-cmake-check-20260924/run-extra.cmd) |
| build/stdexec-cmake-check-20260924/run-matrix.cmd | [tests/archive/stdexec-cmake-check-20260924/run-matrix.cmd](stdexec-cmake-check-20260924/run-matrix.cmd) |
| build/stdexec-cmake-check-20260924/run-matrix.ps1 | [tests/archive/stdexec-cmake-check-20260924/run-matrix.ps1](stdexec-cmake-check-20260924/run-matrix.ps1) |
| build/stdexec-compat-review-20260924/normalized-pilot.cmd | [tests/archive/stdexec-compat-review-20260924/normalized-pilot.cmd](stdexec-compat-review-20260924/normalized-pilot.cmd) |
| build/stdexec-compat-review-20260924/normalized-pilot.ps1 | [tests/archive/stdexec-compat-review-20260924/normalized-pilot.ps1](stdexec-compat-review-20260924/normalized-pilot.ps1) |
| build/stdexec-compat-review-20260924/pilot.cmd | [tests/archive/stdexec-compat-review-20260924/pilot.cmd](stdexec-compat-review-20260924/pilot.cmd) |
| build/stdexec-compat-review-20260924/pilot.ps1 | [tests/archive/stdexec-compat-review-20260924/pilot.ps1](stdexec-compat-review-20260924/pilot.ps1) |
| build/summarize-pause-release.ps1 | [tests/archive/summarize-pause-release.ps1](summarize-pause-release.ps1) |
| build/task-adoption-20260923/build-scheduler-asan.cmd | [tests/archive/task-adoption-20260923/build-scheduler-asan.cmd](task-adoption-20260923/build-scheduler-asan.cmd) |
| build/task-adoption-20260923/run-scheduler-asan.cmd | [tests/archive/task-adoption-20260923/run-scheduler-asan.cmd](task-adoption-20260923/run-scheduler-asan.cmd) |
| build/transition-extreme-asan/build-run.cmd | [tests/archive/transition-extreme-asan/build-run.cmd](transition-extreme-asan/build-run.cmd) |
| build/tsan-check-20260923/check-tsan.sh | [tests/archive/tsan-check-20260923/check-tsan.sh](tsan-check-20260923/check-tsan.sh) |
| build/tsan-tests/posix_pause_shim.hpp | [tests/archive/tsan-tests/posix_pause_shim.hpp](tsan-tests/posix_pause_shim.hpp) |
| build/tsan-tests/run-1000.sh | [tests/archive/tsan-tests/run-1000.sh](tsan-tests/run-1000.sh) |
| build/tsan-tests/run-noaslr.sh | [tests/archive/tsan-tests/run-noaslr.sh](tsan-tests/run-noaslr.sh) |
| build/tsan-tests/run-nopie.sh | [tests/archive/tsan-tests/run-nopie.sh](tsan-tests/run-nopie.sh) |
| build/validate-usage-all.cmd | [tests/archive/validate-usage-all.cmd](validate-usage-all.cmd) |
| build/validate-usage-examples.cmd | [tests/archive/validate-usage-examples.cmd](validate-usage-examples.cmd) |
| build/verify-lldb-dap.py | [tests/archive/verify-lldb-dap.py](verify-lldb-dap.py) |
| build/with-msvc.cmd | [tests/archive/with-msvc.cmd](with-msvc.cmd) |
| cmake/check_stdexec.cpp | [tests/check_stdexec.cpp](../check_stdexec.cpp) |
| scripts/run-chain-stress.ps1 | [tests/scripts/run-chain-stress.ps1](../scripts/run-chain-stress.ps1) |
| scripts/run-pause-release-stress.ps1 | [tests/scripts/run-pause-release-stress.ps1](../scripts/run-pause-release-stress.ps1) |
| scripts/run-pause-stress.ps1 | [tests/scripts/run-pause-stress.ps1](../scripts/run-pause-stress.ps1) |
| scripts/run-pause-tsan.sh | [tests/scripts/run-pause-tsan.sh](../scripts/run-pause-tsan.sh) |
| scripts/summarize-pause-contract.ps1 | [tests/scripts/summarize-pause-contract.ps1](../scripts/summarize-pause-contract.ps1) |
