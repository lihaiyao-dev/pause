> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# Pause Release 极端测试（2026-09-23）

> 历史记录：本报告使用旧 C++ 命名空间 `pause`。2026-09-24 已改为 `coop_pause`，并移除 TSan 脚本的 POSIX 声明 shim；下文保留当时的实现边界和运行证据，不代表改名后的重新验收。

本轮按 `pause-branch-design.md` 第 13、14 节补齐并执行真实实现测试。
`pause.hpp` 未修改；固定 stdexec 提交仍为
`f0e8ae6fdc6c188389b146bb854b80d399724b04`，未增加依赖。
生产头 SHA256 为
`37ECD01A2FB73A17EDB478FABE5E5F1546979993CFA122971E96A4F0FA94170A`。

## 测试口径

主验收使用 MSVC 19.44 x64 的真实 Release preset：
`/O2 /Ob2 /DNDEBUG /MD /EHsc`。测试的 `CHECK` 独立于 `assert`，
不会被 `NDEBUG` 删除。没有用 ASan 的优化构建冒充普通 Release。

按已定义的有限矩阵，**184 个固定配置场景各完成 100,000 次**，另有
两组固定种子的随机参数竞争各 100,000 轮和集成程序自带的 4 条单次深链，合计
**18,600,004 个真实实现测试场景/轮次**。随机竞争结果的分布不计作
每一种线程排列都达到了 100,000 次；深链的重复指标由新增矩阵单独保证。
一次场景可能包含许多暂停点、fork、取消及分配失败，不能把操作数当场景数。

按用户确认，6 类必须终止进程的基础设施错误逐类用独立进程验证；
编译期拒绝、类型约束与死亡测试不套用十万次指标。
模型事件和独立 stdexec 探针也不混入上述真实 Pause 计数。

## Release 运行矩阵

| 程序 | 每轮/矩阵的独立场景 | 每场景次数 | 本轮场景数 |
| --- | ---: | ---: | ---: |
| `pause_integration_test` | 18 固定配置（含 resume/stop 竞争）+ 2 随机参数竞争 | 100,000 | 2,000,004，含原有 4 条深链 |
| `pause_sender_test` | 43 | 100,000 | 4,300,000 |
| `pause_sender_hook_test` | 7 | 100,000 | 700,000 |
| `pause_resource_test` | 25 | 100,000 | 2,500,000 |
| `pause_task_lifetime_test` | 3 | 100,000 | 300,000 |
| `pause_extreme_test` | 25 | 100,000 | 2,500,000 |
| `pause_batch_extreme_test` | 52 | 100,000 | 5,200,000 |
| `pause_transition_extreme_test` | 11 | 100,000 | 1,100,000 |

以上运行均退出 0、未超时、stderr 为空。执行结果、准确耗时、参数、
源码和可执行文件 SHA256 保存在各自的 `*.result.json`。不把并行运行的
逐进程耗时相加称作总墙钟时间。

### 深度、宽度及组合结果

`pause_extreme_test` 的 5 种拓扑分别为：32/64/128 层纯链、5 层完整
二叉树（63 节点、32 叶）、32 层带侧支链（65 节点、33 叶）。每种拓扑
均执行 Continuous、SingleStep、全部叶暂停后根取消、普通业务 sender
error、普通业务 sender stopped，共 25 个独立场景。

业务失败路径先让除一个叶以外的所有叶停在 Pause，剩余叶在普通业务
sender 完成 error/stopped，由 `when_all` 自然取消兄弟；不人工 resume，
也不向根 stop source 请求取消。校验异常类型/内容、恰好一次最终结果、
每层直接 children 的实际退役、parent 重入以及最终 live/waiter 均为零。

最终 250 万完整 scope 包含 value 100 万、stopped 100 万、error 50 万；
实际 entry/retirement 各 177,500,000，join 57,400,000，SingleStep
恢复批次 52,700,000。该组用同一真实 run_loop 上的独立控制 task，
验证嵌套、组合和释放；多 CPU 竞争由其他组验证。

`pause_batch_extreme_test` 明确遍历：

- 宽度 1/2/8/64/256 × Continuous/SingleStep × 无取消/全取消/交替取消/头尾取消：40 类。
- 宽度 64、4 个持久 run_loop × 两种模式 × 四种取消方式：8 类。
- 宽度 2/64 × 两种模式，第一个 completion 自毁、同步取消兄弟并重入，然后才交付第二个节点：4 类。

每个初始 operation 都在公开 completion 内同步销毁，成功分支立即再次
进入暂停点。该组实际有 524,800,000 次暂停 operation 完成/自毁；取消
分支不执行第二次暂停点。用原 token 检查恢复调度，分别检查 schedule
工厂和 start 次数。Source、record 和节点保活到全部调用及工作队列尾部退出。

原有多控制方集成竞争额外执行 100,000 轮，每轮 4 个 run_loop、4 个
controller、4 个 stopper 和 observer；400,000 个分支恰好一次完成，
其中 value=54,683、stopped=345,317。随机 fork/pause 和 parent 重入/
resume 各 100,000 轮，seed=`4542363620118899781`。

### 状态转换与错误边界

新增 `pause_transition_extreme_test` 将随机总量无法保证的状态明确逐项执行：

- ControlRequest/SingleStep 收集中，两个已等待分支依次取消，第三个未到达责任仍使门保持 Collecting；最后责任退役后才成为空 Paused。
- 最后一个 child 在 Open/Collecting/Paused 各自退役；parent 不自动重新参与，下一暂停点才合法重入。
- fork→request_pause 和 request_pause→fork 两种确定串行顺序。
- parent 重入→resume 和 resume→重入，各自交叉 Continuous/SingleStep，检查旧 epoch、同轮 epoch、0/1 次调度。

这 11 类是锁序的确定性状态验证，与真实并发竞争互补，不声称用串行代码
模拟了 CPU 并发。每类为完整真实 scope，各执行 100,000 次。

sender 从 27 类增加至 43 类：补充 Continuous 恢复后的正常/排队取消、
默认 task 与 when_all 两种宿主，以及 Open 和控制请求 Collecting 下的
预取消，覆盖保留 operation 与同步自毁两种方式。

resource 从 19 类增加至 25 类：补充嵌套 parent 在 Open/Collecting 中
固定/动态 fork 的逐分配点失败、无提交/无通知/无泄漏及同 parent 成功重试；
零 count 在完全禁用分配及移动后 branch 上仍返回 InvalidForkCount。
既有 SIZE_MAX/length_error、task frame 分配失败、connect 后零分配路径
全部保留。每个可恢复分配失败点也随外层场景重复 100,000 次。

## 内存与数据竞争补充检查

MSVC `/O2 /Ob2 /DNDEBUG /MD /fsanitize=address` 下，下列最终源码各完成
1,000 次/场景，全部退出 0、stderr 为空：

| 测试 | 场景数 |
| --- | ---: |
| sender，43 类 | 43,000 |
| resource，25 类 | 25,000 |
| nested，25 类 | 25,000 |
| batch，52 类 | 52,000 |
| transition，11 类 | 11,000 |
| 合计 | **156,000** |

WSL Debian 13 / GCC 14.2 的 TSan 补充：sender 43 类各 1,000 次
（`-O1`），batch 52 类各 1,000 次（`-O2 -DNDEBUG`），共 **95,000**
场景，全部退出 0，无数据竞争报告。TSan 结果不计入 MSVC Release 十万次指标。

batch 的 Debug 不变量检查会在宽分支上反复重建关系，初次 `-O1` TSan
运行达到 300 秒超时（退出 124）；提高预算后的同配置运行仍很慢，主动停止
该测试进程，随后改用 `--release` 完整重跑并通过。两次未完成运行均保留，
不列成功、不累加场景数。首次个别 ASan 子进程因缺少 VS runtime PATH
返回 `0xC0000135`，没有执行测试；在 VsDevCmd 环境下重跑最终源码后通过，
原失败记录保留。

TSan 仍沿用测试专用 POSIX 声明 shim，避免 Linux `::pause()` 与公开
`namespace pause` 冲突；此结果不是未经 shim 的原生 Linux 兼容保证。
`setarch -R` 仅作用于测试进程，没有改变系统配置。

## 全套回归、模型与依赖探针

最终源码全量 CTest：**Debug 22/22（39.01 秒）、Release 22/22（12.38 秒）**。
包括多翻译单元、工具链、正负环境编译测试，以及 Debug 默认 STL iterator
proxy 的 6 个预期终止子进程。死亡路径要求准确命中 terminate handler 的
退出码 77，不将任意崩溃算通过。

独立依赖探针也以普通 Release 完成压力，均退出 0、stderr 为空：

| 依赖探针 | 本轮计数 |
| --- | ---: |
| 真实 stop callback | 700,005 |
| 确定性 stop callback 插桩 | 700,000 |
| callback / let_stopped / affinity 生命周期 | 400,000 |
| 旧 exec::task 生命周期对照 | 2,500,000 |
| stdexec::task API / 资源 | 1,300,000 |
| stdexec::task 生命周期 | 2,300,000 |
| stdexec::task scheduler | 3,500,000 |
| 合计 | **11,400,005** |

另外执行 48 个依赖深链回归。旧 task 的通过仅确认对照行为，没有将其
重新纳入 Pause 的受支持宿主。

模型完成 227 个定向场景、7 类故意缺陷拒绝、30,223 个有界状态/
63,792 次转换，以及 **2,000,000 个随机事件**（20 seeds × 100,000，
seed 基值 344693494597，最多 32 records）。随机过程最大 fork 深度 23，
完成 scope 23。这里的 scheduler stopped 只有 5,570 个，正说明模型
事件总量不能替代每个真实场景十万次的要求；本轮未据此声称模型每状态
也重复十万次。

## 审查发现与修订

未发现需要修改 `pause.hpp` 的实现缺陷。交叉审查修正了测试自身的两个问题：

1. latch 变为零并不保证最后一个 `count_down()` 的通知尾部已返回。新批量
   测试在释放 Round/branch/Source 前，为每个持久 run_loop 增加队列 fence。
2. 状态转换测试先退役 child 并完成预期失败 fork，再 connect/start parent
   暂停 operation，严格遵守同一 branch 串行使用契约。

报告只计入修订后源码的完整运行。旧 15 类 nested、修订前 transition、
无 fence 的 batch 小样不重复累计，也不作为最终源码的验收凭证。

## 证据与复现

主要日志目录：

- `build/pause-release-extreme/`：integration、sender、lifetime、最终 batch。
- `build/pause-release-extreme-final/`：最终 25 类 nested、最终 11 类 transition、最终重建 hook。
- `build/release-extreme-resource/`：25 类 resource 十万次；Debug proxy 边界见最终 CTest 日志。
- `build/release-extreme-extra-asan/`、`build/release-extreme-batch-asan/`、
  `build/release-extreme-nested/asan/`、`build/transition-extreme-asan/`：最终 ASan 证据。
- `build/release-extreme-tsan/`：sender 成功及首轮 batch 超时；
  `build/release-extreme-tsan-final/`：主动停止的 Debug batch；
  `build/release-extreme-tsan-o2/`：完整成功的 Release batch TSan。
- `build/release-extreme-final-all-ctest.log`：最终两种配置完整回归。
- `build/pause-release-extreme-summary.json`：实际场景计数与最终源码/二进制对应检查。
- `build/pause-release-components/`：模型及独立依赖探针，单独统计。

普通 Release 和 Debug 回归：

```powershell
.\scripts\msvc-cmake.cmd test all
```

严格 Release 长压力（每个进程默认 4 小时上限，参数、哈希和退出码分别落盘）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -Iterations 100000
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -OnlyTest pause_extreme_test -Iterations 100000
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -OnlyTest pause_batch_extreme_test -Iterations 100000
```

ExecutionPolicy 仅对该脚本进程设置，未修改系统执行策略。独立压力不会把
CTest 的短回归超时调大，也不会修改生产编译选项。

```sh
bash scripts/run-pause-tsan.sh --suite batch --release --iterations 1000 --build-dir build/release-extreme-tsan-o2 --timeout 600
bash scripts/run-pause-tsan.sh --suite sender --iterations 1000 --build-dir build/release-extreme-tsan --timeout 300
```

## 修改范围与结论边界

修改文件仅涉及测试和验证入口：`CMakeLists.txt`、
`tests/pause_sender_test.cpp`、`tests/pause_resource_test.cpp`、新增的
`tests/pause_extreme_test.cpp`、`tests/pause_batch_extreme_test.cpp`、
`tests/pause_transition_extreme_test.cpp`、`scripts/run-pause-release-stress.ps1`、
`scripts/run-pause-tsan.sh`、`tests/README.md` 和本报告。

没有公共 API、生产行为、持久化格式或依赖迁移。回滚只需撤销上述测试变更。
验证范围仍限定设计认可的 run_loop、默认 task allocator 和结构化使用；
非法并发使用同一 branch、提前销毁未完成 operation、未核准 scheduler、
64 位计数回绕及无限深度不在契约保证范围。

有限矩阵、十万次重复和 sanitizer 不能证明所有线程排列或无界栈深度。
深度 128 的通过也不能改写历史上其他极深组合的栈耗尽结论。无需新增
人工批准点；若以后扩大深度、后端或升级 stdexec，须重新做对应接入与
资源/取消/释放测试。
