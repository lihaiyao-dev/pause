# 示例与命名空间迁移的历史验证记录

以下记录从仓库 README 迁入，保留原日期、参数、结果和日志路径。所有 `build/...` 路径均相对仓库根；日志是本地生成产物，不随仓库分发。记录适用于当时输入与环境，不代表当前工作区重新执行的结果。

当前示例入口见 [README](../README.md#5-可直接运行的经典案例)，受维护的测试命令及验收索引见 [测试指南](README.md)。

2026-09-23 文档与应用示例验收：MSVC **Debug 35/35（33.02 秒）、Release 35/35（14.58 秒）**，包含 13 个示例场景和 22 个既有回归。此次验证范围是示例构建及完整回归；本地日志为 `build/usage-examples-all.log`。

2026-09-24 命名空间迁移验收：MSVC **Debug 35/35（42.46 秒）、Release 35/35（23.29 秒）**；日志为 `build/namespace-rename-validation.log`。WSL/Debian 13、GCC 14.2 使用 `-std=c++23 -O2 -DNDEBUG -pthread`，在无 shim 情况下通过含真实 `<unistd.h>` 的双翻译单元编译、链接及运行测试；TSan 脚本通过 Bash 语法检查，本次未重跑 TSan 压测。

2026-09-24 结构化并发示例验收：MSVC **Debug 43/43（37.48 秒）、Release 43/43（14.54 秒）**，包括 21 个示例场景和 22 个既有回归。新增验证覆盖 sender 数据流、bulk 空批次、跨 run_loop 屏障、when_any 取消与 join、动态宽度 2/3/8/32、整组取消及控制器异常清理。Debug 的 `parallel`、`race` 另各重复 50 次通过。日志为 `build/structured-debug-ctest.log`、`build/structured-release-ctest.log`、`build/structured-race-repeat.log`。本轮未运行 ASan/TSan，不扩大其他执行器或 stdexec 版本的兼容性承诺。

2026-09-24 控制 task + 业务 task 综合示例验收：MSVC **Debug 49/49（38.77 秒）、Release 49/49（16.97 秒）**，包括 27 个示例场景和 22 个既有回归。新示例六个模式另在 Release 各重复 30 次，共 180 次通过；验证串行 await、条件分支、递归 fork/join、13/12 轮单步、转连续运行，以及双方 error/stop 的传播和清理。日志为 `build/controlled-workflow-debug-ctest.log`、`build/controlled-workflow-release-ctest.log`、`build/controlled-workflow-repeat.log`。本轮未运行 ASan/TSan；定时器仅用于控制 task 的观察等待，业务 Pause 仍运行于原有 run_loop 环境。

2026-09-25 简单示例初版验收（已被下面的定时指令版替换）：MSVC Debug、Release 构建通过，全部示例测试分别 **28/28（7.78 秒）、28/28（6.69 秒）**。独立读取实时 stdout，核对等待数 `1 → 2 → 2 → 1`、每轮约 1 秒停顿且暂停期间无业务输出；程序同时校验暂停期间步数不变及最终 branch 释放。日志为 `build/simple-debug-ctest.log`、`build/simple-release-ctest.log`、`build/simple-timing-validation.log`。新增 LLDB 入口已检查配置、目标 exe 与 PDB；本轮未重跑 DAP 会话、组件全量测试或 ASan/TSan。

2026-09-25 简单示例串行定时指令版验收（后续加入下述业务内 `when_all`）：MSVC Debug、Release 构建通过，全部示例测试分别 **28/28（7.77 秒）、28/28（7.49 秒）**。独立记录实时 stdout，验证业务异步 sleep、`PAUSE` 后当前步骤仍能结束、两次真实暂停各保持约 1 秒、`STEP` 恰好执行一步，以及控制器发完 `CONTINUE` 后剩余业务正常完成；程序同时检查暂停期间计数不变及最终 branch 释放。日志为 `build/simple-commands-debug-ctest.log`、`build/simple-commands-release-ctest.log`、`build/simple-commands-timing-validation.log`。本轮仅修改简单示例及说明，未重跑 DAP 会话、组件全量测试或 ASan/TSan。

2026-09-25 简单示例业务内 `when_all` 验收：MSVC Debug、Release 构建通过，全部示例测试分别 **28/28（7.67 秒）、28/28（7.50 秒）**。独立记录实时 stdout，验证快任务先完成、慢任务两次暂停期间 `when_all` 一直等待、单步只完成一步，以及慢任务最终完成后父任务才越过 `when_all`。程序同时检查暂停时只有一个 waiter、慢任务计数不变和最终 branch 全部释放。日志为 `build/simple-when-all-debug-ctest.log`、`build/simple-when-all-release-ctest.log`、`build/simple-when-all-timing-validation.log`。本轮未重跑 DAP 会话、组件全量测试或 ASan/TSan。

2026-09-25 简单示例展开与组合验收：业务去掉循环，直接展开加载、转换、条件校验、并发取数、汇总、发布。MSVC Debug、Release 构建通过，全部示例测试分别 **28/28（7.59 秒）、28/28（7.39 秒）**。实时日志验证各阶段数值 `20 → 42 → (42, 3) → 45`、外层汇总结果 `55`，以及内层 `when_all` 等待两份数据、暂停期间外层 `when_all` 持续等待、单步只完成一个阶段。日志为 `build/simple-composition-debug-ctest.log`、`build/simple-composition-release-ctest.log`、`build/simple-composition-timing-validation.log`。本轮未重跑 DAP 会话、组件全量测试或 ASan/TSan。
