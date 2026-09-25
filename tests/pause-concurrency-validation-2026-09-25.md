> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# 当前 Pause 并发、生命周期与边界压力测试（2026-09-25）

本轮依据当前 `pause-branch-design.md` 第 3–14 节审查和测试 `pause.hpp`。**在本轮覆盖的使用契约、执行器和交错下，未确认 Pause 实现的并发或生命周期缺陷；另复现了固定 stdexec 宿主的深链错误传播栈溢出边界，不能给出任意深度安全的结论。**

没有修改 `pause.hpp`、设计契约或第三方依赖。新增测试、构建/脚本注册和本报告；已有工作区改动保留。

## 源码与环境

- Windows x64，MSVC 19.44.35225，当前 `msvc-debug`、`msvc-release` presets；正式 Release 为 `/O2 /Ob2 /DNDEBUG /MD`，测试 `CHECK` 不随 `NDEBUG` 消失。
- 固定 stdexec：`f0e8ae6fdc6c188389b146bb854b80d399724b04`。
- 生产头 SHA256：`2595F4FA379B7DD3D524013B8EDF6E930B76795CD5EBD802EE15A6A2A8EE87B9`。
- 设计文档 SHA256：`6721FB24166892FE7B96C8D3CB73EB8EDC30A4B49DD36AA12E0D6C7E849FFE96`。
- ASan 用 MSVC `/O2 /MD /Zi /fsanitize=address`；nested/batch 追加 `/DNDEBUG`，其余保留内部断言。
- TSan 用 WSL2 Debian 13.6、GCC 14.2、`libtsan.so.2.0.0`，直接编译当前 `coop_pause` 生产头，**没有历史 POSIX shim**。sender/integration/hook/lifetime/concurrency 使用 `-O1`；batch/extreme 使用 `-O2 -DNDEBUG`；始终启用 `-fsanitize=thread`。

各测试重新构建；日志保存命令、退出码、超时状态、测试源码及二进制哈希。运行前后核对生产头和测试输入。原始证据位于本机 `build/pause-audit-20260925/`，该目录被 Git 忽略；本文给出可复现命令，不假设原始日志随仓库提交。

## 新增的覆盖

[pause_concurrency_stress_test.cpp](pause_concurrency_stress_test.cpp) 每轮 7 个完整作用域：

| 场景 | 交错与检查 |
| --- | --- |
| 4 线程竞争 `make_root_branch`，两种初始模式 | barrier 同时起跑；只能一个成功，其余为 `RootAlreadyCreated`；检查 revision、责任数、通知次数和最终退役 |
| `make_branch` 与 `request_pause` 竞争 | 允许两种锁序：成功登记的责任必须被暂停收集，或返回 `InvalidBranchState`；不能发生部分提交；最后退役到空 Paused |
| 4 个 run_loop 的递归树 Continuous / SingleStep | 深度 3、15 个 owning task/branch、7 次 fork、8 个叶子；两个独立 worker 在 barrier 后 fork；检查 scheduler affinity、child-before-parent 退役和父分支重入 |
| 同一递归树的叶子屏障取消 / 业务异常 | controller 不额外 resume 帮助清理；根 stopped 或原业务 error 正确传播，兄弟取消，全部记录释放 |

新测试通过 `sync_wait` 释放根 operation 后，继续 join 所有 run_loop 调用尾部，最后释放 Source、stop source 和观察状态。计数归零、Paused 和根 completion 都没有被当作资源已经静默的替代条件。

Release 的 10,000 次创建/暂停竞争观察到成功 97 次、拒绝 9,903 次；TSan 1,000 次观察到成功 815 次、拒绝 185 次。它们说明本次实际出现了两种合法结果，不代表 barrier 能指定某一种锁获取顺序。

## Release 主压力

使用正式 Release 二进制，8 套既有真实实现测试各重复 100,000 次，新并发测试每类重复 10,000 次。以下合计是**测试场景/作用域次数**，不是不同线程交错数，也不把每个 branch/operation/event 再次累加。

| 测试程序 | 每轮场景 | repetitions | 实际场景数 |
| --- | ---: | ---: | ---: |
| `pause_integration_test` | 20，另 4 条单次深链 | 100,000 | 2,000,004 |
| `pause_sender_test` | 43 | 100,000 | 4,300,000 |
| `pause_sender_hook_test` | 7 | 100,000 | 700,000 |
| `pause_resource_test` | 25 | 100,000 | 2,500,000 |
| `pause_task_lifetime_test` | 3 | 100,000 | 300,000 |
| `pause_extreme_test` | 25 | 100,000 | 2,500,000 |
| `pause_batch_extreme_test` | 52 | 100,000 | 5,200,000 |
| `pause_transition_extreme_test` | 11 | 100,000 | 1,100,000 |
| `pause_concurrency_stress_test` | 7 | 10,000 | 70,000 |
| **合计** | | | **18,670,004** |

主要矩阵与断言：

- 100,000 轮 `4 run_loop + 4 controller + 4 stopper + observer` 竞争，400,000 次 completion 恰好一次，其中 value 46,415、stopped 353,585。
- 各 100,000 次随机参数的 fork/pause、parent reentry/resume 竞争，固定 seed `4542363620118899781`。
- 32/64/128 层真实 Pause 链、5 层满二叉树、32 层侧支链，分别验证连续、单步、暂停后取消、外部业务 error/stopped。
- 宽度 1/2/8/64/256、不同取消图案、4 worker 批次，以及首个 completion 自毁后取消兄弟和立即重入。
- callback 注册可见但构造未返回、已选中尚未进入函数、异线程注销等待、completion 内/异线程销毁、第二次 `request_stop` 不等于 join。
- parent 在 Open/Collecting/Paused 重入、同 epoch 竞争、空 SingleStep、新 epoch 与旧命令拒绝；逐分配点失败后状态完全回滚并在同 parent 上重试。

运行证据：`release/*.result.json`、对应 stdout/stderr，汇总见 `summary.json`。初次 hook 压力之后 CMake 接入新目标导致该二进制重新链接；旧成功记录保存在 `release-before-final-hook-build/`，最终二进制另跑 100,000 轮，主表只计最后一轮结果。

## Sanitizer、全量回归和模型

| 验证 | 本轮结果与计数口径 |
| --- | --- |
| 最终 Debug CTest | **51/51**，46.00 秒；包括 MSVC Debug iterator proxy 的 6 个预期终止子进程和环境正/负编译检查 |
| 最终 Release CTest | **51/51**，21.60 秒 |
| MSVC ASan | **193,004** 个真实 Pause 场景，无 ASan 报告；另 6 个基础设施终止子进程核对专用退出码 77 |
| ASan 依赖生命周期探针 | stdexec 标准 task 23,000、callback/affinity 4,000；与真实 Pause 场景分开统计 |
| WSL TSan | 七套测试 **157,004** 个场景，无生产测试 TSan 报告；故意竞态阳性对照成功报告 data race 并退出 66，不计入通过数 |
| 状态模型 | 227 个定向场景、拒绝 7 类故意缺陷；有界搜索 30,223 状态/63,792 转换；另 **10,000,000** 随机事件及 12 个 64 层完整 scope |

随机模型的 10,000,000 次包括 9,836,130 个成功事件和 163,870 个预期 API 拒绝，100 seeds × 100,000 events，最大记录数 32，实际最大 fork 深度 27。模型不是生产实现，不能把这些事件称作真实 Pause 并发测试。

ASan 证据：`asan/audit-summary.json`。TSan 证据：`tsan/report.md`、`tsan/summary.json`、`tsan/results.tsv`。TSan 使用 `TSAN_OPTIONS=halt_on_error=1:exitcode=66`，`setarch -R` 只作用于测试进程，没有改变系统配置。模型证据：`model.result.json`、`model.stdout.log`。

## 复现的宿主边界：快速 error 的深链栈溢出

用最终 Release 构建的 `stdexec_deep_chain_test` 独立进程复核历史边界。此探针**不包含 `pause.hpp`**，测试固定 `stdexec::task + when_all + run_loop` 的所有权/错误传播组合。

带侧支链的 `error-fast` 路径：105 层三次通过，106 层三次以 `0xC00000FD` 退出；128 层初次探测也发生同类栈溢出。没有通过加大线程栈规避。结果与[历史深链报告](deep-chain-audit-2026-09-23.md)一致；本轮未重抓 CDB 栈，不把历史调用栈当成此次新抓的证据。

这是本轮明确复现的失败/资源边界，**不列作通过，不归类为 Pause UAF，也不把 105 当成通用安全深度**。当前生产 Pause 链深度 128 的有限矩阵通过，与该依赖探针的快速异常侧支拓扑不同，二者不矛盾。证据：`dependency-depth/evidence.json`、逐进程退出码/日志及 `current-release-boundaries.json`。

## 审查和测试本身的修订

- 新递归测试初稿把单步轮数误限定为 `2*depth+1`。多 worker 的子树退出和父重入可以分属不同 epoch，该假设不成立。改成实际暂停 operation 总数上界 22，并独立核对 15 个节点/7 次 join/全部退役。这是测试预期错误，不是核心修复；初稿失败及修订原因记录于 `contract-coverage.md`，`new-probe/` 保存最终独立编译脚本和产物。
- ASan 的一次 PowerShell `Start-Process -Wait` 卡在编译器已退出之后，测试尚未启动。保留记录，改为等待具体编译进程后重新编译/执行；未计为产品失败或成功场景，见 `asan/interrupted-build-wait.json`。
- callback 钩子只进入校验固定源码哈希的测试依赖副本，未向生产 Pause API 加入插桩。

逐契约审查表保存在 `build/pause-audit-20260925/contract-coverage.md`。有限测试尚未单独覆盖：children 存活时移动已委托 parent、并发创建同时注入 OOM、宿主多 observer 的合并/乱序通知分发。当前独立取消、跨资源、通知序列和停机 join 测试提供部分相关证据，但不能据此宣称覆盖所有组合。

文档排除的同 branch 并发使用、提前销毁 Source、双重 reset 等禁止用法未被当作正常压力场景。revision/epoch 回绕、任意 scheduler/allocator/平台和任意深度也不在本次保证范围。

## 复现命令

在仓库根目录执行 Windows 基线：

```powershell
.\scripts\msvc-cmake.cmd test all
```

在 VS x64 Developer PowerShell 中运行新增场景和 ASan：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -OnlyTest pause_concurrency_stress_test -Iterations 10000 -TimeoutSeconds 600 -OutputDirectory build/reproduce-concurrency-release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-stress.ps1 -OnlyTest pause_concurrency_stress_test -Iterations 1000 -TimeoutSeconds 600 -OutputDirectory build/reproduce-concurrency-asan
```

既有八套 Release 主压力，对主表对应的测试名分别指定 `-OnlyTest <name> -Iterations 100000 -TimeoutSeconds 1800`。每个程序有独立结果文件；建议独占输出目录，避免不同轮次互相覆盖。

WSL/Linux，使用已有的固定 stdexec 源码：

```bash
bash scripts/run-pause-tsan.sh --suite concurrency --iterations 1000 --timeout 300 --cxx g++-14 --build-dir build/reproduce-concurrency-tsan
# 宽批次、深树使用同样的 TSan 检测，但关闭昂贵的内部 Debug 重建：
bash scripts/run-pause-tsan.sh --suite batch --release --iterations 1000 --timeout 300 --cxx g++-14 --build-dir build/reproduce-batch-tsan
```

模型与独立宿主深度边界：

```powershell
.\build\msvc-release\pause_state_model_test.exe --depth 10 --random-steps 100000 --seeds 100 --max-records 32 --chain-depth 64
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-chain-stress.ps1 -Executable build/msvc-release/stdexec_deep_chain_test.exe -Label depth-check -OnlyTopology fork -OnlyOutcome error-fast -MaxDepth 128 -TimeoutSeconds 60 -OutputDirectory build/reproduce-dependency-depth
```

最后一个命令用于复现资源失败；必须查看逐进程结果，不能仅凭扫描脚本退出 0 声称全部子用例成功。默认环境需要已有 WSL/编译器；本轮没有安装工具或更换依赖。
