> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# Pause 契约逐场景测试矩阵

本文把 [pause-branch-design.md](../pause-branch-design.md) 的有限状态、错误分支、生命周期交错和指定参数矩阵逐项列出。**所有可运行目标均已完成，实际次数达到各行所列目标；完整性核验缺项为0。** 具体汇总、复用与补测的区别及执行器收尾限制见[验收报告](pause-contract-validation-2026-09-25.md)和[有效证据JSON](../build/pause-contract-completion-20260925/evidence-summary.json)。静态、编译及契约外条目按各自方式验证，不计入运行次数。

“可枚举”指文档要求的状态/结果等价类及明确列出的参数组合，不指无限 task 数、任意深度、全部 OS 调度交错或任意外部 sender 的笛卡尔积。契约外用法、静态性质、编译拒绝、预期终止与合法运行期分开验证。重复编译不算运行压力；重复次数不是不同交错数。多行可核对同一次 scope 的不同契约，**不得把本表行数乘 repetitions 计算独立 scope 总数**。

## 执行目标与证据口径

- **O**：普通运行期，每个固定场景有效实际 **1,000,000** 次。
- **H**：含线程/较深结构的运行期，每个固定场景有效实际 **100,000** 次。
- **A/T**：适用运行期有效实际分别为 ASan、TSan **10,000** 次。若某平台/注入方式不支持，必须记录具体限制，不能填通过。
- **D**：明确约定的终止路径，每项 **1,000 个独立子进程**；核对故障确已注入和指定退出码，普通崩溃不算通过。
- **C**：相应配置正/负编译各一次；**S**：源码/布局/属性核对；**X**：契约外，不执行数据竞争/提前释放来冒充合法运行期测试。
- “旧 100k”等数字是下表指定函数/固定参数在**上一轮**循环中的调用次数；API 子断言共用其外围 scope。随机参数仅记录随机试验次数，不能把总次数赋给每种随机取值。
- exception/notification/outer/host 用例已实现并核对具体函数/参数；全部运行期共240个固定参数项，Release共95,100,000次，ASan/TSan各2,400,000次。有效证据由同日已完成且哈希匹配的记录与本次补测组成；更早的“旧100k”仅作历史对照，不再次累加。生产头、固定依赖或测试有变化后必须重新核对来源哈希。

| 缩写 | 测试文件与旧证据 | 上轮固定参数实际次数（仅辅助） |
| --- | --- | --- |
| I | [pause_integration_test.cpp](pause_integration_test.cpp)；[E-I](../build/pause-audit-20260925/release/pause_integration_test.result.json) | 20 循环项各100,000；循环外4条深链各1；A/T循环项各1,000 |
| S | [pause_sender_test.cpp](pause_sender_test.cpp)；[E-S](../build/pause-audit-20260925/release/pause_sender_test.result.json) | 43项各100,000；A/T各1,000；empty_task校准每进程1次 |
| H | [pause_sender_hook_test.cpp](pause_sender_hook_test.cpp)；[E-H](../build/pause-audit-20260925/release/pause_sender_hook_test.result.json) | 7项各100,000；A/T各1,000 |
| R | [pause_resource_test.cpp](pause_resource_test.cpp)；[E-R](../build/pause-audit-20260925/release/pause_resource_test.result.json) | 25外层family各100,000，每family内部逐分配位置；A各1,000；TSan未单独执行 |
| L | [pause_task_lifetime_test.cpp](pause_task_lifetime_test.cpp)；[E-L](../build/pause-audit-20260925/release/pause_task_lifetime_test.result.json) | 3项各100,000；A/T各1,000 |
| F | [pause_transition_extreme_test.cpp](pause_transition_extreme_test.cpp)；[E-F](../build/pause-audit-20260925/release/pause_transition_extreme_test.result.json) | 11项各100,000；A各1,000；TSan未单独执行 |
| E | [pause_extreme_test.cpp](pause_extreme_test.cpp)；[E-E](../build/pause-audit-20260925/release/pause_extreme_test.result.json) | 25个固定形状/深度/路径各100,000；A/T各1,000 |
| B | [pause_batch_extreme_test.cpp](pause_batch_extreme_test.cpp)；[E-B](../build/pause-audit-20260925/release/pause_batch_extreme_test.result.json) | 52个固定参数项各100,000；A/T各1,000 |
| P | [pause_concurrency_stress_test.cpp](pause_concurrency_stress_test.cpp)；[E-P](../build/pause-audit-20260925/release/pause_concurrency_stress_test.result.json) | 7项各10,000；A/T各1,000 |
| D | [pause_sender_death_test.cpp](pause_sender_death_test.cpp) | 6基础设施项：最终Debug、Release CTest各1子进程/项；ASan另1/项；不合并成压力通过 |
| EX | [pause_exception_matrix_test.cpp](pause_exception_matrix_test.cpp)，本轮新增 | 无旧次数；已实现10普通+19重型项，本轮执行见各行 |
| N | [pause_notification_matrix_test.cpp](pause_notification_matrix_test.cpp)，本轮新增 | 无旧次数；已实现6通知项，本轮执行见各行 |
| O | [pause_outer_lifetime_test.cpp](pause_outer_lifetime_test.cpp)，本轮新增 | 无旧次数；已实现4普通+2重型项，本轮执行见各行 |
| HOST | [pause_host_failure_test.cpp](pause_host_failure_test.cpp)，本轮新增 | 无旧次数；已实现2重型宿主失败项，本轮执行见各行 |
| C/S | [环境编译](check_pause_environment.cmake)、[头文件/ODR](pause_header_compile.cpp)、静态断言 | 上轮最终Debug/Release各构建及CTest一次，不是100k运行 |

上轮汇总：[报告](pause-concurrency-validation-2026-09-25.md)、[summary.json](../build/pause-audit-20260925/summary.json)、[ASan](../build/pause-audit-20260925/asan/audit-summary.json)、[TSan](../build/pause-audit-20260925/tsan/summary.json)。当前目标必须对应新的逐程序日志/输入哈希及实际 repetitions。

## API、拒绝顺序与门状态

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| API-001 | §3.1/3.2 | 空Source → snapshot → revision=0、root=false、Open、计数0 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-002 | §3.2/5.1 | 无root → make_branch → RootNotCreated，状态不提交 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-003 | §3.2/5.2 | 无root → request_pause → RootNotCreated | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-004 | §3.2/5.3 | 无root且epoch任意 → resume → RootNotCreated优先于NotPaused/epoch错误 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-005 | §5.1 | Continuous初始 → make_root → Open、责任1、单次revision/通知 | `P::root_creation_race(Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k |
| API-006 | §5.1 | SingleStep初始 → make_root → Collecting、责任1、step_round=0 | `P::root_creation_race(SingleStep)` 核对创建/phase/计数；`I::test_open_and_single_step(true)` 首轮核对round0 | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；联合属性证据，不额外计scope；旧 P 10k/I 100k |
| API-007 | §3.2 | root仍存活 → 再make_root → RootAlreadyCreated | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-008 | §3.2 | root已退役 → 再make_root → RootAlreadyCreated | `EX::root_recreate_after_retirement()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 缺口，本轮已新增用例 |
| API-009 | §3.2/5.2 | Open有责任 → request_pause → Collecting，不抢占、不伪造已经Paused | `I::test_all_arrive_or_retire` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-010 | §3.2/5.2 | Collecting → request_pause → PauseAlreadyCollecting，revision不变 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-011 | §3.2/5.2 | Paused → request_pause → AlreadyPaused，epoch不变 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-012 | §3.2/5.3 | Open且错误epoch → resume → NotPaused | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-013 | §3.2/5.3 | Collecting且错误epoch → resume → NotPaused优先于StalePauseEpoch | `EX::resume_collecting_wrong_epoch()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 缺口，本轮已新增用例 |
| API-014 | §3.2/5.3 | Paused+错误未来epoch → resume → StalePauseEpoch，无提交 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-015 | §3.2/5.3 | 新一轮Paused+旧epoch → resume → StalePauseEpoch | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-016 | §3.2/5.1 | root存在且Open → make_branch → 增加独立责任1 | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-017 | §3.2/5.1 | Collecting → make_branch → InvalidBranchState | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-018 | §3.2/5.1 | Paused → make_branch → InvalidBranchState | `EX::make_branch_paused()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 缺口，本轮已新增用例 |
| API-019 | §3.3/8.1 | 移出后branch+非零count → fork → InvalidBranchState | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-020 | §3.3/8.1 | parent尚有children → 第二次fork → InvalidBranchState且原children不受损 | `I::test_parent_reentry_and_epoch` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-021 | §4.4 | Open最后责任退出 → 保持Open，root_created仍为初始化事实 | `F::last_child_and_parent_reentry(Open)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| API-022 | §4.4/5.2 | Open零责任且root曾创建 → request_pause → 立即空Paused | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-023 | §4.4/5.3 | 空Paused+当前epoch → resume(Continuous) → Open | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-024 | §4.4/5.3 | 空Paused+当前epoch → resume(SingleStep) → 新空Paused、新epoch | `I::test_api_and_empty_gate` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-025 | §5.3 | 四个连续暂停点 → 每次SingleStep仅放行当前点间区间 | `I::test_open_and_single_step(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-026 | §6.2 | Open连续经过四个暂停点 → 不隐式yield、不强加恢复调度 | `I::test_open_and_single_step(false)；S::completion_reenters()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| API-027 | §5.3 | ControlRequest首选SingleStep → round=1 | `F::reentry_resume_order(true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| API-028 | §5.3 | SingleStep→Continuous→新普通暂停→SingleStep → round重置为1 | `EX::step_round_restarts()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 缺口，本轮已新增用例 |
| API-029 | §3.2/5.1 | 4线程同时make_root(Continuous) → 仅一成功、3拒绝，无丢失/重复记录 | `P::root_creation_race(Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k |
| API-030 | §3.2/5.1 | 4线程同时make_root(SingleStep) → 唯一登记并闭门 | `P::root_creation_race(SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k |
| API-031 | §3.2/5.1 | make_branch与request_pause同步起跑 → 两种锁序对应正确责任/拒绝 | `P::independent_creation_pause_race()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k；分支命中97成功/9903拒绝，不是两结果各10k |

## Snapshot、提交与核心不变量

每行核对的是具体性质，可能与前一节共用scope；Release的CHECK与Debug锁内重建必须区分。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| SNAP-001 | §3.1 | 读snapshot → 固定拥有型值，后续变更不改已复制snapshot | `R::same_snapshot；I::test_api_and_empty_gate（旧快照比较）` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R/I 100k |
| SNAP-002 | §3.1/4.5 | fork后parent仍活 → live只计children，不计record/frame数 | `I::test_parent_reentry_and_epoch` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| SNAP-003 | §3.1 | Resume摘链、队列未运行 → waiting=0而公开operation未完成 | `S::raw_route(true,false,false,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SNAP-004 | §3.1/4.3 | Collecting最后到达或退役 → epoch恰为该次revision | `F::fork_pause_order(true/false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 每参数100k |
| SNAP-005 | §3.1/7.3 | parent加入已Paused屏障 → revision增加、epoch保持 | `I::test_parent_reentry_and_epoch` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| SNAP-006 | §3.1/7.4 | Paused中取消一个waiter → live/waiting减1、epoch保持 | `L::retained_pause_stopped_frame` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 L 100k |
| SNAP-007 | §4.3 | callback撤责后final-stopped再记账 → 不重复减live/pending/revision | `L::retained_pause_stopped_frame；H::resume_unregisters_selected_callback` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 L/H各100k |
| SNAP-008 | §4.5.2–7 | 每次复合提交 → 重建live、pending、parent计数、waiter互链、phase约束 | `pause.hpp::assert_invariants_locked；commit_change_locked在revision/空Paused提交后调用；Debug运行全部真实tests` | 静态 | 本轮源码核对完成；Debug运行由suite日志记录，Release不执行该内部assert；不增加独立scope |
| SNAP-009 | §4.5.8–10 | claim+摘链+清branch.waiter同锁；completion唯一；外部动作锁外 | `H::resume_unregisters_selected_callback/resume_unregisters_entire_batch` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H各100k |
| SNAP-010 | §3.1/11.1 | observer并发读 → 每个snapshot自洽、revision不后退 | `I::test_resume_stop_controller_race` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| SNAP-011 | §3.1/12 | 空Paused、live0、waiting0 → 不把它们当scope完成/可析构 | `S::task_route(Stop/QueuedStop,join,SingleStep)；L::retained_pause_stopped_frame` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S/L各固定参数100k |
| SNAP-012 | §3.1 | tag为0/非零编号 → 不影响暂停裁决 | `B::create_operation(info.index)；P::recursive_task(info.node)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/P 10k；极值tag无专门比较 |
| SNAP-013 | §3.1 | tag=SIZE_MAX仍为纯诊断 → 行为与tag0相同 | `pause.hpp:550/556/557传值捕获info，418/432复制到operation，313/321仅存储；全头无info.tag决策读取` | 静态 | 本轮源码核对完成：所有tag在状态转移上等价，不需遍历2^size_t输入；不计运行次数 |

## 公开 sender 正常/取消/析构的16个固定组合

参数顺序为 raw_route(closed,cancel,empty_token,self_destroy,mode)。两种 self_destroy 分开编号；所有启动由宿主进入环境声明的run_loop。未指定affine的普通connect不增加宿主stopped跳转。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| SEND-001 | §6.1/6.4/7.6/14.3 | Open→start；token=有效；→value；完成后延迟销毁；schedule/start=0 | `S::raw_route(false,false,false,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-002 | §6.1/6.4/7.6/14.3 | Open→start；token=空；→value；完成后延迟销毁；schedule/start=0 | `S::raw_route(false,false,true,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-003 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=有效；→value；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,false,false,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-004 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=空；→value；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,false,true,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-005 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=有效；队列执行前stop→stopped/撤责；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,true,false,false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-006 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=有效；→value；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,false,false,false,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-007 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=空；→value；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,false,true,false,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-008 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=有效；队列执行前stop→stopped/撤责；完成后延迟销毁；schedule/start=1 | `S::raw_route(true,true,false,false,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-009 | §6.1/6.4/7.6/14.3 | Open→start；token=有效；→value；completion同步销毁当前operation；schedule/start=0 | `S::raw_route(false,false,false,true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-010 | §6.1/6.4/7.6/14.3 | Open→start；token=空；→value；completion同步销毁当前operation；schedule/start=0 | `S::raw_route(false,false,true,true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-011 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=有效；→value；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,false,false,true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-012 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=空；→value；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,false,true,true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-013 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(SingleStep)；token=有效；队列执行前stop→stopped/撤责；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,true,false,true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-014 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=有效；→value；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,false,false,true,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-015 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=空；→value；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,false,true,true,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-016 | §6.1/6.4/7.6/14.3 | 闭门Waiting→Resume(Continuous)；token=有效；队列执行前stop→stopped/撤责；completion同步销毁当前operation；schedule/start=1 | `S::raw_route(true,true,false,true,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |

## 构造前/Waiting取消与注册交错

CB-001..010为普通callback_stop的10项；CB-H为依赖副本钩子确定性交错。每项都验证只完成一次，stopped不进入value恢复调度。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| CB-001 | §7.3/7.4/7.6/14.2 | 初始SingleStep收集中；start已Waiting→callback赢得stopped；callback完成后operation保留再析构 | `S::callback_stop(false,false,2)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-002 | §7.3/7.4/7.6/14.2 | 初始SingleStep收集中；start前stop→构造中只记CancelRequested，构造返回后完成；callback完成后operation保留再析构 | `S::callback_stop(true,false,2)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-003 | §7.3/7.4/7.6/14.2 | Open；start前stop→构造中只记CancelRequested，构造返回后完成；callback完成后operation保留再析构 | `S::callback_stop(true,false,0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-004 | §7.3/7.4/7.6/14.2 | ControlRequest收集中；start前stop→构造中只记CancelRequested，构造返回后完成；callback完成后operation保留再析构 | `S::callback_stop(true,false,1)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-005 | §7.3/7.4/7.6/14.2 | ControlRequest收集中；start已Waiting→callback赢得stopped；callback完成后operation保留再析构 | `S::callback_stop(false,false,1)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-006 | §7.3/7.4/7.6/14.2 | 初始SingleStep收集中；start已Waiting→callback赢得stopped；completion自毁/同线程注销 | `S::callback_stop(false,true,2)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-007 | §7.3/7.4/7.6/14.2 | 初始SingleStep收集中；start前stop→构造中只记CancelRequested，构造返回后完成；completion自毁/同线程注销 | `S::callback_stop(true,true,2)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-008 | §7.3/7.4/7.6/14.2 | Open；start前stop→构造中只记CancelRequested，构造返回后完成；completion自毁/同线程注销 | `S::callback_stop(true,true,0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-009 | §7.3/7.4/7.6/14.2 | ControlRequest收集中；start前stop→构造中只记CancelRequested，构造返回后完成；completion自毁/同线程注销 | `S::callback_stop(true,true,1)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-010 | §7.3/7.4/7.6/14.2 | ControlRequest收集中；start已Waiting→callback赢得stopped；completion自毁/同线程注销 | `S::callback_stop(false,true,1)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| CB-H001 | §7.2–7.6/14.2 | 注册可见但emplace未返回→异线程stop→只记标志；live仍1/no completion；放行后由start撤责 | `H::registered_before_emplace_returns()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H002 | §7.2–7.6/14.2 | 构造期被选中、延后进函数→start推进Waiting→callback取得stopped | `H::selected_during_construction(false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H003 | §7.2–7.6/14.2 | 构造期被选中、延后进函数→start先InlinePass；并发request_pause不改本次value；reset锁外等待 | `H::selected_during_construction(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H004 | §7.2–7.6/14.2 | callback已selected未进函数→Resume先claim→reset等待；放callback后看CompletingValue退出；恢复队列观察stop | `H::resume_unregisters_selected_callback()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H005 | §7.2–7.6/14.2 | callback completion已发布→另一线程delete等待注销；第二次request_stop先返回仍不join首次请求 | `H::callback_completion_destroyed_elsewhere()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H006 | §7.2–7.6/14.2 | callback仍在注册链→Open完成显式注销→后来request_stop不调用它 | `H::unregister_before_request()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |
| CB-H007 | §7.2–7.6/14.2 | 两个共享token waiter→Resume先注销全批；第一项schedule期间同步stop不再触发任一旧callback | `H::resume_unregisters_entire_batch()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 H 100k |

## start尾部、再入与默认task affinity

TASK参数为 task_route(route,when_all,mode)。本体恢复/宿主stopped跳转次数分别核对，扣除host空task基线；记录schedule工厂调用与operation start，不能混算。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| TAIL-001 | §7.3/12/14.2 | start已发布Waiting但卡在测试通知尾部→callback完成且自毁operation→放行start，仅局部Source指针收尾 | `S::waiting_start_tail(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TAIL-002 | §6.1/7.3/14.3 | start尾部尚未返回→Resume仍必须schedule一次；该run_loop的实际value待start返回后执行 | `S::waiting_start_tail(false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-017 | §3.3/6.2 | 第一暂停点value continuation中let_value第二暂停点→两次Open直接通过0调度 | `S::completion_reenters()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| SEND-018 | §11.3/14.3 | 公开connect前复制receiver抛异常→普通异常可捕获，状态/通知不变；再次connect/start成功 | `S::public_connect_failure()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-001 | §3.4/6.5/14.3 | 直接task原token；SingleStep/Open→本体/宿主调度=0/0；value在声明资源继续 | `S::task_route(Open,false,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-002 | §3.4/6.5/14.3 | 直接task原token；SingleStep/Resume→本体/宿主调度=1/0；value在声明资源继续 | `S::task_route(Resume,false,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-003 | §3.4/6.5/14.3 | 直接task原token；SingleStep/Stop→本体/宿主调度=0/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(Stop,false,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-004 | §3.4/6.5/14.3 | 直接task原token；SingleStep/QueuedStop→本体/宿主调度=1/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(QueuedStop,false,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-005 | §3.4/6.5/14.3 | when_all组内token；SingleStep/Open→本体/宿主调度=0/0；value在声明资源继续 | `S::task_route(Open,true,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-006 | §3.4/6.5/14.3 | when_all组内token；SingleStep/Resume→本体/宿主调度=1/0；value在声明资源继续 | `S::task_route(Resume,true,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-007 | §3.4/6.5/14.3 | when_all组内token；SingleStep/Stop→本体/宿主调度=0/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(Stop,true,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-008 | §3.4/6.5/14.3 | when_all组内token；SingleStep/QueuedStop→本体/宿主调度=1/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(QueuedStop,true,SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-009 | §3.4/6.5/14.3 | 直接task原token；Continuous/Resume→本体/宿主调度=1/0；value在声明资源继续 | `S::task_route(Resume,false,Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-010 | §3.4/6.5/14.3 | 直接task原token；Continuous/QueuedStop→本体/宿主调度=1/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(QueuedStop,false,Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-011 | §3.4/6.5/14.3 | when_all组内token；Continuous/Resume→本体/宿主调度=1/0；value在声明资源继续 | `S::task_route(Resume,true,Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-012 | §3.4/6.5/14.3 | when_all组内token；Continuous/QueuedStop→本体/宿主调度=1/1；先撤责，宿主队列仍暂停时根未完成；放行后stopped/业务不继续 | `S::task_route(QueuedStop,true,Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |
| TASK-013 | §2.3/6.5/14.3 | 同Source两branch分处两个run_loop→同批Resume各回自己的执行资源，各仅1次恢复 | `S::two_resources()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 S 100k |

## fork、退役、parent重入的11个定向状态组合



| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| FORK-001 | §4.3/4.4/8 | ControlRequest：3子责任仅2到达→逐个取消已到达者仍Collecting；未到达child退役后才空Paused | `F::partial_collecting_cancel(false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-002 | §4.3/4.4/8 | SingleStep：3子责任仅2到达→逐个取消已到达者仍Collecting；未到达child退役后才空Paused | `F::partial_collecting_cancel(true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-003 | §4.2/7.3/8.2 | 最后child在Open退役→parent仍不参与，直接fork拒绝；下一暂停点才原子重入（Open直接value，闭门等待） | `F::last_child_and_parent_reentry(Open)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-004 | §4.2/7.3/8.2 | 最后child在Collecting退役→parent仍不参与，直接fork拒绝；下一暂停点才原子重入（Open直接value，闭门等待） | `F::last_child_and_parent_reentry(Collecting)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-005 | §4.2/7.3/8.2 | 最后child在Paused退役→parent仍不参与，直接fork拒绝；下一暂停点才原子重入（Open直接value，闭门等待） | `F::last_child_and_parent_reentry(Paused)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-006 | §4.5/8.1 | 暂停先于fork→同样3子责任；依次退役无中途伪Pausing，最终epoch=revision | `F::fork_pause_order(true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-007 | §4.5/8.1 | fork先于暂停→同样3子责任；依次退役无中途伪Pausing，最终epoch=revision | `F::fork_pause_order(false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-008 | §4.4/7.3/8.2 | 空Paused下Resume先，mode=Continuous→按锁序进入Open/当前批/下一空epoch；旧epoch正确拒绝 | `F::reentry_resume_order(true,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-009 | §4.4/7.3/8.2 | 空Paused下Resume先，mode=SingleStep→按锁序进入Open/当前批/下一空epoch；旧epoch正确拒绝 | `F::reentry_resume_order(true,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-010 | §4.4/7.3/8.2 | 空Paused下parent重入先，mode=Continuous→按锁序进入Open/当前批/下一空epoch；旧epoch正确拒绝 | `F::reentry_resume_order(false,Continuous)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |
| FORK-011 | §4.4/7.3/8.2 | 空Paused下parent重入先，mode=SingleStep→按锁序进入Open/当前批/下一空epoch；旧epoch正确拒绝 | `F::reentry_resume_order(false,SingleStep)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 F 100k |

## stdexec集成、外部完成与独立取消

每个明确Outcome/转换组合独立一行。禁止把外部业务error与Pause内部基础设施error的terminate策略混同。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| INT-001 | §5/10 | 4暂停点Continuous→业务4次正常完成 | `I::test_open_and_single_step(false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-002 | §5/10 | 4暂停点SingleStep→每轮业务只增1，最后Continuous清origin/round | `I::test_open_and_single_step(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-003 | §4.3 | 两个责任：一个已到达、另一个未到达→后者退役才Paused | `I::test_all_arrive_or_retire()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-004 | §8 | parent join后先重入，再次fork(3)成功，非消耗复用 | `I::test_parent_reentry_and_epoch()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-005 | §2.3/9.2 | 两个资源的两个独立token→正常Resume两者均value | `I::test_two_resources(false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-006 | §9.2 | 先取消其中独立范围→只撤其责任；另一范围可恢复正常业务 | `I::test_two_resources(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-007 | §9.1/9.3.1/14.4 | 外部A=Stopped、直接when_all，B挂起→when_all取消B，根正确停止/报业务error，无人工Resume | `I::test_external_completion(Stopped,false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-008 | §9.1/9.3.1/14.4 | 外部A=Error、直接when_all，B挂起→when_all取消B，根正确停止/报业务error，无人工Resume | `I::test_external_completion(Error,false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-009 | §9.1/9.3.1/14.4 | 外部A=Stopped、owning optional task包装，B挂起→A先退役再交value，B不被错误取消，暂停可收敛 | `I::test_external_completion(Stopped,true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-010 | §9.1/9.3.1/14.4 | 外部A=Value、owning optional task包装，B挂起→A先退役再交value，B不被错误取消，暂停可收敛 | `I::test_external_completion(Value,true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-011 | §9.1/9.3.1/14.4 | 外部A=Error、owning optional task包装，B挂起→when_all取消B，根正确停止/报业务error，无人工Resume | `I::test_external_completion(Error,true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-012 | §9.3 | stopped_as_optional直接包拥有branch的A→A frame保留，B到达后仍Collecting；测试宿主安全清理，不能额外Resume绕过 | `I::test_retained_stopped_counterexample()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-013 | §8/9/14.4 | 15-node递归树single_step=false,cancel=false→逐层fork、children释放后parent重入或根停止 | `I::test_recursive_fork(false,false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-014 | §8/9/14.4 | 15-node递归树single_step=true,cancel=false→逐层fork、children释放后parent重入或根停止 | `I::test_recursive_fork(true,false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-015 | §8/9/14.4 | 15-node递归树single_step=false,cancel=true→逐层fork、children释放后parent重入或根停止 | `I::test_recursive_fork(false,true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-016 | §2.3/9.2 | 一个task依次拥有两个Source的branch→各源状态/取消独立 | `I::test_cross_source_task()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k |
| INT-017 | §5.3/7.6/11.1 | 4 controllers争同旧epoch+4 stoppers+observer→仅1 resume成功、每branch一次完成、最终空新Paused | `I::test_resume_stop_controller_race()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k；value/stopped命中数另记 |
| INT-018 | §8.1 | 随机count1..4、模式、预取消mask/yield参数下fork/pause竞争→合法结果；不是每种取值各重复目标次数 | `I::test_random_fork_pause(Random&)，seed=4542363620118899781` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k随机轮；逐参数频次未记录 |
| INT-019 | §8.2 | 随机mode/yield下parent重入/resume竞争→Open或新epoch合法；不是两个锁序各重复目标次数 | `I::test_random_reentry_resume(Random&)，同seed` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 100k随机轮；两个定向顺序见FORK-008..011 |
| DEEP-001 | §8/14.4 | 真实链depth=32,single_step=false→65暂停点、所有branch清理 | `I::test_deep_chain(32,false)；本轮需--deep-iterations控制` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 仅1/项；旧--iterations不重复循环外deep |
| DEEP-002 | §8/14.4 | 真实链depth=32,single_step=true→65暂停点、所有branch清理 | `I::test_deep_chain(32,true)；本轮需--deep-iterations控制` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 仅1/项；旧--iterations不重复循环外deep |
| DEEP-003 | §8/14.4 | 真实链depth=64,single_step=false→129暂停点、所有branch清理 | `I::test_deep_chain(64,false)；本轮需--deep-iterations控制` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 仅1/项；旧--iterations不重复循环外deep |
| DEEP-004 | §8/14.4 | 真实链depth=64,single_step=true→129暂停点、所有branch清理 | `I::test_deep_chain(64,true)；本轮需--deep-iterations控制` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 I 仅1/项；旧--iterations不重复循环外deep |
| LIFE-001 | §9.3.1/12/14.4 | Pause callback撤责但stopped frame仍由when_all保留→不重复撤责，不提前递减parent children；B完成后children先于parent退役 | `L::retained_pause_stopped_frame()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 L 100k |
| LIFE-002 | §9.3.1/12/14.4 | optional wrapper的starts_on预取消→wrapper未进入但stopped仍取消挂起兄弟，记录释放 | `L::optional_host_precancelled()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 L 100k |
| LIFE-003 | §9.3.1/12/14.4 | wrapper结果之后额外可取消等待stopped→继续向组合器传播取消，不能视作不可取消affinity | `L::optional_host_later_cancellable_wait()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 L 100k |

## 资源分配、每个可捕获故障位置及零分配路径

R::test_fork 的 child_count=4。下表逐record位置列出，不把“25个外层family”误称为仅25个故障点。每次失败比较全部snapshot、通知和净存储，再在同一个parent上重试。动态fork的release分配序号为reserve=0、record=1..4；MSVC Debug默认迭代器代理存在时相应为1、2..5，代理的不可捕获noexcept分配另列death。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| ALLOC-001 | §5.1/11.3 | Continuous空Source→root record第0次分配失败→ResourceExhausted、原snapshot/通知/净分配不变 | `R::test_root(Continuous), Failure(0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-002 | §5.1/11.3 | Continuousroot record准备成功→一次完整提交；离开作用域净存储回到基线 | `R::test_root(Continuous), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-003 | §5.1/11.3 | SingleStep空Source→root record第0次分配失败→ResourceExhausted、原snapshot/通知/净分配不变 | `R::test_root(SingleStep), Failure(0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-004 | §5.1/11.3 | SingleSteproot record准备成功→一次完整提交；离开作用域净存储回到基线 | `R::test_root(SingleStep), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-005 | §5.1/11.3 | Open已有root→独立record分配失败→ResourceExhausted，root仍可用 | `R::test_independent_branch(), Failure(0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-006 | §5.1/11.3 | 独立record成功→live+1/一次通知；退役后回原责任数 | `R::test_independent_branch(), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-007 | §8.1/11.3 | Continuous/root parent→fixed fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,false), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-008 | §8.1/11.3 | Continuous/root parent→fixed fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,false), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-009 | §8.1/11.3 | Continuous/root parent→fixed fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,false), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-010 | §8.1/11.3 | Continuous/root parent→fixed fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,false), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-011 | §8.1/11.3 | Continuous/root parent→fixed fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<false>(Continuous,false), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-012 | §8.1/11.3 | Continuous/nested parent→fixed fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,true), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-013 | §8.1/11.3 | Continuous/nested parent→fixed fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,true), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-014 | §8.1/11.3 | Continuous/nested parent→fixed fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,true), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-015 | §8.1/11.3 | Continuous/nested parent→fixed fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(Continuous,true), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-016 | §8.1/11.3 | Continuous/nested parent→fixed fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<false>(Continuous,true), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-017 | §8.1/11.3 | SingleStep/root parent→fixed fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,false), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-018 | §8.1/11.3 | SingleStep/root parent→fixed fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,false), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-019 | §8.1/11.3 | SingleStep/root parent→fixed fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,false), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-020 | §8.1/11.3 | SingleStep/root parent→fixed fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,false), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-021 | §8.1/11.3 | SingleStep/root parent→fixed fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<false>(SingleStep,false), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-022 | §8.1/11.3 | SingleStep/nested parent→fixed fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,true), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-023 | §8.1/11.3 | SingleStep/nested parent→fixed fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,true), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-024 | §8.1/11.3 | SingleStep/nested parent→fixed fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,true), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-025 | §8.1/11.3 | SingleStep/nested parent→fixed fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<false>(SingleStep,true), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-026 | §8.1/11.3 | SingleStep/nested parent→fixed fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<false>(SingleStep,true), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-027 | §8.1/11.3 | Continuous/root parent→动态vector元素buffer reserve失败→完全回滚，同parent重试成功 | `R::test_fork<true>(Continuous,false), reserve故障` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-028 | §8.1/11.3 | Continuous/root parent→dynamic fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,false), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-029 | §8.1/11.3 | Continuous/root parent→dynamic fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,false), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-030 | §8.1/11.3 | Continuous/root parent→dynamic fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,false), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-031 | §8.1/11.3 | Continuous/root parent→dynamic fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,false), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-032 | §8.1/11.3 | Continuous/root parent→dynamic fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<true>(Continuous,false), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-033 | §8.1/11.3 | Continuous/nested parent→动态vector元素buffer reserve失败→完全回滚，同parent重试成功 | `R::test_fork<true>(Continuous,true), reserve故障` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-034 | §8.1/11.3 | Continuous/nested parent→dynamic fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,true), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-035 | §8.1/11.3 | Continuous/nested parent→dynamic fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,true), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-036 | §8.1/11.3 | Continuous/nested parent→dynamic fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,true), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-037 | §8.1/11.3 | Continuous/nested parent→dynamic fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(Continuous,true), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-038 | §8.1/11.3 | Continuous/nested parent→dynamic fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<true>(Continuous,true), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-039 | §8.1/11.3 | SingleStep/root parent→动态vector元素buffer reserve失败→完全回滚，同parent重试成功 | `R::test_fork<true>(SingleStep,false), reserve故障` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-040 | §8.1/11.3 | SingleStep/root parent→dynamic fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,false), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-041 | §8.1/11.3 | SingleStep/root parent→dynamic fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,false), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-042 | §8.1/11.3 | SingleStep/root parent→dynamic fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,false), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-043 | §8.1/11.3 | SingleStep/root parent→dynamic fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,false), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-044 | §8.1/11.3 | SingleStep/root parent→dynamic fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<true>(SingleStep,false), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-045 | §8.1/11.3 | SingleStep/nested parent→动态vector元素buffer reserve失败→完全回滚，同parent重试成功 | `R::test_fork<true>(SingleStep,true), reserve故障` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-046 | §8.1/11.3 | SingleStep/nested parent→dynamic fork第0个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,true), child_record=0` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-047 | §8.1/11.3 | SingleStep/nested parent→dynamic fork第1个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,true), child_record=1` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-048 | §8.1/11.3 | SingleStep/nested parent→dynamic fork第2个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,true), child_record=2` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-049 | §8.1/11.3 | SingleStep/nested parent→dynamic fork第3个child record分配失败→已准备record全部释放、未转交责任、重试成功 | `R::test_fork<true>(SingleStep,true), child_record=3` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-050 | §8.1/11.3 | SingleStep/nested parent→dynamic fork全部准备成功→一次责任转交，4children退役后live0 | `R::test_fork<true>(SingleStep,true), 首个不失败index` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-001 | §8.1/11.3 | Continuous/moved_valid，故障注入第0次分配→fork<0>直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(Continuous), moved_valid, fixed` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-002 | §8.1/11.3 | Continuous/moved_valid，故障注入第0次分配→动态count=0直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(Continuous), moved_valid, dynamic` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-003 | §8.1/11.3 | Continuous/moved_from，故障注入第0次分配→fork<0>直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(Continuous), moved_from, fixed` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-004 | §8.1/11.3 | Continuous/moved_from，故障注入第0次分配→动态count=0直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(Continuous), moved_from, dynamic` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-005 | §8.1/11.3 | SingleStep/moved_valid，故障注入第0次分配→fork<0>直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(SingleStep), moved_valid, fixed` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-006 | §8.1/11.3 | SingleStep/moved_valid，故障注入第0次分配→动态count=0直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(SingleStep), moved_valid, dynamic` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-007 | §8.1/11.3 | SingleStep/moved_from，故障注入第0次分配→fork<0>直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(SingleStep), moved_from, fixed` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ZERO-008 | §8.1/11.3 | SingleStep/moved_from，故障注入第0次分配→动态count=0直接InvalidForkCount，分配尝试0、状态不变 | `R::test_zero_count_before_allocation(SingleStep), moved_from, dynamic` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-LENGTH | §8.1/11.3 | count=SIZE_MAX→vector length_error转换ResourceExhausted、无部分责任提交 | `R::test_fork_length_error()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| ALLOC-TASK | §10.2/14.4 | 4 children中前2个dormant task已拥有branch、第3个frame分配抛bad_alloc→失败child和已有dormant siblings按生命周期释放，业务未进入 | `R::test_task_creation_failure()` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-P0 | §11.2/14.4 | connect后禁止所有分配→普通Pause route=Open正常完成；branch退役也不分配 | `R::test_no_allocation_after_connect(0,false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-P1 | §11.2/14.4 | connect后禁止所有分配→普通Pause route=Resume SingleStep正常完成；branch退役也不分配 | `R::test_no_allocation_after_connect(1,false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-P2 | §11.2/14.4 | connect后禁止所有分配→普通Pause route=callback stopped正常完成；branch退役也不分配 | `R::test_no_allocation_after_connect(2,false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-P3 | §11.2/14.4 | connect后禁止所有分配→普通Pause route=queued-schedule stopped正常完成；branch退役也不分配 | `R::test_no_allocation_after_connect(3,false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-E0 | §7.3/11.2 | connect后禁止分配、默认空token→Open；无callback注册项且符合正常调度 | `R::test_no_allocation_after_connect(0,true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-E1 | §7.3/11.2 | connect后禁止分配、默认空token→Waiting恢复；无callback注册项且符合正常调度 | `R::test_no_allocation_after_connect(1,true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-T0 | §6.5/11.2 | 默认task connect后禁止分配→Open，公开completion和责任清理正常 | `R::test_task_no_allocation_after_connect(0)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-T1 | §6.5/11.2 | 默认task connect后禁止分配→Resume，公开completion和责任清理正常 | `R::test_task_no_allocation_after_connect(1)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-T2 | §6.5/11.2 | 默认task connect后禁止分配→callback stopped+host hop，公开completion和责任清理正常 | `R::test_task_no_allocation_after_connect(2)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |
| NOALLOC-T3 | §6.5/11.2 | 默认task connect后禁止分配→queued stopped+host hop，公开completion和责任清理正常 | `R::test_task_no_allocation_after_connect(3)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 R 100k |

## 预期终止逐子进程矩阵

这些是已承诺的12种终止路径。每项分别运行1000子进程，核对预期terminate handler的退出码77以及不可达业务回调，不能以任何非零退出码替代。MSVC Debug代理项仅在该STL及_ITERATOR_DEBUG_LEVEL>0时适用；Release不执行并注明N/A。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| DEATH-body-schedule | §6.3/6.5/11.3/14.3 | Waiting恢复，进入本体调度后schedule工厂抛异常→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<false>(Failure::Schedule), --child body-schedule` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-body-connect | §6.3/6.5/11.3/14.3 | Waiting恢复，进入本体调度后内部connect抛异常→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<false>(Failure::Connect), --child body-connect` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-body-error | §6.3/6.5/11.3/14.3 | Waiting恢复，进入本体调度后调度operation交付error→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<false>(Failure::Error), --child body-error` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-affine-schedule | §6.3/6.5/11.3/14.3 | 本体已stopped并撤责，进入外层host affinity后schedule工厂抛异常→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<true>(Failure::Schedule), --child affine-schedule` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-affine-connect | §6.3/6.5/11.3/14.3 | 本体已stopped并撤责，进入外层host affinity后内部connect抛异常→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<true>(Failure::Connect), --child affine-connect` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-affine-error | §6.3/6.5/11.3/14.3 | 本体已stopped并撤责，进入外层host affinity后调度operation交付error→终止77，不追加通知/业务value/stopped/error/let_error恢复 | `D::child<true>(Failure::Error), --child affine-error` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug/Release各1/项，ASan另1/项 |
| DEATH-proxy-continuous-0 | §11.3 | MSVC Debug动态fork4：第0次代理分配失败（vector默认构造，提交前）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(0,Continuous), --proxy-child 0 continuous` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |
| DEATH-proxy-continuous-6 | §11.3 | MSVC Debug动态fork4：第6次代理分配失败（参数noexcept移动，提交前）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(6,Continuous), --proxy-child 6 continuous` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |
| DEATH-proxy-continuous-7 | §11.3 | MSVC Debug动态fork4：第7次代理分配失败（返回expected的noexcept移动，提交后）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(7,Continuous), --proxy-child 7 continuous` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |
| DEATH-proxy-step-0 | §11.3 | MSVC Debug动态fork4：第0次代理分配失败（vector默认构造，提交前）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(0,SingleStep), --proxy-child 0 step` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |
| DEATH-proxy-step-6 | §11.3 | MSVC Debug动态fork4：第6次代理分配失败（参数noexcept移动，提交前）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(6,SingleStep), --proxy-child 6 step` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |
| DEATH-proxy-step-7 | §11.3 | MSVC Debug动态fork4：第7次代理分配失败（返回expected的noexcept移动，提交后）→指定bad_alloc终止77；不要求回滚 | `R::proxy_failure_child(7,SingleStep), --proxy-child 7 step` | 预期终止 | D 1000：通过（有效实际达到上述次数）；旧 最终Debug CTest1/项；旧Release不适用 |

## 新增的parent移动与并发OOM逐项

新测试源码已出现；下表仍只登记待执行项，不预先填通过。移动发生在同branch的既有operation已结束后，children records可保留；未制造同branch并发误用。并发OOM在锁外分配处由独立协调者放行，不持Pause锁等待。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| MOVE-root-open | §3.3/4.5.1/8.3 | Open且两个child records存活→move已委托root并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(false,Open)；case move.root.open` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| MOVE-root-collecting | §3.3/4.5.1/8.3 | Collecting且两个child records存活→move已委托root并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(false,Collecting)；case move.root.collecting` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| MOVE-root-paused | §3.3/4.5.1/8.3 | Paused且两个child records存活→move已委托root并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(false,Paused)；case move.root.paused` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| MOVE-nested-open | §3.3/4.5.1/8.3 | Open且两个child records存活→move已委托嵌套parent并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(true,Open)；case move.nested.open` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| MOVE-nested-collecting | §3.3/4.5.1/8.3 | Collecting且两个child records存活→move已委托嵌套parent并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(true,Collecting)；case move.nested.collecting` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| MOVE-nested-paused | §3.3/4.5.1/8.3 | Paused且两个child records存活→move已委托嵌套parent并析构源holder，无分配/通知/状态变化；child退役仍更新稳定parent record；parent重入后可继续 | `EX::moved_delegated_parent(true,Paused)；case move.nested.paused` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-root-continuous | §5.1/11.3 | 调用者A的root record分配卡住→B成功提交root→A抛bad_alloc仍报ResourceExhausted且不覆盖B；A无故障重试才RootAlreadyCreated | `EX::root_oom_competitor(Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-root-singlestep | §5.1/11.3 | 调用者A的root record分配卡住→B成功提交root→A抛bad_alloc仍报ResourceExhausted且不覆盖B；A无故障重试才RootAlreadyCreated | `EX::root_oom_competitor(SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-independent-rootcommit | §5.1/11.3 | 独立branch分配挂起期间root首次登记提交→故障方ResourceExhausted且快照保留同伴提交；合法时机重试成功 | `EX::independent_oom_competitor(IndependentRace::RootCommit)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-independent-peercreate | §5.1/11.3 | 独立branch分配挂起期间同伴独立branch登记提交→故障方ResourceExhausted且快照保留同伴提交；合法时机重试成功 | `EX::independent_oom_competitor(IndependentRace::PeerCreate)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-independent-pause | §5.1/11.3 | 独立branch分配挂起期间request_pause关门提交→故障方ResourceExhausted且快照保留同伴提交；合法时机重试成功 | `EX::independent_oom_competitor(IndependentRace::Pause)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-open-reserve | §8.1/11.3 | parent A的vector reserve失败窗口→同伴fork提交→A完全回滚、保留同伴提交、同A重试成功；门最终Open | `EX::fork_oom_competitor<true>(false,std::nullopt)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本次补齐 |
| OOM-fork-dynamic-collecting-reserve | §8.1/11.3 | parent A的vector reserve失败窗口→同伴先request_pause再fork提交→A完全回滚、保留同伴提交、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<true>(true,std::nullopt)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本次补齐 |
| OOM-fork-fixed-open-r0 | §8.1/11.3 | parent A准备fork3第0record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<false>(false,0)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-fixed-open-r1 | §8.1/11.3 | parent A准备fork3第1record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<false>(false,1)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-fixed-open-r2 | §8.1/11.3 | parent A准备fork3第2record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<false>(false,2)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-fixed-collecting-r0 | §8.1/11.3 | parent A准备fork3第0record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<false>(true,0)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-fixed-collecting-r1 | §8.1/11.3 | parent A准备fork3第1record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<false>(true,1)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-fixed-collecting-r2 | §8.1/11.3 | parent A准备fork3第2record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<false>(true,2)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-open-r0 | §8.1/11.3 | parent A准备fork3第0record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<true>(false,0)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-open-r1 | §8.1/11.3 | parent A准备fork3第1record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<true>(false,1)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-open-r2 | §8.1/11.3 | parent A准备fork3第2record失败窗口→同伴fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终Open | `EX::fork_oom_competitor<true>(false,2)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-collecting-r0 | §8.1/11.3 | parent A准备fork3第0record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<true>(true,0)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-collecting-r1 | §8.1/11.3 | parent A准备fork3第1record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<true>(true,1)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| OOM-fork-dynamic-collecting-r2 | §8.1/11.3 | parent A准备fork3第2record失败窗口→同伴先request_pause再fork提交→A完全回滚、不回滚同伴、同A重试成功；门最终空Paused | `EX::fork_oom_competitor<true>(true,2)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |

## 宽批次52项完整参数展开

B::run_case(Pool, suite,width,mode,cancel,N)每项独立重复。None=无取消；All=全取消；Alternating=偶数index取消；Ends=头尾取消（宽1只有该唯一节点）；AfterFirst=首个completion自毁/再入并同步取消尚未交付兄弟。除AfterFirst外先把恢复调度排队并保持worker gate，再请求stop并释放队列。所有选中节点必须stopped，存活节点两次operation按预期value；计数及schedule/start次数精确检查。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| BATCH-M-w1-c-none | §6.4/7.2/12/14.2–3 | 1 worker,width=1,Continuous,cancel=None→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-c-all | §6.4/7.2/12/14.2–3 | 1 worker,width=1,Continuous,cancel=All→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-c-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=1,Continuous,cancel=Alternating→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-c-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=1,Continuous,cancel=Ends→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-s-none | §6.4/7.2/12/14.2–3 | 1 worker,width=1,SingleStep,cancel=None→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-s-all | §6.4/7.2/12/14.2–3 | 1 worker,width=1,SingleStep,cancel=All→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-s-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=1,SingleStep,cancel=Alternating→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w1-s-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=1,SingleStep,cancel=Ends→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",1,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-c-none | §6.4/7.2/12/14.2–3 | 1 worker,width=2,Continuous,cancel=None→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-c-all | §6.4/7.2/12/14.2–3 | 1 worker,width=2,Continuous,cancel=All→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-c-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=2,Continuous,cancel=Alternating→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-c-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=2,Continuous,cancel=Ends→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-s-none | §6.4/7.2/12/14.2–3 | 1 worker,width=2,SingleStep,cancel=None→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-s-all | §6.4/7.2/12/14.2–3 | 1 worker,width=2,SingleStep,cancel=All→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-s-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=2,SingleStep,cancel=Alternating→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w2-s-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=2,SingleStep,cancel=Ends→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",2,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-c-none | §6.4/7.2/12/14.2–3 | 1 worker,width=8,Continuous,cancel=None→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-c-all | §6.4/7.2/12/14.2–3 | 1 worker,width=8,Continuous,cancel=All→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-c-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=8,Continuous,cancel=Alternating→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-c-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=8,Continuous,cancel=Ends→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-s-none | §6.4/7.2/12/14.2–3 | 1 worker,width=8,SingleStep,cancel=None→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-s-all | §6.4/7.2/12/14.2–3 | 1 worker,width=8,SingleStep,cancel=All→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-s-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=8,SingleStep,cancel=Alternating→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w8-s-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=8,SingleStep,cancel=Ends→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",8,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-c-none | §6.4/7.2/12/14.2–3 | 1 worker,width=64,Continuous,cancel=None→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-c-all | §6.4/7.2/12/14.2–3 | 1 worker,width=64,Continuous,cancel=All→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-c-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=64,Continuous,cancel=Alternating→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-c-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=64,Continuous,cancel=Ends→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-s-none | §6.4/7.2/12/14.2–3 | 1 worker,width=64,SingleStep,cancel=None→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-s-all | §6.4/7.2/12/14.2–3 | 1 worker,width=64,SingleStep,cancel=All→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-s-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=64,SingleStep,cancel=Alternating→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w64-s-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=64,SingleStep,cancel=Ends→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",64,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-c-none | §6.4/7.2/12/14.2–3 | 1 worker,width=256,Continuous,cancel=None→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-c-all | §6.4/7.2/12/14.2–3 | 1 worker,width=256,Continuous,cancel=All→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-c-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=256,Continuous,cancel=Alternating→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-c-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=256,Continuous,cancel=Ends→恢复后取消只由队列接受；幸存者下一点直接通过；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-s-none | §6.4/7.2/12/14.2–3 | 1 worker,width=256,SingleStep,cancel=None→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-s-all | §6.4/7.2/12/14.2–3 | 1 worker,width=256,SingleStep,cancel=All→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-s-alternating | §6.4/7.2/12/14.2–3 | 1 worker,width=256,SingleStep,cancel=Alternating→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-M-w256-s-ends | §6.4/7.2/12/14.2–3 | 1 worker,width=256,SingleStep,cancel=Ends→恢复后取消只由队列接受；幸存者进入下一屏障，全取消形成空新epoch；所有operation完成后retire records | `B::run_case(pool(1),"matrix",256,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-c-none | §6.4/7.2/12/14.2–3 | 4 workers,width64,Continuous,cancel=None→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,Continuous,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-c-all | §6.4/7.2/12/14.2–3 | 4 workers,width64,Continuous,cancel=All→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,Continuous,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-c-alternating | §6.4/7.2/12/14.2–3 | 4 workers,width64,Continuous,cancel=Alternating→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,Continuous,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-c-ends | §6.4/7.2/12/14.2–3 | 4 workers,width64,Continuous,cancel=Ends→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,Continuous,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-s-none | §6.4/7.2/12/14.2–3 | 4 workers,width64,SingleStep,cancel=None→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,SingleStep,Cancel::None,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-s-all | §6.4/7.2/12/14.2–3 | 4 workers,width64,SingleStep,cancel=All→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,SingleStep,Cancel::All,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-s-alternating | §6.4/7.2/12/14.2–3 | 4 workers,width64,SingleStep,cancel=Alternating→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,SingleStep,Cancel::Alternating,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-P-w64-s-ends | §6.4/7.2/12/14.2–3 | 4 workers,width64,SingleStep,cancel=Ends→各回原资源、唯一completion、取消不遗留pending；每worker队列fence后销毁scope | `B::run_case(pool(4),"parallel",64,SingleStep,Cancel::Ends,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-R-w2-c | §7.2/7.6/14.2 | 1worker,width2,Continuous,AfterFirst→首项自毁、取消剩余兄弟并重入后才允许交付第二项；整批callback已注销，私有未交付节点仍活 | `B::run_case(pool(1),"reentrant",2,Continuous,Cancel::AfterFirst,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-R-w2-s | §7.2/7.6/14.2 | 1worker,width2,SingleStep,AfterFirst→首项自毁、取消剩余兄弟并重入后才允许交付第二项；整批callback已注销，私有未交付节点仍活 | `B::run_case(pool(1),"reentrant",2,SingleStep,Cancel::AfterFirst,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-R-w64-c | §7.2/7.6/14.2 | 1worker,width64,Continuous,AfterFirst→首项自毁、取消剩余兄弟并重入后才允许交付第二项；整批callback已注销，私有未交付节点仍活 | `B::run_case(pool(1),"reentrant",64,Continuous,Cancel::AfterFirst,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |
| BATCH-R-w64-s | §7.2/7.6/14.2 | 1worker,width64,SingleStep,AfterFirst→首项自毁、取消剩余兄弟并重入后才允许交付第二项；整批callback已注销，私有未交付节点仍活 | `B::run_case(pool(1),"reentrant",64,SingleStep,Cancel::AfterFirst,N)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 B 100k/该参数 |

## 真实递归树/深链25项固定矩阵

E每轮每个下列组合各调用一次。它们测试结构化退役和嵌套退出；E自身只有一个run_loop消费线程，不能把这些行单独称CPU并行证据。多worker递归另见PAR。外部error/stopped路径的叶operation由独立的异步controller释放，不等同依赖探针error-fast拓扑。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| TREE-chain-d32-continuous | §8/9/12/14.4 | Chain/depth32/Continuous→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,32,Path::Continuous,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d32-singlestep | §8/9/12/14.4 | Chain/depth32/SingleStep→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,32,Path::SingleStep,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d32-pausedcancel | §8/9/12/14.4 | Chain/depth32/PausedCancel→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,32,Path::PausedCancel,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d32-externalerror | §8/9/12/14.4 | Chain/depth32/ExternalError→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,32,Path::ExternalError,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d32-externalstopped | §8/9/12/14.4 | Chain/depth32/ExternalStopped→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,32,Path::ExternalStopped,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d64-continuous | §8/9/12/14.4 | Chain/depth64/Continuous→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,64,Path::Continuous,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d64-singlestep | §8/9/12/14.4 | Chain/depth64/SingleStep→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,64,Path::SingleStep,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d64-pausedcancel | §8/9/12/14.4 | Chain/depth64/PausedCancel→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,64,Path::PausedCancel,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d64-externalerror | §8/9/12/14.4 | Chain/depth64/ExternalError→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,64,Path::ExternalError,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d64-externalstopped | §8/9/12/14.4 | Chain/depth64/ExternalStopped→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,64,Path::ExternalStopped,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d128-continuous | §8/9/12/14.4 | Chain/depth128/Continuous→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,128,Path::Continuous,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d128-singlestep | §8/9/12/14.4 | Chain/depth128/SingleStep→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,128,Path::SingleStep,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d128-pausedcancel | §8/9/12/14.4 | Chain/depth128/PausedCancel→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,128,Path::PausedCancel,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d128-externalerror | §8/9/12/14.4 | Chain/depth128/ExternalError→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,128,Path::ExternalError,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-chain-d128-externalstopped | §8/9/12/14.4 | Chain/depth128/ExternalStopped→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Chain,128,Path::ExternalStopped,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-binary-d5-continuous | §8/9/12/14.4 | Binary/depth5/Continuous→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Binary,5,Path::Continuous,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-binary-d5-singlestep | §8/9/12/14.4 | Binary/depth5/SingleStep→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Binary,5,Path::SingleStep,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-binary-d5-pausedcancel | §8/9/12/14.4 | Binary/depth5/PausedCancel→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Binary,5,Path::PausedCancel,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-binary-d5-externalerror | §8/9/12/14.4 | Binary/depth5/ExternalError→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Binary,5,Path::ExternalError,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-binary-d5-externalstopped | §8/9/12/14.4 | Binary/depth5/ExternalStopped→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Binary,5,Path::ExternalStopped,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-comb-d32-continuous | §8/9/12/14.4 | Comb/depth32/Continuous→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Comb,32,Path::Continuous,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-comb-d32-singlestep | §8/9/12/14.4 | Comb/depth32/SingleStep→所有节点入口与parent重入业务各按预期执行，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Comb,32,Path::SingleStep,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-comb-d32-pausedcancel | §8/9/12/14.4 | Comb/depth32/PausedCancel→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Comb,32,Path::PausedCancel,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-comb-d32-externalerror | §8/9/12/14.4 | Comb/depth32/ExternalError→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Comb,32,Path::ExternalError,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |
| TREE-comb-d32-externalstopped | §8/9/12/14.4 | Comb/depth32/ExternalStopped→取消或外部非value传播，业务不越过关闭屏障，直接children先于parent退役、最终记录全释放 | `E::run_scope(Shape::Comb,32,Path::ExternalStopped,counts)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 E 100k/该参数 |

## 多worker递归7项

PAR-001..003与API-029..031复用同scope，列在这里便于执行器逐行回填；不得重复统计scope。每个递归scope深3、15节点；正常路径实际暂停operation总数22，恢复批次数随子树重入时机变化，不强求固定7轮。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| PAR-001 | §3.2/8/9/12/14.4 | 4 callers抢root，初始Continuous，唯一登记与释放 | `P::root_creation_race(Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-002 | §3.2/8/9/12/14.4 | 4 callers抢root，初始SingleStep，唯一登记与闭门 | `P::root_creation_race(SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-003 | §3.2/8/9/12/14.4 | make_branch/关门并发，两种合法结果按实际命中分别报告 | `P::independent_creation_pause_race()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-004 | §3.2/8/9/12/14.4 | 4 run_loop递归Continuous：实际跨线程fork/记录退役，parent回自身resource重入 | `P::recursive_scope(Path::Continuous)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-005 | §3.2/8/9/12/14.4 | 4 run_loop递归SingleStep：按快照放行，parent可在不同epoch重入 | `P::recursive_scope(Path::SingleStep)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-006 | §3.2/8/9/12/14.4 | 全部8个叶到达时根request_stop：7个内部node已通过入口，叶业务不执行，全部frame退役 | `P::recursive_scope(Path::Cancel)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |
| PAR-007 | §3.2/8/9/12/14.4 | 叶屏障放行后左most leaf BusinessFailure：不再Resume，嵌套when_all取消兄弟，根error而非terminate | `P::recursive_scope(Path::Error)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 P 10k/该参数 |

## 通知/观察者逐项

N的notifier只更新线程安全信号，不阻塞、不调用Pause API。观察者窗口在宿主handler上协调；乱序票据不携带snapshot、也不授权Resume。每observer自行串行，两个observer可并行读。NOTIFY ID与程序stdout完全一致。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| NOTIFY-01-coalesced | §11.1/12.3/12.8/14.4 | 4次状态提交→每个observer只消费一次合并通知，均读最新revision4；notifier_calls=4、observer_reads=2 | `N::coalesced_observers()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| NOTIFY-02-reordered-extra | §11.1/12.3/12.8/14.4 | 3次提交→宿主按3/1/2/3/0乱序、重复、额外投递；每次读当前snapshot不回退；calls3/reads10 | `N::reordered_dispatch()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| NOTIFY-03-dirty-before-snapshot | §11.1/12.3/12.8/14.4 | 先清dirty、读snapshot前发生新提交→本次读新snapshot且dirty仍留给下一次；calls3/reads6 | `N::dirty_window(Observers::Window::BeforeSnapshot)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| NOTIFY-04-dirty-after-snapshot | §11.1/12.3/12.8/14.4 | 清dirty并读后、handler结束前发生提交→本次可读旧snapshot但dirty保留，下次读新值；calls3/reads6 | `N::dirty_window(Observers::Window::AfterSnapshot)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| NOTIFY-05-concurrent-notifiers | §11.1/12.3/12.8/14.4 | 4线程创建/退役独立branch，2observer按阶段核对snapshot；calls10/reads6；记录notifier实际重叠次数，不预设每scope必重叠 | `N::concurrent_notifiers()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |
| NOTIFY-06-shutdown-wakeup-join | §11.1/12.3/12.8/14.4 | 2 idle observer atomic_wait→停机置quit并额外唤醒→借用尾部仍受Source保活直到join；calls0/reads2 | `N::shutdown_wakeup_join()` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；旧 无（本轮新增） |

## 真实Pause外包完成、host尾部与宿主失败

O的host-tail用例调用真实`branch.suspend_point().affine()`；透明run_loop调度外观只在`schedule.start()`已经入队后停留原调用栈，不通过`task_scheduler`擦除，也不阻塞notifier。外观在start前保存外部控制指针，此后不再读取可能已经销毁的operation。Source和stop_source一直保留到原request_stop返回、worker join。HOST测试验证§10.2宿主骨架；它不增加Pause API的取消/异常保证。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| OUTER-001-open-stopped | §6.2/9.3/14.3 | Open的Pause先value，外包let_value再just_stopped → 保留branch责任；request_pause仍Collecting直到branch退役 | `O::outer_after_value<false>(false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| OUTER-002-open-error | §6.2/9.3/14.3 | Open的Pause先value，外包let_value再just_error(42) → 外部error可正常交付，保留branch责任直到退役 | `O::outer_after_value<true>(false)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| OUTER-003-resumed-stopped | §6.2/9.3/14.3 | Waiting→resume→一次schedule→Pause value→外包stopped → 不追溯撤销已value的branch责任 | `O::outer_after_value<false>(true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| OUTER-004-resumed-error | §6.2/9.3/14.3 | Waiting→resume→一次schedule→Pause value→外包error42 → 不进入Pause内部terminate，不追溯撤责 | `O::outer_after_value<true>(true)` | 运行期 | O 1M + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| OUTER-005-host-receiver-destroy | §7.6/12/14.3 | 真实callback stopped完成权已撤责，affine host跳转在worker极速交付；terminal CPO内销毁outer，而原request_stop仍卡在schedule.start尾部 | `O::host_stop_inflight(true)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| OUTER-006-host-coordinator-destroy | §7.6/12/14.3 | 同上但worker terminal通知后由独立协调器销毁outer；原request_stop尾部放行并join后才销毁Source | `O::host_stop_inflight(false)` | 运行期 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本轮新增 |
| HOST-001-controller-creation | §10.2/12 | worker已执行真实run_loop排队操作、task尚未start → controller创建边界模拟system_error → finish/join先完成，未启动frame/branch完整清理 | `HOST::controller_creation_failure()` | 运行期/宿主 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本轮新增，不耗尽OS线程 |
| HOST-002-controller-business | §10.2/12 | 复用example_support::run_scope，真实workflow等待Pause → controller业务抛错 → host取消workflow、唤醒并join controller、finish/join worker后重抛 | `HOST::controller_business_failure()` | 运行期/宿主 | H 100k + A/T 10k：通过（有效实际达到上述次数）；本轮新增，断言frame退役/revision5/epoch3 |

## 编译接口、属性与静态审查

每个编译配置只构建一次；负例只有在相同环境正例成功后失败才算拒绝证据。没有把编译次数当压力次数。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| COMPILE-001 | §3.2 | Source不能copy construct | `I文件作用域is_copy_constructible_v<PauseSource>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-002 | §3.2 | Source不能move construct | `I文件作用域is_move_constructible_v<PauseSource>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-003 | §3.3 | branch不能copy construct | `I文件作用域is_copy_constructible_v<PauseBranch>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-004 | §3.3 | branch move construct noexcept | `I文件作用域is_nothrow_move_constructible_v<PauseBranch>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-005 | §3.3 | branch不能move assign | `I文件作用域is_move_assignable_v<PauseBranch>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-006 | §3.2 | 非const左值source可以request_pause | `I::CanRequestPause<PauseSource&>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-007 | §3.2 | const左值source不能request_pause | `I::CanRequestPause<const PauseSource&>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-008 | §3.2 | 右值source不能request_pause | `I::CanRequestPause<PauseSource>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-009 | §3.2 | const左值source可以snapshot | `I::CanSnapshot<const PauseSource&>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-010 | §3.2 | 右值source不能snapshot | `I::CanSnapshot<PauseSource>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-011 | §3.3 | 左值branch可以suspend_point | `I::CanSuspend<PauseBranch&>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-012 | §3.3 | 右值branch不能suspend_point | `I::CanSuspend<PauseBranch>静态断言` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-013 | §3.5 | Raw sender静态consteval签名为Inline/Resumed/stopped，带/不带env和const引用均一致 | `S文件作用域RawSignatures 4个static_assert` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-014 | §3.5 | Raw operation有operation_state_concept tag且满足operation_state | `S文件作用域RawOperation static_assert` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-015 | §6.5 | 公开sender带合格env的value属性=inline|asynchronous_affine | `S文件作用域get_completion_behavior<set_value_t,Sender,Env>` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-016 | §6.5 | 公开sender无env的value属性保守unknown | `S文件作用域get_completion_behavior<set_value_t,Sender>` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-017 | §6.5 | 公开sender不支持env的value属性保守unknown | `S文件作用域get_completion_behavior<set_value_t,Sender,env<>>` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-018 | §3.4 | 合格receiver env能connect，作为所有负例的正对照 | `pause_environment_compile.cpp, PAUSE_ENVIRONMENT_CASE=0` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-019 | §3.4 | 缺get_start_scheduler→编译拒绝，即便运行本可走Open | `pause_environment_compile.cpp, PAUSE_ENVIRONMENT_CASE=1` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-020 | §3.4 | receiver提供std::stop_token而非inplace_stop_token→编译拒绝 | `pause_environment_compile.cpp, PAUSE_ENVIRONMENT_CASE=2` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-021 | §3.4 | 缺get_stop_token/default never token→编译拒绝 | `pause_environment_compile.cpp, PAUSE_ENVIRONMENT_CASE=3` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-022 | §3.4 | get_start_scheduler返回int→编译拒绝 | `pause_environment_compile.cpp, PAUSE_ENVIRONMENT_CASE=4` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-023 | §1/3.5/14.5 | 先声明POSIX::pause再include，以及反向次序→命名无冲突 | `pause_header_compile.cpp + pause_header_other.cpp` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| COMPILE-024 | §14.5 | 两个translation unit同头定义→链接无ODR重复定义 | `pause_header_compile目标` | 编译 | 每配置1次：通过（有效实际达到上述次数）；旧Debug/Release各1，CASE3/4为本轮新增 |
| STATIC-001 | §3.2/3.3 | Source copy/move assignment删除；控制API的const/rvalue限制、fork lvalue限定 | `pause.hpp:520/524/526/667/668/669/670及各&限定；pause_environment_compile.cpp的CanMakeRoot/CanMakeIndependent/CanResume/CanForkFixed/CanForkDynamic和3个assignment断言` | 静态/编译 | 本轮源码核对完成；新增编译断言由COMPILE-018正对照统一构建，不额外计scope |
| STATIC-002 | §3.2/11.1 | notifier早于state构造、晚于state析构；state按值，不反向持有Source所有权 | `PauseSource:743..745按initial_mode/notifier/state声明；PauseStateData:155只借用StateChangedNotifier*` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-003 | §4.1 | record由唯一branch拥有，intrusive列表/parent只是借用且只数直接children | `PauseBranch:609 unique_ptr；BranchRecord:133 parent裸指针；IntrusiveList仅first/last借用；fork登记直接parent且退役只减该parent.live_children` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-004 | §7.1 | callback成员最后声明、析构最先注销，保护receiver/token等成员 | `RawSuspendOperation:312..317按state/info/receiver/token/callback声明；无自定义析构反转顺序` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-005 | §7.3/7.6 | callback构造锁外；Registering只记CancelRequested，不触optional | `start:355 callback.emplace先于锁；on_stop:397 Registering路径只赋phase并return；optional不被callback读写` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-006 | §7.2/7.6 | 显式reset唯一方；锁外全批reset后才逐项交付 | `complete_value_batch:288先遍历reset再pop/deliver；start:386只有own!=None才reset；callback胜出路径依赖正常完成析构注销` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-007 | §11.2 | 锁内不分配、不真正释放record、不notify、不schedule/connect/start、不调用terminal CPO | `prepare在make_branch_impl/fork锁前；分配失败/状态拒绝先退lock_guard再销毁局部children；branch析构锁外notify后成员unique_ptr释放；*_locked仅链表/整数/optional/不变量扫描` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-008 | §6.3/7.6/12 | 所有terminal CPO为最后语义操作，不再读取当前operation/receiver | `Raw complete每个CPO后return；on_stop末尾CPO；PolicyReceiver末尾CPO；Waiting发布后start仅读预存s/own/changed，ownNone直接return` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-009 | §3.4/6.3 | PolicyReceiver.get_env原样转发公开receiver env；不注入Pause专属token | `PolicyReceiver:481 decltype(auto)返回stdexec::get_env(receiver)，没有env叠层或自有stop_source` | 静态 | 本轮源码核对完成；GAP-005详述转发边界；不计运行次数 |
| STATIC-010 | §6.5 | Attrs除自身value属性外只转发forwarding queries；raw不宣称公开value affinity | `Attrs:446..467约束forwarding_query、query(inner,args...)合法性，decltype(auto)且条件noexcept；仅Account=true屏蔽自身value属性；Raw无attrs查询` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-011 | §6.5 | unstoppable只用于本体stopped之后的host跳转，不屏蔽正常恢复schedule取消 | `affine:503..512只在let_stopped内unstoppable(schedule)；suspend_point:562的ResumedPass原样schedule(scheduler)` | 静态 | 本轮源码核对完成；不计运行次数 |
| STATIC-012 | §3.4/14.3 | run_loop schedule/connect noexcept；两种token环境operation同布局且均适配内嵌存储 | `__run_loop.hpp:263/301；__task_scheduler.hpp:245..255/658/785；__parallel_scheduler_backend.hpp:384；本轮layout-probe静态断言及布局输出` | 静态/布局 | 本轮核对完成：MSVC x64 Debug两类op均56字节/align8，storage72/align8；具体证据与可移植性边界见下文，不计压力次数 |
| STATIC-013 | §3.4 | task默认env不保证转发任意自定义查询；额外query必须显式TaskEnv配置 | `固定__task.hpp:726..758显式scheduler/allocator/token及受_TaskEnv约束的forwarding query；stdexec_standard_task_api_test对应false/73对照` | 静态 | 本轮源码核对完成；不是任意外层receiver查询自动穿透task的承诺 |
| STATIC-014 | §11.3 | 准备分配在状态检查前，所以OOM可先于并发状态错误；只有进入锁后才用文档错误优先级 | `make_branch_impl:750 prepare先于756状态检查；fork:577/592先prepare/reserve，commit_prepared_fork:623再检查branch/phase` | 静态 | 本轮源码核对完成；EX并发OOM是独立运行证据，不在此重复计scope |
| STATIC-015 | §3.1 | snapshot同一锁内复制、waiting从链表数；读snapshot本身不提交revision或通知 | `snapshot:732..739仅lock、遍历waiters、按值return；没有commit_change_locked/notify_changed调用` | 静态 | 本轮源码核对完成；N::reordered_dispatch独立验证重复读取 |
| STATIC-016 | §2.2/9.2 | Pause不拥有stop_source，不提供全局停止状态，不广播取消 | `公开API无stop操作；Raw保存receiver token并注册callback；withdraw_locked只改变该branch、pending/live整数，无request_stop` | 静态 | 本轮源码核对完成；INT-006验证不连通token独立性 |
| STATIC-017 | §11.3 | std::mutex系统失败、基础不变量require失败为fail-fast，非PauseError可恢复路径 | `lock_state:73 noexcept包mutex加锁；require:67先assert再terminate；不在构造损坏的mutex上执行UB探针` | 静态 | 本轮源码核对完成；DIAG死亡用例只覆盖可在危险访问前诊断的子集 |
| STATIC-018 | §3.5 | 固定草案协议迁移不等于切换原生std::execution；公开API/依赖后端不扩展 | `CMakeLists.txt:10 pin f0e8ae6fdc6c188389b146bb854b80d399724b04；RawSuspendSender/RawSuspendOperation均使用stdexec协议tag/静态签名；configure仅检接口` | 静态 | 本轮源码核对完成；tests/check_stdexec.cpp不是布局或所有后端验收的替代 |

## 契约外的可诊断实现行为

新增pause_contract_death_test目标强制NDEBUG，仅验证当前require的terminate fallback。进程在已确认的前置条件处armed后应退出77；没有承诺所有误用在所有优化/库版本下都有相同诊断。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| DIAG-empty-notifier | §3.3/7.3/8.3/11.1 | 构造空notifier→在使用空callable前require终止 | `pause_contract_death_test.cpp::child("empty-notifier"), --child empty-notifier` | 契约外/预期终止 | 1000独立子进程：通过（有效实际达到上述次数）；本轮新增 |
| DIAG-moved-from-suspend | §3.3/7.3/8.3/11.1 | 从已移出branch调用suspend_point→无记录require终止 | `pause_contract_death_test.cpp::child("moved-from-suspend"), --child moved-from-suspend` | 契约外/预期终止 | 1000独立子进程：通过（有效实际达到上述次数）；本轮新增 |
| DIAG-parent-release-with-children | §3.3/7.3/8.3/11.1 | parent尚有child record时退役→检查live_children后终止，未放行悬空parent | `pause_contract_death_test.cpp::child("parent-release-with-children"), --child parent-release-with-children` | 契约外/预期终止 | 1000独立子进程：通过（有效实际达到上述次数）；本轮新增 |
| DIAG-parent-reentry-with-children | §3.3/7.3/8.3/11.1 | parent尚有child record时start新暂停点→检查live_children后终止 | `pause_contract_death_test.cpp::child("parent-reentry-with-children"), --child parent-reentry-with-children` | 契约外/预期终止 | 1000独立子进程：通过（有效实际达到上述次数）；本轮新增 |
| DIAG-second-active-waiter | §3.3/7.3/8.3/11.1 | 同branch已有Waiting时再start另一operation→检查branch.waiter后终止 | `pause_contract_death_test.cpp::child("second-active-waiter"), --child second-active-waiter` | 契约外/预期终止 | 1000独立子进程：通过（有效实际达到上述次数）；本轮新增 |

## 明确排除的输入、误用和非保证

这些行不设运行压力成功目标；不能为满足数量制造UB后把未崩溃当通过。若文档只assert而没有Release诊断保证，应如实注明。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| OUT-001 | §3.1 | revision/epoch/step计数回绕，不增加回绕处理保证 | `计数类型和排除声明；不运行至2^64` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-002 | §3.3 | 并行move/fork/suspend/析构同一个branch | `禁止同一owner并发访问；无合法正常结果oracle` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-003 | §3.3/12 | 公开operation尚未完成即析构以强制取消 | `禁止；不以提前delete制造压力` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-004 | §12 | Source先于branch/operation/callback/control/observer/notify tail析构 | `Source只Debug assert，Release不保证检测；不得运行悬空访问` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-005 | §7.6 | 两个线程同时reset同一optional、reset与提前析构并发 | `库只保证执行对唯一清理方注销，不保证任意optional并发修改` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-006 | §7.6 | 持Pause mutex reset并等待需要同锁的callback | `设计禁止锁顺序；正常test不制造必死锁` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-007 | §7.6 | callback线程同步等等待自己退出才能完成的析构线程 | `等待环禁止；独立协调器才可释放测试latch` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-008 | §11.1 | notifier阻塞、同步重入Pause API、捕获失效对象或非线程安全调用 | `禁止生产行为；已有waiting_start_tail只是受控测试注入` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-009 | §3.4 | 声明env scheduler但实际start不在其执行上下文 | `宿主错误；concept不能证明执行上下文` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-010 | §3.4 | 未核准scheduler/allocator/TaskEnv、来源未知task_scheduler | `必须先做后端接入验收；不视为当前支持范围` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-011 | §3.4 | 擦除后端connect/分配走std::unreachable | `先拒绝该后端/修复；不执行UB再把崩溃计death成功` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-012 | §3.3 | 保存sender供未来任务、detach、branch生命周期未覆盖adaptor外层 | `禁止；不提供自动延寿或恢复协议` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-013 | §8.2 | fork后parent未join/未重入便继续受保护业务 | `用户逻辑违反暂停契约；Pause不抢占任意业务语句` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-014 | §6.2 | Open暂停点当作yield保证单worker其他任务公平执行 | `不保证；宿主必须显式异步工作/让出` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-015 | §9.3.1 | optional包装推广为void、悬空pointer/view等任意结果类型 | `本文示例仅拥有型可optional非void；不虚构通用封装` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-016 | §2.1/6.4 | stop一被请求就禁止之后执行任何业务指令 | `不承诺；按完成权线性化，不能追溯改写value` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-017 | §7.6 | noexcept的resume/reset具有硬实时或无等待时限 | `异线程callback注销可spin/yield，无完成时限保证` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-018 | §9.3 | 任意外部sender一stopped就要求Pause同步撤销所有任务责任 | `Pause没有该观测；RAII退役/宿主组合负责` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-019 | §12 | 仅第二次request_stop返回就销毁stop_source | `二次请求不是join，源必须覆盖首次请求栈` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |
| OUT-020 | §14.5 | 任意平台、任意深度或任意线程交错的全称安全 | `有限模型/压力不能证明；已知依赖error-fast栈溢出不计通过` | 契约外 | 不适用；不把编译/死亡/UB与合法运行次数混算 |

## 特殊边界的覆盖方式与不可达证明

这些稳定GAP编号保留此前发现的边界及现在的精确落点，不增加独立scope。新增运行用例落地不等于大规模执行完成，实际次数仍由本轮日志填写。所选后端无法触发的路径归入源码/布局证明；不凭空引入未验收后端来制造异常。

| ID | 条款 | 前置 → 操作 → 期望 | 具体函数/参数 | 类别 | 本轮目标/实际；旧证据 |
| --- | --- | --- | --- | --- | --- |
| GAP-001 | §14.3 | 真实Pause的host stopped跳转异线程极速完成/销毁outer，而原callback terminal CPO尚未返回 | `OUTER-005/006；O::host_stop_inflight(true/false)` | 运行期 | 已实现真实Pause精确窗口；H100k+A/T10k实际达到目标并通过；旧依赖探针不代替本轮次数 |
| GAP-002 | §6.2/9.3/14.3 | 外包adaptor在公开Pause已value后才产生stopped | `OUTER-001/003；真实Pause \| let_value(just_stopped)` | 运行期 | 已实现Open/Resumed两项；O1M+A/T10k实际达到目标并通过 |
| GAP-003 | §6.2/9.3/14.3 | 外包adaptor在公开Pause已value后才产生error | `OUTER-002/004；真实Pause \| let_value(just_error(42))` | 运行期 | 已实现Open/Resumed两项；O1M+A/T10k实际达到目标并通过 |
| GAP-004 | §6.3/14.3 | let分派或环境处理自身抛异常（非schedule工厂/内部connect）到达terminate包装 | `__let.hpp:190..214；__read_env.hpp:60/118；__task.hpp:728；STATIC-012和下述固定后端证明；DEATH捕获工厂/connect注入` | 静态/不可达 | 本轮源码/布局核对完成：本选定默认task+run_loop内无独立可抛copy/query入口；保守error签名不等于真实异常入口 |
| GAP-005 | §3.4/14.3 | 额外合法forwarding query跨Pause自身包装的保持 | `STATIC-009/010；PolicyReceiver原env返回及Attrs query模板逐表达式核对，详见下文` | 静态 | 本轮模板核对完成；默认task不承诺任意query透传，新增TaskEnv/后端仍需单独接入验收 |
| GAP-006 | §10.2 | 宿主controller线程构造失败或自身异常后worker正确finish/join | `HOST-001/002；controller_creation_failure/controller_business_failure；后者直接复用example_support::run_scope` | 运行期/宿主 | 已实现2项，H100k+A/T10k实际达到目标并通过；构造边界模拟system_error不等同真实耗尽系统线程 |
| GAP-007 | §7.3/12 | start通知尾部仍活时同资源恢复value已完成且operation销毁 | `TAIL-002覆盖start返回前Resume摘链并排队；固定__run_loop.hpp:62/168..195同步执行当前任务，不能并行取下个任务` | 静态/当前拓扑不可达 | 本轮证明完成，详见下文单消费者/同资源/不嵌套run假设；非未执行的可行case，不计运行次数 |
| GAP-008 | §13/14 | 同一个已完成operation被二次start、其他不能安全提前诊断的误用 | `operation单次start是前置条件；DIAG-second-active-waiter测的是两个不同op而非二次start同一op` | 契约外 | 不适用；不要在callback optional上重复emplace制造UB |

### 固定后端的异常、布局与查询审查记录

核对对象是固定提交`f0e8ae6fdc6c188389b146bb854b80d399724b04`下的`run_loop`、默认`stdexec::task`、默认分配器与当前编译选项。源码链接根为[固定stdexec include](../build/msvc-debug/_deps/stdexec-src/include/stdexec/__detail/)。改变这些前提需要重新验收，不能从scheduler concept推出同样的结果。

1. **schedule operation的存储。** `__run_loop.hpp:121..164`的operation仅有intrusive任务基类、计数指针、队列指针和receiver，token分支只在execute函数体内选择value/stopped。`__parallel_scheduler_backend.hpp:384..421`的proxy receiver只含proxy引用、storage指针和删除函数指针；环境类型只决定`get_env()`的返回类型，不增加成员。因此stoppable与unstoppable不改变这个operation布局。`__task_scheduler.hpp:247..255`验证unstoppable operation适配预分配大小/对齐，`659..674`只有超出大小才分配，`788/792`分别实例化两种环境。审查探针[layout.cpp](../build/pause-audit-20260925/layout-probe/layout.cpp)明确静态断言两种尺寸/对齐相等且均适配，在MSVC x64 Debug实测[layout.result.txt](../build/pause-audit-20260925/layout-probe/layout.result.txt)：**56/56字节、align8；内嵌storage72字节、align8**。这一次布局探针不计压力次数；其他平台通过各自实际构建的固定后端断言与NOALLOC用例核对。

2. **scheduler复制的存储。** `task_scheduler`类型擦除本身的复制并非对任意后端都声明`noexcept`；不能据外层`schedule() noexcept`就断言无分配。固定后端`__backend_for`仅包含run_loop scheduler指针和空默认allocator，`__any.hpp:732`复制转入`__emplace_into`，`340..352/633..645`对小且可无异常移动的model使用原位构造。MSVC类布局[build.log](../build/pause-audit-20260925/layout-probe/build.log)记录这个具体backend的`__value_model`为**16字节**；`__interface_base`的24字节默认payload加vptr给出32字节存储，满足小对象路径；复制最终只是指针/空allocator。`NOALLOC-T0..3`另以真实task热路径检查分配次数。这是所选backend的证明，不适用于来源未知的task_scheduler。

3. **let分派与环境。** `__let.hpp:190..214`把实参存储、调用工厂和内部连接/提交的异常捕获为`set_error(exception_ptr)`；Pause的两层`PolicyReceiver::set_error`都直接terminate，工厂/connect可达注入由DEATH各项验证。当前Pause工厂捕获的`s/b/info`、`InlinePass/ResumedPass`和具体scheduler没有独立可抛业务对象；empty `just`/`just_stopped`不携带可抛值。`run_loop::schedule/connect`均noexcept，固定擦除内嵌路线如上没有分配/可抛copy。`__task.hpp:728`的`get_start_scheduler`返回所保存scheduler并声明noexcept，`__read_env.hpp:60/118`据查询的nothrow性质生成路径；直接receiver则读取测试中固定`prop`值。Pause lambda没有声明noexcept，故静态完成签名仍可保守含error；这不意味着固定配置存在另一个可达的动态异常入口。任意自定义scheduler/query/allocator抛错不是本次假设下漏跑的case，更不能让stdexec内部unreachable后把崩溃计为正确死亡。

4. **查询转发范围。** `PolicyReceiver::get_env()`以`decltype(auto)`原样返回下游`get_env(receiver)`，不复制改写query值，不构造Pause专属stop环境。`Attrs`仅在`forwarding_query(Query{})`为真、内层`query(inner,args...)`合法时参与重载；返回`decltype(auto)`保留引用/值类别，`noexcept(noexcept(query(inner,args...)))`保留异常规格；唯一屏蔽项是`Account=true`时由自己提供的value completion behavior。`Account=false`外层不屏蔽它。这个模板证明适用于实际内层已有的query，不声称制造内层不存在的query。固定默认task的env只明示scheduler/allocator/token，并仅将显式`TaskEnv`支持的额外forwarding query导出；`__parallel_scheduler_backend.hpp:262..276`擦除代理只分派三个内建查询。任意外层query跨越默认task的保留并非Pause保证，现有标准task查询探针的默认false/显式TaskEnv值73与此一致。

5. **start尾部与同资源value的时序。** GAP-007限定每个run_loop只有一个执行`run()`的worker，Pause start确实位于receiver声明的同一个scheduler资源，宿主不在notifier/start尾部递归调用`run()`，也不把该资源交给第二个消费者。该worker在`__execute_all()`中同步调用当前任务，当前task里的raw start及notify未返回时，它无法执行随后入队的恢复schedule。因此控制线程虽可在Waiting发布后resume、摘链并入队，公开value完成仍必须等待当前worker调用栈返回；TAIL-002已验证可达的发布/摘链/入队窗口。callback线程发出stopped后跳回worker属于另一窗口，OUTER-005/006专门覆盖；不能拿这一stopped窗口证明或要求上述同资源value不可达窗口。增加合法多消费者后端或改变宿主拓扑需要新接入验收。

## §13 边界总表逐行索引

这张表仅用于确认设计总表每一行都有分类和测试落点，不增加独立次数。期望与精确参数以上述同名 ID 的行为列为准。

| 文档行ID | §13场景 | 矩阵落点 |
| --- | --- | --- |
| D13-01 | Open直接通过 | SEND-001/002/009/010；TASK-001/005 |
| D13-02 | Open连续暂停点不隐式yield | API-026、SEND-017、OUT-014 |
| D13-03 | 预停止构造内回调 | CB-002/003/004/007/008/009 |
| D13-04 | 构造期间异线程已选callback | CB-H001/002/003 |
| D13-05 | Resume与callback竞争 | CB-H004；INT-017；SEND队列stop项 |
| D13-06 | selected但尚未进函数时注销 | CB-H004 |
| D13-07 | callback stopped同步自毁 | CB-006..010 |
| D13-08 | completion通知异线程析构 | CB-H005 |
| D13-09 | Waiting发布但start未返回即Resume | TAIL-002 |
| D13-10 | Open裁决后pause/stop | CB-H003 |
| D13-11 | SingleStep调度stopped撤销next pending | SEND-005/013；TASK-004/008；BATCH全取消项 |
| D13-12 | 本体stopped但host跳转排队 | TASK-003/004/007/008；LIFE-001 |
| D13-13 | 暂停内部error终止 | DEATH-body-*；DEATH-affine-* |
| D13-14 | Paused中单waiter取消 | SNAP-006、LIFE-001 |
| D13-15 | 所有已等待者取消但尚有未到达责任 | FORK-001/002 |
| D13-16 | 暂停与fork原子排序 | FORK-006/007；INT-018；OOM-fork-* |
| D13-17 | child责任撤销但frame保留 | LIFE-001 |
| D13-18 | 最后child在任意Gate退役 | FORK-003/004/005 |
| D13-19 | parent Paused重入 | FORK-005；SNAP-005 |
| D13-20 | parent重入与Resume顺序 | FORK-008..011；INT-019 |
| D13-21 | 空SingleStep新epoch | API-024 |
| D13-22 | 多个控制方使用同旧epoch | INT-017 |
| D13-23 | 不连通token独立取消 | INT-006 |
| D13-24 | 外部sender停止但Pause未观察到 | INT-007..012；GAP-002/003 |
| D13-25 | 缺scheduler/错误token | COMPILE-018..022 |
| D13-26 | 实际启动资源与env不符 | OUT-009 |
| D13-27 | 未知task_scheduler/未核准后端 | OUT-010/011；STATIC-012 |
| D13-28 | 业务sender可在别处工作后返回核准资源 | TASK各affinity项；INT-007..011；不扩展Pause当前后端 |
| D13-29 | detach/提前析构/并行同branch | OUT-002/003/004/012；DIAG仅明确可诊断子集 |

## §14 验收清单索引与执行注意

| 稳定索引 | §14验收项 | 对应场景/证据边界 |
| --- | --- | --- |
| D14-API | §14.1 全部8条 | API、SNAP、COMPILE-001..012、FORK、ALLOC；第4.5内部重建仅Debug，Release仍保留公开CHECK |
| D14-CB | §14.2 全部16条 | CB、CB-H、TAIL、SEND自毁、LIFE、BATCH、OUTER-005/006；GAP-007为当前拓扑不可达证明 |
| D14-SCHED | §14.3 调度与环境 | SEND、TASK、COMPILE-013..022、NOALLOC、DEATH、STATIC-009..012；额外adaptor见GAP-002/003，内部异常不可达路径见GAP-004 |
| D14-FORK | §14.4 创建/fork/退役 | ALLOC、ZERO、FORK、MOVE、OOM、TREE、PAR；record存活与责任撤销分别断言 |
| D14-HOST | §10.2/14.4 when_all/外部结果/可选宿主 | INT-005..019、LIFE、TREE、PAR、OUTER、HOST；外部error不是内部terminate |
| D14-NOTIFY | §14.4 通知与观察 | NOTIFY-01..06；observer序列/dirty、乱序/额外唤醒、关闭join逐项检查 |
| D14-TOOLS | §14.5 工具 | 每配置构建、正负编译、头文件ODR；运行期A/T分别10k；故意TSan竞态阳性对照不计产品通过 |
| D14-MODEL | §14.5 状态模型 | pause_state_model_test的定向/拒绝/有界探索/随机事件独立记录；不折算真实Pause场景，也不把1千万事件当1千万scope |
| D14-DEP | §14.5 固定依赖审查 | 锁定提交后核对task/stop_token/let/affine/unstoppable/run_loop/when_all/connect_awaitable等；依赖探针不替代真实Pause |
| D14-DEP-FAIL | 宿主深链已知失败 | stdexec_deep_chain_test fork/error-fast：既有105层复测3通过、106层复测3失败（2次0xC00000FD栈溢出、1次0xC0000409 fail-fast）；不含pause.hpp，不计通过，不给通用安全深度 |

### 本轮执行器参数应对应到具体场景

| 执行器 | 本轮普通参数 | 本轮重型参数 | 固定一次/条件性项目 |
| --- | --- | --- | --- |
| pause_sender_test | --ordinary-iterations 1000000：raw_route16、callback_stop10、reenter/connect-failure2 | --iterations 100000：TAIL2、task_route12、two_resources1 | 空task工厂/启动次数baseline每进程1次，仅校准 |
| pause_integration_test | --api-iterations 1000000：test_api_and_empty_gate里的每个API断言 | --iterations 100000：其他19项；--deep-iterations 100000：4个固定深链参数 | 新深链计数必须真包入循环，旧--iterations不会增加它们 |
| pause_resource_test | --iterations 1000000：25family，含内部逐分配位置 | — | MSVC Debug代理6项须另按1000子进程/项执行，主--iterations原本不重复它们 |
| pause_transition_extreme_test | --iterations 1000000：11固定项 | — | 每个合法锁序均定向测试，不靠随机命中充数 |
| pause_sender_hook_test / pause_task_lifetime_test | — | 各--iterations 100000：7/3项 | hook依赖副本插桩不修改生产头 |
| pause_extreme_test / pause_batch_extreme_test / pause_concurrency_stress_test | — | 各--iterations 100000：25/52/7项 | 参数矩阵如TREE/BATCH/PAR逐行，不以二进制数代替case数 |
| pause_exception_matrix_test | --ordinary-iterations 1000000：6move+4api | --iterations 100000：19并发OOM项（含两项vector reserve） | TLS故障注入与MSVC代理死亡路径分离 |
| pause_notification_matrix_test | — | --iterations 100000：6项 | notifier重叠次数按实测，barrier不保证任意一次都发生重叠 |
| pause_outer_lifetime_test | --ordinary-iterations 1000000：4项外包stopped/error | --iterations 100000：2项真实Pause host尾部销毁 | explicit affine+透明run_loop外观，不擦除未验收外观 |
| pause_host_failure_test | — | --iterations 100000：2项宿主失败 | 创建边界注入system_error；业务失败复用既有run_scope |
| pause_sender_death_test | — | — | 6种--child路径各1000；只接受指定终止码 |
| pause_contract_death_test | — | — | 5种可诊断违约各1000，目标强制NDEBUG；不能外推所有违约都可诊断 |
| 编译/静态目标 | 每配置正1负4、接口/属性/ODR各构建一次 | — | 不用1000次重复编译“满足压力” |

真实Pause outer lifetime/外包adaptor、宿主失败专项已落地；上表稳定ID按实际函数/参数对应，完整压力次数以本轮执行日志填写。STATIC-001..018、SNAP-013和GAP-004/005/007已完成源码/布局审查；它们及契约外边界不能靠扩大运行次数代替证明，也不计入独立scope总数。
