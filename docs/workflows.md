# 工作流案例详解

四个案例分别展示一种用法。它们只依赖 ISO C++23 和 stdexec；构建方式见
[项目首页](../README.md#快速开始)，API 与关闭契约见[使用指南](user-guide.md)。

## 工作流案例

| 文件 | 学习重点 | 执行资源 |
| --- | --- | --- |
| [simple_workflow.cpp](../examples/cli/simple_workflow.cpp) | 请求暂停、等待收敛、单步一次、连续运行 | 单 worker 的 run_loop |
| [thread_pool.cpp](../examples/cli/thread_pool.cpp) | 两条路径各持有 branch，fork、when_all、父分支接回责任 | 原生 static_thread_pool，默认 4 workers |
| [cancellation.cpp](../examples/cli/cancellation.cpp) | 暂停时由宿主请求 stop，无需 resume，沿 stopped 退出 | 2 workers 的 static_thread_pool |
| [sender_workflow.cpp](../examples/cli/sender_workflow.cpp) | 普通 sender pipeline 接入，branch 覆盖整个 operation 的借用期 | 2 workers 的 static_thread_pool |

从仓库根目录构建并运行：

```powershell
scripts\msvc-cmake.cmd examples release
.\build\msvc-release\pause_simple_workflow.exe
.\build\msvc-release\pause_thread_pool.exe 4
.\build\msvc-release\pause_thread_pool.exe 1
.\build\msvc-release\pause_cancellation.exe
.\build\msvc-release\pause_sender_workflow.exe
```

启用实现测试后，CTest 的 `pause_example_` 分组运行这些案例；每个程序也会检查自己的
关键结果。案例用于展示接入，完整的并发交错、资源失败和异常验证位于 tests。

## 暂停、单步与连续运行

`simple_workflow` 的业务只有三批计数，每批之前等待一次 `suspend_point()`。
main 创建 Continuous Source 和 root，调用 `request_pause()` 后启动工作；控制方等待
真正进入 Paused，此时业务计数为 0。SingleStep 放行一批，下一屏障计数为 1，随后
Continuous 放行剩余两批，最终为 3。

例子在任务启动前请求暂停，避免用定时器或 sleep 猜测业务执行位置。运行中的
`request_pause()` 使用相同协议：已经开始的业务继续到下一个安全点，成功返回只表示
关门请求已提交。长操作的取消、超时和安全边界由业务自己安排。

## 线程池中的分支与汇合

`thread_pool` 先经过 root 入口点，再 `fork_and_block_self<2>()`。每个 child 按值拥有
自己的 branch，分别执行两段业务。`starts_on(pool.get_scheduler(), child_task(...))`
显式把两条任务提交到池；`when_all` 负责组合和等待结果，本身不保证另一个 worker。

四轮单步分别停在 root 入口、两个 child 的第一段之前、第二段之前、parent 汇合之后。
waiter 数依次为 1、2、2、1，完成的 child 业务单元数依次为 0、0、2、4。
最后一个 child record 退役时自动归还 parent 责任，parent 在汇总结果前再次等待暂停点。
最终结果为 1 + 2 = 3。

线程池恢复可能换 worker，不依赖线程局部状态，也不假定回到同一 OS 线程。
共享进度使用 atomic；业务结果在根等待结束后读取。一个 worker 也能运行，因为控制等待
位于池外，业务暂停通过 sender 挂起，未阻塞 worker。

## 取消和普通 sender

`cancellation` 在确认入口 Paused 后调用宿主 stop source 的 `request_stop()`。
它不调用 resume，也不运行暂停点后的业务；根结果为 stopped。Pause 只响应该 operation
环境提供的 token，不拥有停止源，不替宿主决定取消范围。

`sender_workflow` 将 `branch.suspend_point() | then(...)` 交给宿主，正常恢复后产生 42。
它不使用业务协程；外层 branch 直到根 operation 和控制调用全部退出后才退役。构造
sender 不代表已到达安全点，提前销毁 branch 也不能用于取消。

## 复用示例宿主

[example_support.hpp](../examples/common/example_support.hpp) 提供少量宿主辅助函数：

| 函数/对象 | 职责 |
| --- | --- |
| `take` / `accept` | 检查 expected，将失败交给示例的异常清理 |
| `ChangeSignal` / `phase_notifications` | 将三个阶段通知汇入一个线程安全唤醒信号 |
| `wait_for_barrier` | 在控制线程等待非空 Paused，重新读取快照 |
| `run_scope` / `run_scope_on` | 启动执行上下文、传播 stop token、等待根 operation，并退出控制线程 |
| `require_drained` | 根等待和控制调用退出后的责任计数检查 |

这些是示例设施，不是公共 Pause API。`run_scope_on` 借用调用方的 scheduler，调用方保持
执行资源、Source 和通知对象存活。异常时请求业务停止，完成后唤醒并 join 控制线程；
运行在独立 run_loop 时，最后才 finish 队列并 join worker。

阶段通知只报告完整事务的最终阶段变化，不能用它观察每次计数或 epoch 更新。
工作流结束时由宿主单独唤醒控制等待；空 Paused 或零责任不能当作整个工作流完成。
