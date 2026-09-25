> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# Pause 逐场景异常与压力验收（2026-09-25）

本报告对应 [设计契约](../pause-branch-design.md) 和 [逐场景矩阵](pause-contract-scenario-matrix.md)。全部有限场景目标已完成，`-RequireComplete` 核验通过，缺项为0。有效运行未发现与所列契约不符的行为；机器可核验的结果见 [evidence-summary.json](../build/pause-contract-completion-20260925/evidence-summary.json)，缺少退出结果的日志不计通过。

## 范围与方法

按 API 状态、错误优先级、完成通道、取消竞争、fork/退役、资源失败、通知和宿主生命周期枚举有限等价类。矩阵同时区分编译约束、静态审查、明确的 fail-fast 诊断，以及不得执行的悬空访问等契约外行为。有限压力不能证明任意任务规模、任意递归深度或全部 OS 交错。

普通场景每项 1,000,000 次，线程和较大拓扑场景每项 100,000 次；适用运行期场景分别在 ASan、TSan 下每项 10,000 次。内部错误、可诊断违约和 MSVC Debug STL 代理终止，每个入口各 1,000 个独立子进程。模型事件、真实运行、终止子进程分开统计，同一 scope 中多个矩阵断言不重复累加。

保留并核对已有 `build/pause-contract-audit-20260925` 的有效证据；本次新增、源文件改变或证据不完整的项目在 `build/pause-contract-completion-20260925` 补跑。旧日志不是本次重新执行，旧版 exception/death 的大数结果也不替代更新后的结果。哈希不匹配、缺 result、缺退出码或只有部分 PASS 的记录排除，并在汇总中保留原因。

## 本次修改

- `pause_exception_matrix_test.cpp`：新增 dynamic fork 的 vector reserve 分配失败与同伴提交竞争，覆盖 Open、Collecting；核对故障命中、完整回滚、同伴状态保留、再次成功及无净分配泄漏。现在共 29 项（10 普通、19 并发）。
- `pause_sender_death_test.cpp`：只有指定 schedule/connect/error 故障确已命中，且随后按契约 terminate，才接受退出码 77；注入之前的意外 terminate 返回 78/79，消除假阳性。
- `run-pause-stress.ps1`：ASan 加入 contract death，保留其 NDEBUG 诊断模式；超时只结束该测试的进程树，有界等待并释放进程句柄。
- `run-pause-tsan.sh`：加入 outer、host 两个 suite。
- `summarize-pause-contract.ps1`：核验证据、择取当前源码的有效结果，分别统计测试运行和终止子进程。
- 场景矩阵新增两项 reserve OOM，并更新结果与索引；`tests/README.md` 提供入口。

生产 `pause.hpp`、设计协议、公共 API、数据格式和固定依赖未在此次修改；无迁移要求。回滚本次测试/文档改动不改变运行行为。工作区此前已有的未提交修改保留。

## 验证结果

| 项目 | 参数/规模 | 结果 |
| --- | --- | --- |
| MSVC Debug CTest | 全部 56 项 | 56/56，通过，68.94 秒 |
| MSVC Release CTest | 全部 56 项 | 56/56，通过，31.33 秒 |
| Release 普通/重型 | 13 个运行期程序，逐项 O/H 参数 | 95,100,000次完整用例/外层family，通过 |
| 预期终止 | 内部错误6类、违约5类、Debug代理6类，各1,000子进程 | 17,000子进程，通过；仅接受指定终止码 |
| MSVC ASan | 240 个固定运行期参数项，各10,000 | 2,400,000次，通过，无ASan报告；另11个终止子进程通过 |
| GCC 14 / WSL TSan | 240 个固定运行期参数项，各10,000 | 2,400,000次，通过，无TSan报告 |
| 设计模型 | 20种子 × 500,000 = 10,000,000事件 | 通过；9,835,079成功事件 + 164,921预期API拒绝 |

240项由79个普通项、161个重型项组成，Release计数为79×1,000,000＋161×100,000＝95,100,000。下面按矩阵中的测试缩写列出有效实际次数；每项的具体函数、参数、状态和期望见矩阵。资源用例按外层family计数，内部逐分配位置不再次累加；Debug代理验证附带的25个普通smoke也不累加到主压力数字。

| 测试落点 | 固定参数项 | Release | ASan | TSan |
| --- | ---: | ---: | ---: | ---: |
| I 集成/API/深链 | 24 | 3,300,000 | 240,000 | 240,000 |
| S sender/调度/生命周期 | 43 | 29,500,000 | 430,000 | 430,000 |
| H callback精确窗口 | 7 | 700,000 | 70,000 | 70,000 |
| R 分配失败/回滚/无分配 | 25 | 25,000,000 | 250,000 | 250,000 |
| L task生命周期 | 3 | 300,000 | 30,000 | 30,000 |
| F 状态转换/竞争顺序 | 11 | 11,000,000 | 110,000 | 110,000 |
| E 树形/深链结果路径 | 25 | 2,500,000 | 250,000 | 250,000 |
| B 宽批次/并行/重入 | 52 | 5,200,000 | 520,000 | 520,000 |
| P 并发创建/递归工作流 | 7 | 700,000 | 70,000 | 70,000 |
| EX 移动/API异常/并发OOM | 29 | 11,900,000 | 290,000 | 290,000 |
| N 通知/观察/关闭 | 6 | 600,000 | 60,000 | 60,000 |
| O 外包结果/host尾部销毁 | 6 | 4,200,000 | 60,000 | 60,000 |
| HOST 宿主创建/业务失败 | 2 | 200,000 | 20,000 | 20,000 |
| 合计 | 240 | 95,100,000 | 2,400,000 | 2,400,000 |

Release本次重跑EX和内部错误终止，其余复用匹配当前输入的完整证据；ASan本次补跑E/I/EX及两类终止程序，其余10个程序复用；TSan本次补跑E/P/EX/N/R/F/O/HOST，I/S/H/L/B复用。每份选中记录包含来源和哈希核验结果，不把复用说成本次新执行。

[本次全量回归日志](../build/pause-contract-completion-20260925/final-ctest.log)；[模型结果](../build/pause-contract-completion-20260925/model/pause_state_model_test.result.json)。模型还通过227个定向场景，拒绝7种故意缺陷；有界探索深度10、30,223状态、63,792转换。随机种子基值344693494597，最多32 records，实际最深26层。模型不替代真实内存序和生命周期验证。

TSan [阳性对照](../build/pause-contract-completion-20260925/tsan-positive/result.json)检出故意的未同步写冲突并退出66，确认检测器生效，不计生产通过。汇总脚本另通过9项证据判定检查，涵盖重复/不完整场景、非零退出、stderr、源码哈希变化、同项去重择取及缺项拒绝。

运行器[1秒强制超时自检](../build/pause-contract-completion-20260925/timeout-selfcheck/selfcheck.result.json)按预期退出1、`timed_out=true`；两次按该独立exe完整路径查询均无遗留进程。`taskkill`曾报告一个子PID无法终止，原因未确定，原错误保留；验证的是最终清理结果，不能称每个终止子操作均成功。自检不计产品通过或失败。汇总器也忽略没有测试名称的辅助结果文件，避免误读自检摘要。

## 栈溢出的归因和规避

已知失败的 `stdexec_deep_chain_test` 不包含 `pause.hpp`。既有 fork/error-fast 105层三次通过，106层三次失败，其中两次 `0xC00000FD`，一次 `0xC0000409` fail-fast；调用栈证据指向同步异常传播，停止后的 frame 释放也有递归路径。这支持宿主调用链过深的归因，不能推导出 Pause 状态机有缺陷，也不能承诺任意深度安全。

Pause 批次完成使用两遍循环，branch 退役只更新直接 parent；模型的深链构造和清理也已迭代化。本轮不运行默认8192层的深度极限扫描，不扩大线程栈。真实 Pause 保留已有链32/64/128、二叉5层、comb32的固定矩阵，增加完整作用域的次数；不同宿主结构、编译器和 sanitizer 的栈耗用不同，这些数字不是通用安全上限。业务需要无界递归时应在宿主侧采用迭代展开或分批调度；不能在 Pause 的 Open 路径私自加 yield，因为那会破坏直接通过契约。

## 证据与限制

- Release 通知竞争100,000次中，实测13,820次 notifier 调用重叠；不根据线程启动次数推定重叠。通知器内部不阻塞、不重入 Pause。
- 宿主线程创建失败在构造边界注入 `system_error`，核对已运行 worker 的 finish/join 与未启动 task 清理；不耗尽系统线程。业务控制失败直接验证既有 `run_scope`。
- OOM 通过定向注入，不耗尽机器内存。MSVC Debug STL 的 noexcept 代理分配终止与可回滚分配失败分开验证。
- Sanitizer 结果仅适用于固定 stdexec 提交、受验 run_loop/default task/default allocator 和相应编译参数；不覆盖来源未知的擦除 scheduler 或任意后端。ASan 不能替代 TSan。
- 既有证据和补测的哈希字段详见 JSON；旧 runner 自身有变化时单独记录来源限制，不能据此否认未改变输入/二进制的历史结果，也不声称旧脚本哈希仍与当前一致。
- TSan 外层审计包装器退出1：WSL将文本文件也标为可执行，原manifest选择规则误含清单自身；运行中修改包装器又导致全部子测试完成后的解析错误。原始清单和非零退出均保留，修正清单仅提取原先记录的二进制哈希，不重造测试结果。[独立收尾审计](../build/pause-contract-completion-20260925/tsan/final-audit.log)重新核对八项测试/driver退出0、完整输入与二进制哈希、libtsan链接及修正后的脚本语法，退出0；这属于证据修复，未把包装器失败写成通过。
- 压力同时执行会互相影响耗时；此处验证行为、取消与释放正确性，不给吞吐量或延迟 SLA 结论。

## 复现

```powershell
.\scripts\msvc-cmake.cmd test all
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -OnlyTest pause_exception_matrix_test -Iterations 100000 -OrdinaryIterations 1000000 -OutputDirectory build/pause-contract-recheck/release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -OnlyTest pause_sender_death_test -TerminationIterations 1000 -OutputDirectory build/pause-contract-recheck/release
# ASan 在 x64 Visual Studio Developer PowerShell 中运行；两类迭代参数不可遗漏。
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-stress.ps1 -Iterations 10000 -DeepIterations 10000 -OutputDirectory build/pause-contract-recheck/asan
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/summarize-pause-contract.ps1
```

```sh
bash scripts/run-pause-tsan.sh --suite all --release --cxx g++-14 --iterations 10000 --deep-iterations 10000 --timeout 1800 --build-dir build/pause-contract-recheck/tsan
```

Release 各程序的普通、重型、深链参数见场景矩阵末尾的执行器表。编译拒绝和静态审查不是运行压力；契约外未定义行为不计入合法场景验收。
