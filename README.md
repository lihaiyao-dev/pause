# Pause：协作式暂停、单步与并行分支

`pause.hpp` 为基于 stdexec 的 C++ 协程工作流提供安全暂停点、整体暂停屏障和单步推进。适用于批处理、导入流水线、分块计算、并行请求等需要在明确业务边界暂停的任务。

**暂停是协作式的：正在执行的业务不会被抢占。** 每个参与分支必须到达暂停点或退出，整个屏障才能变成 `Paused`。任务启动、执行器、取消传播、任务组合和资源释放由宿主管理。

| 阅读目的 | 入口 |
| --- | --- |
| 安装后接入库、处理错误与关闭 | [使用指南](docs/user-guide.md)；随包安装为 `share/pause/README.md` |
| 从源码构建并运行案例 | 本文第 1、5 节 |
| 理解状态机、责任转移与并发协议 | [设计契约](pause-branch-design.md)，含状态图、责任转移图和恢复/取消时序图 |
| 打包、安装、更新或卸载 | [开发包说明](docs/packaging.md) |
| 配置调试器、编辑器与本机环境 | [本地开发说明](docs/development.md) |
| 查看测试入口和已执行的验证范围 | [测试指南](tests/README.md) |

建议从 [简单示例](examples/simple_workflow.cpp) 开始：业务直接展开六个阶段，用 `then`、`let_value` 和嵌套 `when_all` 组合数据与异步操作；控制 task 按时间间隔发送暂停、单步、继续指令。日志会展示快任务已经完成、慢任务暂停时，外层 `when_all` 仍在等待；两个流程各自在一个完整函数中。进阶可看 [控制 task + 业务 task 综合示例](examples/controlled_workflow.cpp)，业务主体见 [controlled_business.hpp](examples/controlled_business.hpp)，包含串行调用、条件分支和递归并发组合。另有 [算法细分案例](examples/pause_workflows.cpp) 和 [独立任务与动态分支案例](examples/independent_jobs.cpp)。任务按值拥有 branch，stdexec 的算法负责启动、组合、取消和等待。`run_scope`、`ChangeSignal`、`take` 等执行器与控制辅助设施集中在 [example_support.hpp](examples/example_support.hpp)，不属于 Pause API。

## 1. 支持环境与构建

当前完整回归基线是 **C++23、Windows/MSVC、默认固定提交的 NVIDIA stdexec**：

- stdexec 默认提交：`f0e8ae6fdc6c188389b146bb854b80d399724b04`，由 CMake 获取；可用 `PAUSE_STDEXEC_GIT_TAG` 选择其他兼容提交或 tag。
- 协程宿主：默认 `stdexec::task<T>`，使用默认 allocator 和错误配置。
- 已接入执行器：该版本的 `stdexec::run_loop::scheduler`，由独立线程驱动；不同分支可以使用不同 run_loop。
- 旧 `exec::task`、任意线程池、自定义 allocator 或来源不明的擦除 scheduler，不能仅凭编译成功视为兼容；接入条件见[设计契约 §3.4](pause-branch-design.md#34-环境与-completion-契约)。

公开 C++ 命名空间为 `coop_pause`，可与 POSIX `::pause()` 共存。2026-09-24 从旧命名空间迁移：调用方须将 `pause::PauseSource` 等 C++ 引用改为 `coop_pause::PauseSource`，并重新编译所有使用者；头文件名和 CMake 目标 `pause::pause` 保持不变。不提供旧命名空间别名，以免重新引入冲突。回退时需将库与调用方一起还原。

此次改名消除了已知的 POSIX 名称冲突，并不单独构成完整 Linux 支持认证。此前的 Linux/WSL TSan 报告使用旧名称和测试专用声明 shim，属于历史验证记录。

在 **VS x64 Developer PowerShell** 中运行：

```powershell
cmake -S . -B build/examples-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPAUSE_BUILD_EXAMPLES=ON -DPAUSE_BUILD_DESIGN_TESTS=OFF
cmake --build build/examples-release
ctest --test-dir build/examples-release --output-on-failure -R '^pause_example_'
```

首次配置需要获取依赖。已有本地源码时，可在配置命令追加 `-DFETCHCONTENT_SOURCE_DIR_STDEXEC=<本地源码目录>`；例如本仓库此前构建留下的 `build/msvc-debug/_deps/stdexec-src`。该源码覆盖优先于 `PAUSE_STDEXEC_GIT_TAG`。

CMake 会在每次配置时编译检查实际选用的源码：要求支持 sender 的 `.affine()` 成员定制、当前 `completion_behavior` 及 Pause/default task 接口。已查明的上游 main 最低候选是 **2026-07-05 的 `5e22875aad04fd933a9f0405e09fe85c77bf07db`**（[stdexec #2103](https://github.com/NVIDIA/stdexec/pull/2103)）；`nvhpc-26.05` 缺少必要定制，会被拒绝。检查不依赖 Git 历史，支持源码压缩包、浅克隆和接口齐备的回移版本；不能用项目版本号 `0.11.0` 判断是否满足条件。失败的编译详情保存在构建目录的 `pause-stdexec-check.log`。

选用其他版本时，现有内部插桩测试仍绑定默认提交的头文件哈希，部分依赖契约断言也记录了该提交的行为。请关闭这两组测试，再构建工具链/头文件检查目标或自己的应用：

```powershell
cmake -S . -B build/compatible-stdexec -G Ninja -DCMAKE_BUILD_TYPE=Release -DPAUSE_STDEXEC_GIT_TAG=<兼容提交或tag> -DPAUSE_BUILD_TESTS=OFF -DPAUSE_BUILD_DESIGN_TESTS=OFF
cmake --build build/compatible-stdexec
```

通过配置检查表示必要接口齐备，不代表该版本已经通过全部并发、取消和生命周期回归。恢复默认版本时可新建构建目录，并省略上述覆盖参数。

`PAUSE_BUILD_EXAMPLES` 默认关闭。启用后生成 `pause_simple_workflow`、`pause_controlled_workflow`、`pause_workflows`、`pause_independent_jobs`；同时启用默认的 `PAUSE_BUILD_TESTS` 时，注册 28 个 `pause_example_` 测试。示例复用已有依赖，不改变公共 API。

交互式图形案例使用独立开关 `PAUSE_BUILD_QT_EXAMPLE=ON`，默认关闭；开启时查找 **Qt 6.5+ Widgets**，生成 `pause_qt_workflow`。UI 集成暂停、继续、单步、取消、重启、耗时显示、概览和缩放。[接入入口](examples/qt_workflow.hpp) 提供最小长短路径预览，你可替换图布局与业务函数，用原生 `stdexec::when_all(...) | stdexec::let_value(...)` 自行组合；`RunContext::step/group` 封装安全点和状态显示，Session 控制同一个 PauseSource。真实业务显示实际耗时，可指定预计时长；随机延时仅用于预览。接入与构建说明见 [Qt 分支图示例](examples/qt_workflow.md)。

项目内接入时链接头文件接口目标：

```cmake
target_link_libraries(your_app PRIVATE pause::pause)
```

发布包支持 ZIP/TGZ、指定目录安装/更新/卸载，以及
`find_package(pause 0.1 CONFIG REQUIRED)`。stdexec 优先使用已安装的 CMake 包，
未找到才自动获取固定提交；已有依赖不兼容时停止配置，不自动替换。
命令、离线选项、版本兼容规则和回滚方法见[开发包说明](docs/packaging.md)。
安装后可在 `<prefix>/share/pause/README.md` 离线查阅库介绍、API 速查和使用指南，
同目录 `examples/` 附带可独立构建的简单工作流示例。

开发调试与编辑器设置见 [本地开发说明](docs/development.md)，包含 LLDB DAP、clice 和本机 Qt 编译数据库的配置。

## 2. 先理解三个对象

| 对象 | 职责 | 所有权要求 |
| --- | --- | --- |
| `PauseSource` | 共享暂停门、控制命令与快照 | 地址稳定，不可复制或移动；晚于全部使用者释放 |
| `PauseBranch` | 一条串行执行路径的暂停责任 | 不可复制，可移动构造，不可移动赋值；推荐按值移入任务 |
| stdexec 宿主 | scheduler、stop source、任务组合与 operation/frame 生命周期 | 必须持续驱动执行器，并完成取消、等待和 join |

每个并发路径独立拥有 branch。同一 branch 的暂停点、fork、移动和析构由其持有者串行使用；不能把一个 branch 同时借给多个并行任务。

| API | 用途与限制 |
| --- | --- |
| `PauseSource(mode, notifier)` | 指定初始推进模式；notifier 必须非空、线程安全、`noexcept`、非阻塞 |
| `make_root_branch()` | 一生只能成功一次；只创建责任，不启动任务 |
| `make_branch()` | 创建独立顶层责任；要求 root 已创建且 Gate 为 `Open` |
| `request_pause()` | 请求关闭门并收集责任；成功不等于已经暂停 |
| `snapshot()` | 获取一致快照；可通过 `const PauseSource&` 查询 |
| `resume(epoch, mode)` | 仅在 `Paused` 且 epoch 匹配时放行；模式为 `Continuous` 或 `SingleStep` |
| `branch.suspend_point({tag})` | 构造惰性 sender，必须真正 await/start 才产生暂停行为 |
| `branch.fork_and_block_self<N>()` | 固定宽度 fork，返回 `expected<array<PauseBranch, N>, PauseError>` |
| `branch.fork_and_block_self(count)` | 运行期宽度 fork，返回 `expected<vector<PauseBranch>, PauseError>` |

控制和创建 API 返回 `std::expected`，必须检查结果。`Source` 控制 API 和 branch API 要求左值对象，避免对临时对象建立悬空借用。

`PausePointInfo::tag` 是不透明编号，目前不参与调度决策，也没有按 tag 暂停、查询位置或自动输出日志的 API。需要业务进度时由宿主另外记录。

## 3. 最小工作流与安全暂停点

下面的任务按值拥有 branch，并在每个完整业务单元之前经过安全点：

```cpp
#include "pause.hpp"

stdexec::task<int> sum_batches(coop_pause::PauseBranch branch)
{
    int sum = 0;
    for (int batch = 1; batch <= 3; ++batch) {
        co_await branch.suspend_point();
        sum += batch; // 替换为一个保持业务不变量的完整工作单元。
    }
    co_return sum;
}
```

宿主应通过 `stdexec::starts_on(loop.get_scheduler(), task)` 建立实际启动上下文，并向任务传播 `stdexec::inplace_stop_source` 的 token。完整启动、控制及异常清理方式见 [example_support.hpp](examples/example_support.hpp) 的 `run_scope`，不要省略其 worker 和 controller 收尾逻辑。

公开暂停 sender 的实际 receiver 环境必须提供 `stdexec::inplace_stop_token` 和可查询的 `stdexec::get_start_scheduler`；普通 `std::stop_token` 或丢失查询的环境不满足此契约。宿主还须保证实际启动上下文与声明的 scheduler 一致。

选暂停点时遵守以下约束：

1. 根、子分支和独立任务的第一段受保护业务前，都先经过入口暂停点。
2. 将暂停点放在业务状态一致的位置，例如一个批次提交之后、下个批次开始之前。
3. 不要在暂停期间持有其他分支收敛或控制操作所需的锁、事务或有限资源，否则可能形成等待环。
4. 暂停延迟取决于最慢分支何时到达下一安全点；长计算应自行划分工作单元，阻塞 I/O 也需要自己的超时/取消策略。

创建成功时责任就已登记。创建了 branch，却既不启动持有它的任务，也不释放它，会阻止暂停收敛；仅创建一个 `suspend_point()` sender 同样不能算“已到达”。

## 4. 暂停、恢复与单步

暂停门有三个状态：

| 状态 | 含义 | 控制方应该做什么 |
| --- | --- | --- |
| `Open` | 暂停点可直接通过 | 需要暂停时调用 `request_pause()` |
| `Collecting` | 已关门，仍有责任尚未到达或退出 | 等待通知并重新读快照；不能强行 resume |
| `Paused` | 当前有效责任全部到达或退出 | 用当前 `paused_epoch` 恢复 |

`request_pause()` 成功只表示开始收集。初始 `SingleStep` 在 root 创建时就开始收集，轮次为 0，因此入口暂停点之前不会先执行一段业务。

`resume(epoch, Continuous)` 打开门；`resume(epoch, SingleStep)` 放行当前全部 waiter，并在它们继续执行之前建立下一轮屏障。

**SingleStep 不是全局执行一条指令。** 多个分支可以分别推进到自己的下一暂停点，期间还可能 fork 出新的分支；屏障在这一轮所有有效责任到达或退出后才再次收敛。只有一个线性分支且每个安全点对应一个业务单元时，才能把一次放行解释为一个业务单元。

epoch 是本轮恢复凭证，必须来自同一个 Source 当前的 `paused_epoch`。多控制者竞争时，旧命令可能得到 `NotPaused` 或 `StalePauseEpoch`；重新读取快照后由应用决定是否提交新的命令，不要无条件重试旧 epoch。

快照字段的含义：

- `revision`：状态提交序号；通知合并时观察到跳号很正常，不能据此恢复完整历史。
- `paused_epoch`：仅 `Paused` 时存在；同轮分支加入或退出可改变 revision，但不改 epoch。
- `origin`、`step_round`：当前暂停来源与单步轮次；连续恢复会清除单步信息。
- `live_branch_count`：有效暂停责任数，不是任务数、record 数或协程帧数。
- `waiting_branch_count`：仍挂在暂停门上的 waiter 数；已经放行但还在调度队列中的 operation 不算在内。

零责任可以形成空 `Paused`，空批次单步也可以产生新 epoch。**空 Paused 和计数归零都不是工作流完成信号。** 示例控制器只自动放行有 waiter 的屏障，避免对空屏障无限单步；任务完成由宿主单独观测。计数回绕不在当前保证范围内。

## 5. 可直接运行的经典案例

先运行简单示例（约 5 秒）：

```powershell
.\build\examples-release\pause_simple_workflow.exe
```

只有 `work_task` 和 `control_task` 两个顶层协程函数。`work_task` 内用 `fork_and_block_self<2>() + when_all` 启动快慢两条子流程；子流程直接写成无捕获的内联协程 lambda，通过参数按值接收各自的 branch，没有额外拆出辅助函数。快任务经过入口暂停点，异步 sleep 300ms 后返回 `10`。慢任务没有业务循环，六个阶段直接展开，每段都有明确暂停点：

| 阶段 | 组合方式与结果 |
| --- | --- |
| 1. 加载 | `schedule_after(500ms) \| then`，产生 `20` |
| 2. 转换 | `just \| then \| let_value`，先乘 2，再异步等待 500ms 后加 2，得到 `42` |
| 3. 条件校验 | `if` 内经过暂停点，异步等待 500ms 后校验值为偶数 |
| 4. 并发取数 | 内层 `when_all` 等待 300ms、500ms 两份数据，解构出 `42` 和 `3` |
| 5. 汇总 | 等待 500ms，`just(amount, bonus) \| then` 将两个值合成 `45` |
| 6. 发布 | `just \| let_value` 传入异步发布，500ms 后返回 `45` |

外层 `when_all` 收齐快慢任务的 `10`、`45`，父 branch 重入暂停点后通过 `just \| then` 得到最终结果 `55`。内层并发取数是一个完整业务阶段，两个纯 sender 共同完成后才到下一个暂停点；它们不各自持有 branch。控制指令顺序如下：

| 控制流程 | 业务侧看到的影响 |
| --- | --- |
| 开始连续运行，750ms 后发送 `PAUSE` | 快任务已经完成；慢任务的当前步骤继续完成，在下一个暂停点停下 |
| 确认 `PAUSED`，等待 1 秒后发送 `STEP` | 慢任务只执行一步，再次暂停；`when_all` 仍在等待 |
| 确认再次 `PAUSED`，等待 1 秒后发送 `CONTINUE` | 慢任务完成剩余步骤，`when_all` 返回，父任务重入暂停点后结束 |

注意日志中 **`command: PAUSE` 和 `PAUSED` 是两个时刻**：请求暂停不会打断当前的 sleep；只有到达下一处 `suspend_point()` 才会暂停。每次确认 `PAUSED` 后，1 秒内没有业务输出；单步只多完成一步。具体暂停在第几步取决于调度，通常第一次在第 2 步结束后。

重点观察 `[fast] done` → `[control] PAUSED: fast done, slow paused ...; when_all still waiting` → `[slow] done` → `[work] when_all returned`。**一个子任务正常完成只满足 `when_all` 的一部分条件；另一个仍暂停时，父任务不会继续，`when_all` 也不会自动放行暂停点。** 示例在两次暂停期间检查父任务尚未越过 `when_all`、慢任务计数不变；快任务正常完成后已释放自己的 branch，因此只剩慢任务的一个 waiter。

sleep 使用 stdexec 的 `schedule_after`，不阻塞共享的 run_loop；控制器在等待实际暂停时每 10ms 观察一次快照，确认存在 waiter 后才允许恢复。`main` 中只有一次 `sync_wait(starts_on(..., when_all(control_task(...), work_task(...))))`。控制 task 发完 `CONTINUE` 即可结束，根 `when_all` 继续等待业务完成，无需额外的完成观察函数。task 的 `co_await` 完成后回到同一个 run_loop，再更新进度和日志；定时 sender 链中的 `then`、`let_value` 回调只操作按值传递的数据，不访问共享进度或 branch。异常通过 `when_all` 取消另一方并等待收尾。

需要串行调用、条件分支和递归子任务时，再运行综合示例：

```powershell
.\build\examples-release\pause_controlled_workflow.exe
.\build\examples-release\pause_controlled_workflow.exe continue
.\build\examples-release\pause_controlled_workflow.exe all
```

### 5.1 一个控制 task 与一个业务工作流

[work_task](examples/controlled_business.hpp) 按自然业务顺序编排：入口暂停点 → `fork<1>` 后 `co_await load_source` → 父分支重入 → `if` 内安全点 → `fork<1>` 后 `co_await prepare_data` → 父分支重入 → `fork<3>` 后 `when_all` → 父分支重入并汇总。三个分片各自继续递归 `fork<2> + when_all`，最终形成 12 个叶子。每个子 task 按值拥有 branch；局部 structured binding 取出的 branch 移入子 task 后就不再持有责任。

`fork_and_block_self<N>()` 返回 `expected<array<PauseBranch, N>, PauseError>`，不能直接对 expected 下标或按 N 个 branch 解构。示例先用 `take` 检查错误，再解构：

```cpp
auto [a, b, c] = take(branch.fork_and_block_self<3>());
auto [first, second, third] = co_await stdexec::when_all(
    compute_shard(std::move(a), 1, 2, progress),
    compute_shard(std::move(b), 2, 2, progress),
    compute_shard(std::move(c), 3, 2, progress));
co_await branch.suspend_point(); // 子树已结束，父责任重新加入屏障。
```

[control_task](examples/controlled_workflow.cpp) 自身不持有 branch，也不参与业务屏障。应用入口只有一次 `sync_wait`，等待 `starts_on(executor, when_all(control_task(...), observe_work(work_task(...), session)))`：控制与业务是根作用域中的两个并发分支。`observe_work` 在业务 task 的 await 完整结束后标记完成，控制器据此退出；它不会将暂时没有责任的空 Paused 当作完成。

| 参数 | 控制行为 | 验证重点 |
| --- | --- | --- |
| 无参数或 `step` | 全程单步，打印每轮 waiter 数 | 13 轮单步；递归部分 waiter 数为 3→6→12→6→3→1；结果 59，12 次 fork 与 12 次 join |
| `continue` | 观察三个屏障，随后连续恢复 | 后续完整任务树自动执行并返回 59 |
| `skip-optional` | 跳过 `if` 中的可选业务，全程单步 | 12 轮单步，少执行一个业务单元，结果 54 |
| `cancel` | 在六个嵌套分支等待时请求根 stop | 不 resume，整棵业务树取消，根 stopped |
| `work-error` | 嵌套叶子任务抛出业务错误 | `when_all` 取消仍在等待定时器的控制 task，并传播业务错误 |
| `control-error` | 控制 task 在嵌套屏障抛出错误 | `when_all` 取消业务树，等待清理后传播控制错误 |

这里两个 task 与业务树共享一个 `run_loop`，以协作并发展示结构化控制流。控制器使用 stdexec 自带的 `exec::schedule_after(timer, 1ms)` 异步观察快照，等待期间业务执行器可以继续工作；默认 task 的 affinity 将定时器完成后的控制代码调回该 run_loop。定时器有自己的执行资源，应用无需单独创建控制线程。当前 Pause 没有异步等待快照的 sender，因此本例采用定时观察、通知器为空操作；1ms 是采样间隔，不是实时响应保证。生产应用可对接已有事件循环的异步通知。

业务成功时，控制器观察完成信号退出；任一分支 error/stopped 时，根 `when_all` 请求另一分支停止并等待它结束。根等待返回后才关闭 run_loop 和定时器，最后释放 Source。不同完成通道都验证责任排空，不能提前关闭执行器来强制结束任务。

下面是已有的算法细分示例。`pause_workflows` 不带参数或传入 `all` 时运行全部 14 个模式；每个成功案例输出 `PASS <模式>`，内部同时核对其预期结果。

```powershell
.\build\examples-release\pause_workflows.exe all
.\build\examples-release\pause_workflows.exe sender-pipeline
.\build\examples-release\pause_workflows.exe parallel
.\build\examples-release\pause_workflows.exe race
.\build\examples-release\pause_independent_jobs.exe
.\build\examples-release\pause_independent_jobs.exe --dynamic 3
.\build\examples-release\pause_independent_jobs.exe --dynamic 8
```

### 5.2 sender 数据流、并发组合与批量变换

| 参数 | stdexec 组合 | 暂停行为与结果 |
| --- | --- | --- |
| `sender-pipeline` | `just → let_value → then` 串联三个异步阶段 | 每阶段先经过安全点；单步三次，把输入 6 变成结果 13 |
| `bulk` | `just(array) → bulk(seq) → then` 变换批次数据并求和 | 每批次单步一次；验证 shape=0 保留输入，shape=3 全部翻倍，两批合计 18 |
| `parallel` | `when_all` 启动两个分片，`starts_on` 将一个分片放到另一 run_loop | 两条固定 worker 线程、每分片两个阶段；waiter 数依次为 1→2→2→1，最后汇总为 62 |
| `race` | `exec::when_any` 竞速两个镜像请求 | 首个完成得到 42，自动取消并等待另一请求；父分支重入后才使用汇总阶段 |

`then` 用于同步值变换；回调需要返回下一个 sender 时用 `let_value`，例如：

```cpp
stdexec::task<int> parse_records(coop_pause::PauseBranch branch)
{
    co_return co_await (stdexec::just(6)
        | stdexec::let_value([&](int& records) {
              return branch.suspend_point()
                   | stdexec::then([&] { return records * 2; });
          }));
}
```

这里 `records` 的存储由 `let_value` operation 保持，branch 由 task frame 拥有；两者都覆盖异步暂停的完整使用期。不要让协程依赖临时 lambda 闭包中捕获的 branch，也不要仅将 branch 存在外层组合器长期保留的 operation 中。使用普通协程函数或无捕获协程 lambda，将 branch 按值传入协程帧；算法在这些所有权边界内组合 sender。

`parallel` 先 `co_await when_all(...)` 取得两份结果，再经过 parent 的暂停点，用 `then` 执行汇总。因此无需保存任务对象、逐个 join 或手工清空生命周期容器。`when_all` 表达并发关系，实际执行资源由 scheduler 决定；单个 run_loop 只能交错执行，本例核对两个分片运行于不同线程且暂停后回到各自执行器。

`when_any` 选择**首个完成**，value、error、stopped 都可能胜出；它会请求其余任务停止，并等待它们完成。它不提供“忽略失败直到首个成功”的语义，也不能抢占不响应取消的业务。本例用另一个暂停点模拟慢请求的下一阶段，控制器只在确认竞速已经 join 后放行父分支，避免误放行取消过程中的短暂屏障。

`bulk` 展示一个安全点后的完整批次处理；这里显式使用 `stdexec::seq`，不宣称元素之间并行执行。Pause 的单步粒度是批次，不能在多个同时执行的 bulk 回调里共用一个 branch。需要各自能暂停的并发路径时，采用 `parallel` 的 fork/when_all 结构。`exec::when_any` 属于 stdexec 随库提供的扩展算法。

### 5.3 导入流水线与递归分块计算

| `pause_workflows` 参数 | 应用含义 | 预期结果 |
| --- | --- | --- |
| `pipeline` | 导入、转换、发布；入口暂停，单步一次，再连续运行 | 入口业务计数为 0，单步后为 1，旧 epoch 被拒绝，最终完成 3 个单元 |
| `single-step` | 全程逐业务单元推进 | 放行 3 次，完成 3 个单元；最终可留在零责任的 Paused |
| `nested` | 二叉递归 map/reduce | 7 个节点、3 次父分支重入汇总，最终得到 4 |

fork 是**责任转交**，不启动线程，也不替 parent 等待任务。经典结构是“入口安全点 → fork → when_all → parent 再次安全点 → 汇总”：

```cpp
stdexec::task<int> sum_tree(coop_pause::PauseBranch parent, int depth)
{
    co_await parent.suspend_point();
    if (depth == 0) co_return 1;

    auto children = parent.fork_and_block_self<2>();
    if (!children) throw std::runtime_error("fork failed");

    auto [left, right] = co_await stdexec::when_all(
        sum_tree(std::move((*children)[0]), depth - 1),
        sum_tree(std::move((*children)[1]), depth - 1));

    co_await parent.suspend_point(); // children 已释放后，重新参与当前屏障。
    co_return left + right;
}
```

片段另需 `<stdexcept>`；应用可像完整示例的 `PauseFailure` 一样保留具体 `PauseError`。递归深度来自外部输入时，也应由业务限制资源用量。

fork 后，parent 不能继续受保护业务，也不能立即再次 fork。必须等直接 children 的 **branch records 实际退役**，再经过 parent 的暂停点。最后一个 child 释放不会自动让 parent 重新参与。若 parent 没有后续业务，可以直接退出，无需为析构再经过暂停点。

### 5.4 取消、预取消与失败传播

| 参数 | 应用含义 | 预期结果 |
| --- | --- | --- |
| `cancel` | 用户取消已经暂停的导入 | 宿主请求 stop，无需 resume；业务执行 0 个单元，根任务 stopped |
| `prestop` | 启动前已取消的任务 | 不进入受保护业务，正常完成资源清理 |
| `error` | 一个并行请求发生业务错误 | `when_all` 取消兄弟，错误到达宿主；兄弟不继续业务 |
| `stopped` | 一个并行请求原样报告停止 | `when_all` 取消兄弟，整组 stopped |

Pause 不拥有停止源。发起停止的是宿主的 `workflow_stop.request_stop()`；token 只是观察取消的凭证。默认 `when_all` 为 children 传播组内取消，嵌套组合可以建立自己的取消范围。

停止是 stdexec 的独立完成通道，不等于从暂停点正常返回。不要以 `resume()` 代替取消，也不要在停止后靠销毁仍活跃的 operation 强制结束任务。

### 5.5 可选请求：一个没有结果，另一个继续

| 参数 | 应用含义 | 预期结果 |
| --- | --- | --- |
| `optional` | 可选请求局部 stopped，必要请求继续 | 可选结果为空；其 branch 先退役，必要请求可收敛并完成 |
| `optional-value` | 可选请求正常返回 | 得到 `optional{42}`，必要请求返回 7，parent 正常汇总 |
| `optional-error` | 可选请求发生业务错误 | 错误不被 optional 包装吞掉，仍取消兄弟并传播 |

**不要直接照搬 `when_all(stopped_as_optional(taskA(...)), taskB(...))`。** 若 A 在普通业务 sender 上 stopped，其 frame 可能被外层组合保留到 join 结束。A 的 branch 仍有效，B 又在等整体暂停收敛，就可能永久停在 Collecting。

能修改业务接口时，优先让 A 在内部处理预期的“无结果”，正常返回明确的业务结果。需保留 A 的 `task<int>` 接口时，使用示例中的宿主包装：

```cpp
stdexec::task<std::optional<int>> as_optional_task(stdexec::task<int> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    // 上一句的临时 awaiter/operation 已析构；inner 的 stopped frame 也已释放。
    co_return result;
}
```

A 必须按值拥有 branch；只借用外部 branch 时，释放 A 不会释放外部所有者的责任。包装只转换 inner 的 stopped，不吞业务 error，也不保证宿主在包装启动前接受的整组取消会变成 value。示例使用拥有型 `int`；不要直接推广为悬空指针、视图或 `void` 的通用封装。

### 5.6 独立索引任务与运行期宽度

[独立任务案例](examples/independent_jobs.cpp) 默认用 `make_root_branch()` 和 `make_branch()` 创建两个顶层任务，通过 `when_all` 组合，只有一个根 `sync_wait`。每个索引协程按值拥有自己的 branch，结果沿任务树向上归约；执行器数量固定为两个，不随任务数增长。

所有任务到达入口屏障后，只取消第一个任务；其余任务保持 Paused，epoch 不变。连续恢复后，每个存活分区在自己的 run_loop 上处理 3 个文档，校验和为 22。默认输出包含 `jobs=2, stopped=1, value=1, indexed=3`。

`--dynamic N` 把运行期分块数递归二分，每个内部节点用 `fork_and_block_self<2>() + when_all` 建立结构化子树，每个叶子处理一个分区。fork 只拓展任务树，叶子在第一段业务前经过入口暂停点；内部节点 join 后重新经过 parent 的暂停点才归约结果。N 为 3 时完成 2 个分区、6 个文档；N 为 8 时完成 7 个分区、21 个文档。CLI 的 `2..32` 是示例的输入范围，不是 Pause 的分支上限。

这条任务树不需要保存 branch 或宿主对象的 vector，也无需为每个任务创建 runner 线程。动态 fork 的容器 API 仍可用于应用自身需要的布局，本例的递归结构则直接让协程作用域表达父子生命周期。

首个分区通过 `write_env` 使用局部停止源，并在协程包装内用 `stopped_as_optional` 将局部取消变为空结果。包装同时把上层 token 的停止请求转发到局部停止源，保证根取消或兄弟失败仍能退出整棵树。普通业务 error 继续交给 `when_all` 传播。这里的局部取消隔离来自 stdexec 的组合与环境，共享 PauseSource 本身不定义取消范围。

另可运行 `pause_independent_jobs --cancel-all` 和 `--controller-error`，观察整组取消和控制器失败的关闭路径：两者都在入口屏障处触发，不调用 resume，仍须停止并等待整个任务树、释放全部 branch；前者根任务 stopped，后者保留并报告预期控制异常。

## 6. 错误处理与竞争边界

| 错误 | 常见原因与处理 |
| --- | --- |
| `RootNotCreated` | 尚未创建 root 就控制或创建独立分支；先完成初始化 |
| `RootAlreadyCreated` | 同一 Source 第二次创建 root；root 退出也不会重置该事实 |
| `PauseAlreadyCollecting` / `AlreadyPaused` | 重复暂停请求；结合当前快照处理命令状态 |
| `NotPaused` | 在 Open/Collecting 恢复；应等待 Paused |
| `StalePauseEpoch` | 当前 Paused 已不是命令针对的那一轮；重新观察并由业务决策 |
| `InvalidBranchState` | 非 Open 时 make_branch，或 parent 不满足 fork 条件 |
| `InvalidForkCount` | fork 数量为 0；在调用方校验数量 |
| `ResourceExhausted` | 创建/fork 中可捕获的资源准备失败；共享责任尚未部分提交 |

创建/fork **先准备资源，后校验状态**，资源不足时可能先得到 `ResourceExhausted`，不能承诺总是先报告 root/branch 状态错误；`count == 0` 优先返回 `InvalidForkCount`。

并非所有内存不足都能返回 `ResourceExhausted`：已验证的 MSVC 14.44 默认 Debug STL 会在 vector 的 `noexcept` 构造/移动中分配迭代器调试代理，失败会直接 terminate，甚至可能发生于动态 fork 提交后的返回移动。项目保留默认迭代器调试配置；该边界见[设计契约 §11.3](pause-branch-design.md#113-错误与异常安全)。

`resume()` 与 stop 竞争时，内核只选择一次完成；即使 resume 获胜，后续恢复调度仍可能接受取消。反过来，Open 直接通过已经获胜后到来的暂停或停止，不会回溯撤销刚刚放行的业务。应用必须按协作式边界设计。

普通业务 error 由 stdexec/宿主处理；**公开暂停 operation 启动后的内部调度等基础设施 error 直接终止进程**，外包 `let_error` 或协程 `catch` 不能将其恢复为业务继续。启动前 sender 构造/公开 connect 异常按普通构造失败处理。

固定依赖的 `inplace_stop_source::request_stop()` 返回值不要照搬 `std::stop_source` 的解释：此提交首次请求最终返回 false，已有停止请求时返回 true。示例忽略这个 bool，用任务完成及 `stop_requested()` 观察状态；重复调用也不是等待其他请求线程退出的 join。

## 7. 通知、执行器与安全关闭

通知器只表示“快照可能变化”，可能由多个线程并发调用，也可能合并或乱序。推荐像 `ChangeSignal` 一样只递增原子序号并唤醒观察者；不要在通知器内执行控制命令、同步重入 Pause、阻塞等待或直接调用不支持并发的 UI。

观察循环应先读信号序号，再读 snapshot，再按旧序号等待；使用 dirty 标记时先清标记再读快照。不要读完快照后无条件清除通知，否则会吞掉刚到达的新状态。控制线程退出时，也必须发信号唤醒其等待。

Open 暂停点同步通过，不会隐式 yield。大量 Open 暂停点不保证同一执行器上的其他任务获得执行机会。不要在单线程 run_loop 的 worker 中 `sync_wait` 等待必须由同一 worker 执行的工作。

等待后的正常恢复回到启动环境声明的 scheduler，表示执行资源，不是任意后端都保证相同 OS 线程。当前每个 run_loop 由一个固定线程驱动，因此示例可核对线程 ID；仅手动往 env 填 scheduler 不能建立正确上下文。

安全关闭必须覆盖以下实际使用期，不能用计数归零代替：

1. 不再接受新的外部控制/任务入口；需要终止时通过宿主停止源请求取消。
2. 保持执行器运行，等待根 operation 完成及所有 task frames 释放；stopped 的宿主恢复跳转也需要队列继续工作。
3. 请求控制/观察线程退出，唤醒并 join，确保控制调用、request_stop 和通知尾部已退出。
4. 确认没有剩余工作需要队列后，`finish()` run_loop 并 join worker。
5. 最后释放仍保留的 branch、Source、通知对象、stop source 和执行器资源，满足 parent 晚于 children、Source 晚于全部借用者的约束。

以上不是所有宿主都必须采用的唯一顺序：案例在完整作用域内协调清理；关键是所有实际使用结束前资源始终存活。`Source` 析构不请求 stop、不完成 waiter、不等待任务。branch 的取消撤责、raw waiter 摘链、根 receiver 收到完成，都不能单独证明完整调用栈已退出。

控制 API 会取得 mutex，resume 还可能等待异线程 callback 注销完成；它不是实时或保证无等待的控制接口。Release 中一些不变量错误会 terminate，另一些非法生命周期会导致悬空访问，不应依赖断言作为资源管理机制。

## 8. 常见问题定位

| 现象 | 优先检查 |
| --- | --- |
| 一直 Collecting | 分支是否到达安全点；有无未启动/未释放的 branch、长业务、被保留的 stopped frame |
| fork 后 parent 不能继续 | children 是否真正退役；join 后是否先 await parent 的暂停点 |
| 取消一个任务却影响兄弟 | 是否处于同一个 when_all 取消范围；这通常是组合器的正常传播 |
| 取消一个任务后其他任务仍暂停 | 是否使用独立 token；Pause 不自动广播取消 |
| 计数已归零但 shutdown 卡住 | 是否过早 finish 队列；stopped 宿主跳转或 callback/request_stop 尾部是否仍未退出 |
| 单步推进了多个业务动作 | 是否有多个分支，或一个安全点间包含多个动作；重新明确业务单元 |
| 恢复后仍 stopped | 恢复调度阶段可能接受取消；resume 成功不是业务完成保证 |
| 通知丢失或观察线程不能退出 | 是否错误清 dirty；退出请求后是否唤醒 atomic::wait |
| sender 无法连接/等待 | 实际 receiver 是否有兼容 scheduler 和 inplace_stop_token；是否用了旧 exec::task |

## 9. 实现原理与进一步阅读

内部用同一状态锁原子维护暂停门、有效责任和等待者。fork 将一份父责任转交给子责任；子 record 通过 RAII 退役，parent 在后续暂停点重新参与。收集完成才发布 Paused 和对应 epoch。

每个暂停 operation 从 receiver environment 获取停止 token，并注册 callback。恢复和取消在锁内争夺唯一完成权，通知与实际 completion 在锁外执行。Open 路径直接完成；已等待的正常恢复通过原 scheduler 调度；取消撤销当前分支责任并沿 stopped 通道退出。

完整约束、异常边界和依赖接入要求见 [设计契约](pause-branch-design.md)。当前测试入口见 [tests/README.md](tests/README.md)，设计条款与具体场景的对应关系见 [逐场景矩阵](tests/pause-contract-scenario-matrix.md)，已执行范围见 [验收报告](tests/pause-contract-validation-2026-09-25.md)及 [目录整理验证](tests/test-layout-validation-2026-09-25.md)。

历次示例、命名空间迁移的结果和本地日志路径集中在 [历史验证记录](tests/example-validation-history.md)。这些结果绑定各自日期与输入，不代表当前版本重新执行。升级 stdexec 或更换 scheduler/宿主时，需要重新验证取消传播、调度与 frame 释放契约。
