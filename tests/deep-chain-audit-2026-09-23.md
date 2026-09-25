> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# 多层链式 fork 极限测试与审查（2026-09-23）

## 结论

**极限测试未全部通过。已复现并定位深链错误传播和 stopped frame 释放的栈耗尽。** 不能给当前宿主组合“任意深度递归 fork 安全”的结论。

在正常使用约束下，未找到 Pause 锁内责任转交、直接 child 计数、parent 重入的新逻辑矛盾。但当前设计只规定递归所有权规则，没有给出经验证的深度/资源边界。固定 `stdexec::task + when_all + run_loop` 的某些合法深链组合会耗尽默认线程栈，这需要作为宿主接入与交付限制处理，不能仅靠“结构化并发”和普通深度测试排除。

这不是对强依赖外部库的否定。失败发生于本版选定宿主的实际执行路径；`pause.hpp` 仍为 0 字节，不能把依赖探针的精确深度当作未来真实 Pause 的保证。

## 方法与范围

采用风险驱动边界矩阵、独立不变量检查、确定性交错、独立进程故障隔离、几何增深、故障区间二分及三次复测。这些是常见工程验证实践，不宣称通过某项正式行业认证，也不存在一个通用于所有 C++ 协程的标准极限深度。

两种拓扑：

- `chain`：每层一个 child，深度 D 共 D+1 个节点。
- `fork`：每层一个递归 child 加一个侧支，深度 D 共 2D+1 个节点；没有指数展开。

固定 `when_all(single_sender)` 会原样返回 sender，因此纯链不建立逐层 when_all stop source。带侧支链用于验证嵌套组合器的取消桥接，二者不可互相替代。

真实 task 探针在**全部节点进入、全部叶 operation 发布**后才触发结果，防止深叶提前失败导致实际树没有展开到目标深度。六种路径如下：

| 路径 | 触发与检查 |
| --- | --- |
| value | 全部叶正常完成，每层 join 成功，业务次数等于节点数 |
| error | 最深业务 error；从最深侧支向外逐层等待取消，再以 stopped 完成侧支，业务次数为零 |
| stopped | 最深局部 stopped，逐层确认兄弟取消；用文档 task 包装，在外部 sibling 完成前释放整链 frames |
| root-stop | 全树挂起后由外部请求根停止，核对所有叶收到 token callback，再完成叶 operation |
| error-fast | 最深 error 与侧支快速 value 退场交错，测试同步异常传播的另一种合法顺序 |
| stopped-fast | 最深 stopped 与侧支快速 value 退场交错，测试 frame 释放的另一种顺序 |

每个 record 检查一次构造、一次进入、一次退役；直接 children 为零才允许 parent 退役，且 child 退役序号必须早于 parent。每个叶 operation 恰好启动/释放/完成一次，最终 live、operation 计数均为零。停止路径检查业务不继续。探针 callback 只登记取消；root-stop 在 request_stop 返回后交付叶结果，逐层兄弟取消则通过 callback.reset 同步尾部及 when_all 内部引用计数保活。这不冒充真实 Pause callback 的 completion 内销毁协议。

设计模型新增 12 个完整 scope：两种链 × Open/Collecting 下 fork × 构造期取消/Waiting 取消/恢复调度 stopped。迭代构造和退役，每事件保留原独立 oracle；每层检查延迟 child record 释放、Paused 重入保持 epoch、SingleStep 下一轮 pending、再次 fork 和最终排空。

## 环境与隔离

- Windows x64，MSVC 19.44.35225.0，固定 stdexec `f0e8ae6fdc6c188389b146bb854b80d399724b04`。
- `task-release.exe` 是 `/O2 /MD /Zi /EHsc` 优化构建，未设置 NDEBUG；ASan 构建额外使用 `/fsanitize=address`。CHECK 始终有效。
- PE header 实测 stack reserve 为 `0x100000`（1 MiB），commit 为 `0x1000`（4 KiB）；没有通过加大线程栈掩盖边界。
- 每个深度/路径独立进程，task 超时 60 秒、模型超时 120 秒，采样工作集预算 1 GiB；到达预算后只终止对应测试进程。
- 先测 1、2、4……8192 层；故障后细化观察区间并三次复测。没有遇到故障的路径只能称为“测至 8192 通过”，不称为已找到极限。
- 极限脚本完成的退出码不代表全部子用例通过；每个子进程的 status/exit_code、原始输出及报告才是验收依据。

## 发现一：带侧支深链的快速 error 传播约百层即可溢出

普通优化构建本次观察：**105 层三次通过，106 层三次 `0xC00000FD` 栈溢出**。106 层为 213 个节点、107 个叶 operation。全树已成功启动，失败发生在叶完成阶段。

离线 CDB 对 128 层的诊断捕获到真正的 Stack overflow，重复调用链为：

```text
__connect_await::__on_resume 的 catch
  -> set_error
  -> when_all::__arrive / __complete
  -> coroutine resume
  -> branch_task / await_resume / rethrow_exception
  -> 外一层 __on_resume 的 catch
```

固定 `__connect_awaitable.hpp:470–488` 在 catch 内直接发送 error。侧支已经完成时，when_all 可以同步向父层提交结果；上一层异常处理尚未退出，又进入外一层异常处理，导致调用栈累积。这里是普通业务 error，不是文档规定应 terminate 的 Pause 内部基础设施 error，不能通过把业务 error 一律 terminate 来绕开原有语义。

证据：[Release 调试日志](../build/chain-audit-20260923/cdb-release-fork-error-fast-d128.stdout.log) 第 295 行起，以及 `task-release-fork-error-fast-d105/106-confirm*.result.json`。

ASan 构建的 error-fast 细化观察为 94/95 层；94 层三次通过，95 层两次访问违例、一次通过。单独用 CDB 运行 128 层时，捕获到 `0xC00000FD`；栈涉及异常传播、runtime_error 销毁及 ASan free 路径，已使用约 1,031,336 字节线程栈。ASan 会改变调用栈布局，不能直接套用普通构建阈值。

## 发现二：stopped 传播后的 frame 释放另有深度边界

纯链正常/value、普通 error 和 error-fast 在两种优化构建中均测至 8192 层通过；这没有覆盖 stopped frame 保留及后续销毁的递归成本。

普通构建纯链 stopped 的二分观察在 990/991 层附近，但 990 层复测仅 1/3 通过，不能当作安全上限。ASan 纯链 stopped 的 1599 层三次通过、1600 层三次访问违例。

对 ASan 纯链 stopped 2048 层的独立 CDB 诊断明确捕获 `0xC00000FD`，重复调用栈为 `task::~task -> awaiter::~awaiter -> branch_task -> coroutine_destroy_nothrow`，已使用约 1,032,288 字节线程栈。支持的结论是 stopped frame 递归释放导致栈耗尽；没有发现能够据此认定 UAF 的证据。

证据：[stopped 调试日志](../build/chain-audit-20260923/debug-target-asan-chain-stopped-d2048.cdb.log) 第 148 行起；[ASan error 调试日志](../build/chain-audit-20260923/debug-target-asan-fork-error-fast-d128.cdb.log) 第 347 行起。

普通 start、取消桥接和 frame 销毁是不同路径：固定 when_all 同步 start children；stop callback 同步 request 内层 source；stopped awaiter 可以保留 frame 至析构。直接 task await 的对称转移不能证明这些路径全部栈安全。

## 边界复测：不是安全上限表

下表为普通优化构建的观测值，括号表示三次复测中的通过次数。存在较低点失败、较高点通过的波动，二分不是单调性证明。

| 拓扑/结果 | 较低候选深度（通过/3） | 较高候选深度（通过/3） |
| --- | ---: | ---: |
| chain/stopped | 990（1） | 991（0） |
| chain/root-stop | 1089（2） | 1090（2） |
| chain/stopped-fast | 990（0） | 991（0） |
| fork/value | 931（2） | 932（0） |
| fork/error | 931（2） | 932（0） |
| fork/stopped | 931（0） | 932（0） |
| fork/root-stop | 931（2） | 932（1） |
| fork/error-fast | 105（3） | 106（0） |
| fork/stopped-fast | 835（1） | 836（0） |

ASan 全量细化与复测保存于 `refine-asan-*-boundaries.json` 和对应 cases 文件。例如 stopped-fast/fork 的候选失败点 640 层在三次复测中都通过，说明临界顺序会变化，不能取 JSON 中一个 observed 字段作为产品限制。

原 ASan campaign 的 9 次失败均以 `0xC0000005` 退出，stderr 没有 sanitizer 报告。上述两类代表用例已经通过调试器确认栈耗尽，但**没有将所有访问违例批量改标为已确认栈溢出**。补充根取消的 CDB 子任务被平台安全检查中止，未取得该路径新的动态根因证据；其崩溃与阶段记录保留，根因不标为确认。

## 执行结果与模型预算

| 执行 | 实际结果 |
| --- | --- |
| 最终 Debug CTest | **10/10 通过**，17.58 秒；含深链 task 默认 48 场景和深度 16 模型独立回归 |
| 普通优化构建极限 campaign | 272 个子进程：189 通过、83 栈溢出 |
| ASan 初始极限 campaign | 145 个子进程：136 通过、9 访问违例（原 runner 标记 failure） |
| ASan 针对失败路径的细化复测 | 242 个子进程：165 通过、77 访问违例 |
| ASan 深链模型 | 1/2/4/8/16/32/64/128 层，各 12 个完整 scope 全部通过；256 层触及 120 秒预算 |

深度 128 的模型检查 42,120 个事件、创建/退役 4,620 个 records，峰值 257 个 records，12 个 scope 完整排空。256 层完成了 6 个纯链子场景，未完成全部侧支链场景，因此该深度不能登记为整体通过。

模型 oracle 每事件扫描关系并重建祖先约束，深链总体成本约为三次方；64 层约 9.90 秒，128 层约 60.38 秒。256 层被测试宿主在 120.10 秒终止，当时采样峰值约 227 MiB。这是验证预算耗尽，不是观察到死锁、ASan 内存错误或 Pause 算法的性能上限。本轮没有关闭 oracle 来追求更大的深度数字。

本轮没有重新运行上一轮一千万随机事件；当前验证集中于链结构、完整生命周期及失败边界。没有运行 TSan，不能以 ASan/确定性交错宣称排除了所有数据竞争。

## 建议顺序与兼容性

1. **先明确宿主支持的最大递归深度和资源预算。** 依据实际业务最大深度、所有结果路径和目标平台重新验收，保留余量，在宿主展开任务前拒绝超预算的图。开销及改动最小，但会限制工作流容量。本次 105/106 或 94/95 都不能直接当成跨平台限制，更不能把一次通过的 8192 当成所有路径上限。
2. 若业务需要较深或无预定上界的链，研究将任务展开、结果传播和释放改为有明确调度边界的迭代方式。必须重新验证取消、join、frame 所有权及 Pause 的调度计数；不能直接在所有 Pause 快路径增加 schedule，破坏现有 0/1 次调度契约。
3. 加大线程栈最多是有限容量措施，会增加资源占用，仍不消除随深度增长的问题；不推荐作为唯一修复，尤其取消可能发生在其他宿主线程。

本轮只审核和测试，未实施以上设计变更，也未修改 `pause-branch-design.md`、生产 API 或依赖版本。没有数据迁移；后续若改变宿主调度或容量约束，应另行确认兼容性并完成第 14 节集成验收。

## 修改文件与复现

- `tests/pause_state_model_test.cpp`：可选 `--chain-depth N --chain-only 1`，原默认模型转移和随机行为不变。
- `tests/stdexec_deep_chain_test.cpp`：真实 task 深链探针及独立生命周期检查。
- `scripts/run-chain-stress.ps1`：进程隔离、预算、逐级增深、细化和重复确认；访问违例不会被当作成功。
- `CMakeLists.txt`：注册新 task 测试及深度 16 模型回归，均使用原有设计测试开关。
- `tests/README.md` 和本报告：记录用法、证据和限制。

常规回归：

```powershell
.\scripts\msvc-cmake.cmd test
.\build\msvc-debug\pause_state_model_test.exe --chain-depth 16 --chain-only 1
.\build\msvc-debug\stdexec_deep_chain_test.exe --depth 32 --topology fork --outcome error
```

本轮保留的构建/执行脚本（仓库根目录）：

```powershell
.\build\chain-audit-20260923\build-tests.cmd model
.\build\chain-audit-20260923\build-tests.cmd task
.\build\chain-audit-20260923\run-campaign.cmd model
.\build\chain-audit-20260923\run-campaign.cmd release
.\build\chain-audit-20260923\run-campaign.cmd asan
.\build\chain-audit-20260923\refine-asan.cmd
```

手动运行极限驱动需在 VS x64 Developer PowerShell 中，使 ASan runtime DLL 可见。例如：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-chain-stress.ps1 `
  -Executable build/chain-audit-20260923/task-release.exe -Label manual-release `
  -OnlyTopology fork -OnlyOutcome error-fast -MaxDepth 256
```

完整日志在 [build/chain-audit-20260923](../build/chain-audit-20260923/)，每个测试保留 stdout/stderr/result.json。`task-release-cases.json`、`task-asan-cases.json`、`refine-asan-*-cases.json` 保留所有尝试，不能只看最后候选深度。CDB 使用本地 PDB，未下载符号；初次 ASan 初始化的 first-chance AV 日志不是目标故障证据，报告引用的是后续 `debug-target-*` 日志。

这些结果给出了可复现的宿主资源问题及模型证据；真实 Pause 的侵入链、callback completion 内重入、通知尾部和完整单步收敛仍须实现后验证。
