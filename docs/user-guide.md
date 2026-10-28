# Pause：介绍与使用指南

Pause 是基于 NVIDIA stdexec 的 C++23 头文件库，为协程工作流提供协作式暂停、
整体暂停屏障和单步推进。适用于批处理、数据导入、分块计算、并行请求等需要在
明确业务边界暂停的任务。头文件是 `pause.hpp`，C++ 命名空间是 `coop_pause`，
CMake 目标是 `pause::pause`。

**暂停是协作式的：正在执行的业务不会被抢占。** 每个参与分支必须到达安全暂停点
或退出，整个屏障才能进入 `Paused`。任务启动、执行器、取消和资源释放由宿主管理。
本库不提供线程强制暂停、GUI 控件或任意位置断点。

本指南随包安装为 `<prefix>/share/pause/README.md`；旁边的
[packaging.md](packaging.md) 说明安装、更新、卸载、依赖选择与回滚。
自定义 `CMAKE_INSTALL_DATADIR` 时，以自定义目录替换 `share`。阅读本地指南无需联网。

## 1. 环境与 CMake 接入

当前完整回归基线为 Windows/MSVC、C++23 和固定提交的 stdexec。默认协程宿主为
`stdexec::task<T>`，使用默认 allocator 和错误配置；已验证的 scheduler 为
`stdexec::run_loop::scheduler`。其他平台、宿主或执行器需要自行完成兼容性回归。

消费者的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES CXX)
find_package(pause 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE pause::pause)
```

在启用了编译器的终端配置，例如 VS x64 Developer PowerShell：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/sdk/pause
cmake --build build
```

业务源码使用 `#include <pause.hpp>`。C++23、依赖使用要求及 MSVC 异常展开选项由
目标传递，不需要手写 stdexec 头文件路径。

配置时优先复用已安装的 stdexec；找不到才用 FetchContent 获取固定提交。
发现不兼容依赖会警告并拒绝配置，不自动替换。上游源码获取路径要求 CMake 3.28+，
首次获取需要 Git 和网络。离线使用需预装兼容依赖并设置 `PAUSE_FETCH_STDEXEC=OFF`。
独立依赖前缀可这样指定：

```powershell
cmake -S . -B build "-DCMAKE_PREFIX_PATH=D:/sdk/pause;D:/sdk/stdexec" -DPAUSE_FETCH_STDEXEC=OFF
```

## 2. 运行随包提供的完整示例

文档目录的 `examples/` 内包含 `simple_workflow.cpp`、`example_support.hpp` 和
独立 `CMakeLists.txt`。它们通过已安装的包构建，无需 Pause 源码仓库。
请把构建输出放到安装目录之外：

```powershell
cmake -S D:/sdk/pause/share/pause/examples -B build/pause-example -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=D:/sdk/pause;D:/sdk/stdexec"
cmake --build build/pause-example
ctest --test-dir build/pause-example --output-on-failure
.\build\pause-example\pause_simple_workflow.exe
```

Linux/macOS 使用自己的绝对前缀，运行 `./build/pause-example/pause_simple_workflow`；
该命令形式不代表已完成这些平台的验证。多配置生成器构建时加 `--config Release`，
CTest 加 `-C Release`，可执行文件通常位于 `Release` 子目录。

示例约运行数秒，控制 task 发出暂停、单步和继续命令，业务使用 `then`、`let_value`
和嵌套 `when_all` 组合快慢分支。观察日志中的快任务完成、慢任务暂停和
`when_all returned`：一个子任务完成不会让仍暂停的兄弟分支自动恢复。
示例内部检查暂停期间进度不变以及退出时责任已释放。

`example_support.hpp` 中的 `run_scope`、`ChangeSignal`、`take` 是示例宿主辅助设施，
不属于公共库 API。借鉴示例时须保留错误处理和关闭顺序。

## 3. 核心对象与 API 速查

| 对象/API | 用途与约束 |
| --- | --- |
| `PauseSource(mode, notifier)` | 创建暂停门；地址稳定、不可复制或移动，必须晚于使用者销毁 |
| `PauseBranch` | 一条串行执行路径的暂停责任；可移动构造，不可复制或移动赋值 |
| `make_root_branch()` | 创建根责任，一生只能成功一次；创建不等于启动任务 |
| `make_branch()` | 创建独立顶层责任；要求 root 已创建且门为 `Open` |
| `request_pause()` | 关门并开始收集；成功不表示所有分支已经暂停 |
| `snapshot()` | 读取一致快照；通过 `phase`、`paused_epoch` 等字段观察状态 |
| `resume(epoch, mode)` | 仅在 `Paused` 且 epoch 匹配时恢复；支持连续或单步模式 |
| `branch.suspend_point({tag})` | 返回惰性 sender；实际 await/start 后才检查暂停门、履行到达责任 |
| `branch.fork_and_block_self<N>()` | 固定宽度 fork，返回 `expected<array<PauseBranch, N>, PauseError>` |
| `branch.fork_and_block_self(count)` | 动态宽度 fork，返回 `expected<vector<PauseBranch>, PauseError>` |

创建/控制接口返回 `std::expected`，必须检查失败结果。通知器必须非空、线程安全、
`noexcept`、非阻塞；不要在通知器里重入 Pause、等待完成或直接调用不支持并发的 UI。
可向应用的事件循环投递唤醒，再由观察者重新读取快照。

创建成功时 branch 的责任就已登记。持有尚未启动的 branch 或只构造暂停 sender，
都不能算作到达安全点；这些责任必须最终到达或释放，否则屏障无法收敛。

`PausePointInfo::tag` 仅是不透明编号，没有按 tag 暂停或查询位置的内置 API。

## 4. 放置安全暂停点

下面是业务协程片段，完整启动方式见随包示例：

```cpp
#include <pause.hpp>

stdexec::task<int> sum_batches(coop_pause::PauseBranch branch)
{
    int sum = 0;
    for (int batch = 1; batch <= 3; ++batch) {
        co_await branch.suspend_point();
        sum += batch; // 一个完整的业务单元。
    }
    co_return sum;
}
```

每条执行路径按值拥有自己的 branch。同一 branch 的暂停、fork、移动和析构必须
串行发生，不能将一个 branch 同时借给并行任务。根分支、子分支和独立分支都应在
第一段受保护业务之前经过入口暂停点；后续暂停点放在业务状态一致的边界。

不要暂停时持有其他分支或控制方必需的锁、事务、连接等资源。暂停延迟受最慢分支
到达下一安全点的时间限制，长计算应分段，阻塞 I/O 需要自己的超时和取消策略。

公开暂停 sender 的实际 receiver 环境必须提供 `stdexec::inplace_stop_token` 和
可查询的启动 scheduler；普通 `std::stop_token` 不满足该契约。宿主应通过
`stdexec::starts_on(loop.get_scheduler(), task)` 建立真实启动上下文，并传播宿主的
`stdexec::inplace_stop_source` token，不能仅向环境伪造 scheduler 查询值。

## 5. 暂停、继续、单步与并行

| 门状态 | 含义 | 控制操作 |
| --- | --- | --- |
| `Open` | 暂停点直接通过 | 调用 `request_pause()` 发起暂停 |
| `Collecting` | 仍有责任未到达或退出 | 等待通知并读取快照，不能强行恢复 |
| `Paused` | 当前有效责任均已到达或退出 | 使用当前 `paused_epoch` 调用 `resume` |

`resume(epoch, AdvanceMode::Continuous)` 连续运行；
`resume(epoch, AdvanceMode::SingleStep)` 放行当前 waiter 并建立下一轮屏障。
初始模式为 `SingleStep` 时，root 创建即开始收集，入口安全点前不会先跑一段业务。

单步不是全局执行一条指令：各并行分支分别运行到下一个暂停点，期间还可能产生子分支。
epoch 只能来自同一个 Source 当前的快照；遇到 `NotPaused` 或 `StalePauseEpoch` 应
重新读取状态并由应用决定下一步，不要无条件重试旧命令。

fork 后父责任被阻塞，子任务各自持有子 branch。使用 `when_all` 等宿主算法等待
子树结束并确保全部直接 child 的 branch 已释放，然后父分支重新经过暂停点。
子分支取消时撤销暂停责任，不等于释放其 record 或协程帧；父分支不能据此提前重用。
以无捕获协程 lambda 或普通协程函数按值传递 branch，避免临时闭包捕获导致悬空。

`live_branch_count` 是有效责任数，`waiting_branch_count` 是仍挂在门上的 waiter 数，
都不是协程帧数。零责任可以形成空 `Paused`，所以空屏障或计数归零不能作为任务完成信号。

## 6. 取消、异常与关闭顺序

Pause 负责暂停协议；停止请求和普通业务错误传播由 stdexec 宿主管理。取消不需要先 resume。
`when_all` 的某个分支 error/stopped 可能取消同组兄弟，这是宿主组合算法的语义。

### 6.1 将局部 stopped 转成正常结果

**不要直接组合 `when_all(stopped_as_optional(taskA(...)), taskB(...))`，然后假定 A 的责任已释放。**
若 A 在普通业务 sender 上 stopped，Pause 没有活跃 callback 来观察这次停止。
固定版本的 task 可能将 A 的 stopped frame 保留到整个 join 析构；转换成 optional
又会使 `when_all` 收到 value，不再因 A 的 stopped 取消 B。此时可能形成等待环：

```text
A 的 branch 退役等待 join 释放 frame
  → join 等 B 完成
  → B 在暂停点等待 resume
  → resume 等 A 到达或退役，屏障一直 Collecting
```

优先在 A 内部处理预期的“无结果”，正常返回明确的业务结果。需要保持 A 的
`task<int>` 接口时，可用下面的宿主辅助函数，在向外提交 value 前释放内部等待对象：

```cpp
#include <pause.hpp>
#include <optional>
#include <utility>

stdexec::task<std::optional<int>> as_optional_task(stdexec::task<int> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    // 分号结束该完整表达式：临时 awaiter/operation 已析构，stopped frame 已释放。
    co_return result;
}
```

调用方使用 `when_all(as_optional_task(taskA(...)), taskB(...))`。A 必须按值拥有自己的
branch；只借用外部 branch 时，释放 A 的 frame 不会释放外部所有者的责任。
该包装只转换 inner 的 stopped，普通业务 error 仍传播。宿主启动阶段接受的整组取消
仍可使包装 task 直接 stopped；不要再次从外层吞掉停止并保留其 frame 到 join 结束。
示例使用拥有型 `int`，不能直接推广为 `void` 或依赖外部存储的指针、视图。

### 6.2 错误与异常边界

| 失败位置 | 结果与处理 |
| --- | --- |
| 创建、控制或 fork 的可恢复失败 | 返回 `std::unexpected(PauseError)`；检查后由应用决定下一步 |
| coroutine、sender 构造或最外层公开 `connect` 尚未启动时抛异常 | 按普通 C++/stdexec 构造失败规则处理，释放已取得的资源 |
| 普通业务 sender 的 error | 由 stdexec/task 的 error 通道和宿主处理 |
| 公开暂停 operation 启动后的内部组合、恢复调度或 `.affine()` 宿主适配 error | **直接 `std::terminate()`**；外包 `let_error` 或业务 `catch` 不能恢复 |
| 系统锁失效或不可恢复的不变量破坏 | fail-fast；不能依靠析构或断言修复非法生命周期 |

内部终止不承诺析构、责任清理、剩余恢复批次交付或结构化收尾。stopped 是正常的停止通道，
不属于该终止策略。启动后 `let_value` 捕获的内部 schedule 构造/connect 异常也属于内部 error。

| `PauseError` | 常见原因与处理 |
| --- | --- |
| `RootNotCreated` | 先创建 root，再控制或创建独立分支 |
| `RootAlreadyCreated` | 一个 Source 只能成功创建一次 root；root 退出不会重置此事实 |
| `PauseAlreadyCollecting` / `AlreadyPaused` | 重复暂停请求；读取快照后处理命令状态 |
| `NotPaused` | 门不是 `Paused`，尚不能恢复 |
| `StalePauseEpoch` | 恢复凭证已过期；重新观察并由应用决策 |
| `InvalidBranchState` | 独立分支创建或 parent fork 的前置条件不满足 |
| `InvalidForkCount` | fork 数量为零 |
| `ResourceExhausted` | 创建/fork 中可捕获的资源准备失败，共享状态未部分提交 |

创建/fork 先准备资源再校验状态，资源不足时可能先返回 `ResourceExhausted`；数量为零
优先返回 `InvalidForkCount`。并非所有内存不足都能返回错误：已核对的 MSVC 14.44
默认 Debug STL 在 vector 的 `noexcept` 构造/移动中可能分配迭代器代理，失败会直接
终止，甚至发生于动态 fork 提交后的返回移动。可捕获的 record/元素存储分配失败仍须回滚。

### 6.3 通知与安全关闭

通知可以合并、乱序或由多个线程并发发出。观察者先读取信号序号，再读取 snapshot，
最后按旧序号等待；使用 dirty 标记时先清标记再读快照，避免吞掉并发通知。
退出观察循环时也要唤醒等待者。Open 暂停点同步通过，不会隐式 yield；控制 API 会取得
mutex，resume 还可能等待异线程 callback 注销，因此没有无等待或硬实时保证。

安全关闭按实际使用期安排：

1. 停止接收新的控制和任务入口，需要终止时向宿主请求 stop。
2. 保持执行器运行，等待根 operation 完成及 task frames 释放；stopped 的宿主跳转也需要队列继续工作。
3. 唤醒、停止并 join 控制/观察线程，并确保 callback、request_stop、raw start 和通知尾部的实际调用栈退出。
4. 确认没有剩余工作后关闭 run_loop 并 join worker。
5. 最后释放 branch、Source、通知对象、stop source 和执行器，parent 晚于其子分支存活期结束。

Source 析构不会请求停止、唤醒 waiter 或等待任务，不能靠析构来清理仍在运行的工作。
根 receiver 收到 completion、callback 注销或再次 request_stop 返回，都不能单独证明
所有调用栈已经退出。具体宿主可调整上述步骤顺序，但全部资源必须覆盖其实际使用期。
不要在单线程 run_loop 的 worker 上同步等待必须由同一 worker 完成的任务。

## 7. 常见问题

| 现象 | 优先检查 |
| --- | --- |
| CMake 找不到 Pause | `CMAKE_PREFIX_PATH` 是否指向安装前缀；或设置 `pause_DIR` 指向包配置目录 |
| stdexec 不兼容 | 检查构建目录的 `pause-stdexec-check.log`，修正 `stdexec_DIR`；数字版本号不足以判定兼容 |
| 一直 `Collecting` | 是否有未启动但仍持有 branch 的任务、未到达安全点的业务，或 §6.1 所述被保留的 stopped frame |
| 暂停请求成功后业务还在跑 | 请求只开始收集；实际暂停须等快照为 `Paused` |
| fork 后父任务不能继续 | 子责任是否全部退出，父分支是否重新经过暂停点 |
| 单步执行了多段业务 | 每个安全点是否对应预期粒度，以及是否存在并行分支 |
| 计数为零却仍无法退出 | 是否提前关闭执行器，是否还有调度完成、callback、协程帧或通知尾部未退出 |
| 内部调度错误没有进入业务 catch | 检查 §6.2 的启动后内部 error 终止策略 |
| 通知合并或丢失 | 通知后应重读快照；revision 跳号不等于错误，也不能用于恢复完整历史 |

完整安装与依赖配置参见本目录的 [开发包说明](packaging.md)。升级 stdexec 或更换
scheduler/宿主后，应重新验证取消、调度和生命周期契约；接口编译检查不能替代运行回归。
